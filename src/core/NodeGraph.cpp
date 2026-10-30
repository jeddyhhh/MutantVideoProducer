#include "core/NodeGraph.h"
#include "core/Brand.h"
#include "core/EffectHelpers.h"
#include "core/Generators.h"
#include "core/ImageOps.h"
#include "core/Parallel.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <mutex>
#include <set>

namespace nodes {
namespace {
using Inputs = std::vector<const QImage*>;
using fx::choice;
using fx::flag;
using fx::num;

QImage transparent(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

QImage copyOf(const QImage* in, QSize size)
{
    return in && in->size() == size ? in->copy() : transparent(size);
}

inline int luma(uint32_t p) { return (54 * int((p >> 16) & 0xff) + 183 * int((p >> 8) & 0xff) + 19 * int(p & 0xff)) >> 8; }

// Runs fn(x, y, pixel) -> pixel over a picture, in rows on several threads.
void perPixel(QImage& img, const std::function<uint32_t(int, int, uint32_t)>& fn)
{
    const int w = img.width();
    img.bits(); // (detach once, before the threads)
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(img.scanLine(y));
            for (int x = 0; x < w; ++x)
                row[x] = fn(x, y, row[x]);
        }
    });
}

uint32_t pixelAt(const QImage* img, int x, int y)
{
    if (!img || img->isNull())
        return 0;
    x = std::clamp(x, 0, img->width() - 1);
    y = std::clamp(y, 0, img->height() - 1);
    return reinterpret_cast<const uint32_t*>(img->constScanLine(y))[x];
}

uint32_t bilinear(const QImage& img, float x, float y)
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

inline uint32_t grey(int v, int a) { return (uint32_t(a) << 24) | (uint32_t(v) << 16) | (uint32_t(v) << 8) | uint32_t(v); }

// Keeps the last frames a Delay node saw.
struct DelayState : EffectState {
    std::deque<QImage> frames;
};

// The nodes of their own (the rest are the effects and generators).
std::vector<NodeDef> nativeNodes()
{
    std::vector<NodeDef> list;
    NodeDef d;

    d = {};
    d.id = QStringLiteral("source");
    d.name = QStringLiteral("Source");
    d.category = QStringLiteral("Input and Output");
    d.description = QStringLiteral("The picture the effect is on: the event, the track, or everything below an adjustment layer.");
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("output");
    d.name = QStringLiteral("Output");
    d.category = QStringLiteral("Input and Output");
    d.description = QStringLiteral("What the effect shows. Nothing wired in: the source, unchanged.");
    d.inputs = {QStringLiteral("Image")};
    d.output = false;
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("blend");
    d.name = QStringLiteral("Blend");
    d.category = QStringLiteral("Combine");
    d.description = QStringLiteral("Lays the Layer over the Base with a blend mode (Normal, Add, Screen, Multiply, Overlay...) at an opacity.");
    d.inputs = {QStringLiteral("Base"), QStringLiteral("Layer")};
    d.params = {choice("mode", "Mode", blendModeNames()), num("opacity", "Opacity", 0, 1, 1)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        if (in[1])
            imageops::blendLayer(out, *in[1], float(p.num(QStringLiteral("opacity"))), p.choice(QStringLiteral("mode")));
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("mix");
    d.name = QStringLiteral("Mix");
    d.category = QStringLiteral("Combine");
    d.description = QStringLiteral("Crossfades from A to B (Amount 0: all A, 1: all B).");
    d.inputs = {QStringLiteral("A"), QStringLiteral("B")};
    d.params = {num("amount", "Amount", 0, 1, 0.5)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        const QImage a = copyOf(in[0], c.size), b = copyOf(in[1], c.size);
        QImage out(c.size, QImage::Format_ARGB32_Premultiplied);
        imageops::mix(out, &a, &b, float(p.num(QStringLiteral("amount"))));
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("math");
    d.name = QStringLiteral("Math");
    d.category = QStringLiteral("Combine");
    d.description = QStringLiteral("A and B combined channel by channel: add, subtract, multiply, divide, the smaller or larger, the difference or the average.");
    d.inputs = {QStringLiteral("A"), QStringLiteral("B")};
    d.params = {choice("op", "Operation", {QStringLiteral("Add"), QStringLiteral("Subtract"), QStringLiteral("Multiply"), QStringLiteral("Divide"),
                                           QStringLiteral("Minimum"), QStringLiteral("Maximum"), QStringLiteral("Difference"), QStringLiteral("Average")}),
                num("gain", "Gain", 0, 4, 1)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        const QImage* b = in[1];
        const int op = p.choice(QStringLiteral("op"));
        const float gain = float(p.num(QStringLiteral("gain")));
        perPixel(out, [&](int x, int y, uint32_t pa) {
            const uint32_t pb = pixelAt(b, x, y);
            uint32_t r = 0;
            for (int s = 0; s < 32; s += 8) {
                const float va = float((pa >> s) & 0xff) / 255.f, vb = float((pb >> s) & 0xff) / 255.f;
                float v = 0;
                if (s == 24) {
                    v = std::max(va, vb); // (alpha: what either has)
                } else {
                    switch (op) {
                    case 0: v = va + vb; break;
                    case 1: v = va - vb; break;
                    case 2: v = va * vb; break;
                    case 3: v = vb > 0.004f ? va / vb : 1.f; break;
                    case 4: v = std::min(va, vb); break;
                    case 5: v = std::max(va, vb); break;
                    case 6: v = std::abs(va - vb); break;
                    default: v = 0.5f * (va + vb); break;
                    }
                    v *= gain;
                }
                r |= uint32_t(std::clamp(int(v * 255.f + 0.5f), 0, 255)) << s;
            }
            // (keep the colour within the alpha, as premultiplied pictures must)
            const int a = int(r >> 24);
            for (int s = 0; s < 24; s += 8)
                if (int((r >> s) & 0xff) > a)
                    r = (r & ~(0xffu << s)) | (uint32_t(a) << s);
            return r;
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("mask");
    d.name = QStringLiteral("Mask");
    d.category = QStringLiteral("Keys and Masks");
    d.description = QStringLiteral("Keeps the Image where the Matte is drawn (alpha) or bright (luma), or the other way round.");
    d.inputs = {QStringLiteral("Image"), QStringLiteral("Matte")};
    d.params = {choice("mode", "Use", {QStringLiteral("Alpha"), QStringLiteral("Inverted alpha"), QStringLiteral("Luma"), QStringLiteral("Inverted luma")}, 2)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        imageops::applyMatte(out, copyOf(in[1], c.size), p.choice(QStringLiteral("mode")) + 1);
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("set_alpha");
    d.name = QStringLiteral("Set Alpha");
    d.category = QStringLiteral("Keys and Masks");
    d.description = QStringLiteral("The Image's transparency replaced by the Alpha input's brightness (white: solid, black: see-through).");
    d.inputs = {QStringLiteral("Image"), QStringLiteral("Alpha")};
    d.params = {flag("invert", "Invert", false)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size).convertToFormat(QImage::Format_ARGB32);
        const bool invert = p.flag(QStringLiteral("invert"));
        const QImage* m = in[1];
        perPixel(out, [&](int x, int y, uint32_t px) {
            int a = m ? luma(pixelAt(m, x, y)) : 255;
            if (invert)
                a = 255 - a;
            return (px & 0xffffff) | (uint32_t(a) << 24);
        });
        return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("luma_key");
    d.name = QStringLiteral("Luma Key");
    d.category = QStringLiteral("Keys and Masks");
    d.description = QStringLiteral("Makes the dark parts see-through (Low to High fades from clear to solid), or the bright parts with Invert.");
    d.inputs = {QStringLiteral("Image")};
    d.params = {num("low", "Low", 0, 1, 0.1), num("high", "High", 0, 1, 0.4), flag("invert", "Invert", false)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        const float lo = float(p.num(QStringLiteral("low"))), hi = std::max(lo + 0.001f, float(p.num(QStringLiteral("high"))));
        const bool invert = p.flag(QStringLiteral("invert"));
        perPixel(out, [&](int, int, uint32_t px) {
            const int a = int(px >> 24);
            const float l = a ? float(luma(px)) / float(a) : 0.f; // (of the colour, not the premultiplied value)
            float k = std::clamp((l - lo) / (hi - lo), 0.f, 1.f);
            k = k * k * (3 - 2 * k);
            if (invert)
                k = 1 - k;
            uint32_t r = 0;
            for (int s = 0; s < 32; s += 8)
                r |= uint32_t(float((px >> s) & 0xff) * k + 0.5f) << s;
            return r;
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("difference_key");
    d.name = QStringLiteral("Difference Key");
    d.category = QStringLiteral("Keys and Masks");
    d.description = QStringLiteral("Keeps what differs from a Clean Plate (the same view without the subject): what matches it goes see-through.");
    d.inputs = {QStringLiteral("Image"), QStringLiteral("Clean plate")};
    d.params = {num("threshold", "Threshold", 0, 1, 0.12), num("softness", "Softness", 0, 1, 0.08)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        const QImage* plate = in[1];
        const float t = float(p.num(QStringLiteral("threshold"))), soft = std::max(0.001f, float(p.num(QStringLiteral("softness"))));
        perPixel(out, [&](int x, int y, uint32_t px) {
            const uint32_t q = pixelAt(plate, x, y);
            float d = 0;
            for (int s = 0; s < 24; s += 8)
                d = std::max(d, std::abs(float((px >> s) & 0xff) - float((q >> s) & 0xff)) / 255.f);
            const float k = plate ? std::clamp((d - t) / soft, 0.f, 1.f) : 1.f;
            uint32_t r = 0;
            for (int s = 0; s < 32; s += 8)
                r |= uint32_t(float((px >> s) & 0xff) * k + 0.5f) << s;
            return r;
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("channel");
    d.name = QStringLiteral("Channel");
    d.category = QStringLiteral("Channels");
    d.description = QStringLiteral("One channel of the picture as grey: red, green, blue, alpha or brightness (luma).");
    d.inputs = {QStringLiteral("Image")};
    d.params = {choice("channel", "Channel", {QStringLiteral("Red"), QStringLiteral("Green"), QStringLiteral("Blue"), QStringLiteral("Alpha"), QStringLiteral("Luma")}, 4)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        const int ch = p.choice(QStringLiteral("channel"));
        perPixel(out, [&](int, int, uint32_t px) {
            const int a = int(px >> 24);
            switch (ch) {
            case 0: return grey(int((px >> 16) & 0xff), a);
            case 1: return grey(int((px >> 8) & 0xff), a);
            case 2: return grey(int(px & 0xff), a);
            case 3: return grey(a, 255);
            default: return grey(luma(px), a);
            }
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("combine");
    d.name = QStringLiteral("Combine Channels");
    d.category = QStringLiteral("Channels");
    d.description = QStringLiteral("A picture made of the brightness of up to four pictures: one each for red, green, blue and alpha (none wired to Alpha: solid).");
    d.inputs = {QStringLiteral("Red"), QStringLiteral("Green"), QStringLiteral("Blue"), QStringLiteral("Alpha")};
    d.eval = [](const Inputs& in, const Params&, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out(c.size, QImage::Format_ARGB32);
        perPixel(out, [&](int x, int y, uint32_t) {
            const int r = in[0] ? luma(pixelAt(in[0], x, y)) : 0, g = in[1] ? luma(pixelAt(in[1], x, y)) : 0,
                      b = in[2] ? luma(pixelAt(in[2], x, y)) : 0, a = in[3] ? luma(pixelAt(in[3], x, y)) : 255;
            return (uint32_t(a) << 24) | (uint32_t(r) << 16) | (uint32_t(g) << 8) | uint32_t(b);
        });
        return out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("threshold");
    d.name = QStringLiteral("Threshold");
    d.category = QStringLiteral("Channels");
    d.description = QStringLiteral("Black where the picture is darker than Level, white where it is brighter (Softness blurs the step).");
    d.inputs = {QStringLiteral("Image")};
    d.params = {num("level", "Level", 0, 1, 0.5), num("softness", "Softness", 0, 0.5, 0.02)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = copyOf(in[0], c.size);
        const float level = float(p.num(QStringLiteral("level"))), soft = std::max(0.001f, float(p.num(QStringLiteral("softness"))));
        perPixel(out, [&](int, int, uint32_t px) {
            const int a = int(px >> 24);
            const float l = a ? float(luma(px)) / float(a) : 0.f;
            const float k = std::clamp((l - level) / soft + 0.5f, 0.f, 1.f);
            return grey(int(k * float(a) + 0.5f), a);
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("displace");
    d.name = QStringLiteral("Displace");
    d.category = QStringLiteral("Distort");
    d.description = QStringLiteral("Pushes the Image's pixels around by the Map: mid-grey stays, brighter moves one way, darker the other "
                                   "(by its brightness, or red across and green up and down).");
    d.inputs = {QStringLiteral("Image"), QStringLiteral("Map")};
    d.params = {num("amountX", "Across", -400, 400, 30, 0, " px"), num("amountY", "Up / down", -400, 400, 30, 0, " px"),
                choice("use", "Use", {QStringLiteral("Brightness"), QStringLiteral("Red across, green up / down")})};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        const QImage src = copyOf(in[0], c.size);
        QImage out(c.size, QImage::Format_ARGB32_Premultiplied);
        const QImage* map = in[1];
        const float ax = float(p.num(QStringLiteral("amountX")) * c.render.scale), ay = float(p.num(QStringLiteral("amountY")) * c.render.scale);
        const bool rg = p.choice(QStringLiteral("use")) == 1;
        perPixel(out, [&](int x, int y, uint32_t) {
            const uint32_t m = pixelAt(map, x, y);
            const float mx = rg ? float((m >> 16) & 0xff) / 255.f : float(luma(m)) / 255.f;
            const float my = rg ? float((m >> 8) & 0xff) / 255.f : mx;
            return map ? bilinear(src, float(x) + (mx - 0.5f) * 2 * ax, float(y) + (my - 0.5f) * 2 * ay) : pixelAt(&src, x, y);
        });
        return out;
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("delay");
    d.name = QStringLiteral("Delay");
    d.category = QStringLiteral("Time");
    d.description = QStringLiteral("The picture from a number of frames before (as it plays). Blend it with the present for echoes.");
    d.inputs = {QStringLiteral("Image")};
    d.params = {num("frames", "Frames", 1, 120, 6, 0)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>& state) {
        auto* s = dynamic_cast<DelayState*>(state.get());
        if (!s) {
            state = std::make_unique<DelayState>();
            s = static_cast<DelayState*>(state.get());
        }
        const size_t n = size_t(std::clamp(int(p.num(QStringLiteral("frames"))), 1, 120));
        s->frames.push_back(copyOf(in[0], c.size));
        while (s->frames.size() > n + 1)
            s->frames.pop_front();
        const QImage& old = s->frames.front();
        return old.size() == c.size ? old : transparent(c.size);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("place");
    d.name = QStringLiteral("Place");
    d.category = QStringLiteral("Distort");
    d.description = QStringLiteral("Moves, scales and turns the picture (picture in picture, a corner box).");
    d.inputs = {QStringLiteral("Image")};
    d.params = {num("x", "X", -1, 1, 0, 3), num("y", "Y", -1, 1, 0, 3), num("scale", "Scale", 0.01, 4, 0.5, 3), num("rotation", "Rotation", -360, 360, 0, 1, "°")};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>&) {
        QImage out = transparent(c.size);
        if (!in[0])
            return out;
        QPainter painter(&out);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(c.size.width() * (0.5 + p.num(QStringLiteral("x")) / 2), c.size.height() * (0.5 + p.num(QStringLiteral("y")) / 2));
        painter.rotate(p.num(QStringLiteral("rotation")));
        painter.scale(p.num(QStringLiteral("scale")), p.num(QStringLiteral("scale")));
        painter.drawImage(QPointF(-c.size.width() / 2.0, -c.size.height() / 2.0), *in[0]);
        return out;
    };
    list.push_back(d);
    return list;
}

std::vector<NodeDef> buildNodes()
{
    std::vector<NodeDef> list = nativeNodes();
    // Every video effect, one input to one output.
    for (const EffectDef& e : videoEffects()) {
        NodeDef d;
        d.id = QStringLiteral("fx.") + e.id;
        d.name = e.name;
        d.category = QStringLiteral("Effects/") + e.category;
        d.description = e.description;
        d.inputs = {QStringLiteral("Image")};
        d.params = e.params;
        const EffectDef* def = &e;
        d.eval = [def](const Inputs& in, const Params& p, const ParamMap&, const NodeContext& c, std::unique_ptr<EffectState>& state) {
            QImage img = copyOf(in[0], c.size);
            const fx::PostMask post = def->glitchPost ? fx::postMask(p, img.size()) : fx::PostMask{};
            const QImage before = post.active() ? img.copy() : QImage();
            RenderContext ctx = c.render;
            ctx.contentRect = QRectF(img.rect());
            def->apply(img, p, ctx, state);
            if (post.active())
                fx::applyPostMask(img, before, post);
            return img.format() == QImage::Format_ARGB32_Premultiplied ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        };
        list.push_back(std::move(d));
    }
    // The backgrounds, patterns, animations and text as sources (not the ones that need a scene or sound).
    for (const GeneratorPreset& preset : generatorPresets()) {
        const GeneratorDef* g = findGenerator(preset.generator);
        if (!g || preset.category == QLatin1String("Graphics") || preset.category == QLatin1String("Animation")
            || preset.category == QLatin1String("Adjustment"))
            continue;
        NodeDef d;
        d.id = QStringLiteral("gen.") + preset.id;
        d.name = preset.name;
        d.category = QStringLiteral("Sources/") + preset.category;
        d.description = g->description;
        for (ParamDef pd : g->params) { // (starting as the preset does)
            if (auto n = preset.numbers.find(pd.id); n != preset.numbers.end())
                pd.def = n->second;
            if (auto t = preset.texts.find(pd.id); t != preset.texts.end())
                pd.defText = t->second;
            d.params.push_back(pd);
        }
        const QString type = g->id;
        d.eval = [type](const Inputs&, const Params&, const ParamMap& raw, const NodeContext& c, std::unique_ptr<EffectState>&) {
            GeneratorSpec spec;
            spec.type = type;
            spec.params = raw;
            const FrameRate fps{std::max(1, int(std::lround(c.render.fps))), 1};
            QImage img = renderGenerator(spec, c.size, c.render.scale, c.render.time, 10 * kUsPerSecond, fps);
            if (img.size() != c.size)
                img = img.scaled(c.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            return img.format() == QImage::Format_ARGB32_Premultiplied ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        };
        list.push_back(std::move(d));
    }
    return list;
}

// ---- the registry of user effects ----
std::mutex& registryMutex()
{
    static std::mutex m;
    return m;
}
std::deque<EffectDef>& registry() // (never shrinks: renderers may hold an older version's pointer)
{
    static std::deque<EffectDef> list;
    return list;
}

struct UserState : EffectState {
    QString text;
    Graph graph;
    Evaluator eval;
};
} // namespace

// ---- Graph ----
Node* Graph::find(int id)
{
    for (Node& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

const Node* Graph::find(int id) const { return const_cast<Graph*>(this)->find(id); }

const Link* Graph::input(int to, int port) const
{
    for (const Link& l : links)
        if (l.to == to && l.port == port)
            return &l;
    return nullptr;
}

int Graph::addNode(const QString& type, QPointF pos)
{
    const NodeDef* def = findNode(type);
    if (!def)
        return 0;
    Node n;
    n.id = nextId++;
    n.type = type;
    n.params = defaultParams(def->params);
    n.pos = pos;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void Graph::removeNode(int id)
{
    std::erase_if(nodes, [&](const Node& n) { return n.id == id; });
    std::erase_if(links, [&](const Link& l) { return l.from == id || l.to == id; });
    std::erase_if(exposed, [&](const Exposed& x) { return x.node == id; });
}

bool Graph::connect(int from, int to, int port)
{
    // A loop: `from` already takes its picture (through its inputs) from `to`.
    std::set<int> seen;
    std::function<bool(int)> upstream = [&](int id) {
        if (id == to)
            return true;
        if (!seen.insert(id).second)
            return false;
        for (const Link& l : links)
            if (l.to == id && upstream(l.from))
                return true;
        return false;
    };
    if (!find(from) || !find(to) || upstream(from))
        return false;
    std::erase_if(links, [&](const Link& l) { return l.to == to && l.port == port; });
    links.push_back({from, to, port});
    return true;
}

Graph starter()
{
    Graph g;
    g.name = QStringLiteral("My Effect");
    const int in = g.addNode(QStringLiteral("source"), QPointF(0, 0));
    const int out = g.addNode(QStringLiteral("output"), QPointF(520, 0));
    g.connect(in, out, 0);
    return g;
}

QJsonObject toJson(const Graph& g)
{
    QJsonArray nodesJson, linksJson, exposedJson;
    for (const Node& n : g.nodes)
        nodesJson.append(QJsonObject{{QStringLiteral("id"), n.id},
                                     {QStringLiteral("type"), n.type},
                                     {QStringLiteral("x"), std::round(n.pos.x())},
                                     {QStringLiteral("y"), std::round(n.pos.y())},
                                     {QStringLiteral("params"), paramsToJson(n.params)}});
    for (const Link& l : g.links)
        linksJson.append(QJsonArray{l.from, l.to, l.port});
    for (const Exposed& x : g.exposed)
        exposedJson.append(QJsonObject{{QStringLiteral("id"), x.id}, {QStringLiteral("name"), x.name}, {QStringLiteral("node"), x.node}, {QStringLiteral("param"), x.param}});
    return {{QStringLiteral("format"), QStringLiteral("mvp-node-effect")},
            {QStringLiteral("version"), 1},
            {QStringLiteral("name"), g.name},
            {QStringLiteral("description"), g.description},
            {QStringLiteral("nextId"), g.nextId},
            {QStringLiteral("nodes"), nodesJson},
            {QStringLiteral("links"), linksJson},
            {QStringLiteral("exposed"), exposedJson}};
}

Graph fromJson(const QJsonObject& o)
{
    Graph g;
    g.name = o[QStringLiteral("name")].toString();
    g.description = o[QStringLiteral("description")].toString();
    for (const QJsonValue v : o[QStringLiteral("nodes")].toArray()) {
        const QJsonObject n = v.toObject();
        Node node;
        node.id = n[QStringLiteral("id")].toInt();
        node.type = n[QStringLiteral("type")].toString();
        node.pos = QPointF(n[QStringLiteral("x")].toDouble(), n[QStringLiteral("y")].toDouble());
        node.params = paramsFromJson(n[QStringLiteral("params")].toObject());
        if (node.id > 0 && !g.find(node.id))
            g.nodes.push_back(std::move(node));
    }
    for (const QJsonValue v : o[QStringLiteral("links")].toArray()) {
        const QJsonArray l = v.toArray();
        if (l.size() == 3)
            g.connect(l[0].toInt(), l[1].toInt(), l[2].toInt()); // (refuses loops and wires to nowhere)
    }
    for (const QJsonValue v : o[QStringLiteral("exposed")].toArray()) {
        const QJsonObject x = v.toObject();
        Exposed e{x[QStringLiteral("id")].toString(), x[QStringLiteral("name")].toString(), x[QStringLiteral("node")].toInt(), x[QStringLiteral("param")].toString()};
        if (!e.id.isEmpty() && e.id != QLatin1String("graph") && g.find(e.node))
            g.exposed.push_back(e);
    }
    int top = 0;
    for (const Node& n : g.nodes)
        top = std::max(top, n.id);
    g.nextId = std::max(o[QStringLiteral("nextId")].toInt(1), top + 1);
    return g;
}

const std::vector<NodeDef>& nodeDefs()
{
    static const std::vector<NodeDef> list = buildNodes();
    return list;
}

const NodeDef* findNode(const QString& id)
{
    static const std::map<QString, const NodeDef*> index = [] {
        std::map<QString, const NodeDef*> m;
        for (const NodeDef& d : nodeDefs())
            m[d.id] = &d;
        return m;
    }();
    const auto it = index.find(id);
    return it == index.end() ? nullptr : it->second;
}

// ---- Evaluator ----
QImage Evaluator::run(const Graph& g, const QImage& source0, const RenderContext& ctx, bool keepOutputs)
{
    error.clear();
    outputs.clear();
    const QImage source = source0.format() == QImage::Format_ARGB32_Premultiplied ? source0 : source0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const Node* out = nullptr;
    for (const Node& n : g.nodes)
        if (n.type == QLatin1String("output"))
            out = &n;
    if (!out) {
        error = QStringLiteral("The graph has no Output node.");
        return source;
    }
    for (auto it = m_states.begin(); it != m_states.end();) // (nodes deleted since)
        it = g.find(it->first) ? std::next(it) : m_states.erase(it);
    NodeContext nc{source.size(), ctx};
    std::map<int, QImage> done;
    std::set<int> visiting;
    std::function<QImage(int)> eval = [&](int id) -> QImage {
        if (auto it = done.find(id); it != done.end())
            return it->second;
        const Node* n = g.find(id);
        if (!n || !visiting.insert(id).second)
            return transparent(nc.size);
        const NodeDef* def = findNode(n->type);
        QImage result;
        if (!def) {
            error = QStringLiteral("This version has no \"%1\" node.").arg(n->type);
            result = transparent(nc.size);
        } else if (n->type == QLatin1String("source")) {
            result = source;
        } else {
            std::vector<QImage> ins(size_t(def->inputs.size()));
            Inputs ptrs(ins.size(), nullptr);
            for (int i = 0; i < int(ins.size()); ++i)
                if (const Link* l = g.input(id, i)) {
                    ins[size_t(i)] = eval(l->from);
                    ptrs[size_t(i)] = &ins[size_t(i)];
                }
            if (!def->output) {
                result = ptrs.empty() || !ptrs[0] ? source : ins[0];
            } else {
                State& st = m_states[id];
                if (st.type != n->type) {
                    st.state.reset();
                    st.type = n->type;
                }
                result = def->eval(ptrs, resolveParams(def->params, n->params, ctx.time), n->params, nc, st.state);
                if (result.size() != nc.size)
                    result = result.isNull() ? transparent(nc.size) : result.scaled(nc.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
        visiting.erase(id);
        done[id] = result;
        if (keepOutputs)
            outputs[id] = result;
        return result;
    };
    return eval(out->id);
}

// ---- Saved effects ----
QString userEffectsFolder()
{
    return brand::storageLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/effects");
}

bool save(const Graph& g, const QString& path, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(toJson(g)).toJson(QJsonDocument::Indented));
    return true;
}

bool load(const QString& path, Graph* g, QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o[QStringLiteral("format")].toString() != QLatin1String("mvp-node-effect")) {
        if (error)
            *error = QStringLiteral("not an effect made in the Effect Builder");
        return false;
    }
    *g = fromJson(o);
    return true;
}

QString effectId(const QString& name)
{
    static const QRegularExpression other(QStringLiteral("[^a-z0-9]+"));
    QString slug = name.toLower().replace(other, QStringLiteral("_"));
    while (slug.startsWith(QLatin1Char('_')))
        slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('_')))
        slug.chop(1);
    return QStringLiteral("user.") + (slug.isEmpty() ? QStringLiteral("effect") : slug);
}

namespace {
const EffectDef* registerAs(const Graph& g, const QString& id)
{
    EffectDef d;
    d.id = id;
    d.name = g.name.trimmed().isEmpty() ? QStringLiteral("User Effect") : g.name.trimmed();
    d.category = QStringLiteral("User Created");
    d.description = g.description.trimmed().isEmpty() ? QStringLiteral("Made in the Effect Builder (Tools > Effect Builder).") : g.description.trimmed();
    for (const Exposed& x : g.exposed) {
        const Node* n = g.find(x.node);
        const NodeDef* nd = n ? findNode(n->type) : nullptr;
        if (!nd)
            continue;
        const auto pd = std::find_if(nd->params.begin(), nd->params.end(), [&](const ParamDef& p) { return p.id == x.param; });
        if (pd == nd->params.end() || pd->type == ParamType::Text || pd->type == ParamType::Font)
            continue;
        ParamDef q = *pd;
        q.id = x.id;
        q.name = x.name;
        if (auto v = n->params.find(x.param); v != n->params.end())
            q.def = v->second.value; // (as it was set in the graph)
        d.params.push_back(q);
    }
    ParamDef graph;
    graph.id = QStringLiteral("graph");
    graph.name = QStringLiteral("Graph");
    graph.type = ParamType::Text;
    graph.hidden = true;
    graph.defText = QString::fromUtf8(QJsonDocument(toJson(g)).toJson(QJsonDocument::Compact));
    d.params.push_back(graph);
    d.apply = [](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state) {
        auto* s = dynamic_cast<UserState*>(state.get());
        if (!s) {
            state = std::make_unique<UserState>();
            s = static_cast<UserState*>(state.get());
        }
        const QString text = p.text(QStringLiteral("graph"));
        if (s->text != text) {
            s->text = text;
            s->graph = fromJson(QJsonDocument::fromJson(text.toUtf8()).object());
        }
        // The effect's settings (keyframed, perhaps) into the nodes they show.
        for (const Exposed& x : s->graph.exposed)
            if (Node* n = s->graph.find(x.node); n && p.numbers.count(x.id)) {
                ParamValue& v = n->params[x.param];
                v.keys.clear();
                v.value = p.num(x.id);
            }
        img = s->eval.run(s->graph, img, ctx);
    };
    const EffectDef* added = nullptr;
    {
        std::lock_guard lock(registryMutex());
        registry().push_back(std::move(d));
        added = &registry().back();
    }
    emit Notifier::instance().changed();
    return added;
}
} // namespace

const EffectDef* registerEffect(const Graph& g) { return registerAs(g, effectId(g.name)); }

void loadUserEffects()
{
    const QDir dir(userEffectsFolder());
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.mvpfx")}, QDir::Files, QDir::Name)) {
        Graph g;
        if (load(fi.absoluteFilePath(), &g, nullptr))
            registerEffect(g);
    }
}

std::vector<const EffectDef*> userEffects()
{
    std::lock_guard lock(registryMutex());
    std::vector<const EffectDef*> out;
    std::set<QString> seen;
    for (auto it = registry().rbegin(); it != registry().rend(); ++it)
        if (seen.insert(it->id).second)
            out.push_back(&*it);
    std::sort(out.begin(), out.end(), [](const EffectDef* a, const EffectDef* b) { return a->name.localeAwareCompare(b->name) < 0; });
    return out;
}

const EffectDef* findUserEffect(const QString& id)
{
    std::lock_guard lock(registryMutex());
    for (auto it = registry().rbegin(); it != registry().rend(); ++it)
        if (it->id == id)
            return &*it;
    return nullptr;
}

void registerFromInstance(const EffectInstance& e)
{
    if (!e.type.startsWith(QLatin1String("user.")) || findUserEffect(e.type))
        return;
    const auto it = e.params.find(QStringLiteral("graph"));
    if (it == e.params.end() || it->second.text.isEmpty())
        return;
    registerAs(fromJson(QJsonDocument::fromJson(it->second.text.toUtf8()).object()), e.type);
}

Notifier& Notifier::instance()
{
    static Notifier n;
    return n;
}

} // namespace nodes
