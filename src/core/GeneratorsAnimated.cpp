// More generated media: animated backgrounds (plasma, aurora, starfield,
// bokeh, retro grid, digital rain), overlays (confetti, progress bar,
// letterbox, shapes), kinetic text, a countdown leader and colour bars.
// Everything is a pure function of the event-local time, so any frame can
// be rendered on its own.

#include "core/EffectHelpers.h"
#include "core/Generators.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>

namespace {

using fx::choice;
using fx::color;
using fx::flag;
using fx::hash01;
using fx::num;

constexpr double kPi = 3.14159265358979;

ParamDef text(const char* id, const char* name, const QString& def, bool multiline)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Text;
    p.defText = def;
    p.multiline = multiline;
    return p;
}

ParamDef font(const char* id, const char* name, const char* def)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Font;
    p.defText = QString::fromLatin1(def);
    return p;
}

double seconds(const GeneratorContext& ctx) { return double(ctx.time) / double(kUsPerSecond); }
double fract(double v) { return v - std::floor(v); }
double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }
double easeOut(double t)
{
    t = clamp01(t);
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
}
double backOut(double t)
{
    t = clamp01(t);
    const double c1 = 1.70158, c3 = c1 + 1.0;
    return 1.0 + c3 * std::pow(t - 1.0, 3) + c1 * std::pow(t - 1.0, 2);
}
// A stable random number in 0..1 for item i, field k.
double rnd(int i, int k, uint32_t seed = 0) { return hash01(uint32_t(i), uint32_t(k), 101u + seed); }

double valueNoise1(double x, uint32_t seed)
{
    const double i = std::floor(x), f = x - i;
    const double u = f * f * (3.0 - 2.0 * f);
    const uint32_t xi = uint32_t(int64_t(i));
    const double a = hash01(xi, 0, seed), b = hash01(xi + 1u, 0, seed);
    return a + (b - a) * u;
}

QColor lerp(const QColor& a, const QColor& b, double t)
{
    t = clamp01(t);
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
                            float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

QColor withAlpha(QColor c, double a)
{
    c.setAlphaF(float(clamp01(a)));
    return c;
}

// Colour along a list of stops, back and forth so it cycles without a seam.
QColor paletteAt(const std::vector<QColor>& stops, double f)
{
    f = fract(f);
    const double g = (f < 0.5 ? f * 2.0 : 2.0 - f * 2.0) * double(stops.size() - 1);
    const int i = std::clamp(int(g), 0, int(stops.size()) - 2);
    return lerp(stops[size_t(i)], stops[size_t(i) + 1], g - i);
}

std::vector<QColor> hexes(std::initializer_list<uint32_t> list)
{
    std::vector<QColor> out;
    for (uint32_t c : list)
        out.push_back(QColor::fromRgb(c));
    return out;
}

const QStringList kPaletteNames = {QStringLiteral("Neon"), QStringLiteral("Fire"), QStringLiteral("Ocean"), QStringLiteral("Sunset"),
                                   QStringLiteral("Rainbow")};
std::vector<QColor> palette(int which)
{
    switch (which) {
    case 1: return hexes({0x000000, 0x8a0000, 0xff4500, 0xffb300, 0xfff3b0});
    case 2: return hexes({0x001a33, 0x004e7a, 0x00a6c7, 0x7ff3ff});
    case 3: return hexes({0x2b1055, 0xd53369, 0xff8c42, 0xffd166});
    case 4: return hexes({0xff0040, 0xffd000, 0x00e060, 0x00c8ff, 0x6040ff, 0xff00d0});
    default: return hexes({0x0b0033, 0xff00c8, 0x00e5ff, 0x3d00ff});
    }
}

const QStringList kLightPalettes = {QStringLiteral("Warm"), QStringLiteral("Cool"), QStringLiteral("Party"), QStringLiteral("Gold")};
std::vector<QColor> lightPalette(int which)
{
    switch (which) {
    case 1: return hexes({0x5ad1ff, 0x7a7bff, 0x9fffe8, 0xffffff});
    case 2: return hexes({0xff3d7f, 0xffd23d, 0x3dffb4, 0x3db4ff, 0xb43dff});
    case 3: return hexes({0xffd700, 0xffb347, 0xfff1c1, 0xe6a12e});
    default: return hexes({0xffb35c, 0xff7a45, 0xffe08a, 0xff9e9e});
    }
}

// Per-pixel pictures are worked out at a quarter of the size and smoothed up.
QImage smallCanvas(QSize full)
{
    QImage img(std::max(8, (full.width() + 3) / 4), std::max(8, (full.height() + 3) / 4), QImage::Format_ARGB32_Premultiplied);
    return img;
}

void drawScaledUp(QImage& layer, const QImage& small)
{
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(layer.rect()), small);
}

bool always(const Params&, const GeneratorContext&) { return true; }

// ---- Animated backgrounds --------------------------------------------------------------

void renderPlasma(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx) * p.num(QStringLiteral("speed"));
    const double sc = std::max(0.05, p.num(QStringLiteral("scale")));
    const std::vector<QColor> stops = palette(p.choice(QStringLiteral("palette")));
    QImage small = smallCanvas(layer.size());
    const int sw = small.width(), sh = small.height();
    const double aspect = double(layer.width()) / std::max(1, layer.height());
    parallelFor(sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<QRgb*>(small.scanLine(y));
            for (int x = 0; x < sw; ++x) {
                const double X = (x + 0.5) / sw * aspect * 3.0 / sc, Y = (y + 0.5) / sh * 3.0 / sc;
                const double v = std::sin(X + T) + std::sin(Y * 1.3 - T * 0.7) + std::sin((X + Y) * 0.7 + T * 0.5)
                                 + std::sin(std::hypot(X - 1.5 * aspect / sc + std::sin(T * 0.3), Y - 1.5 / sc + std::cos(T * 0.4)) * 2.0 - T * 1.3);
                row[x] = paletteAt(stops, v / 8.0 + 0.5 + T * 0.03).rgba();
            }
        }
    });
    drawScaledUp(layer, small);
}

void renderAurora(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx) * p.num(QStringLiteral("speed"));
    const QColor c1 = p.color(QStringLiteral("color1")), c2 = p.color(QStringLiteral("color2"));
    const QColor skyTop = p.color(QStringLiteral("sky")), skyLow = p.color(QStringLiteral("horizon"));
    QImage small = smallCanvas(layer.size());
    const int sw = small.width(), sh = small.height();
    const double aspect = double(layer.width()) / std::max(1, layer.height());
    parallelFor(sh, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<QRgb*>(small.scanLine(y));
            const double Y = (y + 0.5) / sh;
            const QColor sky = lerp(skyTop, skyLow, Y);
            for (int x = 0; x < sw; ++x) {
                const double X = (x + 0.5) / sw * aspect;
                double r = sky.redF(), g = sky.greenF(), b = sky.blueF();
                for (int k = 0; k < 3; ++k) {
                    double yc = 0.32 + 0.1 * k + 0.3 * (valueNoise1(X * 1.4 + k * 7.1 + T * 0.12, 30u + uint32_t(k)) - 0.5);
                    yc += 0.04 * std::sin(X * 6.0 + T * 0.8 + k);
                    const double d = Y - yc;
                    const double shape = d < 0 ? std::exp(d / 0.16) : std::exp(-d * d / 0.0009);
                    const double rays = 0.45 + 0.55 * valueNoise1(X * 45.0 + k * 13.0 + T * 0.5, 40u + uint32_t(k));
                    const double i = shape * rays * (0.85 - 0.2 * k);
                    const QColor c = lerp(c1, c2, std::clamp(-d * 3.5, 0.0, 1.0));
                    r += c.redF() * i;
                    g += c.greenF() * i;
                    b += c.blueF() * i;
                }
                row[x] = qRgb(int(clamp01(r) * 255), int(clamp01(g) * 255), int(clamp01(b) * 255));
            }
        }
    });
    drawScaledUp(layer, small);
    if (!p.flag(QStringLiteral("stars")))
        return;
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    const double w = layer.width(), h = layer.height();
    for (int i = 0; i < 180; ++i) {
        const double tw = 0.45 + 0.55 * (0.5 + 0.5 * std::sin(T * (1.0 + rnd(i, 3) * 3.0) + rnd(i, 4) * 6.283));
        painter.setBrush(withAlpha(Qt::white, tw * (0.3 + 0.7 * rnd(i, 5))));
        const double r = (0.5 + rnd(i, 2) * 1.2) * ctx.scale;
        painter.drawEllipse(QPointF(rnd(i, 0) * w, rnd(i, 1) * h * 0.75), r, r);
    }
}

bool paintStarfield(QPainter& painter, QSize size, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx);
    const double speed = p.num(QStringLiteral("speed")), trails = p.num(QStringLiteral("trails"));
    const int count = int(std::lround(p.num(QStringLiteral("count"))));
    const QColor col = p.color(QStringLiteral("color"));
    painter.setCompositionMode(QPainter::CompositionMode_Source);
    painter.fillRect(QRect(QPoint(0, 0), size), p.color(QStringLiteral("background")));
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double w = size.width(), h = size.height(), spread = std::max(w, h) * 0.25;
    for (int i = 0; i < count; ++i) {
        const double x0 = rnd(i, 0) * 2 - 1, y0 = rnd(i, 1) * 2 - 1;
        const double z = fract(rnd(i, 2) - T * speed * 0.15);
        const double zz = 0.02 + z * 0.98;
        const QPointF at(w / 2 + x0 / zz * spread, h / 2 + y0 / zz * spread);
        if (at.x() < -50 || at.y() < -50 || at.x() > w + 50 || at.y() > h + 50)
            continue;
        const double b = std::clamp((1.0 - z) * 1.4, 0.0, 1.0);
        const double r = (0.6 + 2.4 * (1.0 - z)) * ctx.scale;
        const QColor c = withAlpha(lerp(col, QColor(170, 200, 255), rnd(i, 3) * 0.5), b);
        if (trails > 0.01) {
            const double zp = zz + trails * 0.06 * std::max(0.2, speed);
            const QPointF from(w / 2 + x0 / zp * spread, h / 2 + y0 / zp * spread);
            painter.setPen(QPen(c, r, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(from, at);
        } else {
            painter.setPen(Qt::NoPen);
            painter.setBrush(c);
            painter.drawEllipse(at, r / 2, r / 2);
        }
    }
    return true;
}

void renderStarfield(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    QPainter painter(&layer);
    paintStarfield(painter, layer.size(), p, ctx);
}

void renderBokeh(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx) * p.num(QStringLiteral("speed"));
    const int count = int(std::lround(p.num(QStringLiteral("count"))));
    const std::vector<QColor> cols = lightPalette(p.choice(QStringLiteral("palette")));
    const double w = layer.width(), h = layer.height();
    layer.fill(withAlpha(p.color(QStringLiteral("background")), p.num(QStringLiteral("backgroundOpacity"))));
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setCompositionMode(QPainter::CompositionMode_Plus);
    painter.setPen(Qt::NoPen);
    const double size = p.num(QStringLiteral("size")) / 100.0 * h;
    for (int i = 0; i < count; ++i) {
        const double hs = rnd(i, 12), hp = rnd(i, 14);
        const double x = (fract(rnd(i, 10) + T * 0.02 * (hs - 0.5) * 2 + 0.03 * std::sin(T * 0.5 + hp * 6.283)) * 1.2 - 0.1) * w;
        const double y = (fract(rnd(i, 11) - T * 0.015 * (0.5 + hs)) * 1.2 - 0.1) * h;
        const double r = size * (0.4 + 0.8 * hs);
        const double a = 0.22 + 0.3 * (0.5 + 0.5 * std::sin(T * 1.3 + hp * 20.0));
        const QColor c = cols[size_t(rnd(i, 13) * cols.size()) % cols.size()];
        QRadialGradient g(QPointF(x, y), r);
        g.setColorAt(0.0, withAlpha(c, a * 0.55));
        g.setColorAt(0.82, withAlpha(c, a * 0.5));
        g.setColorAt(0.94, withAlpha(c, a * 0.85));
        g.setColorAt(1.0, withAlpha(c, 0));
        painter.setBrush(g);
        painter.drawEllipse(QPointF(x, y), r, r);
    }
}

bool paintRetroGrid(QPainter& painter, QSize size, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx) * p.num(QStringLiteral("speed"));
    const double w = size.width(), h = size.height();
    const double hy = h * p.num(QStringLiteral("horizon"));
    const QColor grid = p.color(QStringLiteral("color"));
    painter.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient sky(0, 0, 0, hy);
    sky.setColorAt(0, QColor(0x0d0221));
    sky.setColorAt(0.6, QColor(0x3b0a57));
    sky.setColorAt(1, QColor(0xb8336a));
    painter.fillRect(QRectF(0, 0, w, hy), sky);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < 70; ++i) {
        painter.setBrush(withAlpha(Qt::white, 0.25 + 0.5 * rnd(i, 22)));
        const double r = (0.6 + rnd(i, 23)) * ctx.scale;
        painter.drawEllipse(QPointF(rnd(i, 20) * w, rnd(i, 21) * hy * 0.7), r, r);
    }
    if (p.flag(QStringLiteral("sun"))) {
        const double r = h * 0.21;
        const QPointF c(w / 2, hy - r * 0.45);
        QRadialGradient glow(c, r * 1.8);
        glow.setColorAt(0, QColor(255, 60, 140, 110));
        glow.setColorAt(1, QColor(255, 60, 140, 0));
        painter.setBrush(glow);
        painter.drawEllipse(c, r * 1.8, r * 1.8);
        QPainterPath sun;
        sun.addEllipse(c, r, r);
        for (int k = 0; k < 6; ++k) {
            QPainterPath cut;
            const double y = c.y() + r * (0.05 + k * 0.16);
            cut.addRect(QRectF(c.x() - r, y, 2 * r, r * (0.025 + k * 0.022)));
            sun = sun.subtracted(cut);
        }
        QLinearGradient sg(0, c.y() - r, 0, c.y() + r);
        sg.setColorAt(0, QColor(0xffd319));
        sg.setColorAt(1, QColor(0xff2975));
        painter.fillPath(sun, sg);
    }
    painter.fillRect(QRectF(0, hy, w, h - hy), QColor(0x0b0016));
    QLinearGradient haze(0, hy - h * 0.03, 0, hy + h * 0.05);
    haze.setColorAt(0, withAlpha(grid, 0));
    haze.setColorAt(0.4, withAlpha(grid, 0.35));
    haze.setColorAt(1, withAlpha(grid, 0));
    painter.fillRect(QRectF(0, hy - h * 0.03, w, h * 0.08), haze);
    // Lines: a wide faint pass for the glow, then a thin bright one.
    const double phase = fract(T);
    for (int pass = 0; pass < 2; ++pass) {
        const double width = (pass == 0 ? 7.0 : 1.8) * ctx.scale;
        const double alpha = pass == 0 ? 0.22 : 1.0;
        for (int i = -24; i <= 24; ++i) {
            painter.setPen(QPen(withAlpha(grid, alpha), width));
            painter.drawLine(QPointF(w / 2 + i * w * 0.004, hy), QPointF(w / 2 + i * w * 0.11, h));
        }
        for (int k = 0; k < 40; ++k) {
            const double z = k + 1 - phase;
            const double y = hy + (h - hy) * 0.5 / z;
            if (y > h + width)
                continue;
            const double fade = std::clamp((y - hy) / (h - hy) * 3.0, 0.12, 1.0);
            painter.setPen(QPen(withAlpha(grid, alpha * fade), width));
            painter.drawLine(QPointF(0, y), QPointF(w, y));
        }
    }
    return true;
}

void renderRetroGrid(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    QPainter painter(&layer);
    paintRetroGrid(painter, layer.size(), p, ctx);
}

void renderDigitalRain(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx);
    const double speed = p.num(QStringLiteral("speed"));
    const QColor col = p.color(QStringLiteral("color"));
    const double w = layer.width(), h = layer.height();
    if (p.flag(QStringLiteral("background")))
        layer.fill(Qt::black);
    static const QString glyphs = [] {
        QString s = QStringLiteral("0123456789Z:=*+-<>");
        for (char16_t u = 0xFF66; u <= 0xFF9D; ++u)
            s += QChar(u);
        return s;
    }();
    const double gs = std::max(6.0, p.num(QStringLiteral("size")) / 100.0 * h);
    QFont f(QStringLiteral("MS Gothic"));
    f.setPixelSize(int(std::lround(gs)));
    f.setHintingPreference(QFont::PreferNoHinting);
    QPainter painter(&layer);
    painter.setFont(f);
    const double colW = gs * 0.8;
    const int cols = int(std::ceil(w / colW)), rows = int(h / gs) + 1;
    const QColor head = lerp(col, Qt::white, 0.75);
    for (int c = 0; c < cols; ++c) {
        const double sp = (0.5 + rnd(c, 1, 7)) * speed * 8.0;
        const int len = 6 + int(rnd(c, 2, 7) * 24);
        const double period = rows + len + rnd(c, 3, 7) * rows;
        const double headRow = std::fmod(T * sp + rnd(c, 4, 7) * period, period);
        for (int k = 0; k <= len; ++k) {
            const int r = int(std::floor(headRow)) - k;
            if (r < 0 || r >= rows)
                continue;
            const uint32_t swap = uint32_t(std::floor(T * (1.5 + rnd(c, r, 8) * 6.0)));
            const QChar ch = glyphs[int(fx::hash32(uint32_t(c), uint32_t(r), swap) % uint32_t(glyphs.size()))];
            const double fade = 1.0 - double(k) / (len + 1);
            painter.setPen(k == 0 ? head : withAlpha(col, std::pow(fade, 1.4)));
            painter.drawText(QPointF(c * colW, (r + 1) * gs), QString(ch));
        }
    }
}

// ---- Overlays --------------------------------------------------------------------------

void renderConfetti(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double T = seconds(ctx);
    const double speed = p.num(QStringLiteral("speed")), size = p.num(QStringLiteral("size"));
    const int count = int(std::lround(p.num(QStringLiteral("count"))));
    const std::vector<QColor> cols = lightPalette(p.choice(QStringLiteral("palette")) == 1 ? 3 : 2);
    const double w = layer.width(), h = layer.height();
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    for (int i = 0; i < count; ++i) {
        const double hs = rnd(i, 30), hr = rnd(i, 31), hf = rnd(i, 32);
        const double y = (fract(rnd(i, 33) + T * (0.5 + hs) * speed * 0.12) * 1.2 - 0.1) * h;
        const double x = rnd(i, 34) * w + std::sin(T * (1.0 + hf * 2.0) + hr * 6.283) * w * 0.02;
        const double angle = T * (60.0 + 240.0 * hr) * (hf < 0.5 ? -1.0 : 1.0) + hr * 360.0;
        const double flip = std::cos(T * (2.0 + hf * 4.0) + hs * 6.283);
        const double s = h * 0.012 * size * (0.6 + 0.8 * hs);
        QColor c = cols[size_t(rnd(i, 35) * cols.size()) % cols.size()];
        if (flip < 0)
            c = c.darker(135);
        painter.save();
        painter.translate(x, y);
        painter.rotate(angle);
        painter.setBrush(c);
        if (rnd(i, 36) < 0.78)
            painter.drawRect(QRectF(-s * 0.8 * std::abs(flip) - 0.25, -s / 2, s * 1.6 * std::abs(flip) + 0.5, s));
        else
            painter.drawEllipse(QPointF(0, 0), s * 0.5 * std::abs(flip) + 0.25, s * 0.5);
        painter.restore();
    }
}

void renderProgressBar(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double w = layer.width(), h = layer.height();
    const double fill = ctx.length > 0 ? clamp01(double(ctx.time) / double(ctx.length)) : 0.0;
    const double bh = std::max(1.0, p.num(QStringLiteral("height")) / 100.0 * h);
    const double margin = p.num(QStringLiteral("margin")) / 100.0 * h;
    const bool top = p.choice(QStringLiteral("position")) == 1;
    const QRectF track(margin, top ? margin : h - margin - bh, w - 2 * margin, bh);
    const double radius = p.flag(QStringLiteral("rounded")) ? bh / 2 : 0.0;
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(p.color(QStringLiteral("trackColor")), p.num(QStringLiteral("trackOpacity"))));
    painter.drawRoundedRect(track, radius, radius);
    painter.setBrush(p.color(QStringLiteral("color")));
    painter.drawRoundedRect(QRectF(track.topLeft(), QSizeF(track.width() * fill, bh)), radius, radius);
}

const double kAspects[] = {2.39, 2.0, 1.85, 4.0 / 3.0, 1.0, 9.0 / 16.0};

void renderLetterbox(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double w = layer.width(), h = layer.height();
    const double target = kAspects[std::clamp(p.choice(QStringLiteral("aspect")), 0, 5)];
    double k = 1.0;
    if (p.flag(QStringLiteral("animate"))) {
        const double d = std::max(0.01, p.num(QStringLiteral("duration")));
        const double T = seconds(ctx), len = double(ctx.length) / kUsPerSecond;
        k = std::min(easeOut(T / d), easeOut((len - T) / d));
    }
    QPainter painter(&layer);
    const QColor c = withAlpha(p.color(QStringLiteral("color")), p.num(QStringLiteral("opacity")));
    if (target > w / h) {
        const double bar = (h - w / target) / 2 * k;
        painter.fillRect(QRectF(0, 0, w, bar), c);
        painter.fillRect(QRectF(0, h - bar, w, bar), c);
    } else {
        const double bar = (w - h * target) / 2 * k;
        painter.fillRect(QRectF(0, 0, bar, h), c);
        painter.fillRect(QRectF(w - bar, 0, bar, h), c);
    }
}

bool letterboxAnimating(const Params& p, const GeneratorContext& ctx)
{
    if (!p.flag(QStringLiteral("animate")))
        return false;
    const TimeUs d = TimeUs(p.num(QStringLiteral("duration")) * kUsPerSecond);
    return ctx.time < d || ctx.time > ctx.length - d;
}

QPainterPath shapePath(int shape, double W, double H, double corner)
{
    QPainterPath path;
    const QRectF r(-W / 2, -H / 2, W, H);
    auto poly = [&](std::initializer_list<QPointF> pts) {
        QPolygonF pg;
        for (QPointF q : pts)
            pg << QPointF(q.x() * W, q.y() * H);
        path.addPolygon(pg);
        path.closeSubpath();
    };
    switch (shape) {
    case 0: path.addRect(r); break;
    case 1: {
        const double c = corner / 100.0 * std::min(W, H);
        path.addRoundedRect(r, c, c);
        break;
    }
    case 2: path.addEllipse(r); break;
    case 3: poly({{0, -0.5}, {0.5, 0.5}, {-0.5, 0.5}}); break;
    case 4: {
        QPolygonF pg;
        for (int i = 0; i < 10; ++i) {
            const double a = -kPi / 2 + i * kPi / 5, rr = i % 2 == 0 ? 0.5 : 0.2;
            pg << QPointF(std::cos(a) * rr * W, std::sin(a) * rr * H + 0.04 * H);
        }
        path.addPolygon(pg);
        path.closeSubpath();
        break;
    }
    case 5:
        path.moveTo(0, 0.5 * H);
        path.cubicTo(-0.62 * W, 0.05 * H, -0.5 * W, -0.52 * H, 0, -0.24 * H);
        path.cubicTo(0.5 * W, -0.52 * H, 0.62 * W, 0.05 * H, 0, 0.5 * H);
        break;
    case 6: poly({{-0.5, -0.15}, {0.1, -0.15}, {0.1, -0.4}, {0.5, 0}, {0.1, 0.4}, {0.1, 0.15}, {-0.5, 0.15}}); break;
    default:
        path.setFillRule(Qt::OddEvenFill);
        path.addEllipse(r);
        path.addEllipse(QRectF(-W * 0.34, -H * 0.34, W * 0.68, H * 0.68));
        break;
    }
    return path;
}

void renderShape(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double w = layer.width(), h = layer.height();
    const double W = p.num(QStringLiteral("width")) / 100.0 * h, H = p.num(QStringLiteral("height")) / 100.0 * h;
    if (W <= 0 || H <= 0)
        return;
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(p.num(QStringLiteral("x")) * w, p.num(QStringLiteral("y")) * h);
    painter.rotate(p.num(QStringLiteral("angle")));
    const QPainterPath path = shapePath(p.choice(QStringLiteral("shape")), W, H, p.num(QStringLiteral("corner")));
    painter.fillPath(path, withAlpha(p.color(QStringLiteral("color")), p.num(QStringLiteral("opacity"))));
    const double outline = p.num(QStringLiteral("outline")) * ctx.scale;
    if (outline > 0)
        painter.strokePath(path, QPen(p.color(QStringLiteral("outlineColor")), outline, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
}

// ---- Text and test patterns ------------------------------------------------------------

QStringList kineticWords(const Params& p)
{
    return p.text(QStringLiteral("text")).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

void renderKineticText(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const QStringList words = kineticWords(p);
    if (words.isEmpty())
        return;
    const double w = layer.width(), h = layer.height();
    const double T = seconds(ctx), wps = std::max(0.1, p.num(QStringLiteral("rate")));
    QFont f(p.text(QStringLiteral("font")));
    f.setPixelSize(std::max(1, int(std::lround(p.num(QStringLiteral("size")) / 100.0 * std::min(h, w * 9.0 / 16.0))))); // (as titles: 16:9 sizes)
    f.setBold(p.flag(QStringLiteral("bold")));
    f.setHintingPreference(QFont::PreferNoHinting);
    const QFontMetricsF fm(f);
    const double space = fm.horizontalAdvance(QLatin1Char(' ')), lh = fm.lineSpacing() * 1.08;
    // Wrap into centred lines.
    struct Placed {
        int line;
        double x, width;
    };
    std::vector<Placed> placed;
    std::vector<double> lineWidths{0};
    for (const QString& word : words) {
        const double ww = fm.horizontalAdvance(word);
        double& lw = lineWidths.back();
        if (lw > 0 && lw + space + ww > w * 0.86)
            lineWidths.push_back(0);
        double& cur = lineWidths.back();
        const double x = cur > 0 ? cur + space : 0;
        placed.push_back({int(lineWidths.size()) - 1, x, ww});
        cur = x + ww;
    }
    const double top = p.num(QStringLiteral("y")) * h - lh * lineWidths.size() / 2;
    const int style = p.choice(QStringLiteral("style"));
    const int current = int(std::floor(T * wps));
    const QColor col = p.color(QStringLiteral("color")), hi = p.color(QStringLiteral("highlight"));
    const double outline = p.num(QStringLiteral("outline")) * ctx.scale;
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (int i = 0; i < int(words.size()); ++i) {
        const double e = (T - i / wps) / 0.3;
        if (e <= 0)
            break;
        const Placed& pl = placed[size_t(i)];
        double scale = 1.0, dy = 0.0, opacity = clamp01(e);
        if (style == 0) {
            scale = backOut(e);
            opacity = clamp01(e * 3);
        } else if (style == 1) {
            dy = (1.0 - easeOut(e)) * lh * 0.6;
        }
        const double cx = w / 2 - lineWidths[size_t(pl.line)] / 2 + pl.x + pl.width / 2;
        const double cy = top + pl.line * lh + lh / 2 + dy;
        QPainterPath path;
        path.addText(QPointF(-pl.width / 2, (fm.ascent() - fm.descent()) / 2), f, words[i]);
        QTransform xf;
        xf.translate(cx, cy);
        xf.scale(scale, scale);
        const QPainterPath placedPath = xf.map(path);
        painter.setOpacity(opacity * 0.55);
        painter.fillPath(placedPath.translated(3 * ctx.scale, 4 * ctx.scale), Qt::black);
        painter.setOpacity(opacity);
        if (outline > 0)
            painter.strokePath(placedPath, QPen(p.color(QStringLiteral("outlineColor")), outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.fillPath(placedPath, i == current ? hi : col);
    }
}

bool kineticAnimating(const Params& p, const GeneratorContext& ctx)
{
    const double wps = std::max(0.1, p.num(QStringLiteral("rate")));
    return seconds(ctx) <= (kineticWords(p).size() + 1) / wps + 0.35;
}

void renderCountdown(QImage& layer, const Params& p, const GeneratorContext& ctx)
{
    const double w = layer.width(), h = layer.height(), T = seconds(ctx);
    const int start = int(std::lround(p.num(QStringLiteral("start"))));
    const int n = start - int(std::floor(T));
    const QColor ink = p.color(QStringLiteral("ink"));
    layer.fill(p.color(QStringLiteral("background")));
    QPainter painter(&layer);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPointF c(w / 2, h / 2);
    const double R = h * 0.42;
    if (n >= 1) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(p.color(QStringLiteral("sweep")));
        const double big = std::hypot(w, h);
        painter.drawPie(QRectF(c.x() - big, c.y() - big, 2 * big, 2 * big), 90 * 16, -int(std::lround(fract(T) * 360.0 * 16)));
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(ink, h * 0.004));
    painter.drawLine(QPointF(0, c.y()), QPointF(w, c.y()));
    painter.drawLine(QPointF(c.x(), 0), QPointF(c.x(), h));
    painter.setPen(QPen(ink, h * 0.007));
    painter.drawEllipse(c, R, R);
    painter.drawEllipse(c, R * 0.84, R * 0.84);
    if (n < 1)
        return;
    QFont f(QStringLiteral("Arial"));
    f.setBold(true);
    f.setPixelSize(std::max(1, int(h * 0.55)));
    QPainterPath digits;
    const QFontMetricsF fm(f);
    const QString s = QString::number(n);
    digits.addText(QPointF(c.x() - fm.horizontalAdvance(s) / 2, c.y() + (fm.ascent() - fm.descent()) / 2), f, s);
    painter.fillPath(digits, ink);
}

void renderColorBars(QImage& layer, const Params& p, const GeneratorContext&)
{
    const double w = layer.width(), h = layer.height();
    QPainter painter(&layer);
    auto bar = [&](double x0, double x1, double y0, double y1, QRgb c) {
        painter.fillRect(QRectF(x0 * w, y0 * h, (x1 - x0) * w, (y1 - y0) * h), QColor::fromRgb(c));
    };
    if (p.choice(QStringLiteral("style")) == 1) {
        const QRgb full[] = {0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000};
        for (int i = 0; i < 8; ++i)
            bar(i / 8.0, (i + 1) / 8.0, 0, 1, full[i]);
        return;
    }
    const QRgb top[] = {0xbfbfbf, 0xbfbf00, 0x00bfbf, 0x00bf00, 0xbf00bf, 0xbf0000, 0x0000bf};
    const QRgb mid[] = {0x0000bf, 0x131313, 0xbf00bf, 0x131313, 0x00bfbf, 0x131313, 0xbfbfbf};
    for (int i = 0; i < 7; ++i) {
        bar(i / 7.0, (i + 1) / 7.0, 0, 0.67, top[i]);
        bar(i / 7.0, (i + 1) / 7.0, 0.67, 0.75, mid[i]);
    }
    const double q = 5.0 / 28.0;
    bar(0, q, 0.75, 1, 0x00214c);
    bar(q, 2 * q, 0.75, 1, 0xffffff);
    bar(2 * q, 3 * q, 0.75, 1, 0x32006a);
    bar(3 * q, 4 * q, 0.75, 1, 0x131313);
    const double pl = 1.0 / 21.0;
    bar(4 * q, 4 * q + pl, 0.75, 1, 0x090909);
    bar(4 * q + pl, 4 * q + 2 * pl, 0.75, 1, 0x131313);
    bar(4 * q + 2 * pl, 4 * q + 3 * pl, 0.75, 1, 0x1d1d1d);
    bar(4 * q + 3 * pl, 1, 0.75, 1, 0x131313);
}

} // namespace

void addAnimatedGenerators(std::vector<GeneratorDef>& list)
{
    auto add = [&](const char* id, const char* name, const char* category, const char* description, std::vector<ParamDef> params,
                   decltype(GeneratorDef::render) render, decltype(GeneratorDef::isAnimating) animating) {
        GeneratorDef d;
        d.id = QString::fromLatin1(id);
        d.name = QString::fromLatin1(name);
        d.category = QString::fromLatin1(category);
        d.description = QString::fromLatin1(description);
        d.params = std::move(params);
        d.render = std::move(render);
        d.isAnimating = std::move(animating);
        list.push_back(std::move(d));
    };
    const QStringList shapes = {QStringLiteral("Rectangle"), QStringLiteral("Rounded rectangle"), QStringLiteral("Ellipse"),
                                QStringLiteral("Triangle"),  QStringLiteral("Star"),              QStringLiteral("Heart"),
                                QStringLiteral("Arrow"),     QStringLiteral("Ring")};

    add("plasma", "Plasma", "Animated", "Flowing, ever-changing colour waves.",
        {choice("palette", "Palette", kPaletteNames), num("speed", "Speed", 0, 5, 1), num("scale", "Scale", 0.2, 5, 1)}, renderPlasma, always);
    add("aurora", "Aurora", "Animated", "Northern lights rippling over a starry night sky.",
        {color("color1", "Light color", 0x3dffa0), color("color2", "Top color", 0x8a5cff), color("sky", "Sky color", 0x02040f),
         color("horizon", "Horizon color", 0x0b2340), num("speed", "Speed", 0, 5, 1), flag("stars", "Stars", true)},
        renderAurora, always);
    add("starfield", "Starfield", "Animated", "Flying through the stars; long trails make it hyperspace.",
        {num("count", "Stars", 20, 3000, 600, 0), num("speed", "Speed", 0, 10, 1), num("trails", "Trails", 0, 1, 0.3),
         color("color", "Star color", 0xffffff), color("background", "Background", 0x000005)},
        renderStarfield, always);
    list.back().paint = paintStarfield; // (on the GPU too, drawn twice as big for smooth lines)
    list.back().paintScale = 2;
    add("bokeh", "Bokeh Lights", "Animated", "Soft out-of-focus lights drifting and twinkling. Lower the background opacity to lay it over footage.",
        {choice("palette", "Colors", kLightPalettes), num("count", "Lights", 5, 200, 40, 0), num("size", "Size (% of height)", 2, 40, 12, 1, "%"),
         num("speed", "Speed", 0, 5, 1), color("background", "Background", 0x07050f), num("backgroundOpacity", "Background opacity", 0, 1, 1)},
        renderBokeh, always);
    add("retro_grid", "Retro Grid", "Animated", "An endless neon grid racing towards a striped sunset.",
        {color("color", "Grid color", 0xff2bd6), num("speed", "Speed", 0, 5, 1), num("horizon", "Horizon", 0.3, 0.8, 0.55),
         flag("sun", "Sun", true)},
        renderRetroGrid, always);
    list.back().paint = paintRetroGrid;
    list.back().paintScale = 2;
    add("digital_rain", "Digital Rain", "Animated", "Columns of glowing glyphs raining down the screen.",
        {color("color", "Color", 0x35ff69), num("speed", "Speed", 0.1, 5, 1), num("size", "Glyph size (% of height)", 1, 8, 2.4, 1, "%"),
         flag("background", "Black background", true)},
        renderDigitalRain, always);
    add("confetti", "Confetti", "Overlays", "Falling, tumbling confetti over whatever is below.",
        {choice("palette", "Colors", {QStringLiteral("Party"), QStringLiteral("Gold")}), num("count", "Pieces", 10, 1000, 220, 0),
         num("speed", "Speed", 0.1, 4, 1), num("size", "Size", 0.3, 3, 1)},
        renderConfetti, always);
    add("progress_bar", "Progress Bar", "Overlays", "A bar that fills up over the length of the event.",
        {choice("position", "Position", {QStringLiteral("Bottom"), QStringLiteral("Top")}), num("height", "Height (% of frame)", 0.2, 10, 1.2, 1, "%"),
         num("margin", "Margin (% of frame)", 0, 10, 0, 1, "%"), color("color", "Bar color", 0xff3b3b), color("trackColor", "Track color", 0xffffff),
         num("trackOpacity", "Track opacity", 0, 1, 0.25), flag("rounded", "Rounded ends", false)},
        renderProgressBar, always);
    add("letterbox", "Letterbox", "Overlays", "Cinema bars for a wider (or narrower) look; they can slide in and out.",
        {choice("aspect", "Aspect",
                {QStringLiteral("2.39:1 (scope)"), QStringLiteral("2:1"), QStringLiteral("1.85:1"), QStringLiteral("4:3 (pillarbox)"),
                 QStringLiteral("1:1"), QStringLiteral("9:16 (pillarbox)")}),
         color("color", "Color", 0x000000), num("opacity", "Opacity", 0, 1, 1), flag("animate", "Slide in and out", true),
         num("duration", "Slide duration", 0, 5, 0.6, 2, " s")},
        renderLetterbox, letterboxAnimating);
    add("shape", "Shape", "Overlays", "A rectangle, ellipse, star, heart, arrow or ring, with fill and outline. Drag it on the preview.",
        {choice("shape", "Shape", shapes, 1), num("x", "Position X", -0.5, 1.5, 0.5, 3), num("y", "Position Y", -0.5, 1.5, 0.5, 3),
         num("width", "Width (% of height)", 1, 400, 40, 1, "%"), num("height", "Height (% of height)", 1, 400, 40, 1, "%"),
         num("angle", "Rotation", -360, 360, 0, 0, "°"), color("color", "Fill color", 0xff3b6b), num("opacity", "Fill opacity", 0, 1, 1),
         num("corner", "Corner radius (%)", 0, 50, 15, 0, "%"), num("outline", "Outline width", 0, 60, 0, 1, " px"),
         color("outlineColor", "Outline color", 0xffffff)},
        renderShape, nullptr);
    add("kinetic_text", "Kinetic Text", "Text", "Words pop on one at a time, the newest highlighted, like social-media captions.",
        {text("text", "Text", QStringLiteral("Make every single word count"), true), font("font", "Font", "Arial Black"),
         num("size", "Size (% of height)", 1, 30, 9, 1, "%"), flag("bold", "Bold", true), color("color", "Color", 0xffffff),
         color("highlight", "Newest word color", 0xffd400), num("rate", "Words per second", 0.5, 12, 3, 1),
         choice("style", "Animation", {QStringLiteral("Pop"), QStringLiteral("Slide up"), QStringLiteral("Fade")}),
         num("y", "Position Y", -0.5, 1.5, 0.5, 3), num("outline", "Outline width", 0, 30, 0, 1, " px"),
         color("outlineColor", "Outline color", 0x000000)},
        renderKineticText, kineticAnimating);
    add("countdown", "Countdown Leader", "Text", "An old film-leader countdown with a sweeping hand, one number a second.",
        {num("start", "Count from", 1, 20, 5, 0), color("background", "Background", 0xc9c9c9), color("sweep", "Sweep", 0x7a7a7a),
         color("ink", "Lines and numbers", 0x141414)},
        renderCountdown, always);
    add("color_bars", "Color Bars", "Test Patterns", "SMPTE-style colour bars (or full-field 100% bars) for checking levels.",
        {choice("style", "Style", {QStringLiteral("SMPTE"), QStringLiteral("Full-field 100%")})}, renderColorBars, nullptr);
}

void addAnimatedGeneratorPresets(std::vector<GeneratorPreset>& list)
{
    auto add = [&](const char* id, const char* name, const char* category, const char* generator, std::map<QString, double> numbers = {},
                   std::map<QString, QString> texts = {}, TimeUs length = 10 * kUsPerSecond) {
        GeneratorPreset p;
        p.id = QString::fromLatin1(id);
        p.name = QString::fromLatin1(name);
        p.category = QString::fromLatin1(category);
        p.generator = QString::fromLatin1(generator);
        p.numbers = std::move(numbers);
        p.texts = std::move(texts);
        p.length = length;
        list.push_back(std::move(p));
    };
    const QString P = QStringLiteral("palette");
    add("plasma_neon", "Plasma (neon)", "Animated", "plasma");
    add("plasma_fire", "Plasma (fire)", "Animated", "plasma", {{P, 1}, {QStringLiteral("scale"), 0.7}});
    add("plasma_ocean", "Plasma (ocean, slow)", "Animated", "plasma", {{P, 2}, {QStringLiteral("speed"), 0.4}});
    add("aurora", "Aurora", "Animated", "aurora");
    add("starfield", "Starfield", "Animated", "starfield");
    add("hyperspace", "Hyperspace", "Animated", "starfield",
        {{QStringLiteral("speed"), 4}, {QStringLiteral("trails"), 1}, {QStringLiteral("count"), 1200}, {QStringLiteral("color"), double(0xbfe0ff)}});
    add("bokeh_warm", "Bokeh lights (warm)", "Animated", "bokeh");
    add("bokeh_party", "Bokeh lights (party, overlay)", "Animated", "bokeh",
        {{P, 2}, {QStringLiteral("backgroundOpacity"), 0}, {QStringLiteral("count"), 60}});
    add("retro_grid", "Retro grid", "Animated", "retro_grid");
    add("digital_rain", "Digital rain", "Animated", "digital_rain");
    add("confetti", "Confetti", "Overlays", "confetti", {}, {}, 6 * kUsPerSecond);
    add("confetti_gold", "Gold confetti", "Overlays", "confetti", {{P, 1}, {QStringLiteral("count"), 320}}, {}, 6 * kUsPerSecond);
    add("progress_bar", "Progress bar", "Overlays", "progress_bar", {}, {}, 30 * kUsPerSecond);
    add("letterbox", "Letterbox 2.39:1", "Overlays", "letterbox");
    add("pillarbox_vertical", "Pillarbox 9:16", "Overlays", "letterbox", {{QStringLiteral("aspect"), 5}, {QStringLiteral("animate"), 0}});
    add("shape_box", "Shape: rounded box", "Overlays", "shape",
        {{QStringLiteral("width"), 90}, {QStringLiteral("height"), 22}, {QStringLiteral("y"), 0.8}, {QStringLiteral("opacity"), 0.6},
         {QStringLiteral("color"), 0.0}});
    add("shape_heart", "Shape: heart", "Overlays", "shape", {{QStringLiteral("shape"), 5}, {QStringLiteral("color"), double(0xff2d55)}});
    add("shape_arrow", "Shape: arrow", "Overlays", "shape",
        {{QStringLiteral("shape"), 6}, {QStringLiteral("width"), 50}, {QStringLiteral("height"), 25}, {QStringLiteral("color"), double(0xffd400)},
         {QStringLiteral("outline"), 4}, {QStringLiteral("outlineColor"), 0.0}});
    add("kinetic_text", "Kinetic words", "Text", "kinetic_text", {}, {}, 5 * kUsPerSecond);
    add("kinetic_caption", "Kinetic caption (lower)", "Text", "kinetic_text",
        {{QStringLiteral("size"), 6}, {QStringLiteral("y"), 0.82}, {QStringLiteral("style"), 1}, {QStringLiteral("outline"), 5}},
        {{QStringLiteral("text"), QStringLiteral("Captions that keep up with you")}}, 4 * kUsPerSecond);
    add("countdown", "Countdown leader", "Text", "countdown", {}, {}, 5 * kUsPerSecond);
    add("bars_smpte", "Color bars (SMPTE)", "Test Patterns", "color_bars");
    add("bars_full", "Color bars (full field)", "Test Patterns", "color_bars", {{QStringLiteral("style"), 1}});
}
