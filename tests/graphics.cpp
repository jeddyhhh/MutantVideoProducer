#include "core/Graphics.h"
#include "core/Compositor.h"
#include "core/FrameRenderer.h"
#include "core/GpuCompositor.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "ui/GraphicsEditor.h"
#include <QApplication>
#include <QDataStream>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFile>
#include <QListWidget>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cmath>
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  int failures = 0;
  auto check = [&](bool ok, QString what) {
    QTextStream(stdout) << (ok ? "PASS " : "FAIL ") << what << Qt::endl;
    if (!ok)
      ++failures;
  };
  for (auto id : graphics::presets()) {
    auto s = graphics::scene(id);
    check(graphics::validateScene(s).isEmpty(), "valid preset " + id);
  }
  auto g = makeGenerator(*findGeneratorPreset("graphics_broadcast"));
  auto img = renderGenerator(g, {640, 360}, 1. / 3, 0, 10 * kUsPerSecond, {});
  check(qAlpha(img.pixel(500, 150)) == 0 && qAlpha(img.pixel(10, 10)) > 0,
        "broadcast keeps footage aperture transparent");
  check(generatorFromJson(generatorToJson(g)) == g,
        "graphics scene JSON round trip");
  auto bad = g.data;
  bad["layers"] = QJsonArray{QJsonObject{{"type", "script"}}};
  check(!graphics::validateScene(bad).isEmpty(),
        "unknown layer types rejected");
  QJsonObject moving{
      {"type", "rect"},
      {"x", QJsonObject{{"keyframes",
                         QJsonArray{QJsonArray{0, 0}, QJsonArray{2, .5}}}}},
      {"y", .2},
      {"width", .1},
      {"height", .1},
      {"fill", "#ff0000"}};
  g.data = {{"version", 1}, {"layers", QJsonArray{moving}}};
  check(renderGenerator(g, {640, 360}, 1, 0, 3 * kUsPerSecond, {}) !=
            renderGenerator(g, {640, 360}, 1, 2 * kUsPerSecond,
                            3 * kUsPerSecond, {}),
        "per-layer animation changes picture");
  bad = g.data;
  auto b = moving;
  b["x"] = QJsonObject{
      {"keyframes", QJsonArray{QJsonArray{2, 0}, QJsonArray{1, .5}}}};
  bad["layers"] = QJsonArray{b};
  check(!graphics::validateScene(bad).isEmpty(), "unsorted keyframes rejected");
  QTemporaryDir dir;
  QString audio = dir.path() + "/stereo.wav";
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
    wav << qint16(i < 48000 ? 0
                            : std::sin(i * 6.283185307 * 440 / 48000) * 8192)
        << qint16(i < 48000 ? 0
                            : std::sin(i * 6.283185307 * 220 / 48000) * 16384);
  }
  file.close();
  QString error;
  auto silence = graphics::buildWaveform(audio, 0, 0, 1, 128, &error),
       tone = graphics::buildWaveform(audio, 0, 1, 1, 128, &error);
  check(error.isEmpty() && graphics::validateWaveform(tone).isEmpty(),
        "decode actual stereo audio");
  double quiet = 0, l = 0, r = 0;
  for (auto v : silence["left"].toArray())
    quiet = std::max(quiet, v.toDouble());
  for (auto v : tone["left"].toArray())
    l = std::max(l, v.toDouble());
  for (auto v : tone["right"].toArray())
    r = std::max(r, v.toDouble());
  check(quiet == 0 && std::abs(l - .25) < .01 && std::abs(r - .5) < .01,
        "source offset and stereo peak magnitudes are correct");
  check(graphics::buildWaveform(
            audio, 0, 0, 1, 128, &error,
            [](double) {
              return false;
            }).isEmpty() &&
            error == "Cancelled",
        "waveform analysis can be cancelled");
  check(graphics::buildWaveform(audio, 0, 0, 3601, 128, &error).isEmpty(),
        "oversized audio range rejected");
  auto wave = makeGenerator(*findGeneratorPreset("audio_waveform"));
  wave.data = tone;
  auto filled=wave;filled.params["style"].value=1;
  auto filledImage=renderGenerator(filled,{640,360},1./3,500000,kUsPerSecond,{});
  auto lowerColour=filledImage.pixelColor(128,309);
  check(lowerColour.green()>160&&lowerColour.blue()>160,"filled envelope mirrors real peaks below the baseline");
  auto scrolling=wave;scrolling.params["scroll"].value=1;scrolling.params["window"].value=.2;scrolling.params["style"].value=2;
  check(!renderGenerator(scrolling,{640,360},1./3,500000,kUsPerSecond,{}).isNull(),"scrolling line style renders safely");
  auto first = renderGenerator(wave, {640, 360}, 1. / 3, 0, kUsPerSecond, {}),
       later =
           renderGenerator(wave, {640, 360}, 1. / 3, 500000, kUsPerSecond, {});
  check(first != later, "waveform playhead follows time");
  wave.params["progress"].value = 0;
  check(renderGenerator(wave, {640, 360}, 1. / 3, 0, kUsPerSecond, {}) ==
            renderGenerator(wave, {640, 360}, 1. / 3, 500000, kUsPerSecond, {}),
        "static waveform stays static");
  check(generatorFromJson(generatorToJson(wave)) == wave,
        "peak envelope saved without an external image");
  wave.data["right"] = QJsonArray{};
  check(!graphics::validateWaveform(wave.data).isEmpty(),
        "mismatched channels rejected");
  renderGenerator(wave, {640, 360}, 1. / 3, 0, kUsPerSecond, {});
  wave.data = tone;
  {
    auto surface = createGpuSurface();
    QString gpuError;
    auto gpu = surface ? GpuCompositor::create(surface.get(), false, &gpuError) : nullptr;
    if (QGuiApplication::platformName() != "offscreen")
      check(bool(gpu), "GPU available for project isolation regression");
    else if (!gpu)
      QTextStream(stdout) << "SKIP GPU isolation checks: offscreen platform has no OpenGL 3.3 context" << Qt::endl;
    for (const auto& effect : {"trails", "video_feedback", "stutter", "slit_scan"}) {
      ProjectSnapshot firstProject;
      firstProject.session = std::make_shared<const int>(0);
      firstProject.settings.width=160; firstProject.settings.height=90;
      firstProject.timeline=defaultTimeline();
      int clipId=ops::insertGenerated(firstProject.timeline,*findGeneratorPreset("solid_white"),0,-1,{});
      firstProject.timeline.findClip(clipId)->effects.push_back(makeEffectOfType(effect,900));
      ProjectSnapshot secondProject=firstProject;
      secondProject.session=std::make_shared<const int>(0);
      secondProject.timeline.findClip(clipId)->generator.params["color"].value=0;
      Compositor reused(false), fresh(false);
      const TimeUs frame=FrameRate{30,1}.timeOf(1);
      reused.render(firstProject,0,{160,90});reused.render(firstProject,frame,{160,90});
      auto actual=reused.render(secondProject,frame,{160,90});
      auto expected=fresh.render(secondProject,frame,{160,90});
      check(actual==expected,QString("CPU %1 clears old project frames").arg(effect));
      if(gpu) {
        gpu->render(firstProject,0,{160,90});gpu->render(firstProject,frame,{160,90});
        auto result=gpu->render(secondProject,frame,{160,90});
        auto clean=GpuCompositor::create(surface.get(),false,&gpuError);
        check(clean && result==clean->render(secondProject,frame,{160,90}),QString("GPU %1 clears old project frames").arg(effect));
      }
      // Editing within the same session must retain the intended echo history.
      Compositor sameSession(false);
      sameSession.render(firstProject,0,{160,90});sameSession.render(firstProject,frame,{160,90});
      auto edit=secondProject;edit.session=firstProject.session;
      auto held=sameSession.render(edit,frame,{160,90});
      if(QString(effect)=="trails") check(held!=expected,"ordinary edits preserve intentional temporal effect history");
    }
  }
  Project project;
  Timeline tl = project.timeline();
  int id = ops::insertGenerated(
      tl, *findGeneratorPreset("graphics_lower_third"), 0, -1, {});
  project.applyEdit(tl, "fixture");
  Compositor cpu;
  auto before = cpu.render(*project.snapshot(), 0, {640, 360});
  auto changed = project.timeline();
  auto scene = changed.findClip(id)->generator.data;
  auto layers = scene["layers"].toArray();
  auto layer = layers[0].toObject();
  layer["fill"] = "#ff0000";
  layers[0] = layer;
  scene["layers"] = layers;
  changed.findClip(id)->generator.data = scene;
  project.applyEdit(changed, "change scene");
  auto after = cpu.render(*project.snapshot(), 0, {640, 360});
  check(before != after, "CPU cache invalidates for scene-only edits");
  project.undoStack()->undo();
  check(cpu.render(*project.snapshot(), 0, {640, 360}) == before,
        "graphics edits undo exactly");
  project.undoStack()->redo();
  auto surface = createGpuSurface();
  auto gpu =
      surface ? GpuCompositor::create(surface.get(), true, &error) : nullptr;
  if(QGuiApplication::platformName()!="offscreen")check(bool(gpu), "GPU renderer available");
  if (gpu) {
    auto ga = gpu->render(*project.snapshot(), 0, {640, 360});
    project.undoStack()->undo();
    auto gb = gpu->render(*project.snapshot(), 0, {640, 360});
    check(ga != gb, "GPU cache invalidates for scene-only edits");
    auto a = gb.convertToFormat(QImage::Format_RGBA8888),
         b = before.convertToFormat(QImage::Format_RGBA8888);
    double delta = 0;
    for (int y = 0; y < a.height(); ++y)
      for (int x = 0; x < a.width() * 4; ++x)
        delta +=
            std::abs(int(a.constScanLine(y)[x]) - int(b.constScanLine(y)[x]));
    check(delta / a.width() / a.height() / 4 < 3,
          "CPU/GPU graphic composition parity");
  }
  tl = project.timeline();
  int waveId = ops::insertGenerated(tl, *findGeneratorPreset("audio_waveform"),
                                    0, -1, {});
  tl.findClip(waveId)->generator = wave;
  project.applyEdit(tl, "waveform");
  auto wavePicture=cpu.render(*project.snapshot(),500000,{640,360});
  auto quietTimeline=project.timeline();quietTimeline.findClip(waveId)->generator.data=silence;
  project.applyEdit(quietTimeline,"quiet waveform");
  check(cpu.render(*project.snapshot(),500000,{640,360})!=wavePicture,"CPU cache invalidates when only audio peaks change");
  project.undoStack()->undo();
  if(gpu){auto waveGpu=gpu->render(*project.snapshot(),500000,{640,360}).convertToFormat(QImage::Format_RGBA8888);auto waveCpu=wavePicture.convertToFormat(QImage::Format_RGBA8888);double difference=0;for(int y=0;y<waveCpu.height();++y)for(int x=0;x<waveCpu.width()*4;++x)difference+=std::abs(int(waveCpu.constScanLine(y)[x])-int(waveGpu.constScanLine(y)[x]));check(difference/waveCpu.width()/waveCpu.height()/4<3,"CPU/GPU waveform composition parity");QTextStream(stdout)<<"Wave difference "<<difference/waveCpu.width()/waveCpu.height()/4<<Qt::endl;waveGpu.save("creative-test/gpu/wave-gpu.png");waveCpu.save("creative-test/gpu/wave-cpu.png");}
  QString saved = dir.path() + "/graphics.mvp";
  check(project.save(saved, &error), "save native graphics project");
  Project loaded;
  check(loaded.load(saved, &error) && loaded.timeline() == project.timeline(),
        "project reload retains graphics and peaks");
  // Exercise the real designer's edit/apply/cancel path without a desktop
  // automation dependency.
  if(app.arguments().contains("--gpu-only"))return failures;
  auto original = project.timeline();
  QWidget hiddenParent;hiddenParent.setAttribute(Qt::WA_DontShowOnScreen);
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        auto *list = widget->findChild<QListWidget *>("graphicsLayers");
        list->setCurrentRow(2);
        widget->findChild<QPlainTextEdit *>("graphicsLayerText")
            ->setPlainText("Edited natively");
        auto* canvas=widget->findChild<QWidget*>("graphicsCanvas");
        double height=std::min(double(canvas->height()),canvas->width()*9./16.);double width=height*16./9.;
        QPointF center((canvas->width()-width)/2+width*(.07+.85/2),(canvas->height()-height)/2+height*(.76+.075/2));
        QMouseEvent press(QEvent::MouseButtonPress,center,canvas->mapToGlobal(center),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);QApplication::sendEvent(canvas,&press);
        // Alt requests the exact free-drag offset instead of snapping to the safe guides.
        QMouseEvent move(QEvent::MouseMove,center+QPointF(width*.04,0),canvas->mapToGlobal(center+QPointF(width*.04,0)),Qt::NoButton,Qt::LeftButton,Qt::AltModifier);QApplication::sendEvent(canvas,&move);
        QMouseEvent release(QEvent::MouseButtonRelease,center+QPointF(width*.04,0),canvas->mapToGlobal(center+QPointF(width*.04,0)),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(canvas,&release);
        if(!qEnvironmentVariable("MVP_TEST_ARTIFACTS").isEmpty())widget->grab().save(qEnvironmentVariable("MVP_TEST_ARTIFACTS")+"/composer.png");
        qobject_cast<QDialog *>(widget)->accept();
      }
  });
  composeGraphics(&hiddenParent, &project, id);
  check(project.timeline() != original,
        "visual composer Apply commits an edit");
  check(std::abs(project.timeline().findClip(id)->generator.data["layers"].toArray()[2].toObject()["x"].toDouble()-.11)<.0001,"dragging a layer updates its saved position");
  auto applied = project.timeline();
  QTimer::singleShot(0, [&] {
    for (auto *widget : QApplication::topLevelWidgets())
      if (widget->objectName() == "graphicsComposer") {
        widget->findChild<QPlainTextEdit *>("graphicsLayerText")
            ->setPlainText("Discard this");
        qobject_cast<QDialog *>(widget)->reject();
      }
  });
  composeGraphics(&hiddenParent, &project, id);
  check(project.timeline() == applied,
        "visual composer Cancel discards changes");
  // In a 4:3 project the canvas is 4:3: a drag of 4% of its picture moves the layer by 0.04.
  {
    const auto before = project.settings();
    auto square = before;
    square.width = 1440;
    square.height = 1080;
    project.setSettings(square);
    const double x0 = project.timeline().findClip(id)->generator.data["layers"].toArray()[2].toObject()["x"].toDouble();
    QTimer::singleShot(0, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "graphicsComposer") {
          widget->findChild<QListWidget *>("graphicsLayers")->setCurrentRow(2);
          auto *canvas = widget->findChild<QWidget *>("graphicsCanvas");
          double height = std::min(double(canvas->height()), canvas->width() * 3. / 4.), width = height * 4. / 3.;
          QPointF center((canvas->width() - width) / 2 + width * (x0 + .85 / 2), (canvas->height() - height) / 2 + height * (.76 + .075 / 2));
          QPointF to = center + QPointF(width * .04, 0);
          QMouseEvent press(QEvent::MouseButtonPress, center, canvas->mapToGlobal(center), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
          QApplication::sendEvent(canvas, &press);
          QMouseEvent move(QEvent::MouseMove, to, canvas->mapToGlobal(to), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
          QApplication::sendEvent(canvas, &move);
          QMouseEvent release(QEvent::MouseButtonRelease, to, canvas->mapToGlobal(to), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
          QApplication::sendEvent(canvas, &release);
          qobject_cast<QDialog *>(widget)->accept();
        }
    });
    composeGraphics(&hiddenParent, &project, id);
    const double x1 = project.timeline().findClip(id)->generator.data["layers"].toArray()[2].toObject()["x"].toDouble();
    check(std::abs(x1 - x0 - .04) < .0001, QString("the composer canvas takes the project's shape (4:3 drag moved %1)").arg(x1 - x0));
    project.undoStack()->undo();
    project.setSettings(before);
  }
  project.undoStack()->undo();
  check(project.timeline() == original, "composer edits are one undo step");
  project.addMedia(probeMedia(audio));
  QTimer::singleShot(0,[&]{for(auto* widget:QApplication::topLevelWidgets())if(widget->objectName()=="waveformSourceDialog"){
      widget->findChild<QDoubleSpinBox*>("waveformStart")->setValue(1);
      widget->findChild<QDoubleSpinBox*>("waveformSpan")->setValue(1);
      qobject_cast<QDialog*>(widget)->accept();}});
  chooseWaveformAudio(&hiddenParent,&project,waveId);
  check(project.timeline().findClip(waveId)->generator.data["source_media_id"].toInt()>0,"source picker builds and binds audio waveform");
  auto waveformApplied=project.timeline();
  QTimer::singleShot(0,[&]{for(auto* widget:QApplication::topLevelWidgets())if(widget->objectName()=="waveformSourceDialog")qobject_cast<QDialog*>(widget)->reject();});
  chooseWaveformAudio(&hiddenParent,&project,waveId);
  check(project.timeline()==waveformApplied,"waveform source picker Cancel preserves event");
  return failures;
}
