#pragma once

#include "core/Time.h"

#include <QSize>
#include <QString>
#include <QStringList>
#include <vector>

struct VideoStreamInfo {
    int index = -1;          // stream index inside the container
    QString codecName;
    QString pixelFormat;
    int width = 0;
    int height = 0;
    int sarNum = 1;          // sample (pixel) aspect ratio
    int sarDen = 1;
    FrameRate frameRate;
    int rotation = 0;        // clockwise display rotation in degrees
    bool hasAlpha = false;

    // Size as displayed: SAR applied and rotation taken into account.
    QSize displaySize() const;
};

struct AudioStreamInfo {
    int index = -1;
    QString codecName;
    int sampleRate = 0;
    int channels = 0;
};

struct MediaInfo {
    QString path;
    QString formatName;
    QString formatLongName;
    TimeUs duration = 0;
    int64_t bitRate = 0;
    bool isStill = false;    // single image: can be stretched to any length
    std::vector<VideoStreamInfo> video;
    std::vector<AudioStreamInfo> audio;
    QStringList warnings;    // streams we skipped and why
    QString error;           // non-empty when the file can't be used at all

    bool ok() const { return error.isEmpty() && (!video.empty() || !audio.empty()); }
    bool hasVideo() const { return !video.empty(); }
    bool hasAudio() const { return !audio.empty(); }
    const VideoStreamInfo* videoStream(int index) const;
    const AudioStreamInfo* audioStream(int index) const;
    QString summary() const;  // multi-line, human readable
};

constexpr TimeUs kDefaultStillDuration = 5 * kUsPerSecond;

// Opens the file, inspects every stream and reports what we can decode.
MediaInfo probeMedia(const QString& path);
