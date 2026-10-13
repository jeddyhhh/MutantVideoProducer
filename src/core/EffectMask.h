#pragma once

// Effect masks: any video effect (or rack) can be limited to a rectangle, an
// ellipse or a freehand shape of the picture (rotated, with a soft edge, or everything outside it),
// e.g. to blur a face or colour one part. The mask's settings live with the
// effect's own ("mask.*", keyframable, and moved by Track Motion), and the CPU
// and GPU renderers blend the effect's result back through the same weights.

#include "core/Effects.h"

#include <QImage>
#include <QPainterPath>
#include <cmath>
#include <vector>

// "mask.shape" (off, rectangle, ellipse), "mask.x" / "mask.y" (centre, share of
// the width / height), "mask.w" / "mask.h" (size, share of the width / height),
// "mask.rotation" (degrees), "mask.feather" (soft edge, % of the height),
// "mask.invert"; freehand: "mask.points" (3..16), "mask.smooth" (curves
// through the points), "mask.animate" (dragging a point keys it), and each
// point "mask.p<i>x" / "mask.p<i>y" in the mask's own frame (-0.5..0.5 across
// its width / height, so moving, sizing, turning and tracking still apply).
// A point without a value sits on a circle (the shape starts round).
const std::vector<ParamDef>& effectMaskParams();

struct EffectMask {
    int shape = 0; // 0 off, 1 rectangle, 2 ellipse, 3 freehand
    float cx = 0, cy = 0, hw = 0, hh = 0; // centre and half size, pixels
    float cs = 1, sn = 0;                 // cos / sin of the rotation
    float feather = 0;                    // pixels
    bool invert = false;
    bool animate = false;        // freehand: dragging a point keys it
    std::vector<QPointF> points; // freehand: the points, pixels
    QPainterPath outline;        // freehand: the shape, pixels
    QImage alpha;                // freehand: coverage (alpha), feathered, layer-sized
    bool active() const { return shape != 0; }
};
// Freehand point i of n in the mask's own frame (its value, or on a circle).
QPointF maskPoint(const ParamMap& params, int i, int n, TimeUs t);
// The mask of an effect's settings at time t, for a layer of `size`.
EffectMask effectMask(const ParamMap& params, TimeUs t, QSize size);

// The effect's weight 0..1 at a pixel centre (the GPU's EffectMask pass computes the same).
inline float maskWeight(const EffectMask& m, float px, float py)
{
    const float dx = px - m.cx, dy = py - m.cy;
    const float qx = dx * m.cs + dy * m.sn, qy = -dx * m.sn + dy * m.cs;
    float e;
    if (m.shape == 3) {
        const int x = std::clamp(int(px), 0, m.alpha.width() - 1), y = std::clamp(int(py), 0, m.alpha.height() - 1);
        const float k = float(m.alpha.constScanLine(y)[x * 4 + 3]) / 255.0f; // (alpha of ARGB32 premultiplied, little-endian)
        return m.invert ? 1.0f - k : k;
    }
    if (m.shape == 1) {
        e = std::min(m.hw - std::abs(qx), m.hh - std::abs(qy));
    } else {
        const float ax = qx / std::max(m.hw, 1e-3f), ay = qy / std::max(m.hh, 1e-3f);
        e = (1.0f - std::sqrt(ax * ax + ay * ay)) * std::min(m.hw, m.hh);
    }
    float k = m.feather > 0 ? std::clamp(e / m.feather + 0.5f, 0.0f, 1.0f) : (e >= 0 ? 1.0f : 0.0f);
    return m.invert ? 1.0f - k : k;
}
// Blends `out` (the effect's result) back towards `before` outside the mask.
void applyEffectMask(QImage& out, const QImage& before, const EffectMask& m);
