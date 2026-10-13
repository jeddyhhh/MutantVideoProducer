#pragma once

#include <vector>

struct AVFilterGraph;
struct AVFilterContext;

// Pitch-preserving time stretch of interleaved stereo float audio, using
// FFmpeg's atempo filter. speed 2.0 = twice as fast (half the samples out).
class AudioTempo {
public:
    AudioTempo() = default;
    ~AudioTempo();
    AudioTempo(const AudioTempo&) = delete;
    AudioTempo& operator=(const AudioTempo&) = delete;

    bool reset(double speed, int sampleRate);
    void push(const float* stereo, int frames);
    // Appends whatever output is ready (interleaved stereo).
    void pull(std::vector<float>& out);

private:
    void release();

    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_src = nullptr;
    AVFilterContext* m_sink = nullptr;
    int m_rate = 48000;
    long long m_pts = 0;
};
