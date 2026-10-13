#pragma once

#include <QString>
#include <cstdint>

// All timeline and media positions are integer microseconds.
using TimeUs = int64_t;
constexpr TimeUs kUsPerSecond = 1'000'000;

struct FrameRate {
    int num = 30;
    int den = 1;

    bool valid() const { return num > 0 && den > 0; }
    double toDouble() const { return den ? double(num) / den : 0.0; }

    // Start time of frame `frame` (exact: frameAt(timeOf(n)) == n).
    TimeUs timeOf(int64_t frame) const;
    // Index of the frame being displayed at time t (floor).
    int64_t frameAt(TimeUs t) const;
    // Nearest frame boundary.
    TimeUs snap(TimeUs t) const;
    TimeUs frameDuration() const { return timeOf(1); }

    QString toString() const; // "29.970" / "25"

    bool operator==(const FrameRate&) const = default;
};

// "HH:MM:SS;FF" - wall clock time plus frame within the second.
QString formatTimecode(TimeUs t, const FrameRate& fps);
// Compact human duration, e.g. "1:23.45" or "12.50 s".
QString formatDuration(TimeUs t);

inline int64_t usToSamples(TimeUs t, int sampleRate) { return t * sampleRate / kUsPerSecond; }
inline TimeUs samplesToUs(int64_t s, int sampleRate) { return s * kUsPerSecond / sampleRate; }
