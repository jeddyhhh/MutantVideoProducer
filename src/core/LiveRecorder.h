#pragma once

// Records the Live tab's pictures to a video file (H.264 in MP4) at a
// constant frame rate, optionally with sound from a live audio input (AAC).
// Frames are converted and encoded on a background thread; a hardware
// encoder (NVENC, Quick Sync, AMF) is used when the machine has one,
// otherwise libx264. Pictures and sound are placed by the time they were
// captured (liveClock()), so they stay in sync even when pictures come
// late or are dropped (the previous slot is then filled with the picture).
//
// As a stream (startStream) it sends the pictures and sound to a server
// (Twitch, YouTube, Kick, any RTMP / RTMPS or SRT address) at a constant
// bitrate with a keyframe every 2 seconds, always with a sound track
// (silence when there is no input); connecting happens on its thread.
//
// As a replay buffer (startBuffer) it keeps the last seconds of encoded
// pictures and sound in memory instead, and saveReplay() writes them to a
// file whenever asked (from the first keyframe in them), while it carries on.

#include <QImage>
#include <QSize>
#include <QString>
#include <memory>

class LiveRecorder {
public:
    LiveRecorder();
    ~LiveRecorder(); // finishes the file
    LiveRecorder(const LiveRecorder&) = delete;
    LiveRecorder& operator=(const LiveRecorder&) = delete;

    // Opens `path` for `size` (rounded to even) pictures at `fps`; with
    // `withAudio` also a stereo 48 kHz sound track fed by addAudio().
    // The recording starts at startTime() (liveClock() when it was opened).
    bool start(const QString& path, QSize size, double fps, QString* error, bool withAudio = false);
    // Stream: rtmp:// / rtmps:// (FLV) or srt:// / udp:// (MPEG-TS) at `kbps` for the picture.
    bool startStream(const QString& url, QSize size, double fps, int kbps, QString* error, bool withAudio = false);
    // Streams: connected to the server yet; what went wrong (the stream then stops: isRecording() false).
    bool isConnected() const;
    QString streamError() const;
    // Replay buffer: keeps the last `seconds` in memory (no file).
    bool startBuffer(QSize size, double fps, double seconds, QString* error, bool withAudio = false);
    // Writes the buffer to `path` (any thread; the buffer carries on). `seconds`: how much.
    bool saveReplay(const QString& path, QString* error = nullptr, double* seconds = nullptr);
    double bufferedSeconds() const;
    QSize size() const;
    // Queues the next frame (scaled to the recording size if needed), captured
    // at `time` (liveClock(); < 0 = now). Returns false if the encoder fell
    // behind and the frame was dropped.
    bool addFrame(const QImage& frame, double time = -1);
    // Queues interleaved stereo 48 kHz samples that started at `time` (liveClock()).
    void addAudio(const float* samples, int frames, double time);
    // Flushes the encoder and closes the file.
    bool finish(QString* error = nullptr);

    bool isRecording() const;
    bool hasAudio() const;
    double startTime() const;
    int64_t frames() const;       // written
    int64_t droppedFrames() const;
    double seconds() const;       // recorded duration
    QString encoderName() const;
    QImage thumbnail() const;     // a frame from early in the recording

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

// Folder the Live tab records into (Videos/Mutant Video Producer Live; created on demand)
// and a fresh file name in it ("Live 2026-10-04 18-30-12.mp4").
QString liveRecordingsFolder();
QString newLiveRecordingPath(const QString& prefix = QStringLiteral("Live"), const QString& extension = QStringLiteral("mp4"));
// Cached thumbnail of a recording (written next to the app's cache).
QString liveThumbnailPath(const QString& recording);
