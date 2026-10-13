#pragma once

#include "core/Captions.h"
#include "core/Project.h"

#include <QString>
#include <QStringList>
#include <functional>
#include <vector>

class QOffscreenSurface;

// How the picture fills an output frame of another shape than the project
// (e.g. a 16:9 project rendered for a 9:16 Reels template).
enum class Framing { Fit, Fill, Stretch, BlurFit };

struct ExportSettings {
    QString path;
    QString muxer;               // FFmpeg muxer name; empty = guess from extension
    QString videoEncoder;        // empty = no video stream
    QString audioEncoder;        // empty = no audio stream
    int width = 1920;
    int height = 1080;
    FrameRate fps{30, 1};
    int64_t videoBitrate = 0;    // bit/s; 0 = use quality / encoder default
    int quality = -1;            // CRF (x264/x265/vpx/aom/svt) or qscale (MPEG-family); -1 = default
    QString encoderPreset;       // value for the encoder's "preset" option
    QString videoOptions;        // extra encoder options "key=value:key=value"
    QString audioOptions;
    int64_t audioBitrate = 192000;
    int sampleRate = 48000;
    int channels = 2;
    TimeUs rangeStart = 0;
    TimeUs rangeEnd = 0;
    QString pixelFormat;         // FFmpeg pixel format name; empty = automatic
    int64_t targetSizeBytes = 0; // > 0: video bitrate chosen so the file fits this size
    Framing framing = Framing::Fit;
    QString muxerOptions;        // "key=value:key=value" for the muxer (HLS segment length...)
    double gopSeconds = 0;       // keyframe interval; 0 = 2 seconds
    std::vector<Caption> captions; // a subtitle track (timeline times), where the format has one
};

struct ExportPreset {
    QString name;
    QString muxer;
    QString extension;
    QString videoEncoder;
    QString audioEncoder;
    int width = 0;               // 0 = project size; width only = keep aspect
    int height = 0;
    FrameRate fps{0, 0};         // invalid = project rate
    int quality = -1;
    int64_t videoBitrate = 0;
    QString encoderPreset;
    QString videoOptions;
    int64_t audioBitrate = 192000;
    int sampleRate = 48000;
    int channels = 2;
    QString category;            // template group in the Render As dialog
    QString notes;               // platform limits / what the template is for
    QString pixelFormat;
    double targetSizeMB = 0;     // fit the file into this size (messaging apps)
    int maxSeconds = 0;          // platform length limit (the dialog warns)
    Framing framing = Framing::Fit;
    QString muxerOptions;
    double gopSeconds = 0;
};

const std::vector<ExportPreset>& exportPresets();
// Same settings on a hardware encoder (NVENC, Quick Sync, AMF) when the
// codec has one that works on this PC; `used` gets its name (or stays empty).
ExportSettings withGpuEncoder(const ExportSettings& s, QString* used = nullptr);
// "name.png" -> "name_%05d.png" for image sequences.
QString imageSequencePattern(const QString& path);
// False for presets whose encoders this machine can't use (e.g. NVENC without NVIDIA).
bool presetAvailable(const ExportPreset& p);
// Fills settings from a preset, resolving "project" placeholders.
ExportSettings settingsFromPreset(const ExportPreset& p, const ProjectSettings& project);

// Worker-owned counters, delivered as immutable snapshots. All durations are measured wall time.
struct ExportStatus {
    QString stage, detail, renderer, videoEncoder, audioEncoder, pixelFormat, sampleFormat;
    QString colorConversion = QStringLiteral("CPU (swscale)");
    QStringList activeClips;
    qint64 frames = 0, totalFrames = 0, samples = 0, totalSamples = 0;
    qint64 videoPackets = 0, audioPackets = 0, subtitlePackets = 0, packetBytes = 0, fileBytes = 0;
    double elapsed = 0, renderingSeconds = 0, conversionSeconds = 0, audioSeconds = 0, encodingSeconds = 0, muxingSeconds = 0;
    double position = 0, progress = 0;
    int width = 0, height = 0, sampleRate = 0, channels = 0;
};

class Exporter {
public:
    // Called with progress 0..1; return false to cancel.
    using Progress = std::function<bool(double)>;

    // With a GPU surface (created on the GUI thread) frames are rendered on
    // the GPU when enabled; otherwise on the CPU.
    bool run(const ProjectSnapshot& snap, const ExportSettings& settings, const Progress& progress = {},
             QOffscreenSurface* gpuSurface = nullptr, const std::function<void(const ExportStatus&)>& status = {});
    const QString& error() const { return m_error; }
    const QString& rendererName() const { return m_rendererName; }

private:
    QString m_error;
    QString m_rendererName;
};
