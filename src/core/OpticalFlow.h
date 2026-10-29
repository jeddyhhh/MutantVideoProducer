#pragma once

// Smooth slow motion: frames that were never filmed, made between two that were
// by following how things move between them (optical flow), instead of fading
// one into the other (frame blending). The motion is found on small grey copies
// of the two frames (a pyramid of block matches, coarse to fine, then smoothed);
// each in-between frame moves the first frame forward and the second back along
// it, and mixes them.

#include <QImage>
#include <vector>

namespace flow {

// The motion from frame a to frame b, one vector a cell, in fractions of the picture (x of its width, y of its height).
struct Field {
    int w = 0, h = 0;
    std::vector<float> dx, dy;
    bool empty() const { return w == 0; }
};

// The size the two frames are analysed at (at most 320 wide).
QSize analysisSize(QSize picture);
// The motion from a to b (the same size, any format).
Field estimate(const QImage& a, const QImage& b);
// The frame a fraction `w` (0..1) of the way from a to b.
QImage interpolate(const QImage& a, const QImage& b, const Field& f, float w);

} // namespace flow
