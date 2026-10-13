#pragma once

#include "core/Effects.h"

#include <QImage>

// The per-pixel transitions (Glitch, Ripple, Cube Spin, Film Burn...). Each
// is written once as a kernel: the CPU renderer runs the C++ version and the
// GPU renderer the GLSL copy (kGlsl), fed the same uniforms by setup().
namespace tfx {

struct Uniforms {
    int mode = 0;
    float p = 0;
    float w = 0, h = 0;
    float a[4] = {}, b[4] = {};
    float color[4] = {}, color2[4] = {};
};

// The kernel transitions' definitions (appended to transitions()).
void addTransitions(std::vector<TransitionDef>& list);

// Fills `u` for transition `id` at progress p; false if `id` is not a kernel transition.
bool setup(const QString& id, const Params& prm, double p, QSize size, double scale, Uniforms& u);

// CPU: writes the blend of `from` -> `to` (either may be null) into `out`.
void render(QImage& out, const QImage* from, const QImage* to, const Uniforms& u);

// GPU: fragment shader body (after the renderer's common header) with
// uniforms uFrom, uTo, uMode, uP, uSize, uA, uB, uColor, uColor2.
extern const char* const kGlsl;

} // namespace tfx
