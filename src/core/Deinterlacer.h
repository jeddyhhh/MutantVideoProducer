#pragma once

// Interlaced frames made whole with FFmpeg's bwdif: each frame from its two fields and its neighbours, one frame out
// for each frame in (one frame behind; flush() lets the last one out). Frames not marked interlaced go through as they
// are. The colour tags of what goes in are kept on what comes out.

#include "core/FFmpeg.h"

struct AVFilterGraph;
struct AVFilterContext;

class Deinterlacer {
public:
    Deinterlacer() = default;
    ~Deinterlacer();
    Deinterlacer(const Deinterlacer&) = delete;
    Deinterlacer& operator=(const Deinterlacer&) = delete;

    // Hands it a frame (a reference is taken; `f` is left as it was). A new size or format starts the filter again
    // (what was in it is let go). False when the filter can't be made: show the frame as it is.
    bool push(const AVFrame* f);
    // The next frame made, into `out` (whatever it held is let go). False: none ready yet (or none left after flush).
    bool pull(AVFrame* out);
    // No more frames are coming (the end of the file): the ones still in it come out of pull().
    void flush();
    // Forgets the frames in it (after a seek, or another file).
    void reset();
    bool failed() const { return m_failed; }

private:
    bool start(const AVFrame* f);

    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_in = nullptr;
    AVFilterContext* m_out = nullptr;
    bool m_flushed = false;
    bool m_failed = false;
    int m_w = 0, m_h = 0, m_fmt = -1;
    AVColorTransferCharacteristic m_trc = AVCOL_TRC_UNSPECIFIED;
    AVColorSpace m_space = AVCOL_SPC_UNSPECIFIED;
    AVColorPrimaries m_primaries = AVCOL_PRI_UNSPECIFIED;
    AVColorRange m_range = AVCOL_RANGE_UNSPECIFIED;
};
