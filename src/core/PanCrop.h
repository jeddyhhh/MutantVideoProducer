#pragma once

#include "core/Effects.h"

#include <QSizeF>
#include <QTransform>

// Event Pan/Crop: how an event's picture is framed in the output.
// Parameters (all keyframable except the mode, times event-local):
//   mode     0 Fit (letterbox), 1 Fill (crop to fill), 2 Stretch
//   centerX/centerY  point of the source (0..1) shown at the frame centre
//   zoom     1 = whole picture as fitted, 2 = twice as close
//   rotation degrees clockwise, flipH/flipV
const std::vector<ParamDef>& panCropParams();

// True when the map leaves the picture exactly as the default fit.
bool isDefaultPanCrop(const ParamMap& params);

// Maps source display coordinates (0..sourceSize, after SAR and rotation
// metadata) to output canvas pixels.
QTransform panCropTransform(const Params& p, QSizeF sourceSize, QSizeF canvas);
