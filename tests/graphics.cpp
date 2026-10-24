#include "core/Graphics.h"
#include "core/MotionGraphics.h"
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/FrameRenderer.h"
#include "core/GpuCompositor.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "ui/GraphicsEditor.h"
#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QPushButton>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
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
  // Animation is its own clip type: motion presets make one, the broadcast layouts stay Graphics Overlays.
  {
    const auto *anim = findGeneratorPreset("animation_characters"), *overlay = findGeneratorPreset("graphics_broadcast");
    check(anim && anim->generator == "animation" && graphics::sceneType(makeGenerator(*anim).data) == "animation" && overlay
              && overlay->generator == "graphics_overlay" && graphics::sceneType(makeGenerator(*overlay).data) == "graphics_overlay"
              && findGeneratorPreset("animation_blank") && !findGeneratorPreset("graphics_motion_characters"),
          "motion presets make Animation clips, broadcast presets Graphics Overlays");
  }
  // The characters preset: valid, character A's mouth follows voice_a, and a colour variable recolours a part.
  {
    QJsonObject chars = graphics::scene("motion_characters");
    check(graphics::validateScene(chars).isEmpty() && chars["variables"].toObject().contains("a_jacket"),
          "characters preset is valid with colour variables");
    auto picture = [](const QJsonObject &scene) {
      QImage image(640, 360, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      motion::render(image, scene, 0.5);
      return image;
    };
    const QImage silent = picture(chars);
    QJsonObject loud = chars;
    loud["audio"] = QJsonObject{{"voice_a", QJsonObject{{"version", 1}, {"duration", 1.0}, {"left", QJsonArray{1.0, 1.0}},
                                                        {"right", QJsonArray{1.0, 1.0}}, {"rate", 1}, {"offset", 0}}}};
    check(picture(loud) != silent, "characters preset: a loud voice_a moves character A");
    QDir().mkpath("creative-test");
    picture(loud).save("creative-test/characters.png");
    QJsonObject recoloured = chars;
    auto vars = recoloured["variables"].toObject();
    vars["a_jacket"] = "#00ff00";
    recoloured["variables"] = vars;
    check(picture(recoloured) != silent, "characters preset: the a_jacket variable recolours character A");
  }
  // The character library: a kid from it added twice, its mouth taking the shape of the words.
  {
    const auto library = graphics::characterLibrary();
    const auto kid = std::find_if(library.begin(), library.end(), [](const graphics::CharacterFile &f) { return f.path.endsWith("kid.json"); });
    check(library.size() >= 9 && kid != library.end() && kid->builtIn, QStringLiteral("the character library comes with the program (%1)").arg(library.size()));
    // Arms (and what is in their hands) in front of the body. (Terry's stick out at his sides.)
    for (const auto &c : library) {
      if (!c.builtIn || c.character["kind"] == "towel")
        continue;
      int arm = -1, body = -1, i = 0;
      for (auto v : c.character["layers"].toArray()) {
        const QString id = v.toObject()["id"].toString();
        if (id == "armR")
          arm = i;
        if (id == "jacket" || id == "torso" || id == "body")
          body = i;
        ++i;
      }
      check(arm > body, QStringLiteral("%1 holds its arms in front of its body").arg(c.character["name"].toString()));
    }
    const auto towel = std::find_if(library.begin(), library.end(), [](const graphics::CharacterFile &f) { return f.path.endsWith("towel.json"); });
    if (towel != library.end()) {
      QString error;
      const QJsonObject terry = motion::withCharacter(QJsonObject{{"version", 2}, {"layers", QJsonArray{}}}, towel->character, "Terry", .4, .9, &error);
      check(error.isEmpty() && motion::validate(terry).isEmpty() && terry["variables"].toObject().contains("terry_towel")
                && terry["variables"].toObject().contains("terry_mood"),
            QStringLiteral("Terry the towel joins a scene with his colours and expressions %1").arg(error));
    } else {
      check(false, "Terry the towel is in the character library");
    }
    if (kid != library.end()) {
      QString error;
      QJsonObject scene{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "rect"}, {"id", "bg"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1},
                                                                       {"fill", "#8ecae6"}}}}};
      scene = motion::withCharacter(scene, kid->character, "Gus", .2, .9, &error);
      scene = motion::withCharacter(scene, kid->character, "Gus", .6, .9, &error);
      auto find = [&](const QString &id) {
        for (auto v : scene["layers"].toArray())
          if (v.toObject()["id"] == id)
            return v.toObject();
        return QJsonObject{};
      };
      check(error.isEmpty() && !find("gus").isEmpty() && !find("gus_2_mouth_O").isEmpty() && scene["variables"].toObject().contains("gus_jacket")
                && scene["variables"].toObject().contains("gus_2_jacket") && motion::validate(scene).isEmpty(),
            QStringLiteral("a character joins a scene under its name, a second one numbered %1").arg(error));
      QJsonArray loud;
      for (int i = 0; i < 64; ++i)
        loud.append(1);
      scene["audio"] = QJsonObject{{"gus", QJsonObject{{"version", 1}, {"duration", 2}, {"left", loud}, {"right", loud}, {"rate", 1}, {"offset", 0},
                                                       {"visemes", QJsonArray{QJsonArray{0, "O"}, QJsonArray{1, "M"}}}}}};
      auto at = [&](const QString &id, double t) { return motion::evaluate(find(id)["opacity"], t, scene).toDouble(); };
      check(at("gus_mouth_O", .5) == 1 && at("gus_mouth_A", .5) == 0 && at("gus_mouth_shut_m", 1.5) == 1 && at("gus_mouth_O", 1.5) == 0,
            "the mouth takes the shape of the words: O, then closed for M");
      QImage o(640, 360, QImage::Format_ARGB32_Premultiplied), m(640, 360, QImage::Format_ARGB32_Premultiplied);
      o.fill(Qt::transparent);
      m.fill(Qt::transparent);
      motion::render(o, scene, .5);
      motion::render(m, scene, 1.5);
      check(o != m, "the mouth shapes are drawn");
      o.save("creative-test/character-o.png");
      const QJsonObject saved = motion::characterFrom(scene, "gus", "Gus copy", &error);
      check(saved["layers"].toArray().size() == kid->character["layers"].toArray().size() && saved["binding"] == "gus" &&
                saved["layers"].toArray()[0].toObject()["id"] == "root" && saved["variables"].toObject().contains("jacket"),
            QStringLiteral("a scene's character saves back as a library character %1").arg(error));
      const QJsonObject again = motion::withCharacter(scene, saved, "Mo", .4, .9, &error);
      bool moBinding = false;
      for (auto v : again["layers"].toArray())
        if (v.toObject()["id"] == "mo_mouth_O")
          moBinding = v.toObject()["opacity"].toObject()["audio"] == "mo";
      check(moBinding, "a saved character added again speaks under its new name");
      // Moves: a hop rises and lands, a walk ends where it was sent.
      const QJsonObject hop = motion::withMove(scene, "gus", "hop", 1, 0, 1, NAN, &error);
      auto y = [&](const QJsonObject &s, double t) {
        for (auto v : s["layers"].toArray())
          if (v.toObject()["id"] == "gus")
            return motion::evaluate(v.toObject()["y"], t, s).toDouble();
        return 0.;
      };
      check(!hop.isEmpty() && y(hop, 1.25) < y(hop, 1) - .03 && std::abs(y(hop, 1.5) - y(hop, 1)) < 1e-6, "a hop goes up and lands where it was");
      const QJsonObject walk = motion::withMove(scene, "gus", "walk", 0, 2, 1, .7, &error);
      double xEnd = 0;
      for (auto v : walk["layers"].toArray())
        if (v.toObject()["id"] == "gus")
          xEnd = motion::evaluate(v.toObject()["x"], 2, walk).toDouble();
      check(std::abs(xEnd - .7) < 1e-6 && motion::withMove(scene, "gus", "moonwalk", 0, 0, 1, NAN, &error).isEmpty(),
            "a walk ends where it was sent; an unknown move is refused");
      // A pop on a character whose scale breathes in a loop: it happens once (the loop stops for it), and no key is null.
      const QJsonObject popped = motion::withMove(scene, "gus", "pop", 1.15, 0, 1, NAN, &error);
      auto gus = [](const QJsonObject &s) {
        for (auto v : s["layers"].toArray())
          if (v.toObject()["id"] == "gus")
            return v.toObject();
        return QJsonObject{};
      };
      auto scaleY = [&](double t) { return motion::evaluate(gus(popped)["scale_y"], t, popped).toDouble(-1); };
      bool nulls = false;
      for (const char *p : {"scale_x", "scale_y"})
        for (auto k : gus(popped)[p].toObject()["keyframes"].toArray())
          nulls = nulls || k.toArray()[1].isNull();
      check(!popped.isEmpty() && gus(scene)["scale_y"].toObject().contains("loop") && scaleY(1.17) < .5 && std::abs(scaleY(5) - scaleY(9)) < 1e-6
                && scaleY(9) > .9 && !nulls,
            QStringLiteral("a pop on a breathing character happens once, with no empty keys (%1 at 1.17 s, %2 and %3 later)")
                .arg(scaleY(1.17)).arg(scaleY(5)).arg(scaleY(9)));
    }
  }
  // The look: shading, blur, depth of field, contact shadows and the scene's motion blur.
  {
    auto drawn = [](const QJsonObject &s, double t = 0) {
      QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      motion::render(image, s, t);
      return image;
    };
    auto box = [](QJsonObject extra = {}) {
      QJsonObject o{{"type", "rect"}, {"id", "box"}, {"x", .3}, {"y", .3}, {"width", .4}, {"height", .4}, {"fill", "#808080"}};
      for (auto it = extra.begin(); it != extra.end(); ++it)
        o[it.key()] = it.value();
      return QJsonObject{{"version", 2}, {"layers", QJsonArray{o}}};
    };
    const QImage flat = drawn(box()), shaded = drawn(box({{"shading", .8}}));
    check(qGray(shaded.pixel(100, 60)) > qGray(flat.pixel(100, 60)) && qGray(shaded.pixel(220, 120)) < qGray(flat.pixel(220, 120)),
          "shading lights the top left and shadows the bottom right");
    const QImage soft = drawn(box({{"blur", .03}}));
    check(qAlpha(soft.pixel(96, 90)) > 0 && qAlpha(soft.pixel(96, 90)) < 255 && qAlpha(flat.pixel(94, 90)) == 0, "a blurred layer has soft edges");
    QJsonObject far = box({{"depth", 1}});
    far["camera"] = QJsonObject{{"zoom", 2}, {"depth_of_field", 3}};
    QJsonObject near = far;
    auto layers = near["layers"].toArray();
    auto o = layers[0].toObject();
    o["depth"] = 0;
    layers[0] = o;
    near["layers"] = layers;
    const QImage farImage = drawn(far), nearImage = drawn(near);
    int softFar = 0, softNear = 0;
    for (int x = 0; x < 320; ++x) {
      softFar += qAlpha(farImage.pixel(x, 90)) > 0 && qAlpha(farImage.pixel(x, 90)) < 250;
      softNear += qAlpha(nearImage.pixel(x, 90)) > 0 && qAlpha(nearImage.pixel(x, 90)) < 250;
    }
    check(softFar > softNear + 4, QStringLiteral("depth of field blurs a far layer as the camera zooms in (%1 soft pixels, near %2)").arg(softFar).arg(softNear));
    const QImage grounded = drawn(box({{"contact_shadow", 1}, {"fill", "#00000000"}, {"stroke", "#000000"}, {"stroke_width", .002}}));
    check(qAlpha(grounded.pixel(160, 128)) > 20 && qAlpha(grounded.pixel(160, 90)) == 0, "a contact shadow sits under the layer's box");
    QJsonObject moving = box({{"x", QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, .6}}}}}});
    QJsonObject smeared = moving;
    smeared["motion_blur"] = QJsonObject{{"samples", 6}, {"shutter", .1}};
    const QImage sharp = drawn(moving, .5), blurredFrame = drawn(smeared, .5);
    int partial = 0;
    for (int x = 0; x < 320; ++x)
      partial += qAlpha(blurredFrame.pixel(x, 90)) > 10 && qAlpha(blurredFrame.pixel(x, 90)) < 245;
    check(sharp != blurredFrame && partial > 8 && blurredFrame == drawn(smeared, .5),
          QStringLiteral("scene motion blur smears what moves, the same every time (%1 smeared pixels)").arg(partial));
  }
  // Video layers, a layer's own effects, the speaker camera, expressions, gestures and sets.
  {
    auto drawn = [](const QJsonObject &s, double t = 0) {
      QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      motion::render(image, s, t);
      return image;
    };
    auto rect = [](const QString &id, double x, double y, double w, double h, const QString &fill) {
      return QJsonObject{{"type", "rect"}, {"id", id}, {"x", x}, {"y", y}, {"width", w}, {"height", h}, {"fill", fill}};
    };
    // A second of red video, made by the exporter, plays inside a layer's box.
    QTemporaryDir videoDir;
    Project source;
    ProjectSettings small;
    small.width = 320;
    small.height = 180;
    source.setSettings(small);
    Timeline vt = source.timeline();
    Track track;
    track.id = vt.allocId();
    track.type = TrackType::Video;
    Clip red;
    red.id = vt.allocId();
    red.length = kUsPerSecond;
    red.generator = makeGenerator(*findGeneratorPreset("graphics_blank"));
    red.generator.data = QJsonObject{{"version", 2}, {"layers", QJsonArray{rect("r", 0, 0, 1, 1, "#ff0000")}}};
    track.clips.push_back(red);
    vt.tracks = {track};
    source.applyEdit(vt, "red");
    const ExportPreset *mp4 = nullptr;
    for (const auto &preset : exportPresets())
      if (!mp4 && preset.extension == "mp4" && presetAvailable(preset))
        mp4 = &preset;
    Exporter exporter;
    ExportSettings es = mp4 ? settingsFromPreset(*mp4, source.settings()) : ExportSettings{};
    es.path = videoDir.filePath("red.mp4");
    es.rangeStart = 0;
    es.rangeEnd = kUsPerSecond;
    check(mp4 && exporter.run(*source.snapshot(), es), "a test video is made: " + exporter.error());
    QJsonObject video{{"type", "video"}, {"id", "tv"}, {"x", .25}, {"y", .25}, {"width", .5}, {"height", .5}, {"path", es.path}, {"loop", true}};
    const QImage played = drawn(QJsonObject{{"version", 2}, {"layers", QJsonArray{video}}}, .5);
    const QRgb inside = played.pixel(160, 90);
    check(qRed(inside) > 200 && qGreen(inside) < 60 && qAlpha(played.pixel(20, 20)) == 0 && motion::validate(QJsonObject{{"version", 2}, {"layers", QJsonArray{video}}}).isEmpty(),
          "a video layer plays its file inside its box");
    QJsonObject remapped = video;
    remapped["source_time"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0.2}, QJsonArray{10, 0.6}}}};
    remapped["loop"] = false;
    const QImage late = drawn(QJsonObject{{"version", 2}, {"layers", QJsonArray{remapped}}}, 9.0);
    check(qRed(late.pixel(160, 90)) > 200, "a video layer's source_time picks the moment of the file it shows");
    // A layer's effects change only that layer.
    QJsonObject inverted = rect("a", 0, 0, .5, 1, "#ffffff");
    inverted["effects"] = QJsonArray{QJsonObject{{"type", "invert"}}};
    const QImage two = drawn(QJsonObject{{"version", 2}, {"layers", QJsonArray{inverted, rect("b", .5, 0, .5, 1, "#ffffff")}}});
    check(qGray(two.pixel(80, 90)) < 30 && qGray(two.pixel(240, 90)) > 225, "a layer's effect changes that layer only");
    QJsonObject screen = rect("tv", .3, .3, .4, .4, "#ff0000");
    screen["effects"] = QJsonArray{QJsonObject{{"type", "crt"}}, QJsonObject{{"type", "noise"}}};
    const QImage tv = drawn(QJsonObject{{"version", 2}, {"layers", QJsonArray{screen}}});
    int outside = 0;
    for (int y = 0; y < 180; y += 3)
      for (int x = 0; x < 320; x += 3)
        outside += (x < 90 || x > 230 || y < 50 || y > 130) && qAlpha(tv.pixel(x, y)) != 0;
    check(outside == 0 && qRed(tv.pixel(160, 90)) > 150, QStringLiteral("a layer's effects stay inside its box (%1 pixels outside)").arg(outside));
    // The speaker camera frames whoever is loudest.
    QJsonArray loud, quiet;
    for (int i = 0; i < 64; ++i) {
      loud.append(1);
      quiet.append(0);
    }
    auto envelope = [](const QJsonArray &a) {
      return QJsonObject{{"version", 1}, {"duration", 2}, {"left", a}, {"right", a}, {"rate", 1}, {"offset", 0}};
    };
    QJsonObject talk{{"version", 2}, {"layers", QJsonArray{rect("Ann", .05, .3, .3, .5, "#00ff00"), rect("Bob", .65, .3, .3, .5, "#0000ff")}},
                     {"audio", QJsonObject{{"ann", envelope(quiet)}, {"bob", envelope(loud)}}}};
    const QImage wide = drawn(talk, 1);
    talk["camera"] = QJsonObject{{"speaker", QJsonObject{{"zoom", 2}}}};
    const QImage close = drawn(talk, 1);
    check(qAlpha(wide.pixel(160, 90)) == 0 && qBlue(close.pixel(160, 90)) > 200 && qGreen(close.pixel(160, 90)) < 60,
          "the speaker camera frames whoever is talking");
    // A pause in a line keeps the close-up; after the last line, the wide shot.
    QJsonArray paused, ended;
    for (int i = 0; i < 64; ++i) {
      paused.append(i < 29 || i >= 52 ? 1 : 0); // (talking to 0.9 s, again from 1.6 s)
      ended.append(i < 29 ? 1 : 0);
    }
    QJsonObject pause = talk;
    pause["audio"] = QJsonObject{{"ann", envelope(quiet)}, {"bob", envelope(paused)}};
    const QImage held = drawn(pause, 1.55);
    pause["audio"] = QJsonObject{{"ann", envelope(quiet)}, {"bob", envelope(ended)}};
    const QImage after = drawn(pause, 1.55);
    check(qBlue(held.pixel(160, 90)) > 200 && qAlpha(after.pixel(160, 90)) == 0,
          QStringLiteral("the speaker camera holds its close-up through a pause, and goes wide after the last line (%1, %2)")
              .arg(held.pixel(160, 90), 0, 16).arg(after.pixel(160, 90), 0, 16));
    // Under motion blur the speaker camera still cuts: every moment of a frame frames the same speaker.
    QJsonArray first, second;
    for (int i = 0; i < 64; ++i) {
      first.append(i < 32 ? 1 : 0);
      second.append(i < 32 ? 0 : 1);
    }
    QJsonObject turns = talk;
    turns["audio"] = QJsonObject{{"ann", envelope(first)}, {"bob", envelope(second)}};
    double cut = -1;
    for (double t = 1; t < 1.6 && cut < 0; t += .01)
      if (drawn(turns, t).pixel(160, 90) != drawn(turns, 1).pixel(160, 90))
        cut = t;
    QJsonObject turnsBlurred = turns;
    turnsBlurred["motion_blur"] = QJsonObject{{"samples", 6}, {"shutter", .1}};
    check(cut > 0 && drawn(turnsBlurred, cut) == drawn(turns, cut) && drawn(turnsBlurred, cut - .02) == drawn(turns, cut - .02),
          QStringLiteral("the speaker camera cuts cleanly under motion blur (cut at %1 s)").arg(cut));
    // Its shots: a reaction (the listener), or the keyed wide shot, for a while. (Ann talks for a second, then Bob for
    // three: long enough that no framing is a glimpse.)
    QJsonObject shots = turns;
    {
      QJsonArray first, second;
      for (int i = 0; i < 64; ++i) {
        first.append(i < 16 ? 1 : 0);
        second.append(i < 16 ? 0 : 1);
      }
      auto four = [](const QJsonArray &a) {
        return QJsonObject{{"version", 1}, {"duration", 4}, {"left", a}, {"right", a}, {"rate", 1}, {"offset", 0}};
      };
      shots["audio"] = QJsonObject{{"ann", four(first)}, {"bob", four(second)}};
    }
    shots["camera"] = QJsonObject{{"speaker", QJsonObject{{"zoom", 2}, {"shots", QJsonArray{QJsonArray{2.0, 2.6, "listener"}, QJsonArray{2.6, 3.2, "wide"}}}}}};
    const QImage onBob = drawn(shots, 1.7), reaction = drawn(shots, 2.3), stillWide = drawn(shots, 2.9), bobAgain = drawn(shots, 3.6);
    check(qBlue(onBob.pixel(160, 90)) > 200 && qGreen(reaction.pixel(160, 90)) > 200 && qAlpha(stillWide.pixel(160, 90)) == 0
              && qBlue(bobAgain.pixel(160, 90)) > 200,
          "the speaker camera's shots: a reaction of the listener, then the wide shot, then the speaker again");
    // A hand-over: in the gap between two lines the shot stays on whoever was heard last, so the next speaker's first
    // syllables (louder, then quieter than the end of the line before) don't flick the camera back and forth.
    {
      QJsonArray ann, bob;
      for (int i = 0; i < 64; ++i) {
        ann.append(i < 16 ? .12 : 0);                     // a soft end of a line at 0.5 s (64 bins over 2 s)
        bob.append(i == 26 ? .2 : i >= 40 ? 1. : 0);      // a short sound at 0.8 s, then the next line at 1.25 s
      }
      QJsonObject handover = talk;
      handover["audio"] = QJsonObject{{"ann", envelope(ann)}, {"bob", envelope(bob)}};
      QString seen;
      for (double t = .3; t < 1.95; t += 1. / 24) {
        const QRgb c = drawn(handover, t).pixel(160, 90);
        const QChar who = qBlue(c) > 200 && qGreen(c) < 60 ? 'B' : qGreen(c) > 200 ? 'A' : '-';
        if (seen.isEmpty() || seen.back() != who)
          seen += who;
      }
      check(seen == "AB", QStringLiteral("at a hand-over the speaker camera cuts once (%1)").arg(seen));
    }
    // No glimpses: as a reaction shot of Bob ends, Ann's line has just finished and Bob speaks next; the camera stays on
    // Bob instead of cutting to Ann for a frame or two.
    {
      QJsonArray ann, bob;
      for (int i = 0; i < 64; ++i) {
        bob.append(i <= 12 || i >= 46 ? 1 : 0); // 0 to 0.4 s, and from 1.45 s
        ann.append(i >= 16 && i <= 35 ? 1 : 0); // 0.5 to 1.1 s
      }
      QJsonObject reaction = talk;
      reaction["audio"] = QJsonObject{{"ann", envelope(ann)}, {"bob", envelope(bob)}};
      reaction["camera"] = QJsonObject{{"speaker", QJsonObject{{"zoom", 2}, {"shots", QJsonArray() << QJsonArray{0.5, 1.3, "listener"}}}}};
      QString seen;
      for (double t = .6; t < 1.95; t += 1. / 24) {
        const QRgb c = drawn(reaction, t).pixel(160, 90);
        const QChar who = qBlue(c) > 200 && qGreen(c) < 60 ? 'B' : qGreen(c) > 200 ? 'A' : '-';
        if (seen.isEmpty() || seen.back() != who)
          seen += who;
      }
      check(seen == "B", QStringLiteral("no glimpse of the last speaker as a reaction shot ends (%1)").arg(seen));
    }
    // A two-shot keeps the same two people for its whole length: two people who spoke by turns before it (so either
    // could be the listener at a given moment) and a pause in the speaker's line don't move the camera.
    {
      QJsonArray ann, bob, cy;
      for (int i = 0; i < 64; ++i) {
        ann.append(i < 48 && i % 2 == 0 ? 1 : 0);
        bob.append(i < 48 && i % 2 == 1 ? 1 : 0);
        cy.append(i >= 48 && (i < 54 || i > 57) ? 1 : 0);
      }
      auto four = [](const QJsonArray &a) {
        return QJsonObject{{"version", 1}, {"duration", 4}, {"left", a}, {"right", a}, {"rate", 1}, {"offset", 0}};
      };
      QJsonObject two{{"version", 2},
                      {"layers", QJsonArray{rect("Ann", .02, .3, .2, .5, "#00ff00"), rect("Bob", .38, .3, .2, .5, "#0000ff"),
                                            rect("Cy", .74, .3, .2, .5, "#ff0000")}},
                      {"audio", QJsonObject{{"ann", four(ann)}, {"bob", four(bob)}, {"cy", four(cy)}}},
                      {"camera", QJsonObject{{"speaker", QJsonObject{{"zoom", 2}, {"shots", QJsonArray() << QJsonArray{3.0, 4.0, "two"}}}}}}};
      const QImage first = drawn(two, 3.1);
      int moved = 0;
      for (double t = 3.1; t < 3.9; t += 1. / 24)
        moved += drawn(two, t) != first;
      check(moved == 0 && first != drawn(two, 2.0), QStringLiteral("a two-shot holds its framing for the whole shot (%1 frames moved)").arg(moved));
    }
    // A library character's expression and gestures.
    const auto library = graphics::characterLibrary();
    const auto kid = std::find_if(library.begin(), library.end(), [](const graphics::CharacterFile &f) { return f.path.endsWith("kid.json"); });
    if (kid != library.end()) {
      QString error;
      QJsonObject scene{{"version", 2}, {"layers", QJsonArray{rect("bg", 0, 0, 1, 1, "#8ecae6")}}};
      scene = motion::withCharacter(scene, kid->character, "Gus", .4, .9, &error);
      const QJsonObject angry = motion::withExpression(scene, "gus", "angry", 1, &error);
      int shown = 0;
      for (auto v : angry["layers"].toArray()) {
        const QJsonObject o = v.toObject();
        if (o["opacity"].toObject()["equals"] == "angry")
          shown += 1 + 10 * (motion::evaluate(o["opacity"], .5, angry).toDouble() != 0) + 100 * (motion::evaluate(o["opacity"], 1.5, angry).toDouble() != 1);
      }
      check(!angry.isEmpty() && shown > 0 && shown < 10 && motion::withExpression(scene, "bg", "angry", 1, &error).isEmpty(),
            QStringLiteral("an expression shows from its time on, only on a character (%1)").arg(shown));
      check(drawn(angry, .5) != drawn(angry, 1.5), "an expression changes the face");
      // Props: one in Gus's hand hangs from his arm, so it goes up when he points.
      const auto props = graphics::propLibrary();
      const auto phone = std::find_if(props.begin(), props.end(), [](const graphics::CharacterFile &f) { return QFileInfo(f.path).completeBaseName() == "phone"; });
      check(props.size() >= 8 && phone != props.end(), QStringLiteral("the prop library comes with the program (%1)").arg(props.size()));
      if (phone != props.end()) {
        const QJsonObject held = motion::withProp(scene, phone->character, "gus", "R", &error);
        const QJsonObject pointing = motion::withMove(held, "gus", "point", 1, 0, 1, NAN, &error);
        QJsonObject root;
        for (auto v : held["layers"].toArray())
          if (v.toObject()["id"] == "gus_phone")
            root = v.toObject();
        const QRectF before = motion::bounds(pointing, "gus_phone", 0.5, QSizeF(1, 1)), up = motion::bounds(pointing, "gus_phone", 1.8, QSizeF(1, 1));
        check(root["parent"] == "gus_armR_t" && std::abs(before.width() - phone->character["width"].toDouble()) < 0.01 && up.center().y() < before.center().y() - 0.02 &&
                  motion::withProp(scene, phone->character, "bg", "R", &error).isEmpty(),
              QStringLiteral("a prop is held in the hand at its own size and moves with the arm (%1, width %2, y %3 then %4)")
                  .arg(root["parent"].toString()).arg(before.width()).arg(before.center().y()).arg(up.center().y()));
      }
      const QJsonObject point = motion::withMove(scene, "gus", "point", 1, 0, 1, NAN, &error);
      double arm = 0;
      for (auto v : point["layers"].toArray())
        if (v.toObject()["id"] == "gus_armR_pose")
          arm = motion::evaluate(v.toObject()["rotation"], 1.8, point).toDouble();
      check(std::abs(arm + 80) < 1e-6 && motion::withMove(scene, "bg", "point", 1, 0, 1, NAN, &error).isEmpty(),
            "pointing turns the character's arm; a layer without arms is refused");
      // Sets: one set at a time, behind the scene, its front part in front.
      error.clear();
      const auto sets = graphics::setLibrary();
      check(sets.size() >= 17, QStringLiteral("the set library comes with the program (%1)").arg(sets.size()));
      if (sets.size() >= 2) {
        const auto withFront = std::find_if(sets.begin(), sets.end(), [](const graphics::CharacterFile &f) { return !f.character["front"].toArray().isEmpty(); });
        QJsonObject set = motion::withSet(scene, sets[0].character, &error);
        set = motion::withSet(set, withFront == sets.end() ? sets[1].character : withFront->character, &error);
        const QJsonArray layers = set["layers"].toArray();
        int groups = 0;
        for (auto v : layers)
          groups += v.toObject()["id"] == "set";
        check(!set.isEmpty() && groups == 1 && layers[0].toObject()["id"] == "set" &&
                  (withFront == sets.end() || layers[layers.size() - 1].toObject()["id"].toString().startsWith("setf_")) &&
                  layers.size() > scene["layers"].toArray().size(),
              "a set goes behind the scene and replaces the set before it " + error);
        const auto hall = std::find_if(sets.begin(), sets.end(), [](const graphics::CharacterFile &f) { return f.path.endsWith("town-hall.json"); });
        if (hall != sets.end()) {
          const QJsonObject withHall = motion::withSet(scene, hall->character, &error);
          const QJsonObject reworded = motion::withPatch(withHall, QJsonArray{QJsonObject{{"layer_id", "$variables"}, {"property", "set_banner"},
                                                                                         {"value", "FEELINGS ARE BACK"}}}, &error);
          QJsonObject banner;
          for (auto v : reworded["layers"].toArray())
            if (v.toObject()["id"] == "set_banner_t")
              banner = v.toObject();
          const QJsonObject replaced = motion::withSet(reworded, sets[0].character, &error);
          check(withHall["variables"].toObject()["set_banner"] == "TOWN MEETING TONIGHT" &&
                    motion::evaluate(banner["text"], 0, reworded) == "FEELINGS ARE BACK" && !replaced["variables"].toObject().contains("set_banner"),
                "a set's signs are scene variables (set_banner), gone with the set");
        }
      }
    }
    // Draft previews draw fewer motion blur samples (and look the same with none).
    QJsonObject moving{{"version", 2}, {"layers", QJsonArray{rect("m", 0, .3, .2, .4, "#ffffff")}}};
    auto layers = moving["layers"].toArray();
    auto m = layers[0].toObject();
    m["x"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, .8}}}};
    layers[0] = m;
    moving["layers"] = layers;
    moving["motion_blur"] = QJsonObject{{"samples", 8}, {"shutter", .1}};
    const QImage full = drawn(moving, .5);
    motion::setDraft(true);
    const QImage draft = drawn(moving, .5);
    motion::setDraft(false);
    check(full != draft && drawn(moving, .5) == full, "a draft preview draws less motion blur, and only while drafting");
  }
  // Text animated a word (or a letter) at a time ends up where the same text without animation is: the words keep their spaces.
  {
    auto ink = [](const QImage &im) {
      int left = im.width(), right = -1;
      for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
          if (qAlpha(im.pixel(x, y)) > 60) {
            left = std::min(left, x);
            right = std::max(right, x);
          }
      return std::pair{left, right};
    };
    QJsonObject text{{"type", "text"}, {"id", "t"}, {"text", "FROM THE DIRECTOR OF A VIDEO OF HIMSELF"}, {"x", 0.05}, {"y", 0.3},
                     {"width", 0.9}, {"height", 0.3}, {"font_size", 0.045}, {"font", "Times New Roman"}, {"bold", true}, {"align", "center"},
                     {"fill", "#ffffff"}};
    auto drawnText = [&](const QJsonObject &layer) {
      QImage im(480, 270, QImage::Format_ARGB32_Premultiplied);
      im.fill(Qt::transparent);
      motion::render(im, QJsonObject{{"version", 2}, {"layers", QJsonArray{layer}}}, 5);
      return im;
    };
    const auto plain = ink(drawnText(text));
    bool same = true;
    QString seen = QStringLiteral("plain %1-%2").arg(plain.first).arg(plain.second);
    for (const char *unit : {"word", "character"}) {
      QJsonObject animated = text;
      animated["text_animation"] = QJsonObject{{"unit", unit}, {"style", "fade"}, {"stagger", 0.02}, {"duration", 0.2}};
      const auto got = ink(drawnText(animated));
      seen += QStringLiteral(", %1 %2-%3").arg(unit).arg(got.first).arg(got.second);
      same = same && std::abs(got.first - plain.first) <= 2 && std::abs(got.second - plain.second) <= 2;
    }
    check(same, "text animated a word or a letter at a time keeps its spacing (" + seen + ")");
  }
  // The scene's own clock, video time, generated textures, flashes and the new moves.
  {
    auto drawn = [](const QJsonObject &sc, double t) {
      QImage image(320, 180, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      motion::render(image, sc, t);
      return image;
    };
    QJsonObject moving{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "rect"}, {"id", "m"}, {"y", 0.3}, {"width", 0.2}, {"height", 0.4},
                                                                      {"fill", "#ffffff"}, {"x", QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0}, QJsonArray{1, 0.8}}}}}}}}};
    QJsonObject reversed = moving, frozen = moving;
    reversed["time"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 1}, QJsonArray{1, 0}}}};
    frozen["time"] = QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 0.5}, QJsonArray{1, 0.5}}}};
    check(drawn(reversed, 0.25) == drawn(moving, 0.75) && drawn(frozen, 0.9) == drawn(moving, 0.5) && drawn(frozen, 0.1) == drawn(moving, 0.5),
          "a scene's time can play it backwards or hold it (a freeze)");
    QJsonObject textures{{"version", 2}, {"layers", QJsonArray{
        QJsonObject{{"type", "noise"}, {"id", "snow"}, {"style", "static"}, {"x", 0}, {"y", 0}, {"width", 0.5}, {"height", 1}},
        QJsonObject{{"type", "noise"}, {"id", "fog"}, {"style", "fog"}, {"x", 0.5}, {"y", 0}, {"width", 0.5}, {"height", 1}, {"amount", 1}}}}};
    const QImage a = drawn(textures, 1.0), b = drawn(textures, 1.0 + 1. / 24);
    int softFog = 0, flatFog = 0;
    for (int x = 170; x < 320; x += 5) {
      softFog += qAlpha(a.pixel(x, 90)) > 10 && qAlpha(a.pixel(x, 90)) < 245;
      flatFog += qAlpha(a.pixel(x, 90)) == qAlpha(a.pixel(170, 90));
    }
    check(qAlpha(a.pixel(40, 40)) == 255 && a.copy(0, 0, 160, 180) != b.copy(0, 0, 160, 180) && a == drawn(textures, 1.0) && softFog > 5 &&
              flatFog < 25,
          QStringLiteral("noise layers: static that changes every frame (the same each time), soft drifting fog (%1 soft)").arg(softFog));
    QJsonObject flash{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "flash"}, {"id", "f"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1},
                                                                     {"times", QJsonArray{1.0}}, {"decay", 0.25}}}}};
    check(qAlpha(drawn(flash, 0.9).pixel(10, 10)) == 0 && qAlpha(drawn(flash, 1.0).pixel(10, 10)) > 240 && qAlpha(drawn(flash, 1.3).pixel(10, 10)) < 20,
          "a flash layer flashes at its times and fades over its decay");
    QString error;
    QJsonObject person{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "rect"}, {"id", "p"}, {"x", 0.4}, {"y", 0.5}, {"width", 0.15},
                                                                      {"height", 0.4}, {"fill", "#ffffff"}}}}};
    auto at = [&](const QJsonObject &sc, const char *property, double t, double fallback) {
      return motion::evaluate(sc["layers"].toArray()[0].toObject()[property], t, sc).toDouble(fallback);
    };
    const QJsonObject walkIn = motion::withMove(person, "p", "walk in", 1, 2, 1, NAN, &error);
    const QJsonObject fell = motion::withMove(person, "p", "fall", 1, 0, 1, NAN, &error);
    const QJsonObject sat = motion::withMove(person, "p", "sit", 1, 0, 1, NAN, &error);
    const QJsonObject floated = motion::withMove(person, "p", "float", 0, 4, 1, NAN, &error);
    check(at(walkIn, "x", 0.5, 0) < 0 && std::abs(at(walkIn, "x", 3, 0) - 0.4) < 1e-6 && std::abs(at(fell, "rotation", 5, 0) - 82) < 1 &&
              std::abs(at(sat, "scale_y", 5, 1) - 0.8) < 1e-6 && std::abs(at(floated, "y", 4, 0) - 0.5) < 1e-6 && at(floated, "y", 1, 0) < 0.5,
          "new moves: walk in from off the frame, fall and stay down, sit, float " + error);
  }
  // Patching: small changes by path, without sending the scene; and room for more characters.
  {
    QJsonObject scene{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "text"}, {"id", "sign"}, {"text", "OPEN"}, {"x", 0}, {"y", 0},
                                                                     {"width", 1}, {"height", 0.2}, {"shadow", QJsonObject{{"blur", 0.01}}}}}}};
    QString error;
    const QJsonObject patched = motion::withPatch(scene, QJsonArray{
        QJsonObject{{"layer_id", "sign"}, {"property", "text"}, {"value", "CLOSED"}},
        QJsonObject{{"layer_id", "sign"}, {"property", "shadow.blur"}, {"value", 0.03}},
        QJsonObject{{"layer_id", "sign"}, {"property", "effects"}, {"value", QJsonArray{QJsonObject{{"type", "glow"}}}}},
        QJsonObject{{"layer_id", "$camera"}, {"property", "zoom"}, {"value", 1.5}},
        QJsonObject{{"layer_id", "$scene"}, {"property", "motion_blur"}, {"value", QJsonObject{{"samples", 4}}}}}, &error);
    const QJsonObject sign = patched["layers"].toArray()[0].toObject();
    check(error.isEmpty() && sign["text"] == "CLOSED" && sign["shadow"].toObject()["blur"].toDouble() == 0.03 && sign.contains("effects") &&
              patched["camera"].toObject()["zoom"].toDouble() == 1.5 && patched.contains("motion_blur"),
          "a patch sets properties by path on layers, the camera and the scene " + error);
    const QJsonObject removed = motion::withPatch(patched, QJsonArray{QJsonObject{{"layer_id", "sign"}, {"property", "effects"}, {"remove", true}}}, &error);
    check(!removed["layers"].toArray()[0].toObject().contains("effects"), "a patch removes a property");
    check(motion::withPatch(scene, QJsonArray{QJsonObject{{"layer_id", "sign"}, {"property", "id"}, {"value", "x"}}}, &error).isEmpty() &&
              motion::withPatch(scene, QJsonArray{QJsonObject{{"layer_id", "nobody"}, {"property", "text"}, {"value", "x"}}}, &error).isEmpty() &&
              error.contains("Change 1"),
          "a patch refuses structural properties and missing layers, naming the change");
    QJsonArray many;
    for (int i = 0; i < 900; ++i)
      many.append(QJsonObject{{"type", "rect"}, {"id", QStringLiteral("r%1").arg(i)}, {"x", 0}, {"y", 0}, {"width", 0.01}, {"height", 0.01}});
    QJsonArray tooMany = many;
    for (int i = 900; i < motion::kMaxLayers + 1; ++i)
      tooMany.append(QJsonObject{{"type", "rect"}, {"id", QStringLiteral("r%1").arg(i)}, {"x", 0}, {"y", 0}, {"width", 0.01}, {"height", 0.01}});
    check(motion::validate(QJsonObject{{"version", 2}, {"layers", many}}).isEmpty() &&
              !motion::validate(QJsonObject{{"version", 2}, {"layers", tooMany}}).isEmpty(),
          "a scene holds up to 1024 layers (a dozen library characters)");
  }
  // Playing a scene in the preview: the frames drawn ahead on other cores are the frames drawn one by one.
  {
    GeneratorSpec spec = makeGenerator(*findGeneratorPreset("graphics_blank"));
    spec.data = motion::charactersScene();
    spec.data["camera"] = QJsonObject{{"zoom", QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, 1}, QJsonArray{1, 1.6}}}}}};
    const FrameRate fps{24, 1};
    int differing = 0;
    for (int f = 0; f < 16; ++f) {
      const TimeUs t = fps.timeOf(f);
      const QImage played = renderGenerator(spec, {320, 180}, 1. / 6, t, 10 * kUsPerSecond, fps);
      QImage direct(320, 180, QImage::Format_ARGB32_Premultiplied);
      direct.fill(Qt::transparent);
      const bool was = motion::setDraft(true);
      graphics::drawScene(direct, spec.data, double(t) / kUsPerSecond);
      motion::setDraft(was);
      differing += played != direct;
    }
    check(differing == 0, QStringLiteral("frames drawn ahead while playing match frames drawn one by one (%1 differ)").arg(differing));
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
  {
    // A voice track for lip sync: its tone event from 10 to 11 s and a silent
    // one from 12 to 13 s, seen by a scene from 9.5 s, 4 s long (10 ms bins).
    auto item = std::make_shared<MediaItem>();
    item->id = 1;
    item->path = audio;
    item->info = probeMedia(audio);
    std::map<int, std::shared_ptr<const MediaItem>> media{{1, item}};
    Track voice;
    voice.type = TrackType::Audio;
    Clip line;
    line.mediaId = 1;
    line.start = 10 * kUsPerSecond;
    line.length = kUsPerSecond;
    line.sourceIn = kUsPerSecond;
    voice.clips.push_back(line);
    line.start = 12 * kUsPerSecond;
    line.sourceIn = 0;
    voice.clips.push_back(line);
    const auto env = graphics::trackEnvelope(voice, media, 9.5, 4, 400, &error);
    const auto left = env["left"].toArray(), right = env["right"].toArray();
    double before = 0, during = 0, loud = 0, after = 0;
    for (int i = 0; i < left.size(); ++i) {
      double &slot = i < 49 ? before : i > 151 ? after : during;
      slot = std::max(slot, left[i].toDouble());
    }
    for (int i = 55; i < 145 && i < right.size(); ++i)
      loud = std::max(loud, right[i].toDouble());
    check(graphics::validateWaveform(env).isEmpty() && left.size() == 400 && before == 0 && after == 0 &&
              std::abs(during - .25) < .01 && std::abs(loud - .5) < .01,
          "a track's envelope: each event's sound at its timeline place");
    {
      // Said backwards: a two second line (silence, then the tone) reversed is the tone, then silence.
      error.clear();
      Track backwards;
      backwards.type = TrackType::Audio;
      Clip turned;
      turned.mediaId = 1;
      turned.start = 30 * kUsPerSecond;
      turned.length = 2 * kUsPerSecond;
      backwards.clips.push_back(turned);
      const auto forward = graphics::trackEnvelope(backwards, media, 30, 2, 200, &error);
      backwards.clips[0].reverse = true;
      const auto reversed = graphics::trackEnvelope(backwards, media, 30, 2, 200, &error);
      auto loud = [](const QJsonObject &env, int from, int to) {
        double peak = 0;
        for (int i = from; i < to; ++i)
          peak = std::max(peak, env["left"].toArray()[i].toDouble());
        return peak;
      };
      check(!reversed.isEmpty() && loud(forward, 5, 95) < .01 && loud(forward, 105, 195) > .2 && loud(reversed, 5, 95) > .2 &&
                loud(reversed, 105, 195) < .01 && forward["signature"] != reversed["signature"],
            "a reversed line moves the mouth where its sound is " + error);
      // Subtitles: a text bound to the track shows the words of the line playing, and nothing between lines.
      backwards.clips[0].reverse = false;
      backwards.clips[0].name = "Is the door still a door?";
      const auto said = graphics::trackEnvelope(backwards, media, 29, 4, 200, &error);
      const QJsonObject scene{{"version", 2}, {"audio", QJsonObject{{"blip", said}}}, {"layers", QJsonArray{}}};
      const QJsonObject words{{"subtitle", "blip"}, {"upper", true}};
      check(motion::evaluate(words, 0.5, scene).toString().isEmpty() && motion::evaluate(words, 2.0, scene) == "IS THE DOOR STILL A DOOR?" &&
                motion::evaluate(words, 3.5, scene).toString().isEmpty() && motion::animating(QJsonObject{{"text", words}}),
            "a subtitle bound to a voice track shows the line being said");
    }
    check(graphics::trackEnvelope(voice, media, 20, 2, 400, &error).isEmpty() && !error.isEmpty(),
          "a track with nothing during the scene is refused");
    // Mouth shapes from the words, spread over the sound of the line.
    voice.clips[0].narration["text"] = "Hello, Bob";
    const auto spoken = graphics::trackEnvelope(voice, media, 9.5, 4, 400, &error)["visemes"].toArray();
    QStringList shapes;
    for (auto v : spoken)
      shapes << v.toArray()[1].toString();
    check(shapes.contains("O") && shapes.contains("M") && shapes.contains("E") && spoken.first().toArray()[0].toDouble() >= .5 &&
              spoken.last().toArray()[0].toDouble() <= 1.5,
          QStringLiteral("a line's mouth shapes come from its words, during its sound (%1)").arg(shapes.join(' ')));
    // Lip sync follows the voice track: moving the line re-analyses the scene's binding, as part of the same edit.
    Project project;
    const int mediaId = project.addMedia(item->info);
    Timeline tl = project.timeline();
    Track voiceTrack = voice;
    voiceTrack.id = tl.allocId();
    voiceTrack.name = "Gus";
    for (auto &c : voiceTrack.clips) {
      c.id = tl.allocId();
      c.mediaId = mediaId;
    }
    QJsonObject sceneData{{"version", 2}, {"layers", QJsonArray{QJsonObject{{"type", "rect"}, {"id", "mouth"}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1},
                                                                       {"scale_y", QJsonObject{{"audio", "gus"}, {"base", 0}, {"amount", 1}}}}}}};
    sceneData["audio"] = QJsonObject{{"gus", graphics::trackEnvelope(voiceTrack, project.snapshot()->media, 9.5, 4, 400, &error)}};
    Track picture;
    picture.id = tl.allocId();
    picture.type = TrackType::Video;
    Clip scene;
    scene.id = tl.allocId();
    scene.start = TimeUs(9.5 * kUsPerSecond);
    scene.length = 4 * kUsPerSecond;
    scene.generator = makeGenerator(*findGeneratorPreset("graphics_blank"));
    scene.generator.data = sceneData;
    picture.clips.push_back(scene);
    tl.tracks = {picture, voiceTrack};
    project.applyEdit(tl, "lip sync scene");
    auto loudAt = [&](const Timeline &t) { // the first loud bin of the scene's binding, in seconds
      const auto left = t.findClip(scene.id)->generator.data["audio"].toObject()["gus"].toObject()["left"].toArray();
      for (int i = 0; i < left.size(); ++i)
        if (left[i].toDouble() > .1)
          return i * 4. / left.size();
      return -1.;
    };
    const double soundBefore = loudAt(project.timeline());
    check(!graphics::trackBindingsStale(project.timeline()), "a freshly bound scene is up to date");
    Timeline moved = project.timeline();
    moved.tracks[1].clips[0].start += kUsPerSecond;
    project.applyEdit(moved, "move the line");
    const double soundAfter = loudAt(project.timeline());
    check(std::abs(soundBefore - .5) < .02 && std::abs(soundAfter - 1.5) < .02 && !graphics::trackBindingsStale(project.timeline()),
          QStringLiteral("moving a voice line moves the mouth with it (sound at %1 s, then %2 s)").arg(soundBefore).arg(soundAfter));
    project.undoStack()->undo();
    check(std::abs(loudAt(project.timeline()) - soundBefore) < .02, "one undo puts the line and the mouth back together");
  }
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
  {
    // The last layers selected, then a starting layout with fewer layers: the old rows outlived the scene (a crash).
    int rows = 0, after = -1, expected = graphics::scene("blank")["layers"].toArray().size();
    QTimer::singleShot(0, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "graphicsComposer") {
          auto *list = widget->findChild<QListWidget *>("graphicsLayers");
          auto *presets = widget->findChild<QComboBox *>("graphicsPresets");
          const int big = presets->findData("broadcast");
          presets->setCurrentIndex(big);
          emit presets->activated(big);
          rows = list->count();
          list->setCurrentRow(rows - 1);
          for (int r = 0; r < rows; ++r)
            list->item(r)->setSelected(true);
          const int k = presets->findData("blank");
          presets->setCurrentIndex(k);
          emit presets->activated(k);
          after = list->count();
          qobject_cast<QDialog *>(widget)->reject();
        }
    });
    composeGraphics(&hiddenParent, &project, id);
    check(rows > expected && after == expected,
          QString("choosing a smaller starting layout with the last layers selected (%1 layers, then %2)").arg(rows).arg(after));
  }
  {
    // Follow audio on the Keys tab: a layer's scale_y bound to a sound, with no JSON typed.
    const int undoIndex = project.undoStack()->index();
    Timeline tl = project.timeline();
    auto *clip = tl.findClip(id);
    QJsonObject scene = clip->generator.data;
    scene["version"] = 2;
    scene["audio"] = QJsonObject{{"voice", QJsonObject{{"version", 1}, {"duration", 1.0}, {"left", QJsonArray{0.0, 1.0}},
                                                      {"right", QJsonArray{0.0, 1.0}}, {"rate", 1}, {"offset", 0}}}};
    clip->generator.data = scene;
    project.applyEdit(tl, "test audio binding");
    QTimer::singleShot(0, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "graphicsComposer") {
          widget->findChild<QListWidget *>("graphicsLayers")->setCurrentRow(0);
          auto *property = widget->findChild<QComboBox *>("graphicsAnimationProperty");
          property->setCurrentText("scale_y");
          auto *sound = widget->findChild<QComboBox *>("graphicsAudioBinding");
          sound->setCurrentIndex(sound->findData("voice"));
          widget->findChild<QDoubleSpinBox *>("graphicsAudioBase")->setValue(0.2);
          widget->findChild<QDoubleSpinBox *>("graphicsAudioAmount")->setValue(1.5);
          widget->findChild<QPushButton *>("graphicsAudioFollow")->click();
          qobject_cast<QDialog *>(widget)->accept();
        }
    });
    composeGraphics(&hiddenParent, &project, id);
    const QJsonObject spec = project.timeline().findClip(id)->generator.data["layers"].toArray()[0].toObject()["scale_y"].toObject();
    check(spec["audio"].toString() == "voice" && std::abs(spec["base"].toDouble() - 0.2) < 1e-9 && std::abs(spec["amount"].toDouble() - 1.5) < 1e-9
              && spec["channel"].toString() == "peak",
          "Follow audio binds a layer's property to a sound from the Keys tab");
    while (project.undoStack()->index() > undoIndex)
      project.undoStack()->undo();
  }
  {
    // A move from the Keys tab, and motion blur from the Scene tab.
    const int undoIndex = project.undoStack()->index();
    QTimer::singleShot(0, [&] {
      for (auto *widget : QApplication::topLevelWidgets())
        if (widget->objectName() == "graphicsComposer") {
          widget->findChild<QListWidget *>("graphicsLayers")->setCurrentRow(0);
          auto *move = widget->findChild<QComboBox *>("graphicsMove");
          move->setCurrentIndex(move->findData("hop"));
          widget->findChild<QPushButton *>("graphicsAddMove")->click();
          widget->findChild<QCheckBox *>("graphicsMotionBlur")->setChecked(true);
          qobject_cast<QDialog *>(widget)->accept();
        }
    });
    composeGraphics(&hiddenParent, &project, id);
    const QJsonObject edited = project.timeline().findClip(id)->generator.data;
    check(edited["layers"].toArray()[0].toObject()["y"].toObject().contains("keyframes") && edited["motion_blur"].isObject(),
          "a move from the Keys tab adds its keys; Scene > Look turns on motion blur");
    while (project.undoStack()->index() > undoIndex)
      project.undoStack()->undo();
  }
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
