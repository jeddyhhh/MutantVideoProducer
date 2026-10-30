#include "core/OpticalFlow.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace flow {
namespace {
constexpr int kBlock = 8, kLevels = 3;

struct Grey {
    int w = 0, h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; }
};

Grey grey(const QImage& img, QSize size)
{
    const QImage s = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_Grayscale8);
    Grey g{s.width(), s.height(), std::vector<float>(size_t(s.width()) * size_t(s.height()))};
    for (int y = 0; y < g.h; ++y) {
        const uchar* row = s.constScanLine(y);
        for (int x = 0; x < g.w; ++x)
            g.v[size_t(y) * size_t(g.w) + size_t(x)] = row[x];
    }
    return g;
}

Grey half(const Grey& g)
{
    Grey h{std::max(1, g.w / 2), std::max(1, g.h / 2), {}};
    h.v.resize(size_t(h.w) * size_t(h.h));
    for (int y = 0; y < h.h; ++y)
        for (int x = 0; x < h.w; ++x)
            h.v[size_t(y) * size_t(h.w) + size_t(x)] = 0.25f * (g.at(2 * x, 2 * y) + g.at(2 * x + 1, 2 * y) + g.at(2 * x, 2 * y + 1) + g.at(2 * x + 1, 2 * y + 1));
    return h;
}

// Block matches at one level of the pyramid, around a guess (cells of kBlock pixels).
void match(const Grey& a, const Grey& b, int cw, int ch, std::vector<float>& vx, std::vector<float>& vy, int radius)
{
    for (int cy = 0; cy < ch; ++cy)
        for (int cx = 0; cx < cw; ++cx) {
            const size_t i = size_t(cy) * size_t(cw) + size_t(cx);
            const int gx = int(std::lround(vx[i])), gy = int(std::lround(vy[i]));
            const int x0 = cx * kBlock, y0 = cy * kBlock;
            float best = 1e30f;
            int bx = gx, by = gy;
            for (int dy = gy - radius; dy <= gy + radius; ++dy)
                for (int dx = gx - radius; dx <= gx + radius; ++dx) {
                    float sad = 0;
                    for (int y = 0; y < kBlock && sad < best; ++y)
                        for (int x = 0; x < kBlock; ++x)
                            sad += std::abs(a.at(x0 + x, y0 + y) - b.at(x0 + x + dx, y0 + y + dy));
                    sad += 0.5f * float(std::abs(dx - gx) + std::abs(dy - gy)); // (a little against wandering from the guess)
                    if (sad < best) {
                        best = sad;
                        bx = dx;
                        by = dy;
                    }
                }
            vx[i] = float(bx);
            vy[i] = float(by);
        }
}

// Odd vectors out: each the median of the 3x3 around it (a mean would smear a small moving thing into what stands still).
void smooth(int w, int h, std::vector<float>& v)
{
    std::vector<float> m(v.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float n[9];
            int k = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                    n[k++] = v[size_t(std::clamp(y + j, 0, h - 1)) * size_t(w) + size_t(std::clamp(x + i, 0, w - 1))];
            std::nth_element(n, n + 4, n + 9);
            m[size_t(y) * size_t(w) + size_t(x)] = n[4];
        }
    v = std::move(m);
}

inline uint32_t sample(const QImage& img, float x, float y)
{
    const int w = img.width(), h = img.height();
    x = std::clamp(x, 0.f, float(w - 1));
    y = std::clamp(y, 0.f, float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - float(x0), fy = y - float(y0);
    const auto* r0 = reinterpret_cast<const uint32_t*>(img.constScanLine(y0));
    const auto* r1 = reinterpret_cast<const uint32_t*>(img.constScanLine(y1));
    const uint32_t p[4] = {r0[x0], r0[x1], r1[x0], r1[x1]};
    const float wt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    uint32_t out = 0;
    for (int c = 0; c < 32; c += 8) {
        float s = 0;
        for (int k = 0; k < 4; ++k)
            s += wt[k] * float((p[k] >> c) & 0xff);
        out |= uint32_t(std::min(255.f, s + 0.5f)) << c;
    }
    return out;
}
inline uint32_t nearest(const QImage& img, float x, float y)
{
    const int px = std::clamp(int(x + 0.5f), 0, img.width() - 1), py = std::clamp(int(y + 0.5f), 0, img.height() - 1);
    return reinterpret_cast<const uint32_t*>(img.constScanLine(py))[px];
}
} // namespace

QSize analysisSize(QSize picture)
{
    const int w = std::min(320, std::max(1, picture.width()));
    return QSize(w, std::max(1, int(std::lround(double(picture.height()) * w / std::max(1, picture.width())))));
}

Field estimate(const QImage& a, const QImage& b)
{
    if (a.isNull() || b.isNull() || a.size() != b.size())
        return {};
    const QSize size = analysisSize(a.size());
    Grey pa[kLevels], pb[kLevels];
    pa[0] = grey(a, size);
    pb[0] = grey(b, size);
    for (int l = 1; l < kLevels; ++l) {
        pa[l] = half(pa[l - 1]);
        pb[l] = half(pb[l - 1]);
    }
    // Coarse to fine: the smallest level searches widest, each finer one refines the guess doubled up from the last.
    std::vector<float> vx, vy;
    int cw = 0, ch = 0;
    for (int l = kLevels - 1; l >= 0; --l) {
        const int nw = std::max(1, (pa[l].w + kBlock - 1) / kBlock), nh = std::max(1, (pa[l].h + kBlock - 1) / kBlock);
        std::vector<float> nx(size_t(nw) * size_t(nh), 0.f), ny(nx.size(), 0.f);
        if (!vx.empty())
            for (int y = 0; y < nh; ++y)
                for (int x = 0; x < nw; ++x) {
                    const size_t from = size_t(std::min(y * ch / nh, ch - 1)) * size_t(cw) + size_t(std::min(x * cw / nw, cw - 1));
                    nx[size_t(y) * size_t(nw) + size_t(x)] = vx[from] * 2;
                    ny[size_t(y) * size_t(nw) + size_t(x)] = vy[from] * 2;
                }
        match(pa[l], pb[l], nw, nh, nx, ny, l == kLevels - 1 ? 6 : 2);
        smooth(nw, nh, nx);
        smooth(nw, nh, ny);
        vx = std::move(nx);
        vy = std::move(ny);
        cw = nw;
        ch = nh;
    }
    Field f{cw, ch, std::move(vx), std::move(vy)};
    for (float& v : f.dx)
        v /= float(size.width());
    for (float& v : f.dy)
        v /= float(size.height());
    return f;
}

QImage interpolate(const QImage& a0, const QImage& b0, const Field& f, float w)
{
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32_Premultiplied), b = b0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (f.empty() || a.size() != b.size())
        return a;
    QImage out(a.size(), QImage::Format_ARGB32_Premultiplied);
    const int W = a.width(), H = a.height();
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* o = reinterpret_cast<uint32_t*>(out.scanLine(y));
            const float fy = std::clamp((float(y) + 0.5f) / float(H) * float(f.h) - 0.5f, 0.f, float(f.h - 1));
            const int cy0 = int(fy), cy1 = std::min(cy0 + 1, f.h - 1);
            const float ty = fy - float(cy0);
            for (int x = 0; x < W; ++x) {
                const float fx = std::clamp((float(x) + 0.5f) / float(W) * float(f.w) - 0.5f, 0.f, float(f.w - 1));
                const int cx0 = int(fx), cx1 = std::min(cx0 + 1, f.w - 1);
                const float tx = fx - float(cx0);
                auto lerp = [&](const std::vector<float>& v) {
                    const float top = v[size_t(cy0) * size_t(f.w) + size_t(cx0)] * (1 - tx) + v[size_t(cy0) * size_t(f.w) + size_t(cx1)] * tx;
                    const float bottom = v[size_t(cy1) * size_t(f.w) + size_t(cx0)] * (1 - tx) + v[size_t(cy1) * size_t(f.w) + size_t(cx1)] * tx;
                    return top * (1 - ty) + bottom * ty;
                };
                // The motion here: of the blend of the cells around and each of the 25 nearest, the one that takes the two frames to
                // the same colour (at the edge of something moving, the blend says half-moving and shows a ghost of it).
                float vx = lerp(f.dx) * float(W), vy = lerp(f.dy) * float(H);
                auto mismatch = [&](float mx, float my) {
                    const uint32_t ca = nearest(a, float(x) - w * mx, float(y) - w * my), cb = nearest(b, float(x) + (1 - w) * mx, float(y) + (1 - w) * my);
                    int d = 0;
                    for (int c = 0; c < 24; c += 8)
                        d += std::abs(int((ca >> c) & 0xff) - int((cb >> c) & 0xff));
                    return d;
                };
                int best = mismatch(vx, vy);
                const int nx = int(fx + 0.5f), ny = int(fy + 0.5f); // (the nearest cell, and two around it each way)
                for (int k = 0; k < 25 && best > 24; ++k) {
                    const size_t cell = size_t(std::clamp(ny + k / 5 - 2, 0, f.h - 1)) * size_t(f.w) + size_t(std::clamp(nx + k % 5 - 2, 0, f.w - 1));
                    const float mx = f.dx[cell] * float(W), my = f.dy[cell] * float(H);
                    if (const int d = mismatch(mx, my); d + 12 < best) {
                        best = d;
                        vx = mx;
                        vy = my;
                    }
                }
                const uint32_t pa = sample(a, float(x) - w * vx, float(y) - w * vy);
                const uint32_t pb = sample(b, float(x) + (1 - w) * vx, float(y) + (1 - w) * vy);
                uint32_t px = 0;
                for (int c = 0; c < 32; c += 8)
                    px |= uint32_t(std::lround(float((pa >> c) & 0xff) * (1 - w) + float((pb >> c) & 0xff) * w)) << c;
                o[x] = px;
            }
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(rows, H * t / threads, H * (t + 1) / threads);
    for (auto& t : pool)
        t.join();
    return out;
}

QImage denoise(const QImage& frame, const QImage* prev0, const Field* toPrev, const QImage* next0, const Field* toNext, double strength)
{
    const QImage cur = frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    auto usable = [&](const QImage* img, const Field* f) {
        return img && f && !f->empty() && img->size() == cur.size() ? img->convertToFormat(QImage::Format_ARGB32_Premultiplied) : QImage();
    };
    const QImage prev = usable(prev0, toPrev), next = usable(next0, toNext);
    const int W = cur.width(), H = cur.height();
    const float sim = denoiseSimilarity(strength), inv = 1.0f / (2.0f * sim * sim);
    static const float spatial[3] = {1.0f, 0.6f, 0.37f}; // (by how far from the middle: none, a side, a corner)
    QImage out(cur.size(), QImage::Format_ARGB32_Premultiplied);
    // The motion at a pixel, in pixels (as interpolate: between the cells around it).
    auto motion = [&](const Field& f, int x, int y, float& vx, float& vy) {
        const float fy = std::clamp((float(y) + 0.5f) / float(H) * float(f.h) - 0.5f, 0.f, float(f.h - 1));
        const float fx = std::clamp((float(x) + 0.5f) / float(W) * float(f.w) - 0.5f, 0.f, float(f.w - 1));
        const int cx0 = int(fx), cy0 = int(fy), cx1 = std::min(cx0 + 1, f.w - 1), cy1 = std::min(cy0 + 1, f.h - 1);
        const float tx = fx - float(cx0), ty = fy - float(cy0);
        auto lerp = [&](const std::vector<float>& v) {
            const float top = v[size_t(cy0) * size_t(f.w) + size_t(cx0)] * (1 - tx) + v[size_t(cy0) * size_t(f.w) + size_t(cx1)] * tx;
            const float bottom = v[size_t(cy1) * size_t(f.w) + size_t(cx0)] * (1 - tx) + v[size_t(cy1) * size_t(f.w) + size_t(cx1)] * tx;
            return top * (1 - ty) + bottom * ty;
        };
        vx = lerp(f.dx) * float(W);
        vy = lerp(f.dy) * float(H);
    };
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* o = reinterpret_cast<uint32_t*>(out.scanLine(y));
            for (int x = 0; x < W; ++x) {
                const uint32_t cp = nearest(cur, float(x), float(y));
                float c[4], sum[4] = {0, 0, 0, 0}, total = 0;
                for (int k = 0; k < 4; ++k)
                    c[k] = float((cp >> (8 * k)) & 0xff) / 255.0f;
                auto taps = [&](const QImage& img, float ax, float ay, bool exact) {
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const uint32_t q = exact ? nearest(img, ax + float(dx), ay + float(dy)) : sample(img, ax + float(dx), ay + float(dy));
                            float v[4], d2 = 0;
                            for (int k = 0; k < 4; ++k) {
                                v[k] = float((q >> (8 * k)) & 0xff) / 255.0f;
                                if (k < 3)
                                    d2 += (v[k] - c[k]) * (v[k] - c[k]);
                            }
                            const float w = spatial[std::abs(dx) + std::abs(dy)] * std::exp(-d2 / 3.0f * inv);
                            for (int k = 0; k < 4; ++k)
                                sum[k] += w * v[k];
                            total += w;
                        }
                };
                taps(cur, float(x), float(y), true);
                float vx = 0, vy = 0;
                if (!prev.isNull()) {
                    motion(*toPrev, x, y, vx, vy);
                    taps(prev, float(x) + vx, float(y) + vy, false);
                }
                if (!next.isNull()) {
                    motion(*toNext, x, y, vx, vy);
                    taps(next, float(x) + vx, float(y) + vy, false);
                }
                uint32_t px = 0;
                for (int k = 0; k < 4; ++k)
                    px |= uint32_t(std::clamp(int(sum[k] / total * 255.0f + 0.5f), 0, 255)) << (8 * k);
                o[x] = px;
            }
        }
    };
    const int threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back(rows, H * t / threads, H * (t + 1) / threads);
    for (auto& t : pool)
        t.join();
    return out;
}

} // namespace flow
