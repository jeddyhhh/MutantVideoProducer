#pragma once
// Cutting time out of a whole edit: silences between words, and words struck from a transcript.

#include "core/Captions.h"
#include "core/Project.h"

#include <vector>

namespace cuts {
struct Range {
    TimeUs from = 0, to = 0;
    bool operator==(const Range&) const = default;
};

// The quiet stretches of `mono` (at `rate` Hz, its first sample at timeline time `start`): below
// `thresholdDb` (RMS over 20 ms) for at least `minLength`, each shortened by `keep` at both ends so
// the words either side keep their breath.
std::vector<Range> silences(const std::vector<float>& mono, int rate, TimeUs start, double thresholdDb, TimeUs minLength, TimeUs keep);

// The time to take out for the struck words (`removed[i]` for `words[i]`, one caption per word in
// timeline time): each run of struck words from its first word's start to the next kept word's
// start, or to the run's last word's end when nothing follows.
std::vector<Range> wordCuts(const std::vector<Caption>& words, const std::vector<bool>& removed);

// Takes the ranges (snapped to frames, merged) out of every unlocked track, splitting the events at
// their edges, and closes each gap: what follows, and its markers, move back. Returns the time removed.
TimeUs removeRanges(Timeline& tl, std::vector<Range> ranges, const FrameRate& fps);

// The opposite: `length` of empty time at `at` (snapped to frames) on every unlocked track, splitting the events there;
// what follows, its markers and its story chapters move later, and a region or chapter running across `at` grows.
TimeUs insertTime(Timeline& tl, TimeUs at, TimeUs length, const FrameRate& fps);
} // namespace cuts
