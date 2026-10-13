// Glitch, fake-analog and psychedelic video effects. All work in place on a
// premultiplied ARGB32 layer the size of the output canvas, with spatial
// parameters in project pixels (scaled by RenderContext::scale). Animated
// randomness is seeded from the frame number so renders are repeatable, and
// time-based effects (trails, datamosh, slit-scan...) keep recent frames in
// a FrameHistory that starts over after a seek.

#include "core/Dither.h"
#include "core/EffectHelpers.h"
#include "core/ImageOps.h"

#include <QFont>
#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>

using namespace fx;

namespace {

constexpr float kPi = 3.14159265358979f;

QString S(const char* s) { return QString::fromLatin1(s); }

float fract(float v) { return v - std::floor(v); }

// ---- Colour helpers ----------------------------------------------------------------

struct Yiq {
    float y, i, q;
};
inline Yiq toYiq(float r, float g, float b)
{
    return {0.299f * r + 0.587f * g + 0.114f * b, 0.596f * r - 0.274f * g - 0.322f * b, 0.211f * r - 0.523f * g + 0.312f * b};
}
inline void fromYiq(float y, float i, float q, float& r, float& g, float& b)
{
    r = y + 0.956f * i + 0.621f * q;
    g = y - 0.272f * i - 0.647f * q;
    b = y - 1.106f * i + 1.703f * q;
}

// HSV hue (0..1) of a pixel.
float hueOf(uint32_t p)
{
    const float r = float(pr(p)), g = float(pg(p)), b = float(pb(p));
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
    if (d <= 0)
        return 0;
    float h = mx == r ? (g - b) / d : (mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d);
    h /= 6.0f;
    return h < 0 ? h + 1 : h;
}
uint32_t hsvColor(float h, float s, float v, int a = 255)
{
    h = fract(h) * 6.0f;
    const int i = int(h);
    const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i) {
    case 0: r = v, g = t, b = p; break;
    case 1: r = q, g = v, b = p; break;
    case 2: r = p, g = v, b = t; break;
    case 3: r = p, g = q, b = v; break;
    case 4: r = t, g = p, b = v; break;
    default: r = v, g = p, b = q; break;
    }
    const float k = float(a);
    return packf(k, r * k, g * k, b * k);
}

// Running box average of a float row, radius r (edges clamp).
void boxRow(float* v, int n, int r, std::vector<float>& sum)
{
    if (r <= 0 || n <= 1)
        return;
    sum.resize(size_t(n) + 1);
    sum[0] = 0;
    for (int i = 0; i < n; ++i)
        sum[size_t(i) + 1] = sum[size_t(i)] + v[i];
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - r), b = std::min(n - 1, i + r);
        v[i] = (sum[size_t(b) + 1] - sum[size_t(a)]) / float(b - a + 1);
    }
}

// Linear interpolation into a float row at fractional position (clamped).
inline float rowAt(const float* v, int n, float x)
{
    x = std::clamp(x, 0.0f, float(n - 1));
    const int i = std::min(int(x), n - 2 < 0 ? 0 : n - 2);
    const float f = x - float(i);
    return n == 1 ? v[0] : v[i] + (v[i + 1] - v[i]) * f;
}

// Per-pixel combine of three channel sources (chromatic effects).
uint32_t combineChannels(uint32_t r, uint32_t g, uint32_t b)
{
    const int a = std::max({pa(r), pa(g), pa(b)});
    return pack(a, pr(r), pg(g), pb(b));
}

// ---- Glitch ------------------------------------------------------------------------------

void chromaticAberration(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float amount = chromaAmount(p, ctx);
    if (amount == 0)
        return;
    const int w = img.width(), h = img.height();
    const QImage src = img;
    const Pixels in(src);
    const float cx = float(p.num(S("centerX"))) * float(w), cy = float(p.num(S("centerY"))) * float(h);
    const bool radial = p.choice(S("mode")) == 0;
    const float rmax = 0.5f * std::hypot(float(w), float(h));
    const float k = amount / rmax;
    const float falloff = float(p.num(S("falloff"))), gs = float(p.num(S("greenShift")));
    const float ang = float(p.num(S("angle"))) * kPi / 180.0f;
    const float ox = std::cos(ang) * amount, oy = std::sin(ang) * amount;
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            uint32_t r, g, b;
            if (radial) {
                // Offset grows with the distance from the centre (falloff 1: linearly).
                const float dx = px - cx, dy = py - cy;
                const float f = falloff == 1.0f ? 1.0f : std::pow(std::max(std::sqrt(dx * dx + dy * dy) / rmax, 1e-4f), falloff - 1.0f);
                r = in.sample(cx + dx * (1 + k * f), cy + dy * (1 + k * f), Edge::Clamp);
                b = in.sample(cx + dx * (1 - k * f), cy + dy * (1 - k * f), Edge::Clamp);
                g = gs == 0 ? in.at(x, y) : in.sample(cx + dx * (1 + gs * k * f), cy + dy * (1 + gs * k * f), Edge::Clamp);
            } else {
                r = in.sample(px + ox, py + oy, Edge::Clamp);
                b = in.sample(px - ox, py - oy, Edge::Clamp);
                g = gs == 0 ? in.at(x, y) : in.sample(px + gs * ox, py + gs * oy, Edge::Clamp);
            }
            row[x] = combineChannels(r, g, b);
        }
    });
}

const Edge kEdges4[4] = {Edge::Clamp, Edge::Wrap, Edge::Mirror, Edge::Transparent};

void rgbSplit(QImage& img, const Params& p, const RenderContext& ctx)
{
    const std::array<float, 6> o = rgbSplitOffsets(p, ctx);
    const Edge edge = kEdges4[std::clamp(p.choice(S("edge")), 0, 3)];
    const QImage src = img;
    const Pixels in(src);
    const int w = img.width();
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            row[x] = combineChannels(in.sample(px - o[0], py - o[1], edge), in.sample(px - o[2], py - o[3], edge),
                                     in.sample(px - o[4], py - o[5], edge));
        }
    });
}

void pixelSort(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float lo = float(p.num(S("lower"))), hi = float(p.num(S("upper")));
    const int by = p.choice(S("sortBy"));
    const bool vertical = p.choice(S("direction")) == 1;
    const bool reverse = p.flag(S("reverse"));
    const bool invert = p.flag(S("invert"));
    const int maxLen = int(p.num(S("maxLength")) * ctx.scale);
    const int thresholdBy = p.choice(S("thresholdBy"));
    const float breaks = float(p.num(S("breaks")));
    const uint32_t breakSeed = pixelSortBreakSeed(p, ctx);
    const int w = img.width(), h = img.height();
    const int lines = vertical ? w : h, len = vertical ? h : w;
    uchar* bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    auto key = [by](uint32_t px) -> int {
        switch (by) {
        case 1: return int(hueOf(px) * 255.0f);
        case 2: {
            const int mx = std::max({pr(px), pg(px), pb(px)}), mn = std::min({pr(px), pg(px), pb(px)});
            return mx ? (mx - mn) * 255 / mx : 0;
        }
        case 3: return pr(px);
        case 4: return pg(px);
        case 5: return pb(px);
        default: return int(luma(px) * 255.0f);
        }
    };
    parallelFor(lines, [&](int l0, int l1) {
        std::vector<uint32_t> line(static_cast<size_t>(len));
        std::vector<std::pair<int, uint32_t>> run;
        for (int l = l0; l < l1; ++l) {
            for (int i = 0; i < len; ++i)
                line[size_t(i)] = vertical ? reinterpret_cast<uint32_t*>(bits + bpl * i)[l] : reinterpret_cast<uint32_t*>(bits + bpl * l)[i];
            int i = 0;
            while (i < len) {
                auto inRange = [&](uint32_t px) {
                    float v;
                    if (thresholdBy == 1) {
                        v = hueOf(px);
                    } else if (thresholdBy == 2) {
                        const int mx = std::max({pr(px), pg(px), pb(px)}), mn = std::min({pr(px), pg(px), pb(px)});
                        v = float(mx ? (mx - mn) * 255 / mx : 0) / 255.0f;
                    } else {
                        v = luma(px);
                    }
                    return (v >= lo && v <= hi) != invert && pa(px) > 0;
                };
                // Random breaks start a new run at that pixel.
                auto breaksAt = [&](int k) { return breaks > 0 && hash01(uint32_t(k), uint32_t(l), breakSeed) < breaks; };
                if (!inRange(line[size_t(i)])) {
                    ++i;
                    continue;
                }
                int j = i;
                while (j < len && inRange(line[size_t(j)]) && (maxLen <= 0 || j - i < maxLen) && !(j > i && breaksAt(j)))
                    ++j;
                run.clear();
                for (int k = i; k < j; ++k)
                    run.emplace_back(key(line[size_t(k)]), line[size_t(k)]);
                std::stable_sort(run.begin(), run.end(), [reverse](const auto& a, const auto& b) { return reverse ? a.first > b.first : a.first < b.first; });
                for (int k = i; k < j; ++k)
                    line[size_t(k)] = run[size_t(k - i)].second;
                i = j;
            }
            for (int k = 0; k < len; ++k) {
                if (vertical)
                    reinterpret_cast<uint32_t*>(bits + bpl * k)[l] = line[size_t(k)];
                else
                    reinterpret_cast<uint32_t*>(bits + bpl * l)[k] = line[size_t(k)];
            }
        }
    });
}

// Datamosh: estimates block motion between consecutive input frames and
// moves the *previous output* along it instead of showing the new frame,
// like a video whose keyframes were deleted (melt), or keeps repeating the
// same motion (bloom).
struct MoshState : FrameHistory {
    std::vector<std::pair<int, int>> vectors; // per block, full-resolution pixels
    int blocksX = 0, blocksY = 0, blockPx = 0;
    int64_t cleanTick = std::numeric_limits<int64_t>::min(); // automatic clean frames
};

std::vector<uint8_t> lumaPlane(const QImage& img, int f, int& lw, int& lh)
{
    lw = std::max(1, img.width() / f);
    lh = std::max(1, img.height() / f);
    std::vector<uint8_t> out(size_t(lw) * size_t(lh));
    const Pixels in(img);
    parallelFor(lh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < lw; ++x) {
                int sum = 0;
                for (int j = 0; j < f; ++j)
                    for (int i = 0; i < f; ++i) {
                        const uint32_t px = in.at(std::min(x * f + i, in.w - 1), std::min(y * f + j, in.h - 1));
                        sum += (pr(px) * 77 + pg(px) * 150 + pb(px) * 29) >> 8;
                    }
                out[size_t(y) * size_t(lw) + size_t(x)] = uint8_t(sum / (f * f));
            }
    });
    return out;
}

void datamosh(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    MoshState& st = stateOf<MoshState>(state);
    const auto step = st.push(ctx, img, 2);
    const int64_t cleanTick = datamoshCleanTick(p, ctx);
    const bool clean = p.flag(S("refresh")) || cleanTick != st.cleanTick;
    st.cleanTick = cleanTick;
    if (step == FrameHistory::Step::Reset || clean || st.prevOut.isNull() || st.frames.size() < 2) {
        st.out = img;
        st.vectors.clear();
        return;
    }
    const QImage& prevIn = st.ago(1);
    const int w = img.width(), h = img.height();
    constexpr int f = 4; // motion search on a quarter-resolution luma plane
    const int bs = std::max(2, int(std::lround(p.num(S("blockSize")) * ctx.scale / f)));
    const int range = std::max(1, int(std::lround(p.num(S("search")) * ctx.scale / f)));
    const bool bloom = p.choice(S("mode")) == 1;
    int lw, lh;
    if (!bloom || st.vectors.empty() || st.blockPx != bs * f) {
        const std::vector<uint8_t> cur = lumaPlane(img, f, lw, lh), prev = lumaPlane(prevIn, f, lw, lh);
        st.blocksX = (lw + bs - 1) / bs;
        st.blocksY = (lh + bs - 1) / bs;
        st.blockPx = bs * f;
        st.vectors.assign(size_t(st.blocksX) * size_t(st.blocksY), {0, 0});
        parallelFor(st.blocksY, [&](int b0, int b1) {
            for (int by = b0; by < b1; ++by)
                for (int bx = 0; bx < st.blocksX; ++bx) {
                    const int x0 = bx * bs, y0 = by * bs;
                    auto sad = [&](int dx, int dy, int limit) {
                        int total = 0;
                        for (int y = y0; y < std::min(lh, y0 + bs); ++y) {
                            const int sy = std::clamp(y + dy, 0, lh - 1);
                            for (int x = x0; x < std::min(lw, x0 + bs); ++x) {
                                const int sx = std::clamp(x + dx, 0, lw - 1);
                                total += std::abs(int(cur[size_t(y) * size_t(lw) + size_t(x)]) - int(prev[size_t(sy) * size_t(lw) + size_t(sx)]));
                            }
                            if (total >= limit)
                                return total;
                        }
                        return total;
                    };
                    int best = sad(0, 0, INT32_MAX), bdx = 0, bdy = 0;
                    if (best > bs * bs * 2)
                        for (int dy = -range; dy <= range; ++dy)
                            for (int dx = -range; dx <= range; ++dx) {
                                const int v = sad(dx, dy, best);
                                if (v < best) {
                                    best = v;
                                    bdx = dx;
                                    bdy = dy;
                                }
                            }
                    st.vectors[size_t(by) * size_t(st.blocksX) + size_t(bx)] = {bdx * f, bdy * f};
                }
        });
    }
    // Move the previous output along the motion; keep a little of the new frame.
    const float amount = float(p.num(S("amount")));
    const QImage prevOut = st.prevOut.size() == img.size() ? st.prevOut : img;
    const Pixels po(prevOut);
    const QImage src = img;
    const Pixels in(src);
    const int a256 = int(amount * 256.0f);
    const float motion = float(p.num(S("motionScale")));
    forRows(img, [&](int y, uint32_t* row) {
        const int by = std::min(st.blocksY - 1, y / st.blockPx);
        for (int x = 0; x < w; ++x) {
            const int bx = std::min(st.blocksX - 1, x / st.blockPx);
            const auto [vx, vy] = st.vectors[size_t(by) * size_t(st.blocksX) + size_t(bx)];
            const int dx = int(std::lround(float(vx) * motion)), dy = int(std::lround(float(vy) * motion));
            const uint32_t moved = po.at(std::clamp(x + dx, 0, w - 1), std::clamp(y + dy, 0, h - 1));
            const uint32_t fresh = in.at(x, y);
            const uint32_t rb = (((moved & 0x00ff00ffu) * uint32_t(a256) + (fresh & 0x00ff00ffu) * uint32_t(256 - a256)) >> 8) & 0x00ff00ffu;
            const uint32_t ag = ((((moved >> 8) & 0x00ff00ffu) * uint32_t(a256) + ((fresh >> 8) & 0x00ff00ffu) * uint32_t(256 - a256))) & 0xff00ff00u;
            row[x] = rb | ag;
        }
    });
    st.out = img;
}

uint32_t glitchColor(uint32_t px, int mode)
{
    const int a = pa(px), r = pr(px), g = pg(px), b = pb(px);
    switch (mode) {
    case 0: return pack(a, b, g, r);                                 // swap red/blue
    case 1: return pack(a, a - r, a - g, a - b);                     // invert
    case 2: return pack(a, r, 0, 0);                                 // red only
    case 3: return pack(a, 0, g, b);                                 // cyan
    case 4: return pack(a, r & 0xc0, g & 0xc0, b & 0xc0);            // crushed
    default: return pack(a, (r ^ 0x5a) & 0xff, (g ^ 0xa5) & 0xff, b); // xor
    }
}

uint32_t rateSeed(const Params& p, const RenderContext& ctx)
{
    const double rate = p.num(S("rate"));
    const uint32_t tick = rate > 0 ? uint32_t(int64_t(std::floor(seconds(ctx) * rate))) : 0u;
    return tick * 7919u + uint32_t(p.num(S("seed"))) * 104729u + 17u;
}

void blockGlitch(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float amount = float(p.num(S("amount")));
    if (amount <= 0)
        return;
    const uint32_t seed = rateSeed(p, ctx);
    const int bh = std::max(2, int(p.num(S("blockSize")) * ctx.scale));
    const int bw = std::max(2, int(float(bh) * float(p.num(S("aspect")))));
    const float shift = float(p.num(S("maxShift")) * ctx.scale);
    const float vshift = float(p.num(S("verticalShift")));
    const float colorP = float(p.num(S("colorGlitch")));
    const int colorMode = p.choice(S("colorMode"));
    const float noiseP = float(p.num(S("noise")));
    const int w = img.width();
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        const int by = y / bh;
        for (int x = 0; x < w; ++x) {
            // Blocks in each row band are staggered so the grid is less obvious.
            const int stagger = int(hash01(uint32_t(by), 3, seed) * float(bw));
            const int bx = (x + stagger) / bw;
            const float hsh = hash01(uint32_t(bx), uint32_t(by), seed);
            if (hsh >= amount)
                continue;
            if (noiseP > 0 && hash01(uint32_t(bx), uint32_t(by), seed + 5) < noiseP) { // a block of static
                const uint32_t n = hash32(uint32_t(x), uint32_t(y), seed + 6);
                row[x] = pack(255, int(n & 255u), int((n >> 8) & 255u), int((n >> 16) & 255u));
                continue;
            }
            const int dx = int((hash01(uint32_t(bx), uint32_t(by), seed + 1) - 0.5f) * 2.0f * shift);
            const int dy = int((hash01(uint32_t(bx), uint32_t(by), seed + 2) - 0.5f) * 2.0f * vshift * float(bh));
            uint32_t px = in.fetch(x + dx, y + dy, Edge::Wrap);
            if (hash01(uint32_t(bx), uint32_t(by), seed + 3) < colorP)
                px = glitchColor(px, colorMode == 0 ? int(hash32(uint32_t(bx), uint32_t(by), seed + 4) % 6) : colorMode - 1);
            row[x] = px;
        }
    });
}

const Edge kSliceEdges[3] = {Edge::Wrap, Edge::Clamp, Edge::Transparent};

void sliceGlitch(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float split = float(p.num(S("rgbSplit")) * ctx.scale);
    const bool vertical = p.choice(S("direction")) == 1;
    const Edge edge = kSliceEdges[std::clamp(p.choice(S("edge")), 0, 2)];
    const int w = img.width(), h = img.height();
    const std::vector<RowPlan> rows = sliceRows(p, ctx, vertical ? w : h);
    const float sx = vertical ? 0.0f : split, sy = vertical ? split : 0.0f;
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const RowPlan& r = rows[size_t(vertical ? x : y)];
            if (r.b == 0)
                continue;
            const float px = x + 0.5f - (vertical ? 0.0f : r.a), py = y + 0.5f - (vertical ? r.a : 0.0f);
            uint32_t v = split != 0 ? combineChannels(in.sample(px - sx, py - sy, edge), in.sample(px, py, edge), in.sample(px + sx, py + sy, edge))
                                    : in.sample(px, py, edge);
            if (r.c > 0)
                v = glitchColor(v, int(r.c) - 1);
            row[x] = v;
        }
    });
}

// JPEG-style compression: NxN DCT blocks quantised with the standard
// tables, optional chroma subsampling, and optional coefficient corruption.
const int kJpegLuma[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69, 56,
                           14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
                           49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
const int kJpegChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99,
                             47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                             99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};

void dctPlane(std::vector<float>& plane, int w, int h, int n, const int* table, float qscale, float corruption, uint32_t seed,
              int target)
{
    std::vector<float> cosT(size_t(n) * size_t(n));
    for (int k = 0; k < n; ++k)
        for (int i = 0; i < n; ++i)
            cosT[size_t(k) * size_t(n) + size_t(i)] = (k == 0 ? std::sqrt(1.0f / n) : std::sqrt(2.0f / n)) * std::cos((2 * i + 1) * k * kPi / (2 * n));
    std::vector<float> q(size_t(n) * size_t(n));
    for (int v = 0; v < n; ++v)
        for (int u = 0; u < n; ++u)
            q[size_t(v) * size_t(n) + size_t(u)] = std::max(1.0f, std::floor((table[(v * 8 / n) * 8 + u * 8 / n] * qscale + 50.0f) / 100.0f));
    const int bxN = (w + n - 1) / n, byN = (h + n - 1) / n;
    parallelFor(byN, [&](int b0, int b1) {
        std::vector<float> blk(size_t(n) * size_t(n)), tmp(size_t(n) * size_t(n));
        for (int by = b0; by < b1; ++by)
            for (int bx = 0; bx < bxN; ++bx) {
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                        blk[size_t(y) * size_t(n) + size_t(x)] =
                            plane[size_t(std::min(h - 1, by * n + y)) * size_t(w) + size_t(std::min(w - 1, bx * n + x))] - 128.0f;
                // Forward DCT (rows, then columns).
                for (int y = 0; y < n; ++y)
                    for (int k = 0; k < n; ++k) {
                        float s = 0;
                        for (int x = 0; x < n; ++x)
                            s += cosT[size_t(k) * size_t(n) + size_t(x)] * blk[size_t(y) * size_t(n) + size_t(x)];
                        tmp[size_t(y) * size_t(n) + size_t(k)] = s;
                    }
                for (int k = 0; k < n; ++k)
                    for (int u = 0; u < n; ++u) {
                        float s = 0;
                        for (int y = 0; y < n; ++y)
                            s += cosT[size_t(k) * size_t(n) + size_t(y)] * tmp[size_t(y) * size_t(n) + size_t(u)];
                        const float qq = q[size_t(k) * size_t(n) + size_t(u)];
                        blk[size_t(k) * size_t(n) + size_t(u)] = std::round(s / qq) * qq;
                    }
                // Corruption: smashed coefficients in some blocks.
                if (corruption > 0 && hash01(uint32_t(bx), uint32_t(by), seed) < corruption) {
                    const int count = 1 + int(hash32(uint32_t(bx), uint32_t(by), seed + 1) % 4);
                    for (int c = 0; c < count; ++c) {
                        const uint32_t hh = hash32(uint32_t(bx), uint32_t(by), seed + 2 + uint32_t(c));
                        // Any coefficient, the block's colour (DC) or its detail (AC) only.
                        const size_t idx = target == 1 ? 0 : (target == 2 ? 1 + size_t(hh % uint32_t(n * n - 1)) : size_t(hh % uint32_t(n * n)));
                        blk[idx] += (float(int(hh >> 8) % 512) - 256.0f) * 2.0f;
                    }
                }
                // Inverse DCT.
                for (int y = 0; y < n; ++y)
                    for (int u = 0; u < n; ++u) {
                        float s = 0;
                        for (int k = 0; k < n; ++k)
                            s += cosT[size_t(k) * size_t(n) + size_t(y)] * blk[size_t(k) * size_t(n) + size_t(u)];
                        tmp[size_t(y) * size_t(n) + size_t(u)] = s;
                    }
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x) {
                        float s = 0;
                        for (int k = 0; k < n; ++k)
                            s += cosT[size_t(k) * size_t(n) + size_t(x)] * tmp[size_t(y) * size_t(n) + size_t(k)];
                        const int py = by * n + y, px = bx * n + x;
                        if (py < h && px < w)
                            plane[size_t(py) * size_t(w) + size_t(px)] = s + 128.0f;
                    }
            }
    });
}

void compressionArtifacts(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int quality = std::clamp(int(p.num(S("quality"))), 1, 100);
    const float qscale = quality < 50 ? 5000.0f / quality : 200.0f - 2.0f * quality;
    const int cq = int(p.num(S("chromaQuality"))) > 0 ? std::clamp(int(p.num(S("chromaQuality"))), 1, 100) : quality;
    const float qscaleC = cq < 50 ? 5000.0f / cq : 200.0f - 2.0f * cq;
    const int n = jpegBlockSize(p, ctx);
    const int target = p.choice(S("corruptWhat"));
    const float corruption = float(p.num(S("corruption")));
    const uint32_t seed = p.flag(S("animated")) ? frameIndex(ctx) * 31u + 5u : 5u;
    const bool subsample = p.flag(S("subsample"));
    const int w = img.width(), h = img.height();
    std::vector<float> Y(size_t(w) * size_t(h)), Cb(Y.size()), Cr(Y.size());
    const Pixels in(img);
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t px = in.at(x, y);
                const float un = pa(px) ? 255.0f / float(pa(px)) : 0.0f; // straight colour
                const float r = float(pr(px)) * un, g = float(pg(px)) * un, b = float(pb(px)) * un;
                const size_t i = size_t(y) * size_t(w) + size_t(x);
                Y[i] = 0.299f * r + 0.587f * g + 0.114f * b;
                Cb[i] = 128.0f - 0.168736f * r - 0.331264f * g + 0.5f * b;
                Cr[i] = 128.0f + 0.5f * r - 0.418688f * g - 0.081312f * b;
            }
    });
    dctPlane(Y, w, h, n, kJpegLuma, qscale, corruption, seed, target);
    auto chroma = [&](std::vector<float>& c, uint32_t extra) {
        if (!subsample) {
            dctPlane(c, w, h, n, kJpegChroma, qscaleC, corruption, seed + extra, target);
            return;
        }
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        std::vector<float> half(size_t(cw) * size_t(ch));
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                float s = 0;
                for (int j = 0; j < 2; ++j)
                    for (int i = 0; i < 2; ++i)
                        s += c[size_t(std::min(h - 1, y * 2 + j)) * size_t(w) + size_t(std::min(w - 1, x * 2 + i))];
                half[size_t(y) * size_t(cw) + size_t(x)] = s * 0.25f;
            }
        dctPlane(half, cw, ch, n, kJpegChroma, qscaleC, corruption, seed + extra, target);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                c[size_t(y) * size_t(w) + size_t(x)] = half[size_t(y / 2) * size_t(cw) + size_t(x / 2)];
    };
    chroma(Cb, 100);
    chroma(Cr, 200);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const float yy = Y[i], cb = Cb[i] - 128.0f, cr = Cr[i] - 128.0f;
            const int a = pa(row[x]);
            const float k = float(a) / 255.0f;
            row[x] = packf(float(a), (yy + 1.402f * cr) * k, (yy - 0.344136f * cb - 0.714136f * cr) * k, (yy + 1.772f * cb) * k);
        }
    });
}

// Databending: treats the picture's colour bytes as one long stream and
// edits it like sound or a hex file, so effects cross channel boundaries.
void databend(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int mode = p.choice(S("mode"));
    const float mix = float(p.num(S("mix")));
    if (mix <= 0)
        return;
    const int w = img.width(), h = img.height();
    const size_t n = size_t(w) * size_t(h) * 3;
    std::vector<uint8_t> in(n), out;
    {
        const Pixels px(img);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint32_t v = px.at(x, y);
                const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 3;
                in[i] = uint8_t(pr(v));
                in[i + 1] = uint8_t(pg(v));
                in[i + 2] = uint8_t(pb(v));
            }
    }
    out = in;
    const size_t delay = std::max<size_t>(1, size_t(p.num(S("delay")) * ctx.scale * ctx.scale));
    const float fb = float(p.num(S("feedback")));
    const size_t chunk = std::max<size_t>(3, size_t(p.num(S("chunk")) * ctx.scale * ctx.scale));
    const uint32_t seed = p.flag(S("animated")) ? frameIndex(ctx) : 1u;
    switch (mode) {
    case 0: { // echo with feedback (an audio delay on the bytes): acc[i] = in[i] + fb * acc[i - delay]
        // A linear recurrence, so the GPU can compute it with a doubling scan.
        std::vector<float> acc(n);
        for (size_t i = 0; i < n; ++i) {
            acc[i] = float(in[i]) + (i >= delay ? fb * acc[i - delay] : 0.0f);
            out[i] = uint8_t(std::min(255.0f, acc[i]));
        }
        break;
    }
    case 1: // reverse every other chunk
        for (size_t c = 0; c * chunk < n; c += 2)
            std::reverse(out.begin() + std::ptrdiff_t(c * chunk), out.begin() + std::ptrdiff_t(std::min(n, (c + 1) * chunk)));
        break;
    case 2: { // rotate bits of every byte
        const int bits = std::clamp(int(p.num(S("bits"))), 1, 7);
        for (size_t i = 0; i < n; ++i)
            out[i] = uint8_t((in[i] << bits) | (in[i] >> (8 - bits)));
        break;
    }
    case 3: // XOR random chunks with noise
        for (size_t c = 0; c * chunk < n; ++c) {
            if (hash01(uint32_t(c), 7, seed) > fb)
                continue;
            const uint8_t key = uint8_t(hash32(uint32_t(c), 8, seed));
            for (size_t i = c * chunk; i < std::min(n, (c + 1) * chunk); ++i)
                out[i] ^= key;
        }
        break;
    case 4: { // byte shift: the stream slides by the delay, colours rotate between channels
        const size_t shift = delay % n;
        for (size_t i = 0; i < n; ++i)
            out[i] = in[(i + shift) % n];
        break;
    }
    case 5: { // wrong stride: read with a slightly wrong line length, the picture skews and tears
        const int64_t N = int64_t(n), stride = int64_t(w) * 3 + int64_t(p.num(S("stride")));
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 3; ++c) {
                    const int64_t j = int64_t(y) * stride + int64_t(x) * 3 + c;
                    out[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(c)] = in[size_t(((j % N) + N) % N)];
                }
        break;
    }
    default: // repeat chunks: random chunks show the chunk before them again
        for (size_t c = 1; c * chunk < n; ++c) {
            if (hash01(uint32_t(c), 9, seed) > fb)
                continue;
            for (size_t i = c * chunk; i < std::min(n, (c + 1) * chunk); ++i)
                out[i] = in[i - chunk];
        }
        break;
    }
    const int m = int(mix * 256.0f);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (size_t(y) * size_t(w) + size_t(x)) * 3;
            const int a = pa(row[x]);
            auto blend = [&](int k) { return (int(in[i + size_t(k)]) * (256 - m) + int(out[i + size_t(k)]) * m) >> 8; };
            row[x] = pack(a, blend(0), blend(1), blend(2));
        }
    });
}

void pixelStretch(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int dir = p.choice(S("direction"));
    const float pos = stretchPosition(p, ctx);
    const float length = float(p.num(S("length")));
    const float jitter = float(p.num(S("jitter")) * ctx.scale);
    const uint32_t seed = stretchSeed(p, ctx);
    const int w = img.width(), h = img.height();
    const bool horizontal = dir <= 1;
    const int n = horizontal ? w : h; // pixels along the stretch
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const int line = horizontal ? y : x, along = horizontal ? x : y;
            // The smeared row / column, moved per line by the jitter.
            const float off = jitter > 0 ? (hash01(uint32_t(line), 21, seed) - 0.5f) * 2.0f * jitter : 0.0f;
            const int at = std::clamp(int(pos * float(n) + off), 0, n - 1);
            const bool forward = dir == 0 || dir == 2;
            const bool take = forward ? along > at && along <= at + int(length * float(n - 1 - at))
                                      : along < at && along >= at - int(length * float(at));
            if (take)
                row[x] = horizontal ? in.at(at, y) : in.at(x, at);
        }
    });
}

// Frame-rate stutter: holds frames like a low frame rate.
struct HoldState : EffectState {
    int64_t tick = std::numeric_limits<int64_t>::min();
    int64_t frozenUntil = std::numeric_limits<int64_t>::min();
    QImage held;
};

void stutter(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    HoldState& st = stateOf<HoldState>(state);
    const double fps = std::max(0.1, p.num(S("fps")));
    const int64_t tick = int64_t(std::floor(seconds(ctx) * fps + 1e-6));
    if (st.held.size() != img.size()) {
        st.tick = tick;
        st.frozenUntil = std::numeric_limits<int64_t>::min();
        st.held = img;
        return;
    }
    if (tick != st.tick) {
        const bool fresh = stutterShowsNew(p, tick, st.tick, st.frozenUntil);
        st.tick = tick;
        if (fresh) {
            st.held = img;
            return;
        }
    }
    img = st.held;
}

void strobe(QImage& img, const Params& p, const RenderContext& ctx)
{
    const StrobeNow now = strobeNow(p, ctx);
    const float opacity = float(p.num(S("opacity")));
    if (!now.on || opacity <= 0)
        return;
    const int mode = p.choice(S("mode"));
    const int cr = int((now.rgb >> 16) & 255u), cg = int((now.rgb >> 8) & 255u), cb = int(now.rgb & 255u);
    const uint32_t k = opacity >= 1 ? 256u : uint32_t(opacity * 256.0f), s = 256 - k;
    forRows(img, [&](int, uint32_t* row) {
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t px = row[x];
            const int a = pa(px);
            uint32_t f;
            switch (mode) {
            case 1: f = pack(a, a - pr(px), a - pg(px), a - pb(px)); break;
            case 2: f = pack(a, 0, 0, 0); break;
            case 3: f = 0; break;
            case 6: { // black & white
                const int v = luma(px) * 255.0f >= float(a) * 0.5f ? a : 0;
                f = pack(a, v, v, v);
                break;
            }
            default: f = pack(255, cr, cg, cb); break; // flash colour, random colour, rainbow
            }
            if (k < 256) {
                const uint32_t rb = (((px & 0x00ff00ffu) * s + (f & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
                const uint32_t ag = ((((px >> 8) & 0x00ff00ffu) * s + ((f >> 8) & 0x00ff00ffu) * k)) & 0xff00ff00u;
                f = rb | ag;
            }
            row[x] = f;
        }
    });
}

// RGB time split: red now, green and blue from earlier frames.
void timeSplit(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    FrameHistory& st = stateOf<FrameHistory>(state);
    const std::array<int, 3> d = timeSplitDelays(p);
    st.push(ctx, img, size_t(std::max({d[0], d[1], d[2]})) + 1);
    const Pixels r(st.ago(size_t(d[0]))), g(st.ago(size_t(d[1]))), b(st.ago(size_t(d[2])));
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < img.width(); ++x)
            row[x] = combineChannels(r.at(x, y), g.at(x, y), b.at(x, y));
    });
}

// ---- Analog & retro ---------------------------------------------------------------------------

void vhs(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float s = float(ctx.scale);
    const int w = img.width(), h = img.height();
    const int blur = int(std::lround(p.num(S("blur")) * s));
    const int bleed = int(std::lround(p.num(S("bleed")) * s));
    const float chromaShift = float(p.num(S("chromaShift"))) * s;
    const float sat = float(p.num(S("saturation")));
    const float noise = float(p.num(S("noise")));
    const float tracking = float(p.num(S("tracking")));
    const uint32_t frame = p.flag(S("animated")) ? frameIndex(ctx) : 0u;
    const std::vector<RowPlan> rows = vhsRows(p, ctx, w, h);
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        thread_local std::vector<float> Y, I, Q, A, sum;
        Y.resize(size_t(w));
        I.resize(size_t(w));
        Q.resize(size_t(w));
        A.resize(size_t(w));
        const RowPlan& rp = rows[size_t(y)];
        for (int x = 0; x < w; ++x) {
            const uint32_t px = in.sample(x + 0.5f - rp.a, y + 0.5f, Edge::Clamp);
            const Yiq c = toYiq(float(pr(px)), float(pg(px)), float(pb(px)));
            Y[size_t(x)] = c.y;
            I[size_t(x)] = c.i;
            Q[size_t(x)] = c.q;
            A[size_t(x)] = float(pa(px));
        }
        boxRow(Y.data(), w, blur, sum);
        boxRow(I.data(), w, bleed, sum);
        boxRow(I.data(), w, bleed, sum);
        boxRow(Q.data(), w, bleed, sum);
        boxRow(Q.data(), w, bleed, sum);
        const int dropStart = int(rp.c), dropLen = int(rp.d);
        for (int x = 0; x < w; ++x) {
            float yy = Y[size_t(x)];
            const float ii = rowAt(I.data(), w, x - chromaShift) * sat, qq = rowAt(Q.data(), w, x - chromaShift) * sat;
            if (noise > 0)
                yy += (hash01(uint32_t(x), uint32_t(y), frame * 3 + 1) - 0.5f) * noise * 90.0f;
            if (rp.b > 0)
                yy += (hash01(uint32_t(x / 2), uint32_t(y), frame + 9) - 0.5f) * tracking * 120.0f;
            if (dropStart >= 0 && x >= dropStart && x < dropStart + dropLen)
                yy += 140.0f * (1.0f - std::abs(float(x - dropStart) / float(std::max(1, dropLen)) * 2.0f - 1.0f));
            float r, g, b;
            fromYiq(yy, ii, qq, r, g, b);
            const float a = A[size_t(x)];
            row[x] = packf(a, r, g, b);
        }
    });
}

void vhsTracking(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width();
    const float s = float(ctx.scale);
    if (p.num(S("height")) <= 0)
        return;
    const float noise = float(p.num(S("noise")));
    const uint32_t frame = frameIndex(ctx);
    const std::vector<RowPlan> rows = vhsTrackingRows(p, ctx, img.height());
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        const float wgt = rows[size_t(y)].a, off = rows[size_t(y)].b;
        if (wgt <= 0)
            return;
        for (int x = 0; x < w; ++x) {
            uint32_t px = in.sample(x + 0.5f - off, y + 0.5f, Edge::Clamp);
            const float n = hash01(uint32_t(x / std::max(1, int(2 * s))), uint32_t(y), frame + 52);
            if (n < noise * wgt * 0.6f) {
                const int v = 150 + int(hash01(uint32_t(x), uint32_t(y), frame + 53) * 105);
                px = pack(255, v, v, v);
            } else {
                const int lift = int(30.0f * wgt);
                px = pack(pa(px), pr(px) + lift, pg(px) + lift, pb(px) + lift);
            }
            row[x] = px;
        }
    });
}

void crt(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const float scan = float(p.num(S("scanlines")));
    const float lines = float(p.num(S("lineCount")));
    const int mask = p.choice(S("mask"));
    const float maskK = 1.0f - float(p.num(S("maskStrength")));
    const float curve = float(p.num(S("curvature")));
    const float vig = float(p.num(S("vignette")));
    const float bright = float(p.num(S("brightness")));
    const int pitch = std::max(1, int(std::lround(3 * ctx.scale)));
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            float u = (x + 0.5f) / w * 2 - 1, v = (y + 0.5f) / h * 2 - 1;
            const float r2 = u * u + v * v;
            u *= 1 + curve * r2;
            v *= 1 + curve * r2;
            if (std::abs(u) > 1 || std::abs(v) > 1) {
                row[x] = pack(255, 0, 0, 0);
                continue;
            }
            const uint32_t px = in.sample((u + 1) * 0.5f * w, (v + 1) * 0.5f * h, Edge::Clamp);
            const float phase = (v + 1) * 0.5f * lines;
            float k = (1.0f - scan * (0.5f - 0.5f * std::cos(2 * kPi * phase))) * bright;
            k *= 1.0f - vig * (u * u * v * v * 3.0f + r2 * 0.15f);
            float mr = 1, mg = 1, mb = 1;
            if (mask > 0) {
                int col = (x / pitch) % 3;
                if (mask == 2 && ((y / pitch) & 1))
                    col = (col + 1) % 3; // shadow mask: rows offset
                if (mask == 3 && ((y / (pitch * 2)) % 2) && ((x / (pitch * 3)) % 2))
                    col = (col + 1) % 3; // slot mask
                mr = col == 0 ? 1.0f : maskK;
                mg = col == 1 ? 1.0f : maskK;
                mb = col == 2 ? 1.0f : maskK;
            }
            row[x] = packf(float(pa(px)), pr(px) * k * mr, pg(px) * k * mg, pb(px) * k * mb);
        }
    });
}

// NTSC composite: encodes each line onto a colour subcarrier and decodes it
// with simple filters, which brings back dot crawl, rainbow fringes on fine
// detail and smeared colour.
void ntsc(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float strength = float(p.num(S("strength")));
    if (strength <= 0)
        return;
    const int w = img.width();
    const float period = std::max(2.0f, float(w) / 190.0f);
    const int perR = std::max(1, int(period * 0.5f));
    const int bleed = std::max(perR, int(std::lround(p.num(S("bleed")) * ctx.scale)));
    const float ringing = float(p.num(S("ringing")));
    const uint32_t frame = p.flag(S("dotCrawl")) ? frameIndex(ctx) : 0u;
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        thread_local std::vector<float> sig, Y, I, Q, cosv, sinv, sum, blurY;
        for (auto* v : {&sig, &Y, &I, &Q, &cosv, &sinv, &blurY})
            v->resize(size_t(w));
        const float lineShift = ((y + int(frame)) & 1) ? kPi : 0.0f;
        for (int x = 0; x < w; ++x) {
            const float ph = 2 * kPi * x / period + lineShift;
            cosv[size_t(x)] = std::cos(ph);
            sinv[size_t(x)] = std::sin(ph);
            const uint32_t px = in.at(x, y);
            const Yiq c = toYiq(float(pr(px)), float(pg(px)), float(pb(px)));
            sig[size_t(x)] = c.y + c.i * cosv[size_t(x)] + c.q * sinv[size_t(x)];
        }
        // Luma: the carrier averages out over one period.
        std::copy(sig.begin(), sig.end(), Y.begin());
        boxRow(Y.data(), w, perR, sum);
        for (int x = 0; x < w; ++x) {
            const float c = sig[size_t(x)] - Y[size_t(x)];
            I[size_t(x)] = 2 * c * cosv[size_t(x)];
            Q[size_t(x)] = 2 * c * sinv[size_t(x)];
        }
        boxRow(I.data(), w, bleed, sum);
        boxRow(Q.data(), w, bleed, sum);
        if (ringing > 0) {
            std::copy(Y.begin(), Y.end(), blurY.begin());
            boxRow(blurY.data(), w, perR * 3, sum);
            for (int x = 0; x < w; ++x)
                Y[size_t(x)] += (Y[size_t(x)] - blurY[size_t(x)]) * ringing * 1.5f;
        }
        for (int x = 0; x < w; ++x) {
            float r, g, b;
            fromYiq(Y[size_t(x)], I[size_t(x)], Q[size_t(x)], r, g, b);
            const uint32_t o = in.at(x, y);
            const float a = float(pa(o));
            row[x] = packf(a, pr(o) + (r - pr(o)) * strength, pg(o) + (g - pg(o)) * strength, pb(o) + (b - pb(o)) * strength);
        }
    });
}

void tvStatic(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float amount = float(p.num(S("amount")));
    const bool colorNoise = p.flag(S("colorNoise"));
    const uint32_t frame = p.flag(S("animated")) ? frameIndex(ctx) : 0u;
    const int w = img.width();
    const int grain = std::max(1, int(std::lround(ctx.scale * 1.5)));
    const std::vector<RowPlan> rows = tvStaticRows(p, ctx, img.height());
    forRows(img, [&](int y, uint32_t* row) {
        const float barK = rows[size_t(y)].a;
        const bool lost = rows[size_t(y)].b > 0;
        const float mixK = lost ? 1.0f : amount;
        for (int x = 0; x < w; ++x) {
            const uint32_t px = row[x];
            const uint32_t hsh = hash32(uint32_t(x / grain), uint32_t(y / grain), frame + 62);
            const int n = int(hsh & 0xff);
            const int nr = colorNoise ? int((hsh >> 8) & 0xff) : n, ng = colorNoise ? int((hsh >> 16) & 0xff) : n;
            const float r = pr(px) + (float(nr) - pr(px)) * mixK, g = pg(px) + (float(ng) - pg(px)) * mixK, b = pb(px) + (float(n) - pb(px)) * mixK;
            const float a = lost ? 255.0f : float(pa(px)) + (255.0f - float(pa(px))) * mixK;
            row[x] = packf(a, r * barK, g * barK, b * barK);
        }
    });
}

void signalJitter(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float s = float(ctx.scale);
    const int h = img.height();
    const float amp = float(p.num(S("amplitude"))) * s;
    const float freq = float(p.num(S("frequency")));
    const double t = seconds(ctx);
    const float speed = float(p.num(S("speed")));
    const float noise = float(p.num(S("noise"))) * s;
    const float roll = float(fract(float(t * p.num(S("roll"))))) * float(h);
    const int barH = p.num(S("roll")) != 0 ? std::max(2, h / 25) : 0;
    const uint32_t frame = frameIndex(ctx);
    warp(img, Edge::Wrap, [&](float x, float y, float& sx, float& sy) {
        float yy = y - roll;
        yy -= std::floor(yy / h) * h;
        if (barH && yy > h - barH)
            return false; // the blank bar between frames while rolling
        sy = yy;
        sx = x - amp * std::sin(2 * kPi * (yy / h * freq + float(t) * speed)) -
             noise * (hash01(uint32_t(int(yy)), frame, 71) - 0.5f) * 2.0f;
        return true;
    });
}

void interlace(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    const int mode = p.choice(S("mode"));
    FrameHistory& st = stateOf<FrameHistory>(state);
    st.push(ctx, img, 2);
    const QImage prev = st.ago(1);
    const QImage src = img;
    const Pixels in(src), pv(prev);
    const float shift = float(p.num(S("shift")) * ctx.scale);
    forRows(img, [&](int y, uint32_t* row) {
        if (!(y & 1))
            return;
        for (int x = 0; x < img.width(); ++x) {
            switch (mode) {
            case 0: row[x] = pv.at(x, y); break;                                       // combing
            case 1: row[x] = in.sample(x + 0.5f - shift, y + 0.5f, Edge::Clamp); break; // line shift
            default: row[x] = in.at(x, y - 1); break;                                  // line doubling
            }
        }
    });
}

void oldFilm(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const uint32_t frame = frameIndex(ctx);
    const FilmPlan plan = oldFilmPlan(p, ctx, w, h);
    const float sepia = float(p.num(S("sepia")));
    const float grain = float(p.num(S("grain")));
    const float vig = float(p.num(S("vignette")));
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = in.sample(x + 0.5f - plan.dx, y + 0.5f - plan.dy, Edge::Clamp);
            float r = float(pr(px)), g = float(pg(px)), b = float(pb(px));
            const float l = 0.299f * r + 0.587f * g + 0.114f * b;
            r += (l * 1.07f + 12 - r) * sepia;
            g += (l * 0.95f + 4 - g) * sepia;
            b += (l * 0.75f - 6 - b) * sepia;
            float k = plan.flicker;
            if (vig > 0) {
                const float u = (x + 0.5f) / w - 0.5f, v = (y + 0.5f) / h - 0.5f;
                k *= 1.0f - vig * std::min(1.0f, (u * u + v * v) * 2.2f);
            }
            const float n = grain > 0 ? (hash01(uint32_t(x), uint32_t(y), frame + 84) - 0.5f) * grain * 80.0f : 0.0f;
            uint32_t out = packf(float(pa(px)), r * k + n, g * k + n, b * k + n);
            // Scratches: thin vertical lines that drift and come and go.
            for (size_t i = 0; i < plan.scratchDark.size(); ++i) {
                const int sx = int(plan.scratchX[size_t(y) * plan.scratchDark.size() + i]);
                if (x >= sx && x < sx + plan.scratchWidth)
                    out = plan.scratchDark[i] ? pack(pa(out), pr(out) / 3, pg(out) / 3, pb(out) / 3)
                                              : pack(pa(out), pr(out) + 110, pg(out) + 110, pb(out) + 100);
            }
            // Dust: small dark and light specks, new every frame.
            for (const auto& sp : plan.specks) {
                const float d = std::hypot(x + 0.5f - sp[0], y + 0.5f - sp[1]);
                const float cov = std::clamp(sp[2] + 0.5f - d, 0.0f, 1.0f);
                if (cov <= 0)
                    continue;
                const float target = sp[3] > 0 ? 15.0f : 235.0f;
                out = packf(float(pa(out)), pr(out) + (target - pr(out)) * cov, pg(out) + (target - pg(out)) * cov, pb(out) + (target - pb(out)) * cov);
            }
            row[x] = out;
        }
    });
}

void ghosting(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float off = float(p.num(S("offset")) * ctx.scale);
    const float strength = float(p.num(S("strength")));
    const int echoes = std::max(1, int(p.num(S("echoes"))));
    if (strength <= 0 || off == 0)
        return;
    std::vector<float> weights;
    float total = 1;
    for (int k = 1; k <= echoes; ++k) {
        weights.push_back(strength * std::pow(0.6f, float(k - 1)));
        total += weights.back();
    }
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t c = in.at(x, y);
            float a = float(pa(c)), r = float(pr(c)), g = float(pg(c)), b = float(pb(c));
            for (int k = 1; k <= echoes; ++k) {
                const uint32_t e = in.sample(x + 0.5f - off * k, y + 0.5f, Edge::Clamp);
                const float wk = weights[size_t(k - 1)];
                a += pa(e) * wk;
                r += pr(e) * wk;
                g += pg(e) * wk;
                b += pb(e) * wk;
            }
            row[x] = packf(std::max(float(pa(c)), a / total), r / total, g / total, b / total);
        }
    });
}


// ---- Psychedelic & distort -------------------------------------------------------------------

void kaleidoscope(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int segments = std::max(2, int(p.num(S("segments"))));
    const float rot = float((p.num(S("rotation")) + p.num(S("spin")) * seconds(ctx)) * kPi / 180.0);
    const float zoom = float(p.num(S("zoom")));
    const int w = img.width(), h = img.height();
    const float cx = float(p.num(S("centerX"))) * w, cy = float(p.num(S("centerY"))) * h;
    const float seg = 2 * kPi / segments;
    warp(img, Edge::Mirror, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy;
        const float r = std::sqrt(dx * dx + dy * dy) / zoom;
        float a = std::atan2(dy, dx) - rot;
        a -= std::floor(a / seg) * seg;
        if (a > seg * 0.5f)
            a = seg - a;
        a += rot;
        sx = cx + r * std::cos(a);
        sy = cy + r * std::sin(a);
        return true;
    });
}

void mirrorSymmetry(QImage& img, const Params& p)
{
    const int mode = p.choice(S("mode"));
    const int w = img.width(), h = img.height();
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            int sx = x, sy = y;
            const bool left = x < w / 2, top = y < h / 2;
            switch (mode) {
            case 0: if (!left) sx = w - 1 - x; break;   // left half onto the right
            case 1: if (left) sx = w - 1 - x; break;    // right half onto the left
            case 2: if (!top) sy = h - 1 - y; break;    // top onto bottom
            case 3: if (top) sy = h - 1 - y; break;     // bottom onto top
            default:                                     // four-way from the top-left quarter
                if (!left) sx = w - 1 - x;
                if (!top) sy = h - 1 - y;
                break;
            }
            row[x] = in.at(sx, sy);
        }
    });
}

void waveWarp(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int dir = p.choice(S("direction"));
    const float amp = float(p.num(S("amplitude")) * ctx.scale);
    const float len = float(std::max(1.0, p.num(S("wavelength")) * ctx.scale));
    const float ph = float(p.num(S("speed")) * seconds(ctx));
    if (amp == 0)
        return;
    warp(img, Edge::Mirror, [&](float x, float y, float& sx, float& sy) {
        sx = x;
        sy = y;
        if (dir != 1)
            sx += amp * std::sin(2 * kPi * (y / len - ph));
        if (dir != 0)
            sy += amp * std::sin(2 * kPi * (x / len - ph));
        return true;
    });
}

void twirl(QImage& img, const Params& p)
{
    const int w = img.width(), h = img.height();
    const float angle = float(p.num(S("angle"))) * kPi / 180.0f;
    const float radius = float(p.num(S("radius"))) * h * 0.5f;
    const float cx = float(p.num(S("centerX"))) * w, cy = float(p.num(S("centerY"))) * h;
    if (angle == 0 || radius <= 0)
        return;
    warp(img, Edge::Clamp, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
        sx = x;
        sy = y;
        if (r < radius) {
            const float k = 1 - r / radius, a = angle * k * k;
            const float c = std::cos(a), s = std::sin(a);
            sx = cx + dx * c - dy * s;
            sy = cy + dx * s + dy * c;
        }
        return true;
    });
}

void ripple(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const float amp = float(p.num(S("amplitude")) * ctx.scale);
    const float len = float(std::max(1.0, p.num(S("wavelength")) * ctx.scale));
    const float ph = float(p.num(S("speed")) * seconds(ctx));
    const float decay = float(p.num(S("decay")));
    const float cx = float(p.num(S("centerX"))) * w, cy = float(p.num(S("centerY"))) * h;
    const float diag = 0.5f * std::hypot(float(w), float(h));
    warp(img, Edge::Mirror, [&](float x, float y, float& sx, float& sy) {
        const float dx = x - cx, dy = y - cy, r = std::sqrt(dx * dx + dy * dy);
        const float off = amp * std::sin(2 * kPi * (r / len - ph)) * std::exp(-decay * 4.0f * r / diag);
        const float k = r > 0.001f ? (r + off) / r : 1.0f;
        sx = cx + dx * k;
        sy = cy + dy * k;
        return true;
    });
}

void lensDistortion(QImage& img, const Params& p)
{
    const int w = img.width(), h = img.height();
    const float k = float(p.num(S("amount"))), zoom = float(p.num(S("zoom")));
    const float cx = w * 0.5f, cy = h * 0.5f, norm = 0.5f * std::hypot(float(w), float(h));
    warp(img, Edge::Transparent, [&](float x, float y, float& sx, float& sy) {
        const float dx = (x - cx) / norm, dy = (y - cy) / norm, r2 = dx * dx + dy * dy;
        const float f = (1 + k * r2) / zoom;
        sx = cx + dx * f * norm;
        sy = cy + dy * f * norm;
        return sx >= 0 && sy >= 0 && sx <= w && sy <= h;
    });
}

void polarCoordinates(QImage& img, const Params& p)
{
    const int w = img.width(), h = img.height();
    const bool toPolar = p.choice(S("mode")) == 0;
    const float cx = w * 0.5f, cy = h * 0.5f, rmax = std::min(cx, cy);
    warp(img, Edge::Wrap, [&](float x, float y, float& sx, float& sy) {
        if (toPolar) {
            // The rectangle is wrapped around the centre: x = angle, y = radius.
            const float dx = (x - cx) / rmax, dy = (y - cy) / rmax;
            const float a = std::atan2(dx, -dy) / (2 * kPi) + 0.5f;
            const float r = std::sqrt(dx * dx + dy * dy);
            sx = a * w;
            sy = r * h;
            return r <= 1.0f;
        }
        const float a = (x / w - 0.5f) * 2 * kPi, r = y / h * rmax;
        sx = cx + r * std::sin(a);
        sy = cy - r * std::cos(a);
        return true;
    });
}

void tile(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const float tx = float(std::max(1.0, p.num(S("tilesX")))), ty = float(std::max(1.0, p.num(S("tilesY"))));
    const bool mirror = p.flag(S("mirror"));
    const float sx0 = float(p.num(S("scrollX")) * seconds(ctx)), sy0 = float(p.num(S("scrollY")) * seconds(ctx));
    warp(img, mirror ? Edge::Mirror : Edge::Wrap, [&](float x, float y, float& sx, float& sy) {
        sx = (x / w * tx + sx0) * w;
        sy = (y / h * ty + sy0) * h;
        return true;
    });
}

void radialBlur(QImage& img, const Params& p)
{
    const int w = img.width(), h = img.height();
    const bool spin = p.choice(S("mode")) == 1;
    const float amount = float(p.num(S("amount")));
    if (amount <= 0)
        return;
    const float cx = float(p.num(S("centerX"))) * w, cy = float(p.num(S("centerY"))) * h;
    const float radius = std::hypot(float(w), float(h)) * 0.5f;
    const float spread = spin ? amount * kPi / 180.0f * radius : amount * radius; // largest smear in pixels
    // Each pass averages the picture with two copies transformed by +-half the
    // remaining step, doubling the number of effective samples: 2^passes
    // samples spread evenly over the whole range for 2 x passes lookups.
    const int passes = std::clamp(int(std::ceil(std::log2(std::max(2.0f, spread / 1.5f)))), 1, 8);
    for (int k = 0; k < passes; ++k) {
        const float frac = 1.0f / float(2 << k); // 1/2, 1/4, ...
        float c = 1, s = 0, zoomIn = 1, zoomOut = 1;
        if (spin) {
            const float a = amount * kPi / 180.0f * frac * 0.5f;
            c = std::cos(a);
            s = std::sin(a);
        } else {
            zoomIn = std::pow(1.0f + amount, frac * 0.5f);
            zoomOut = 1.0f / zoomIn;
        }
        const QImage src = img;
        const Pixels in(src);
        forRows(img, [&](int y, uint32_t* row) {
            for (int x = 0; x < w; ++x) {
                const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
                uint32_t p0, p1;
                if (spin) {
                    p0 = in.sample(cx + dx * c - dy * s, cy + dx * s + dy * c, Edge::Clamp);
                    p1 = in.sample(cx + dx * c + dy * s, cy - dx * s + dy * c, Edge::Clamp);
                } else {
                    p0 = in.sample(cx + dx * zoomIn, cy + dy * zoomIn, Edge::Clamp);
                    p1 = in.sample(cx + dx * zoomOut, cy + dy * zoomOut, Edge::Clamp);
                }
                row[x] = (((p0 & 0x00ff00ffu) + (p1 & 0x00ff00ffu)) >> 1 & 0x00ff00ffu) | ((((p0 >> 8) & 0x00ff00ffu) + ((p1 >> 8) & 0x00ff00ffu)) << 7 & 0xff00ff00u);
            }
        });
    }
}

void melt(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width();
    const float amount = float(p.num(S("amount")) * ctx.scale);
    const float freq = float(p.num(S("frequency")));
    const float ph = float(p.num(S("speed")) * seconds(ctx));
    const bool up = p.choice(S("direction")) == 1;
    if (amount == 0)
        return;
    const QImage src = img;
    const Pixels in(src);
    warp(img, Edge::Clamp, [&](float x, float y, float& sx, float& sy) {
        const float l = luma(in.at(std::min(int(x), in.w - 1), std::min(int(y), in.h - 1)));
        const float wave = 0.5f + 0.5f * std::sin(2 * kPi * (x / w * freq + ph));
        sx = x;
        sy = y + (up ? 1.0f : -1.0f) * amount * l * wave;
        return true;
    });
}

void shake(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const double t = seconds(ctx) * p.num(S("speed"));
    const uint32_t seed = uint32_t(p.num(S("seed"))) * 7u + 3u;
    const float amt = float(p.num(S("amount")) * ctx.scale);
    const float dx = amt * noise1(t, seed), dy = amt * noise1(t, seed + 1);
    const float rot = float(p.num(S("rotation"))) * kPi / 180.0f * noise1(t, seed + 2);
    const float zoom = float(p.num(S("zoom")));
    const float cx = w * 0.5f, cy = h * 0.5f, c = std::cos(-rot), s = std::sin(-rot);
    warp(img, Edge::Mirror, [&](float x, float y, float& sx, float& sy) {
        const float ux = (x - cx - dx) / zoom, uy = (y - cy - dy) / zoom;
        sx = cx + ux * c - uy * s;
        sy = cy + ux * s + uy * c;
        return true;
    });
}

void trails(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    FrameHistory& st = stateOf<FrameHistory>(state);
    st.push(ctx, img, 1);
    if (st.prevOut.isNull() || st.prevOut.size() != img.size()) {
        st.out = img;
        return;
    }
    const float decay = float(p.num(S("decay")));
    const int mode = p.choice(S("mode"));
    const Pixels prev(st.prevOut);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t c = row[x], o = prev.at(x, y);
            const float oa = pa(o) * decay, orr = pr(o) * decay, og = pg(o) * decay, ob = pb(o) * decay;
            switch (mode) {
            case 0: // lighten: light painting
                row[x] = packf(std::max(float(pa(c)), oa), std::max(float(pr(c)), orr), std::max(float(pg(c)), og), std::max(float(pb(c)), ob));
                break;
            case 1: // blend: motion echo
                row[x] = packf(pa(c) * (1 - decay) + oa, pr(c) * (1 - decay) + orr, pg(c) * (1 - decay) + og, pb(c) * (1 - decay) + ob);
                break;
            case 2: // add: neon
                row[x] = packf(std::max(float(pa(c)), oa), pr(c) + orr * 0.5f, pg(c) + og * 0.5f, pb(c) + ob * 0.5f);
                break;
            default: { // darken: dark trails fading to the picture
                const float k = 1 - decay;
                row[x] = packf(float(pa(c)), std::min(float(pr(c)), pr(o) + (255 - pr(o)) * k), std::min(float(pg(c)), pg(o) + (255 - pg(o)) * k),
                               std::min(float(pb(c)), pb(o) + (255 - pb(o)) * k));
                break;
            }
            }
        }
    });
    st.out = img;
}

void videoFeedback(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    FrameHistory& st = stateOf<FrameHistory>(state);
    st.push(ctx, img, 1);
    if (st.prevOut.isNull() || st.prevOut.size() != img.size()) {
        st.out = img;
        return;
    }
    const int w = img.width(), h = img.height();
    const float zoom = float(p.num(S("zoom")));
    const float rot = float(p.num(S("rotation"))) * kPi / 180.0f;
    const float ox = float(p.num(S("offsetX")) * ctx.scale), oy = float(p.num(S("offsetY")) * ctx.scale);
    const float decay = float(p.num(S("decay")));
    const float hue = float(p.num(S("hueShift"))) * kPi / 180.0f;
    const float inMix = float(p.num(S("input")));
    const bool lighten = p.choice(S("mode")) == 1;
    const float ch = std::cos(hue), sh = std::sin(hue), c = std::cos(-rot), s = std::sin(-rot);
    const float cx = w * 0.5f, cy = h * 0.5f;
    const Pixels prev(st.prevOut);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float ux = (x + 0.5f - cx - ox) / zoom, uy = (y + 0.5f - cy - oy) / zoom;
            const uint32_t f = prev.sample(cx + ux * c - uy * s, cy + ux * s + uy * c, Edge::Transparent);
            // Hue-rotate the feedback (rotate chroma in YIQ).
            const Yiq q = toYiq(float(pr(f)), float(pg(f)), float(pb(f)));
            float fr, fg, fb;
            fromYiq(q.y, q.i * ch - q.q * sh, q.i * sh + q.q * ch, fr, fg, fb);
            const float fa = pa(f) * decay;
            fr *= decay, fg *= decay, fb *= decay;
            const uint32_t in = row[x];
            if (lighten)
                row[x] = packf(std::max(float(pa(in)), fa), std::max(float(pr(in)), fr), std::max(float(pg(in)), fg), std::max(float(pb(in)), fb));
            else
                row[x] = packf(fa + (pa(in) - fa) * inMix, fr + (pr(in) - fr) * inMix, fg + (pg(in) - fg) * inMix, fb + (pb(in) - fb) * inMix);
        }
    });
    st.out = img;
}

void slitScan(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    FrameHistory& st = stateOf<FrameHistory>(state);
    const int n = std::max(2, int(p.num(S("frames"))));
    st.push(ctx, img, size_t(n));
    const int dir = p.choice(S("direction"));
    const int w = img.width(), h = img.height();
    std::vector<Pixels> frames;
    for (const QImage& f : st.frames)
        frames.emplace_back(f);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            float f;
            switch (dir) {
            case 0: f = (y + 0.5f) / h; break;
            case 1: f = 1 - (y + 0.5f) / h; break;
            case 2: f = (x + 0.5f) / w; break;
            case 3: f = 1 - (x + 0.5f) / w; break;
            default: f = std::min(1.0f, std::hypot((x + 0.5f) / w - 0.5f, (y + 0.5f) / h - 0.5f) * 2.0f); break;
            }
            const size_t ago = std::min(frames.size() - 1, size_t(f * float(n - 1) + 0.5f));
            row[x] = frames[ago].at(x, y);
        }
    });
}

void psychedelicColors(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float base = float(p.num(S("speed")) * seconds(ctx));
    const float byLuma = float(p.num(S("lumaSpread")));
    const float byPos = float(p.num(S("positionSpread")));
    const float sat = float(p.num(S("saturation")));
    const int w = img.width();
    forRows(img, [&](int, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = row[x];
            const Yiq q = toYiq(float(pr(px)), float(pg(px)), float(pb(px)));
            const float a = (base + byLuma * q.y / 255.0f + byPos * float(x) / float(w)) * kPi / 180.0f;
            const float c = std::cos(a) * sat, s = std::sin(a) * sat;
            float r, g, b;
            fromYiq(q.y, q.i * c - q.q * s, q.i * s + q.q * c, r, g, b);
            row[x] = packf(float(pa(px)), r, g, b);
        }
    });
}

// Gradient maps (brightness -> colour).
struct Gradient {
    const char* name;
    std::vector<std::pair<float, uint32_t>> stops;
};
const std::vector<Gradient>& gradients()
{
    static const std::vector<Gradient> list = {
        {"Thermal", {{0.0f, 0x000010}, {0.25f, 0x3b0a8c}, {0.45f, 0xc4237a}, {0.65f, 0xf2712b}, {0.85f, 0xfadd3c}, {1.0f, 0xffffff}}},
        {"Night Vision", {{0.0f, 0x000800}, {0.5f, 0x1f9a2a}, {1.0f, 0xd8ffd0}}},
        {"Infrared", {{0.0f, 0x1a0026}, {0.35f, 0x6a1b9a}, {0.6f, 0xff4fa3}, {0.85f, 0xffd1e8}, {1.0f, 0xffffff}}},
        {"Acid", {{0.0f, 0x120024}, {0.2f, 0x00ff9c}, {0.4f, 0xfff200}, {0.6f, 0xff00c8}, {0.8f, 0x00c8ff}, {1.0f, 0xffffff}}},
        {"Rainbow", {{0.0f, 0xff0000}, {0.17f, 0xffa500}, {0.33f, 0xffff00}, {0.5f, 0x00ff00}, {0.67f, 0x00bfff}, {0.83f, 0x4b0082}, {1.0f, 0xee82ee}}},
        {"Ice", {{0.0f, 0x00020a}, {0.4f, 0x0b4f8a}, {0.75f, 0x7fd4ff}, {1.0f, 0xffffff}}},
        {"Gold", {{0.0f, 0x0d0700}, {0.4f, 0x7a4a05}, {0.75f, 0xe8b83a}, {1.0f, 0xfff6d8}}},
    };
    return list;
}

void gradientMap(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float mix = float(p.num(S("mix")));
    const std::array<uint32_t, 256> lut = gradientMapLut(p, ctx);
    forRows(img, [&](int, uint32_t* row) {
        for (int x = 0; x < img.width(); ++x) {
            const uint32_t px = row[x];
            const int a = pa(px);
            if (!a)
                continue;
            const int l = std::min(255, int(luma(px) * 255.0f * 255.0f / float(a)));
            const uint32_t c = lut[size_t(l)];
            const float k = float(a) / 255.0f;
            row[x] = packf(float(a), pr(px) + (float((c >> 16) & 0xff) * k - pr(px)) * mix, pg(px) + (float((c >> 8) & 0xff) * k - pg(px)) * mix,
                           pb(px) + (float(c & 0xff) * k - pb(px)) * mix);
        }
    });
}

struct AsciiState : EffectState {
    int cell = 0;
    int charset = -1;
    std::vector<QImage> glyphs; // Alpha8, ordered dark -> bright
};

const char* const kCharsets[] = {" .:-=+*#%@", " .'`^\",:;Il!i><~+_-?][}{1)(|/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$",
                                 " ░▒▓█", " 01"};

void asciiArt(QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    AsciiState& st = stateOf<AsciiState>(state);
    const int cell = std::max(3, int(std::lround(p.num(S("cellSize")) * ctx.scale)));
    const int charset = std::clamp(p.choice(S("charset")), 0, 3);
    if (st.cell != cell || st.charset != charset) {
        st.cell = cell;
        st.charset = charset;
        st.glyphs = asciiGlyphs(cell, charset);
    }
    const int mode = p.choice(S("colorMode"));
    const float bg = float(p.num(S("background")));
    const int w = img.width(), h = img.height();
    const int cw = (w + cell - 1) / cell;
    const QImage src = img;
    const Pixels in(src);
    const int ng = int(st.glyphs.size());
    forRows(img, [&](int y, uint32_t* row) {
        const int cy = y / cell, gy = y % cell;
        for (int cx = 0; cx < cw; ++cx) {
            // Average colour of the cell (sampled on a 3x3 grid).
            float r = 0, g = 0, b = 0, a = 0;
            for (int j = 0; j < 3; ++j)
                for (int i = 0; i < 3; ++i) {
                    const uint32_t px = in.at(std::min(w - 1, cx * cell + (i * 2 + 1) * cell / 6), std::min(h - 1, cy * cell + (j * 2 + 1) * cell / 6));
                    r += pr(px), g += pg(px), b += pb(px), a += pa(px);
                }
            r /= 9, g /= 9, b /= 9, a /= 9;
            const float l = (0.299f * r + 0.587f * g + 0.114f * b) / 255.0f;
            const QImage& glyph = st.glyphs[size_t(std::clamp(int(l * float(ng)), 0, ng - 1))];
            float fr, fg, fb;
            switch (mode) {
            case 1: fr = 40, fg = 255, fb = 90; break;    // green terminal
            case 2: fr = 255, fg = 176, fb = 40; break;   // amber terminal
            case 3: fr = fg = fb = 255; break;            // white
            default: {                                     // original colour, brightened
                const float mx = std::max({r, g, b, 1.0f});
                fr = r * 255 / mx, fg = g * 255 / mx, fb = b * 255 / mx;
                break;
            }
            }
            const uchar* gl = glyph.constScanLine(gy);
            for (int gx = 0; gx < cell; ++gx) {
                const int x = cx * cell + gx;
                if (x >= w)
                    break;
                const float k = gl[gx] / 255.0f;
                const uint32_t o = in.at(x, y);
                row[x] = packf(255, pr(o) * bg * (1 - k) + fr * k, pg(o) * bg * (1 - k) + fg * k, pb(o) * bg * (1 - k) + fb * k);
            }
        }
    });
}

void halftone(QImage& img, const Params& p, const RenderContext& ctx)
{
    const float cell = float(std::max(2.0, p.num(S("cellSize")) * ctx.scale));
    const float angle = float(p.num(S("angle"))) * kPi / 180.0f;
    const int mode = p.choice(S("mode"));
    const QColor ink = p.color(S("ink")), paper = p.color(S("paper"));
    const int w = img.width();
    const QImage src = img;
    const Pixels in(src);
    // Coverage (0..1) of a dot screen at angle a for a value v (0 = no ink).
    auto dot = [&](float x, float y, float a, auto value) {
        const float c = std::cos(a), s = std::sin(a);
        const float u = x * c + y * s, v = -x * s + y * c;
        const float cu = (std::floor(u / cell) + 0.5f) * cell, cv = (std::floor(v / cell) + 0.5f) * cell;
        const float sx = cu * c - cv * s, sy = cu * s + cv * c; // cell centre back in image space
        const float val = value(in.sample(sx, sy, Edge::Clamp));
        const float radius = cell * 0.70710678f * std::sqrt(std::clamp(val, 0.0f, 1.0f));
        const float d = std::hypot(u - cu, v - cv);
        return std::clamp(radius - d + 0.5f, 0.0f, 1.0f);
    };
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const int a = pa(in.at(x, y));
            if (mode == 0) {
                const float k = dot(px, py, angle, [](uint32_t c) { return 1.0f - luma(c) * 255.0f / std::max(1, pa(c)); });
                row[x] = packf(float(a), (paper.red() + (ink.red() - paper.red()) * k) * a / 255.0f,
                               (paper.green() + (ink.green() - paper.green()) * k) * a / 255.0f,
                               (paper.blue() + (ink.blue() - paper.blue()) * k) * a / 255.0f);
            } else if (mode == 1) {
                // CMYK screens at the classic angles.
                auto cmyk = [](uint32_t c, int ch) {
                    const float al = float(std::max(1, pa(c)));
                    const float r = pr(c) / al, g = pg(c) / al, b = pb(c) / al;
                    const float k = 1 - std::max({r, g, b});
                    if (ch == 3)
                        return k;
                    const float v = ch == 0 ? r : (ch == 1 ? g : b);
                    return k >= 0.999f ? 0.0f : (1 - v - k) / (1 - k);
                };
                const float cC = dot(px, py, angle + 15 * kPi / 180, [&](uint32_t c) { return cmyk(c, 0); });
                const float cM = dot(px, py, angle + 75 * kPi / 180, [&](uint32_t c) { return cmyk(c, 1); });
                const float cY = dot(px, py, angle, [&](uint32_t c) { return cmyk(c, 2); });
                const float cK = dot(px, py, angle + 45 * kPi / 180, [&](uint32_t c) { return cmyk(c, 3); });
                const float r = 255 * (1 - cC) * (1 - cK), g = 255 * (1 - cM) * (1 - cK), b = 255 * (1 - cY) * (1 - cK);
                row[x] = packf(float(a), r * a / 255.0f, g * a / 255.0f, b * a / 255.0f);
            } else {
                // RGB screen: additive dots on black, like a magnified display.
                auto chan = [](uint32_t c, int sh) { return float((c >> sh) & 0xff) / float(std::max(1, pa(c))); };
                const float r = dot(px, py, angle, [&](uint32_t c) { return chan(c, 16); });
                const float g = dot(px, py, angle + 30 * kPi / 180, [&](uint32_t c) { return chan(c, 8); });
                const float b = dot(px, py, angle + 60 * kPi / 180, [&](uint32_t c) { return chan(c, 0); });
                row[x] = packf(float(a), r * a, g * a, b * a);
            }
        }
    });
}

void neonEdges(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width(), h = img.height();
    const float strength = float(p.num(S("strength")));
    const int mode = p.choice(S("colorMode"));
    const QColor single = p.color(S("color"));
    const float bg = float(p.num(S("background")));
    const double glowR = p.num(S("glow")) * ctx.scale;
    const float hueT = float(seconds(ctx) * 0.1);
    std::vector<float> L(size_t(w) * size_t(h));
    {
        const Pixels in(img);
        parallelFor(h, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < w; ++x)
                    L[size_t(y) * size_t(w) + size_t(x)] = luma(in.at(x, y));
        });
    }
    auto at = [&](int x, int y) { return L[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; };
    QImage edges(img.size(), QImage::Format_ARGB32_Premultiplied);
    const QImage src = img;
    const Pixels in(src);
    forRows(edges, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float gx = at(x + 1, y - 1) + 2 * at(x + 1, y) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x - 1, y) - at(x - 1, y + 1);
            const float gy = at(x - 1, y + 1) + 2 * at(x, y + 1) + at(x + 1, y + 1) - at(x - 1, y - 1) - 2 * at(x, y - 1) - at(x + 1, y - 1);
            const float e = std::min(1.0f, std::sqrt(gx * gx + gy * gy) * strength);
            uint32_t c;
            if (mode == 1) {
                c = hsvColor(std::atan2(gy, gx) / (2 * kPi) + hueT, 1, 1);
            } else if (mode == 2) {
                c = single.rgb();
            } else {
                const uint32_t o = in.at(x, y);
                const float mx = float(std::max({pr(o), pg(o), pb(o), 1}));
                c = 0xff000000u | uint32_t(pr(o) * 255 / int(mx)) << 16 | uint32_t(pg(o) * 255 / int(mx)) << 8 | uint32_t(pb(o) * 255 / int(mx));
            }
            row[x] = packf(255 * e, pr(c) * e, pg(c) * e, pb(c) * e);
        }
    });
    QImage glow;
    if (glowR >= 0.5) {
        glow = edges.copy();
        imageops::gaussianBlur(glow, glowR, glowR);
    }
    const Pixels ed(edges), gl(glow.isNull() ? edges : glow);
    const bool hasGlow = !glow.isNull();
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const uint32_t o = row[x], e = ed.at(x, y), g = hasGlow ? gl.at(x, y) : 0u;
            const float k = hasGlow ? 1.5f : 0.0f;
            row[x] = packf(std::max(float(pa(o)), float(pa(e))), pr(o) * bg + pr(e) + pr(g) * k, pg(o) * bg + pg(e) + pg(g) * k,
                           pb(o) * bg + pb(e) + pb(g) * k);
        }
    });
}

void emboss(QImage& img, const Params& p, const RenderContext& ctx)
{
    const int w = img.width();
    const float a = float(p.num(S("angle"))) * kPi / 180.0f;
    const float d = float(std::max(1.0, ctx.scale * 1.5));
    const float dx = std::cos(a) * d, dy = std::sin(a) * d;
    const float strength = float(p.num(S("strength")));
    const float mix = float(p.num(S("mix")));
    const bool keepColor = p.flag(S("keepColor"));
    const QImage src = img;
    const Pixels in(src);
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float e = (luma(in.sample(px - dx, py - dy, Edge::Clamp)) - luma(in.sample(px + dx, py + dy, Edge::Clamp))) * strength;
            const uint32_t o = in.at(x, y);
            const float al = float(pa(o));
            float r, g, b;
            if (keepColor) {
                r = pr(o) + e * 255, g = pg(o) + e * 255, b = pb(o) + e * 255;
            } else {
                r = g = b = (0.5f + e) * al;
            }
            row[x] = packf(al, pr(o) + (r - pr(o)) * mix, pg(o) + (g - pg(o)) * mix, pb(o) + (b - pb(o)) * mix);
        }
    });
}

} // namespace

// ---------------------------------------------------------------------------
// Plans shared with the GPU shaders (GpuCompositor): everything random or
// table-driven is computed here once, so both renderers agree exactly.
// ---------------------------------------------------------------------------

namespace fx {

uint32_t glitchRateSeed(const Params& p, const RenderContext& ctx)
{
    return rateSeed(p, ctx);
}

float chromaAmount(const Params& p, const RenderContext& ctx)
{
    const double wobble = p.num(S("wobble"));
    const double k = wobble > 0 ? 1.0 + wobble * double(noise1(seconds(ctx) * p.num(S("wobbleSpeed")), 991u)) : 1.0;
    return float(p.num(S("amount")) * ctx.scale * k);
}

std::array<float, 6> rgbSplitOffsets(const Params& p, const RenderContext& ctx)
{
    const float s = float(ctx.scale);
    std::array<float, 6> o = {float(p.num(S("redX"))) * s, float(p.num(S("redY"))) * s, float(p.num(S("greenX"))) * s,
                              float(p.num(S("greenY"))) * s, float(p.num(S("blueX"))) * s, float(p.num(S("blueY"))) * s};
    const float jitter = float(p.num(S("jitter"))) * s;
    if (jitter > 0) {
        const uint32_t seed = rateSeed(p, ctx) + 23u;
        for (size_t i = 0; i < 6; ++i)
            o[i] += (hash01(uint32_t(i), 31, seed) - 0.5f) * 2.0f * jitter;
    }
    return o;
}

uint32_t pixelSortBreakSeed(const Params& p, const RenderContext& ctx)
{
    return (p.flag(S("animated")) ? frameIndex(ctx) * 13u + 7u : 7u) + uint32_t(p.num(S("seed"))) * 7919u;
}

float stretchPosition(const Params& p, const RenderContext& ctx)
{
    const double sweep = p.num(S("sweep"));
    const double pos = p.num(S("position")) + (sweep > 0 ? sweep * 0.5 * std::sin(2.0 * double(kPi) * seconds(ctx) * p.num(S("sweepSpeed"))) : 0.0);
    return float(std::clamp(pos, 0.0, 1.0));
}

uint32_t stretchSeed(const Params& p, const RenderContext& ctx)
{
    return rateSeed(p, ctx) + 29u;
}

bool stutterShowsNew(const Params& p, int64_t tick, int64_t lastTick, int64_t& frozenUntil)
{
    if (tick < lastTick || tick > lastTick + 100000) // a jump: no freeze carries over
        frozenUntil = std::numeric_limits<int64_t>::min();
    if (tick < frozenUntil)
        return false;
    const double chance = p.num(S("freeze"));
    const uint32_t seed = uint32_t(p.num(S("seed"))) * 7919u + 3u;
    frozenUntil = tick + 1;
    if (chance > 0 && hash01(uint32_t(tick), 31, seed) < chance) // this frame sticks for a while
        frozenUntil = tick + 1 + int64_t(hash32(uint32_t(tick), 32, seed) % uint32_t(std::max(1, int(p.num(S("maxFreeze"))))));
    return true;
}

StrobeNow strobeNow(const Params& p, const RenderContext& ctx)
{
    StrobeNow now;
    const double phase = seconds(ctx) * p.num(S("rate"));
    const double cycle = std::floor(phase);
    const uint32_t seed = uint32_t(p.num(S("seed"))) * 7919u + 11u;
    const double chance = p.num(S("chance"));
    now.on = phase - cycle < p.num(S("duty")) && (chance >= 1 || hash01(uint32_t(int64_t(cycle)), 41, seed) < chance);
    const QColor c = p.color(S("color"));
    now.rgb = (uint32_t(c.red()) << 16) | (uint32_t(c.green()) << 8) | uint32_t(c.blue());
    const int mode = p.choice(S("mode"));
    if (mode == 4)
        now.rgb = hash32(uint32_t(int64_t(cycle)), 42, seed) & 0xffffffu;
    else if (mode == 5)
        now.rgb = hsvColor(fract(float(cycle) * 0.618034f), 1.0f, 1.0f) & 0xffffffu;
    return now;
}

std::array<int, 3> timeSplitDelays(const Params& p)
{
    const int d = std::max(1, int(p.num(S("delay"))));
    const int curve = p.choice(S("curve"));
    const int last = curve == 1 ? d : (curve == 2 ? d * 4 : d * 2);
    // Which channel is now, one delay late, and last.
    static const int orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
    const int* o = orders[std::clamp(p.choice(S("order")), 0, 5)];
    std::array<int, 3> delays{};
    delays[size_t(o[0])] = 0;
    delays[size_t(o[1])] = d;
    delays[size_t(o[2])] = last;
    return delays;
}

int64_t datamoshCleanTick(const Params& p, const RenderContext& ctx)
{
    const double interval = p.num(S("autoClean"));
    return interval > 0 ? int64_t(std::floor(seconds(ctx) / interval + 1e-9)) : 0;
}

int jpegBlockSize(const Params& p, const RenderContext& ctx)
{
    const int mult = 1 << std::clamp(p.choice(S("blockScale")), 0, 2);
    return std::clamp(int(std::lround(8 * ctx.scale * mult)), 2, 32);
}

std::vector<ParamDef> glitchCommonParams()
{
    return {num("fxMix", "Mix", 0, 1, 1),
            choice("fxMask", "Apply to",
                   {S("Everything"), S("Inside box"), S("Outside box"), S("Bright areas"), S("Dark areas")}),
            num("fxThreshold", "Mask threshold", 0, 1, 0.5), num("fxFeather", "Mask softness", 0, 0.5, 0.05),
            num("fxBoxX", "Box left", 0, 1, 0.25), num("fxBoxY", "Box top", 0, 1, 0.25),
            num("fxBoxW", "Box width", 0, 1, 0.5), num("fxBoxH", "Box height", 0, 1, 0.5)};
}

PostMask postMask(const Params& p, QSize size)
{
    PostMask m;
    m.mix = float(std::clamp(p.num(S("fxMix")), 0.0, 1.0));
    m.mode = std::clamp(p.choice(S("fxMask")), 0, 4);
    const float w = float(size.width()), h = float(size.height());
    const float feather = float(p.num(S("fxFeather")));
    m.threshold = float(p.num(S("fxThreshold")));
    // Box softness in pixels (a fraction of the shorter side); luma softness as is.
    m.feather = m.mode == 1 || m.mode == 2 ? feather * std::min(w, h) : feather;
    m.x0 = float(p.num(S("fxBoxX"))) * w;
    m.y0 = float(p.num(S("fxBoxY"))) * h;
    m.x1 = m.x0 + float(p.num(S("fxBoxW"))) * w;
    m.y1 = m.y0 + float(p.num(S("fxBoxH"))) * h;
    return m;
}

void applyPostMask(QImage& out, const QImage& before, const PostMask& m)
{
    if (out.size() != before.size() || before.isNull())
        return;
    const Pixels in(before);
    const int w = out.width();
    forRows(out, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const uint32_t pa = in.at(x, y);
            const float t = postWeight(m, float(x) + 0.5f, float(y) + 0.5f, luma(pa));
            if (t >= 1.0f)
                continue;
            if (t <= 0.0f) {
                row[x] = pa;
                continue;
            }
            const uint32_t pb = row[x], k = uint32_t(t * 256.0f), s = 256 - k;
            const uint32_t rb = (((pa & 0x00ff00ffu) * s + (pb & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
            const uint32_t ag = ((((pa >> 8) & 0x00ff00ffu) * s + ((pb >> 8) & 0x00ff00ffu) * k)) & 0xff00ff00u;
            row[x] = rb | ag;
        }
    });
}

std::vector<RowPlan> sliceRows(const Params& p, const RenderContext& ctx, int h)
{
    const int slices = std::max(1, int(p.num(S("slices"))));
    const float amount = float(p.num(S("amount")));
    const float shift = float(p.num(S("maxShift")) * ctx.scale);
    const float colorGlitch = float(p.num(S("colorGlitch")));
    const uint32_t seed = rateSeed(p, ctx);
    // Uneven bands: random weights, normalised.
    std::vector<float> edges(size_t(slices) + 1, 0.0f);
    for (int i = 0; i < slices; ++i)
        edges[size_t(i) + 1] = edges[size_t(i)] + 0.25f + hash01(uint32_t(i), 11, seed);
    std::vector<RowPlan> rows(size_t(std::max(0, h)));
    for (int y = 0, band = 0; y < h; ++y) {
        const float pos = (y + 0.5f) / float(h) * edges.back();
        while (band + 1 < slices && pos > edges[size_t(band) + 1])
            ++band;
        if (hash01(uint32_t(band), 12, seed) < amount) {
            rows[size_t(y)].a = (hash01(uint32_t(band), 13, seed) - 0.5f) * 2.0f * shift;
            rows[size_t(y)].b = 1;
            if (hash01(uint32_t(band), 14, seed) < colorGlitch)
                rows[size_t(y)].c = float(1 + hash32(uint32_t(band), 15, seed) % 6);
        }
    }
    return rows;
}

std::vector<RowPlan> vhsRows(const Params& p, const RenderContext& ctx, int w, int h)
{
    const float s = float(ctx.scale);
    const float jitter = float(p.num(S("jitter"))) * s;
    const float tracking = float(p.num(S("tracking")));
    const float dropouts = float(p.num(S("dropouts")));
    const uint32_t frame = p.flag(S("animated")) ? frameIndex(ctx) : 0u;
    const double t = p.flag(S("animated")) ? seconds(ctx) : 0.0;
    const int headRows = int(h * 0.04f);
    std::vector<RowPlan> rows(size_t(std::max(0, h)));
    for (int y = 0; y < h; ++y) {
        RowPlan& r = rows[size_t(y)];
        // Time-base jitter: a slow wobble plus rare kicks.
        float off = jitter * noise1(y * 0.02 + t * 3.0, 41) * 0.6f;
        if (hash01(uint32_t(y / std::max(1, int(4 * s))), frame, 43) < 0.02f)
            off += jitter * (hash01(uint32_t(y), frame, 44) - 0.5f) * 6.0f;
        // Head-switching noise at the bottom of the frame.
        const bool head = tracking > 0 && y >= h - headRows;
        if (head) {
            const float k = float(y - (h - headRows)) / float(std::max(1, headRows));
            off += tracking * k * 40.0f * s * (0.6f + 0.4f * hash01(uint32_t(y), frame, 45));
        }
        r.a = off;
        r.b = head ? 1.0f : 0.0f;
        // Dropout: a short bright streak on this line.
        r.c = -1;
        r.d = 0;
        if (dropouts > 0 && hash01(uint32_t(y), frame, 46) < dropouts * 0.015f) {
            r.c = float(int(hash01(uint32_t(y), frame, 47) * w));
            r.d = float(int((0.02f + 0.15f * hash01(uint32_t(y), frame, 48)) * w));
        }
    }
    return rows;
}

std::vector<RowPlan> vhsTrackingRows(const Params& p, const RenderContext& ctx, int h)
{
    const float s = float(ctx.scale);
    const double t = seconds(ctx);
    const float center = fract(float(p.num(S("position")) + p.num(S("speed")) * t));
    const float half = float(p.num(S("height"))) * 0.5f;
    const float dist = float(p.num(S("distortion"))) * s;
    const uint32_t frame = frameIndex(ctx);
    std::vector<RowPlan> rows(size_t(std::max(0, h)));
    if (half <= 0)
        return rows;
    for (int y = 0; y < h; ++y) {
        float d = (y + 0.5f) / float(h) - center;
        d -= std::round(d); // wraps around the frame
        const float k = 1.0f - std::abs(d) / half;
        if (k <= 0)
            continue;
        const float wgt = k * k;
        rows[size_t(y)].a = wgt;
        rows[size_t(y)].b = dist * wgt * (std::sin(float(y) * 0.35f / std::max(0.3f, s) + float(t) * 25.0f) * 0.5f + hash01(uint32_t(y), frame, 51) - 0.5f);
    }
    return rows;
}

std::vector<RowPlan> tvStaticRows(const Params& p, const RenderContext& ctx, int h)
{
    const float bars = float(p.num(S("bars")));
    const float loss = float(p.num(S("signalLoss")));
    const double t = seconds(ctx);
    const uint32_t frame = p.flag(S("animated")) ? frameIndex(ctx) : 0u;
    const float barPos = fract(float(t * p.num(S("barSpeed"))));
    std::vector<RowPlan> rows(size_t(std::max(0, h)));
    for (int y = 0; y < h; ++y) {
        const float v = (y + 0.5f) / h;
        rows[size_t(y)].a = 1.0f - bars * 0.5f * (0.5f + 0.5f * std::cos(2 * kPi * (v - barPos)));
        rows[size_t(y)].b = (loss > 0 && hash01(uint32_t(y / std::max(1, h / 60)), frame, 61) < loss * 0.4f) ? 1.0f : 0.0f;
    }
    return rows;
}

FilmPlan oldFilmPlan(const Params& p, const RenderContext& ctx, int w, int h)
{
    FilmPlan plan;
    const float s = float(ctx.scale);
    const uint32_t frame = frameIndex(ctx);
    const double t = seconds(ctx);
    const float weave = float(p.num(S("weave"))) * s;
    plan.dx = weave * noise1(t * 6.0, 81);
    plan.dy = weave * noise1(t * 5.0, 82);
    plan.flicker = 1.0f + float(p.num(S("flicker"))) * (hash01(frame, 1, 83) - 0.5f) * 0.5f;
    plan.scratchWidth = std::max(1, int(s * 1.5f));
    const int scratches = int(std::lround(p.num(S("scratches")) * 6));
    std::vector<int> active;
    for (int i = 0; i < scratches; ++i)
        if (hash01(uint32_t(i), frame / 3, 85) >= 0.35f)
            active.push_back(i);
    plan.scratchX.resize(size_t(std::max(0, h)) * active.size());
    for (size_t k = 0; k < active.size(); ++k) {
        const int i = active[k];
        const float x0 = (0.5f + 0.5f * noise1(t * 0.7 + i * 13.1, 86 + uint32_t(i))) * w;
        plan.scratchDark.push_back(hash01(uint32_t(i), 0, 87) < 0.5f ? 1 : 0);
        for (int y = 0; y < h; ++y)
            plan.scratchX[size_t(y) * active.size() + k] = float(int(x0 + noise1(y * 0.01 + i, 88) * 3 * s));
    }
    const int specks = int(std::lround(p.num(S("dust")) * 40));
    for (int i = 0; i < specks; ++i)
        plan.specks.push_back({hash01(uint32_t(i), frame, 89) * w, hash01(uint32_t(i), frame, 90) * h, (0.7f + hash01(uint32_t(i), frame, 91) * 3.0f) * s,
                               hash01(uint32_t(i), frame, 92) < 0.7f ? 1.0f : 0.0f});
    return plan;
}

const int* jpegQuantTable(bool chroma)
{
    return chroma ? kJpegChroma : kJpegLuma;
}



std::array<uint32_t, 256> gradientMapLut(const Params& p, const RenderContext& ctx)
{
    const auto& g = gradients()[size_t(std::clamp(p.choice(S("palette")), 0, int(gradients().size()) - 1))].stops;
    const float shift = float(p.num(S("cycle")) * seconds(ctx));
    const bool cycling = p.num(S("cycle")) != 0;
    std::array<uint32_t, 256> lut{};
    for (int i = 0; i < 256; ++i) {
        float t = float(i) / 255.0f;
        if (cycling)
            t = fract(t + shift);
        size_t k = 0;
        while (k + 2 < g.size() && t > g[k + 1].first)
            ++k;
        const auto& a = g[k];
        const auto& b = g[k + 1];
        const float f = std::clamp((t - a.first) / std::max(1e-4f, b.first - a.first), 0.0f, 1.0f);
        auto ch = [&](int shiftBits) { return int(float((a.second >> shiftBits) & 0xff) * (1 - f) + float((b.second >> shiftBits) & 0xff) * f + 0.5f); };
        lut[size_t(i)] = uint32_t(ch(16) << 16 | ch(8) << 8 | ch(0));
    }
    return lut;
}

std::vector<QImage> asciiGlyphs(int cell, int charset)
{
    const QString chars = QString::fromUtf8(kCharsets[std::clamp(charset, 0, 3)]);
    QFont font(QStringLiteral("Consolas"));
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(std::max(3, int(cell * 1.15)));
    std::vector<std::pair<int, QImage>> measured;
    for (const QChar c : chars) {
        QImage g(cell, cell, QImage::Format_Alpha8);
        g.fill(0);
        QPainter painter(&g);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(0, 0, cell, cell), Qt::AlignCenter, QString(c));
        painter.end();
        int cover = 0;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x)
                cover += g.constScanLine(y)[x];
        measured.emplace_back(cover, g);
    }
    std::stable_sort(measured.begin(), measured.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<QImage> out;
    for (auto& m : measured)
        out.push_back(m.second);
    return out;
}

} // namespace fx

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

void addGlitchEffects(std::vector<EffectDef>& list)
{
    using St = std::unique_ptr<EffectState>;
    auto add = [&](const char* id, const char* name, const char* category, const char* description, std::vector<ParamDef> params,
                   std::function<void(QImage&, const Params&, const RenderContext&, St&)> apply) {
        EffectDef d;
        d.id = QString::fromLatin1(id);
        d.name = QString::fromUtf8(name);
        d.category = QString::fromUtf8(category);
        d.description = QString::fromUtf8(description);
        d.params = std::move(params);
        d.apply = std::move(apply);
        if (d.category == QLatin1String("Glitch")) { // shared Mix and "Apply to" mask
            for (ParamDef& c : glitchCommonParams())
                d.params.push_back(std::move(c));
            d.glitchPost = true;
        }
        list.push_back(std::move(d));
    };
    // Stateless effects ignore the state argument.
    auto pure = [](void (*fn)(QImage&, const Params&, const RenderContext&)) {
        return [fn](QImage& img, const Params& p, const RenderContext& ctx, St&) { fn(img, p, ctx); };
    };
    auto simple = [](void (*fn)(QImage&, const Params&)) {
        return [fn](QImage& img, const Params& p, const RenderContext&, St&) { fn(img, p); };
    };
    const char* G = "Glitch";
    const char* A = "Analog & Retro";
    const char* P = "Psychedelic & Distort";
    const char* Y = "Stylize";
    const QStringList dirs4 = {QStringLiteral("Right"), QStringLiteral("Left"), QStringLiteral("Down"), QStringLiteral("Up")};

    // ---- Glitch (every one also gets glitchCommonParams(): Mix and the "Apply to" mask)
    const QStringList colorModes = {S("Random"), S("Swap red / blue"), S("Invert"), S("Red only"), S("Cyan"), S("Crushed"), S("XOR")};
    add("chromatic_aberration", "Chromatic Aberration", G,
        "Red and blue split apart like a cheap lens (radial, around a centre you choose) or a misaligned signal (linear). "
        "Falloff shapes how fast the split grows towards the edges; Green shift moves green too; Wobble makes it breathe.",
        {num("amount", "Amount", -60, 60, 8, 1, " px"), choice("mode", "Mode", {S("Radial (lens)"), S("Linear")}),
         num("angle", "Angle", -180, 180, 0, 0, "°"), num("centerX", "Centre X", 0, 1, 0.5), num("centerY", "Centre Y", 0, 1, 0.5),
         num("falloff", "Falloff", 0.25, 4, 1), num("greenShift", "Green shift", -1, 1, 0), num("wobble", "Wobble", 0, 1, 0),
         num("wobbleSpeed", "Wobble speed", 0, 20, 2, 1, " Hz")},
        pure(chromaticAberration));
    add("rgb_split", "RGB Split", G, "Shifts the red, green and blue channels independently; Jitter throws them around at random.",
        {num("redX", "Red X", -200, 200, -12, 1, " px"), num("redY", "Red Y", -200, 200, 0, 1, " px"), num("greenX", "Green X", -200, 200, 0, 1, " px"),
         num("greenY", "Green Y", -200, 200, 0, 1, " px"), num("blueX", "Blue X", -200, 200, 12, 1, " px"), num("blueY", "Blue Y", -200, 200, 0, 1, " px"),
         num("jitter", "Jitter", 0, 200, 0, 1, " px"), num("rate", "Jitter changes per second", 0, 60, 12, 1), num("seed", "Seed", 0, 1000, 1, 0),
         choice("edge", "Edges", {S("Stretch"), S("Wrap around"), S("Mirror"), S("Black")})},
        pure(rgbSplit));
    add("pixel_sort", "Pixel Sort", G,
        "Sorts runs of pixels whose brightness (or hue, or saturation) is between the thresholds (Asendorf-style), along rows or "
        "columns. Random breaks chop the runs into streaks of different lengths.",
        {num("lower", "Lower threshold", 0, 1, 0.25), num("upper", "Upper threshold", 0, 1, 0.8),
         choice("sortBy", "Sort by", {S("Brightness"), S("Hue"), S("Saturation"), S("Red"), S("Green"), S("Blue")}),
         choice("direction", "Direction", {S("Horizontal"), S("Vertical")}), flag("reverse", "Reverse order", false),
         flag("invert", "Sort outside the range", false), num("maxLength", "Max run length (0 = any)", 0, 4000, 0, 0, " px"),
         choice("thresholdBy", "Threshold on", {S("Brightness"), S("Hue"), S("Saturation")}), num("breaks", "Random breaks", 0, 0.2, 0, 3),
         num("seed", "Seed", 0, 1000, 1, 0), flag("animated", "New breaks every frame", false)},
        pure(pixelSort));
    add("datamosh", "Datamosh", G,
        "Fakes deleted keyframes: the picture is pushed around by the motion of the footage instead of being refreshed, so "
        "images melt into each other. Bloom repeats the same motion. Motion scales how far things are pushed. Keyframe "
        "\"Clean frame\" on, or set Auto clean, to restart from a clean picture.",
        {num("amount", "Melt", 0, 1, 0.95), num("blockSize", "Block size", 4, 64, 16, 0, " px"), num("search", "Motion search", 4, 64, 16, 0, " px"),
         choice("mode", "Mode", {S("Melt (no keyframes)"), S("Bloom (repeat motion)")}), flag("refresh", "Clean frame", false),
         num("motionScale", "Motion", 0, 4, 1), num("autoClean", "Auto clean every (0 = never)", 0, 30, 0, 1, " s")},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { datamosh(img, p, ctx, st); });
    add("block_glitch", "Digital Corruption (Blocks)", G,
        "Random blocks of the picture jump around and change colour or turn to static, like a corrupt stream.",
        {num("amount", "Amount", 0, 1, 0.25), num("blockSize", "Block size", 4, 400, 40, 0, " px"), num("maxShift", "Max shift", 0, 2000, 160, 0, " px"),
         num("colorGlitch", "Colour glitches", 0, 1, 0.4), num("rate", "Changes per second", 0, 60, 12, 1), num("seed", "Seed", 0, 1000, 1, 0),
         num("aspect", "Block shape (width / height)", 0.25, 8, 2), num("verticalShift", "Vertical shift", 0, 1, 0.5),
         choice("colorMode", "Colour glitch", colorModes), num("noise", "Static blocks", 0, 1, 0)},
        pure(blockGlitch));
    add("slice_glitch", "Slice Displacement", G,
        "Slices of the picture tear sideways (or up and down), optionally with RGB fringes and colour glitches.",
        {num("slices", "Slices", 1, 200, 20, 0), num("amount", "Amount", 0, 1, 0.45), num("maxShift", "Max shift", 0, 2000, 120, 0, " px"),
         num("rgbSplit", "RGB split", 0, 100, 6, 1, " px"), num("rate", "Changes per second", 0, 60, 10, 1), num("seed", "Seed", 0, 1000, 1, 0),
         choice("direction", "Slices", {S("Horizontal"), S("Vertical")}), choice("edge", "Edges", {S("Wrap around"), S("Stretch"), S("Black")}),
         num("colorGlitch", "Colour glitches", 0, 1, 0)},
        pure(sliceGlitch));
    add("jpeg_glitch", "Compression Artifacts", G,
        "Real JPEG-style DCT compression: blocky macroblocks, ringing and smeared colour. Corruption smashes coefficients in random "
        "blocks: any of them, only the block colour (DC) or only the detail. Colour can be compressed harder than brightness.",
        {num("quality", "Quality", 1, 100, 8, 0), num("corruption", "Corruption", 0, 1, 0.0), flag("subsample", "Chroma subsampling", true),
         flag("animated", "New corruption every frame", true), num("chromaQuality", "Colour quality (0 = same)", 0, 100, 0, 0),
         choice("blockScale", "Block size", {S("8 px"), S("16 px"), S("32 px")}),
         choice("corruptWhat", "Corrupt", {S("Any coefficient"), S("Block colour (DC)"), S("Detail (AC)")})},
        pure(compressionArtifacts));
    add("databend", "Databend", G,
        "Edits the raw colour bytes like audio or a hex file: echo, reversed chunks, rotated bits, XOR noise, a byte shift, a wrong "
        "line length (stride) or repeated chunks. Colours bleed across channels.",
        {choice("mode", "Mode", {S("Echo"), S("Reverse chunks"), S("Rotate bits"), S("XOR noise"), S("Byte shift"), S("Wrong stride"),
                                 S("Repeat chunks")}),
         num("delay", "Echo delay / shift", 1, 200000, 3000, 0, " bytes"), num("feedback", "Feedback / amount", 0, 0.95, 0.5),
         num("chunk", "Chunk size", 3, 200000, 1500, 0, " bytes"), num("bits", "Bits", 1, 7, 2, 0), num("mix", "Bend amount", 0, 1, 1),
         flag("animated", "Animated", true), num("stride", "Stride error", -2000, 2000, 6, 0, " bytes")},
        pure(databend));
    {
        EffectDef d;
        d.id = QStringLiteral("bitcrush");
        d.name = QStringLiteral("Bitcrush / Overflow");
        d.category = QString::fromUtf8(G);
        d.description = QStringLiteral("Fewer bits per colour; with overflow on, bright colours wrap around to dark like an integer overflow "
                                       "(or fold back). Channels can get different bit depths.");
        d.params = {num("bits", "Bits per channel", 1, 8, 3, 0), num("gain", "Gain", 0.25, 8, 1.6), flag("wrap", "Overflow wraps", true),
                    flag("fold", "Fold instead of wrap", false), num("offset", "Offset", -1, 1, 0), flag("separate", "Separate channel bits", false),
                    num("bitsG", "Green bits", 1, 8, 3, 0), num("bitsB", "Blue bits", 1, 8, 2, 0)};
        d.luts = [](const Params& p, ChannelLuts& l) {
            const double gain = p.num(QStringLiteral("gain")), offset = p.num(QStringLiteral("offset")) * 255.0;
            const bool wrap = p.flag(QStringLiteral("wrap")), fold = p.flag(QStringLiteral("fold"));
            const bool separate = p.flag(QStringLiteral("separate"));
            for (int ch = 0; ch < 3; ++ch) {
                const char* bitsId = !separate || ch == 0 ? "bits" : (ch == 1 ? "bitsG" : "bitsB");
                const int bits = std::clamp(int(p.num(QString::fromLatin1(bitsId))), 1, 8);
                const int levels = (1 << bits) - 1;
                for (int i = 0; i < 256; ++i) {
                    int v = int((i + offset) * gain);
                    if (wrap && fold) { // overflow folds back down (a triangle wave)
                        const int m = ((v % 510) + 510) % 510;
                        v = m > 255 ? 510 - m : m;
                    } else if (wrap) {
                        v = ((v % 256) + 256) % 256;
                    } else {
                        v = std::clamp(v, 0, 255);
                    }
                    l[size_t(ch)][size_t(i)] = uint8_t(std::lround(std::round(v / 255.0 * levels) / levels * 255.0));
                }
            }
            return true;
        };
        for (ParamDef& c : glitchCommonParams())
            d.params.push_back(std::move(c));
        d.glitchPost = true;
        list.push_back(std::move(d));
    }
    add("pixel_stretch", "Pixel Stretch", G,
        "Smears one row or column of pixels towards the edge of the frame. Length stops the smear early, Sweep moves the line "
        "back and forth, Jitter tears it per line.",
        {choice("direction", "Direction", dirs4), num("position", "Position", 0, 1, 0.5, 3), num("length", "Length", 0, 1, 1),
         num("sweep", "Sweep", 0, 1, 0), num("sweepSpeed", "Sweep speed", 0, 10, 0.5, 2, " Hz"), num("jitter", "Jitter", 0, 500, 0, 0, " px"),
         num("rate", "Jitter changes per second", 0, 60, 10, 1), num("seed", "Seed", 0, 1000, 1, 0)},
        pure(pixelStretch));
    {
        EffectDef d;
        d.id = QStringLiteral("channel_swap");
        d.name = QStringLiteral("Channel Swap");
        d.category = QString::fromUtf8(G);
        d.description = QStringLiteral("Rearranges the red, green and blue channels, and can invert any of them.");
        d.params = {choice("order", "Order", {S("GBR"), S("BRG"), S("BGR"), S("RBG"), S("GRB"), S("RGB (unchanged)")}),
                    flag("invertR", "Invert red", false), flag("invertG", "Invert green", false), flag("invertB", "Invert blue", false)};
        d.colorMatrix = [](const Params& p, ColorMatrix& m) {
            static const int orders[6][3] = {{1, 2, 0}, {2, 0, 1}, {2, 1, 0}, {0, 2, 1}, {1, 0, 2}, {0, 1, 2}};
            const int* o = orders[std::clamp(p.choice(QStringLiteral("order")), 0, 5)];
            const bool inv[3] = {p.flag(QStringLiteral("invertR")), p.flag(QStringLiteral("invertG")), p.flag(QStringLiteral("invertB"))};
            m.fill(0);
            for (int row = 0; row < 3; ++row) {
                m[size_t(row * 4 + o[row])] = inv[row] ? -1.0f : 1.0f;
                m[size_t(row * 4 + 3)] = inv[row] ? 1.0f : 0.0f;
            }
            return true;
        };
        for (ParamDef& c : glitchCommonParams())
            d.params.push_back(std::move(c));
        d.glitchPost = true;
        list.push_back(std::move(d));
    }
    add("time_split", "RGB Time Split", G,
        "One channel shows now, the others lag behind by a few frames: moving things leave colour ghosts. Choose which channel "
        "lags most and how far apart the delays are.",
        {num("delay", "Delay", 1, 30, 4, 0, " frames"),
         choice("order", "Now / later / latest", {S("Red / green / blue"), S("Red / blue / green"), S("Green / red / blue"),
                                                  S("Green / blue / red"), S("Blue / red / green"), S("Blue / green / red")}),
         choice("curve", "Delays", {S("1x and 2x"), S("Both 1x"), S("1x and 4x")})},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { timeSplit(img, p, ctx, st); });
    add("stutter", "Frame Rate Stutter", G,
        "Holds frames to play at a lower frame rate (posterize time); Freeze makes random frames stick for longer.",
        {num("fps", "Frame rate", 1, 30, 8, 1, " fps"), num("freeze", "Freeze chance", 0, 1, 0), num("maxFreeze", "Longest freeze", 1, 30, 8, 0, " frames"),
         num("seed", "Seed", 0, 1000, 1, 0)},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { stutter(img, p, ctx, st); });
    add("strobe", "Strobe", G,
        "Flashes a colour (fixed, random or rainbow), inverts, blacks out, goes black & white or hides the picture at a steady "
        "rate. Chance skips flashes at random; Opacity makes them partial.",
        {num("rate", "Rate", 0.5, 30, 6, 1, " Hz"), num("duty", "On time", 0.05, 0.95, 0.3),
         choice("mode", "Mode", {S("Flash colour"), S("Invert"), S("Black"), S("Hide"), S("Random colour"), S("Rainbow"), S("Black & white")}),
         color("color", "Colour", 0xffffff), num("opacity", "Opacity", 0, 1, 1), num("chance", "Chance", 0, 1, 1), num("seed", "Seed", 0, 1000, 1, 0)},
        pure(strobe));

    // ---- Analog & retro
    add("vhs", "VHS", A,
        "Consumer VHS look: soft picture, colour bleeding sideways, time-base wobble, tape noise, dropouts and head-switching noise at the bottom.",
        {num("blur", "Softness", 0, 10, 1.5, 1, " px"), num("bleed", "Colour bleed", 0, 40, 8, 1, " px"), num("chromaShift", "Colour shift", -20, 20, 4, 1, " px"),
         num("saturation", "Saturation", 0, 2, 0.85), num("noise", "Noise", 0, 1, 0.15), num("jitter", "Jitter", 0, 20, 2, 1, " px"),
         num("tracking", "Head-switching noise", 0, 1, 0.6), num("dropouts", "Dropouts", 0, 1, 0.3), flag("animated", "Animated", true)},
        pure(vhs));
    add("vhs_tracking", "VHS Tracking Error", A, "A band of torn, noisy picture rolling through the frame, like a badly tracked tape.",
        {num("position", "Position", 0, 1, 0.7, 3), num("speed", "Roll speed", -2, 2, 0.25, 2), num("height", "Band height", 0, 1, 0.15),
         num("distortion", "Distortion", 0, 300, 50, 0, " px"), num("noise", "Noise", 0, 1, 0.6)},
        pure(vhsTracking));
    add("crt", "CRT Monitor", A, "Old TV / arcade monitor: curved glass, scanlines, a phosphor mask and dark corners.",
        {num("scanlines", "Scanlines", 0, 1, 0.5), num("lineCount", "Line count", 60, 1200, 270, 0),
         choice("mask", "Phosphor mask", {QStringLiteral("None"), QStringLiteral("Aperture grille"), QStringLiteral("Shadow mask"), QStringLiteral("Slot mask")}, 1),
         num("maskStrength", "Mask strength", 0, 1, 0.3), num("curvature", "Curvature", 0, 0.5, 0.12), num("vignette", "Vignette", 0, 1, 0.35),
         num("brightness", "Brightness", 0.5, 2, 1.25)},
        pure(crt));
    add("ntsc", "NTSC Composite", A,
        "Encodes the picture onto an NTSC colour subcarrier and decodes it again: dot crawl, rainbow fringes on fine detail and blurred colour.",
        {num("strength", "Strength", 0, 1, 1), num("bleed", "Colour bleed", 0, 40, 6, 1, " px"), num("ringing", "Ringing", 0, 1, 0.3),
         flag("dotCrawl", "Dot crawl (animated)", true)},
        pure(ntsc));
    add("tv_static", "TV Static", A, "Analog snow, rolling hum bars and bands of lost signal.",
        {num("amount", "Amount", 0, 1, 0.3), flag("colorNoise", "Colour noise", false), num("bars", "Hum bars", 0, 1, 0.3),
         num("barSpeed", "Bar speed", -2, 2, 0.4), num("signalLoss", "Signal loss", 0, 1, 0.15), flag("animated", "Animated", true)},
        pure(tvStatic));
    add("signal_jitter", "Sync Wobble / Rolling", A, "Lines wobble sideways (horizontal sync) and the picture can roll vertically (V-hold).",
        {num("amplitude", "Wobble", 0, 200, 8, 1, " px"), num("frequency", "Waves", 0, 30, 3, 1), num("speed", "Speed", -10, 10, 1, 2, " Hz"),
         num("noise", "Line noise", 0, 100, 3, 1, " px"), num("roll", "Vertical roll", -4, 4, 0, 2, " /s")},
        pure(signalJitter));
    add("interlace", "Interlacing", A, "Interlaced video artefacts: combing on motion, shifted fields or doubled lines.",
        {choice("mode", "Mode", {QStringLiteral("Combing (previous field)"), QStringLiteral("Shifted field"), QStringLiteral("Line doubling")}),
         num("shift", "Field shift", -40, 40, 6, 1, " px")},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { interlace(img, p, ctx, st); });
    add("old_film", "Old Film", A, "Projected film: scratches, dust, flicker, gate weave, grain, sepia and vignetting.",
        {num("scratches", "Scratches", 0, 1, 0.5), num("dust", "Dust", 0, 1, 0.5), num("flicker", "Flicker", 0, 1, 0.4),
         num("weave", "Gate weave", 0, 10, 1.5, 1, " px"), num("grain", "Grain", 0, 1, 0.25), num("sepia", "Sepia", 0, 1, 0.7),
         num("vignette", "Vignette", 0, 1, 0.5)},
        pure(oldFilm));
    add("ghosting", "Ghosting", A, "Faint repeated copies to the side, like multipath reception on an aerial.",
        {num("offset", "Offset", -300, 300, 30, 0, " px"), num("strength", "Strength", 0, 1, 0.4), num("echoes", "Echoes", 1, 5, 2, 0)},
        pure(ghosting));
    add("dither", "Dither / Retro Palette", A,
        "Reduces the picture to a palette (computers, consoles, pixel art, colour depths, or colours picked from the picture) with "
        "ordered, halftone, noise, pattern or error-diffusion dithering.",
        {choice("palette", "Palette", dither::paletteNames(), 1),
         choice("method", "Dithering", dither::methodNames(), 2),
         num("pixelSize", "Pixel size", 1, 32, 3, 0, " px"),
         num("strength", "Strength", 0, 2, 1, 2),
         choice("match", "Match colours", {QStringLiteral("By weighted RGB"), QStringLiteral("Perceptually (OKLab)")}, 0),
         flag("animate", "Animate noise", false)},
        pure(dither::apply));

    // ---- Psychedelic & distort
    add("kaleidoscope", "Kaleidoscope", P, "Mirrored wedges around a centre; spin animates it.",
        {num("segments", "Segments", 2, 32, 6, 0), num("rotation", "Rotation", -360, 360, 0, 1, "°"), num("spin", "Spin", -360, 360, 0, 1, "°/s"),
         num("zoom", "Zoom", 0.1, 4, 1, 2), num("centerX", "Centre X", 0, 1, 0.5, 3), num("centerY", "Centre Y", 0, 1, 0.5, 3)},
        pure(kaleidoscope));
    add("mirror_symmetry", "Mirror Symmetry", P, "Reflects one half (or quarter) of the picture onto the other.",
        {choice("mode", "Mode", {QStringLiteral("Left onto right"), QStringLiteral("Right onto left"), QStringLiteral("Top onto bottom"),
                                 QStringLiteral("Bottom onto top"), QStringLiteral("Four-way")})},
        simple(mirrorSymmetry));
    add("wave_warp", "Wave Warp", P, "Sine-wave distortion that can flow over time.",
        {choice("direction", "Direction", {QStringLiteral("Horizontal"), QStringLiteral("Vertical"), QStringLiteral("Both")}),
         num("amplitude", "Amplitude", -300, 300, 20, 1, " px"), num("wavelength", "Wavelength", 2, 3000, 240, 0, " px"),
         num("speed", "Speed", -10, 10, 0.5, 2, " Hz")},
        pure(waveWarp));
    add("twirl", "Twirl", P, "Swirls the picture around a centre, strongest in the middle.",
        {num("angle", "Angle", -1440, 1440, 180, 0, "°"), num("radius", "Radius", 0, 3, 0.9), num("centerX", "Centre X", 0, 1, 0.5, 3),
         num("centerY", "Centre Y", 0, 1, 0.5, 3)},
        simple(twirl));
    add("ripple", "Ripple", P, "Circular ripples spreading from a point, like a drop in water.",
        {num("amplitude", "Amplitude", -100, 100, 10, 1, " px"), num("wavelength", "Wavelength", 2, 800, 70, 0, " px"),
         num("speed", "Speed", -10, 10, 1.5, 2, " Hz"), num("decay", "Fade with distance", 0, 2, 0.4), num("centerX", "Centre X", 0, 1, 0.5, 3),
         num("centerY", "Centre Y", 0, 1, 0.5, 3)},
        pure(ripple));
    add("lens_distortion", "Lens Distortion / Fisheye", P, "Barrel (fisheye) or pincushion distortion.",
        {num("amount", "Amount", -1, 2, 0.5), num("zoom", "Zoom", 0.3, 3, 1, 2)}, simple(lensDistortion));
    add("polar", "Polar Coordinates", P, "Wraps the picture around its centre into a circle, or unwraps a circle into a strip.",
        {choice("mode", "Mode", {QStringLiteral("Rectangular to polar"), QStringLiteral("Polar to rectangular")})}, simple(polarCoordinates));
    add("tile", "Tile / Repeat", P, "Repeats the picture in a grid, optionally mirrored and scrolling.",
        {num("tilesX", "Across", 1, 16, 3, 1), num("tilesY", "Down", 1, 16, 3, 1), flag("mirror", "Mirror alternate tiles", true),
         num("scrollX", "Scroll X", -5, 5, 0, 2, " /s"), num("scrollY", "Scroll Y", -5, 5, 0, 2, " /s")},
        pure(tile));
    add("radial_blur", "Zoom / Spin Blur", P, "Blurs towards or around a centre point for a warp-speed or spinning look.",
        {choice("mode", "Mode", {QStringLiteral("Zoom"), QStringLiteral("Spin")}), num("amount", "Amount", 0, 1, 0.15, 3),
         num("centerX", "Centre X", 0, 1, 0.5, 3), num("centerY", "Centre Y", 0, 1, 0.5, 3)},
        simple(radialBlur));
    add("melt", "Melt", P, "Bright parts of the picture drip and run in waves.",
        {num("amount", "Amount", -400, 400, 60, 0, " px"), num("frequency", "Drips", 0, 60, 10, 1), num("speed", "Speed", -5, 5, 0.5, 2, " Hz"),
         choice("direction", "Direction", {QStringLiteral("Down"), QStringLiteral("Up")})},
        pure(melt));
    add("shake", "Camera Shake", P, "Smooth random handheld shake, with a little zoom to hide the edges.",
        {num("amount", "Amount", 0, 300, 15, 1, " px"), num("rotation", "Rotation", 0, 30, 1, 1, "°"), num("speed", "Speed", 0.1, 30, 6, 1, " Hz"),
         num("zoom", "Zoom", 1, 1.5, 1.05, 3), num("seed", "Seed", 0, 1000, 1, 0)},
        pure(shake));
    add("trails", "Trails / Echo", P, "Each frame leaves a fading trail: light painting, motion echo, neon or dark trails.",
        {num("decay", "Persistence", 0, 0.99, 0.85), choice("mode", "Mode", {QStringLiteral("Lighten (light painting)"), QStringLiteral("Blend (echo)"),
                                                                              QStringLiteral("Add (neon)"), QStringLiteral("Darken")})},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { trails(img, p, ctx, st); });
    add("video_feedback", "Video Feedback", P,
        "The previous output is zoomed, rotated and hue-shifted underneath the picture, like pointing a camera at its own monitor: infinite tunnels.",
        {num("zoom", "Zoom", 0.5, 1.5, 1.04, 3), num("rotation", "Rotation", -30, 30, 2, 1, "°"), num("offsetX", "Drift X", -50, 50, 0, 1, " px"),
         num("offsetY", "Drift Y", -50, 50, 0, 1, " px"), num("decay", "Persistence", 0, 1, 0.9), num("hueShift", "Hue shift", -60, 60, 8, 1, "°"),
         num("input", "New picture", 0, 1, 0.5), choice("mode", "Mode", {QStringLiteral("Blend"), QStringLiteral("Lighten")}, 1)},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { videoFeedback(img, p, ctx, st); });
    add("slit_scan", "Slit Scan / Time Displacement", P,
        "Each line comes from a different moment: moving things stretch and bend through time.",
        {num("frames", "Time span", 2, 60, 30, 0, " frames"),
         choice("direction", "Direction", {QStringLiteral("Top to bottom"), QStringLiteral("Bottom to top"), QStringLiteral("Left to right"),
                                           QStringLiteral("Right to left"), QStringLiteral("Centre outwards")})},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { slitScan(img, p, ctx, st); });
    add("psychedelic", "Psychedelic Colors", P, "Cycling rainbow hues that shift with brightness and position.",
        {num("speed", "Cycle speed", -720, 720, 90, 0, "°/s"), num("lumaSpread", "By brightness", 0, 1080, 360, 0, "°"),
         num("positionSpread", "Across the frame", 0, 1080, 0, 0, "°"), num("saturation", "Saturation", 0, 4, 1.6)},
        pure(psychedelicColors));
    add("gradient_map", "Thermal / Gradient Map", P, "Maps brightness to a colour gradient: thermal camera, night vision, infrared, acid...",
        {choice("palette", "Gradient",
                [] {
                    QStringList n;
                    for (const auto& g : gradients())
                        n << QString::fromUtf8(g.name);
                    return n;
                }()),
         num("mix", "Mix", 0, 1, 1), num("cycle", "Cycle", -2, 2, 0, 2, " /s")},
        pure(gradientMap));
    {
        EffectDef d;
        d.id = QStringLiteral("solarize");
        d.name = QStringLiteral("Solarize");
        d.category = QString::fromUtf8(P);
        d.description = QStringLiteral("Tones above the threshold are inverted (Sabattier effect).");
        d.params = {num("threshold", "Threshold", 0, 1, 0.5)};
        d.luts = [](const Params& p, ChannelLuts& l) {
            const double t = p.num(QStringLiteral("threshold"));
            for (int i = 0; i < 256; ++i) {
                const double v = i / 255.0;
                l[0][size_t(i)] = uint8_t(std::lround((v > t ? 1.0 - v : v) * 255.0));
            }
            l[1] = l[2] = l[0];
            return true;
        };
        list.push_back(std::move(d));
    }

    // ---- Stylize
    add("ascii", "ASCII Art", Y, "Renders the picture as text characters, in colour or terminal green/amber.",
        {num("cellSize", "Character size", 4, 64, 12, 0, " px"),
         choice("charset", "Characters", {QStringLiteral("Classic"), QStringLiteral("Detailed"), QStringLiteral("Blocks"), QStringLiteral("Binary")}),
         choice("colorMode", "Colour", {QStringLiteral("Original"), QStringLiteral("Green terminal"), QStringLiteral("Amber terminal"), QStringLiteral("White")}),
         num("background", "Background", 0, 1, 0)},
        [](QImage& img, const Params& p, const RenderContext& ctx, St& st) { asciiArt(img, p, ctx, st); });
    add("halftone", "Halftone", Y, "Printed dot screens: one ink, CMYK colour print, or RGB screen dots.",
        {num("cellSize", "Dot size", 2, 64, 8, 1, " px"), num("angle", "Angle", -90, 90, 45, 0, "°"),
         choice("mode", "Mode", {QStringLiteral("One ink"), QStringLiteral("CMYK"), QStringLiteral("RGB screen")}), color("ink", "Ink", 0x101018),
         color("paper", "Paper", 0xf4efe4)},
        pure(halftone));
    add("neon_edges", "Neon Edges", Y, "Glowing outlines of edges, in the picture's colours, a rainbow or one colour.",
        {num("strength", "Strength", 0, 10, 3), choice("colorMode", "Colour", {QStringLiteral("Original"), QStringLiteral("Rainbow"), QStringLiteral("Single colour")}, 1),
         color("color", "Colour", 0x00e5ff), num("background", "Background", 0, 1, 0), num("glow", "Glow", 0, 60, 6, 1, " px")},
        pure(neonEdges));
    add("emboss", "Emboss", Y, "Raised relief look from the picture's edges.",
        {num("angle", "Light angle", -180, 180, 45, 0, "°"), num("strength", "Strength", 0, 10, 3), num("mix", "Mix", 0, 1, 1),
         flag("keepColor", "Keep colour", false)},
        pure(emboss));
}
