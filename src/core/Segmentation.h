#pragma once

// Finding the person in a picture, run with ONNX Runtime (on the GPU through
// DirectML when there is one) when the build has it: the Remove Background
// effect. Fast is MediaPipe selfie segmentation (a mask), Quality is
// RobustVideoMatting (a matte that follows hair, with a memory of the frames
// before for steady video).

#include "core/Effects.h"

#include <QImage>

enum class SegmentModel { Fast, Quality };

// Whether the model loaded (false: no ONNX Runtime in this build, or no model file).
bool segmentationAvailable(SegmentModel which = SegmentModel::Fast, QString* error = nullptr);
// Where the models run: "DirectML (GPU)" or "CPU" (empty: not loaded).
QString segmentationDevice();
// How sure the Fast model is that each pixel is a person, 0..255, as a
// Grayscale8 image the size of `picture`. Null on failure.
QImage personMask(const QImage& picture, QString* error = nullptr);
// The steps of the Remove Background effect (both renderers): the size the
// model takes the picture at, the mask (or matte) of a picture at that size
// (the model's memory and Steady's last mask kept in `state`), and Steady:
// the mask blended with the last one.
QSize segmentationInputSize(SegmentModel which, QSize picture);
QImage personMaskSmall(const QImage& picture, SegmentModel which, std::unique_ptr<EffectState>& state, QString* error = nullptr);
void steadyMask(QImage& mask, std::unique_ptr<EffectState>& state);

// Registers Remove Background (VideoEffects.cpp).
void addSegmentationEffects(std::vector<EffectDef>& list);
