#include "core/EffectMask.h"

#include "core/ImageOps.h"
#include "core/Parallel.h"

#include <QPainter>
#include <QPainterPath>

#include <numbers>

namespace {
ParamDef num(const char* id, const char* name, double min, double max, double def, int decimals, const char* suffix = "")
{
    ParamDef d;
    d.id = QString::fromLatin1(id);
    d.name = QString::fromLatin1(name);
    d.min = min;
    d.max = max;
    d.def = def;
    d.decimals = decimals;
    d.suffix = QString::fromLatin1(suffix);
    return d;
}
} // namespace

const std::vector<ParamDef>& effectMaskParams()
{
    static const std::vector<ParamDef> params = [] {
        std::vector<ParamDef> v;
        ParamDef shape;
        shape.id = QStringLiteral("mask.shape");
        shape.name = QStringLiteral("Mask");
        shape.type = ParamType::Choice;
        shape.choices = {QStringLiteral("Off (everywhere)"), QStringLiteral("Rectangle"), QStringLiteral("Ellipse"), QStringLiteral("Freehand")};
        shape.tooltip = QStringLiteral("Limit the effect to a rectangle, an ellipse or a freehand shape (drag its points on the preview; "
                                       "Alt+click the outline to add a point, Alt+click a point to remove it)");
        v.push_back(shape);
        v.push_back(num("mask.x", "Mask X", 0, 1, 0.5, 3));
        v.push_back(num("mask.y", "Mask Y", 0, 1, 0.5, 3));
        v.push_back(num("mask.w", "Mask width", 0.01, 1.5, 0.35, 3));
        v.push_back(num("mask.h", "Mask height", 0.01, 1.5, 0.45, 3));
        v.push_back(num("mask.rotation", "Mask angle", -180, 180, 0, 1, "°"));
        v.push_back(num("mask.feather", "Mask feather", 0, 30, 2, 1, " %"));
        ParamDef invert;
        invert.id = QStringLiteral("mask.invert");
        invert.name = QStringLiteral("Outside the mask");
        invert.type = ParamType::Bool;
        invert.tooltip = QStringLiteral("Apply the effect everywhere except inside the mask");
        v.push_back(invert);
        v.push_back(num("mask.points", "Freehand points", 3, 16, 6, 0));
        ParamDef smooth;
        smooth.id = QStringLiteral("mask.smooth");
        smooth.name = QStringLiteral("Smooth curve");
        smooth.type = ParamType::Bool;
        smooth.def = 1;
        smooth.tooltip = QStringLiteral("Freehand: a smooth curve through the points (off: straight lines)");
        v.push_back(smooth);
        ParamDef animate;
        animate.id = QStringLiteral("mask.animate");
        animate.name = QStringLiteral("Animate shape");
        animate.type = ParamType::Bool;
        animate.tooltip = QStringLiteral("Freehand: dragging a point keys it at the cursor, so the shape can change over time (rotoscoping)");
        v.push_back(animate);
        return v;
    }();
    return params;
}

QPointF maskPoint(const ParamMap& params, int i, int n, TimeUs t)
{
    const double a = -std::numbers::pi / 2 + 2 * std::numbers::pi * i / std::max(n, 1);
    auto value = [&](const QString& id, double def) {
        const auto it = params.find(id);
        return it == params.end() ? def : it->second.at(t, ParamType::Number);
    };
    return {value(QStringLiteral("mask.p%1x").arg(i), 0.5 * std::cos(a)), value(QStringLiteral("mask.p%1y").arg(i), 0.5 * std::sin(a))};
}

EffectMask effectMask(const ParamMap& params, TimeUs t, QSize size)
{
    EffectMask m;
    auto it = params.find(QStringLiteral("mask.shape"));
    if (it == params.end() || size.isEmpty())
        return m;
    const Params p = resolveParams(effectMaskParams(), params, t);
    m.shape = std::clamp(p.choice(QStringLiteral("mask.shape")), 0, 3);
    if (!m.shape)
        return m;
    const float w = float(size.width()), h = float(size.height());
    m.cx = float(p.num(QStringLiteral("mask.x"))) * w;
    m.cy = float(p.num(QStringLiteral("mask.y"))) * h;
    m.hw = float(p.num(QStringLiteral("mask.w"))) * w / 2;
    m.hh = float(p.num(QStringLiteral("mask.h"))) * h / 2;
    const double a = p.num(QStringLiteral("mask.rotation")) * std::numbers::pi / 180.0;
    m.cs = float(std::cos(a));
    m.sn = float(std::sin(a));
    m.feather = float(p.num(QStringLiteral("mask.feather"))) / 100.0f * h;
    m.invert = p.flag(QStringLiteral("mask.invert"));
    if (m.shape == 3) {
        m.animate = p.flag(QStringLiteral("mask.animate"));
        const int n = std::clamp(int(std::lround(p.num(QStringLiteral("mask.points")))), 3, 16);
        for (int i = 0; i < n; ++i) {
            const QPointF q = maskPoint(params, i, n, t);
            const double qx = q.x() * 2 * m.hw, qy = q.y() * 2 * m.hh;
            m.points.emplace_back(m.cx + qx * m.cs - qy * m.sn, m.cy + qx * m.sn + qy * m.cs);
        }
        QPainterPath path(m.points[0]);
        if (p.flag(QStringLiteral("mask.smooth"))) {
            // A closed Catmull-Rom curve through the points, as Beziers.
            for (int i = 0; i < n; ++i) {
                const QPointF p0 = m.points[size_t((i + n - 1) % n)], p1 = m.points[size_t(i)];
                const QPointF p2 = m.points[size_t((i + 1) % n)], p3 = m.points[size_t((i + 2) % n)];
                path.cubicTo(p1 + (p2 - p0) / 6, p2 - (p3 - p1) / 6, p2);
            }
        } else {
            for (int i = 1; i < n; ++i)
                path.lineTo(m.points[size_t(i)]);
            path.closeSubpath();
        }
        m.alpha = QImage(size, QImage::Format_ARGB32_Premultiplied);
        m.alpha.fill(Qt::transparent);
        {
            QPainter painter(&m.alpha);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.fillPath(path, Qt::white);
        }
        m.outline = path;
        if (m.feather > 0.5f) // the soft edge across the outline, about as wide as the feather
            imageops::gaussianBlur(m.alpha, m.feather / 2.5, m.feather / 2.5);
    }
    return m;
}

void applyEffectMask(QImage& out, const QImage& before, const EffectMask& m)
{
    if (!m.active() || out.size() != before.size() || before.isNull())
        return;
    const int w = out.width();
    out.bits(); // detach here, not from several threads
    parallelFor(out.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const uint32_t* in = reinterpret_cast<const uint32_t*>(before.constScanLine(y));
            uint32_t* row = reinterpret_cast<uint32_t*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float t = maskWeight(m, float(x) + 0.5f, float(y) + 0.5f);
                if (t >= 1.0f)
                    continue;
                const uint32_t pa = in[x];
                if (t <= 0.0f) {
                    row[x] = pa;
                    continue;
                }
                const uint32_t pb = row[x], k = uint32_t(t * 256.0f), s = 256 - k;
                const uint32_t rb = (((pa & 0x00ff00ffu) * s + (pb & 0x00ff00ffu) * k) >> 8) & 0x00ff00ffu;
                const uint32_t ag = ((((pa >> 8) & 0x00ff00ffu) * s + ((pb >> 8) & 0x00ff00ffu) * k)) & 0xff00ff00u;
                row[x] = rb | ag;
            }
        }
    });
}
