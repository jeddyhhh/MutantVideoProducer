#pragma once

#include "core/Project.h"
#include "core/PreviewCache.h"

#include <algorithm>
#include <QAudioFormat>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

class QAudioSink;
class QOffscreenSurface;
class AudioRingDevice;

// Drives preview rendering and real-time playback.
//
// - A video thread renders frames with its own Compositor: on demand while
//   stopped (scrubbing), and a few frames ahead while playing.
// - An audio thread mixes into a ring buffer that the audio device pulls from.
// - The audio device is the master clock; video frames are shown when due
//   and dropped when rendering falls behind.
class PlaybackEngine : public QObject {
    Q_OBJECT
public:
    explicit PlaybackEngine(QObject* parent = nullptr);
    ~PlaybackEngine() override;

    void setSnapshot(std::shared_ptr<const ProjectSnapshot> snap);
    // Preview resolution divisor: 1 = full, 2 = half, 4 = quarter.
    void setPreviewDivisor(int divisor);
    int previewDivisor() const { return m_divisor; }

    // Renders the frame at t asynchronously (latest request wins); emits frameReady.
    void requestFrame(TimeUs t);

    void play(TimeUs from);
    void stop();
    bool isPlaying() const { return m_playing; }
    TimeUs position() const { return m_clock; }
    // How long the sound takes from the device to the ears beyond what the device reports (HDMI and monitor speakers,
    // Bluetooth): the picture waits that long, so mouths and hits line up with what is heard. Exports don't change.
    void setOutputLatency(TimeUs latency) { m_outputLatency = std::max<TimeUs>(0, latency); }
    TimeUs outputLatency() const { return m_outputLatency; }
    // While playing: where the sound the device is playing now comes from on the timeline.
    TimeUs heardPosition();

    // Recreates the preview renderer (after GPU / hardware decoding settings change).
    void resetRenderer();

    // Pre-rendering (on unless switched off): while stopped, a thread renders the heavy stretches near the cursor (frames
    // slower to make than the frame rate allows) into the cache folder, and the preview plays them from there. An edit
    // makes its stretch new, so what is played is always what the project looks like now.
    void setPreRender(bool on);
    bool preRender() const { return m_preRender; }
    void clearPreRender();
    struct PreRenderSpan {
        TimeUs start, end;
        bool done; // pre-rendered (else heavy, not yet)
        bool operator==(const PreRenderSpan&) const = default;
    };
    precache::Store& preRenderStore() { return m_store; }

    // Peak levels (linear) of the audio sent to the device since the last call.
    std::pair<float, float> takePeaks();
    // Per audio track (by id), for what has been heard since the last call.
    std::map<int, std::pair<float, float>> takeTrackPeaks();

    // Audio scrubbing: while stopped, plays a short snippet of the mix at t
    // (dragging the cursor, stepping frames). The newest request wins.
    void scrubAudio(TimeUs t);

    // Sound goes to the system's default output, and follows it when it
    // changes (headphones plugged in, a Bluetooth device connecting, the device
    // in use unplugged). Opens it again even when it hasn't changed.
    void reopenAudioOutput();

signals:
    void frameReady(const QImage& image, TimeUs t);
    void positionChanged(TimeUs t);
    void rendererChanged(const QString& name);
    // The heavy stretches near the cursor, pre-rendered or not yet (for the timeline's ruler).
    void preRenderChanged(const std::vector<PlaybackEngine::PreRenderSpan>& spans);

private:
    void onTick();
    void videoThreadMain();
    void preRenderThreadMain();
    QImage cachedFrame(const std::shared_ptr<const ProjectSnapshot>& snap, TimeUs t, QSize size);
    void audioThreadMain();
    TimeUs computeClock();
    QSize previewSize(const ProjectSnapshot& s) const;
    void setupAudio();
    void followDefaultOutput();

    // Shared state (guarded by m_mutex).
    std::mutex m_mutex;
    std::condition_variable m_videoCv;
    std::condition_variable m_audioCv;
    std::shared_ptr<const ProjectSnapshot> m_snap;
    std::optional<TimeUs> m_scrubRequest;
    std::deque<std::pair<TimeUs, QImage>> m_queue;
    int64_t m_nextFrame = 0;
    uint64_t m_generation = 0;
    int m_divisor = 2;
    bool m_quit = false;
    std::atomic<bool> m_playing{false};
    std::atomic<TimeUs> m_clock{0};

    // Audio ring (guarded by m_ringMutex).
    friend class AudioRingDevice;
    std::mutex m_ringMutex;
    std::vector<float> m_ring;
    size_t m_ringRead = 0;
    size_t m_ringFill = 0;
    int64_t m_audioPos = 0;      // next timeline sample the mixer produces
    uint64_t m_audioGeneration = 0;
    int m_sampleRate = 48000;
    float m_peakL = 0, m_peakR = 0;     // guarded by m_ringMutex
    int64_t m_ringReadPos = 0;          // timeline sample at the ring's read head (guarded by m_ringMutex)
    // Track peaks per mixed chunk, by the timeline sample the chunk ends at.
    std::deque<std::pair<int64_t, std::map<int, std::pair<float, float>>>> m_meterQueue;
    std::optional<int64_t> m_scrubAudio;  // pending scrub snippet start (guarded by m_ringMutex)
    bool m_scrubSink = false;             // the sink runs for scrubbing (GUI thread)
    QTimer m_scrubStop;

    QAudioSink* m_sink = nullptr;
    AudioRingDevice* m_device = nullptr;
    QAudioFormat m_format;
    QByteArray m_outputId; // the device the sink plays to

    std::unique_ptr<QOffscreenSurface> m_gpuSurface; // created on the GUI thread
    std::atomic<int> m_rendererGeneration{0};

    QTimer m_tick;
    QElapsedTimer m_wallClock;
    TimeUs m_startPos = 0;
    TimeUs m_clockAdjust = 0;
    TimeUs m_outputLatency = 0;

    std::thread m_videoThread;
    std::thread m_audioThread;

    // Pre-rendering.
    precache::Store m_store;
    std::mutex m_preMutex; // guards the stretches and what they are for
    std::shared_ptr<const ProjectSnapshot> m_preSnap;
    QSize m_preSize;
    bool m_preProxies = false;
    std::vector<precache::Stretch> m_stretches;
    std::condition_variable m_preCv; // (with m_mutex)
    std::atomic<bool> m_preRender{true};
    std::atomic<int> m_preCleared{0}; // bumped by clearPreRender: the thread forgets which stretches it measured heavy
    std::atomic<TimeUs> m_cursorHint{0};
    std::chrono::steady_clock::time_point m_snapChanged; // guarded by m_mutex
    std::unique_ptr<QOffscreenSurface> m_preSurface;     // created on the GUI thread
    std::thread m_preThread;
};
