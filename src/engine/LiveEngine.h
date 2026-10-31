#pragma once

#include "core/BeatTracker.h"
#include "core/LiveAudio.h"
#include "core/LiveCapture.h"
#include "core/LiveModulation.h"
#include "core/Project.h"

#include <QImage>
#include <QObject>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

class LiveRecorder;
class QOffscreenSurface;

// Drives the Live tab: a thread opens the capture source (and the layers'
// sources, if chosen), then renders the project's Live FX chain over the
// newest captured picture at a steady frame rate (so animated effects keep
// moving when the source is still) and hands the results to the UI, dropping
// frames the UI has not shown yet. Per frame: the layers (each with its own FX)
// are laid over the main picture, then the Live FX, the active pad and any held
// (momentary) pads are applied (No Effects layers are laid over after them), with modulated settings moved by the beat
// clock, the audio levels and MIDI controllers, and the overlays drawn on top.
// The result can be recorded, and kept in a replay buffer of the last seconds.
class LiveEngine : public QObject {
    Q_OBJECT
public:
    struct Options {
        LiveSource source;
        LiveFormat format;  // cameras: format to ask for
        QSize maxSize;      // processing size (fit inside; empty = source size)
        double fps = 30;    // render rate
        bool mirror = false;
        LiveAudioSource audio; // recorded with the clips (null = none)
        int pad = -1;          // active pad (0..15) on start, -1 = none
        QSize canvas;          // a fixed output shape (portrait, square...); empty = the source's shape, fitted in maxSize
        LiveFraming framing = LiveFraming::CropFill;
        std::array<LiveSource, kLiveLayers> layers; // sources laid over the main one (empty name = none)
        std::array<LiveLayerSettings, kLiveLayers> layerSettings;
        std::vector<LiveSource> inputs; // the switcher's other inputs (2, 3...; empty names = unused)
        int program = 0;                // the input on air: 0 = the source above, 1.. = inputs
    };
    static constexpr int kMaxInputs = 4; // switcher inputs besides the main source

    explicit LiveEngine(QObject* parent = nullptr);
    ~LiveEngine() override;

    void setSnapshot(std::shared_ptr<const ProjectSnapshot> snap);
    // Opens the source asynchronously: emits started() or failed().
    void start(const Options& options);
    void stop();
    bool isRunning() const { return m_running; }
    // The latest picture before the effects (as the clean recording has it); null when not running.
    QImage cleanFrame() const;

    // While running.
    void setMirror(bool on) { m_mirror = on; }
    void setMaxSize(QSize size);
    // A fixed output shape (empty = off) and how the source goes into it.
    void setCanvas(QSize canvas, LiveFraming framing);
    // Pan and zoom of the source in the output (any size; the source is
    // captured at the zoomed size, so zooming in keeps its detail).
    void setPanZoom(const LivePanZoom& pz);
    LivePanZoom panZoom() const;
    void setFps(double fps) { m_fps = std::max(1.0, fps); }
    // Paused: keeps the source open but stops rendering (tab hidden).
    void setPaused(bool paused);
    // Recreates the renderer (GPU setting changed).
    void resetRenderer() { ++m_rendererGeneration; }
    // Switches the audio input (null = none) without reopening the picture.
    void setAudioSource(const LiveAudioSource& source);
    // Beat detection on the audio input (settings, tempo, the onset curve for a display).
    BeatTracker* beatTracker() const { return m_beats.get(); }
    // Switches to pad `pad` (0..15, -1 = none: just the Live FX), blending
    // from the previous one with `transition` over `seconds` (0 = cut).
    void setPad(int pad, const TransitionSpec& transition, double seconds);
    // Momentary pads held down now, in the order they were pressed: their
    // chains go on top of the active pad's, straight away.
    void setHeldPads(const std::vector<int>& pads);
    // A layer's source (empty name = none), opened or closed without touching the main one.
    void setLayer(int layer, const LiveSource& source);
    void setLayerSettings(int layer, const LiveLayerSettings& settings);
    // Video files: the next one in the playlist (of the main source: -1, or a layer's).
    void nextFile(int layer);
    // The switcher: other inputs kept open beside the main source (each fitted
    // to its picture), and which one is on air (0 = the main source), switched
    // with `transition` over `seconds` (0 = a cut). Effects, pads and overlays
    // go on whatever is on air.
    void setInputs(const std::vector<LiveSource>& inputs);
    void setProgram(int input, const TransitionSpec& transition, double seconds);
    int program() const { return m_program; }

    // The beat clock modulation follows: a beat now, at `bpm`, `barBeat` into
    // its bar (0 = the 1). Between beats (and without any) it runs on at the tempo.
    void noteBeat(double bpm, int barBeat);
    void setTempo(double bpm);
    // A MIDI controller moved (channel 0..15, controller 0..127, value 0..127).
    void setMidiController(int channel, int controller, int value);
    // What modulation reads now (for tests and displays).
    ModInputs modInputs() const;

    // Records the live pictures (with the effects, or the clean picture) to
    // `path` from the next frame on, until stopRecording() or stop().
    void startRecording(const QString& path, bool withEffects);
    void stopRecording();
    bool isRecording() const { return m_recording; }

    // Replay buffer: keeps the last `seconds` (with the effects or the clean
    // picture) while capturing; saveReplay writes them to `path`.
    void setReplay(bool on, double seconds, bool withEffects);
    // Sends the output (with the effects and overlays) to other apps while
    // capturing: as a Spout sender called `spoutName`, and through the OBS
    // Virtual Camera (core/VideoShare.h). Rendering goes on with the tab hidden.
    void setSharing(bool spout, const QString& spoutName, bool virtualCamera);
    // Streams the output (with effects and overlays) and the audio input to
    // `url` (an RTMP / RTMPS / SRT address with the stream key) at `kbps`,
    // while capturing, until stopStream() or stop().
    // Cue: renders a pad (Live FX + that pad + overlays) or a scene (its Live
    // FX, pad and overlays) beside the output, smaller, with its own effect
    // state, without putting it live (cueFrameReady). kind 0 = none, 1 pad, 2 scene.
    enum CueKind { CueNone, CuePad, CueScene };
    void setCue(int kind, int index);
    void startStream(const QString& url, int kbps);
    void stopStream();
    bool isStreaming() const { return m_streaming; }
    void saveReplay(const QString& path);
    bool replayRunning() const { return m_replayRunning; }

signals:
    void frameReady(const QImage& image);
    void started(const QString& description);
    void failed(const QString& error);
    void stopped();
    // Once a second: capture and render rates, renderer.
    void statsChanged(double captureFps, double renderFps, double renderMs, const QString& renderer, QSize sourceSize);
    // The audio input opened (description) or could not be opened / stopped (error); both empty = no audio.
    void audioStatus(const QString& description, const QString& error);
    // About 25 times a second while an audio input is open: linear peaks.
    void audioLevel(float left, float right);
    // A beat heard on the audio input (see BeatTracker): the tempo and how sure it is.
    void beat(double bpm, double confidence);
    void recordingStarted(const QString& path, const QString& encoder);
    void recordingProgress(double seconds, qint64 droppedFrames);
    // The file is complete (or failed); its thumbnail is at liveThumbnailPath(path).
    void recordingFinished(const QString& path, bool ok, const QString& error, double seconds);
    // A layer's source opened (description), failed (error), or was closed (both empty).
    void layerStatus(int layer, const QString& description, const QString& error);
    // The replay buffer started (encoder) or stopped / failed (error; both empty = off).
    void replayStatus(bool running, const QString& encoder, const QString& error);
    void replaySaved(const QString& path, bool ok, const QString& error, double seconds);
    // Sharing changed: the Spout sender's name (empty = off) or why it failed;
    // the virtual camera's size (empty = off) or why it failed.
    void sharingStatus(const QString& spoutName, const QString& spoutError, QSize cameraSize, const QString& cameraError);
    // The stream: connecting (live false, error empty), live, or stopped / failed (error).
    void streamStatus(bool connecting, bool live, const QString& encoder, const QString& error);
    // The cued pad / scene, about 15 times a second.
    void cueFrameReady(const QImage& image);
    // About 8 times a second with switcher inputs: each input's picture, small
    // (0 = the main source; a null image = not open).
    void inputFrames(const QList<QImage>& pictures);
    // Once a second while streaming: how long, pictures dropped.
    void streamProgress(double seconds, qint64 droppedFrames);

private:
    void threadMain(Options options);

    std::unique_ptr<QOffscreenSurface> m_gpuSurface; // created on the GUI thread
    std::thread m_thread;
    std::thread m_saveThread; // writing a replay
    std::atomic<bool> m_quit{false}, m_running{false}, m_mirror{false}, m_paused{false}, m_framePending{false};
    std::atomic<double> m_fps{30};
    std::atomic<int> m_rendererGeneration{0};
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::shared_ptr<const ProjectSnapshot> m_snap;
    QSize m_maxSize;
    bool m_maxSizeChanged = false;
    QSize m_canvas; // guarded by m_mutex
    QImage m_clean; // guarded by m_mutex
    LiveFraming m_framing = LiveFraming::CropFill;
    LivePanZoom m_panZoom; // guarded by m_mutex
    // Recording requests (guarded by m_mutex).
    QString m_recordPath;
    bool m_recordEffects = true, m_recordStart = false, m_recordStop = false;
    std::atomic<bool> m_recording{false};
    LiveAudioSource m_audioRequest; // guarded by m_mutex
    bool m_audioChange = false;
    int m_padRequest = -1; // guarded by m_mutex
    TransitionSpec m_padTransition;
    double m_padSeconds = 0;
    bool m_padChange = false;
    std::vector<int> m_held;           // guarded by m_mutex
    std::array<LiveSource, kLiveLayers> m_layerRequest;         // guarded by m_mutex
    std::array<bool, kLiveLayers> m_layerChange{};
    std::array<LiveLayerSettings, kLiveLayers> m_layerSettings; // guarded by m_mutex
    bool m_nextFile = false;
    std::array<bool, kLiveLayers> m_nextLayerFile{};
    std::vector<LiveSource> m_inputsRequest; // guarded by m_mutex
    bool m_inputsChange = false;
    int m_programRequest = 0; // guarded by m_mutex
    TransitionSpec m_programTransition;
    double m_programSeconds = 0;
    bool m_programChange = false;
    std::atomic<int> m_program{0};
    // Replay buffer requests (guarded by m_mutex).
    bool m_replayWanted = false, m_replayEffects = true, m_replayChange = false;
    double m_replaySeconds = 30;
    QString m_replaySavePath;
    std::atomic<bool> m_replayRunning{false};
    // Sharing requests (guarded by m_mutex).
    bool m_spoutWanted = false, m_cameraWanted = false, m_shareChange = false;
    QString m_spoutName = QStringLiteral("Mutant Video Producer");
    std::atomic<bool> m_sharing{false};
    // Stream requests (guarded by m_mutex).
    QString m_streamUrl;
    int m_streamKbps = 4500;
    bool m_streamStart = false, m_streamStop = false;
    std::atomic<bool> m_streaming{false};
    std::atomic<int> m_cueKind{0}, m_cueIndex{-1};
    std::atomic<bool> m_cuePending{false};
    // Modulation inputs.
    mutable std::mutex m_clockMutex;
    std::chrono::steady_clock::time_point m_lastBeat = std::chrono::steady_clock::now();
    double m_beatCount = 0, m_clockBpm = 120;
    int m_barBeat = 0;
    std::atomic<float> m_level{0}, m_bass{0}, m_mids{0}, m_highs{0};
    // The last moment of the audio input (mono), for the visualiser overlays.
    std::mutex m_scopeMutex;
    std::vector<float> m_scope = std::vector<float>(4096, 0.0f);
    size_t m_scopePos = 0;
    std::array<std::array<std::atomic<uint8_t>, 128>, 16> m_midi{};
    // The recordings the audio thread feeds (guarded by m_audioMutex).
    std::mutex m_audioMutex;
    std::shared_ptr<BeatTracker> m_beats = std::make_shared<BeatTracker>(LiveAudioCapture::kSampleRate);
    LiveRecorder* m_audioTarget = nullptr;
    LiveRecorder* m_replayAudioTarget = nullptr;
    LiveRecorder* m_streamAudioTarget = nullptr;
};
