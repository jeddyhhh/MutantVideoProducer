// The Effect Builder's graphs (core/NodeGraph.h): every node runs, nodes put a picture back together, wiring refuses
// loops, a graph survives saving, and a saved effect renders on an event as it did in the builder, its shown
// settings and all, also on a computer that never had its file. On the GPU (when there is one) each node and each
// example draws as on the CPU, and the examples keep up with a live picture.
#include "core/Compositor.h"
#include "core/FFmpeg.h"
#include "core/FrameRenderer.h"
#include "core/GpuCompositor.h"
#include "core/NodeGraph.h"
#include "core/RetroShaders.h"
#if __has_include(<QtCore/private/qzipwriter_p.h>)
#include <QtCore/private/qzipwriter_p.h>
#else // (Qt before 6.5 keeps it in QtGui)
#include <QtGui/private/qzipwriter_p.h>
#endif
#include <QTemporaryDir>
#include <QElapsedTimer>
#include "core/Project.h"
#include "core/TimelineOps.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QGuiApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QOffscreenSurface>
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

// The example effects under a folder, its sub-folders (submenus of Examples) too.
QFileInfoList examplesIn(const QString& folder)
{
    QFileInfoList list;
    QDirIterator it(folder, {QStringLiteral("*.mvpfx")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext())
        list << QFileInfo(it.next());
    std::sort(list.begin(), list.end(), [](const QFileInfo& x, const QFileInfo& y) { return x.completeBaseName() < y.completeBaseName(); });
    return list;
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
        for (const QFileInfo& fi : examplesIn(QStringLiteral("data/effects"))) {
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
        // Where they're listed: the examples folder's own under Node Editor, a folder of them where its path says.
        nodes::loadUserEffects();
        const EffectDef* hologram = nodes::findUserEffect(nodes::effectId(QStringLiteral("Hologram")));
        const EffectDef* circuit = nodes::findUserEffect(nodes::effectId(QStringLiteral("Composite Circuit")));
        check(hologram && hologram->category == QLatin1String("Node Editor") && circuit
                  && circuit->category == QLatin1String("Broken VGS/Analogue Circuit Emu"),
              QStringLiteral("the examples are in the Node Editor menu, the analogue circuits in Broken VGS > Analogue Circuit Emu (%1; %2)")
                  .arg(hologram ? hologram->category : QString(), circuit ? circuit->category : QString()));
    }
    {
        // Composite signal: colour bars encoded and decoded by the TV come back (locked, in colour), the same frame twice
        // draws the same; sync taken off, the TV runs free (the picture moves from frame to frame); the Enhancer's wide
        // gain channel sharpens, a big RC low pass smears.
        QImage bars(320, 240, QImage::Format_ARGB32_Premultiplied);
        {
            QPainter p(&bars);
            const QColor colours[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue, Qt::black};
            for (int i = 0; i < 8; ++i)
                p.fillRect(QRect(i * 40, 0, 40, 240), colours[i]);
        }
        auto chain = [](const QStringList& types, std::vector<int>* ids = nullptr) {
            nodes::Graph g = nodes::starter();
            int prev = g.nodes[0].id;
            for (const QString& t : types) {
                const int n = g.addNode(t, {});
                g.connect(prev, n, 0);
                prev = n;
                if (ids)
                    ids->push_back(n);
            }
            g.connect(prev, g.nodes[1].id, 0);
            return g;
        };
        auto at = [&](int frame) {
            RenderContext r = ctx;
            r.fps = 30;
            r.time = r.timelineTime = frame * kUsPerSecond / 30;
            return r;
        };
        auto edges = [](const QImage& img) { // how much the picture changes from pixel to pixel along its lines
            double sum = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 1; x < img.width(); ++x)
                    sum += std::abs(qGray(img.pixel(x, y)) - qGray(img.pixel(x - 1, y)));
            return sum / (double(img.width()) * img.height());
        };
        const bool dump = qEnvironmentVariableIsSet("MVP_COMPOSITE_DUMP");
        auto save = [&](const QImage& img, const char* name) {
            if (dump)
                img.save(QDir::temp().filePath(QStringLiteral("mvp-composite-%1.png").arg(QLatin1String(name))));
        };
        nodes::Evaluator ev;
        const nodes::Graph tv = chain({QStringLiteral("composite_encode"), QStringLiteral("composite_decode")});
        ev.run(tv, bars, at(10));
        const QImage f11 = ev.run(tv, bars, at(11)), again = ev.run(tv, bars, at(11));
        save(f11, "roundtrip");
        const QRgb red = f11.pixel(5 * 40 + 20, 120), blue = f11.pixel(6 * 40 + 20, 120);
        const double off = difference(f11, bars);
        check(off < 20 && difference(f11, again) == 0 && qRed(red) > 150 && qGreen(red) < 100 && qBlue(blue) > 150 && qRed(blue) < 100,
              QStringLiteral("composite: colour bars through the signal and the TV come back (%1 off; red bar %2,%3,%4), the same frame "
                             "drawn again the same")
                  .arg(off, 0, 'f', 1).arg(qRed(red)).arg(qGreen(red)).arg(qBlue(red)));

        nodes::Evaluator played, cold; // (a TV started at frame 100, as when playing starts there, against one played up to it)
        played.run(tv, bars, at(98));
        played.run(tv, bars, at(99));
        const double settle = difference(played.run(tv, bars, at(100)), cold.run(tv, bars, at(100)));
        check(settle < 0.5, QStringLiteral("composite: the TV starts locked wherever playing starts (%1 off the one that played up to it)").arg(settle, 0, 'f', 2));

        nodes::Graph cut = nodes::starter(); // the picture's sync taken off, the Sync input given none
        const int enc = cut.addNode(QStringLiteral("composite_encode"), {}), sep = cut.addNode(QStringLiteral("composite_sync_separator"), {});
        const int mixer = cut.addNode(QStringLiteral("composite_sync_mixer"), {}), dec = cut.addNode(QStringLiteral("composite_decode"), {});
        cut.connect(cut.nodes[0].id, enc, 0);
        cut.connect(enc, sep, 0);
        cut.connect(enc, mixer, 0);
        cut.connect(sep, mixer, 1);
        cut.connect(mixer, dec, 0);
        cut.connect(dec, cut.nodes[1].id, 0);
        cut.find(sep)->params[QStringLiteral("correction")].value = 0;
        nodes::Evaluator loose;
        const QImage shapes = picture.scaled(320, 240); // (something that changes down the picture: the roll shows)
        loose.run(cut, shapes, at(20));
        const QImage r21 = loose.run(cut, shapes, at(21)), r24 = loose.run(cut, shapes, at(24));
        save(r24, "nosync");
        check(difference(r21, r24) > 8 && difference(r24, shapes) > 15,
              QStringLiteral("composite: without sync the TV runs free, the picture moving (%1 between frames)").arg(difference(r21, r24), 0, 'f', 1));

        std::vector<int> ids;
        nodes::Graph sharp = chain({QStringLiteral("composite_encode"), QStringLiteral("composite_enhancer"), QStringLiteral("composite_decode")}, &ids);
        sharp.find(ids[1])->params[QStringLiteral("gainChannel")].value = 3;
        std::vector<int> lids;
        nodes::Graph soft = chain({QStringLiteral("composite_encode"), QStringLiteral("composite_low_pass"), QStringLiteral("composite_decode")}, &lids);
        soft.find(lids[1])->params[QStringLiteral("resistance")].value = 4700;
        soft.find(lids[1])->params[QStringLiteral("capacitor")].value = 2;
        const QImage sharpOut = nodes::Evaluator().run(sharp, picture.scaled(320, 240), at(30));
        const QImage softOut = nodes::Evaluator().run(soft, picture.scaled(320, 240), at(30));
        const QImage plainOut = nodes::Evaluator().run(tv, picture.scaled(320, 240), at(30));
        save(sharpOut, "enhancer");
        save(softOut, "lowpass");
        save(plainOut, "plain");
        if (dump) // (each example that uses them, as it looks on frame 30 of a picture and colour bars side by side)
            for (const QFileInfo& fi : examplesIn(QStringLiteral("data/effects"))) {
                nodes::Graph example;
                nodes::load(fi.absoluteFilePath(), &example, nullptr);
                if (std::none_of(example.nodes.begin(), example.nodes.end(), [](const nodes::Node& n) { return n.type.startsWith(QLatin1String("composite_")); }))
                    continue;
                QImage both(640, 240, QImage::Format_ARGB32_Premultiplied);
                QPainter p(&both);
                p.drawImage(0, 0, testPicture(QSize(320, 240)));
                p.drawImage(320, 0, bars);
                p.end();
                nodes::Evaluator e;
                for (int f = 26; f < 30; ++f)
                    e.run(example, both, at(f));
                e.run(example, both, at(30)).save(QDir::temp().filePath(QStringLiteral("mvp-ex-%1.png").arg(fi.completeBaseName())));
            }
        check(edges(sharpOut) > edges(plainOut) * 1.2 && edges(softOut) < edges(plainOut) * 0.8,
              QStringLiteral("composite: the Enhancer sharpens (%1), a 4.7k into 1 nF low pass smears (%2; through the TV alone %3)")
                  .arg(edges(sharpOut), 0, 'f', 2).arg(edges(softOut), 0, 'f', 2).arg(edges(plainOut), 0, 'f', 2));
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
    {
        // The GPU renderer runs a graph node by node (GpuCompositor): each node as the CPU draws it, the examples too,
        // over a few frames (Delay, feedback), each fast enough for the Live tab.
        const auto surface = createGpuSurface();
        QString err;
        auto gpu = surface ? createFrameRenderer(false, surface.get(), &err) : nullptr;
        if (!gpu || !gpu->isGpu()) {
            QTextStream(stdout) << "SKIP  user effects on the GPU: no GPU here (" << err << ")" << Qt::endl;
        } else {
            QTextStream(stdout) << "INFO  user effects on the GPU: " << gpu->name() << Qt::endl;
            const QImage pic = testPicture(QSize(640, 360));
            static const EffectChain kNoChain;
            {
                // A screen-shader zip as GitHub gives one (everything in one folder), unpacked and run: a two-pass preset of
                // our own (an alias, a parameter set by the preset, a pass at half size, nearest sampling). Each pass
                // inverts what it gets by Amount: twice at 1 is the picture back (at the 240 lines it is fed, scaled up).
                // (A pass of our own, lines joined with newlines; the preset likewise.)
                const QString shader = QStringList{
                    QStringLiteral("#pragma parameter AMOUNT \"Amount\" 0.5 0.0 1.0 0.05"),
                    QStringLiteral("#ifndef PARAMETER_UNIFORM"), QStringLiteral("#define AMOUNT 0.5"), QStringLiteral("#endif"),
                    QStringLiteral("#if defined(VERTEX)"),
                    QStringLiteral("in vec4 VertexCoord; in vec4 TexCoord; out vec2 uv; uniform mat4 MVPMatrix;"),
                    QStringLiteral("void main() { gl_Position = MVPMatrix * VertexCoord; uv = TexCoord.xy; }"),
                    QStringLiteral("#elif defined(FRAGMENT)"),
                    QStringLiteral("in vec2 uv; out vec4 FragColor; uniform sampler2D Texture; uniform float AMOUNT;"),
                    QStringLiteral("void main() { vec3 c = texture(Texture, uv).rgb; FragColor = vec4(mix(c, 1.0 - c, AMOUNT), 1.0); }"),
                    QStringLiteral("#endif")}.join(QLatin1Char('\n'));
                const QString preset = QStringList{QStringLiteral("shaders = 2"), QStringLiteral("shader0 = shaders/flip.glsl"),
                                                   QStringLiteral("scale_type0 = source"), QStringLiteral("scale0 = 0.5"), QStringLiteral("alias0 = FIRST"),
                                                   QStringLiteral("shader1 = shaders/flip.glsl"), QStringLiteral("filter_linear1 = false"),
                                                   QStringLiteral("parameters = AMOUNT"), QStringLiteral("AMOUNT = 1.0")}
                                           .join(QLatin1Char('\n'));
                QTemporaryDir zipDir;
                const QString zipPath = zipDir.filePath(QStringLiteral("shaders.zip"));
                {
                    QZipWriter zw(zipPath);
                    zw.addDirectory(QStringLiteral("glsl-shaders-master")); // (GitHub's zips list their folder first)
                    zw.addFile(QStringLiteral("glsl-shaders-master/test/flip.glslp"), preset.toUtf8());
                    zw.addFile(QStringLiteral("glsl-shaders-master/test/shaders/flip.glsl"), shader.toUtf8());
                    zw.close();
                }
                const QByteArray keep = qgetenv("MVP_SCREEN_SHADERS_DIR");
                qputenv("MVP_SCREEN_SHADERS_DIR", zipDir.filePath(QStringLiteral("wrap/glsl-shaders-master")).toUtf8());
                QString why;
                QTextStream(stdout) << "INFO  a screen-shader zip: unpacking" << Qt::endl; // (where a crash happened, on CI)
                const bool unpacked = retro::installZip(zipPath, &why);
                QTextStream(stdout) << "INFO  a screen-shader zip: unpacked (" << why << "), running it" << Qt::endl;
                // (Read from the folder above as well: GitHub's folder alone in it is looked into, as an older download left it.)
                qputenv("MVP_SCREEN_SHADERS_DIR", zipDir.filePath(QStringLiteral("wrap")).toUtf8());
                retro::reload();
                const std::vector<const EffectDef*> mine = retro::effects();
                const EffectDef* flip = retro::findEffect(QStringLiteral("retro.test/flip"));
                const retro::Preset* pre = retro::preset(QStringLiteral("retro.test/flip"));
                double back = 999;
                if (flip && pre) {
                    auto r = createFrameRenderer(false, surface.get(), nullptr);
                    EffectInstance fx = makeEffect(*flip, 1);
                    const QImage out = r->renderLive(pic, EffectChain{fx}, 0, 30);
                    QImage opaque(pic.size(), QImage::Format_ARGB32_Premultiplied);
                    opaque.fill(Qt::black);
                    QPainter(&opaque).drawImage(0, 0, pic);
                    back = difference(out, opaque);
                }
                check(unpacked && mine.size() == 1 && flip && flip->category == QLatin1String("Retro Shaders/test") && pre && pre->passes.size() == 2
                          && pre->passes[0].alias == QLatin1String("FIRST") && pre->parameters.size() == 1 && pre->parameters[0].def == 1.0
                          && back < 12 && !retro::passSource(QStringLiteral("#version 120") + QLatin1Char('\n') + QStringLiteral("void main(){}"), true)
                                                .contains(QLatin1String("120")),
                      QStringLiteral("screen shaders: a zip unpacked into a preset effect (its parameter at the preset's value), run pass after pass "
                                     "on the GPU: inverted twice is the picture back (%1 off at 240 lines) %2").arg(back, 0, 'f', 1).arg(why));
                if (keep.isEmpty())
                    qunsetenv("MVP_SCREEN_SHADERS_DIR");
                else
                    qputenv("MVP_SCREEN_SHADERS_DIR", keep);
                retro::reload();
            }
            if (qEnvironmentVariableIsSet("MVP_SCREEN_SHADERS_DIR")) {
                // Screen shaders (libretro's, from MVP_SCREEN_SHADERS_DIR): every preset run on the GPU over three frames.
                // One that can't compile leaves the picture as it was; one that runs changes it (it is fed in at 240 lines).
                const std::vector<const EffectDef*> all = retro::effects();
                QStringList failed;
                int ran = 0;
                QElapsedTimer timer;
                timer.start();
                for (const EffectDef* def : all) {
                    auto r = createFrameRenderer(false, surface.get(), nullptr);
                    const EffectChain chain{makeEffect(*def, 1)};
                    QImage out;
                    for (int f = 0; f < 3; ++f)
                        out = r->renderLive(pic, chain, TimeUs(f) * kUsPerSecond / 30, 30);
                    if (!out.isNull() && difference(out, pic) > 0.01)
                        ++ran;
                    if (qEnvironmentVariableIsSet("MVP_SCREEN_SHADER_GRABS")
                        && qEnvironmentVariable("MVP_SCREEN_SHADER_GRABS").split(QLatin1Char(',')).contains(def->id.mid(6)))
                        out.save(QDir::temp().filePath(QStringLiteral("mvp-shader-%1.png").arg(QString(def->id.mid(6)).replace(QLatin1Char('/'), QLatin1Char('_')))));
                    else
                        failed << def->id.mid(6);
                }
                QFile list(QDir::temp().filePath(QStringLiteral("mvp-screen-shaders-failed.txt")));
                if (list.open(QIODevice::WriteOnly | QIODevice::Text))
                    list.write(failed.join(QLatin1Char('\n')).toUtf8());
                check(!all.empty() && ran >= int(all.size()) * 0.8,
                      QStringLiteral("screen shaders: %1 of %2 libretro presets run on the GPU (%3 s; the rest listed in %4)")
                          .arg(ran).arg(all.size()).arg(timer.elapsed() / 1000.0, 0, 'f', 1).arg(QDir::toNativeSeparators(list.fileName())));
            }
            auto renderBoth = [&](const nodes::Graph& g, int frames, double* ms) {
                const EffectDef* def = nodes::registerEffect(g);
                const EffectChain chain{makeEffect(*def, 1)};
                Compositor cpu;
                auto fresh = createFrameRenderer(false, surface.get(), nullptr); // (its own effect states)
                QImage a, b;
                QElapsedTimer t;
                std::vector<qint64> gpuNs; // (the median frame: one slow one, the machine busy, doesn't decide it)
                for (int f = 0; f < frames; ++f) {
                    a = cpu.renderLive(pic, chain, TimeUs(f) * 33333, 30, true);
                    t.start();
                    b = fresh->renderLive(pic, chain, TimeUs(f) * 33333, 30, true);
                    gpuNs.push_back(t.nsecsElapsed());
                }
                std::sort(gpuNs.begin(), gpuNs.end());
                if (ms)
                    *ms = gpuNs.empty() ? 0 : gpuNs[gpuNs.size() / 2] / 1e6;
                return difference(a, b);
            };
            // Each node with what it takes: the source, and a checkerboard (drawn the same on both) as a second picture.
            struct Case {
                QString type;
                std::map<QString, double> settings;
            };
            std::vector<Case> cases = {{QStringLiteral("mix"), {}}, {QStringLiteral("set_alpha"), {}}, {QStringLiteral("set_alpha"), {{QStringLiteral("invert"), 1}}},
                                       {QStringLiteral("luma_key"), {}}, {QStringLiteral("difference_key"), {}}, {QStringLiteral("combine"), {}},
                                       {QStringLiteral("threshold"), {}}, {QStringLiteral("displace"), {}}, {QStringLiteral("displace"), {{QStringLiteral("use"), 1}}},
                                       {QStringLiteral("place"), {{QStringLiteral("x"), 0.2}, {QStringLiteral("rotation"), 30}}}, {QStringLiteral("delay"), {{QStringLiteral("frames"), 2}}},
                                       {QStringLiteral("fx.glow"), {}}, {QStringLiteral("gen.retro_grid"), {}}, {QStringLiteral("gen.hyperspace"), {}}};
            for (const char* gen : {"plasma_neon", "aurora", "bokeh_warm", "digital_rain", "confetti", "progress_bar", "letterbox",
                                    "shape_heart", "kinetic_text", "countdown", "bars_smpte"})
                cases.push_back({QStringLiteral("gen.") + QLatin1String(gen), {}});
            for (int op = 0; op < 8; ++op)
                cases.push_back({QStringLiteral("math"), {{QStringLiteral("op"), op}}});
            for (int ch = 0; ch < 5; ++ch)
                cases.push_back({QStringLiteral("channel"), {{QStringLiteral("channel"), ch}}});
            for (int mode = 0; mode < 8; ++mode)
                cases.push_back({QStringLiteral("blend"), {{QStringLiteral("mode"), mode}, {QStringLiteral("opacity"), 0.8}}});
            for (int mode = 0; mode < 4; ++mode)
                cases.push_back({QStringLiteral("mask"), {{QStringLiteral("mode"), mode}}});
            QStringList off;
            double worst = 0;
            for (const Case& c : cases) {
                nodes::Graph g = nodes::starter();
                g.name = QStringLiteral("GPU node selftest");
                const int src = g.nodes.front().id, out = g.nodes.back().id;
                const int checker = g.addNode(QStringLiteral("gen.checkerboard"), {});
                const int n = g.addNode(c.type, {});
                for (const auto& [k, v] : c.settings)
                    g.find(n)->params[k].value = v;
                const int inputs = int(nodes::findNode(c.type)->inputs.size());
                for (int i = 0; i < inputs; ++i)
                    g.connect(i % 2 ? checker : src, n, i);
                g.connect(n, out, 0);
                const double d = renderBoth(g, 3, nullptr);
                worst = std::max(worst, d);
                if (d > (c.type.startsWith(QLatin1String("gen.")) ? 6.0 : 2.0)) // (sources painted on the GPU: lines antialiased its way)
                    off << QStringLiteral("%1 %2 (%3)").arg(c.type).arg(c.settings.empty() ? 0 : c.settings.begin()->second).arg(d, 0, 'f', 1);
            }
            check(off.isEmpty(), QStringLiteral("each node draws on the GPU as on the CPU (worst %1 of 255; off: %2)")
                                     .arg(worst, 0, 'f', 2)
                                     .arg(off.isEmpty() ? QStringLiteral("none") : off.join(QStringLiteral(", "))));
            QStringList slowOrOff;
            for (const QFileInfo& fi : examplesIn(nodes::examplesFolder())) {
                nodes::Graph g;
                if (!nodes::load(fi.absoluteFilePath(), &g, nullptr))
                    continue;
                double ms = 0;
                const double d = renderBoth(g, 5, &ms);
                QTextStream(stdout) << "      " << g.name << ": " << QString::number(ms, 'f', 1) << " ms a frame on the GPU, "
                                    << QString::number(d, 'f', 2) << " off" << Qt::endl;
                if (d > 6.0 || ms > 33.0)
                    slowOrOff << QStringLiteral("%1 (%2 off, %3 ms)").arg(g.name).arg(d, 0, 'f', 1).arg(ms, 0, 'f', 1);
            }
            {
                // The Live tab's chains in one go (one upload) draw as one at a time.
                const EffectChain liveFx{makeEffect(*findEffect(QStringLiteral("glow")), 11)};
                EffectChain padFx{makeEffect(*findEffect(QStringLiteral("rgb_split")), 12)};
                auto one = createFrameRenderer(false, surface.get(), nullptr), both = createFrameRenderer(false, surface.get(), nullptr);
                const QImage apart = one->renderLive(one->renderLive(pic, liveFx, 0, 30), padFx, 0, 30);
                LiveFrameJob job;
                job.frame = pic;
                job.liveFx = &liveFx;
                job.after = {&kNoChain, &padFx};
                const QImage together = both->renderLiveFrame(job);
                check(difference(apart, together) < 0.01, QStringLiteral("the Live FX and a pad in one go draw as one after the other (%1 off)")
                                                              .arg(difference(apart, together), 0, 'f', 3));
            }
            {
                // The Live tab's whole frame in one go on the GPU as on the CPU: a layer in each blend mode (with its own
                // FX, cropped to fill or squeezed into a corner), a second one over every effect (No Effects), the Live FX,
                // a pad switch half way, held pads, and the picture before the effects.
                const QImage second = testPicture(QSize(320, 240)).mirrored(true, false);
                const EffectChain liveFx{makeEffect(*findEffect(QStringLiteral("hsl")), 21)};
                const EffectChain layerFx{makeEffect(*findEffect(QStringLiteral("gaussian_blur")), 22)};
                const EffectChain padA{makeEffect(*findEffect(QStringLiteral("invert")), 23)};
                const EffectChain padB{makeEffect(*findEffect(QStringLiteral("rgb_split")), 24)};
                QStringList bad;
                double worst = 0;
                for (int blend = 0; blend < 8; ++blend)
                    for (int fade = 0; fade < 2; ++fade) {
                        LiveFrameJob job;
                        job.frame = pic;
                        LiveFrameLayer layer;
                        layer.image = second;
                        layer.cover = blend % 2 == 1;
                        layer.rect = layer.cover ? QRect(QPoint(0, 0), pic.size()) : QRect(400, 200, 200, 150);
                        layer.opacity = 0.8;
                        layer.blend = blend;
                        layer.fx = blend == 2 ? &layerFx : nullptr;
                        LiveFrameLayer top = layer;
                        top.rect = QRect(20, 20, 160, 120);
                        top.cover = false;
                        top.blend = 7 - blend;
                        top.overEffects = true;
                        job.layers = {layer, top};
                        job.liveFx = &liveFx;
                        if (fade) {
                            job.fadeProgress = 0.4;
                            job.fadeFrom = &padA;
                            job.fadeTo = &padB;
                            job.fadeSpec.type = blend < 4 ? QStringLiteral("dissolve") : QStringLiteral("wipe");
                        } else {
                            job.after = {&padB};
                        }
                        Compositor cpu;
                        auto g = createFrameRenderer(false, surface.get(), nullptr);
                        QImage cleanCpu, cleanGpu;
                        const double d = difference(cpu.renderLiveFrame(job, &cleanCpu), g->renderLiveFrame(job, &cleanGpu));
                        const double dc = difference(cleanCpu, cleanGpu);
                        worst = std::max({worst, d, dc});
                        if (d > 2.0 || dc > 2.0)
                            bad << QStringLiteral("blend %1%2 (%3, clean %4)").arg(blend).arg(fade ? " fading" : "").arg(d, 0, 'f', 1).arg(dc, 0, 'f', 1);
                    }
                check(bad.isEmpty(), QStringLiteral("the Live tab's frame (layer, Live FX, pad switch) on the GPU as on the CPU (worst %1; off: %2)")
                                         .arg(worst, 0, 'f', 2)
                                         .arg(bad.isEmpty() ? QStringLiteral("none") : bad.join(QStringLiteral(", "))));
            }
            {
                // The Effect Builder's preview on the GPU: the result and each node's thumbnail as the CPU's evaluator.
                auto g = createFrameRenderer(false, surface.get(), nullptr);
                auto* gc = dynamic_cast<GpuCompositor*>(g.get());
                nodes::Graph graph;
                nodes::load(QDir(nodes::examplesFolder()).filePath(QStringLiteral("Split Reality.mvpfx")), &graph, nullptr);
                RenderContext rc;
                rc.scale = 640 / 1920.0;
                rc.fps = 30;
                rc.contentRect = QRectF(pic.rect());
                nodes::Evaluator ev;
                const QImage cpuOut = ev.run(graph, pic, rc, true);
                std::map<int, QImage> thumbs;
                const QImage gpuOut = gc ? gc->renderGraph(graph, pic, rc, &thumbs, QSize(164, 92)) : QImage();
                double thumbOff = thumbs.size() == ev.outputs.size() ? 0 : 255;
                for (const auto& [id, t] : thumbs)
                    if (ev.outputs.count(id))
                        thumbOff = std::max(thumbOff, difference(t, ev.outputs[id].scaled(t.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation)));
                check(gc && difference(cpuOut, gpuOut) < 3 && thumbOff < 8,
                      QStringLiteral("the Effect Builder's preview on the GPU: as the CPU's (%1 off), %2 node pictures (worst %3 off)")
                          .arg(difference(cpuOut, gpuOut), 0, 'f', 2)
                          .arg(thumbs.size())
                          .arg(thumbOff, 0, 'f', 1));
            }
            check(slowOrOff.isEmpty(), QStringLiteral("the example effects draw on the GPU as on the CPU, at 30 frames a second (not: %1)")
                                           .arg(slowOrOff.isEmpty() ? QStringLiteral("none") : slowOrOff.join(QStringLiteral(", "))));
        }
    }
    QTextStream(stdout) << (failures ? "FAILED" : "ALL PASSED") << Qt::endl;
    return failures ? 1 : 0;
}
