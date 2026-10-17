#pragma once
// Finding the cuts between shots in a video, for Split at Scene Changes.

#include "core/Project.h"

#include <functional>
#include <optional>
#include <set>
#include <vector>

namespace scenes {
// Source times (in µs, each the first frame of a new shot) of the cuts between `from` and `to` in
// one video stream. `sensitivity` 0..1: higher finds subtler cuts. Shots are at least half a second.
// `progress` gets 0..1 and returns false to cancel (the result is then empty).
std::vector<TimeUs> detect(const MediaItem& media, int stream, TimeUs from, TimeUs to, double sensitivity,
                           const std::function<bool(double)>& progress = {}, QString* error = nullptr);

// The video events among `ids` whose picture plays forwards at a steady speed, one per group;
// `skipped` counts the reversed and speed-ramped ones.
std::vector<int> videoEvents(const Timeline& tl, const MediaMap& media, const std::set<int>& ids, int* skipped = nullptr);

// Splits each of `events` (with what is grouped with it) at its scene changes. The timeline times cut,
// or nothing when `progress` (0..1 over all of them) cancels.
std::optional<std::vector<TimeUs>> splitEvents(Timeline& tl, const MediaMap& media, const std::vector<int>& events, double sensitivity,
                                               const FrameRate& fps, const std::function<bool(double)>& progress = {});
} // namespace scenes
