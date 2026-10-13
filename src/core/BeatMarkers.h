#pragma once

// The beat tracker on the editor's timeline: finds the beats of the music in a
// stretch of the timeline (the mix, or just some events), for markers that
// cuts snap to, splitting events on them, and cutting a montage to the music.

#include "core/BeatTracker.h"
#include "core/Project.h"

#include <functional>
#include <memory>
#include <set>
#include <vector>

struct BeatScan {
    std::vector<TimeUs> beats; // timeline times, steady from the first beat in the stretch
    double bpm = 0;
    double confidence = 0;
};

// Mixes the timeline's sound from `from` to `to` (only the audio events in
// `onlyClips` if it isn't empty) and finds its beats. `progress` gets 0..1 and
// can stop it (return false). It stops at the end of the last audio event, and
// scans at most 4 hours.
BeatScan scanBeats(const ProjectSnapshot& snap, TimeUs from, TimeUs to, const std::set<int>& onlyClips, const BeatSettings& settings,
                   const std::function<bool(double)>& progress = {});

// Markers on every `every`-th beat (1 = each beat, 4 = each bar...), the first
// beat counting as the 1; bars are named by number. With `replace`, markers
// already in that stretch go first. Returns how many were added.
int addBeatMarkers(Timeline& tl, const std::vector<TimeUs>& beats, int every, bool replace, TimeUs from, TimeUs to);

namespace ops {
// A montage cut to `cuts` (times, sorted): a new video track on top with an event
// from each stretch between two cuts, taken from `media` in turn (or shuffled),
// each from a random part of its file. Returns the new events.
std::vector<int> beatMontage(Timeline& tl, const std::vector<std::shared_ptr<const MediaItem>>& media, const std::vector<TimeUs>& cuts,
                             bool shuffle, uint32_t seed);
// Splits `ids` at every marker inside them; returns the new pieces.
std::vector<int> splitAtMarkers(Timeline& tl, const std::set<int>& ids);
} // namespace ops
