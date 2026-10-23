#include "core/Generators.h"
#include "core/Graphics.h"

#include "core/ImageOps.h"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {

// ---- Parameter helpers -------------------------------------------------------

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

ParamDef flag(const char* id, const char* name, bool def)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Bool;
    p.max = 1;
    p.def = def ? 1 : 0;
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

// Shared styling parameters for everything that draws text.
std::vector<ParamDef> textStyleParams(double size, int align, double x, double y)
{
    return {font("font", "Font", "Arial"),
            num("size", "Size (% of height)", 0.5, 50, size, 1, "%"),
            flag("bold", "Bold", true),
            flag("italic", "Italic", false),
            color("color", "Text color", 0xffffff),
            choice("align", "Alignment", {QStringLiteral("Left"), QStringLiteral("Center"), QStringLiteral("Right")}, align),
            num("x", "Position X", -0.5, 1.5, x, 3),
            num("y", "Position Y", -0.5, 1.5, y, 3),
            num("lineSpacing", "Line spacing", 0.5, 3, 1.0),
            num("tracking", "Letter spacing", -50, 200, 0, 0, "%"),
            num("outline", "Outline width", 0, 40, 0, 1, " px"),
            color("outlineColor", "Outline color", 0x000000),
            num("shadow", "Shadow opacity", 0, 1, 0.6),
            color("shadowColor", "Shadow color", 0x000000),
            num("shadowX", "Shadow offset X", -50, 50, 4, 1, " px"),
            num("shadowY", "Shadow offset Y", -50, 50, 4, 1, " px"),
            num("shadowBlur", "Shadow blur", 0, 50, 6, 1, " px"),
            num("box", "Background box opacity", 0, 1, 0),
            color("boxColor", "Background box color", 0x000000),
            num("boxPadding", "Background box padding", 0, 200, 20, 0, " px"),
            num("boxRadius", "Background box corner radius", 0, 100, 6, 0, " px")};
}

// ---- Text layout & drawing -----------------------------------------------------

struct TextStyle {
    QFont font;
    QFont headingFont;
    QColor color;
    int align = 1;
    double lineSpacing = 1.0;
    double outline = 0;        // output pixels
    QColor outlineColor;
    double shadow = 0;
    QColor shadowColor;
    QPointF shadowOffset;
    double shadowBlur = 0;
    double box = 0;
    QColor boxColor;
    double boxPadding = 0;
    double boxRadius = 0;
};

TextStyle styleFrom(const Params& p, QSize canvas, double scale, double sizeOverride = -1)
{
    TextStyle s;
    s.font = QFont(p.text(QStringLiteral("font")));
    const double sizePct = sizeOverride > 0 ? sizeOverride : p.num(QStringLiteral("size"));
    // Sizes are a percentage of a 16:9 picture's height: in a taller frame (4:3, 9:16) that is the height of the
    // widest 16:9 area that fits, so a title that fits across a 16:9 frame fits across any other.
    const double reference = std::min<double>(canvas.height(), canvas.width() * 9.0 / 16.0);
    s.font.setPixelSize(std::max(1, int(std::lround(sizePct * reference / 100.0))));
    s.font.setBold(p.flag(QStringLiteral("bold")));
    s.font.setItalic(p.flag(QStringLiteral("italic")));
    s.font.setLetterSpacing(QFont::PercentageSpacing, 100.0 + p.num(QStringLiteral("tracking")));
    s.font.setHintingPreference(QFont::PreferNoHinting); // smooth when animated
    s.headingFont = s.font;
    s.headingFont.setPixelSize(std::max(1, int(std::lround(s.font.pixelSize() * 1.4))));
    s.headingFont.setBold(true);
    s.color = p.color(QStringLiteral("color"));
    s.align = p.choice(QStringLiteral("align"));
    s.lineSpacing = p.num(QStringLiteral("lineSpacing"));
    s.outline = p.num(QStringLiteral("outline")) * scale;
    s.outlineColor = p.color(QStringLiteral("outlineColor"));
    s.shadow = p.num(QStringLiteral("shadow"));
    s.shadowColor = p.color(QStringLiteral("shadowColor"));
    s.shadowOffset = QPointF(p.num(QStringLiteral("shadowX")), p.num(QStringLiteral("shadowY"))) * scale;
    s.shadowBlur = p.num(QStringLiteral("shadowBlur")) * scale;
    s.box = p.num(QStringLiteral("box"));
    s.boxColor = p.color(QStringLiteral("boxColor"));
    s.boxPadding = p.num(QStringLiteral("boxPadding")) * scale;
    s.boxRadius = p.num(QStringLiteral("boxRadius")) * scale;
    return s;
}

struct TextBlock {
    QPainterPath path;         // glyph outlines, block coordinates (top-left origin)
    QSizeF size;
};

// Lays out lines with alignment. Only the first `visibleChars` characters are
// added (typewriter), but positions come from the full text so nothing shifts.
// With `headings`, lines starting with "# " use the heading font.
TextBlock layoutText(const QString& text, const TextStyle& st, int visibleChars, bool headings)
{
    struct Line {
        QString text;
        const QFont* font;
        double width;
        double ascent;
        double advance;
    };
    std::vector<Line> lines;
    const QFontMetricsF body(st.font), head(st.headingFont);
    for (QString l : text.split(QLatin1Char('\n'))) {
        const bool isHeading = headings && l.startsWith(QLatin1String("# "));
        if (isHeading)
            l = l.mid(2);
        const QFont* f = isHeading ? &st.headingFont : &st.font;
        const QFontMetricsF& fm = isHeading ? head : body;
        lines.push_back({l, f, fm.horizontalAdvance(l), fm.ascent(), fm.lineSpacing() * st.lineSpacing});
    }
    TextBlock block;
    double width = 0, height = 0;
    for (const auto& l : lines)
        width = std::max(width, l.width);
    int remaining = visibleChars < 0 ? std::numeric_limits<int>::max() : visibleChars;
    double y = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line& l = lines[i];
        const double x = st.align == 0 ? 0.0 : (st.align == 1 ? (width - l.width) / 2 : width - l.width);
        const QString shown = l.text.left(std::max(0, remaining));
        remaining -= int(l.text.size());
        if (!shown.isEmpty())
            block.path.addText(QPointF(x, y + l.ascent), *l.font, shown);
        // The last line contributes its font height, not the full line advance.
        height = (i + 1 == lines.size()) ? y + QFontMetricsF(*l.font).height() : y;
        y += l.advance;
    }
    block.size = QSizeF(width, height);
    return block;
}

void paintTextBlock(QPainter& painter, QSize layer, const TextBlock& block, const QTransform& xf, const TextStyle& st, double opacity)
{
    if (opacity <= 0.002)
        return;
    const QPainterPath path = xf.map(block.path);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (st.box > 0) {
        const QRectF r = xf.mapRect(QRectF(QPointF(0, 0), block.size))
                             .adjusted(-st.boxPadding, -st.boxPadding, st.boxPadding, st.boxPadding);
        QColor c = st.boxColor;
        c.setAlphaF(float(std::clamp(st.box * opacity, 0.0, 1.0)));
        painter.setPen(Qt::NoPen);
        painter.setBrush(c);
        painter.drawRoundedRect(r, st.boxRadius, st.boxRadius);
    }
    if (st.shadow > 0 && !path.isEmpty()) {
        // Shadow rendered into a small image around the text, then blurred.
        const double margin = st.shadowBlur * 3 + st.outline + 2;
        const QRect area = path.boundingRect().translated(st.shadowOffset).adjusted(-margin, -margin, margin, margin).toAlignedRect()
                               .intersected(QRect(QPoint(), layer));
        if (!area.isEmpty()) {
            QImage sh(area.size(), QImage::Format_ARGB32_Premultiplied);
            sh.fill(Qt::transparent);
            {
                QPainter sp(&sh);
                sp.setRenderHint(QPainter::Antialiasing, true);
                sp.translate(st.shadowOffset - QPointF(area.topLeft()));
                if (st.outline > 0)
                    sp.strokePath(path, QPen(st.shadowColor, st.outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                sp.fillPath(path, st.shadowColor);
            }
            if (st.shadowBlur > 0.3)
                imageops::gaussianBlur(sh, st.shadowBlur / 2, st.shadowBlur / 2);
            painter.setOpacity(std::clamp(st.shadow * opacity, 0.0, 1.0));
            painter.drawImage(area.topLeft(), sh);
        }
    }
    painter.setOpacity(std::clamp(opacity, 0.0, 1.0));
    if (st.outline > 0)
        painter.strokePath(path, QPen(st.outlineColor, st.outline * 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.fillPath(path, st.color);
}

// Places a block by its alignment anchor at (x, y) fractions of the canvas,
// with optional offset and scale about the block centre.
QTransform placeBlock(const TextBlock& block, const TextStyle& st, QSize canvas, double x, double y, QPointF offset = {},
                      double scale = 1.0)
{
    const double ax = x * canvas.width(), ay = y * canvas.height();
    const double left = st.align == 0 ? ax : (st.align == 1 ? ax - block.size.width() / 2 : ax - block.size.width());
    const double top = ay - block.size.height() / 2;
    QTransform t;
    t.translate(left + block.size.width() / 2 + offset.x(), top + block.size.height() / 2 + offset.y());
    t.scale(scale, scale);
    t.translate(-block.size.width() / 2, -block.size.height() / 2);
    return t;
}

int countChars(const QString& s)
{
    return int(s.size() - s.count(QLatin1Char('\n')));
}

double easeOut(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
}

const QStringList kInAnimations = {QStringLiteral("None"),          QStringLiteral("Fade"),
                                   QStringLiteral("Slide from left"), QStringLiteral("Slide from right"),
                                   QStringLiteral("Slide from top"),  QStringLiteral("Slide from bottom"),
                                   QStringLiteral("Zoom in"),         QStringLiteral("Typewriter")};
const QStringList kOutAnimations = {QStringLiteral("None"),        QStringLiteral("Fade"),
                                    QStringLiteral("Slide to left"), QStringLiteral("Slide to right"),
                                    QStringLiteral("Slide to top"),  QStringLiteral("Slide to bottom"),
                                    QStringLiteral("Zoom out")};

struct AnimState {
    double opacity = 1.0;
    QPointF offset;
    double scale = 1.0;
    int visibleChars = -1;
    bool active = false;
};

// e = progress 1 (fully shown) .. 0 (fully gone).
void applyAnimation(int type, double e, QSize canvas, int totalChars, AnimState& st)
{
    if (type == 0 || e >= 1.0)
        return;
    st.active = true;
    const double k = easeOut(e);
    switch (type) {
    case 1: st.opacity *= e; break;
    case 2: st.offset += QPointF(-(1 - k) * canvas.width(), 0); st.opacity *= std::min(1.0, e * 2); break;
    case 3: st.offset += QPointF((1 - k) * canvas.width(), 0); st.opacity *= std::min(1.0, e * 2); break;
    case 4: st.offset += QPointF(0, -(1 - k) * canvas.height() * 0.6); st.opacity *= std::min(1.0, e * 2); break;
    case 5: st.offset += QPointF(0, (1 - k) * canvas.height() * 0.6); st.opacity *= std::min(1.0, e * 2); break;
    case 6: st.scale *= 0.3 + 0.7 * k; st.opacity *= e; break;
    case 7: st.visibleChars = int(std::floor(totalChars * e)); break;
    }
}

AnimState titleAnimation(const Params& p, const GeneratorContext& ctx, QSize canvas, int totalChars)
{
    AnimState st;
    const TimeUs dIn = TimeUs(p.num(QStringLiteral("inDuration")) * kUsPerSecond);
    const TimeUs dOut = TimeUs(p.num(QStringLiteral("outDuration")) * kUsPerSecond);
    if (dIn > 0)
        applyAnimation(p.choice(QStringLiteral("animIn")), double(ctx.time) / double(dIn), canvas, totalChars, st);
    if (dOut > 0) {
        int out = p.choice(QStringLiteral("animOut"));
        applyAnimation(out, double(ctx.length - ctx.time) / double(dOut), canvas, totalChars, st);
    }
    return st;
}

bool titleAnimating(const Params& p, const GeneratorContext& ctx)
{
    const TimeUs dIn = TimeUs(p.num(QStringLiteral("inDuration")) * kUsPerSecond);
    const TimeUs dOut = TimeUs(p.num(QStringLiteral("outDuration")) * kUsPerSecond);
    return (p.choice(QStringLiteral("animIn")) != 0 && ctx.time < dIn)
           || (p.choice(QStringLiteral("animOut")) != 0 && ctx.time > ctx.length - dOut);
}

// ---- Generators ----------------------------------------------------------------

void paintTitle(QPainter& painter, QSize layer, const Params& p, const GeneratorContext& ctx)
{
    const QString str = p.text(QStringLiteral("text"));
    if (str.isEmpty())
        return;
    const TextStyle st = styleFrom(p, layer, ctx.scale);
    const AnimState anim = titleAnimation(p, ctx, layer, countChars(str));
    const TextBlock block = layoutText(str, st, anim.visibleChars, false);
    const QTransform xf = placeBlock(block, st, layer, p.num(QStringLiteral("x")), p.num(QStringLiteral("y")), anim.offset, anim.scale);
    paintTextBlock(painter, layer, block, xf, st, anim.opacity);
}

void paintCredits(QPainter& painter, QSize layer, const Params& p, const GeneratorContext& ctx)
{
    const QString str = p.text(QStringLiteral("text"));
    if (str.isEmpty())
        return;
    const TextStyle st = styleFrom(p, layer, ctx.scale);
    const TextBlock block = layoutText(str, st, -1, true);
    // Scroll from just below the frame to just above it over the event.
    const double progress = ctx.length > 0 ? double(ctx.time) / double(ctx.length) : 0.0;
    const double h = layer.height();
    const double top = h - (h + block.size.height()) * progress;
    const double ax = p.num(QStringLiteral("x")) * layer.width();
    const double left = st.align == 0 ? ax : (st.align == 1 ? ax - block.size.width() / 2 : ax - block.size.width());
    QTransform xf;
    xf.translate(left, top);
    paintTextBlock(painter, layer, block, xf, st, 1.0);
}

void paintTimecode(QPainter& painter, QSize layer, const Params& p, const GeneratorContext& ctx)
{
    const int mode = p.choice(QStringLiteral("mode"));
    const TimeUs t = mode == 1 ? ctx.time : ctx.timelineTime;
    QString str;
    if (mode == 2)
        str = QString::number(ctx.fps.frameAt(ctx.timelineTime));
    else
        str = formatTimecode(t, ctx.fps);
    const QString prefix = p.text(QStringLiteral("prefix"));
    if (!prefix.isEmpty())
        str = prefix + QLatin1Char(' ') + str;
    const TextStyle st = styleFrom(p, layer, ctx.scale);
    const TextBlock block = layoutText(str, st, -1, false);
    paintTextBlock(painter, layer, block, placeBlock(block, st, layer, p.num(QStringLiteral("x")), p.num(QStringLiteral("y"))), st, 1.0);
}

std::vector<GeneratorDef> buildGenerators()
{
    std::vector<GeneratorDef> list;
    GeneratorDef d;

    d = {};
    d.id = QStringLiteral("text");
    d.name = QStringLiteral("Titles & Text");
    d.category = QStringLiteral("Text");
    d.description = QStringLiteral("Styled text with outline, shadow, background box and in/out animations. "
                                   "Every number can be keyframed.");
    d.params = {text("text", "Text", QStringLiteral("Your Title"), true)};
    for (auto& prm : textStyleParams(10, 1, 0.5, 0.5))
        d.params.push_back(prm);
    d.params.push_back(choice("animIn", "Animate in", kInAnimations, 1));
    d.params.push_back(num("inDuration", "In duration", 0, 10, 0.5, 2, " s"));
    d.params.push_back(choice("animOut", "Animate out", kOutAnimations, 1));
    d.params.push_back(num("outDuration", "Out duration", 0, 10, 0.5, 2, " s"));
    d.render = [](QImage& image, const Params& p, const GeneratorContext& c) { QPainter q(&image); paintTitle(q, image.size(), p, c); };
    d.paint = [](QPainter& q, QSize size, const Params& p, const GeneratorContext& c) { paintTitle(q, size, p, c); return true; };
    d.isAnimating = titleAnimating;
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("credits");
    d.name = QStringLiteral("Credits Roll");
    d.category = QStringLiteral("Text");
    d.description = QStringLiteral("Text that scrolls up through the frame over the event's length (make the event "
                                   "longer to scroll slower). Lines starting with \"# \" are headings.");
    d.params = {text("text", "Text", QStringLiteral("# Directed by\nYour Name\n\n# Starring\nSomeone Famous\nSomeone Else\n\n"
                                                    "# Music\nThe Band\n\n# Thanks for watching"),
                     true)};
    for (auto& prm : textStyleParams(4.5, 1, 0.5, 0.5))
        if (prm.id != QLatin1String("y"))
            d.params.push_back(prm);
    d.render = [](QImage& image, const Params& p, const GeneratorContext& c) { QPainter q(&image); paintCredits(q, image.size(), p, c); };
    d.paint = [](QPainter& q, QSize size, const Params& p, const GeneratorContext& c) { paintCredits(q, size, p, c); return true; };
    d.isAnimating = [](const Params&, const GeneratorContext&) { return true; };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("timecode");
    d.name = QStringLiteral("Timecode");
    d.category = QStringLiteral("Text");
    d.description = QStringLiteral("Burns the current timecode or frame number into the picture.");
    d.params = {choice("mode", "Show", {QStringLiteral("Timeline time"), QStringLiteral("Event time"), QStringLiteral("Frame number")}),
                text("prefix", "Prefix", QString(), false)};
    for (auto& prm : textStyleParams(5, 1, 0.5, 0.9)) {
        if (prm.id == QLatin1String("box"))
            prm.def = 0.6;
        if (prm.id == QLatin1String("font"))
            prm.defText = QStringLiteral("Consolas");
        if (prm.id == QLatin1String("shadow"))
            prm.def = 0;
        d.params.push_back(prm);
    }
    d.render = [](QImage& image, const Params& p, const GeneratorContext& c) { QPainter q(&image); paintTimecode(q, image.size(), p, c); };
    d.paint = [](QPainter& q, QSize size, const Params& p, const GeneratorContext& c) { paintTimecode(q, size, p, c); return true; };
    d.isAnimating = [](const Params&, const GeneratorContext&) { return true; };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("solid");
    d.name = QStringLiteral("Solid Color");
    d.category = QStringLiteral("Backgrounds");
    d.description = QStringLiteral("Fills the frame with one colour.");
    d.params = {color("color", "Color", 0x000000)};
    d.render = [](QImage& layer, const Params& p, const GeneratorContext&) { layer.fill(p.color(QStringLiteral("color"))); };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("gradient");
    d.name = QStringLiteral("Color Gradient");
    d.category = QStringLiteral("Backgrounds");
    d.description = QStringLiteral("Linear or radial blend between two colours.");
    d.params = {choice("shape", "Shape", {QStringLiteral("Linear"), QStringLiteral("Radial")}), color("color1", "Color 1", 0x1e3c72),
                color("color2", "Color 2", 0x2a5298), num("angle", "Angle", -180, 180, 90, 0, "°"),
                num("centerX", "Center X", 0, 1, 0.5), num("centerY", "Center Y", 0, 1, 0.5)};
    d.render = [](QImage& layer, const Params& p, const GeneratorContext&) {
        const double w = layer.width(), h = layer.height();
        QPainter painter(&layer);
        if (p.choice(QStringLiteral("shape")) == 1) {
            const QPointF c(p.num(QStringLiteral("centerX")) * w, p.num(QStringLiteral("centerY")) * h);
            QRadialGradient g(c, std::hypot(std::max(c.x(), w - c.x()), std::max(c.y(), h - c.y())));
            g.setColorAt(0, p.color(QStringLiteral("color1")));
            g.setColorAt(1, p.color(QStringLiteral("color2")));
            painter.fillRect(layer.rect(), g);
        } else {
            const double a = p.num(QStringLiteral("angle")) * 3.14159265 / 180.0;
            const QPointF dir(std::cos(a), std::sin(a));
            const double half = (std::abs(dir.x()) * w + std::abs(dir.y()) * h) / 2;
            const QPointF c(w / 2, h / 2);
            QLinearGradient g(c - dir * half, c + dir * half);
            g.setColorAt(0, p.color(QStringLiteral("color1")));
            g.setColorAt(1, p.color(QStringLiteral("color2")));
            painter.fillRect(layer.rect(), g);
        }
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("checkerboard");
    d.name = QStringLiteral("Checkerboard");
    d.category = QStringLiteral("Backgrounds");
    d.description = QStringLiteral("Alternating squares, handy as a test pattern.");
    d.params = {num("size", "Square size", 2, 400, 60, 0, " px"), color("color1", "Color 1", 0xffffff), color("color2", "Color 2", 0x000000)};
    d.render = [](QImage& layer, const Params& p, const GeneratorContext& ctx) {
        const double size = std::max(1.0, p.num(QStringLiteral("size")) * ctx.scale);
        layer.fill(p.color(QStringLiteral("color1")));
        QPainter painter(&layer);
        const QColor c2 = p.color(QStringLiteral("color2"));
        for (int row = 0; row * size < layer.height(); ++row)
            for (int col = row % 2; col * size < layer.width(); col += 2)
                painter.fillRect(QRectF(col * size, row * size, size, size), c2);
    };
    list.push_back(d);
    addAnimatedGenerators(list);
    graphics::addGenerators(list);
    return list;
}

std::vector<GeneratorPreset> buildPresets()
{
    std::vector<GeneratorPreset> list;
    GeneratorPreset p;

    p = {};
    p.id = QStringLiteral("title");
    p.name = QStringLiteral("Title (centered)");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("title_subtitle");
    p.name = QStringLiteral("Title over subtitle");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    p.texts = {{QStringLiteral("text"), QStringLiteral("Big Title\nA smaller line underneath")}};
    p.numbers = {{QStringLiteral("size"), 7}, {QStringLiteral("lineSpacing"), 1.2}};
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("lower_third");
    p.name = QStringLiteral("Lower third (name + role)");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    p.texts = {{QStringLiteral("text"), QStringLiteral("Firstname Lastname\nRole or description")}};
    p.numbers = {{QStringLiteral("size"), 5}, {QStringLiteral("align"), 0}, {QStringLiteral("x"), 0.07}, {QStringLiteral("y"), 0.83},
                 {QStringLiteral("box"), 0.55}, {QStringLiteral("boxPadding"), 22}, {QStringLiteral("shadow"), 0},
                 {QStringLiteral("animIn"), 2}, {QStringLiteral("animOut"), 1}};
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("subtitle");
    p.name = QStringLiteral("Subtitle / caption");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    p.texts = {{QStringLiteral("text"), QStringLiteral("This is what they are saying.")}};
    p.numbers = {{QStringLiteral("size"), 4.5}, {QStringLiteral("bold"), 0}, {QStringLiteral("y"), 0.9},
                 {QStringLiteral("box"), 0.5}, {QStringLiteral("boxPadding"), 12}, {QStringLiteral("shadow"), 0},
                 {QStringLiteral("inDuration"), 0.15}, {QStringLiteral("outDuration"), 0.15}};
    p.length = 3 * kUsPerSecond;
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("typewriter");
    p.name = QStringLiteral("Typewriter");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    p.texts = {{QStringLiteral("text"), QStringLiteral("Once upon a time...")}, {QStringLiteral("font"), QStringLiteral("Courier New")}};
    p.numbers = {{QStringLiteral("size"), 7}, {QStringLiteral("animIn"), 7}, {QStringLiteral("inDuration"), 2.0}};
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("zoom_title");
    p.name = QStringLiteral("Big zoom title");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("text");
    p.texts = {{QStringLiteral("text"), QStringLiteral("EPIC")}, {QStringLiteral("font"), QStringLiteral("Impact")}};
    p.numbers = {{QStringLiteral("size"), 22}, {QStringLiteral("outline"), 6}, {QStringLiteral("animIn"), 6},
                 {QStringLiteral("inDuration"), 0.6}, {QStringLiteral("animOut"), 6}};
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("credits");
    p.name = QStringLiteral("Credits roll");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("credits");
    p.length = 15 * kUsPerSecond;
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("timecode");
    p.name = QStringLiteral("Timecode burn-in");
    p.category = QStringLiteral("Text");
    p.generator = QStringLiteral("timecode");
    p.length = 30 * kUsPerSecond;
    list.push_back(p);

    for (auto [id, name, rgb] : {std::tuple{"solid_black", "Solid black", 0x000000}, std::tuple{"solid_white", "Solid white", 0xffffff},
                                 std::tuple{"solid_blue", "Solid color (blue)", 0x1e5aa8}}) {
        p = {};
        p.id = QString::fromLatin1(id);
        p.name = QString::fromLatin1(name);
        p.category = QStringLiteral("Backgrounds");
        p.generator = QStringLiteral("solid");
        p.numbers = {{QStringLiteral("color"), double(rgb)}};
        list.push_back(p);
    }

    p = {};
    p.id = QStringLiteral("gradient");
    p.name = QStringLiteral("Gradient (blue)");
    p.category = QStringLiteral("Backgrounds");
    p.generator = QStringLiteral("gradient");
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("vignette_gradient");
    p.name = QStringLiteral("Radial spotlight");
    p.category = QStringLiteral("Backgrounds");
    p.generator = QStringLiteral("gradient");
    p.numbers = {{QStringLiteral("shape"), 1}, {QStringLiteral("color1"), double(0x444444)}, {QStringLiteral("color2"), 0.0}};
    list.push_back(p);

    p = {};
    p.id = QStringLiteral("checkerboard");
    p.name = QStringLiteral("Checkerboard");
    p.category = QStringLiteral("Backgrounds");
    p.generator = QStringLiteral("checkerboard");
    list.push_back(p);
    addAnimatedGeneratorPresets(list);
    graphics::addPresets(list);
    return list;
}

} // namespace

const std::vector<GeneratorDef>& generators()
{
    static const std::vector<GeneratorDef> list = buildGenerators();
    return list;
}

const GeneratorDef* findGenerator(const QString& id)
{
    for (const auto& g : generators())
        if (g.id == id)
            return &g;
    return nullptr;
}

const std::vector<GeneratorPreset>& generatorPresets()
{
    static const std::vector<GeneratorPreset> list = buildPresets();
    return list;
}

const GeneratorPreset* findGeneratorPreset(const QString& id)
{
    for (const auto& p : generatorPresets())
        if (p.id == id)
            return &p;
    return nullptr;
}

GeneratorSpec makeGenerator(const GeneratorPreset& preset)
{
    GeneratorSpec g;
    const GeneratorDef* def = findGenerator(preset.generator);
    if (!def)
        return g;
    g.type = def->id;
    g.params = defaultParams(def->params);
    for (const auto& [id, v] : preset.numbers)
        g.params[id].value = v;
    for (const auto& [id, v] : preset.texts)
        g.params[id].text = v;
    if(graphics::isScene(g.type)){auto it=preset.texts.find("preset");g.data=graphics::scene(it==preset.texts.end()?"broadcast":it->second);}
    return g;
}

QString generatorLabel(const GeneratorSpec& spec)
{
    const GeneratorDef* def = findGenerator(spec.type);
    if (!def)
        return QStringLiteral("Generated media");
    auto it = spec.params.find(QStringLiteral("text"));
    if (it != spec.params.end() && !it->second.text.isEmpty()) {
        QString first = it->second.text.section(QLatin1Char('\n'), 0, 0);
        if (first.startsWith(QLatin1String("# ")))
            first = first.mid(2);
        return QStringLiteral("%1: %2").arg(def->name, first);
    }
    return def->name;
}

QImage renderGenerator(const GeneratorSpec& spec, QSize size, double scale, TimeUs t, TimeUs length, FrameRate fps)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    const GeneratorDef* def = findGenerator(spec.type);
    if (!def || size.isEmpty())
        return img;
    GeneratorContext ctx;
    ctx.time = t;
    ctx.length = length;
    ctx.timelineTime = t;
    ctx.scale = scale;
    ctx.fps = fps;
    ctx.data = &spec.data;
    def->render(img, resolveParams(def->params, spec.params, t), ctx);
    return img;
}

QJsonObject generatorToJson(const GeneratorSpec& g)
{
    QJsonObject o;
    o[QStringLiteral("type")] = g.type;
    o[QStringLiteral("params")] = paramsToJson(g.params);
    if(!g.data.isEmpty())o["data"]=g.data;
    return o;
}

GeneratorSpec generatorFromJson(const QJsonObject& o)
{
    GeneratorSpec g;
    g.type = o[QStringLiteral("type")].toString();
    g.params = paramsFromJson(o[QStringLiteral("params")].toObject());
    g.data = o["data"].toObject();
    return g;
}
