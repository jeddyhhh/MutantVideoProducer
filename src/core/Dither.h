#pragma once

// Dither / Retro Palette: the palettes, the dithering methods and the CPU
// renderer. The GPU renderer (GpuCompositor) uses the same tables, so both
// draw the same picture (error diffusion exactly: it works in integers).

#include "core/Effects.h"

#include <QImage>
#include <QStringList>
#include <cstdint>
#include <vector>

namespace dither {

struct Palette {
    const char* name;
    std::vector<uint32_t> colors; // 0xRRGGBB, at most 64
    int levels = 0;   // > 0 instead: every mix of this many even steps per channel (worked out, not searched)
    int adaptive = 0; // > 0 instead: this many colours picked from the picture each frame (median cut)
};
// Saved projects keep a palette by its place in this list: new ones go at the end.
const std::vector<Palette>& palettes();
QStringList paletteNames();

enum class Kind { None, Map, WhiteNoise, GradientNoise, Pattern, Diffusion, Riemersma };

struct Tap {
    int dx, dy, weight; // where the error goes (dy > 0, or dy == 0 and dx > 0) and its share
};

struct Method {
    const char* name;
    Kind kind;
    int map = -1;          // Map, Pattern: a threshold map (thresholdMap)
    std::vector<Tap> taps; // Diffusion: the kernel
    int divisor = 1;       // Diffusion: the shares are weight / divisor
};
// Saved projects keep a method by its place in this list: new ones go at the end.
const std::vector<Method>& methods();
QStringList methodNames();

// An n x n threshold map, values in [0, 1) (each (rank + 0.5) / n²).
struct ThresholdMap {
    int n = 0;
    std::vector<float> values;
};
const ThresholdMap& thresholdMap(int id);

// Error diffusion goes through the picture in diagonal waves t = x + slope * y:
// every tap then comes from an earlier wave, `lag` waves back.
int waveSlope(const Method& m);
inline int tapLag(const Tap& t, int slope) { return t.dx + slope * t.dy; }

// Up to `count` colours for the picture's cells (un-premultiplied RGB, alpha
// > 0 counted), by median cut; sorted dark to light.
std::vector<uint32_t> adaptivePalette(const std::vector<float>& rgb, const std::vector<uint8_t>& alpha, int count);

// The integer hash behind White noise (the same on the GPU).
inline uint32_t noiseHash(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return h;
}

// Shift of the noise patterns for frame `frame` (Animate noise), the same on the GPU.
inline void noiseOffset(uint32_t frame, int& ox, int& oy)
{
    ox = int(noiseHash(frame, 17, 3) % 4096u);
    oy = int(noiseHash(frame, 29, 5) % 4096u);
}

// The effect on the CPU.
void apply(QImage& img, const Params& p, const RenderContext& ctx);

} // namespace dither
