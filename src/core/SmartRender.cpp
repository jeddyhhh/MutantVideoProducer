#include "core/SmartRender.h"

#include "core/FFmpeg.h"
#include "core/PanCrop.h"

#include <QFileInfo>
#include <algorithm>
#include <cmath>
#include <map>

namespace smartrender {
namespace {

bool active(const EffectChain& chain)
{
    return std::any_of(chain.begin(), chain.end(), [](const EffectInstance& e) { return e.enabled; });
}

QString at(TimeUs t)
{
    return QStringLiteral("%1 s").arg(double(t) / kUsPerSecond, 0, 'f', 2);
}

struct Source {
    ff::InputFormatPtr fmt;
    AVStream* st = nullptr;
};

bool open(const QString& path, int stream, Source& src)
{
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, path.toUtf8().constData(), nullptr, nullptr) < 0)
        return false;
    src.fmt.reset(raw);
    if (avformat_find_stream_info(raw, nullptr) < 0 || stream < 0 || stream >= int(raw->nb_streams))
        return false;
    src.st = raw->streams[stream];
    return src.st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
}

// The packets a piece is copied from, in decode order: from the keyframe at (or, for the first piece, before) its start
// up to its end; `keep` gets each packet shown in it, `clean` whether the cut ends and starts cleanly.
struct Scan {
    bool found = false;       // a keyframe to start from
    bool startsOnKey = false; // a keyframe exactly at the start
    bool cleanEnd = true;     // nothing shown before the end needs a packet after it
    bool cleanStart = true;   // nothing after the start keyframe is shown before it (open GOP)
    int held = 0;             // pictures before the end that need one past it: not copied (the one before them holds)
};

Scan scan(Source& src, const Piece& p, bool first, const std::function<bool(AVPacket*)>& keep)
{
    Scan out;
    AVStream* st = src.st;
    const AVRational us{1, int(kUsPerSecond)};
    const int64_t in = av_rescale_q(p.sourceIn, us, st->time_base);
    const int64_t end = av_rescale_q(p.sourceIn + p.length, us, st->time_base);
    const AVRational rate = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    const int64_t half = rate.num ? av_rescale_q(1, av_inv_q(rate), st->time_base) / 2 : 1;
    const int64_t slack = av_rescale_q(2 * kUsPerSecond, us, st->time_base);
    av_seek_frame(src.fmt.get(), st->index, in, AVSEEK_FLAG_BACKWARD);
    ff::PacketPtr pkt = ff::makePacket();
    bool started = false, pastEnd = false;
    int64_t keyPts = 0;
    while (av_read_frame(src.fmt.get(), pkt.get()) >= 0) {
        if (pkt->stream_index != st->index || pkt->pts == AV_NOPTS_VALUE) {
            av_packet_unref(pkt.get());
            continue;
        }
        const int64_t pts = pkt->pts;
        if (!started) {
            // The keyframe to start from: the one at the start, or (the first piece) the last one before it.
            if (!(pkt->flags & AV_PKT_FLAG_KEY) || pts > in + half) {
                if (pts > in + half)
                    break; // no keyframe before the start
                av_packet_unref(pkt.get());
                continue;
            }
            started = true;
            out.found = true;
            keyPts = pts;
            out.startsOnKey = std::llabs(pts - in) <= half;
            if (!first && !out.startsOnKey)
                break;
        } else if (pts < keyPts && pts >= in - half) {
            out.cleanStart = false; // a leading picture of an open GOP, shown after the cut
        }
        if (pts >= end - half) {
            pastEnd = true;
        } else if (pastEnd) {
            out.cleanEnd = false; // shown before the end, but decoded after a picture past it: left out
            ++out.held;
        } else if (keep && !keep(pkt.get())) {
            break;
        }
        if (pkt->dts != AV_NOPTS_VALUE && pkt->dts > end + slack)
            break;
        av_packet_unref(pkt.get());
    }
    if (!started)
        out.startsOnKey = false;
    return out;
}

} // namespace

Plan plan(const ProjectSnapshot& snap, const ExportSettings& s)
{
    Plan p;
    auto no = [&](const QString& why) {
        p.ok = false;
        p.why = why;
        p.pieces.clear();
        return p;
    };
    if (s.videoEncoder.isEmpty())
        return no(QStringLiteral("there is no video to copy"));
    const AVOutputFormat* muxer = av_guess_format(s.muxer.isEmpty() ? nullptr : s.muxer.toUtf8().constData(), s.path.toUtf8().constData(), nullptr);
    const QString muxerName = muxer ? QString::fromLatin1(muxer->name) : QString();
    if (!muxerName.contains(QLatin1String("mp4")) && !muxerName.contains(QLatin1String("mov")))
        return no(QStringLiteral("only MP4 and MOV files are smart rendered"));
    const AVCodec* encoder = avcodec_find_encoder_by_name(s.videoEncoder.toUtf8().constData());
    if (!encoder)
        return no(QStringLiteral("the encoder isn't available"));
    const Timeline& tl = snap.timeline;
    if (active(tl.outputEffects))
        return no(QStringLiteral("the Video Output FX change the picture"));
    const bool anySolo = std::any_of(tl.tracks.begin(), tl.tracks.end(), [](const Track& t) { return isVisualTrack(t.type) && t.solo; });
    const Track* shown = nullptr;
    for (const Track& t : tl.tracks) {
        if (!isVisualTrack(t.type) || t.muted || (anySolo && !t.solo))
            continue;
        const bool inRange = std::any_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.start < s.rangeEnd && c.end() > s.rangeStart; });
        if (!inRange)
            continue;
        if (shown)
            return no(QStringLiteral("more than one video track shows something"));
        shown = &t;
    }
    if (!shown)
        return no(QStringLiteral("there is no video in the range"));
    if (active(shown->effects) || shown->level < 0.999f || shown->composite.active || shown->blend || shown->matte)
        return no(QStringLiteral("track %1 has effects, a level or a blend mode").arg(shown->name));
    const TimeUs halfFrame = s.fps.frameDuration() / 2;
    TimeUs covered = s.rangeStart;
    for (const Clip& c : shown->clips) {
        if (c.end() <= s.rangeStart || c.start >= s.rangeEnd)
            continue;
        if (c.start > covered + halfFrame)
            return no(QStringLiteral("there is a gap at %1").arg(at(covered)));
        if (c.start < covered - halfFrame)
            return no(QStringLiteral("events overlap at %1").arg(at(c.start)));
        const auto mit = snap.media.find(c.mediaId);
        const MediaItem* m = mit != snap.media.end() ? mit->second.get() : nullptr;
        if (c.isGenerated() || c.isSequence() || !m || m->info.isStill || !m->info.videoStream(c.streamIndex))
            return no(QStringLiteral("the event at %1 isn't a video file").arg(at(c.start)));
        if (c.speed != 1.0 || c.reverse || c.hasRamp() || c.loops())
            return no(QStringLiteral("the event at %1 plays at another speed").arg(at(c.start)));
        if (active(c.effects) || !isDefaultPanCrop(c.panCrop) || c.gain < 0.999f || c.stabilize > 0 || c.denoise > 0)
            return no(QStringLiteral("the event at %1 has effects, Pan/Crop, an opacity or a fix").arg(at(c.start)));
        const TimeUs a = std::max(c.start, s.rangeStart), b = std::min(c.end(), s.rangeEnd);
        if ((c.fadeIn > 0 && c.start >= s.rangeStart) || (c.fadeOut > 0 && c.end() <= s.rangeEnd) || !c.transitionIn.type.isEmpty()
            || !c.transitionOut.type.isEmpty())
            return no(QStringLiteral("the event at %1 fades or has a transition").arg(at(c.start)));
        const VideoStreamInfo* vs = m->info.videoStream(c.streamIndex);
        p.pieces.push_back({m->path, vs->index, c.sourceIn + (a - c.start), b - a, a - s.rangeStart});
        covered = b;
    }
    if (covered < s.rangeEnd - halfFrame)
        return no(QStringLiteral("there is a gap at %1").arg(at(covered)));

    // The files: the codec, size, frame rate and encoding the render asks for, the same in each; cuts on keyframes.
    QByteArray extradata;
    int format = -1;
    for (size_t i = 0; i < p.pieces.size(); ++i) {
        const Piece& piece = p.pieces[i];
        Source src;
        if (!open(piece.path, piece.stream, src))
            return no(QStringLiteral("%1 can't be read").arg(QFileInfo(piece.path).fileName()));
        const AVCodecParameters* par = src.st->codecpar;
        const QString name = QFileInfo(piece.path).fileName();
        if (par->codec_id != encoder->id)
            return no(QStringLiteral("%1 is %2, not %3").arg(name, QString::fromLatin1(avcodec_get_name(par->codec_id)),
                                                             QString::fromLatin1(avcodec_get_name(encoder->id))));
        if (par->width != s.width || par->height != s.height)
            return no(QStringLiteral("%1 is %2x%3, not %4x%5").arg(name).arg(par->width).arg(par->height).arg(s.width).arg(s.height));
        const AVRational rate = src.st->avg_frame_rate.num ? src.st->avg_frame_rate : src.st->r_frame_rate;
        if (!rate.num || std::abs(av_q2d(rate) / s.fps.toDouble() - 1.0) > 0.001)
            return no(QStringLiteral("%1 runs at %2 fps, not %3").arg(name).arg(av_q2d(rate), 0, 'f', 3).arg(s.fps.toDouble(), 0, 'f', 3));
        const QByteArray extra(reinterpret_cast<const char*>(par->extradata), par->extradata_size);
        if (i == 0) {
            extradata = extra;
            format = par->format;
        } else if (extra != extradata || par->format != format) {
            return no(QStringLiteral("%1 was encoded differently from %2").arg(name, QFileInfo(p.pieces.front().path).fileName()));
        }
        const Scan sc = scan(src, piece, i == 0, {});
        const TimeUs cut = s.rangeStart + piece.start;
        if (!sc.found || (i > 0 && !sc.startsOnKey))
            return no(QStringLiteral("the cut at %1 isn't on a keyframe of %2").arg(at(cut), name));
        if (!sc.cleanStart && i > 0)
            return no(QStringLiteral("the cut at %1 starts inside a group of pictures of %2").arg(at(cut), name));
        if (!sc.cleanEnd && i + 1 < p.pieces.size())
            return no(QStringLiteral("the cut at %1 doesn't end on a keyframe of %2").arg(at(cut + piece.length), name));
        if (i + 1 == p.pieces.size())
            p.heldFrames = sc.held;
    }
    p.ok = true;
    return p;
}

int copyParameters(const Plan& plan, AVCodecParameters* out, AVRational* timeBase)
{
    Source src;
    if (plan.pieces.empty() || !open(plan.pieces.front().path, plan.pieces.front().stream, src))
        return AVERROR(ENOENT);
    *timeBase = src.st->time_base;
    const int r = avcodec_parameters_copy(out, src.st->codecpar);
    out->codec_tag = 0; // (the output's own tag for the codec)
    return r;
}

int copy(const Plan& plan, AVFormatContext* oc, AVStream* vst, const std::function<bool(TimeUs, qint64)>& progress)
{
    const AVRational us{1, int(kUsPerSecond)};
    int64_t lastDts = AV_NOPTS_VALUE;
    int result = 0;
    for (size_t i = 0; i < plan.pieces.size() && result >= 0; ++i) {
        const Piece& piece = plan.pieces[i];
        Source src;
        if (!open(piece.path, piece.stream, src))
            return AVERROR(ENOENT);
        const AVRational tb = src.st->time_base;
        const int64_t shift = av_rescale_q(piece.start, us, tb) - av_rescale_q(piece.sourceIn, us, tb);
        scan(src, piece, i == 0, [&](AVPacket* pkt) {
            pkt->pts += shift;
            if (pkt->dts != AV_NOPTS_VALUE)
                pkt->dts += shift;
            av_packet_rescale_ts(pkt, tb, vst->time_base);
            if (pkt->dts != AV_NOPTS_VALUE && lastDts != AV_NOPTS_VALUE && pkt->dts <= lastDts) // (two files' decoding delays)
                pkt->dts = lastDts + 1;
            if (pkt->dts != AV_NOPTS_VALUE && pkt->pts < pkt->dts)
                pkt->pts = pkt->dts;
            lastDts = pkt->dts;
            pkt->stream_index = vst->index;
            pkt->pos = -1;
            const TimeUs t = std::max<TimeUs>(0, av_rescale_q(pkt->pts, vst->time_base, us));
            const qint64 bytes = pkt->size;
            if ((result = av_interleaved_write_frame(oc, pkt)) < 0)
                return false;
            return !progress || progress(t, bytes);
        });
        if (progress && !progress(-1, 0))
            return AVERROR_EXIT;
    }
    return result;
}

} // namespace smartrender
