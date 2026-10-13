#include "core/Compositor.h"
#include "core/FrameRenderer.h"
#include "core/GpuCompositor.h"
#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "core/Story.h"
#include "core/FFmpeg.h"
#include "ui/GraphicsEditor.h"
#include <QApplication>
#include <QCheckBox>
#include <QDataStream>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLineF>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cmath>

namespace {
QJsonObject rect(QString id, double x = 0, double y = 0, double w = 1,
                 double h = 1, QString fill = "#ff0000") {
  return {{"type", "rect"}, {"id", id},    {"x", x},      {"y", y},
          {"width", w},     {"height", h}, {"fill", fill}};
}
QJsonObject scene(QJsonArray layers) {
  return {{"version", 2}, {"layers", layers}};
}
QJsonObject keys(QJsonValue a, QJsonValue b, QString easing = "linear") {
  return {
      {"keyframes", QJsonArray{QJsonArray{0, a, easing}, QJsonArray{1, b}}}};
}
QImage picture(QJsonObject s, double t = 0, QSize size = {200, 100}) {
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::transparent);
  motion::render(image, s, t);
  return image;
}
} // namespace
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  int failures = 0;
  auto check = [&](bool ok, QString what) {
    QTextStream(stdout) << (ok ? "PASS " : "FAIL ") << what << Qt::endl;
    if (!ok)
      ++failures;
  };
  QTimer dialogs;
  dialogs.setInterval(50);
  QObject::connect(&dialogs, &QTimer::timeout, [&] {
    for (auto *w : QApplication::topLevelWidgets())
      if (auto *box = qobject_cast<QMessageBox *>(w); box && box->isVisible()) {
        check(false, "unexpected dialog: " + box->text());
        box->reject();
      }
  });
  dialogs.start();
  QString artifacts = qEnvironmentVariable("MVP_TEST_ARTIFACTS");
  if (!artifacts.isEmpty())
    QDir().mkpath(artifacts);
  for (auto id : motion::presets()) {
    auto s = motion::preset(id);
    auto error = motion::validate(s);
    check(error.isEmpty(), "valid " + id + " " + error);
    auto a = picture(s, 0, {640, 360}), b = picture(s, 2, {640, 360});
    check(!a.isNull() && !b.isNull(), "render " + id);
    if (id != "motion_audio" && id != "motion_reusable")
      check(a != b, "time changes " + id);
    if (!artifacts.isEmpty()) {
      b.save(artifacts + "/" + id + ".png");
      QFile file(artifacts + "/" + id + ".json");
      file.open(QIODevice::WriteOnly);
      file.write(QJsonDocument(s).toJson());
    }
  }
  check(std::abs(motion::evaluate(keys(0, 1, "ease_in"), .5).toDouble() -
                 .125) < 1e-6,
        "cubic ease-in");
  check(std::abs(motion::ease(.5, QJsonArray{.42, 0, .58, 1}) - .5) < 1e-6,
        "custom cubic Bezier timing");
  check(motion::evaluate(keys(0, 1, "hold"), .99) == 0 &&
            motion::evaluate(keys(0, 1, "hold"), 1) == 1,
        "hold key closes at exact boundary");
  check(motion::evaluate(keys("before", "after"), .9) == "before" &&
            motion::evaluate(keys("before", "after"), 1) == "after",
        "text keys are discrete");
  QColor mid(motion::evaluate(keys("#ff0000", "#0000ff"), .5).toString());
  check(std::abs(mid.red() - 128) <= 1 && std::abs(mid.blue() - 128) <= 1,
        "colours interpolate");
  auto ping = keys(0, 1);
  ping["loop"] = 1;
  ping["ping_pong"] = true;
  check(std::abs(motion::evaluate(ping, 1.75).toDouble() - .25) < 1e-6 &&
            motion::evaluate(ping, 2) == 0,
        "ping-pong traverses backwards and loops");
  auto invalid = scene(QJsonArray{rect("a")});
  auto r = rect("a");
  r["x"] = QJsonObject{
      {"keyframes", QJsonArray{QJsonArray{1, 1}, QJsonArray{0, 0}}}};
  invalid["layers"] = QJsonArray{r};
  check(!motion::validate(invalid).isEmpty(), "unordered keys rejected");
  r["x"] = QJsonObject{
      {"keyframes", QJsonArray{QJsonArray{0, 0}, QJsonArray{0, 1}}}};
  invalid["layers"] = QJsonArray{r};
  check(!motion::validate(invalid).isEmpty(), "duplicate key times rejected");
  r = rect("a");
  r["fill"] = keys("#ff0000", "bad-color");
  check(!motion::validate(scene({r})).isEmpty(),
        "invalid animated colours rejected");
  r = rect("a");
  r["parent"] = "missing";
  check(!motion::validate(scene({r})).isEmpty(), "unknown parents rejected");
  QJsonObject ga{{"type", "group"}, {"id", "a"}, {"parent", "b"}},
      gb{{"type", "group"}, {"id", "b"}, {"parent", "a"}};
  check(!motion::validate(scene({ga, gb})).isEmpty(), "parent cycles rejected");
  QJsonObject route{
      {"type", "path"},
      {"id", "route"},
      {"x", .1},
      {"y", .2},
      {"width", .8},
      {"height", .6},
      {"commands", QJsonArray{QJsonArray{"M", 0, .5}, QJsonArray{"L", 1, .5}}},
      {"fill", "#00000000"},
      {"stroke", "#0000ff"},
      {"stroke_width", .02}};
  ga = {{"type", "group"},
        {"id", "group"},
        {"follow", QJsonObject{{"path", "route"}}},
        {"children", QJsonArray{route}}};
  check(!motion::validate(scene({ga})).isEmpty(),
        "implicit nested path/parent cycle rejected");
  QJsonObject badSymbol =
      scene({QJsonObject{{"type", "instance"}, {"symbol", "recursive"}}});
  badSymbol["symbols"] = QJsonObject{
      {"recursive", QJsonObject{{"layers", QJsonArray{QJsonObject{
                                               {"type", "instance"},
                                               {"symbol", "recursive"}}}}}}};
  check(!motion::validate(badSymbol).isEmpty(), "recursive symbols rejected");
  auto localCycle =
      scene({QJsonObject{{"type", "instance"}, {"symbol", "bad"}}});
  localCycle["symbols"] = QJsonObject{
      {"bad",
       QJsonObject{
           {"layers", QJsonArray{QJsonObject{
                          {"type", "group"}, {"id", "a"}, {"parent", "a"}}}}}}};
  check(!motion::validate(localCycle).isEmpty(),
        "symbol-local parent cycles rejected");
  QJsonArray tooMany;
  for (int i = 0; i < 513; ++i)
    tooMany.append(rect(QString::number(i)));
  check(!motion::validate(scene(tooMany)).isEmpty(), "layer budget enforced");
  auto huge = scene({QJsonObject{
      {"type", "repeater"},
      {"count", 128},
      {"children",
       QJsonArray{QJsonObject{{"type", "repeater"},
                              {"count", 128},
                              {"children", QJsonArray{rect("x")}}}}}}});
  check(!motion::validate(huge).isEmpty(), "expanded repeater work is bounded");
  auto group =
      QJsonObject{{"type", "group"},
                  {"id", "g"},
                  {"x", .2},
                  {"y", .3},
                  {"width", .5},
                  {"height", .5},
                  {"children", QJsonArray{rect("child", .2, .2, .2, .2)}}};
  auto im = picture(scene({group}));
  check(im.pixelColor(70, 45).red() > 240 && im.pixelColor(25, 20).alpha() == 0,
        "nested normalized group transforms");
  auto child = rect("child", .2, .2, .2, .2);
  child["parent"] = "g";
  auto flat = group;
  flat.remove("children");
  check(picture(scene({flat, child})) == im,
        "flat parenting equals nested group rendering");
  group["rotation"] = 90;
  auto gs = scene({group});
  auto bound = motion::bounds(gs, "child", 0, {200, 100});
  check(picture(gs).pixelColor(bound.center().toPoint()).red() > 240,
        "rotated parent bounds match rendering");
  auto actor = rect("car", 0, 0, .1, .1);
  actor["follow"] = QJsonObject{
      {"path", "route"}, {"progress", keys(0, 1)}, {"orient", false}};
  auto racing = scene({route, actor});
  check(picture(racing, .5).pixelColor(100, 50).red() > 240 &&
            picture(racing, 0).pixelColor(20, 50).red() > 240,
        "path followers use actual arc positions");
  racing["camera"] = QJsonObject{{"follow", "car"}, {"zoom", 2}};
  auto a = motion::bounds(racing, "car", 0, {200, 100}),
       b = motion::bounds(racing, "car", 1, {200, 100});
  check(QLineF(a.center(), QPointF(100, 50)).length() < .01 &&
            QLineF(b.center(), QPointF(100, 50)).length() < .01,
        "camera follows moving objects");
  auto masked = rect("mask");
  masked["mask"] = QJsonObject{{"type", "ellipse"},
                               {"x", .25},
                               {"y", .25},
                               {"width", .5},
                               {"height", .5}};
  auto mi = picture(scene({masked}));
  check(mi.pixelColor(0, 0).alpha() == 0 &&
            mi.pixelColor(100, 50).alpha() == 255,
        "ellipse clipping mask");
  auto m = masked["mask"].toObject();
  m["invert"] = true;
  masked["mask"] = m;
  mi = picture(scene({masked}));
  check(mi.pixelColor(0, 0).alpha() == 255 &&
            mi.pixelColor(100, 50).alpha() == 0,
        "inverse clipping mask");
  auto alphaGroup =
      QJsonObject{{"type", "group"},
                  {"id", "alpha"},
                  {"opacity", .5},
                  {"children", QJsonArray{rect("one", .1, .1, .5, .5),
                                          rect("two", .3, .1, .5, .5)}}};
  auto alpha = picture(scene({alphaGroup}));
  check(std::abs(alpha.pixelColor(40, 30).alpha() -
                 alpha.pixelColor(80, 30).alpha()) <= 1 &&
            std::abs(alpha.pixelColor(80, 30).alpha() - 128) <= 1,
        "group opacity composites children once");
  alphaGroup["blend"] = "multiply";
  auto blend =
      picture(scene({rect("back", 0, 0, 1, 1, "#00ff00"), alphaGroup}));
  check(blend.pixelColor(80, 30).red() == 0 &&
            std::abs(blend.pixelColor(80, 30).green() - 127) <= 1,
        "group blending applies to the composed group once");
  auto gradient = rect("gradient");
  gradient["gradient"] =
      QJsonObject{{"type", "linear"},
                  {"x2", 1},
                  {"y2", 0},
                  {"stops", QJsonArray{QJsonArray{0, "#ff0000"},
                                       QJsonArray{1, "#0000ff"}}}};
  auto gi = picture(scene({gradient}));
  check(gi.pixelColor(5, 50).red() > gi.pixelColor(5, 50).blue() &&
            gi.pixelColor(195, 50).blue() > gi.pixelColor(195, 50).red(),
        "gradient fills");
  auto reveal = route;
  reveal["trim_end"] = keys(0, 1);
  auto ri = picture(scene({reveal}), .5);
  check(ri.pixelColor(30, 50).blue() > 200 &&
            ri.pixelColor(175, 50).alpha() == 0,
        "stroke reveal trims real path length");
  auto moving = rect("moving", 0, .3, .1, .3);
  moving["x"] = keys(0, .8);
  moving["trail"] =
      QJsonObject{{"samples", 8}, {"duration", .5}, {"opacity", 1}};
  auto trail = picture(scene({moving}), .8);
  check(trail.pixelColor(135, 40).alpha() > 200 &&
            trail.pixelColor(100, 40).alpha() > 0,
        "motion trails retain past positions");
  auto blurred = rect("blur", .3, .2, .3, .4);
  blurred["motion_blur"] = QJsonObject{{"samples", 5}, {"shutter", .1}};
  check(picture(scene({blurred}), .5).pixelColor(100, 40).alpha() > 250,
        "stationary motion blur retains opacity");
  auto text = rect("words", .1, .1, .8, .8);
  text["type"] = "text";
  text["font_size"] = .2;
  text["text"] = "One two\nthree";
  text["text_animation"] = QJsonObject{
      {"unit", "word"}, {"style", "rise"}, {"delay", 1}, {"stagger", .2}};
  check(picture(scene({text}), .5) != picture(scene({text}), 3),
        "word animation and multiline layout");
  text["text"] = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
  auto plainText = text;
  plainText.remove("text_animation");
  check(picture(scene({text}), 3) == picture(scene({plainText}), 3),
        "animated words retain all wrapped lines");
  auto particles = motion::preset("motion_particles");
  check(picture(particles, 2) == picture(particles, 2) &&
            picture(particles, 1) != picture(particles, 2),
        "particles seek deterministically");
  QTemporaryDir dir;
  auto audio = dir.path() + "/audio.wav";
  QFile file(audio);
  file.open(QIODevice::WriteOnly);
  QDataStream wav(&file);
  wav.setByteOrder(QDataStream::LittleEndian);
  int frames = 96000;
  file.write("RIFF", 4);
  wav << quint32(36 + frames * 4);
  file.write("WAVEfmt ", 8);
  wav << quint32(16) << quint16(1) << quint16(2) << quint32(48000)
      << quint32(192000) << quint16(4) << quint16(16);
  file.write("data", 4);
  wav << quint32(frames * 4);
  for (int i = 0; i < frames; ++i) {
    qint16 sample =
        i < 48000
            ? 0
            : qint16(std::sin(i * 2 * 3.141592653589793 * 440 / 48000) * 16384);
    wav << sample << sample;
  }
  file.close();
  QString error;
  auto envelope =
      graphics::buildWaveform(audio, 0, 0, 2, 256, &error, {}, true);
  check(error.isEmpty() && envelope.contains("beats") &&
            envelope.contains("beat_confidence"),
        "native envelope and beat analysis");
  auto audioScene = scene({rect("audio-driven", 0, .1, .1, .8)});
  audioScene["audio"] = QJsonObject{{"source", envelope}};
  auto audioLayer = rect("audio-driven", 0, .1, .1, .8);
  audioLayer["x"] =
      QJsonObject{{"audio", "source"}, {"base", 0}, {"amount", 1}};
  audioScene["layers"] = QJsonArray{audioLayer};
  check(picture(audioScene, .2) != picture(audioScene, 1.5),
        "real source audio drives scene properties");
  auto beatEnvelope = envelope;
  beatEnvelope["beats"] = QJsonArray{.5, 1};
  audioScene["audio"] = QJsonObject{{"source", beatEnvelope}};
  check(motion::evaluate(QJsonObject{{"audio", "source"},
                                     {"channel", "beat"},
                                     {"release", .2}},
                         .5, audioScene)
                .toDouble() > .99,
        "beat-triggered animation envelope");
  auto edited = motion::withKeyframe(scene({rect("edit")}), "edit", "x", 1, .5,
                                     "smooth", &error);
  check(!edited.isEmpty() && edited["layers"]
                                     .toArray()[0]
                                     .toObject()["x"]
                                     .toObject()["keyframes"]
                                     .toArray()
                                     .size() == 2,
        "keyframe edits retain initial values");
  check(motion::withKeyframe(edited, "edit", "id", 2, "bad", "linear", &error)
            .isEmpty(),
        "structural properties cannot be animated");
  Project project;
  auto timeline = project.timeline();
  int eid = ops::insertGenerated(
      timeline, *findGeneratorPreset("graphics_blank"), 0, -1, {});
  timeline.findClip(eid)->generator.data = motion::preset("motion_route");
  project.applyEdit(timeline, "motion fixture");
  Compositor cpu;
  auto cpuFrame = cpu.render(*project.snapshot(), 1000000, {640, 360});
  auto surface = createGpuSurface();
  auto gpu =
      surface ? GpuCompositor::create(surface.get(), true, &error) : nullptr;
  if (QGuiApplication::platformName() != "offscreen")
    check(bool(gpu), "GPU available");
  if (gpu) {
    auto gpuFrame = gpu->render(*project.snapshot(), 1000000, {640, 360})
                        .convertToFormat(QImage::Format_RGBA8888);
    auto cf = cpuFrame.convertToFormat(QImage::Format_RGBA8888);
    double delta = 0;
    for (int y = 0; y < cf.height(); ++y)
      for (int x = 0; x < cf.width() * 4; ++x)
        delta += std::abs(int(cf.constScanLine(y)[x]) -
                          int(gpuFrame.constScanLine(y)[x]));
    check(delta / cf.width() / cf.height() / 4 < 3, "v2 CPU/GPU parity");
    for (const auto &id : motion::presets()) {
      timeline.findClip(eid)->generator.data = motion::preset(id);
      project.applyEdit(timeline, "GPU fixture " + id);
      auto c = cpu.render(*project.snapshot(), 1500000, {640, 360}).convertToFormat(QImage::Format_RGBA8888);
      auto g = gpu->render(*project.snapshot(), 1500000, {640, 360}).convertToFormat(QImage::Format_RGBA8888);
      double difference = 0;
      for (int y = 0; y < c.height(); ++y)
        for (int x = 0; x < c.width()*4; ++x)
          difference += std::abs(int(c.constScanLine(y)[x])-int(g.constScanLine(y)[x]));
      check(difference / c.width() / c.height() / 4 < 3, "direct GPU paint parity: " + id);
      if (!artifacts.isEmpty()) {
        c.save(artifacts + "/" + id + "-cpu.png");
        g.save(artifacts + "/" + id + "-gpu.png");
      }
    }
    // A static title must exist on its first GPU frame and stay cached correctly.
    timeline.findClip(eid)->generator.data = scene({
      QJsonObject{{"type", "text"}, {"id", "title"}, {"text", "GPU FIRST FRAME"},
       {"x", .05}, {"y", .05}, {"width", .9}, {"height", .6},
       {"font_size", .12}, {"fill", "#ffffff"}}});
    project.applyEdit(timeline, "static GPU title");
    auto title = gpu->render(*project.snapshot(), 0, {640,360});
    auto reference = cpu.render(*project.snapshot(), 0, {640,360});
    auto direct = picture(timeline.findClip(eid)->generator.data, 0, {640,360});
    if (!artifacts.isEmpty()) direct.save(artifacts + "/static-title-direct.png");
    int ink = 0, referenceInk = 0;
    for (int y = 0; y < title.height(); ++y)
      for (int x = 0; x < title.width(); ++x) {
        ink += qRed(title.pixel(x,y)) > 128;
        referenceInk += qRed(reference.pixel(x,y)) > 128;
      }
    check(ink > referenceInk*.9 && ink < referenceInk*1.1, "static GPU title appears on first frame");
    QTextStream(stdout) << "Title ink GPU " << ink << " CPU " << referenceInk << Qt::endl;
    if (!artifacts.isEmpty()) {
      title.save(artifacts + "/static-title-gpu.png");
      reference.save(artifacts + "/static-title-cpu.png");
    }
    check(title == gpu->render(*project.snapshot(), 1000000, {640,360}), "static GPU title cache remains stable");
    auto packed = timeline;
    auto containers = story::compound(packed, {eid}, "GPU compound", &error);
    check(!containers.empty(), "create GPU compound fixture");
    if (!containers.empty()) {
      project.applyEdit(packed, "GPU compound");
      auto nested = gpu->render(*project.snapshot(), 1000000, {640,360});
      check(nested == title, "compound picture stays on GPU with identical output");
      auto twice = packed;
      auto outer = story::compound(twice, {containers.front()}, "GPU nested compound", &error);
      project.applyEdit(twice, "GPU nested compound");
      check(!outer.empty() && gpu->render(*project.snapshot(), 1000000, {640,360}) == title,
            "recursive compound GPU rendering retains alpha and text");
      project.applyEdit(timeline, "restore GPU fixture");
    }
    // Direct encoder output checks both colour matrices, ranges and plane layout.
    auto yuv = ff::makeFrame();
    yuv->width=640; yuv->height=360; yuv->format=AV_PIX_FMT_YUV420P;
    yuv->colorspace=AVCOL_SPC_BT709; yuv->color_range=AVCOL_RANGE_MPEG;
    av_frame_get_buffer(yuv.get(), 32);
    timeline.findClip(eid)->generator.data=scene({rect("red")});
    project.applyEdit(timeline,"GPU YUV fixture");
    check(gpu->renderYuv(*project.snapshot(),0,yuv.get()),"direct GPU YUV420 encoder output");
    check(std::abs(int(yuv->data[0][0])-63)<=1 && std::abs(int(yuv->data[1][0])-102)<=1
          && std::abs(int(yuv->data[2][0])-240)<=1,"GPU BT709 limited-range red values");
    yuv->colorspace=AVCOL_SPC_SMPTE170M;
    gpu->renderYuv(*project.snapshot(),0,yuv.get());
    check(std::abs(int(yuv->data[0][0])-81)<=1 && std::abs(int(yuv->data[1][0])-90)<=1
          && std::abs(int(yuv->data[2][0])-240)<=1,"GPU BT601 limited-range red values");
    yuv->color_range=AVCOL_RANGE_JPEG;
    gpu->renderYuv(*project.snapshot(),0,yuv.get());
    check(std::abs(int(yuv->data[0][0])-76)<=1 && std::abs(int(yuv->data[1][0])-85)<=1
          && yuv->data[2][0]==255,"GPU full-range red values");
    auto nv12=ff::makeFrame();nv12->width=640;nv12->height=360;nv12->format=AV_PIX_FMT_NV12;
    nv12->colorspace=AVCOL_SPC_BT709;nv12->color_range=AVCOL_RANGE_MPEG;
    av_frame_get_buffer(nv12.get(),32);
    check(gpu->renderYuv(*project.snapshot(),0,nv12.get()) && std::abs(int(nv12->data[1][0])-102)<=1
          && std::abs(int(nv12->data[1][1])-240)<=1,"GPU NV12 interleaved chroma output");
    if (app.arguments().contains("--benchmark")) {
      timeline.findClip(eid)->generator.data = motion::preset("motion_route");
      project.applyEdit(timeline, "GPU benchmark");
      QElapsedTimer timer;
      timer.start();
      for (int i = 0; i < 90; ++i)
        gpu->render(*project.snapshot(), i*33333, {1920,1080});
      QTextStream(stdout) << "BENCH graphics 90 HD frames " << timer.elapsed() << " ms" << Qt::endl;
    }
  }
  timeline.findClip(eid)->generator.data=motion::preset("motion_route");
  project.applyEdit(timeline,"restore editor fixture");
  auto saved = dir.path() + "/motion.mvp";
  check(project.save(saved, &error), "save scene graph");
  Project loaded;
  check(loaded.load(saved, &error) && loaded.timeline() == project.timeline(),
        "scene graph project reload is lossless");
  if (app.arguments().contains("--gpu-only"))
    return failures;
  QWidget parent;
  parent.setAttribute(Qt::WA_DontShowOnScreen);
  auto original = project.timeline();
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        auto *list = widget->findChild<QListWidget *>("graphicsLayers");
        list->setCurrentRow(1);
        widget->findChild<QCheckBox *>("graphicsRecordKeys")->setChecked(true);
        widget->findChild<QDoubleSpinBox *>("graphicsPreviewTime")->setValue(2);
        widget->findChild<QDoubleSpinBox *>("graphics_x")->setValue(.15);
        if (!artifacts.isEmpty())
          widget->grab().save(artifacts + "/animation-composer.png");
        qobject_cast<QDialog *>(widget)->accept();
      }
  });
  composeGraphics(&parent, &project, eid);
  auto result = project.timeline().findClip(eid)->generator.data;
  check(result["layers"]
                .toArray()[1]
                .toObject()["x"]
                .toObject()["keyframes"]
                .toArray()
                .size() == 2,
        "composer records keys at the scrubbed time");
  project.undoStack()->undo();
  check(project.timeline() == original,
        "animation composer commits one undo step");
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        auto *list = widget->findChild<QListWidget *>("graphicsLayers");
        list->setCurrentRow(2);
        widget->findChild<QCheckBox *>("graphicsEditPath")->setChecked(true);
        auto *canvas = widget->findChild<QWidget *>("graphicsCanvas");
        double h =
            std::min(double(canvas->height()), canvas->width() * 9. / 16.);
        QSizeF size(h * 16. / 9., h);
        auto hs = motion::handles(
            project.timeline().findClip(eid)->generator.data, "route", 0, size);
        QPointF origin((canvas->width() - size.width()) / 2,
                       (canvas->height() - size.height()) / 2),
            point = origin + hs[0].position;
        QMouseEvent press(QEvent::MouseButtonPress, point, Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &press);
        QMouseEvent move(QEvent::MouseMove, point + QPointF(20, 0),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, point + QPointF(20, 0),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(canvas, &release);
        qobject_cast<QDialog *>(widget)->accept();
      }
  });
  composeGraphics(&parent, &project, eid);
  check(project.timeline()
                .findClip(eid)
                ->generator.data["layers"]
                .toArray()[2]
                .toObject()["commands"]
                .toArray()[0]
                .toArray()[1]
                .toDouble() > 0,
        "dragging native curve handles changes geometry");
  auto grouped = scene(
      {QJsonObject{
           {"id", "assembly"}, {"type", "group"}, {"width", 1}, {"height", 1}},
       QJsonObject{{"id", "child"},
                   {"type", "rect"},
                   {"parent", "assembly"},
                   {"x", .1},
                   {"width", .2},
                   {"height", .2},
                   {"fill", "#ffffff"}}});
  timeline = project.timeline();
  timeline.findClip(eid)->generator.data = grouped;
  project.applyEdit(timeline, "group fixture");
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        widget->findChild<QListWidget *>("graphicsLayers")->setCurrentRow(0);
        for (auto *button : widget->findChildren<QPushButton *>())
          if (button->text() == "Duplicate") {
            button->click();
            break;
          }
        qobject_cast<QDialog *>(widget)->accept();
      }
  });
  composeGraphics(&parent, &project, eid);
  auto duplicated = project.timeline().findClip(eid)->generator.data;
  check(duplicated["layers"].toArray().size() == 4 &&
            motion::validate(duplicated).isEmpty(),
        "duplicating a group copies its children and remaps their parents");
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        widget->findChild<QListWidget *>("graphicsLayers")->setCurrentRow(0);
        for (auto *button : widget->findChildren<QPushButton *>())
          if (button->text() == "Delete") {
            button->click();
            break;
          }
        qobject_cast<QDialog *>(widget)->accept();
      }
  });
  composeGraphics(&parent, &project, eid);
  auto deleted = project.timeline().findClip(eid)->generator.data;
  check(deleted["layers"].toArray().size() == 2 &&
            motion::validate(deleted).isEmpty(),
        "deleting a group removes its children without orphaned parents");
  return failures;
}
