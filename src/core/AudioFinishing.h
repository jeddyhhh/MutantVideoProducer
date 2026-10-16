#pragma once
#include "core/Project.h"
#include <QStringList>
#include <functional>

namespace finishing {
struct Target {
    QString name = "Online video / speech";
    double lufs = -16, truePeak = -1.5;
};
std::vector<Target> targets();
struct Report {
    QString error;
    bool cancelled = false, audible = false;
    double seconds = 0, integrated = -70, range = 0, truePeak = -120, samplePeak = -120;
    double momentaryMax = -120, shortTermMax = -120, correlation = 0;
    double rmsLeft = -120, rmsRight = -120, silenceSeconds = 0;
    qint64 clippedSamples = 0, invalidSamples = 0;
    QStringList warnings;
    QJsonObject json(const Target& target) const;
};
// Receives post-master stereo floats, or an injected source for deterministic tests.
using Source = std::function<void(int64_t, int, float*)>;
Report analyze(int rate, int64_t frames, const Source& source,
               const std::function<bool(double)>& progress = {});
Report analyze(const ProjectSnapshot& snapshot, const std::function<bool(double)>& progress = {});
QStringList warnings(const Report& report, const Target& target);
// Linear adjustment preserves dynamics and is limited by measured true-peak headroom.
double safeGain(const Report& report, const Target& target);
bool applyGain(Timeline& timeline, const Report& report, const Target& target, QString* error);
// A track's events as a YouTube description list: "01. Name - 04:19" ("1:04:19" from an hour on).
// Names are the event's own name, or its file name without the extension and a leading track number.
QString trackListing(const Project& project, int track);
}
