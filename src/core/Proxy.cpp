#include "core/Brand.h"
#include "core/Proxy.h"

#include "core/FFmpeg.h"
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>

namespace proxy {
namespace {

std::atomic<bool> g_enabled{true};

// What playbackPath found, re-checked every few seconds (a proxy made by
// another window, or the file replaced).
struct Known {
    QString proxy; // empty: none
    qint64 checkedMs = 0;
};
std::mutex g_mutex;
std::map<QString, Known> g_known; // by path#stream
QElapsedTimer g_clock;
std::set<QString> g_doomed; // deleted while still open (the preview was playing them): not used, deleted when let go

// Tries again to delete the doomed ones. With g_mutex held.
void purgeLocked()
{
    for (auto it = g_doomed.begin(); it != g_doomed.end();)
        it = QFile::remove(*it) || !QFileInfo::exists(*it) ? g_doomed.erase(it) : std::next(it);
}

bool doomed(const QString& f)
{
    std::lock_guard lock(g_mutex);
    purgeLocked();
    return g_doomed.count(f) > 0;
}

QString key(const QString& path, int stream)
{
    return path + QLatin1Char('#') + QString::number(stream);
}

void forget(const QString& path, int stream)
{
    std::lock_guard lock(g_mutex);
    g_known.erase(key(path, stream));
}

QString directory()
{
    const QByteArray env = qgetenv("MVP_PROXY_DIR");
    const QString dir = !env.isEmpty() ? QString::fromLocal8Bit(env)
                                       : QDir(brand::storageLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("proxies"));
    QDir().mkpath(dir);
    return dir;
}

} // namespace

QString file(const QString& path, int stream)
{
    const QFileInfo fi(path);
    const QByteArray id = (fi.absoluteFilePath() + QString::number(fi.size()) + QString::number(fi.lastModified().toMSecsSinceEpoch())
                           + QLatin1Char('#') + QString::number(stream))
                              .toUtf8();
    return QDir(directory()).filePath(QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Md5).toHex())
                                      + QStringLiteral(".mp4"));
}

bool exists(const QString& path, int stream)
{
    const QString f = file(path, stream);
    return QFileInfo(f).size() > 0 && !doomed(f);
}

bool recommended(const MediaInfo& info)
{
    if (!info.ok() || info.isStill || info.video.empty())
        return false;
    const VideoStreamInfo& v = info.video.front();
    if (v.hasAlpha)
        return false;
    const QString codec = v.codecName.toLower();
    const QString pix = v.pixelFormat.toLower();
    const bool big = qint64(v.width) * v.height > qint64(1920) * 1088;
    const bool heavyCodec = codec == QLatin1String("hevc") || codec == QLatin1String("av1") || codec == QLatin1String("prores")
                            || codec == QLatin1String("dnxhd") || codec == QLatin1String("vp9") || codec == QLatin1String("cfhd");
    const bool deep = pix.contains(QLatin1String("10")) || pix.contains(QLatin1String("12")) || pix.contains(QLatin1String("16"));
    return big || heavyCodec || deep;
}

bool make(const QString& path, int stream, int height, const std::function<bool(double)>& progress, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    ff::initialize();
    // The stream's own time base: the proxy keeps it, so its timestamps read
    // back exactly as the source's.
    AVRational tb{1, 90000};
    AVRational rate{0, 1};
    TimeUs duration = 0;
    {
        ff::InputFormatPtr fc;
        if (ff::openInput(path, fc) < 0)
            return fail(QStringLiteral("can't open the file"));
        if (stream < 0 || stream >= int(fc->nb_streams) || fc->streams[stream]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
            return fail(QStringLiteral("no such video stream"));
        const AVStream* st = fc->streams[stream];
        if (st->time_base.num > 0 && st->time_base.den > 0)
            tb = st->time_base;
        rate = st->avg_frame_rate.num > 0 ? st->avg_frame_rate : st->r_frame_rate;
        if (fc->duration > 0)
            duration = fc->duration;
    }
    if (tb.den / std::max(1, tb.num) < 1000) // too coarse to hold the timestamps well (rare)
        tb = {1, 90000};

    VideoDecoder dec;
    QString err;
    if (!dec.open(path, stream, &err))
        return fail(err.isEmpty() ? QStringLiteral("can't decode the video") : err);
    const AVFrame* f = dec.frameAt(0);
    if (!f)
        return fail(QStringLiteral("no pictures"));
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(AVPixelFormat(f->format));
    if (desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA))
        return fail(QStringLiteral("it has transparency, which a proxy can't keep"));

    const int outH = std::max(2, std::min(height, f->height) & ~1);
    const int outW = std::max(2, int(std::lround(double(f->width) * outH / std::max(1, f->height))) & ~1);

    const QString final = file(path, stream);
    const QString tmp = final + QStringLiteral(".part");
    QFile::remove(tmp);
    AVFormatContext* oc = nullptr;
    if (avformat_alloc_output_context2(&oc, nullptr, "mp4", tmp.toUtf8().constData()) < 0 || !oc)
        return fail(QStringLiteral("can't create the proxy file"));
    struct Out {
        AVFormatContext* oc;
        AVCodecContext* enc = nullptr;
        SwsContext* sws = nullptr;
        bool opened = false;
        ~Out()
        {
            if (opened)
                avio_closep(&oc->pb);
            avformat_free_context(oc);
            avcodec_free_context(&enc);
            sws_freeContext(sws);
        }
    } out{oc};

    const bool x264 = avcodec_find_encoder_by_name("libx264") != nullptr;
    const AVCodec* codec = x264 ? avcodec_find_encoder_by_name("libx264") : avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    if (!codec)
        return fail(QStringLiteral("no H.264 or MPEG-4 encoder"));
    AVCodecContext* c = out.enc = avcodec_alloc_context3(codec);
    c->width = outW;
    c->height = outH;
    c->time_base = tb;
    if (rate.num > 0 && rate.den > 0)
        c->framerate = rate;
    c->pix_fmt = AV_PIX_FMT_YUV420P;
    c->gop_size = 12; // quick seeking: scrubbing and jumping around
    c->max_b_frames = 0; // no reordering: timestamps go straight through, cheap decoding
    c->sample_aspect_ratio = f->sample_aspect_ratio;
    const bool fullRange = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P || f->format == AV_PIX_FMT_YUVJ422P
                           || f->format == AV_PIX_FMT_YUVJ444P;
    c->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    // The colour matrix the original is shown with, written down: untagged
    // files are guessed from their height, and the proxy is smaller.
    int cs = f->colorspace;
    if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RGB || cs == AVCOL_SPC_RESERVED)
        cs = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
    c->colorspace = AVColorSpace(cs);
    c->color_primaries = f->color_primaries;
    c->color_trc = f->color_trc;
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    AVDictionary* opts = nullptr;
    if (x264) {
        av_dict_set(&opts, "preset", "superfast", 0);
        av_dict_set(&opts, "tune", "fastdecode", 0);
        av_dict_set(&opts, "crf", "22", 0);
    } else {
        c->flags |= AV_CODEC_FLAG_QSCALE;
        c->global_quality = FF_QP2LAMBDA * 4;
    }
    int r = avcodec_open2(c, codec, &opts);
    av_dict_free(&opts);
    if (r < 0)
        return fail(QStringLiteral("encoder: %1").arg(ff::errorString(r)));
    AVStream* ost = avformat_new_stream(oc, nullptr);
    avcodec_parameters_from_context(ost->codecpar, c);
    ost->time_base = tb;
    if (avio_open(&oc->pb, tmp.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
        return fail(QStringLiteral("can't write %1").arg(tmp));
    out.opened = true;
    if ((r = avformat_write_header(oc, nullptr)) < 0)
        return fail(QStringLiteral("proxy file: %1").arg(ff::errorString(r)));

    ff::FramePtr yuv = ff::makeFrame();
    yuv->format = AV_PIX_FMT_YUV420P;
    yuv->width = outW;
    yuv->height = outH;
    yuv->color_range = c->color_range;
    yuv->colorspace = c->colorspace;
    yuv->color_primaries = c->color_primaries;
    yuv->color_trc = c->color_trc;
    if (av_frame_get_buffer(yuv.get(), 0) < 0)
        return fail(QStringLiteral("out of memory"));
    const int64_t frameTicks = rate.num > 0 && rate.den > 0 ? std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), tb)) : 0;
    ff::PacketPtr pkt = ff::makePacket();
    auto drain = [&]() {
        while (avcodec_receive_packet(c, pkt.get()) == 0) {
            if (pkt->duration <= 0) // the file must not end before the last picture
                pkt->duration = frameTicks;
            av_packet_rescale_ts(pkt.get(), c->time_base, ost->time_base);
            pkt->stream_index = ost->index;
            if (av_interleaved_write_frame(oc, pkt.get()) < 0)
                return false;
        }
        return true;
    };

    int64_t lastPts = -1;
    bool cancelled = false, ok = true;
    while (f) {
        const TimeUs us = dec.currentPts();
        // The first picture from 0 (the source shows it from there too); then the source's own times.
        int64_t pts = lastPts < 0 ? 0 : av_rescale_q(us, AV_TIME_BASE_Q, tb);
        if (pts > lastPts) {
            out.sws = sws_getCachedContext(out.sws, f->width, f->height, AVPixelFormat(f->format), outW, outH, AV_PIX_FMT_YUV420P,
                                           SWS_AREA, nullptr, nullptr, nullptr);
            if (!out.sws) {
                ok = false;
                break;
            }
            // Keep the colours as they are: same matrix and range in and out.
            const int* coeffs = sws_getCoefficients(cs);
            sws_setColorspaceDetails(out.sws, coeffs, fullRange, coeffs, fullRange, 0, 1 << 16, 1 << 16);
            if (av_frame_make_writable(yuv.get()) < 0) {
                ok = false;
                break;
            }
            sws_scale(out.sws, f->data, f->linesize, 0, f->height, yuv->data, yuv->linesize);
            yuv->pts = pts;
            yuv->duration = frameTicks; // the last picture keeps its length
            yuv->pict_type = AV_PICTURE_TYPE_NONE;
            if (avcodec_send_frame(c, yuv.get()) < 0 || !drain()) {
                ok = false;
                break;
            }
            lastPts = pts;
        }
        if (progress && duration > 0 && !progress(std::clamp(double(us) / double(duration), 0.0, 1.0))) {
            cancelled = true;
            break;
        }
        if (!dec.nextFrame())
            break;
        const TimeUs next = dec.nextPts();
        f = dec.frameAt(next);
        if (dec.currentPts() <= us)
            break; // no further
    }
    if (ok && !cancelled) {
        avcodec_send_frame(c, nullptr);
        ok = drain() && av_write_trailer(oc) >= 0;
    }
    avio_closep(&oc->pb);
    out.opened = false;
    if (cancelled || !ok || lastPts < 0) {
        QFile::remove(tmp);
        return cancelled ? fail(QStringLiteral("cancelled")) : fail(QStringLiteral("encoding failed"));
    }
    QFile::remove(final);
    if (!QFile::rename(tmp, final)) {
        QFile::remove(tmp);
        return fail(QFileInfo::exists(final) ? QStringLiteral("the old proxy is still being played: try again in a moment")
                                             : QStringLiteral("can't move the proxy into place"));
    }
    {
        std::lock_guard lock(g_mutex);
        g_doomed.erase(final);
    }
    forget(path, stream);
    if (progress)
        progress(1.0);
    return true;
}

bool remove(const QString& path, int stream)
{
    const QString f = file(path, stream);
    const bool had = QFileInfo::exists(f) && !doomed(f);
    if (QFileInfo::exists(f) && !QFile::remove(f)) {
        std::lock_guard lock(g_mutex);
        g_doomed.insert(f);
    }
    forget(path, stream);
    return had;
}

void purge()
{
    std::lock_guard lock(g_mutex);
    purgeLocked();
}

void setEnabled(bool on)
{
    g_enabled = on;
}

bool enabled()
{
    return g_enabled;
}

QString playbackPath(const QString& path, int stream)
{
    if (!g_enabled)
        return path;
    std::lock_guard lock(g_mutex);
    if (!g_clock.isValid())
        g_clock.start();
    const qint64 now = g_clock.elapsed();
    Known& k = g_known[key(path, stream)];
    if (k.checkedMs == 0 || now - k.checkedMs > 3000) {
        purgeLocked();
        const QString p = file(path, stream);
        k.proxy = QFileInfo(p).size() > 0 && !g_doomed.count(p) ? p : QString();
        k.checkedMs = std::max<qint64>(1, now);
    }
    return k.proxy.isEmpty() ? path : k.proxy;
}

qint64 totalBytes()
{
    qint64 total = 0;
    for (const QFileInfo& fi : QDir(directory()).entryInfoList({QStringLiteral("*.mp4")}, QDir::Files))
        if (!doomed(fi.absoluteFilePath()))
            total += fi.size();
    return total;
}

int removeAll()
{
    int n = 0;
    std::lock_guard lock(g_mutex);
    for (const QFileInfo& fi : QDir(directory()).entryInfoList({QStringLiteral("*.mp4"), QStringLiteral("*.part")}, QDir::Files)) {
        const QString f = fi.absoluteFilePath();
        if (g_doomed.count(f))
            continue;
        ++n;
        if (!QFile::remove(f))
            g_doomed.insert(f); // being played: goes when let go
    }
    g_known.clear();
    return n;
}

} // namespace proxy
