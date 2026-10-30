#pragma once

#include <QImage>
#include <array>
#include <cstdint>
#include <functional>

// Pixel kernels for effects and transitions. All images are
// QImage::Format_ARGB32_Premultiplied; work is spread across cores.
namespace imageops {

using Lut = std::array<uint8_t, 256>;

// Applies per-channel lookup tables to unpremultiplied colour.
void applyLuts(QImage& img, const Lut& r, const Lut& g, const Lut& b);

// out = M * (r,g,b,1) on unpremultiplied colour in 0..1; M is 3x4 row-major.
void applyColorMatrix(QImage& img, const float m[12]);

// Separable box blur, radii in pixels; `passes` = 3 approximates a Gaussian.
void boxBlur(QImage& img, int radiusX, int radiusY, int passes = 3);
void gaussianBlur(QImage& img, double sigmaX, double sigmaY);

// Generic per-pixel helper on unpremultiplied colour (alpha preserved).
void forEachPixel(QImage& img, const std::function<void(int y, uint32_t* row, int width)>& rowFn);

// Blends `from` -> `to` with a per-pixel mask m(x, y) in 0..1 (1 = `to`).
// Null inputs count as transparent.
void maskBlend(QImage& out, const QImage* from, const QImage* to, const std::function<float(int x, int y)>& mask);

// Linear mix of two premultiplied images (either may be null).
void mix(QImage& out, const QImage* from, const QImage* to, float t);

// Composites `layer` over `base` (same size) with a blend mode (BlendMode
// order: Normal, Add, Screen, Multiply, Overlay, Lighten, Darken, Difference)
// and opacity, W3C compositing style. Matches the GPU renderer's shader.
void blendLayer(QImage& base, const QImage& layer, float opacity, int mode);
// A track matte (TrackMatte mode) on a premultiplied layer: kept where the matte is drawn (alpha) or bright (luma).
void applyMatte(QImage& layer, const QImage& matte, int mode);

inline uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }
inline float smoothstep(float e0, float e1, float x)
{
    if (e1 <= e0)
        return x < e0 ? 0.0f : 1.0f;
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t * t * (3.0f - 2.0f * t);
}
// Wipe-style mask: 0 before the moving edge, 1 after, soft edge of width `soft`.
inline float wipeMask(float s, float p, float soft)
{
    if (soft <= 0.0001f)
        return s < p ? 1.0f : 0.0f;
    float m = (p * (1.0f + soft) - s) / soft;
    return m < 0 ? 0.0f : (m > 1 ? 1.0f : m);
}

} // namespace imageops
