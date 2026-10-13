#pragma once

// Stabilisation: an event's shaky footage held steady. A pass over the video
// (once per file and stream, cached on disk) measures how the picture moves
// from frame to frame: textured blocks are matched between frames and a shift
// and rotation fitted to them, leaving out blocks that move on their own. At
// render time the camera's path is compared with a smoothed version of it, and
// the difference is the correction (part of the event's framing, so the CPU and
// GPU renderers and exports all get it), with a zoom to hide the moving edges.

#include "core/Time.h"

#include <QSizeF>
#include <QString>
#include <QTransform>
#include <functional>

struct VideoStreamInfo;

namespace stabilize {

// Runs the analysis (unless it is cached). `progress` gets 0..1 and can stop it.
bool analyze(const QString& path, int stream, const std::function<bool(double)>& progress, QString* error = nullptr);
bool isAnalyzed(const QString& path, int stream);

// The correction for the frame shown at source time `srcT`, in source display
// coordinates (the space Pan/Crop maps to the canvas): the path smoothed over
// `smoothing` seconds; with `zoom`, scaled up just enough to hide the edges over
// the event's span [spanFrom, spanTo] of the source. Identity when not analysed.
QTransform correction(const QString& path, const VideoStreamInfo& vs, TimeUs srcT, double smoothing, bool zoom, TimeUs spanFrom,
                      TimeUs spanTo);

// Frame-to-frame motion as measured (tests): shift in analysis pixels, rotation in radians.
struct Motion {
    std::vector<TimeUs> pts;
    std::vector<float> dx, dy, da;
    int width = 0, height = 0; // analysis size
};
Motion motion(const QString& path, int stream);

} // namespace stabilize
