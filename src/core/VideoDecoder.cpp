#include "core/VideoDecoder.h"

#include "core/Hardware.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
}

#include <algorithm>

namespace {
// get_format callback: take the hardware format when offered, otherwise the
// first software format (the decoder then simply runs on the CPU).
AVPixelFormat pickFormat(AVCodecContext* ctx, const AVPixelFormat* fmts)
{
    const auto wanted = AVPixelFormat(reinterpret_cast<intptr_t>(ctx->opaque));
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == wanted)
            return *p;
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
        const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(*p);
        if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL))
            return *p;
    }
    return fmts[0];
}

// Forward jumps shorter than this are decoded through rather than seeked.
constexpr TimeUs kForwardDecodeLimit = 3 * kUsPerSecond;
constexpr int kMaxReadErrors = 64;
constexpr size_t kMaxHistoryBytes = 192u << 20;
}

VideoDecoder::VideoDecoder() = default;
VideoDecoder::~VideoDecoder() = default;

void VideoDecoder::close()
{
    stopDeinterlace();
    m_deintFailed = m_interlaced = false;
    clearHistory();
    m_dec.reset();
    m_fmt.reset();
    m_pkt.reset();
    m_cur.reset();
    m_next.reset();
    m_prev.reset();
    m_stream = nullptr;
    m_hasCur = m_hasNext = m_hasPrev = false;
}

bool VideoDecoder::open(const QString& path, int streamIndex, QString* error, bool allowHardware)
{
    close();
    m_path = path;
    m_streamIndex = streamIndex;
    m_allowHardware = allowHardware;
    m_hwFormat = AV_PIX_FMT_NONE;

    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        close();
        return false;
    };

    int ret = ff::openInput(path, m_fmt);
    if (ret < 0)
        return fail(QStringLiteral("open failed: %1").arg(ff::errorString(ret)));
    AVFormatContext* fc = m_fmt.get();
    if (streamIndex < 0 || unsigned(streamIndex) >= fc->nb_streams
        || fc->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO)
        return fail(QStringLiteral("stream %1 is not a video stream").arg(streamIndex));

    m_stream = fc->streams[streamIndex];
    // Don't demux what we don't decode.
    for (unsigned i = 0; i < fc->nb_streams; ++i)
        if (int(i) != streamIndex)
            fc->streams[i]->discard = AVDISCARD_ALL;

    int deviceType = 0;
    // Hardware decoding where it pays: the heavy modern codecs. Older ones (MPEG-2,
    // MPEG-4 Part 2, VC-1...) are cheap in software, and a damaged one has crashed
    // a GPU driver's decoder (NVDEC with an MPEG-2 stream), which nothing can catch.
    const AVCodecID id = m_stream->codecpar->codec_id;
    const bool worthHardware = id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_AV1;
    AVBufferRef* device = (allowHardware && worthHardware && hw::settings().hardwareDecoding) ? hw::decodeDevice(&deviceType) : nullptr;
    // The default decoder may have no hardware path (e.g. libdav1d for AV1);
    // prefer one of the codec's decoders that can use the device.
    const AVCodec* hwCodec = nullptr;
    if (device) {
        auto supports = [&](const AVCodec* c) {
            for (int i = 0;; ++i) {
                const AVCodecHWConfig* cfg = avcodec_get_hw_config(c, i);
                if (!cfg)
                    return false;
                if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AVHWDeviceType(deviceType))
                    return true;
            }
        };
        const AVCodec* preferred = avcodec_find_decoder(m_stream->codecpar->codec_id);
        if (preferred && !supports(preferred)) {
            void* it = nullptr;
            while (const AVCodec* c = av_codec_iterate(&it))
                if (av_codec_is_decoder(c) && c->id == m_stream->codecpar->codec_id && supports(c)) {
                    hwCodec = c;
                    break;
                }
        }
    }
    ret = ff::openDecoder(m_stream, m_dec, [&](AVCodecContext* ctx, const AVCodec* codec) {
        if (!device)
            return;
        for (int i = 0;; ++i) {
            const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
            if (!cfg)
                break;
            if ((cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && cfg->device_type == AVHWDeviceType(deviceType)) {
                ctx->hw_device_ctx = av_buffer_ref(device);
                ctx->opaque = reinterpret_cast<void*>(intptr_t(cfg->pix_fmt));
                ctx->get_format = pickFormat;
                m_hwFormat = cfg->pix_fmt;
                break;
            }
        }
    }, hwCodec);
    m_hwOnlyDecoder = hwCodec != nullptr && m_hwFormat != AV_PIX_FMT_NONE;
    if (ret < 0 && m_hwFormat != AV_PIX_FMT_NONE) {
        m_hwFormat = AV_PIX_FMT_NONE; // hardware setup failed: plain software decoder
        ret = ff::openDecoder(m_stream, m_dec);
    }
    if (ret < 0)
        return fail(QStringLiteral("decoder: %1").arg(ff::errorString(ret)));

    m_swFrame = ff::makeFrame();
    m_pkt = ff::makePacket();
    m_cur = ff::makeFrame();
    m_next = ff::makeFrame();
    m_prev = ff::makeFrame();
    m_startOffset = ff::formatStartTime(fc);
    AVRational fr = m_stream->avg_frame_rate.num > 0 ? m_stream->avg_frame_rate : m_stream->r_frame_rate;
    m_frameDur = (fr.num > 0 && fr.den > 0) ? std::max<TimeUs>(1000, av_rescale(kUsPerSecond, fr.den, fr.num)) : 40000;
    m_still = ff::isImageDemuxer(fc->iformat);
    m_hasCur = m_hasNext = m_draining = m_eof = m_hasLastPts = false;
    return true;
}

bool VideoDecoder::reopen()
{
    const QString path = m_path;
    const int idx = m_streamIndex;
    return open(path, idx, nullptr, m_allowHardware);
}

void VideoDecoder::stopDeinterlace()
{
    avfilter_graph_free(&m_deint);
    m_deintIn = m_deintOut = nullptr;
    m_deintPts.clear();
    m_deintFlushed = false;
}

bool VideoDecoder::startDeinterlace(const AVFrame* f)
{
    stopDeinterlace();
    m_deint = avfilter_graph_alloc();
    const AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{1, 1};
    const QByteArray args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=%4/%5:pixel_aspect=%6/%7:colorspace=%8:range=%9")
                                .arg(f->width).arg(f->height).arg(f->format)
                                .arg(m_stream->time_base.num).arg(m_stream->time_base.den).arg(sar.num).arg(sar.den)
                                .arg(int(f->colorspace)).arg(int(f->color_range))
                                .toLatin1();
    AVFilterContext* bwdif = nullptr;
    bool ok = m_deint && avfilter_graph_create_filter(&m_deintIn, avfilter_get_by_name("buffer"), "in", args.constData(), nullptr, m_deint) >= 0
              && avfilter_graph_create_filter(&bwdif, avfilter_get_by_name("bwdif"), "deint", "mode=send_frame:parity=auto:deint=interlaced",
                                              nullptr, m_deint) >= 0
              && avfilter_graph_create_filter(&m_deintOut, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_deint) >= 0
              && avfilter_link(m_deintIn, 0, bwdif, 0) >= 0 && avfilter_link(bwdif, 0, m_deintOut, 0) >= 0
              && avfilter_graph_config(m_deint, nullptr) >= 0;
    if (!ok) {
        stopDeinterlace();
        m_deintFailed = true; // (shown as it is)
        return false;
    }
    m_deintW = f->width;
    m_deintH = f->height;
    m_deintFmt = f->format;
    return true;
}

bool VideoDecoder::readFrame(AVFrame* dst, TimeUs* ptsOut)
{
    if (!m_raw)
        m_raw.reset(av_frame_alloc());
    for (;;) {
        if (m_deint) {
            av_frame_unref(dst); // (the sink moves its frame in without letting go of what was there)
            const int got = av_buffersink_get_frame(m_deintOut, dst);
            if (got >= 0) {
                *ptsOut = m_deintPts.empty() ? m_lastPts : m_deintPts.front();
                if (!m_deintPts.empty())
                    m_deintPts.pop_front();
                // (a format conversion on the way may drop them)
                dst->color_trc = m_deintTrc;
                dst->colorspace = m_deintSpace;
                dst->color_primaries = m_deintPrimaries;
                dst->color_range = m_deintRange;
                return true;
            }
            if (got == AVERROR_EOF)
                return false;
        }
        TimeUs t = 0;
        if (!readDecoded(m_raw.get(), &t)) {
            if (m_deint && !m_deintFlushed) { // the frames still in the filter
                av_buffersrc_add_frame(m_deintIn, nullptr);
                m_deintFlushed = true;
                continue;
            }
            return false;
        }
        const AVFrame* f = m_raw.get();
        m_interlaced = m_interlaced || (f->flags & AV_FRAME_FLAG_INTERLACED);
        if (m_deint && (f->width != m_deintW || f->height != m_deintH || f->format != m_deintFmt))
            stopDeinterlace(); // (a new size or format midway: the frames still in the filter are let go)
        if (!m_deint && m_interlaced && !m_deintFailed)
            startDeinterlace(f);
        if (!m_deint) {
            av_frame_unref(dst);
            av_frame_move_ref(dst, m_raw.get());
            *ptsOut = t;
            return true;
        }
        m_deintTrc = f->color_trc;
        m_deintSpace = f->colorspace;
        m_deintPrimaries = f->color_primaries;
        m_deintRange = f->color_range;
        if (av_buffersrc_add_frame(m_deintIn, m_raw.get()) < 0) {
            av_frame_unref(m_raw.get());
            continue;
        }
        m_deintPts.push_back(t);
    }
}

bool VideoDecoder::readDecoded(AVFrame* dst, TimeUs* ptsOut)
{
    int readErrors = 0;
    for (;;) {
        int ret = avcodec_receive_frame(m_dec.get(), dst);
        if (ret == 0) {
            const int64_t pts = dst->best_effort_timestamp;
            TimeUs t;
            if (pts == AV_NOPTS_VALUE)
                t = m_hasLastPts ? m_lastPts + m_frameDur : 0;
            else
                t = av_rescale_q(pts, m_stream->time_base, AV_TIME_BASE_Q) - m_startOffset;
            // Broken files sometimes repeat or reorder timestamps.
            if (m_hasLastPts && t <= m_lastPts)
                t = m_lastPts + m_frameDur;
            m_lastPts = t;
            m_hasLastPts = true;
            *ptsOut = t;
            if (dst->hw_frames_ctx) {
                // Copy the GPU surface to system memory (NV12 / P010).
                av_frame_unref(m_swFrame.get());
                if (av_hwframe_transfer_data(m_swFrame.get(), dst, 0) < 0)
                    continue;
                av_frame_copy_props(m_swFrame.get(), dst);
                av_frame_unref(dst);
                av_frame_move_ref(dst, m_swFrame.get());
            } else if (m_hwFormat != AV_PIX_FMT_NONE) {
                m_hwFormat = AV_PIX_FMT_NONE; // the decoder fell back to software for this stream
            }
            return true;
        }
        if (ret == AVERROR_EOF || m_draining) {
            m_eof = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN) && ++readErrors > kMaxReadErrors)
            return false;

        ret = av_read_frame(m_fmt.get(), m_pkt.get());
        if (ret < 0) {
            const bool atEnd = ret == AVERROR_EOF || (m_fmt->pb && avio_feof(m_fmt->pb));
            if (!atEnd && ++readErrors <= kMaxReadErrors)
                continue; // transient demux error, keep going
            avcodec_send_packet(m_dec.get(), nullptr); // drain buffered frames
            m_draining = true;
            continue;
        }
        if (m_pkt->stream_index == m_streamIndex)
            avcodec_send_packet(m_dec.get(), m_pkt.get()); // corrupt packets are skipped
        av_packet_unref(m_pkt.get());
    }
}

void VideoDecoder::seek(TimeUs t)
{
    stopDeinterlace(); // (a fresh filter after the jump: no neighbours from before it)
    m_hasCur = m_hasNext = m_draining = m_eof = m_hasLastPts = m_hasPrev = false;
    int64_t ts = av_rescale_q(t + m_startOffset, AV_TIME_BASE_Q, m_stream->time_base);
    // Seeking before the first timestamp confuses some demuxers (MPEG-TS).
    if (m_stream->start_time != AV_NOPTS_VALUE)
        ts = std::max(ts, m_stream->start_time);
    int ret = av_seek_frame(m_fmt.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
        ret = avformat_seek_file(m_fmt.get(), -1, INT64_MIN, t + m_startOffset, t + m_startOffset, 0);
    if (ret < 0) {
        // Unseekable input: restart from the beginning and decode forward.
        reopen();
        return;
    }
    avcodec_flush_buffers(m_dec.get());
}

void VideoDecoder::clearHistory()
{
    m_history.clear();
    m_historyBytes = 0;
}

const AVFrame* VideoDecoder::show(const AVFrame* f, TimeUs pts, bool fromHistory)
{
    m_shownPts = pts;
    m_shownFromHistory = fromHistory;
    return f;
}

void VideoDecoder::remember(const AVFrame* f, TimeUs pts)
{
    // Keep the history sorted: frames decoded after a backwards seek are
    // earlier than what we hold, and the later ones are no longer needed.
    while (!m_history.empty() && m_history.back().first >= pts) {
        m_historyBytes -= size_t(std::max(0, av_image_get_buffer_size(AVPixelFormat(m_history.back().second->format),
                                                                      m_history.back().second->width,
                                                                      m_history.back().second->height, 1)));
        m_history.pop_back();
    }
    ff::FramePtr clone(av_frame_clone(f)); // shares the decoded buffers, no copy
    if (!clone)
        return;
    const int bytes = av_image_get_buffer_size(AVPixelFormat(f->format), f->width, f->height, 1);
    m_historyBytes += size_t(std::max(0, bytes));
    m_history.emplace_back(pts, std::move(clone));
    while (m_historyBytes > kMaxHistoryBytes && m_history.size() > 1) {
        const AVFrame* old = m_history.front().second.get();
        m_historyBytes -= size_t(std::max(0, av_image_get_buffer_size(AVPixelFormat(old->format), old->width, old->height, 1)));
        m_history.pop_front();
    }
}

const AVFrame* VideoDecoder::previousFrame(TimeUs* pts) const
{
    if (!m_keepPrevious || !m_hasPrev || !m_hasCur || m_shownFromHistory || m_shownPts != m_curPts || m_curPts - m_prevPts > m_frameDur * 3 / 2)
        return nullptr;
    if (pts)
        *pts = m_prevPts;
    return m_prev.get();
}

const AVFrame* VideoDecoder::findInHistory(TimeUs t)
{
    for (size_t i = m_history.size(); i-- > 0;) {
        const TimeUs pts = m_history[i].first;
        if (pts > t)
            continue;
        const TimeUs next = i + 1 < m_history.size() ? m_history[i + 1].first : (m_hasCur ? m_curPts : pts);
        // Only trust contiguous runs (seeks leave gaps).
        if (t < next && next - pts <= m_frameDur * 5 / 2)
            return show(m_history[i].second.get(), pts, true);
        return nullptr;
    }
    return nullptr;
}

bool VideoDecoder::seekAndPrime(TimeUs t)
{
    // Some demuxers land after the requested time; back off until the first
    // decoded frame is at or before the target.
    TimeUs backoff = 0;
    for (int attempt = 0; attempt < 5; ++attempt) {
        const TimeUs seekT = std::max<TimeUs>(0, t - backoff);
        seek(seekT);
        if (!m_dec)
            return false;
        if (readFrame(m_cur.get(), &m_curPts)) {
            m_hasCur = true;
            if (m_curPts <= t + m_frameDur / 2)
                return true;
            if (seekT == 0) {
                // Even a seek to the start landed late (MPEG-PS/TS can land on
                // the second keyframe): read from the true start instead.
                if (reopen() && readFrame(m_cur.get(), &m_curPts))
                    return true;
                return m_hasCur;
            }
        } else if (seekT == 0) {
            // Last resort: start over from the beginning of the file.
            if (!reopen() || !readFrame(m_cur.get(), &m_curPts))
                return false;
            m_hasCur = true;
            return true;
        }
        backoff = backoff ? backoff * 4 : kUsPerSecond;
    }
    return m_hasCur;
}

const AVFrame* VideoDecoder::frameAt(TimeUs t, bool accurate)
{
    if (!m_dec)
        return nullptr;
    m_lastRequest = t;

    if (m_still) {
        if (!m_hasCur) {
            if (!readFrame(m_cur.get(), &m_curPts)) {
                // Image demuxers can't always rewind; reopen and retry once.
                if (!reopen() || !readFrame(m_cur.get(), &m_curPts))
                    return nullptr;
            }
            m_hasCur = true;
        }
        return show(m_cur.get(), m_curPts, false);
    }

    // Containers with millisecond timestamps (Matroska, WebM, FLV) round frame
    // times, so a frame due at 66.667 ms may be stamped 67 ms. Look slightly
    // ahead so frame-grid requests still land on the intended frame.
    t = std::max<TimeUs>(t, 0) + std::min<TimeUs>(m_frameDur / 4, 5000);
    if (m_hasCur && m_hasNext && t >= m_curPts && t < m_nextPts)
        return show(m_cur.get(), m_curPts, false);

    if (m_hasCur && t < m_curPts) {
        // Going backwards (reverse playback, stepping back a frame).
        if (m_curPts - t < 4 * kUsPerSecond)
            m_keepHistory = true;
        m_forwardRequests = 0;
        if (const AVFrame* f = findInHistory(t))
            return f;
    } else if (m_keepHistory && ++m_forwardRequests > 200) {
        m_keepHistory = false; // plain forward playback again: release memory
        clearHistory();
    }

    const bool needSeek = !m_hasCur || t < m_curPts || t > m_curPts + kForwardDecodeLimit;
    if (needSeek) {
        if (!seekAndPrime(t)) {
            if (!m_hwOnlyDecoder)
                return nullptr;
            // The GPU rejected this stream and the chosen decoder has no
            // software path: reopen with the normal software decoder.
            const QString path = m_path;
            if (!open(path, m_streamIndex, nullptr, false) || !seekAndPrime(t))
                return nullptr;
        }
        if (!accurate)
            return show(m_cur.get(), m_curPts, false);
    }

    for (;;) {
        if (!m_hasNext) {
            if (m_eof || !readFrame(m_next.get(), &m_nextPts))
                return m_hasCur ? show(m_cur.get(), m_curPts, false) : nullptr; // past the end: hold last frame
            m_hasNext = true;
        }
        if (m_nextPts > t)
            return show(m_cur.get(), m_curPts, false);
        if (m_keepHistory)
            remember(m_cur.get(), m_curPts);
        if (m_keepPrevious) {
            av_frame_unref(m_prev.get());
            m_hasPrev = av_frame_ref(m_prev.get(), m_cur.get()) >= 0;
            m_prevPts = m_curPts;
        }
        std::swap(m_cur, m_next);
        m_curPts = m_nextPts;
        m_hasNext = false;
    }
}
