// The Effect Builder's graphs (core/NodeGraph.h): every node runs, nodes put a picture back together, wiring refuses
// loops, a graph survives saving, and a saved effect renders on an event as it did in the builder, its shown
// settings and all, also on a computer that never had its file.
#include "core/Compositor.h"
#include "core/FFmpeg.h"
#include "core/NodeGraph.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QPainter>
#include <QTemporaryDir>
#include <QTextStream>
#include <cmath>

namespace {
// A picture with something in it: a gradient, a few shapes, a see-through corner.
QImage testPicture(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    QLinearGradient g(0, 0, size.width(), size.height());
    g.setColorAt(0, QColor(220, 60, 40));
    g.setColorAt(1, QColor(30, 90, 220));
    p.fillRect(img.rect(), g);
    p.setBrush(QColor(250, 230, 60));
    p.drawEllipse(QRectF(size.width() * 0.3, size.height() * 0.25, size.width() * 0.3, size.height() * 0.5));
    p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.fillRect(QRect(0, 0, size.width() / 8, size.height() / 8), Qt::transparent);
    return img;
}

double difference(const QImage& a0, const QImage& b0)
{
    const QImage a = a0.convertToFormat(QImage::Format_ARGB32_Premultiplied), b = b0.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (a.size() != b.size())
        return 255;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y) {
        const auto* ra = reinterpret_cast<const uint32_t*>(a.constScanLine(y));
        const auto* rb = reinterpret_cast<const uint32_t*>(b.constScanLine(y));
        for (int x = 0; x < a.width(); ++x)
            for (int s = 0; s < 32; s += 8)
                sum += std::abs(int((ra[x] >> s) & 0xff) - int((rb[x] >> s) & 0xff));
    }
    return sum / (double(a.width()) * a.height() * 4);
}
} // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv); // (text nodes draw text)
    ff::initialize();
    int failures = 0;
    auto check = [&](bool ok, const QString& message) {
        QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << message << Qt::endl;
        failures += !ok;
    };
    const QSize size(192, 108);
    const QImage picture = testPicture(size);
    RenderContext ctx;
    ctx.time = kUsPerSecond;
    ctx.timelineTime = kUsPerSecond;
    ctx.scale = 0.1;
    ctx.contentRect = QRectF(QPointF(0, 0), QSizeF(size));

    {
        // Every node, wired between Source and Output (each input from the Source), runs and gives a picture.
        int effects = 0, sources = 0, broken = 0;
        QStringList bad;
        for (const nodes::NodeDef& d : nodes::nodeDefs()) {
            effects += d.id.startsWith(QLatin1String("fx."));
            sources += d.id.startsWith(QLatin1String("gen."));
            if (d.id == QLatin1String("source") || d.id == QLatin1String("output") || d.id == QLatin1String("fx.remove_bg"))
                continue; // (Remove Background needs its AI model)
            nodes::Graph g = nodes::starter();
            const int n = g.addNode(d.id, QPointF());
            for (int i = 0; i < int(d.inputs.size()); ++i)
                g.connect(g.nodes[0].id, n, i);
            g.connect(n, g.nodes[1].id, 0);
            nodes::Evaluator eval;
            const QImage out = eval.run(g, picture, ctx);
            if (out.size() != size || !eval.error.isEmpty()) {
                ++broken;
                bad << d.id;
            }
        }
        check(effects > 150 && sources > 25 && nodes::nodeDefs().size() > 200 && broken == 0,
              QStringLiteral("all %1 nodes run (%2 effects, %3 sources) %4").arg(nodes::nodeDefs().size()).arg(effects).arg(sources).arg(bad.join(QStringLiteral(", "))));
    }
    {
        // The example effects that come with the program (data/effects): every one reads, has only nodes this build has,
        // and runs without an error.
        QStringList bad;
        int count = 0;
        for (const QFileInfo& fi : QDir(QStringLiteral("data/effects")).entryInfoList({QStringLiteral("*.mvpfx")}, QDir::Files)) {
            ++count;
            nodes::Graph g;
            QString err;
            nodes::Evaluator eval;
            const bool read = nodes::load(fi.absoluteFilePath(), &g, &err);
            const QImage out = read ? eval.run(g, picture, ctx) : QImage();
            const bool known = std::all_of(g.nodes.begin(), g.nodes.end(), [](const nodes::Node& n) { return nodes::findNode(n.type); });
            if (!read || !known || !eval.error.isEmpty() || out.size() != size || g.name.isEmpty())
                bad << fi.fileName();
        }
        check(count >= 10 && bad.isEmpty(), QStringLiteral("the %1 example effects read and run %2").arg(count).arg(bad.join(QStringLiteral(", "))));
    }
    {
        // A text source late in a long event still shows its text (a source is as long as any event it's on).
        nodes::Graph g = nodes::starter();
        const int title = g.addNode(QStringLiteral("gen.title"), {});
        g.connect(title, g.nodes[1].id, 0);
        RenderContext late = ctx;
        late.time = 40 * kUsPerSecond;
        const QImage out = nodes::Evaluator().run(g, picture, late);
        int drawn = 0;
        for (int y = 0; y < out.height(); ++y)
            for (int x = 0; x < out.width(); ++x)
                drawn += qAlpha(out.pixel(x, y)) > 128;
        check(drawn > 50, QStringLiteral("a text source 40 s into an event still draws its text (%1 pixels)").arg(drawn));
    }
    {
        // Source -> Invert -> Output is the Invert effect; the three channels split and combined again are the picture.
        nodes::Graph g = nodes::starter();
        const int src = g.nodes[0].id, out = g.nodes[1].id;
        const int inv = g.addNode(QStringLiteral("fx.invert"), {});
        g.connect(src, inv, 0);
        g.connect(inv, out, 0);
        nodes::Evaluator eval;
        QImage direct = picture.copy();
        std::unique_ptr<EffectState> none;
        const EffectDef* invert = findEffect(QStringLiteral("invert"));
        invert->apply(direct, resolveParams(invert->params, defaultParams(invert->params), 0), ctx, none);
        const double viaGraph = difference(eval.run(g, picture, ctx), direct);

        nodes::Graph split = nodes::starter();
        const int s2 = split.nodes[0].id, o2 = split.nodes[1].id;
        const int comb = split.addNode(QStringLiteral("combine"), {});
        for (int c = 0; c < 3; ++c) {
            const int ch = split.addNode(QStringLiteral("channel"), {});
            split.find(ch)->params[QStringLiteral("channel")].value = c;
            split.connect(s2, ch, 0);
            split.connect(ch, comb, c);
        }
        split.connect(comb, o2, 0);
        QImage opaque = picture.copy();
        opaque.fill(Qt::black);
        QPainter(&opaque).drawImage(0, 0, picture);
        const double rebuilt = difference(nodes::Evaluator().run(split, opaque, ctx), opaque);
        check(viaGraph < 0.01 && rebuilt < 0.6, QStringLiteral("a graph of Invert is the Invert effect (%1); red, green and blue split and combined give "
                                                               "the picture back (%2)")
                                                    .arg(viaGraph, 0, 'f', 3)
                                                    .arg(rebuilt, 0, 'f', 3));
    }
    {
        // Wiring: no loops; a new wire into an input replaces the old; saved and read back, the same graph.
        nodes::Graph g = nodes::starter();
        const int a = g.addNode(QStringLiteral("fx.gaussian_blur"), {}), b = g.addNode(QStringLiteral("fx.glow"), {});
        g.connect(g.nodes[0].id, a, 0);
        g.connect(a, b, 0);
        const bool loop = g.connect(b, a, 0);
        const bool self = g.connect(a, a, 0);
        g.connect(b, g.nodes[1].id, 0);
        g.connect(a, g.nodes[1].id, 0); // (replaces b -> output)
        g.exposed.push_back({QStringLiteral("n2_radius"), QStringLiteral("Glow"), b, QStringLiteral("radius")});
        g.find(a)->params[QStringLiteral("horizontal")].value = 7;
        const nodes::Graph back = nodes::fromJson(QJsonDocument::fromJson(QJsonDocument(nodes::toJson(g)).toJson()).object());
        const int intoOutput = int(std::count_if(g.links.begin(), g.links.end(), [&](const nodes::Link& l) { return l.to == g.nodes[1].id; }));
        check(!loop && !self && intoOutput == 1 && back.nodes.size() == g.nodes.size() && back.links == g.links && back.exposed.size() == 1
                  && back.find(a)->params.at(QStringLiteral("horizontal")).value == 7 && back.nextId == g.nextId,
              QStringLiteral("wiring refuses loops, one wire an input, and a graph reads back as it was saved"));
    }
    {
        // A saved effect on an event renders as in the builder; its shown setting changes the picture (and keyframes).
        nodes::Graph g = nodes::starter();
        g.name = QStringLiteral("Selftest Posterized Glow");
        const int post = g.addNode(QStringLiteral("fx.posterize"), {}), blend = g.addNode(QStringLiteral("blend"), {});
        g.connect(g.nodes[0].id, post, 0);
        g.connect(g.nodes[0].id, blend, 0);
        g.connect(post, blend, 1);
        g.connect(blend, g.nodes[1].id, 0);
        g.find(blend)->params[QStringLiteral("opacity")].value = 0.8;
        g.exposed.push_back({QStringLiteral("mix"), QStringLiteral("Mix"), blend, QStringLiteral("opacity")});
        QTemporaryDir dir;
        QString err;
        const bool saved = nodes::save(g, dir.filePath(QStringLiteral("glow.mvpfx")), &err);
        nodes::Graph loaded;
        const bool read = nodes::load(dir.filePath(QStringLiteral("glow.mvpfx")), &loaded, &err);
        const EffectDef* def = nodes::registerEffect(loaded);
        const bool listed = def && findEffect(def->id) == def && def->category == QLatin1String("User Created")
                            && std::any_of(def->params.begin(), def->params.end(), [](const ParamDef& p) { return p.id == QLatin1String("mix"); });
        // On a solid-colour event (a generated picture) through the compositor, against the graph run on that picture.
        ProjectSnapshot s;
        s.settings.width = 192;
        s.settings.height = 108;
        s.timeline = defaultTimeline();
        const int id = ops::insertGenerated(s.timeline, *findGeneratorPreset(QStringLiteral("gradient")), 0, 0, FrameRate{30, 1});
        Clip* c = s.timeline.findClip(id);
        Compositor cpu;
        const QImage plain = cpu.render(s, kUsPerSecond, size);
        c->effects.push_back(makeEffect(*def, s.timeline.allocId()));
        const QImage withFx = cpu.render(s, kUsPerSecond, size);
        RenderContext rc = ctx;
        rc.scale = 1.0;
        const double same = difference(withFx, nodes::Evaluator().run(loaded, plain, rc));
        c->effects.back().params[QStringLiteral("mix")].value = 0.0; // (Mix at 0: the picture as it was)
        Compositor cpu2;
        const double off = difference(cpu2.render(s, kUsPerSecond, size), plain);
        check(saved && read && listed && same < 0.5 && difference(withFx, plain) > 3 && off < 0.5,
              QStringLiteral("a saved effect is in User Created and renders on an event as in the builder (%1 off); its shown "
                             "setting at 0 leaves the picture (%2)")
                  .arg(same, 0, 'f', 2)
                  .arg(off, 0, 'f', 2));

        // Another computer: a project with an effect it never had, carrying its graph.
        EffectInstance carried = makeEffect(*def, 5);
        carried.type = QStringLiteral("user.from_elsewhere_selftest");
        const EffectInstance back = effectFromJson(effectToJson(carried));
        const EffectDef* there = findEffect(back.type);
        check(there && there->name == loaded.name, QStringLiteral("an effect from another computer's project registers from the graph it carries"));
    }
    QTextStream(stdout) << (failures ? "FAILED" : "ALL PASSED") << Qt::endl;
    return failures ? 1 : 0;
}
