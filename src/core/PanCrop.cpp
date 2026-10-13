#include "core/PanCrop.h"

#include <algorithm>

const std::vector<ParamDef>& panCropParams()
{
    static const std::vector<ParamDef> defs = [] {
        std::vector<ParamDef> d;
        auto add = [&](const char* id, const char* name, ParamType type, double min, double max, double def, int decimals,
                       const char* suffix = "", QStringList choices = {}) {
            ParamDef p;
            p.id = QString::fromLatin1(id);
            p.name = QString::fromLatin1(name);
            p.type = type;
            p.min = min;
            p.max = max;
            p.def = def;
            p.decimals = decimals;
            p.suffix = QString::fromUtf8(suffix);
            p.choices = std::move(choices);
            d.push_back(p);
        };
        add("mode", "Framing", ParamType::Choice, 0, 2, 0, 0, "",
            {QStringLiteral("Fit (letterbox)"), QStringLiteral("Fill (crop to fill frame)"), QStringLiteral("Stretch")});
        add("zoom", "Zoom", ParamType::Number, 0.05, 20, 1, 3, "x");
        d.back().logScale = true;
        add("centerX", "Center X", ParamType::Number, -1, 2, 0.5, 3);
        add("centerY", "Center Y", ParamType::Number, -1, 2, 0.5, 3);
        add("rotation", "Rotation", ParamType::Number, -720, 720, 0, 1, "°");
        add("flipH", "Flip horizontally", ParamType::Bool, 0, 1, 0, 0);
        add("flipV", "Flip vertically", ParamType::Bool, 0, 1, 0, 0);
        return d;
    }();
    return defs;
}

bool isDefaultPanCrop(const ParamMap& params)
{
    for (const ParamDef& d : panCropParams()) {
        auto it = params.find(d.id);
        if (it == params.end())
            continue;
        if (!it->second.keys.empty() || it->second.value != d.def)
            return false;
    }
    return true;
}

QTransform panCropTransform(const Params& p, QSizeF src, QSizeF canvas)
{
    QTransform t;
    if (src.isEmpty() || canvas.isEmpty())
        return t;
    const int mode = p.choice(QStringLiteral("mode"));
    double sx = canvas.width() / src.width(), sy = canvas.height() / src.height();
    if (mode == 0)
        sx = sy = std::min(sx, sy);
    else if (mode == 1)
        sx = sy = std::max(sx, sy);
    const double zoom = std::max(0.001, p.num(QStringLiteral("zoom")));
    // Built canvas-side first: points go source -> centred -> scaled -> rotated -> placed.
    t.translate(canvas.width() / 2, canvas.height() / 2);
    t.rotate(p.num(QStringLiteral("rotation")));
    t.scale(sx * zoom * (p.flag(QStringLiteral("flipH")) ? -1 : 1), sy * zoom * (p.flag(QStringLiteral("flipV")) ? -1 : 1));
    t.translate(-p.num(QStringLiteral("centerX")) * src.width(), -p.num(QStringLiteral("centerY")) * src.height());
    return t;
}
