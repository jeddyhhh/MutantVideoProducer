#pragma once

#include "core/Deinterlacer.h"
#include "core/FFmpeg.h"
#include "core/Time.h"

#include <QString>
#include <deque>
#include <utility>

// Frame-accurate random access to one video stream.
//
// Sequential access (playback, forward scrubbing) only decodes forward;
// jumps backwards or far ahead seek to the preceding keyframe first.
// Media time 0 is the start of the file (container start_time removed),
// shared with AudioDecoder so audio and video stay in sync.
class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder&) = delete;
    VideoDecoder& operator=(const VideoDecoder&) = delete;

    // allowHardware: use the GPU video decoder when enabled and supported.
    bool open(const QString& path, int streamIndex, QString* error = nullptr, bool allowHardware = true);
    void close();
    bool isOpen() const { return m_dec != nullptr; }

    // Frame shown at media time t. With accurate=false a seek returns the
    // first decodable frame after the keyframe (fast; used for thumbnails).
    // The returned frame stays valid until the next call.
    const AVFrame* frameAt(TimeUs t, bool accurate = true);

    // Timestamp of the frame last returned by frameAt().
    TimeUs currentPts() const { return m_shownPts; }
    // The frame following the one last returned (for frame blending), or null.
    const AVFrame* nextFrame() const { return (!m_shownFromHistory && m_hasNext) ? m_next.get() : nullptr; }
    TimeUs nextPts() const { return m_nextPts; }
    // The frame before the one last returned, when it was decoded just before it (else null). Kept only once asked
    // for with setKeepPrevious (noise reduction looks at both neighbours).
    const AVFrame* previousFrame(TimeUs* pts) const;
    void setKeepPrevious(bool on) { m_keepPrevious = on; }
    TimeUs lastRequest() const { return m_lastRequest; }
    const QString& path() const { return m_path; }
    // True while frames come from the hardware decoder.
    bool usingHardware() const { return m_hwFormat != AV_PIX_FMT_NONE; }
    int streamIndex() const { return m_streamIndex; }

private:
    // The next frame, deinterlaced when the file is interlaced (bwdif: each frame from its two fields and its
    // neighbours, one frame a frame).
    bool readFrame(AVFrame* dst, TimeUs* pts);
    bool readDecoded(AVFrame* dst, TimeUs* pts);
    bool seekAndPrime(TimeUs t);
    void seek(TimeUs t);
    bool reopen();
    const AVFrame* show(const AVFrame* f, TimeUs pts, bool fromHistory);
    const AVFrame* findInHistory(TimeUs t);
    void remember(const AVFrame* f, TimeUs pts);
    void clearHistory();

    QString m_path;
    int m_streamIndex = -1;
    ff::InputFormatPtr m_fmt;
    ff::CodecContextPtr m_dec;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_cur;
    ff::FramePtr m_next;
    ff::FramePtr m_prev;
    TimeUs m_prevPts = 0;
    bool m_hasPrev = false;
    bool m_keepPrevious = false;
    AVStream* m_stream = nullptr;

    TimeUs m_startOffset = 0;
    TimeUs m_frameDur = 40000;
    TimeUs m_curPts = 0;
    TimeUs m_nextPts = 0;
    TimeUs m_lastPts = 0;
    TimeUs m_lastRequest = 0;
    bool m_hasLastPts = false;
    bool m_hasCur = false;
    bool m_hasNext = false;
    bool m_draining = false;
    bool m_eof = false;
    bool m_still = false;
    bool m_allowHardware = true;
    AVPixelFormat m_hwFormat = AV_PIX_FMT_NONE;
    bool m_hwOnlyDecoder = false;         // decoder chosen for its hardware path (can't run in software)
    ff::FramePtr m_swFrame;               // hardware frames are copied here
    // Deinterlacing (from the first interlaced frame on): the filter, the times of the frames in it.
    Deinterlacer m_deint;
    std::deque<TimeUs> m_deintPts;
    bool m_deintFlushed = false;
    bool m_interlaced = false; // a frame said so
    int m_deintW = 0, m_deintH = 0, m_deintFmt = -1;
    ff::FramePtr m_raw; // a decoded frame on its way into the filter

    // Recently decoded frames, kept while the caller is stepping backwards
    // (reverse playback) so each step doesn't re-decode from the keyframe.
    std::deque<std::pair<TimeUs, ff::FramePtr>> m_history;
    size_t m_historyBytes = 0;
    bool m_keepHistory = false;
    int m_forwardRequests = 0;
    TimeUs m_shownPts = 0;
    bool m_shownFromHistory = false;
};
