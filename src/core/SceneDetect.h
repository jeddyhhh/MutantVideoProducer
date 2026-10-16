#pragma once
// Finding the cuts between shots in a video, for Split at Scene Changes.

#include "core/Project.h"

#include <functional>
#include <vector>

namespace scenes {
// Source times (in µs, each the first frame of a new shot) of the cuts between `from` and `to` in
// one video stream. `sensitivity` 0..1: higher finds subtler cuts. Shots are at least half a second.
// `progress` gets 0..1 and returns false to cancel (the result is then empty).
std::vector<TimeUs> detect(const MediaItem& media, int stream, TimeUs from, TimeUs to, double sensitivity,
                           const std::function<bool(double)>& progress = {}, QString* error = nullptr);
} // namespace scenes
