// Built-in video effects. Each works in place on a premultiplied ARGB32
// layer the size of the output canvas. Spatial parameters are given in
// project pixels and scaled by RenderContext::scale so previews at reduced
// resolution look the same as the final render.

#include "core/Effects.h"
#include "core/EffectHelpers.h"
#include "core/ConsoleEngine.h"
#include "core/Segmentation.h"
#include "core/CubeLut.h"
#include "core/FilterGraph.h"
#include "core/ImageOps.h"
#include "core/Parallel.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace imageops;
using namespace fx;

namespace {

// ---- Colour matrices (3x4, row-major, 0..1 colour) ------------------------

struct Matrix {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
};

Matrix multiply(const Matrix& a, const Matrix& b) // a after b
{
    Matrix r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j)
            r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j];
        r.m[i * 4 + 3] = a.m[i * 4] * b.m[3] + a.m[i * 4 + 1] * b.m[7] + a.m[i * 4 + 2] * b.m[11] + a.m[i * 4 + 3];
    }
    return r;
}

Matrix lerp(const Matrix& a, const Matrix& b, float t)
{
    Matrix r;
    for (int i = 0; i < 12; ++i)
        r.m[i] = a.m[i] + (b.m[i] - a.m[i]) * t;
    return r;
}

Matrix saturationMatrix(float s)
{
    return Matrix{{0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0,
                   0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0,
                   0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0}};
}

Matrix hueMatrix(float degrees)
{
    const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
    return Matrix{{0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f, 0,
                   0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f, 0,
                   0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f, 0}};
}

Matrix lightnessMatrix(float l)
{
    // l > 0 blends towards white, l < 0 towards black.
    if (l >= 0)
        return Matrix{{1 - l, 0, 0, l, 0, 1 - l, 0, l, 0, 0, 1 - l, l}};
    return Matrix{{1 + l, 0, 0, 0, 0, 1 + l, 0, 0, 0, 0, 1 + l, 0}};
}

// Monotone cubic curve (Fritsch-Carlson) through points with increasing x:
// smooth, and no overshoot between points that rise or fall steadily.
double smoothCurve(const std::vector<std::pair<double, double>>& pts, double x)
{
    const size_t n = pts.size();
    if (x <= pts.front().first)
        return pts.front().second;
    if (x >= pts.back().first)
        return pts.back().second;
    std::vector<double> d(n - 1), m(n);
    for (size_t i = 0; i + 1 < n; ++i)
        d[i] = (pts[i + 1].second - pts[i].second) / (pts[i + 1].first - pts[i].first);
    m[0] = d[0];
    m[n - 1] = d[n - 2];
    for (size_t i = 1; i + 1 < n; ++i)
        m[i] = d[i - 1] * d[i] <= 0 ? 0.0 : (d[i - 1] + d[i]) / 2;
    for (size_t i = 0; i + 1 < n; ++i) {
        if (d[i] == 0) {
            m[i] = m[i + 1] = 0;
            continue;
        }
        const double a = m[i] / d[i], b = m[i + 1] / d[i], h = a * a + b * b;
        if (h > 9) {
            const double t = 3 / std::sqrt(h);
            m[i] = t * a * d[i];
            m[i + 1] = t * b * d[i];
        }
    }
    size_t i = 0;
    while (x > pts[i + 1].first)
        ++i;
    const double h = pts[i + 1].first - pts[i].first, t = (x - pts[i].first) / h;
    const double t2 = t * t, t3 = t2 * t;
    return (2 * t3 - 3 * t2 + 1) * pts[i].second + (t3 - 2 * t2 + t) * h * m[i] + (-2 * t3 + 3 * t2) * pts[i + 1].second
           + (t3 - t2) * h * m[i + 1];
}

Lut makeLut(const std::function<double(double)>& f)
{
    Lut lut{};
    for (int i = 0; i < 256; ++i)
        lut[size_t(i)] = clamp8(int(std::lround(f(i / 255.0) * 255.0)));
    return lut;
}

// ---- Effects ----------------------------------------------------------------

void applyTransform(QImage& layer, const Params& p, const RenderContext& ctx)
{
    const double scale = p.num(QStringLiteral("scale"));
    const double x = p.num(QStringLiteral("x")), y = p.num(QStringLiteral("y"));
    const double rot = p.num(QStringLiteral("rotation"));
    const double cl = p.num(QStringLiteral("cropLeft")), cr = p.num(QStringLiteral("cropRight"));
    const double ct = p.num(QStringLiteral("cropTop")), cb = p.num(QStringLiteral("cropBottom"));
    const double opacity = p.num(QStringLiteral("opacity"));
    const double border = p.num(QStringLiteral("border")) * ctx.scale;
    if (scale == 1.0 && x == 0 && y == 0 && rot == 0 && cl + cr + ct + cb == 0 && opacity >= 1.0 && border <= 0)
        return;

    const QRectF content = ctx.contentRect.isEmpty() ? QRectF(layer.rect()) : ctx.contentRect;
    const QRectF crop(content.left() + cl * content.width(), content.top() + ct * content.height(),
                      std::max(1.0, content.width() * (1.0 - cl - cr)), std::max(1.0, content.height() * (1.0 - ct - cb)));
    const double W = layer.width(), H = layer.height();

    QImage out(layer.size(), QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.setRenderHint(QPainter::Antialiasing, rot != 0);
    // Position offsets are fractions of the frame; transforms pivot on the frame centre.
    painter.translate(W / 2 + x * W, H / 2 + y * H);
    painter.rotate(rot);
    painter.scale(scale, scale);
    painter.translate(-W / 2, -H / 2);
    painter.setOpacity(opacity);
    painter.drawImage(crop.topLeft(), layer, crop);
    if (border > 0) {
        QPen pen(p.color(QStringLiteral("borderColor")), border / std::max(0.01, scale));
        pen.setJoinStyle(Qt::MiterJoin);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        const double half = pen.widthF() / 2;
        painter.drawRect(crop.adjusted(-half, -half, half, half));
    }
    painter.end();
    layer = std::move(out);
}

void applyChromaKey(QImage& layer, const Params& p)
{
    const QColor key = p.color(QStringLiteral("key"));
    const float tol = float(p.num(QStringLiteral("tolerance")));
    const float soft = float(p.num(QStringLiteral("softness")));
    const float spill = float(p.num(QStringLiteral("spill")));
    const bool showMask = p.flag(QStringLiteral("showMask"));
    auto cbcr = [](float r, float g, float b, float& cb, float& cr) {
        cb = -0.1146f * r - 0.3854f * g + 0.5f * b;
        cr = 0.5f * r - 0.4542f * g - 0.0458f * b;
    };
    float kcb, kcr;
    cbcr(key.redF(), key.greenF(), key.blueF(), kcb, kcr);
    forEachPixel(layer, [&](int, uint32_t* row, int w) {
        for (int x = 0; x < w; ++x) {
            const uint32_t px = row[x];
            const uint32_t a0 = px >> 24;
            if (!a0)
                continue;
            float r = ((px >> 16) & 0xff) / 255.0f, g = ((px >> 8) & 0xff) / 255.0f, b = (px & 0xff) / 255.0f;
            float cb, cr;
            cbcr(r, g, b, cb, cr);
            const float dist = std::hypot(cb - kcb, cr - kcr) / 0.7f;
            const float alpha = smoothstep(tol, tol + soft + 1e-4f, dist);
            // Spill suppression: pull colours near the key towards grey.
            const float s = spill * (1.0f - smoothstep(tol, tol + soft + 0.35f, dist));
            if (s > 0) {
                const float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                r += (luma - r) * s;
                g += (luma - g) * s;
                b += (luma - b) * s;
            }
            const uint32_t a = uint32_t(a0 * alpha + 0.5f);
            if (showMask) {
                const uint32_t m = a;
                row[x] = 0xff000000u | (m << 16) | (m << 8) | m;
            } else {
                row[x] = (a << 24) | (uint32_t(clamp8(int(r * 255 + 0.5f))) << 16) | (uint32_t(clamp8(int(g * 255 + 0.5f))) << 8)
                         | clamp8(int(b * 255 + 0.5f));
            }
        }
    });
}

void applyVignette(QImage& layer, const Params& p)
{
    const float amount = float(p.num(QStringLiteral("amount")));
    const float size = float(p.num(QStringLiteral("size")));
    const float soft = float(p.num(QStringLiteral("softness")));
    const float round = float(p.num(QStringLiteral("roundness")));
    if (amount == 0)
        return;
    const float w = float(layer.width()), h = float(layer.height());
    const float xScale = (1.0f - round) * (2.0f / w) + round * (2.0f / h);
    const float yScale = 2.0f / h;
    const float inner = size * (1.0f - soft);
    forEachPixel(layer, [&](int y, uint32_t* row, int width) {
        const float dy = (y + 0.5f - h / 2) * yScale;
        for (int x = 0; x < width; ++x) {
            const uint32_t px = row[x];
            if (!(px >> 24))
                continue;
            const float dx = (x + 0.5f - w / 2) * xScale;
            const float v = smoothstep(inner, size + 1e-4f, std::sqrt(dx * dx + dy * dy) / 1.41421f);
            int c[3] = {int((px >> 16) & 0xff), int((px >> 8) & 0xff), int(px & 0xff)};
            for (int& ch : c)
                ch = amount > 0 ? int(ch * (1.0f - amount * v)) : int(ch + (255 - ch) * (-amount) * v);
            row[x] = (px & 0xff000000u) | (uint32_t(c[0]) << 16) | (uint32_t(c[1]) << 8) | uint32_t(c[2]);
        }
    });
}

void applySharpen(QImage& layer, const Params& p, const RenderContext& ctx)
{
    const float amount = float(p.num(QStringLiteral("amount")));
    const double radius = p.num(QStringLiteral("radius")) * ctx.scale;
    if (amount <= 0)
        return;
    QImage blurred = layer.copy();
    gaussianBlur(blurred, radius, radius);
    const int w = layer.width();
    parallelFor(layer.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(layer.scanLine(y));
            const auto* bl = reinterpret_cast<const uint32_t*>(blurred.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t o = row[x], b = bl[x];
                const int a = int(o >> 24);
                uint32_t out = o & 0xff000000u;
                for (int shift : {16, 8, 0}) {
                    const int co = int((o >> shift) & 0xff), cb = int((b >> shift) & 0xff);
                    out |= uint32_t(std::clamp(int(co + amount * (co - cb)), 0, a)) << shift;
                }
                row[x] = out;
            }
        }
    });
}

void applyGlow(QImage& layer, const Params& p, const RenderContext& ctx)
{
    const float threshold = float(p.num(QStringLiteral("threshold")));
    const double radius = p.num(QStringLiteral("radius")) * ctx.scale;
    const float intensity = float(p.num(QStringLiteral("intensity")));
    if (intensity <= 0)
        return;
    QImage bright = layer.copy();
    const int w = layer.width();
    // Bright pass: keep what's above the threshold, scaled to full range.
    parallelFor(bright.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(bright.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t px = row[x];
                const float luma = (0.2126f * ((px >> 16) & 0xff) + 0.7152f * ((px >> 8) & 0xff) + 0.0722f * (px & 0xff)) / 255.0f;
                const float k = smoothstep(threshold, std::min(1.0f, threshold + 0.15f), luma);
                row[x] = (uint32_t(((px >> 24) & 0xff) * k) << 24) | (uint32_t(((px >> 16) & 0xff) * k) << 16)
                         | (uint32_t(((px >> 8) & 0xff) * k) << 8) | uint32_t((px & 0xff) * k);
            }
        }
    });
    gaussianBlur(bright, radius, radius);
    // Screen the glow over the original.
    parallelFor(layer.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(layer.scanLine(y));
            const auto* g = reinterpret_cast<const uint32_t*>(bright.constScanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t o = row[x], gl = g[x];
                uint32_t out = 0;
                for (int shift : {24, 16, 8, 0}) {
                    const int a = int((o >> shift) & 0xff);
                    const int b = std::min(255, int(((gl >> shift) & 0xff) * intensity));
                    out |= uint32_t(a + b - a * b / 255) << shift;
                }
                row[x] = out;
            }
        }
    });
}

void applyPixelate(QImage& layer, const Params& p, const RenderContext& ctx)
{
    const int block = std::max(1, int(std::lround(p.num(QStringLiteral("size")) * ctx.scale)));
    if (block <= 1)
        return;
    const int w = layer.width(), h = layer.height();
    const int rows = (h + block - 1) / block;
    parallelFor(rows, [&](int r0, int r1) {
        for (int br = r0; br < r1; ++br) {
            const int y0 = br * block, y1 = std::min(h, y0 + block);
            for (int x0 = 0; x0 < w; x0 += block) {
                const int x1 = std::min(w, x0 + block);
                uint64_t s[4] = {0, 0, 0, 0};
                for (int y = y0; y < y1; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(layer.constScanLine(y));
                    for (int x = x0; x < x1; ++x)
                        for (int c = 0; c < 4; ++c)
                            s[c] += (row[x] >> (c * 8)) & 0xff;
                }
                const uint64_t n = uint64_t(x1 - x0) * uint64_t(y1 - y0);
                const uint32_t avg = uint32_t(s[0] / n) | uint32_t(s[1] / n) << 8 | uint32_t(s[2] / n) << 16 | uint32_t(s[3] / n) << 24;
                for (int y = y0; y < y1; ++y) {
                    auto* row = reinterpret_cast<uint32_t*>(layer.scanLine(y));
                    std::fill(row + x0, row + x1, avg);
                }
            }
        }
    }, 1);
}

void applyNoise(QImage& layer, const Params& p, const RenderContext& ctx)
{
    const float amount = float(p.num(QStringLiteral("amount")));
    if (amount <= 0)
        return;
    const bool mono = p.flag(QStringLiteral("monochrome"));
    const uint32_t seed = p.flag(QStringLiteral("animated")) ? uint32_t(std::llround(ctx.timelineTime * ctx.fps / kUsPerSecond)) : 7u;
    const int strength = int(amount * 128);
    const int w = layer.width();
    parallelFor(layer.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(layer.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t px = row[x];
                const int a = int(px >> 24);
                if (!a)
                    continue;
                const uint32_t hsh = hash32(uint32_t(x), uint32_t(y), seed);
                uint32_t out = px & 0xff000000u;
                int i = 0;
                for (int shift : {16, 8, 0}) {
                    const uint32_t bits = mono ? hsh : (hsh >> (i++ * 8)) * 2654435761u;
                    const int n = int((bits >> 8) & 0xff) - 128;
                    out |= uint32_t(std::clamp(int((px >> shift) & 0xff) + n * strength / 128 * a / 255, 0, a)) << shift;
                }
                row[x] = out;
            }
        }
    });
}

void applyFFmpegFilter(QImage& layer, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
{
    if (!state)
        state = std::make_unique<FilterGraphState>();
    auto* fg = static_cast<FilterGraphState*>(state.get());
    QString error;
    layer = fg->process(layer, p.text(QStringLiteral("filter")), ctx.timelineTime, ctx.fps, &error);
    if (!error.isEmpty()) {
        QPainter painter(&layer);
        QFont f = painter.font();
        f.setPixelSize(std::max(10, layer.height() / 30));
        painter.setFont(f);
        painter.fillRect(QRect(0, 0, layer.width(), f.pixelSize() * 2), QColor(120, 0, 0, 200));
        painter.setPen(Qt::white);
        painter.drawText(QRect(6, 0, layer.width() - 12, f.pixelSize() * 2), Qt::AlignVCenter, error);
    }
}

std::vector<EffectDef> buildEffects()
{
    std::vector<EffectDef> list;
    auto add = [&](EffectDef d) { list.push_back(std::move(d)); };
    EffectDef d;

    // ---- Colour -------------------------------------------------------------
    d = {};
    d.id = QStringLiteral("brightness_contrast");
    d.name = QStringLiteral("Brightness and Contrast");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Raise or lower brightness and stretch or flatten contrast.");
    d.params = {num("brightness", "Brightness", -1, 1, 0), num("contrast", "Contrast", -1, 1, 0),
                num("center", "Contrast center", 0, 1, 0.5)};
    d.luts = [](const Params& p, ChannelLuts& l) {
        const double b = p.num(QStringLiteral("brightness")), c = p.num(QStringLiteral("contrast")),
                     ctr = p.num(QStringLiteral("center"));
        if (b == 0 && c == 0)
            return false;
        const double k = c >= 0 ? 1.0 / std::max(0.01, 1.0 - c * 0.99) : 1.0 + c;
        l[0] = l[1] = l[2] = makeLut([&](double v) { return (v - ctr) * k + ctr + b; });
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("levels");
    d.name = QStringLiteral("Levels");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Remap input black/white points and gamma to an output range.");
    d.params = {num("inBlack", "Input black", 0, 1, 0, 3), num("inWhite", "Input white", 0, 1, 1, 3),
                num("gamma", "Gamma", 0.1, 10, 1, 2), num("outBlack", "Output black", 0, 1, 0, 3),
                num("outWhite", "Output white", 0, 1, 1, 3)};
    d.luts = [](const Params& p, ChannelLuts& l) {
        const double ib = p.num(QStringLiteral("inBlack")), iw = p.num(QStringLiteral("inWhite"));
        const double g = p.num(QStringLiteral("gamma")), ob = p.num(QStringLiteral("outBlack")),
                     ow = p.num(QStringLiteral("outWhite"));
        if (ib == 0 && iw == 1 && g == 1 && ob == 0 && ow == 1)
            return false;
        l[0] = l[1] = l[2] = makeLut([&](double v) {
            const double n = std::clamp((v - ib) / std::max(1e-4, iw - ib), 0.0, 1.0);
            return ob + std::pow(n, 1.0 / g) * (ow - ob);
        });
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("hsl");
    d.name = QStringLiteral("Hue, Saturation and Lightness");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Rotate hues, boost or reduce saturation, lighten or darken.");
    d.params = {num("hue", "Hue", -180, 180, 0, 1, "°"), num("saturation", "Saturation", -1, 1, 0),
                num("lightness", "Lightness", -1, 1, 0)};
    d.colorMatrix = [](const Params& p, ColorMatrix& out) {
        const float h = float(p.num(QStringLiteral("hue"))), s = float(p.num(QStringLiteral("saturation"))),
                    l = float(p.num(QStringLiteral("lightness")));
        if (h == 0 && s == 0 && l == 0)
            return false;
        const Matrix m = multiply(lightnessMatrix(l), multiply(saturationMatrix(1.0f + s), hueMatrix(h)));
        std::copy(std::begin(m.m), std::end(m.m), out.begin());
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("color_balance");
    d.name = QStringLiteral("Color Balance");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Shift red, green and blue separately in shadows, midtones and highlights.");
    for (const char* range : {"shadows", "midtones", "highlights"})
        for (const char* ch : {"Red", "Green", "Blue"}) {
            const QString id = QString::fromLatin1(range) + QString::fromLatin1(ch);
            const QString name = QStringLiteral("%1 %2").arg(QString::fromLatin1(range).left(1).toUpper() + QString::fromLatin1(range).mid(1),
                                                             QString::fromLatin1(ch).toLower());
            ParamDef pd = num("", "", -1, 1, 0);
            pd.id = id;
            pd.name = name;
            d.params.push_back(pd);
        }
    d.luts = [](const Params& p, ChannelLuts& l) {
        bool any = false;
        const char* chans[3] = {"Red", "Green", "Blue"};
        for (int c = 0; c < 3; ++c) {
            const double s = p.num(QStringLiteral("shadows") + QLatin1String(chans[c])) * 0.5;
            const double m = p.num(QStringLiteral("midtones") + QLatin1String(chans[c])) * 0.5;
            const double h = p.num(QStringLiteral("highlights") + QLatin1String(chans[c])) * 0.5;
            any |= s != 0 || m != 0 || h != 0;
            l[size_t(c)] = makeLut([&](double v) {
                const double ws = (1 - v) * (1 - v), wh = v * v, wm = 2 * v * (1 - v);
                return v + s * ws + m * wm + h * wh;
            });
        }
        return any;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("curves");
    d.name = QStringLiteral("Curves");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral(
        "Tone curves: raise or lower the shadows, darks, midtones, lights and highlights of the whole picture "
        "(a smooth curve through them), then shape red, green and blue separately. An S-curve (darks down, "
        "lights up) adds contrast; lifting the shadows gives a faded film look.");
    for (const auto& [id, name] : {std::pair{"shadows", "Shadows"}, {"darks", "Darks"}, {"midtones", "Midtones"},
                                   {"lights", "Lights"}, {"highlights", "Highlights"}})
        d.params.push_back(num(id, name, -0.5, 0.5, 0, 3));
    for (const char* ch : {"red", "green", "blue"})
        for (const auto& [id, name] : {std::pair{"Shadows", "shadows"}, {"Midtones", "midtones"}, {"Highlights", "highlights"}}) {
            ParamDef pd = num("", "", -0.5, 0.5, 0, 3);
            pd.id = QString::fromLatin1(ch) + QString::fromLatin1(id);
            pd.name = QStringLiteral("%1 %2").arg(QString::fromLatin1(ch).left(1).toUpper() + QString::fromLatin1(ch).mid(1),
                                                  QString::fromLatin1(name));
            d.params.push_back(pd);
        }
    d.luts = [](const Params& p, ChannelLuts& l) {
        auto v = [&](const char* id) { return p.num(QString::fromLatin1(id)); };
        const std::vector<std::pair<double, double>> master{{0.0, 0.0 + v("shadows")}, {0.25, 0.25 + v("darks")},
                                                             {0.5, 0.5 + v("midtones")}, {0.75, 0.75 + v("lights")},
                                                             {1.0, 1.0 + v("highlights")}};
        bool any = false;
        for (const auto& pt : master)
            any |= pt.second != pt.first;
        const char* chans[3] = {"red", "green", "blue"};
        for (int c = 0; c < 3; ++c) {
            const QString ch = QString::fromLatin1(chans[c]);
            const double s = p.num(ch + QStringLiteral("Shadows")), m = p.num(ch + QStringLiteral("Midtones")),
                         h = p.num(ch + QStringLiteral("Highlights"));
            any |= s != 0 || m != 0 || h != 0;
            const std::vector<std::pair<double, double>> curve{{0.0, s}, {0.5, 0.5 + m}, {1.0, 1.0 + h}};
            l[size_t(c)] = makeLut([&](double x) { return smoothCurve(curve, std::clamp(smoothCurve(master, x), 0.0, 1.0)); });
        }
        return any;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("color_wheels");
    d.name = QStringLiteral("Color Wheels (Lift / Gamma / Gain)");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral(
        "Colour grading in three ranges: Lift moves the shadows, Gamma the midtones, Gain the highlights. Each has a "
        "brightness and a colour push (its direction on the colour wheel, and how far). Example: Lift towards teal "
        "and Gain towards orange for the blockbuster look.");
    for (const auto& [id, name] : {std::pair{"lift", "Lift"}, {"gamma", "Gamma"}, {"gain", "Gain"}}) {
        ParamDef hue = num("", "", 0, 360, id == std::string("gain") ? 30 : 200, 0, "°");
        hue.id = QString::fromLatin1(id) + QStringLiteral("Hue");
        hue.name = QStringLiteral("%1 colour").arg(QString::fromLatin1(name));
        hue.tooltip = QStringLiteral("Direction on the colour wheel: 0 red, 60 yellow, 120 green, 180 cyan, 240 blue, 300 magenta");
        ParamDef amount = num("", "", 0, 1, 0, 2);
        amount.id = QString::fromLatin1(id) + QStringLiteral("Amount");
        amount.name = QStringLiteral("%1 push").arg(QString::fromLatin1(name));
        ParamDef lum = num("", "", -1, 1, 0, 2);
        lum.id = QString::fromLatin1(id);
        lum.name = QStringLiteral("%1 level").arg(QString::fromLatin1(name));
        d.params.push_back(lum);
        d.params.push_back(hue);
        d.params.push_back(amount);
    }
    d.luts = [](const Params& p, ChannelLuts& l) {
        // Per channel: out = (gain * (v + lift * (1 - v)))^(1 / gamma).
        auto wheel = [&](const char* id, int c) {
            const QString base = QString::fromLatin1(id);
            const double level = p.num(base), amount = p.num(base + QStringLiteral("Amount"));
            const double hue = p.num(base + QStringLiteral("Hue")) * 3.14159265358979 / 180.0;
            const double push = amount * std::cos(hue - c * 2.0943951023931953); // red, green, blue at 0, 120, 240 degrees
            return level * 0.5 + push * 0.25;
        };
        bool any = false;
        for (const char* id : {"lift", "gamma", "gain"})
            any |= p.num(QString::fromLatin1(id)) != 0 || p.num(QString::fromLatin1(id) + QStringLiteral("Amount")) != 0;
        if (!any)
            return false;
        for (int c = 0; c < 3; ++c) {
            const double lift = wheel("lift", c), gain = 1.0 + 2.0 * wheel("gain", c);
            const double gamma = std::max(0.1, 1.0 + 2.0 * wheel("gamma", c));
            l[size_t(c)] = makeLut([&](double v) { return std::pow(std::clamp(gain * (v + lift * (1.0 - v)), 0.0, 1.0), 1.0 / gamma); });
        }
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("lut_cube");
    d.name = QStringLiteral("LUT (.cube)");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral(
        "Applies a 3D colour lookup table from a .cube file (from DaVinci Resolve, Premiere, LUT packs, camera "
        "makers' log-to-Rec.709 LUTs...). Strength mixes it with the original picture.");
    {
        ParamDef file;
        file.id = QStringLiteral("file");
        file.name = QStringLiteral("LUT file");
        file.type = ParamType::Text;
        file.fileFilter = QStringLiteral("Cube LUT (*.cube);;All files (*)");
        file.tooltip = d.description;
        d.params = {file, num("strength", "Strength", 0, 1, 1, 2)};
    }
    d.apply = [](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) {
        if (auto lut = loadCubeLut(p.text(QStringLiteral("file"))))
            applyCubeLut(img, *lut, float(p.num(QStringLiteral("strength"))));
    };
    add(d);

    d = {};
    d.id = QStringLiteral("black_white");
    d.name = QStringLiteral("Black and White");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Desaturate to greyscale.");
    d.params = {num("amount", "Amount", 0, 1, 1)};
    d.colorMatrix = [](const Params& p, ColorMatrix& out) {
        const float a = float(p.num(QStringLiteral("amount")));
        if (a <= 0)
            return false;
        const Matrix m = saturationMatrix(1.0f - a);
        std::copy(std::begin(m.m), std::end(m.m), out.begin());
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("sepia");
    d.name = QStringLiteral("Sepia");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Old-photo brown toning.");
    d.params = {num("amount", "Amount", 0, 1, 1)};
    d.colorMatrix = [](const Params& p, ColorMatrix& out) {
        const float a = float(p.num(QStringLiteral("amount")));
        if (a <= 0)
            return false;
        const Matrix sepia{{0.393f, 0.769f, 0.189f, 0, 0.349f, 0.686f, 0.168f, 0, 0.272f, 0.534f, 0.131f, 0}};
        const Matrix m = lerp(Matrix{}, sepia, a);
        std::copy(std::begin(m.m), std::end(m.m), out.begin());
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("tint");
    d.name = QStringLiteral("Color Tint");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Colourise the picture with a single tint colour.");
    d.params = {color("color", "Tint color", 0xff9a3c), num("amount", "Amount", 0, 1, 0.5)};
    d.colorMatrix = [](const Params& p, ColorMatrix& out) {
        const float a = float(p.num(QStringLiteral("amount")));
        const QColor c = p.color(QStringLiteral("color"));
        if (a <= 0)
            return false;
        const float cr = float(c.redF()), cg = float(c.greenF()), cb = float(c.blueF());
        const Matrix tint{{0.2126f * cr, 0.7152f * cr, 0.0722f * cr, 0, 0.2126f * cg, 0.7152f * cg, 0.0722f * cg, 0,
                           0.2126f * cb, 0.7152f * cb, 0.0722f * cb, 0}};
        const Matrix m = lerp(Matrix{}, tint, a);
        std::copy(std::begin(m.m), std::end(m.m), out.begin());
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("invert");
    d.name = QStringLiteral("Invert");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Photographic negative.");
    d.params = {num("amount", "Amount", 0, 1, 1)};
    d.luts = [](const Params& p, ChannelLuts& l) {
        const double a = p.num(QStringLiteral("amount"));
        if (a <= 0)
            return false;
        l[0] = l[1] = l[2] = makeLut([&](double v) { return v + (1.0 - 2.0 * v) * a; });
        return true;
    };
    add(d);

    d = {};
    d.id = QStringLiteral("posterize");
    d.name = QStringLiteral("Posterize");
    d.category = QStringLiteral("Color");
    d.description = QStringLiteral("Reduce each channel to a few levels.");
    d.params = {num("levels", "Levels", 2, 64, 6, 0)};
    d.luts = [](const Params& p, ChannelLuts& l) {
        const double n = std::round(p.num(QStringLiteral("levels"))) - 1;
        l[0] = l[1] = l[2] = makeLut([&](double v) { return std::round(v * n) / n; });
        return true;
    };
    add(d);

    // ---- Blur & sharpen -------------------------------------------------------
    d = {};
    d.id = QStringLiteral("gaussian_blur");
    d.name = QStringLiteral("Gaussian Blur");
    d.category = QStringLiteral("Blur & Sharpen");
    d.description = QStringLiteral("Smooth blur; horizontal and vertical can differ.");
    d.params = {num("horizontal", "Horizontal radius", 0, 200, 8, 1, " px"), num("vertical", "Vertical radius", 0, 200, 8, 1, " px")};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) {
        gaussianBlur(img, p.num(QStringLiteral("horizontal")) * ctx.scale, p.num(QStringLiteral("vertical")) * ctx.scale);
    };
    add(d);

    d = {};
    d.id = QStringLiteral("sharpen");
    d.name = QStringLiteral("Sharpen");
    d.category = QStringLiteral("Blur & Sharpen");
    d.description = QStringLiteral("Unsharp mask: increases local contrast around edges.");
    d.params = {num("amount", "Amount", 0, 5, 1), num("radius", "Radius", 0.5, 20, 2, 1, " px")};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) { applySharpen(img, p, ctx); };
    add(d);

    d = {};
    d.id = QStringLiteral("glow");
    d.name = QStringLiteral("Glow");
    d.category = QStringLiteral("Blur & Sharpen");
    d.description = QStringLiteral("Bright areas bloom outwards.");
    d.params = {num("threshold", "Threshold", 0, 1, 0.7), num("radius", "Radius", 1, 200, 20, 1, " px"),
                num("intensity", "Intensity", 0, 3, 1)};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) { applyGlow(img, p, ctx); };
    add(d);

    // ---- Stylize ---------------------------------------------------------------
    d = {};
    d.id = QStringLiteral("vignette");
    d.name = QStringLiteral("Vignette");
    d.category = QStringLiteral("Stylize");
    d.description = QStringLiteral("Darken (or lighten) the edges of the frame.");
    d.params = {num("amount", "Amount", -1, 1, 0.5), num("size", "Size", 0.05, 1.5, 0.75), num("softness", "Softness", 0, 1, 0.6),
                num("roundness", "Roundness", 0, 1, 0)};
    d.apply = [](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) { applyVignette(img, p); };
    add(d);

    d = {};
    d.id = QStringLiteral("pixelate");
    d.name = QStringLiteral("Pixelate");
    d.category = QStringLiteral("Stylize");
    d.description = QStringLiteral("Mosaic of large square blocks.");
    d.params = {num("size", "Block size", 1, 200, 16, 0, " px")};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) { applyPixelate(img, p, ctx); };
    add(d);

    d = {};
    d.id = QStringLiteral("noise");
    d.name = QStringLiteral("Film Grain");
    d.category = QStringLiteral("Stylize");
    d.description = QStringLiteral("Adds random grain, optionally changing every frame.");
    d.params = {num("amount", "Amount", 0, 1, 0.15), flag("monochrome", "Monochrome", true), flag("animated", "Animated", true)};
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) { applyNoise(img, p, ctx); };
    add(d);

    // ---- Keying & compositing --------------------------------------------------
    d = {};
    d.id = QStringLiteral("chroma_key");
    d.name = QStringLiteral("Chroma Keyer");
    d.category = QStringLiteral("Keying & Compositing");
    d.description = QStringLiteral("Makes a colour (green/blue screen) transparent so lower tracks show through.");
    d.params = {color("key", "Key color", 0x00ff00), num("tolerance", "Tolerance", 0, 1, 0.3), num("softness", "Softness", 0, 1, 0.15),
                num("spill", "Spill suppression", 0, 1, 0.6), flag("showMask", "Show mask only", false)};
    d.apply = [](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) { applyChromaKey(img, p); };
    add(d);

    d = {};
    d.id = QStringLiteral("transform");
    d.name = QStringLiteral("Transform (Picture in Picture)");
    d.category = QStringLiteral("Keying & Compositing");
    d.description = QStringLiteral("Scale, move, rotate and crop the picture, with an optional border. "
                                   "Keyframe it for animated picture-in-picture.");
    d.params = {num("scale", "Scale", 0.01, 5, 1, 3),
                num("x", "Position X", -2, 2, 0, 3),
                num("y", "Position Y", -2, 2, 0, 3),
                num("rotation", "Rotation", -360, 360, 0, 1, "°"),
                num("cropLeft", "Crop left", 0, 0.49, 0, 3),
                num("cropRight", "Crop right", 0, 0.49, 0, 3),
                num("cropTop", "Crop top", 0, 0.49, 0, 3),
                num("cropBottom", "Crop bottom", 0, 0.49, 0, 3),
                num("opacity", "Opacity", 0, 1, 1),
                num("border", "Border width", 0, 100, 0, 1, " px"),
                color("borderColor", "Border color", 0xffffff)};
    d.params.front().logScale = true;
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>&) { applyTransform(img, p, ctx); };
    add(d);

    d = {};
    d.id = QStringLiteral("flip");
    d.name = QStringLiteral("Mirror / Flip");
    d.category = QStringLiteral("Keying & Compositing");
    d.description = QStringLiteral("Flip the picture horizontally and/or vertically.");
    d.params = {flag("horizontal", "Horizontal", true), flag("vertical", "Vertical", false)};
    d.apply = [](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) {
        const bool h = p.flag(QStringLiteral("horizontal")), v = p.flag(QStringLiteral("vertical"));
        if (h || v)
            img = img.mirrored(h, v);
    };
    add(d);

    // ---- Advanced ------------------------------------------------------------
    d = {};
    d.id = QStringLiteral("ffmpeg_filter");
    d.name = QStringLiteral("FFmpeg Filter (advanced)");
    d.category = QStringLiteral("Advanced");
    d.description = QStringLiteral(
        "Runs any FFmpeg filter chain on the picture. Examples: \"hqdn3d=4:3:6:4\" (denoise), "
        "\"eq=gamma=1.2:saturation=1.3\", \"lut3d=file=look.cube\", \"edgedetect\", \"unsharp=5:5:1.0\", "
        "\"colorbalance=rs=0.1\", \"lenscorrection=k1=-0.2\". See ffmpeg.org/ffmpeg-filters.html.");
    {
        ParamDef t;
        t.id = QStringLiteral("filter");
        t.name = QStringLiteral("Filter chain");
        t.type = ParamType::Text;
        t.defText = QStringLiteral("eq=contrast=1.1:saturation=1.3");
        t.tooltip = d.description;
        d.params = {t};
    }
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state) {
        applyFFmpegFilter(img, p, ctx, state);
    };
    add(d);

    addGlitchEffects(list);
    addSegmentationEffects(list); // Remove Background (AI)
    vg::addEffects(list); // Broken Video Game Systems
    for (EffectDef& e : list) {
        if (e.luts && !e.apply)
            e.apply = [f = e.luts](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) {
                ChannelLuts l;
                if (f(p, l))
                    applyLuts(img, l[0], l[1], l[2]);
            };
        if (e.colorMatrix && !e.apply)
            e.apply = [f = e.colorMatrix](QImage& img, const Params& p, const RenderContext&, std::unique_ptr<EffectState>&) {
                ColorMatrix m;
                if (f(p, m))
                    applyColorMatrix(img, m.data());
            };
    }
    return list;
}

} // namespace

const std::vector<EffectDef>& videoEffects()
{
    static const std::vector<EffectDef> list = buildEffects();
    return list;
}

const EffectDef* findEffect(const QString& id)
{
    for (const auto& e : videoEffects())
        if (e.id == id)
            return &e;
    return nullptr;
}
