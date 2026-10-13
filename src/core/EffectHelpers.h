#pragma once

// Shared building blocks for the built-in video effects: parameter
// definitions, hashing, pixel access, bilinear sampling, warping and the
// frame history used by time-based effects.

#include "core/Effects.h"
#include "core/Parallel.h"

#include <QImage>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace fx {

// ---- Parameter definitions ----------------------------------------------------

inline ParamDef num(const char* id, const char* name, double min, double max, double def, int decimals = 2, const char* suffix = "")
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromUtf8(name);
    p.type = ParamType::Number;
    p.min = min;
    p.max = max;
    p.def = def;
    p.decimals = decimals;
    p.suffix = QString::fromUtf8(suffix);
    return p;
}

inline ParamDef choice(const char* id, const char* name, QStringList choices, int def = 0)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromUtf8(name);
    p.type = ParamType::Choice;
    p.choices = std::move(choices);
    p.max = double(p.choices.size() - 1);
    p.def = def;
    return p;
}

inline ParamDef flag(const char* id, const char* name, bool def)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromUtf8(name);
    p.type = ParamType::Bool;
    p.max = 1;
    p.def = def ? 1 : 0;
    return p;
}

inline ParamDef color(const char* id, const char* name, uint32_t rgb)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromUtf8(name);
    p.type = ParamType::Color;
    p.max = 0xffffff;
    p.def = rgb;
    return p;
}

// ---- Random numbers (same function as the GPU shaders' hash32) ----------------

inline uint32_t hash32(uint32_t x, uint32_t y, uint32_t seed)
{
    uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float hash01(uint32_t x, uint32_t y, uint32_t seed)
{
    return float(hash32(x, y, seed) & 0xffffffu) / float(0x1000000);
}
// Frame number of the render (for animated randomness).
inline uint32_t frameIndex(const RenderContext& ctx)
{
    return uint32_t(std::llround(double(ctx.timelineTime) * ctx.fps / double(kUsPerSecond)));
}
inline double seconds(const RenderContext& ctx)
{
    return double(ctx.timelineTime) / double(kUsPerSecond);
}
// Smooth 1D value noise in -1..1.
inline float noise1(double t, uint32_t seed)
{
    const double i = std::floor(t);
    const float f = float(t - i), u = f * f * (3.0f - 2.0f * f);
    const float a = hash01(uint32_t(int64_t(i)), 0, seed) * 2 - 1, b = hash01(uint32_t(int64_t(i) + 1), 0, seed) * 2 - 1;
    return a + (b - a) * u;
}

// ---- Pixels (premultiplied ARGB32) ----------------------------------------------

inline int pa(uint32_t p) { return int(p >> 24); }
inline int pr(uint32_t p) { return int((p >> 16) & 0xff); }
inline int pg(uint32_t p) { return int((p >> 8) & 0xff); }
inline int pb(uint32_t p) { return int(p & 0xff); }
inline uint32_t pack(int a, int r, int g, int b)
{
    auto c = [](int v) { return uint32_t(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    const uint32_t A = c(a);
    auto cc = [&](int v) { return std::min(c(v), A); }; // keep premultiplied colour <= alpha
    return (A << 24) | (cc(r) << 16) | (cc(g) << 8) | cc(b);
}
inline uint32_t packf(float a, float r, float g, float b)
{
    return pack(int(a + 0.5f), int(r + 0.5f), int(g + 0.5f), int(b + 0.5f));
}
// Rec. 601 luma 0..1 of premultiplied colour (transparent counts as black).
inline float luma(uint32_t p)
{
    return (0.299f * float(pr(p)) + 0.587f * float(pg(p)) + 0.114f * float(pb(p))) / 255.0f;
}

enum class Edge { Clamp, Wrap, Mirror, Transparent };

inline int edgeIndex(int i, int n, Edge e)
{
    switch (e) {
    case Edge::Wrap: return ((i % n) + n) % n;
    case Edge::Mirror: {
        const int m = ((i % (2 * n)) + 2 * n) % (2 * n);
        return m < n ? m : 2 * n - 1 - m;
    }
    default: return i < 0 ? 0 : (i >= n ? n - 1 : i);
    }
}

// Read-only view for fast sampling.
struct Pixels {
    const uchar* bits = nullptr;
    qsizetype bpl = 0;
    int w = 0, h = 0;
    explicit Pixels(const QImage& img) : bits(img.constBits()), bpl(img.bytesPerLine()), w(img.width()), h(img.height()) {}
    uint32_t at(int x, int y) const { return reinterpret_cast<const uint32_t*>(bits + bpl * y)[x]; }
    uint32_t fetch(int x, int y, Edge e) const
    {
        if (e == Edge::Transparent && (x < 0 || y < 0 || x >= w || y >= h))
            return 0;
        return at(edgeIndex(x, w, e), edgeIndex(y, h, e));
    }
    // Bilinear sample at pixel coordinates (pixel centres at +0.5, like GL).
    uint32_t sample(float x, float y, Edge e) const
    {
        x -= 0.5f;
        y -= 0.5f;
        const float fx0 = std::floor(x), fy0 = std::floor(y);
        const int x0 = int(fx0), y0 = int(fy0);
        const uint32_t wx = uint32_t((x - fx0) * 256.0f + 0.5f), wy = uint32_t((y - fy0) * 256.0f + 0.5f);
        uint32_t p00, p10, p01, p11;
        if (x0 >= 0 && y0 >= 0 && x0 + 1 < w && y0 + 1 < h) { // inside: no edge handling
            const auto* r0 = reinterpret_cast<const uint32_t*>(bits + bpl * y0) + x0;
            const auto* r1 = reinterpret_cast<const uint32_t*>(bits + bpl * (y0 + 1)) + x0;
            p00 = r0[0], p10 = r0[1], p01 = r1[0], p11 = r1[1];
        } else {
            p00 = fetch(x0, y0, e), p10 = fetch(x0 + 1, y0, e), p01 = fetch(x0, y0 + 1, e), p11 = fetch(x0 + 1, y0 + 1, e);
        }
        auto lerp2 = [](uint32_t a, uint32_t b, uint32_t t) {
            const uint32_t rb = (((a & 0x00ff00ffu) * (256 - t) + (b & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
            const uint32_t ag = ((((a >> 8) & 0x00ff00ffu) * (256 - t) + ((b >> 8) & 0x00ff00ffu) * t)) & 0xff00ff00u;
            return rb | ag;
        };
        return lerp2(lerp2(p00, p10, wx), lerp2(p01, p11, wx), wy);
    }
};

// Writes every pixel from map(x, y, sx, sy) -> sampled source position
// (centre coordinates); map returns false for a transparent pixel.
template <class Map>
void warp(QImage& img, Edge edge, Map map)
{
    const QImage src = img;
    uchar* bits = img.bits(); // detaches once, before the threads start
    const qsizetype bpl = img.bytesPerLine();
    const Pixels in(src);
    const int w = img.width();
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
            for (int x = 0; x < w; ++x) {
                float sx, sy;
                row[x] = map(float(x) + 0.5f, float(y) + 0.5f, sx, sy) ? in.sample(sx, sy, edge) : 0u;
            }
        }
    });
}

// Runs fn(y, row) over all rows in parallel (row writable).
template <class Fn>
void forRows(QImage& img, Fn fn)
{
    uchar* bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            fn(y, reinterpret_cast<uint32_t*>(bits + bpl * y));
    });
}

// ---- Time-based effects ---------------------------------------------------------------

// Recent input frames (newest first) and the previous output, for effects
// that look back in time. A jump (seek, scrub) starts over.
struct FrameHistory : EffectState {
    enum class Step { Reset, Next, Same };
    std::vector<QImage> frames;
    QImage prevOut; // output of the previous frame
    QImage out;     // output of this frame (set by the effect)
    TimeUs last = std::numeric_limits<TimeUs>::min();

    Step push(const RenderContext& ctx, const QImage& img, size_t keep)
    {
        const double frame = double(kUsPerSecond) / std::max(1.0, ctx.fps);
        const bool sameSize = !frames.empty() && frames.front().size() == img.size();
        Step s = Step::Reset;
        if (sameSize && ctx.timelineTime == last)
            s = Step::Same; // re-render of the same frame (preview refresh)
        else if (sameSize && std::abs(double(ctx.timelineTime - last) - frame) < frame * 0.5)
            s = Step::Next;
        if (s == Step::Reset) {
            frames.clear();
            prevOut = QImage();
            out = QImage();
        }
        if (s == Step::Same) {
            frames.front() = img;
        } else {
            if (s == Step::Next)
                prevOut = out;
            frames.insert(frames.begin(), img);
            if (frames.size() > std::max<size_t>(1, keep))
                frames.resize(std::max<size_t>(1, keep));
        }
        last = ctx.timelineTime;
        return s;
    }
    const QImage& ago(size_t n) const { return frames[std::min(n, frames.size() - 1)]; }
};

template <class T>
T& stateOf(std::unique_ptr<EffectState>& state)
{
    if (!state || !dynamic_cast<T*>(state.get()))
        state = std::make_unique<T>();
    return *static_cast<T*>(state.get());
}

// ---- Mix and mask shared by the glitch effects (GlitchEffects.cpp) -----------------

// Parameters "fxMix", "fxMask" (everything, inside / outside a box, bright /
// dark areas), "fxThreshold", "fxFeather", "fxBoxX/Y/W/H".
std::vector<ParamDef> glitchCommonParams();
struct PostMask {
    float mix = 1;
    int mode = 0;                        // 0 everything, 1 inside box, 2 outside, 3 bright, 4 dark
    float threshold = 0.5f, feather = 0; // luma units, or pixels for the box
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // box, pixels
    bool active() const { return mix < 1.0f || mode != 0; }
};
PostMask postMask(const Params& p, QSize size);
// Effect weight 0..1 at a pixel centre (px, py) whose input luma is L
// (the GPU's GlitchPost pass computes the same).
inline float postWeight(const PostMask& m, float px, float py, float L)
{
    float k = 1.0f;
    if (m.mode == 1 || m.mode == 2) {
        const float e = std::min(std::min(px - m.x0, m.x1 - px), std::min(py - m.y0, m.y1 - py));
        k = m.feather > 0 ? std::clamp(e / m.feather + 0.5f, 0.0f, 1.0f) : (e >= 0 ? 1.0f : 0.0f);
        if (m.mode == 2)
            k = 1.0f - k;
    } else if (m.mode >= 3) {
        k = m.feather > 0 ? std::clamp((L - m.threshold) / m.feather + 0.5f, 0.0f, 1.0f) : (L >= m.threshold ? 1.0f : 0.0f);
        if (m.mode == 4)
            k = 1.0f - k;
    }
    return k * m.mix;
}
// Blends `out` (the effect's result) back towards `before` through the mask.
void applyPostMask(QImage& out, const QImage& before, const PostMask& m);

// ---- Per-frame decisions shared by the CPU effects and the GPU renderer ----------

// Chromatic aberration amount in pixels, with the wobble applied.
float chromaAmount(const Params& p, const RenderContext& ctx);
// RGB Split channel offsets in pixels (red x, y, green x, y, blue x, y), jitter included.
std::array<float, 6> rgbSplitOffsets(const Params& p, const RenderContext& ctx);
// Pixel Sort random run breaks: seed for hash01(position, line, seed) < breaks.
uint32_t pixelSortBreakSeed(const Params& p, const RenderContext& ctx);
// Pixel Stretch line position (0..1) after the sweep, and the per-line jitter seed.
float stretchPosition(const Params& p, const RenderContext& ctx);
uint32_t stretchSeed(const Params& p, const RenderContext& ctx);
// Frame Rate Stutter: whether tick `tick` (one after another, or a jump) shows a
// new frame; random freezes hold the current one until `frozenUntil`.
bool stutterShowsNew(const Params& p, int64_t tick, int64_t lastTick, int64_t& frozenUntil);
// Strobe: whether it flashes now, and the flash colour (0xRRGGBB) for the colour modes.
struct StrobeNow {
    bool on = false;
    uint32_t rgb = 0xffffff;
};
StrobeNow strobeNow(const Params& p, const RenderContext& ctx);
// RGB Time Split: frames ago for the red, green and blue channel.
std::array<int, 3> timeSplitDelays(const Params& p);
// Datamosh automatic clean frame: changes every "autoClean" seconds (0 = never).
int64_t datamoshCleanTick(const Params& p, const RenderContext& ctx);
// Compression Artifacts DCT block size in pixels (8, 16 or 32 at full size).
int jpegBlockSize(const Params& p, const RenderContext& ctx);

// ---- Plans shared by the CPU effects and the GPU shaders (GlitchEffects.cpp) ----

struct RowPlan {
    float a = 0, b = 0, c = 0, d = 0;
};
uint32_t glitchRateSeed(const Params& p, const RenderContext& ctx);
std::vector<RowPlan> sliceRows(const Params& p, const RenderContext& ctx, int h);           // a shift, b torn (0/1), c colour mode + 1 (0 = none)
std::vector<RowPlan> vhsRows(const Params& p, const RenderContext& ctx, int w, int h);      // a offset, b head noise, c/d dropout start/length
std::vector<RowPlan> vhsTrackingRows(const Params& p, const RenderContext& ctx, int h);   // a band weight, b offset
std::vector<RowPlan> tvStaticRows(const Params& p, const RenderContext& ctx, int h);      // a bar gain, b lost (0/1)
struct FilmPlan {
    float dx = 0, dy = 0, flicker = 1;
    int scratchWidth = 1;
    std::vector<uint8_t> scratchDark;          // per visible scratch
    std::vector<float> scratchX;               // [row * scratches + scratch] left x
    std::vector<std::array<float, 4>> specks;  // x, y, radius, dark
};
FilmPlan oldFilmPlan(const Params& p, const RenderContext& ctx, int w, int h);
const int* jpegQuantTable(bool chroma); // 8x8 standard tables
std::array<uint32_t, 256> gradientMapLut(const Params& p, const RenderContext& ctx);
std::vector<QImage> asciiGlyphs(int cell, int charset); // Alpha8 cells, dark -> bright

} // namespace fx

// Registers the glitch / analog / psychedelic effects (GlitchEffects.cpp).
void addGlitchEffects(std::vector<EffectDef>& list);
