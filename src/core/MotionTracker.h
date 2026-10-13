#pragma once

// Motion tracking: follows what is inside a box of a video from frame to frame
// (a face, a sign, a car), for a mask that moves with it. The box's picture is
// looked for near where it was (and where it was heading), compared after
// taking out brightness changes, to a fraction of a pixel; the picture looked
// for is updated a little each frame so slow changes of shape don't lose it.

#include "core/Time.h"

#include <QPointF>
#include <QRectF>
#include <QString>
#include <functional>
#include <vector>

namespace tracker {

struct Point {
    TimeUs srcT = 0;  // source time of the frame
    QPointF centre;   // the box's centre, normalised (0..1) in the stored frame (before rotation metadata)
    float match = 1;  // 0..1: how well it matched (low: lost)
};

// Tracks `box` (normalised, stored frame) from source time `from` to `to`.
// `progress` gets 0..1 and can stop it (what was tracked so far is kept).
std::vector<Point> track(const QString& path, int stream, TimeUs from, TimeUs to, QRectF box, const std::function<bool(double)>& progress = {},
                         QString* error = nullptr);

// Normalised positions between the stored frame and the frame as displayed
// (rotation metadata: 0, 90, 180, 270 degrees clockwise).
QPointF storedToDisplay(QPointF p, int rotation);
QPointF displayToStored(QPointF p, int rotation);

} // namespace tracker
