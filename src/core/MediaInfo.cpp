#include "core/MediaInfo.h"

#include "core/FFmpeg.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>

QSize VideoStreamInfo::displaySize() const
{
    int w = width;
    if (sarNum > 0 && sarDen > 0 && sarNum != sarDen)
        w = int(std::lround(double(width) * sarNum / sarDen));
    if (rotation == 90 || rotation == 270)
        return QSize(height, w);
    return QSize(w, height);
}

const VideoStreamInfo* MediaInfo::videoStream(int index) const
{
    for (const auto& v : video)
        if (v.index == index)
            return &v;
    return nullptr;
}

const AudioStreamInfo* MediaInfo::audioStream(int index) const
{
    for (const auto& a : audio)
        if (a.index == index)
            return &a;
    return nullptr;
}

QString MediaInfo::summary() const
{
    QStringList lines;
    lines << QFileInfo(path).fileName();
    lines << QStringLiteral("Format: %1 (%2)").arg(formatLongName, formatName);
    if (isStill)
        lines << QStringLiteral("Still image");
    else
        lines << QStringLiteral("Duration: %1").arg(formatDuration(duration));
    if (bitRate > 0)
        lines << QStringLiteral("Bitrate: %1 kb/s").arg(bitRate / 1000);
    for (const auto& v : video) {
        QString s = QStringLiteral("Video #%1: %2 %3x%4 %5, %6 fps")
                        .arg(v.index)
                        .arg(v.codecName)
                        .arg(v.width)
                        .arg(v.height)
                        .arg(v.pixelFormat)
                        .arg(v.frameRate.toString());
        if (v.sarNum != v.sarDen)
            s += QStringLiteral(", SAR %1:%2").arg(v.sarNum).arg(v.sarDen);
        if (v.rotation)
            s += QStringLiteral(", rotated %1°").arg(v.rotation);
        lines << s;
    }
    for (const auto& a : audio)
        lines << QStringLiteral("Audio #%1: %2 %3 Hz, %4 ch").arg(a.index).arg(a.codecName).arg(a.sampleRate).arg(a.channels);
    for (const auto& w : warnings)
        lines << QStringLiteral("Note: %1").arg(w);
    if (!error.isEmpty())
        lines << QStringLiteral("Error: %1").arg(error);
    return lines.join(QLatin1Char('\n'));
}

namespace {

AVRational pickFrameRate(const AVStream* st)
{
    auto sane = [](AVRational r) { return r.num > 0 && r.den > 0 && av_q2d(r) >= 0.5 && av_q2d(r) <= 1000.0; };
    if (sane(st->avg_frame_rate))
        return st->avg_frame_rate;
    if (sane(st->r_frame_rate))
        return st->r_frame_rate;
    return AVRational{25, 1};
}

// Some streams (raw elementary streams, broken headers) report no duration.
// Walk the packets to find the last timestamp.
TimeUs scanDuration(AVFormatContext* fc)
{
    const int64_t start = ff::formatStartTime(fc);
    TimeUs end = 0;
    ff::PacketPtr pkt = ff::makePacket();
    while (av_read_frame(fc, pkt.get()) >= 0) {
        const AVStream* st = fc->streams[pkt->stream_index];
        int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
        if (ts != AV_NOPTS_VALUE) {
            ts += std::max<int64_t>(pkt->duration, 0);
            end = std::max(end, av_rescale_q(ts, st->time_base, AV_TIME_BASE_Q) - start);
        }
        av_packet_unref(pkt.get());
    }
    return end;
}

} // namespace

MediaInfo probeMedia(const QString& path)
{
    MediaInfo info;
    info.path = path;

    ff::InputFormatPtr fmt;
    const int ret = ff::openInput(path, fmt);
    if (ret < 0) {
        info.error = QStringLiteral("Could not open file: %1").arg(ff::errorString(ret));
        return info;
    }
    AVFormatContext* fc = fmt.get();
    info.formatName = QString::fromUtf8(fc->iformat->name);
    info.formatLongName = QString::fromUtf8(fc->iformat->long_name ? fc->iformat->long_name : fc->iformat->name);
    info.bitRate = fc->bit_rate;

    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        AVStream* st = fc->streams[i];
        const AVCodecParameters* par = st->codecpar;
        const AVCodec* dec = avcodec_find_decoder(par->codec_id);
        const QString codecName = QString::fromUtf8(avcodec_get_name(par->codec_id));

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (st->disposition & AV_DISPOSITION_ATTACHED_PIC)
                continue; // cover art embedded in audio files
            if (!dec) {
                info.warnings << QStringLiteral("no decoder for video stream #%1 (%2)").arg(i).arg(codecName);
                continue;
            }
            if (par->width <= 0 || par->height <= 0) {
                info.warnings << QStringLiteral("video stream #%1 has no dimensions").arg(i);
                continue;
            }
            VideoStreamInfo v;
            v.index = int(i);
            v.codecName = codecName;
            v.width = par->width;
            v.height = par->height;
            AVRational sar = par->sample_aspect_ratio.num > 0 ? par->sample_aspect_ratio : st->sample_aspect_ratio;
            if (sar.num > 0 && sar.den > 0) {
                v.sarNum = sar.num;
                v.sarDen = sar.den;
            }
            const AVRational fr = pickFrameRate(st);
            v.frameRate = FrameRate{fr.num, fr.den};
            v.rotation = ff::streamRotation(st);
            const char* pf = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format));
            v.pixelFormat = pf ? QString::fromUtf8(pf) : QStringLiteral("unknown");
            const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
            v.hasAlpha = desc && (desc->flags & AV_PIX_FMT_FLAG_ALPHA);
            info.video.push_back(v);
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (!dec) {
                info.warnings << QStringLiteral("no decoder for audio stream #%1 (%2)").arg(i).arg(codecName);
                continue;
            }
            if (par->sample_rate <= 0) {
                info.warnings << QStringLiteral("audio stream #%1 has no sample rate").arg(i);
                continue;
            }
            AudioStreamInfo a;
            a.index = int(i);
            a.codecName = codecName;
            a.sampleRate = par->sample_rate;
            a.channels = std::max(1, par->ch_layout.nb_channels);
            info.audio.push_back(a);
        }
    }

    info.isStill = ff::isImageDemuxer(fc->iformat) && info.video.size() == 1 && info.audio.empty();

    if (info.isStill) {
        info.duration = kDefaultStillDuration;
    } else {
        if (fc->duration != AV_NOPTS_VALUE && fc->duration > 0)
            info.duration = fc->duration; // AV_TIME_BASE == microseconds
        for (unsigned i = 0; i < fc->nb_streams && info.duration <= 0; ++i) {
            const AVStream* st = fc->streams[i];
            if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
                info.duration = std::max(info.duration, av_rescale_q(st->duration, st->time_base, AV_TIME_BASE_Q));
        }
        if (info.duration <= 0 && (!info.video.empty() || !info.audio.empty()))
            info.duration = scanDuration(fc);
        if (info.duration <= 0 && !info.video.empty()) {
            // Last resort: a stream we can decode but can't time. Treat as one frame.
            info.duration = info.video.front().frameRate.frameDuration();
        }
    }

    if (info.video.empty() && info.audio.empty()) {
        info.error = QStringLiteral("No decodable audio or video streams");
        if (!info.warnings.isEmpty())
            info.error += QStringLiteral(" (%1)").arg(info.warnings.join(QStringLiteral("; ")));
    }
    return info;
}
