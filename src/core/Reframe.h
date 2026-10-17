#pragma once
// Auto Reframe: a wide picture cropped for a tall frame (16:9 to 9:16 for Shorts, Reels and TikTok),
// the crop following the subject.

#include "core/Project.h"

#include <QImage>
#include <functional>
#include <vector>

namespace reframe {
struct Sample {
    TimeUs t = 0;    // source time
    double x = 0.5;  // the subject's centre across the picture, 0..1
    double weight = 0; // how sure: 0 (nothing found) .. 1
};

// Where the subject is across `frame`: the person in `personMask` (0..255 per pixel, may be null),
// or else where the picture changed most since `previous` (same size). Weight 0: neither found.
Sample locate(const QImage& frame, const QImage& previous, const QImage& personMask);

// Samples every `step` of a video stream between `from` and `to` (source times), with the person
// model when it is available. `progress` gets 0..1 and returns false to cancel (empty result).
std::vector<Sample> follow(const MediaItem& media, int stream, TimeUs from, TimeUs to, TimeUs step,
                           const std::function<bool(double)>& progress = {}, QString* error = nullptr);

// The crop's centre over time: the samples smoothed (gaps hold the last place), moving only when the
// subject nears the crop's edge, no faster than about a picture width in 1.5 s, and kept inside the
// picture. `visible` is the crop's width as a fraction of the picture's (0..1). Times stay source times.
std::vector<Keyframe> path(const std::vector<Sample>& samples, double visible);

// The fraction of a `sourceAspect` (width / height) picture's width that a `frameAspect` frame shows
// in Fill mode.
double visibleWidth(double sourceAspect, double frameAspect);

// Gives each of `events` (video) a Fill Pan/Crop whose centre follows the subject for a `frame`-shaped output
// (a reversed or speed-ramped event: a still crop centred where the subject mostly is). The number reframed (0: the frame shows the whole picture
// already), or -1 when `progress` (0..1 over all of them) cancels.
int apply(Timeline& tl, const MediaMap& media, const std::vector<int>& events, QSize frame, const std::function<bool(double)>& progress = {});
} // namespace reframe
