#include "core/ImageOps.h"

#include "core/Parallel.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace imageops {

namespace {

inline uint32_t unpremul(uint32_t p)
{
    const uint32_t a = p >> 24;
    if (a == 255 || a == 0)
        return p;
    const uint32_t r = ((p >> 16) & 0xff) * 255 / a;
    const uint32_t g = ((p >> 8) & 0xff) * 255 / a;
    const uint32_t b = (p & 0xff) * 255 / a;
    return (a << 24) | (std::min(r, 255u) << 16) | (std::min(g, 255u) << 8) | std::min(b, 255u);
}

inline uint32_t premul(uint32_t p)
{
    const uint32_t a = p >> 24;
    if (a == 255)
        return p;
    if (a == 0)
        return 0;
    const uint32_t r = ((p >> 16) & 0xff) * a / 255;
    const uint32_t g = ((p >> 8) & 0xff) * a / 255;
    const uint32_t b = (p & 0xff) * a / 255;
    return (a << 24) | (r << 16) | (g << 8) | b;
}

} // namespace

void forEachPixel(QImage& img, const std::function<void(int, uint32_t*, int)>& rowFn)
{
    const int w = img.width();
    uchar* bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
            for (int x = 0; x < w; ++x)
                row[x] = unpremul(row[x]);
            rowFn(y, row, w);
            for (int x = 0; x < w; ++x)
                row[x] = premul(row[x]);
        }
    });
}

void applyLuts(QImage& img, const Lut& lr, const Lut& lg, const Lut& lb)
{
    forEachPixel(img, [&](int, uint32_t* row, int w) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            if (!(p >> 24))
                continue;
            row[x] = (p & 0xff000000u) | (uint32_t(lr[(p >> 16) & 0xff]) << 16) | (uint32_t(lg[(p >> 8) & 0xff]) << 8)
                     | lb[p & 0xff];
        }
    });
}

void applyColorMatrix(QImage& img, const float m[12])
{
    // Fixed point 8.8 matrix for speed; offsets scaled to 0..255.
    int fm[12];
    for (int i = 0; i < 12; ++i)
        fm[i] = int(std::lround(m[i] * ((i % 4 == 3) ? 255.0f * 256.0f : 256.0f)));
    forEachPixel(img, [&](int, uint32_t* row, int w) {
        for (int x = 0; x < w; ++x) {
            const uint32_t p = row[x];
            if (!(p >> 24))
                continue;
            const int r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;
            const int nr = (fm[0] * r + fm[1] * g + fm[2] * b + fm[3]) >> 8;
            const int ng = (fm[4] * r + fm[5] * g + fm[6] * b + fm[7]) >> 8;
            const int nb = (fm[8] * r + fm[9] * g + fm[10] * b + fm[11]) >> 8;
            row[x] = (p & 0xff000000u) | (uint32_t(clamp8(nr)) << 16) | (uint32_t(clamp8(ng)) << 8) | clamp8(nb);
        }
    });
}

namespace {

// One horizontal box pass over rows [y0, y1); clamps at the edges.
void boxRowPass(uchar* bits, qsizetype bpl, int w, int y0, int y1, int r, std::vector<uint32_t>& tmp)
{
    const int win = 2 * r + 1;
    const uint32_t inv = (1u << 16) / uint32_t(win);
    tmp.resize(size_t(w));
    for (int y = y0; y < y1; ++y) {
        auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
        uint32_t s[4] = {0, 0, 0, 0};
        for (int i = -r; i <= r; ++i) {
            const uint32_t p = row[std::clamp(i, 0, w - 1)];
            s[0] += p & 0xff;
            s[1] += (p >> 8) & 0xff;
            s[2] += (p >> 16) & 0xff;
            s[3] += p >> 24;
        }
        for (int x = 0; x < w; ++x) {
            tmp[size_t(x)] = ((s[0] * inv) >> 16) | (((s[1] * inv) >> 16) << 8) | (((s[2] * inv) >> 16) << 16)
                             | (((s[3] * inv) >> 16) << 24);
            const uint32_t add = row[std::min(x + r + 1, w - 1)];
            const uint32_t sub = row[std::max(x - r, 0)];
            s[0] += (add & 0xff) - (sub & 0xff);
            s[1] += ((add >> 8) & 0xff) - ((sub >> 8) & 0xff);
            s[2] += ((add >> 16) & 0xff) - ((sub >> 16) & 0xff);
            s[3] += (add >> 24) - (sub >> 24);
        }
        std::copy(tmp.begin(), tmp.end(), row);
    }
}

// One vertical box pass over columns [x0, x1), processed as a strip so
// memory is read row by row.
void boxColumnPass(uchar* bits, qsizetype bpl, int h, int x0, int x1, int r, std::vector<uint32_t>& sums,
                   std::vector<uint32_t>& out)
{
    const int win = 2 * r + 1;
    const uint32_t inv = (1u << 16) / uint32_t(win);
    const int n = x1 - x0;
    sums.assign(size_t(n) * 4, 0);
    out.resize(size_t(n) * size_t(h));
    auto px = [&](int y, int x) { return reinterpret_cast<const uint32_t*>(bits + bpl * std::clamp(y, 0, h - 1))[x]; };
    for (int i = -r; i <= r; ++i)
        for (int x = 0; x < n; ++x) {
            const uint32_t p = px(i, x0 + x);
            uint32_t* s = &sums[size_t(x) * 4];
            s[0] += p & 0xff;
            s[1] += (p >> 8) & 0xff;
            s[2] += (p >> 16) & 0xff;
            s[3] += p >> 24;
        }
    for (int y = 0; y < h; ++y) {
        const auto* addRow = reinterpret_cast<const uint32_t*>(bits + bpl * std::min(y + r + 1, h - 1));
        const auto* subRow = reinterpret_cast<const uint32_t*>(bits + bpl * std::max(y - r, 0));
        uint32_t* o = &out[size_t(y) * size_t(n)];
        for (int x = 0; x < n; ++x) {
            uint32_t* s = &sums[size_t(x) * 4];
            o[x] = ((s[0] * inv) >> 16) | (((s[1] * inv) >> 16) << 8) | (((s[2] * inv) >> 16) << 16)
                   | (((s[3] * inv) >> 16) << 24);
            const uint32_t add = addRow[x0 + x], sub = subRow[x0 + x];
            s[0] += (add & 0xff) - (sub & 0xff);
            s[1] += ((add >> 8) & 0xff) - ((sub >> 8) & 0xff);
            s[2] += ((add >> 16) & 0xff) - ((sub >> 16) & 0xff);
            s[3] += (add >> 24) - (sub >> 24);
        }
    }
    for (int y = 0; y < h; ++y)
        std::copy_n(&out[size_t(y) * size_t(n)], n, reinterpret_cast<uint32_t*>(bits + bpl * y) + x0);
}

} // namespace

void boxBlur(QImage& img, int rx, int ry, int passes)
{
    const int w = img.width(), h = img.height();
    if (w < 2 || h < 2)
        return;
    uchar* bits = img.bits();
    const qsizetype bpl = img.bytesPerLine();
    rx = std::min(rx, w - 1);
    ry = std::min(ry, h - 1);
    for (int pass = 0; pass < passes; ++pass) {
        if (rx > 0)
            parallelFor(h, [&](int y0, int y1) {
                std::vector<uint32_t> tmp;
                boxRowPass(bits, bpl, w, y0, y1, rx, tmp);
            });
        if (ry > 0) {
            constexpr int kStrip = 32;
            const int strips = (w + kStrip - 1) / kStrip;
            parallelFor(strips, [&](int s0, int s1) {
                std::vector<uint32_t> sums, out;
                for (int s = s0; s < s1; ++s)
                    boxColumnPass(bits, bpl, h, s * kStrip, std::min(w, (s + 1) * kStrip), ry, sums, out);
            }, 1);
        }
    }
}

void gaussianBlur(QImage& img, double sigmaX, double sigmaY)
{
    // Three box passes whose combined variance matches the Gaussian.
    auto radii = [](double sigma, int out[3]) {
        if (sigma < 0.3) {
            out[0] = out[1] = out[2] = 0;
            return;
        }
        const int n = 3;
        int wl = int(std::floor(std::sqrt(12.0 * sigma * sigma / n + 1.0)));
        if (wl % 2 == 0)
            --wl;
        const int wu = wl + 2;
        const int m = int(std::lround((12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) / (-4.0 * wl - 4.0)));
        for (int i = 0; i < n; ++i)
            out[i] = ((i < m ? wl : wu) - 1) / 2;
    };
    int rx[3], ry[3];
    radii(sigmaX, rx);
    radii(sigmaY, ry);
    for (int i = 0; i < 3; ++i)
        boxBlur(img, rx[i], ry[i], 1);
}

void maskBlend(QImage& out, const QImage* from, const QImage* to, const std::function<float(int, int)>& mask)
{
    const int w = out.width();
    uchar* ob = out.bits();
    const qsizetype bpl = out.bytesPerLine();
    parallelFor(out.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* o = reinterpret_cast<uint32_t*>(ob + bpl * y);
            const auto* a = from ? reinterpret_cast<const uint32_t*>(from->constScanLine(y)) : nullptr;
            const auto* b = to ? reinterpret_cast<const uint32_t*>(to->constScanLine(y)) : nullptr;
            for (int x = 0; x < w; ++x) {
                const float m = mask(x, y);
                const uint32_t pa = a ? a[x] : 0u, pb = b ? b[x] : 0u;
                if (m <= 0.0f) {
                    o[x] = pa;
                } else if (m >= 1.0f) {
                    o[x] = pb;
                } else {
                    const uint32_t t = uint32_t(m * 256.0f), s = 256 - t;
                    const uint32_t rb = (((pa & 0x00ff00ffu) * s + (pb & 0x00ff00ffu) * t) >> 8) & 0x00ff00ffu;
                    const uint32_t ag = ((((pa >> 8) & 0x00ff00ffu) * s + ((pb >> 8) & 0x00ff00ffu) * t)) & 0xff00ff00u;
                    o[x] = rb | ag;
                }
            }
        }
    });
}

void mix(QImage& out, const QImage* from, const QImage* to, float t)
{
    maskBlend(out, from, to, [t](int, int) { return t; });
}

namespace {
inline float blendChannel(int mode, float b, float s)
{
    switch (mode) {
    case 1: return std::min(1.0f, b + s);                                        // Add
    case 2: return b + s - b * s;                                                // Screen
    case 3: return b * s;                                                        // Multiply
    case 4: return b <= 0.5f ? 2.0f * b * s : 1.0f - 2.0f * (1.0f - b) * (1.0f - s); // Overlay
    case 5: return std::max(b, s);                                               // Lighten
    case 6: return std::min(b, s);                                               // Darken
    case 7: return std::abs(b - s);                                              // Difference
    default: return s;                                                           // Normal
    }
}
} // namespace

void applyMatte(QImage& layer, const QImage& matte, int mode)
{
    if (layer.size() != matte.size() || mode <= 0)
        return;
    const int w = layer.width();
    parallelFor(layer.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* l = reinterpret_cast<uint32_t*>(layer.scanLine(y));
            const auto* m = reinterpret_cast<const uint32_t*>(matte.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t mp = m[x];
                // (premultiplied: luma counts what isn't drawn as black)
                float k = mode <= 2 ? float(mp >> 24) / 255.0f
                                    : (0.2126f * float((mp >> 16) & 0xff) + 0.7152f * float((mp >> 8) & 0xff) + 0.0722f * float(mp & 0xff)) / 255.0f;
                if (mode == 2 || mode == 4)
                    k = 1.0f - k;
                const uint32_t lp = l[x];
                uint32_t out = 0;
                for (int c = 0; c < 32; c += 8)
                    out |= uint32_t(float((lp >> c) & 0xff) * k + 0.5f) << c;
                l[x] = out;
            }
        }
    });
}

void blendLayer(QImage& base, const QImage& layer, float opacity, int mode)
{
    if (base.size() != layer.size() || opacity <= 0.0f)
        return;
    opacity = std::min(opacity, 1.0f);
    const int w = base.width();
    parallelFor(base.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* b = reinterpret_cast<uint32_t*>(base.scanLine(y));
            const auto* l = reinterpret_cast<const uint32_t*>(layer.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t lp = l[x];
                const float la = float(lp >> 24) / 255.0f;
                if (la <= 0.0f)
                    continue;
                const uint32_t bp = b[x];
                const float ba = float(bp >> 24) / 255.0f;
                const float a = la * opacity;
                float out[3];
                for (int c = 0; c < 3; ++c) {
                    const int shift = 16 - 8 * c;
                    const float bc = float((bp >> shift) & 0xff) / 255.0f, lc = float((lp >> shift) & 0xff) / 255.0f;
                    const float cb = ba > 0 ? bc / ba : 0.0f, cs = lc / la;
                    const float mixed = (1.0f - ba) * cs + ba * blendChannel(mode, cb, cs);
                    out[c] = mixed * a + bc * (1.0f - a);
                }
                const float oa = a + ba * (1.0f - a);
                auto q = [](float v) { return uint32_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)); };
                b[x] = (q(oa) << 24) | (q(out[0]) << 16) | (q(out[1]) << 8) | q(out[2]);
            }
        }
    });
}

} // namespace imageops
