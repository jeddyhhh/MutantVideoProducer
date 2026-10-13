#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Pitch-preserving playback along an arbitrary source-position curve (speed
// ramps): WSOLA. Hann-windowed grains are read around the curve's position
// and each is nudged (by at most a few ms) to line up with the previous one,
// then overlap-added. Because every grain is placed from the curve itself,
// the output never drifts from the timeline however the speed changes.
// Interleaved stereo float.
class AudioStretch {
public:
    static constexpr int kFrame = 1536;          // grain length (32 ms at 48 kHz)
    static constexpr int kHop = kFrame / 2;      // output hop, 50% overlap
    static constexpr int kSeek = 256;            // alignment search, +-5 ms
    static constexpr double kMinRate = 1.0 / 16; // slower than this is silent (a freeze)

    // Source sample position at timeline sample t (fractional allowed).
    using Position = std::function<double(int64_t t)>;
    // Reads `count` stereo frames of source starting at `from`.
    using Fetch = std::function<void(int64_t from, int count, float* stereo)>;

    AudioStretch();

    // True when the next render() continues from where the last one ended.
    bool continuesAt(int64_t timelinePos) const { return m_valid && m_nextOut == timelinePos; }
    void reset(int64_t timelinePos);
    // Renders timeline samples [a, a + n) into out (2 * n floats).
    void render(int64_t a, int n, float* out, const Position& position, const Fetch& fetch);

private:
    void addGrain(int64_t start, const Position& position, const Fetch& fetch);
    // Reads `count` frames of source from `pos` in direction dir (+1 / -1).
    void readGrain(int64_t pos, int dir, int count, float* dst, const Fetch& fetch);

    std::vector<float> m_window;
    std::vector<float> m_ola;   // pending output (stereo) from m_olaStart
    int64_t m_olaStart = 0;
    int64_t m_nextGrain = 0;    // timeline sample the next grain starts at
    int64_t m_nextOut = 0;
    bool m_valid = false;
    bool m_prevValid = false;   // the previous grain was played (alignment reference)
    int64_t m_prevPos = 0;
    int m_prevDir = 1;
    std::vector<float> m_span, m_ref, m_grain, m_refMono, m_spanMono;
};
