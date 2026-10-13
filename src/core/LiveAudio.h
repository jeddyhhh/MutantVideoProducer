#pragma once

// Live audio inputs for the Live tab's recordings: microphones, line-in and
// capture-card audio (Windows: DirectShow, including "Stereo Mix" when it is
// enabled; Linux: PulseAudio sources, including the monitors of outputs, or
// ALSA), opened through FFmpeg's capture devices and read on a background
// thread. Samples are delivered as interleaved stereo float at 48 kHz,
// stamped with the time they were captured on liveClock().

#include <QString>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

struct LiveAudioSource {
    QString name;   // shown in the UI
    QString format; // FFmpeg input device: dshow, pulse, alsa (lavfi in tests); empty = no audio
    QString id;     // device name / URL for that format
    bool isNull() const { return format.isEmpty(); }
    bool operator==(const LiveAudioSource&) const = default;
};

// Audio capture devices of this machine.
std::vector<LiveAudioSource> listLiveAudioSources();

// Monotonic clock (seconds) that live audio and video are stamped with.
double liveClock();

class LiveAudioCapture {
public:
    static constexpr int kSampleRate = 48000;
    // Called on the capture thread with `frames` stereo samples that started at `time` (liveClock()).
    using Sink = std::function<void(const float* samples, int frames, double time)>;

    LiveAudioCapture();
    ~LiveAudioCapture();
    LiveAudioCapture(const LiveAudioCapture&) = delete;
    LiveAudioCapture& operator=(const LiveAudioCapture&) = delete;

    bool start(const LiveAudioSource& source, Sink sink, QString* error);
    void stop();
    bool isRunning() const;

    // Linear peaks of each channel since the last call (for a meter).
    std::pair<float, float> takePeaks();
    QString failure() const;     // set when reading stopped because of an error
    QString description() const; // how the source was opened ("DirectShow pcm_s16le 44100 Hz stereo")

    struct Impl;

private:
    std::shared_ptr<Impl> d; // shared with the reading thread
};
