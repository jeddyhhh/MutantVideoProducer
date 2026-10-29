#pragma once

// Lines up recordings of the same moment by their sound: a second camera, or a
// separate audio recorder, against the first. Each event's sound becomes an
// onset envelope (1000 a second: where it gets louder), and the cross-
// correlation of two envelopes (by FFT) gives how far one is from the other.

#include "core/Project.h"

#include <QString>
#include <map>
#include <vector>

namespace audiosync {

constexpr int kRate = 1000; // envelope values a second

// The sound of [sourceIn, sourceIn + length) of a file's audio stream as an onset envelope.
std::vector<float> envelope(const QString& path, int stream, TimeUs sourceIn, TimeUs length);

struct Match {
    double lag = 0;        // seconds: b's envelope matches a's this much later in b (positive) or earlier
    double score = 0;      // normalised correlation at the peak, 0..1
    bool ok = false;       // a clear peak (score, and the peak stands out from the rest)
};
// Where b's sound lines up with a's (envelopes at kRate).
Match correlate(const std::vector<float>& a, const std::vector<float>& b);

struct Result {
    std::map<int, TimeUs> moved;   // event id -> how far it (and its group) moved
    std::vector<int> unmatched;    // events whose sound couldn't be lined up
    QString error;
};
// Moves each selected event (with its group) so its sound lines up with the
// reference event's. The reference is the selected event on the topmost track
// (the earliest, on a tie); every event is matched through the sound of its
// own media, or a grouped event's when it has none. Events at a speed other
// than 1 are left where they are.
Result syncByAudio(Timeline& tl, const MediaMap& media, const std::vector<int>& clipIds);

} // namespace audiosync
