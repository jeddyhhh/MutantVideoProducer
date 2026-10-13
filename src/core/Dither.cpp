#include "core/Dither.h"

#include "core/EffectHelpers.h"
#include "core/Parallel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <functional>
#include <numeric>

using namespace fx;

namespace dither {

namespace {

QString S(const char* s) { return QString::fromLatin1(s); }

// The NES's 2C02 picture: 64 entries, the unused blacks dropped below.
const std::vector<uint32_t> kNes = {
    0x7c7c7c, 0x0000fc, 0x0000bc, 0x4428bc, 0x940084, 0xa80020, 0xa81000, 0x881400, 0x503000, 0x007800, 0x006800, 0x005800, 0x004058, 0x000000,
    0xbcbcbc, 0x0078f8, 0x0058f8, 0x6844fc, 0xd800cc, 0xe40058, 0xf83800, 0xe45c10, 0xac7c00, 0x00b800, 0x00a800, 0x00a844, 0x008888,
    0xf8f8f8, 0x3cbcfc, 0x6888fc, 0x9878f8, 0xf878f8, 0xf85898, 0xf87858, 0xfca044, 0xf8b800, 0xb8f818, 0x58d854, 0x58f898, 0x00e8d8, 0x787878,
    0xfcfcfc, 0xa4e4fc, 0xb8b8f8, 0xd8b8f8, 0xf8b8f8, 0xf8a4c0, 0xf0d0b0, 0xfce0a8, 0xf8d878, 0xd8f878, 0xb8f8b8, 0xb8f8d8, 0x00fcfc, 0xf8d8f8};

std::vector<uint32_t> grays(int n)
{
    std::vector<uint32_t> out;
    for (int i = 0; i < n; ++i) {
        const uint32_t v = uint32_t((i * 255 * 2 + (n - 1)) / (2 * (n - 1)));
        out.push_back(v << 16 | v << 8 | v);
    }
    return out;
}

} // namespace

const std::vector<Palette>& palettes()
{
    static const std::vector<Palette> list = [] {
        std::vector<Palette> l = {
            // (The first ten keep their places: saved projects refer to them by number.)
            {"Black & White (1-bit)", {0x000000, 0xffffff}},
            {"Game Boy", {0x0f380f, 0x306230, 0x8bac0f, 0x9bbc0f}},
            {"CGA 1 (cyan, magenta, white)", {0x000000, 0x55ffff, 0xff55ff, 0xffffff}},
            {"EGA (16 colours)", {0x000000, 0x0000aa, 0x00aa00, 0x00aaaa, 0xaa0000, 0xaa00aa, 0xaa5500, 0xaaaaaa, 0x555555, 0x5555ff, 0x55ff55,
                                  0x55ffff, 0xff5555, 0xff55ff, 0xffff55, 0xffffff}},
            {"Commodore 64", {0x000000, 0xffffff, 0x880000, 0xaaffee, 0xcc44cc, 0x00cc55, 0x0000aa, 0xeeee77, 0xdd8855, 0x664400, 0xff7777,
                              0x333333, 0x777777, 0xaaff66, 0x0088ff, 0xbbbbbb}},
            {"PICO-8", {0x000000, 0x1d2b53, 0x7e2553, 0x008751, 0xab5236, 0x5f574f, 0xc2c3c7, 0xfff1e8, 0xff004d, 0xffa300, 0xffec27, 0x00e436,
                        0x29adff, 0x83769c, 0xff77a8, 0xffccaa}},
            {"Teletext (8 colours)", {0x000000, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff}},
            {"Grayscale (4 levels)", {0x000000, 0x555555, 0xaaaaaa, 0xffffff}},
            {"Vaporwave", {0x2d1b4e, 0xff71ce, 0x01cdfe, 0x05ffa1, 0xb967ff, 0xfffb96}},
            {"Sepia (4 tones)", {0x2b1d0e, 0x6b4a2b, 0xb08a5b, 0xf2e2c4}},
            // PC
            {"CGA 0 (green, red, brown)", {0x000000, 0x00aa00, 0xaa0000, 0xaa5500}},
            {"CGA 0 bright (green, red, yellow)", {0x000000, 0x55ff55, 0xff5555, 0xffff55}},
            {"CGA 1 dim (cyan, magenta, grey)", {0x000000, 0x00aaaa, 0xaa00aa, 0xaaaaaa}},
            {"CGA mode 5 (cyan, red, white)", {0x000000, 0x55ffff, 0xff5555, 0xffffff}},
            {"EGA / Master System (all 64)", {}, 4},
            // Home computers
            {"Commodore 64 (Pepto)", {0x000000, 0xffffff, 0x68372b, 0x70a4b2, 0x6f3d86, 0x588d43, 0x352879, 0xb8c76f, 0x6f4f25, 0x433900, 0x9a6759,
                                      0x444444, 0x6c6c6c, 0x9ad284, 0x6c5eb5, 0x959595}},
            {"Commodore 64 (Colodore)", {0x000000, 0xffffff, 0x813338, 0x75cec8, 0x8e3c97, 0x56ac4d, 0x2e2c9b, 0xedf171, 0x8e5029, 0x553800,
                                         0xc46c71, 0x4a4a4a, 0x7b7b7b, 0xa9ff9f, 0x706deb, 0xb2b2b2}},
            {"ZX Spectrum", {0x000000, 0x0000d7, 0xd70000, 0xd700d7, 0x00d700, 0x00d7d7, 0xd7d700, 0xd7d7d7, 0x0000ff, 0xff0000, 0xff00ff,
                             0x00ff00, 0x00ffff, 0xffff00, 0xffffff}},
            {"Amstrad CPC (27)", {}, 3},
            {"MSX / ColecoVision (TMS9918)", {0x000000, 0x21c842, 0x5edc78, 0x5455ed, 0x7d76fc, 0xd4524d, 0x42ebf5, 0xfc5554, 0xff7978, 0xd4c154,
                                              0xe6ce80, 0x21b03b, 0xc95bba, 0xcccccc, 0xffffff}},
            {"Apple II (hi-res)", {0x000000, 0xffffff, 0x14f53c, 0xff44fd, 0xff6a3c, 0x14cffd}},
            {"Amiga Workbench 1.3", {0x0055aa, 0xffffff, 0x000022, 0xff8800}},
            {"Amiga Workbench 2.0", {0xaaaaaa, 0x000000, 0xffffff, 0x6688bb}},
            {"Macintosh II (16)", {0xffffff, 0xfbf305, 0xff6403, 0xdd0907, 0xf20884, 0x4700a5, 0x0000d3, 0x02abea, 0x1fb714, 0x006412, 0x562c05,
                                   0x90713a, 0xc0c0c0, 0x808080, 0x404040, 0x000000}},
            {"Windows (16)", {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0, 0x808080, 0xff0000, 0x00ff00,
                              0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff}},
            {"Windows (20)", {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xc0c0c0, 0xc0dcc0, 0xa6caf0, 0xfffbf0,
                              0xa0a0a4, 0x808080, 0xff0000, 0x00ff00, 0xffff00, 0x0000ff, 0xff00ff, 0x00ffff, 0xffffff}},
            {"RISC OS (16)", {0xffffff, 0xdddddd, 0xbbbbbb, 0x999999, 0x777777, 0x555555, 0x333333, 0x000000, 0x004499, 0xeeee00, 0x00cc00,
                              0xdd0000, 0xeeeebb, 0x558800, 0xffbb00, 0x00bbff}},
            // Consoles and handhelds
            {"NES", kNes},
            {"Game Boy Pocket", {0x1f1f1f, 0x4d533c, 0x8b956d, 0xc4cfa1}},
            {"Game Boy (Kirokaze)", {0x332c50, 0x46878f, 0x94e344, 0xe2f3e4}},
            {"Game Boy (Ice Cream)", {0x7c3f58, 0xeb6b6f, 0xf9a875, 0xfff6d3}},
            {"Game Boy (Mist)", {0x2d1b00, 0x1e606e, 0x5ab9a8, 0xc4f0c2}},
            {"Game Boy (Rustic)", {0x2c2137, 0x764462, 0xa96868, 0xedb4a1}},
            {"Virtual Boy", {0x000000, 0x550000, 0xaa0000, 0xff0000}},
            {"Nokia 3310", {0x43523d, 0xc7f0d8}},
            // Colour depths
            {"9-bit (512: Atari ST, Mega Drive)", {}, 8},
            {"12-bit (4,096: Amiga, Game Gear)", {}, 16},
            {"15-bit (32,768: SNES, Game Boy Advance)", {}, 32},
            {"Web-safe (216)", {}, 6},
            // Pixel art
            {"Sweetie 16 (TIC-80)", {0x1a1c2c, 0x5d275d, 0xb13e53, 0xef7d57, 0xffcd75, 0xa7f070, 0x38b764, 0x257179, 0x29366f, 0x3b5dc9, 0x41a6f6,
                                     0x73eff7, 0xf4f4f4, 0x94b0c2, 0x566c86, 0x333c57}},
            {"DawnBringer 16", {0x140c1c, 0x442434, 0x30346d, 0x4e4a4e, 0x854c30, 0x346524, 0xd04648, 0x757161, 0x597dce, 0xd27d2c, 0x8595a1,
                                0x6daa2c, 0xd2aa99, 0x6dc2ca, 0xdad45e, 0xdeeed6}},
            {"DawnBringer 32", {0x000000, 0x222034, 0x45283c, 0x663931, 0x8f563b, 0xdf7126, 0xd9a066, 0xeec39a, 0xfbf236, 0x99e550, 0x6abe30,
                                0x37946e, 0x4b692f, 0x524b24, 0x323c39, 0x3f3f74, 0x306082, 0x5b6ee1, 0x639bff, 0x5fcde4, 0xcbdbfc, 0xffffff,
                                0x9badb7, 0x847e87, 0x696a6a, 0x595652, 0x76428a, 0xac3232, 0xd95763, 0xd77bba, 0x8f974a, 0x8a6f30}},
            {"Endesga 32", {0xbe4a2f, 0xd77643, 0xead4aa, 0xe4a672, 0xb86f50, 0x733e39, 0x3e2731, 0xa22633, 0xe43b44, 0xf77622, 0xfeae34,
                            0xfee761, 0x63c74d, 0x3e8948, 0x265c42, 0x193c3e, 0x124e89, 0x0099db, 0x2ce8f5, 0xffffff, 0xc0cbdc, 0x8b9bb4,
                            0x5a6988, 0x3a4466, 0x262b44, 0x181425, 0xff0044, 0x68386c, 0xb55088, 0xf6757a, 0xe8b796, 0xc28569}},
            // Screens, print and moods
            {"Green screen (4 tones)", {0x001a00, 0x006600, 0x00b300, 0x33ff33}},
            {"Amber screen (4 tones)", {0x1a0d00, 0x664000, 0xb37300, 0xffb000}},
            {"Grayscale (8 levels)", grays(8)},
            {"Grayscale (16 levels)", grays(16)},
            {"Risograph (pink, blue, yellow)", {0x1a1a1a, 0x0078bf, 0xff48b0, 0xffe800, 0xf7f3ea}},
            {"CMYK print", {0x231f20, 0x2e3192, 0xed1c24, 0x00a651, 0xec008c, 0x00aeef, 0xfff200, 0xffffff}},
            {"Neon night", {0x0b0a1f, 0x1f1147, 0x6e1e8c, 0xff2a6d, 0x05d9e8, 0xd1f7ff}},
            // From the picture itself
            {"Adaptive (2 colours from the picture)", {}, 0, 2},
            {"Adaptive (4 colours from the picture)", {}, 0, 4},
            {"Adaptive (8 colours from the picture)", {}, 0, 8},
            {"Adaptive (16 colours from the picture)", {}, 0, 16},
            {"Adaptive (32 colours from the picture)", {}, 0, 32},
            {"Adaptive (64 colours from the picture)", {}, 0, 64},
        };
        for (Palette& p : l) { // the NES's repeated blacks
            std::vector<uint32_t> unique;
            for (uint32_t c : p.colors)
                if (std::find(unique.begin(), unique.end(), c) == unique.end())
                    unique.push_back(c);
            p.colors = std::move(unique);
        }
        return l;
    }();
    return list;
}

QStringList paletteNames()
{
    QStringList names;
    for (const Palette& p : palettes())
        names << QString::fromUtf8(p.name);
    return names;
}

const std::vector<Method>& methods()
{
    using K = Kind;
    static const std::vector<Method> list = {
        // (The first five keep their places: saved projects refer to them by number.)
        {"None (nearest colour)", K::None},
        {"Ordered: Bayer 4x4", K::Map, 1},
        {"Ordered: Bayer 8x8", K::Map, 2},
        {"Error diffusion: Floyd-Steinberg", K::Diffusion, -1, {{1, 0, 7}, {-1, 1, 3}, {0, 1, 5}, {1, 1, 1}}, 16},
        {"Error diffusion: Atkinson", K::Diffusion, -1, {{1, 0, 1}, {2, 0, 1}, {-1, 1, 1}, {0, 1, 1}, {1, 1, 1}, {0, 2, 1}}, 8},
        {"Ordered: Bayer 2x2", K::Map, 0},
        {"Ordered: Bayer 16x16", K::Map, 3},
        {"Ordered: clustered dots 4x4", K::Map, 4},
        {"Ordered: clustered dots 8x8", K::Map, 5},
        {"Ordered: halftone 45°", K::Map, 6},
        {"Ordered: horizontal lines", K::Map, 7},
        {"Ordered: vertical lines", K::Map, 8},
        {"Ordered: diagonal lines", K::Map, 9},
        {"Ordered: crosshatch", K::Map, 10},
        {"Noise: blue noise", K::Map, 11},
        {"Noise: white noise", K::WhiteNoise},
        {"Noise: interleaved gradient", K::GradientNoise},
        {"Pattern (Knoll)", K::Pattern, 1},
        {"Error diffusion: false Floyd-Steinberg", K::Diffusion, -1, {{1, 0, 3}, {0, 1, 3}, {1, 1, 2}}, 8},
        {"Error diffusion: Jarvis, Judice & Ninke", K::Diffusion, -1,
         {{1, 0, 7}, {2, 0, 5}, {-2, 1, 3}, {-1, 1, 5}, {0, 1, 7}, {1, 1, 5}, {2, 1, 3}, {-2, 2, 1}, {-1, 2, 3}, {0, 2, 5}, {1, 2, 3}, {2, 2, 1}}, 48},
        {"Error diffusion: Stucki", K::Diffusion, -1,
         {{1, 0, 8}, {2, 0, 4}, {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2}, {-2, 2, 1}, {-1, 2, 2}, {0, 2, 4}, {1, 2, 2}, {2, 2, 1}}, 42},
        {"Error diffusion: Burkes", K::Diffusion, -1, {{1, 0, 8}, {2, 0, 4}, {-2, 1, 2}, {-1, 1, 4}, {0, 1, 8}, {1, 1, 4}, {2, 1, 2}}, 32},
        {"Error diffusion: Sierra", K::Diffusion, -1,
         {{1, 0, 5}, {2, 0, 3}, {-2, 1, 2}, {-1, 1, 4}, {0, 1, 5}, {1, 1, 4}, {2, 1, 2}, {-1, 2, 2}, {0, 2, 3}, {1, 2, 2}}, 32},
        {"Error diffusion: Sierra two-row", K::Diffusion, -1, {{1, 0, 4}, {2, 0, 3}, {-2, 1, 1}, {-1, 1, 2}, {0, 1, 3}, {1, 1, 2}, {2, 1, 1}}, 16},
        {"Error diffusion: Sierra Lite", K::Diffusion, -1, {{1, 0, 2}, {-1, 1, 1}, {0, 1, 1}}, 4},
        {"Error diffusion: Shiau-Fan", K::Diffusion, -1, {{1, 0, 4}, {-2, 1, 1}, {-1, 1, 1}, {0, 1, 2}}, 8},
        {"Error diffusion: Shiau-Fan 2", K::Diffusion, -1, {{1, 0, 8}, {-3, 1, 1}, {-2, 1, 1}, {-1, 1, 2}, {0, 1, 4}}, 16},
        {"Riemersma (Hilbert curve)", K::Riemersma},
    };
    return list;
}

QStringList methodNames()
{
    QStringList names;
    for (const Method& m : methods())
        names << QString::fromUtf8(m.name);
    return names;
}

int waveSlope(const Method& m)
{
    int k = 1;
    for (const Tap& t : m.taps)
        if (t.dy > 0)
            k = std::max(k, (-t.dx) / t.dy + 1);
    return k;
}

// ---- Threshold maps ----------------------------------------------------------------

namespace {

// Ranks to thresholds: the cells in order of `key` (ties: reading order).
ThresholdMap fromKeys(int n, const std::function<double(int x, int y)>& key)
{
    std::vector<int> order(size_t(n) * size_t(n));
    std::iota(order.begin(), order.end(), 0);
    std::vector<double> k(order.size());
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x)
            k[size_t(y * n + x)] = key(x, y);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return k[size_t(a)] < k[size_t(b)]; });
    ThresholdMap m;
    m.n = n;
    m.values.resize(order.size());
    for (size_t r = 0; r < order.size(); ++r)
        m.values[size_t(order[r])] = (float(r) + 0.5f) / float(order.size());
    return m;
}

ThresholdMap bayer(int n)
{
    std::vector<int> m{0};
    for (int size = 1; size < n; size *= 2) {
        std::vector<int> next(size_t(size) * 4 * size_t(size));
        static const int q[2][2] = {{0, 2}, {3, 1}};
        for (int y = 0; y < size * 2; ++y)
            for (int x = 0; x < size * 2; ++x)
                next[size_t(y * size * 2 + x)] = 4 * m[size_t((y % size) * size + x % size)] + q[y / size][x / size];
        m = std::move(next);
    }
    return fromKeys(n, [&](int x, int y) { return double(m[size_t(y * n + x)]); });
}

// Blue noise by void and cluster (Ulichney): a pattern whose dots are as
// evenly spread as can be at every level, so the dither has no visible texture.
ThresholdMap blueNoise(int n)
{
    const int count = n * n;
    // Gaussian energy of each pixel to every other (the map wraps around).
    std::vector<float> kernel(static_cast<size_t>(count));
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const int dx = std::min(x, n - x), dy = std::min(y, n - y);
            kernel[size_t(y * n + x)] = float(std::exp(-(dx * dx + dy * dy) / (2.0 * 1.5 * 1.5)));
        }
    std::vector<uint8_t> on(size_t(count), 0);
    std::vector<float> energy(size_t(count), 0.0f);
    auto toggle = [&](int i, bool set) {
        on[size_t(i)] = set;
        const int ix = i % n, iy = i / n;
        const float s = set ? 1.0f : -1.0f;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                energy[size_t(y * n + x)] += s * kernel[size_t(((y - iy + n) % n) * n + (x - ix + n) % n)];
    };
    auto extreme = [&](bool ones, bool tightest) {
        int best = -1;
        float bestE = 0;
        for (int i = 0; i < count; ++i)
            if (bool(on[size_t(i)]) == ones && (best < 0 || (tightest ? energy[size_t(i)] > bestE : energy[size_t(i)] < bestE))) {
                best = i;
                bestE = energy[size_t(i)];
            }
        return best;
    };
    // A random start (a fixed seed: the same map every time), relaxed until even.
    uint32_t rng = 12345;
    const int initial = count / 10;
    for (int placed = 0; placed < initial;) {
        rng = rng * 1664525u + 1013904223u;
        const int i = int((rng >> 8) % uint32_t(count));
        if (!on[size_t(i)]) {
            toggle(i, true);
            ++placed;
        }
    }
    for (int guard = 0; guard < count; ++guard) {
        const int cluster = extreme(true, true);
        toggle(cluster, false);
        const int voidAt = extreme(false, false);
        if (voidAt == cluster) {
            toggle(cluster, true);
            break;
        }
        toggle(voidAt, true);
    }
    const std::vector<uint8_t> start = on;
    const std::vector<float> startEnergy = energy;
    std::vector<int> rank(size_t(count), 0);
    // Ranks below the start: take the tightest clusters away one by one.
    for (int r = initial - 1; r >= 0; --r) {
        const int i = extreme(true, true);
        toggle(i, false);
        rank[size_t(i)] = r;
    }
    on = start;
    energy = startEnergy;
    // Ranks above: fill the largest voids.
    for (int r = initial; r < count; ++r) {
        const int i = extreme(false, false);
        toggle(i, true);
        rank[size_t(i)] = r;
    }
    return fromKeys(n, [&](int x, int y) { return double(rank[size_t(y * n + x)]); });
}

const std::vector<ThresholdMap>& allMaps()
{
    static const std::vector<ThresholdMap> maps = [] {
        constexpr double kTau = 6.283185307179586;
        std::vector<ThresholdMap> m;
        m.push_back(bayer(2));
        m.push_back(bayer(4));
        m.push_back(bayer(8));
        m.push_back(bayer(16));
        // Clustered dots: darkest first in the middle of each cell, growing outwards.
        for (int n : {4, 8})
            m.push_back(fromKeys(n, [n](int x, int y) {
                const double dx = x + 0.5 - n / 2.0, dy = y + 0.5 - n / 2.0;
                return dx * dx + dy * dy;
            }));
        // A newspaper screen: dots on a 45° grid.
        m.push_back(fromKeys(8, [](int x, int y) {
            const double u = (x + 0.5) / 8.0, v = (y + 0.5) / 8.0;
            return -(std::cos(kTau * (u + v)) + std::cos(kTau * (u - v)));
        }));
        m.push_back(fromKeys(4, [](int, int y) { return -std::cos(kTau * (y + 0.5) / 4.0); }));     // horizontal lines
        m.push_back(fromKeys(4, [](int x, int) { return -std::cos(kTau * (x + 0.5) / 4.0); }));     // vertical lines
        m.push_back(fromKeys(8, [](int x, int y) { return -std::cos(kTau * (x + y + 1) / 8.0); })); // diagonal lines
        m.push_back(fromKeys(8, [](int x, int y) { // crosshatch
            return -std::max(std::cos(kTau * (x + 0.5) / 8.0), std::cos(kTau * (y + 0.5) / 8.0));
        }));
        m.push_back(blueNoise(64));
        return m;
    }();
    return maps;
}

} // namespace

const ThresholdMap& thresholdMap(int id)
{
    const auto& maps = allMaps();
    return maps[size_t(std::clamp(id, 0, int(maps.size()) - 1))];
}

// ---- Adaptive palettes -------------------------------------------------------------------

std::vector<uint32_t> adaptivePalette(const std::vector<float>& rgb, const std::vector<uint8_t>& alpha, int count)
{
    struct Point {
        float c[3];
    };
    std::vector<Point> points;
    const size_t cells = alpha.size();
    const size_t step = std::max<size_t>(1, cells / 65536); // plenty to choose from
    for (size_t k = 0; k < cells; k += step)
        if (alpha[k] > 0)
            points.push_back({{std::clamp(rgb[k * 3], 0.0f, 255.0f), std::clamp(rgb[k * 3 + 1], 0.0f, 255.0f), std::clamp(rgb[k * 3 + 2], 0.0f, 255.0f)}});
    if (points.empty())
        return {0x000000};
    struct Box {
        size_t begin, end;
    };
    std::vector<Box> boxes{{0, points.size()}};
    auto range = [&](const Box& b, int& channel) {
        float best = -1;
        for (int c = 0; c < 3; ++c) {
            float lo = 255, hi = 0;
            for (size_t i = b.begin; i < b.end; ++i) {
                lo = std::min(lo, points[i].c[c]);
                hi = std::max(hi, points[i].c[c]);
            }
            if (hi - lo > best) {
                best = hi - lo;
                channel = c;
            }
        }
        return best;
    };
    while (int(boxes.size()) < count) {
        // Split the box with the widest spread of colour at its median.
        int which = -1, channel = 0;
        float widest = 0;
        for (size_t b = 0; b < boxes.size(); ++b) {
            if (boxes[b].end - boxes[b].begin < 2)
                continue;
            int c = 0;
            const float r = range(boxes[b], c);
            if (r > widest) {
                widest = r;
                which = int(b);
                channel = c;
            }
        }
        if (which < 0)
            break;
        Box& b = boxes[size_t(which)];
        std::stable_sort(points.begin() + std::ptrdiff_t(b.begin), points.begin() + std::ptrdiff_t(b.end),
                         [channel](const Point& p, const Point& q) { return p.c[channel] < q.c[channel]; });
        const size_t mid = b.begin + (b.end - b.begin) / 2;
        const Box upper{mid, b.end};
        b.end = mid;
        boxes.push_back(upper);
    }
    std::vector<uint32_t> out;
    for (const Box& b : boxes) {
        double s[3] = {0, 0, 0};
        for (size_t i = b.begin; i < b.end; ++i)
            for (int c = 0; c < 3; ++c)
                s[c] += points[i].c[c];
        const double n = double(b.end - b.begin);
        const uint32_t color = uint32_t(std::lround(s[0] / n)) << 16 | uint32_t(std::lround(s[1] / n)) << 8 | uint32_t(std::lround(s[2] / n));
        if (std::find(out.begin(), out.end(), color) == out.end())
            out.push_back(color);
    }
    std::stable_sort(out.begin(), out.end(), [](uint32_t a, uint32_t b) {
        auto y = [](uint32_t c) { return 299 * int((c >> 16) & 0xff) + 587 * int((c >> 8) & 0xff) + 114 * int(c & 0xff); };
        return y(a) < y(b);
    });
    return out;
}

// ---- The effect ----------------------------------------------------------------------------

namespace {

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

// OKLab (perceptual) from sRGB 0..255; the GPU does the same sums.
std::array<float, 3> oklab(float r, float g, float b)
{
    auto lin = [](float c) {
        c = std::clamp(c, 0.0f, 255.0f) / 255.0f;
        return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
    };
    const float lr = lin(r), lg = lin(g), lb = lin(b);
    const float l = std::pow(0.4122214708f * lr + 0.5363325363f * lg + 0.0514459929f * lb, 1.0f / 3.0f);
    const float m = std::pow(0.2119034982f * lr + 0.6806995451f * lg + 0.1073969566f * lb, 1.0f / 3.0f);
    const float s = std::pow(0.0883024619f * lr + 0.2817188376f * lg + 0.6299787005f * lb, 1.0f / 3.0f);
    return {0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s, 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
            0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s};
}

// The colours of a frame and how to find the nearest one.
struct Colours {
    std::vector<uint32_t> list; // when not levels
    int levels = 0;
    bool perceptual = false;
    std::vector<std::array<float, 3>> lab;

    int count() const { return levels ? levels * levels * levels : int(list.size()); }
    uint32_t color(int i) const
    {
        if (!levels)
            return list[size_t(i)];
        const int l = levels;
        auto v = [l](int q) { return uint32_t((q * 255 * 2 + (l - 1)) / (2 * (l - 1))); };
        return v(i / (l * l)) << 16 | v(i / l % l) << 8 | v(i % l);
    }
    int nearest(float r, float g, float b) const
    {
        if (levels) {
            auto q = [this](float c) { return std::clamp(int(std::floor(c * float(levels - 1) / 255.0f + 0.5f)), 0, levels - 1); };
            return (q(r) * levels + q(g)) * levels + q(b);
        }
        int best = 0;
        float bestD = std::numeric_limits<float>::max();
        if (perceptual) {
            const auto p = oklab(r, g, b);
            for (size_t i = 0; i < lab.size(); ++i) {
                const float d0 = p[0] - lab[i][0], d1 = p[1] - lab[i][1], d2 = p[2] - lab[i][2];
                const float d = d0 * d0 + d1 * d1 + d2 * d2;
                if (d < bestD) {
                    bestD = d;
                    best = int(i);
                }
            }
            return best;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            const float dr = r - float((list[i] >> 16) & 0xff), dg = g - float((list[i] >> 8) & 0xff), db = b - float(list[i] & 0xff);
            const float d = dr * dr * 0.3f + dg * dg * 0.59f + db * db * 0.11f;
            if (d < bestD) {
                bestD = d;
                best = int(i);
            }
        }
        return best;
    }
    // Error diffusion works in integer 1/16 colour units (the GPU matches it exactly).
    int nearest16(int r, int g, int b) const
    {
        if (levels) {
            const int l = levels;
            auto q = [l](int v) { return (std::clamp(v, 0, 4080) * (l - 1) * 2 + 4080) / 8160; };
            return (q(r) * l + q(g)) * l + q(b);
        }
        int best = 0;
        int bestD = std::numeric_limits<int>::max();
        for (size_t i = 0; i < list.size(); ++i) {
            const int dr = r - int((list[i] >> 16) & 0xff) * 16, dg = g - int((list[i] >> 8) & 0xff) * 16, db = b - int(list[i] & 0xff) * 16;
            const int d = dr * dr * 3 + dg * dg * 6 + db * db;
            if (d < bestD) {
                bestD = d;
                best = int(i);
            }
        }
        return best;
    }
};

// The Hilbert curve through a w x h grid (a 2^k square, the points outside left out).
std::vector<std::pair<int, int>> hilbertPath(int w, int h)
{
    int n = 1;
    while (n < std::max(w, h))
        n *= 2;
    std::vector<std::pair<int, int>> path;
    path.reserve(size_t(w) * size_t(h));
    for (int64_t d = 0; d < int64_t(n) * n; ++d) {
        int x = 0, y = 0;
        int64_t t = d;
        for (int s = 1; s < n; s *= 2) {
            const int rx = int(1 & (t / 2)), ry = int(1 & (t ^ rx));
            if (ry == 0) {
                if (rx == 1) {
                    x = s - 1 - x;
                    y = s - 1 - y;
                }
                std::swap(x, y);
            }
            x += s * rx;
            y += s * ry;
            t /= 4;
        }
        if (x < w && y < h)
            path.emplace_back(x, y);
    }
    return path;
}

} // namespace

void apply(QImage& img, const Params& p, const RenderContext& ctx)
{
    const Palette& palette = palettes()[size_t(std::clamp(p.choice(S("palette")), 0, int(palettes().size()) - 1))];
    const Method& method = methods()[size_t(std::clamp(p.choice(S("method")), 0, int(methods().size()) - 1))];
    const int cell = std::max(1, int(std::lround(p.num(S("pixelSize")) * ctx.scale)));
    const int w = img.width(), h = img.height();
    const int sw = (w + cell - 1) / cell, sh = (h + cell - 1) / cell;
    const size_t cells = size_t(sw) * size_t(sh);
    // Downscale (box), un-premultiplied colour.
    std::vector<float> rgb(cells * 3);
    std::vector<uint8_t> alpha(cells);
    std::vector<int> sums(cells * 4); // r, g, b, a totals per cell
    {
        const Pixels in(img);
        parallelFor(sh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < sw; ++x) {
                    float r = 0, g = 0, b = 0, a = 0;
                    int n = 0;
                    for (int j = y * cell; j < std::min(h, (y + 1) * cell); ++j)
                        for (int i = x * cell; i < std::min(w, (x + 1) * cell); ++i) {
                            const uint32_t px = in.at(i, j);
                            r += float(pr(px)), g += float(pg(px)), b += float(pb(px)), a += float(pa(px));
                            ++n;
                        }
                    const size_t k = size_t(y) * size_t(sw) + size_t(x);
                    sums[k * 4] = int(r), sums[k * 4 + 1] = int(g), sums[k * 4 + 2] = int(b), sums[k * 4 + 3] = int(a);
                    const float un = a > 0 ? 255.0f / a : 0.0f;
                    rgb[k * 3] = r * un;
                    rgb[k * 3 + 1] = g * un;
                    rgb[k * 3 + 2] = b * un;
                    alpha[k] = uint8_t(a / float(std::max(1, n)) + 0.5f);
                }
        });
    }
    Colours colours;
    colours.levels = palette.levels;
    colours.list = palette.adaptive ? adaptivePalette(rgb, alpha, palette.adaptive) : palette.colors;
    colours.perceptual = p.choice(S("match")) == 1 && !colours.levels && method.kind != Kind::Diffusion;
    if (colours.perceptual)
        for (uint32_t c : colours.list)
            colours.lab.push_back(oklab(float((c >> 16) & 0xff), float((c >> 8) & 0xff), float(c & 0xff)));
    const double strength = p.num(S("strength"));
    const float spread = float(strength) * (colours.levels ? 255.0f / float(colours.levels - 1)
                                                           : 255.0f / std::max(1.5f, std::cbrt(float(colours.count()))));
    int ox = 0, oy = 0;
    if (p.flag(S("animate")))
        noiseOffset(frameIndex(ctx), ox, oy);

    std::vector<int> index(cells);
    switch (method.kind) {
    case Kind::None:
    case Kind::Map:
    case Kind::WhiteNoise:
    case Kind::GradientNoise: {
        const ThresholdMap* map = method.kind == Kind::Map ? &thresholdMap(method.map) : nullptr;
        const bool moving = method.kind != Kind::Map || method.map == 11; // only the noises move (Animate)
        const int mx = moving ? ox : 0, my = moving ? oy : 0;
        parallelFor(sh, [&](int y0, int y1) {
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < sw; ++x) {
                    const size_t k = size_t(y) * size_t(sw) + size_t(x);
                    float t = 0.5f;
                    if (map)
                        t = map->values[size_t(((y + my) % map->n) * map->n + (x + mx) % map->n)];
                    else if (method.kind == Kind::WhiteNoise)
                        t = float(noiseHash(uint32_t(x + mx), uint32_t(y + my), 0) >> 8) / 16777216.0f;
                    else if (method.kind == Kind::GradientNoise) {
                        const float f = 0.06711056f * float(x + mx) + 0.00583715f * float(y + my);
                        const float g = 52.9829189f * (f - std::floor(f));
                        t = g - std::floor(g);
                    }
                    const float o = (t - 0.5f) * spread;
                    index[k] = colours.nearest(rgb[k * 3] + o, rgb[k * 3 + 1] + o, rgb[k * 3 + 2] + o);
                }
        });
        break;
    }
    case Kind::Pattern: {
        // Knoll's pattern dithering: a plan of 16 colours whose average is the
        // wanted one (each chosen with the error so far), lightest last; the
        // threshold map picks one per pixel.
        const ThresholdMap& map = thresholdMap(method.map);
        const int plan = map.n * map.n;
        const float carry = float(std::clamp(strength, 0.0, 2.0) / 2.0 + 0.5);
        parallelFor(sh, [&](int y0, int y1) {
            std::vector<int> picks(static_cast<size_t>(plan));
            for (int y = y0; y < y1; ++y)
                for (int x = 0; x < sw; ++x) {
                    const size_t k = size_t(y) * size_t(sw) + size_t(x);
                    const float c[3] = {rgb[k * 3], rgb[k * 3 + 1], rgb[k * 3 + 2]};
                    float e[3] = {0, 0, 0};
                    for (int i = 0; i < plan; ++i) {
                        const int pick = colours.nearest(std::clamp(c[0] + e[0] * carry, 0.0f, 255.0f), std::clamp(c[1] + e[1] * carry, 0.0f, 255.0f),
                                                         std::clamp(c[2] + e[2] * carry, 0.0f, 255.0f));
                        picks[size_t(i)] = pick;
                        const uint32_t pc = colours.color(pick);
                        e[0] += c[0] - float((pc >> 16) & 0xff);
                        e[1] += c[1] - float((pc >> 8) & 0xff);
                        e[2] += c[2] - float(pc & 0xff);
                    }
                    auto luma = [&](int i) {
                        const uint32_t pc = colours.color(i);
                        return 299 * int((pc >> 16) & 0xff) + 587 * int((pc >> 8) & 0xff) + 114 * int(pc & 0xff);
                    };
                    std::stable_sort(picks.begin(), picks.end(), [&](int a, int b) { return luma(a) < luma(b); });
                    const float t = map.values[size_t((y % map.n) * map.n + x % map.n)];
                    index[k] = picks[size_t(std::min(plan - 1, int(t * float(plan))))];
                }
        });
        break;
    }
    case Kind::Diffusion: {
        // In integer 1/16 units, so the GPU (which works through the picture in
        // diagonal waves) gets exactly the same result. Strength scales the error passed on.
        const int s256 = int(std::lround(strength * 256.0));
        std::vector<int> v(cells * 3);
        for (size_t k = 0; k < cells; ++k)
            for (size_t c = 0; c < 3; ++c) {
                const int a = sums[k * 4 + 3];
                v[k * 3 + c] = a > 0 ? (sums[k * 4 + c] * 4080 * 2 + a) / (2 * a) : 0; // round(sum * 255 / a * 16)
            }
        for (int y = 0; y < sh; ++y)
            for (int x = 0; x < sw; ++x) {
                const size_t k = size_t(y) * size_t(sw) + size_t(x);
                const int i = colours.nearest16(v[k * 3], v[k * 3 + 1], v[k * 3 + 2]);
                index[k] = i;
                const uint32_t pc = colours.color(i);
                int err[3] = {v[k * 3] - int((pc >> 16) & 0xff) * 16, v[k * 3 + 1] - int((pc >> 8) & 0xff) * 16, v[k * 3 + 2] - int(pc & 0xff) * 16};
                for (int& e : err)
                    e = floorDiv(e * s256, 256);
                for (const Tap& t : method.taps) {
                    const int xx = x + t.dx, yy = y + t.dy;
                    if (xx < 0 || xx >= sw || yy >= sh)
                        continue;
                    const size_t kk = size_t(yy) * size_t(sw) + size_t(xx);
                    for (int c = 0; c < 3; ++c)
                        v[kk * 3 + size_t(c)] += floorDiv(err[c] * t.weight, method.divisor);
                }
            }
        break;
    }
    case Kind::Riemersma: {
        // Error diffusion along a Hilbert curve: the error of the last 16 pixels
        // on the curve, the newest counting most, so no direction shows.
        constexpr int kQueue = 16;
        float weights[kQueue];
        for (int i = 0; i < kQueue; ++i)
            weights[i] = float(std::exp(std::log(16.0) * i / (kQueue - 1))) / 16.0f * float(strength);
        float queue[kQueue][3] = {};
        for (const auto& [x, y] : hilbertPath(sw, sh)) {
            const size_t k = size_t(y) * size_t(sw) + size_t(x);
            float add[3] = {0, 0, 0};
            for (int i = 0; i < kQueue; ++i)
                for (int c = 0; c < 3; ++c)
                    add[c] += queue[i][c] * weights[i];
            const int i = colours.nearest(rgb[k * 3] + add[0], rgb[k * 3 + 1] + add[1], rgb[k * 3 + 2] + add[2]);
            index[k] = i;
            const uint32_t pc = colours.color(i);
            // (The error of the picture itself, not of the corrected value: that would feed back.)
            std::copy(&queue[1][0], &queue[kQueue][0], &queue[0][0]);
            queue[kQueue - 1][0] = rgb[k * 3] - float((pc >> 16) & 0xff);
            queue[kQueue - 1][1] = rgb[k * 3 + 1] - float((pc >> 8) & 0xff);
            queue[kQueue - 1][2] = rgb[k * 3 + 2] - float(pc & 0xff);
        }
        break;
    }
    }
    forRows(img, [&](int y, uint32_t* row) {
        for (int x = 0; x < w; ++x) {
            const size_t k = size_t(y / cell) * size_t(sw) + size_t(x / cell);
            const uint32_t c = colours.color(index[k]);
            const int a = alpha[k];
            row[x] = pack(a, int((c >> 16) & 0xff) * a / 255, int((c >> 8) & 0xff) * a / 255, int(c & 0xff) * a / 255);
        }
    });
}

} // namespace dither
