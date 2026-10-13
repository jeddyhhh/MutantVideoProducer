// Built-in transitions. `from`/`to` are premultiplied ARGB32 layers the size
// of the output (either may be null = transparent); p runs 0..1.

#include "core/Effects.h"
#include "core/ImageOps.h"
#include "core/TransitionKernels.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

using namespace imageops;

namespace {

constexpr float kPi = 3.14159265f;

ParamDef num(const char* id, const char* name, double min, double max, double def, int decimals = 2, const char* suffix = "")
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.min = min;
    p.max = max;
    p.def = def;
    p.decimals = decimals;
    p.suffix = QString::fromUtf8(suffix);
    return p;
}

ParamDef choice(const char* id, const char* name, QStringList choices, int def = 0)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Choice;
    p.choices = std::move(choices);
    p.max = p.choices.size() - 1;
    p.def = def;
    return p;
}

ParamDef color(const char* id, const char* name, uint32_t rgb)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Color;
    p.max = 0xffffff;
    p.def = rgb;
    return p;
}

const QStringList kDirections = {QStringLiteral("From left"), QStringLiteral("From right"), QStringLiteral("From top"),
                                 QStringLiteral("From bottom")};

float ease(double p)
{
    const float t = float(std::clamp(p, 0.0, 1.0));
    return t * t * (3.0f - 2.0f * t);
}

QPointF directionOffset(int dir, double amount, QSize size)
{
    switch (dir) {
    case 0: return {-amount * size.width(), 0};
    case 1: return {amount * size.width(), 0};
    case 2: return {0, -amount * size.height()};
    default: return {0, amount * size.height()};
    }
}

void drawLayer(QPainter& painter, const QImage* img, QPointF at, double opacity = 1.0)
{
    if (!img || opacity <= 0)
        return;
    painter.setOpacity(opacity);
    painter.drawImage(at, *img);
    painter.setOpacity(1.0);
}

std::vector<TransitionDef> buildTransitions()
{
    std::vector<TransitionDef> list;
    auto add = [&](TransitionDef d) { list.push_back(std::move(d)); };
    TransitionDef d;

    d = {};
    d.id = QStringLiteral("dissolve");
    d.name = QStringLiteral("Crossfade");
    d.description = QStringLiteral("Smooth blend from one picture to the next (the default).");
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params&, const RenderContext&) {
        mix(out, a, b, float(p));
    };
    add(d);

    d = {};
    d.id = QStringLiteral("dip");
    d.name = QStringLiteral("Dip to Color");
    d.description = QStringLiteral("Fade out to a colour (black by default), then fade in.");
    d.params = {color("color", "Color", 0x000000)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        QImage solid(out.size(), QImage::Format_ARGB32_Premultiplied);
        solid.fill(prm.color(QStringLiteral("color")));
        if (p < 0.5)
            mix(out, a, &solid, float(p * 2));
        else
            mix(out, &solid, b, float((p - 0.5) * 2));
    };
    add(d);

    d = {};
    d.id = QStringLiteral("wipe");
    d.name = QStringLiteral("Linear Wipe");
    d.description = QStringLiteral("A straight edge sweeps across the frame at any angle.");
    d.params = {num("angle", "Angle", -180, 180, 0, 0, "°"), num("softness", "Softness", 0, 1, 0.1)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const float ang = float(prm.num(QStringLiteral("angle"))) * kPi / 180.0f;
        const float soft = float(prm.num(QStringLiteral("softness")));
        const float c = std::cos(ang), s = std::sin(ang);
        const float w = float(out.width()), h = float(out.height());
        const float extent = (std::abs(c) * w + std::abs(s) * h) / 2.0f;
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            const float proj = (x + 0.5f - w / 2) * c + (y + 0.5f - h / 2) * s;
            return wipeMask(0.5f + proj / (2.0f * extent), pp, soft);
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("slide");
    d.name = QStringLiteral("Slide");
    d.description = QStringLiteral("The next picture slides in over the current one.");
    d.params = {choice("direction", "Direction", kDirections)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        out.fill(Qt::transparent);
        QPainter painter(&out);
        drawLayer(painter, a, {0, 0});
        drawLayer(painter, b, directionOffset(prm.choice(QStringLiteral("direction")), 1.0 - ease(p), out.size()));
    };
    add(d);

    d = {};
    d.id = QStringLiteral("push");
    d.name = QStringLiteral("Push");
    d.description = QStringLiteral("The next picture pushes the current one out of frame.");
    d.params = {choice("direction", "Direction", kDirections)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        out.fill(Qt::transparent);
        QPainter painter(&out);
        const int dir = prm.choice(QStringLiteral("direction"));
        const double e = ease(p);
        drawLayer(painter, a, -directionOffset(dir, e, out.size()));
        drawLayer(painter, b, directionOffset(dir, 1.0 - e, out.size()));
    };
    add(d);

    d = {};
    d.id = QStringLiteral("iris");
    d.name = QStringLiteral("Iris");
    d.description = QStringLiteral("A growing shape reveals the next picture.");
    d.params = {choice("shape", "Shape", {QStringLiteral("Circle"), QStringLiteral("Diamond"), QStringLiteral("Rectangle")}),
                num("centerX", "Center X", 0, 1, 0.5), num("centerY", "Center Y", 0, 1, 0.5), num("softness", "Softness", 0, 1, 0.1)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const int shape = prm.choice(QStringLiteral("shape"));
        const float w = float(out.width()), h = float(out.height());
        const float cx = float(prm.num(QStringLiteral("centerX"))) * w, cy = float(prm.num(QStringLiteral("centerY"))) * h;
        const float soft = float(prm.num(QStringLiteral("softness")));
        const float mx = std::max(cx, w - cx), my = std::max(cy, h - cy);
        const float maxD = shape == 0 ? std::hypot(mx, my) : (shape == 1 ? mx + my : 1.0f);
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            const float dx = std::abs(x + 0.5f - cx), dy = std::abs(y + 0.5f - cy);
            float s;
            if (shape == 0)
                s = std::hypot(dx, dy) / maxD;
            else if (shape == 1)
                s = (dx + dy) / maxD;
            else
                s = std::max(dx / mx, dy / my);
            return wipeMask(s, pp, soft);
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("clock");
    d.name = QStringLiteral("Clock Wipe");
    d.description = QStringLiteral("A sweeping hand reveals the next picture.");
    d.params = {num("start", "Start angle", -180, 180, 0, 0, "°"), num("softness", "Softness", 0, 0.5, 0.02),
                choice("rotation", "Rotation", {QStringLiteral("Clockwise"), QStringLiteral("Counter-clockwise")})};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const float w = float(out.width()), h = float(out.height());
        const float start = float(prm.num(QStringLiteral("start"))) / 360.0f;
        const float soft = float(prm.num(QStringLiteral("softness")));
        const bool ccw = prm.choice(QStringLiteral("rotation")) == 1;
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            // 0 at 12 o'clock, increasing clockwise.
            float ang = std::atan2(x + 0.5f - w / 2, -(y + 0.5f - h / 2)) / (2 * kPi);
            if (ccw)
                ang = -ang;
            float s = ang - start;
            s -= std::floor(s);
            return wipeMask(s, pp, soft);
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("barn_door");
    d.name = QStringLiteral("Barn Door");
    d.description = QStringLiteral("Opens from the middle like two doors.");
    d.params = {choice("orientation", "Orientation", {QStringLiteral("Vertical split"), QStringLiteral("Horizontal split")}),
                num("softness", "Softness", 0, 1, 0.05)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const bool horizontal = prm.choice(QStringLiteral("orientation")) == 1;
        const float w = float(out.width()), h = float(out.height());
        const float soft = float(prm.num(QStringLiteral("softness")));
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            const float s = horizontal ? std::abs((y + 0.5f) / h - 0.5f) * 2 : std::abs((x + 0.5f) / w - 0.5f) * 2;
            return wipeMask(s, pp, soft);
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("blinds");
    d.name = QStringLiteral("Venetian Blinds");
    d.description = QStringLiteral("Many strips wipe at once.");
    d.params = {num("count", "Strips", 2, 64, 10, 0), choice("orientation", "Orientation", {QStringLiteral("Horizontal"), QStringLiteral("Vertical")}),
                num("softness", "Softness", 0, 1, 0.1)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const float n = float(std::round(prm.num(QStringLiteral("count"))));
        const bool vertical = prm.choice(QStringLiteral("orientation")) == 1;
        const float w = float(out.width()), h = float(out.height());
        const float soft = float(prm.num(QStringLiteral("softness")));
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            float s = vertical ? (x + 0.5f) / w * n : (y + 0.5f) / h * n;
            s -= std::floor(s);
            return wipeMask(s, pp, soft);
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("pixel_dissolve");
    d.name = QStringLiteral("Pixel Dissolve");
    d.description = QStringLiteral("Random blocks switch to the next picture.");
    d.params = {num("size", "Block size", 1, 100, 8, 0, " px")};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext& ctx) {
        const int block = std::max(1, int(std::lround(prm.num(QStringLiteral("size")) * ctx.scale)));
        const float pp = float(p);
        maskBlend(out, a, b, [=](int x, int y) {
            uint32_t hsh = uint32_t(x / block) * 374761393u + uint32_t(y / block) * 668265263u;
            hsh = (hsh ^ (hsh >> 13)) * 1274126177u;
            hsh ^= hsh >> 16;
            return (hsh & 0xffffff) / float(0x1000000) < pp ? 1.0f : 0.0f;
        });
    };
    add(d);

    d = {};
    d.id = QStringLiteral("cross_zoom");
    d.name = QStringLiteral("Cross Zoom");
    d.description = QStringLiteral("Zooms into the current picture while the next one zooms out into place.");
    d.params = {num("amount", "Zoom amount", 0.1, 4, 1)};
    d.apply = [](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext&) {
        const double amount = prm.num(QStringLiteral("amount"));
        const double e = ease(p);
        out.fill(Qt::transparent);
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QPointF c(out.width() / 2.0, out.height() / 2.0);
        auto zoomed = [&](const QImage* img, double scale, double opacity) {
            if (!img || opacity <= 0)
                return;
            painter.save();
            painter.translate(c);
            painter.scale(scale, scale);
            painter.translate(-c);
            painter.setOpacity(opacity);
            painter.drawImage(QPointF(0, 0), *img);
            painter.restore();
        };
        zoomed(a, 1.0 + e * amount, 1.0);
        zoomed(b, 1.0 + (1.0 - e) * amount, e);
    };
    add(d);

    // Glitch, Ripple, Cube Spin, Film Burn... (TransitionKernels.cpp)
    tfx::addTransitions(list);
    return list;
}

} // namespace

const std::vector<TransitionDef>& transitions()
{
    static const std::vector<TransitionDef> list = buildTransitions();
    return list;
}

const TransitionDef* findTransition(const QString& id)
{
    for (const auto& t : transitions())
        if (t.id == id)
            return &t;
    return nullptr;
}
