#include "core/Graphics.h"
#include "core/AudioDecoder.h"
#include "core/BeatTracker.h"
#include "core/MotionGraphics.h"
#include "core/Project.h"
#include "core/Brand.h"
#include "core/Profiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QImageReader>
#include <QJsonDocument>
#include <QPainter>
#include <QPaintEngine>
#include <QTextLayout>
#include <QGlyphRun>
#include <QPainterPath>
#include <QRegularExpression>
#include <QThreadPool>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <future>
#include <mutex>
#include <optional>

namespace graphics {
namespace {
QJsonObject layer(QString type, QString name, double x, double y, double w,
                  double h, QString fill) {
  return {{"type", type}, {"name", name}, {"x", x},      {"y", y},
          {"width", w},   {"height", h},  {"fill", fill}};
}
QJsonObject text(QString name, QString words, double x, double y, double w,
                 double h, double size, QString fill) {
  auto l = layer("text", name, x, y, w, h, fill);
  l["text"] = words;
  l["font_size"] = size;
  l["bold"] = true;
  return l;
}
double value(const QJsonObject &o, const QString &key, double fallback,
             double time) {
  auto v = o[key];
  if (!v.isObject())
    return v.toDouble(fallback);
  const auto keys = v.toObject()["keyframes"].toArray();
  if (keys.isEmpty())
    return fallback;
  auto first = keys.first().toArray();
  if (time <= first[0].toDouble())
    return first[1].toDouble();
  for (int i = 1; i < keys.size(); ++i) {
    auto a = keys[i - 1].toArray(), b = keys[i].toArray();
    if (time <= b[0].toDouble()) {
      double u = (time - a[0].toDouble()) /
                 std::max(.000001, b[0].toDouble() - a[0].toDouble());
      return a[1].toDouble() + (b[1].toDouble() - a[1].toDouble()) * u;
    }
  }
  return keys.last().toArray()[1].toDouble();
}
QColor color(const QJsonObject &o, const char *key, const char *fallback) {
  QColor c(o[key].toString(fallback));
  return c.isValid() ? c : QColor(fallback);
}
ParamDef number(const char *id, const char *name, double min, double max,
                double def) {
  ParamDef p;
  p.id = id;
  p.name = name;
  p.min = min;
  p.max = max;
  p.def = def;
  p.decimals = 3;
  return p;
}
ParamDef tint(const char *id, const char *name, int def) {
  auto p = number(id, name, 0, 0xffffff, def);
  p.type = ParamType::Color;
  return p;
}
ParamDef flag(const char *id, const char *name, bool def) {
  auto p = number(id, name, 0, 1, def);
  p.type = ParamType::Bool;
  return p;
}
void paintWave(QPainter &d, QSize image, const Params &p, const GeneratorContext &ctx) {
  const auto data = ctx.data ? *ctx.data : QJsonObject{};
  d.setRenderHint(QPainter::Antialiasing);
  QRectF box(p.num("x") * image.width(), p.num("y") * image.height(),
             p.num("width") * image.width(), p.num("height") * image.height());
  d.setClipRect(box);
  QColor bg = p.color("background");
  bg.setAlphaF(p.num("background_opacity"));
  d.fillRect(box, bg);
  const auto left = data["left"].toArray(), right = data["right"].toArray();
  if (!validateWaveform(data).isEmpty()) {
    d.setPen(p.color("color"));
    d.drawText(
        box, Qt::AlignCenter,
        QStringLiteral("Choose an audio source in the generator editor"));
    return;
  }
  const double duration = data["duration"].toDouble(1),
               now = double(ctx.time) / kUsPerSecond * p.num("rate") +
                     p.num("offset");
  double begin = 0, end = duration;
  if (p.flag("scroll")) {
    double window = std::min(duration, p.num("window"));
    begin = std::clamp(now - window / 2, 0., std::max(0., duration - window));
    end = begin + window;
  }
  double maximum = 0;
  for (auto a : left)
    maximum = std::max(maximum, a.toDouble());
  for (auto a : right)
    maximum = std::max(maximum, a.toDouble());
  double gain = p.num("amplitude") /
                (p.flag("normalize") ? std::max(.00001, maximum) : 1.);
  int channels = p.flag("stereo") ? 2 : 1,
      bars = std::clamp(
          int(box.width() / std::max(1., p.num("spacing") * ctx.scale)), 8,
          2048);
  for (int ch = 0; ch < channels; ++ch) {
    double center = box.top() + (ch + .5) * box.height() / channels;
    QPainterPath shape;
    std::vector<QPointF> lower;
    for (int x = 0; x < bars; ++x) {
      double a = begin + (end - begin) * x / bars,
             b = begin + (end - begin) * (x + 1) / bars;
      int lo = std::clamp(int(a / duration * left.size()), 0,
                          int(left.size()) - 1),
          hi = std::clamp(int(std::ceil(b / duration * left.size())), lo + 1,
                          int(left.size()));
      double peak = 0;
      for (int k = lo; k < hi; ++k) {
        double v =
            channels == 2 && ch == 1 ? right[k].toDouble() : left[k].toDouble();
        if (channels == 1)
          v = std::max(v, right[k].toDouble());
        peak = std::max(peak, v);
      }
      double px = box.left() + (x + .5) * box.width() / bars,
             h = std::min(1., peak * gain) * box.height() * .45 / channels;
      QColor c =
          p.color(p.flag("progress") && a <= now ? "played_color" : "color");
      d.setPen(QPen(c, std::max(1., p.num("thickness") * ctx.scale)));
      if (p.choice("style") == 0)
        d.drawLine(QPointF(px, center - h), QPointF(px, center + h));
      else {
        lower.emplace_back(px, center + h);
        if (x == 0)
          shape.moveTo(px, center - h);
        else
          shape.lineTo(px, center - h);
      }
    }
    if (p.choice("style") != 0) {
      d.setPen(
          QPen(p.color("color"), std::max(1., p.num("thickness") * ctx.scale)));
      if (p.choice("style") == 1) {
        for (auto it = lower.rbegin(); it != lower.rend(); ++it)
          shape.lineTo(*it);
        shape.closeSubpath();
        d.fillPath(shape, p.color("color"));
      } else
        d.drawPath(shape);
      if (p.flag("progress")) {
        d.save();
        double played =
            std::clamp((now - begin) / std::max(.00001, end - begin), 0., 1.);
        d.setClipRect(
            QRectF(box.left(), box.top(), box.width() * played, box.height()),
            Qt::IntersectClip);
        if (p.choice("style") == 1)
          d.fillPath(shape, p.color("played_color"));
        else {
          d.setPen(QPen(p.color("played_color"),
                        std::max(1., p.num("thickness") * ctx.scale)));
          d.drawPath(shape);
        }
        d.restore();
      }
    }
  }
  if (p.flag("progress")) {
    double x =
        box.left() +
        std::clamp((now - begin) / std::max(.00001, end - begin), 0., 1.) *
            box.width();
    d.setPen(QPen(p.color("played_color"), 2 * ctx.scale));
    d.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
  }
}
} // namespace
QStringList presets() {
  return QStringList{"blank", "broadcast", "lower_third", "info_card",
                     "timeline"} +
         motion::presets();
}
QJsonObject scene(const QString &preset) {
  if (preset == "motion_blank")
    return {{"version", 2}, {"layers", QJsonArray{}}};
  if (motion::presets().contains(preset))
    return motion::preset(preset);
  QJsonArray l;
  if (preset == "broadcast") {
    l.append(layer("rect", "Header", 0, 0, 1, .09, "#12243b"));
    l.append(layer("rect", "Brand", .03, .02, .14, .052, "#1e6deb"));
    l.append(text("Brand text", "CULTURE DESK", .04, .02, .28, .052, .027,
                  "#ffffff"));
    l.append(layer("rect", "Sidebar", .03, .13, .26, .59, "#12243b"));
    l.append(layer("rect", "Accent", .03, .13, .004, .59, "#66dddf"));
    l.append(
        text("Artist", "ARTIST / SUBJECT", .05, .29, .22, .18, .04, "#ffffff"));
    l.append(
        text("Era", "LOCATION / ERA", .05, .17, .22, .08, .024, "#adc0d5"));
    l.append(text("Description", "Explain the story behind this footage.", .05,
                  .56, .22, .12, .027, "#adc0d5"));
    l.append(layer("rect", "Headline bar", .03, .74, .94, .085, "#1e6deb"));
    l.append(text("Headline", "YOUR STORY HEADLINE", .045, .75, .90, .065, .04,
                  "#ffffff"));
    l.append(layer("rect", "Caption background", 0, .84, 1, .10, "#091322"));
    l.append(layer("rect", "Footer", 0, .95, 1, .05, "#12243b"));
    l.append(text("Footer text", "SPECIAL REPORT / SOURCE CREDIT", .03, .954,
                  .94, .04, .020, "#adc0d5"));
  } else if (preset == "lower_third") {
    l.append(layer("rect", "Panel", .05, .75, .90, .16, "#12243b"));
    l.append(layer("rect", "Accent", .05, .75, .006, .16, "#66dddf"));
    l.append(
        text("Name", "NAME / SUBJECT", .07, .76, .85, .075, .04, "#ffffff"));
    l.append(text("Subtitle", "Role, location or source credit", .07, .845, .85,
                  .05, .026, "#adc0d5"));
  } else if (preset == "info_card") {
    l.append(layer("rect", "Card", .08, .12, .84, .70, "#12243b"));
    l.append(text("Heading", "THE STORY IN FOUR POINTS", .11, .16, .78, .10,
                  .042, "#66dddf"));
    for (int i = 0; i < 4; ++i) {
      l.append(layer("rect", QString("Bullet %1").arg(i + 1), .11,
                     .30 + i * .115, .006, .07, "#1e6deb"));
      l.append(text(QString("Point %1").arg(i + 1),
                    QString("%1 / Add a fact, date or explanation").arg(i + 1),
                    .14, .30 + i * .115, .72, .08, .030, "#ffffff"));
    }
  } else if (preset == "timeline") {
    l.append(layer("rect", "Panel", .05, .2, .9, .55, "#12243b"));
    l.append(text("Heading", "A HISTORY IN SOUND", .08, .24, .84, .1, .045,
                  "#66dddf"));
    l.append(layer("line", "Timeline line", .1, .5, .8, 0, "#66dddf"));
    for (int i = 0; i < 4; ++i) {
      l.append(layer("ellipse", QString("Node %1").arg(i), .105 + i * .24, .482,
                     .02, .036, "#1e6deb"));
      l.append(text(QString("Date %1").arg(i),
                    QString("%1s").arg(1990 + i * 10), .075 + i * .24, .39, .16,
                    .08, .031, "#ffffff"));
      l.append(text(QString("Stage %1").arg(i),
                    QString("Chapter %1").arg(i + 1), .075 + i * .24, .55, .16,
                    .08, .026, "#adc0d5"));
    }
  }
  return {{"version", 1}, {"layers", l}};
}
QString validateScene(const QJsonObject &s) {
  if (s["version"].toInt() == 2)
    return motion::validate(s);
  if (s["version"].toInt() != 1 || !s["layers"].isArray())
    return "Graphics scenes need version: 1 and a layers array.";
  if (s["layers"].toArray().size() > 128 ||
      QJsonDocument(s).toJson(QJsonDocument::Compact).size() > 2 * 1024 * 1024)
    return "Use at most 128 layers and 2 MB of scene data.";
  const QStringList numeric = {
      "x",         "y",      "width",        "height", "opacity", "rotation",
      "font_size", "radius", "stroke_width", "in",     "out"};
  for (auto v : s["layers"].toArray()) {
    if (!v.isObject())
      return "Every layer must be an object.";
    auto o = v.toObject();
    if (!QStringList{"rect", "ellipse", "line", "text", "image"}.contains(
            o["type"].toString()))
      return "Layer types: rect, ellipse, line, text, image.";
    for (auto key : numeric)
      if (o.contains(key)) {
        auto valid = [&](QJsonValue a) {
          return a.isDouble() && std::isfinite(a.toDouble()) &&
                 std::abs(a.toDouble()) <= 10000;
        };
        if (o[key].isObject()) {
          auto keys = o[key].toObject()["keyframes"].toArray();
          double previous = -1;
          if (keys.isEmpty() || keys.size() > 256)
            return "Use 1–256 keyframes per property.";
          for (auto kv : keys) {
            auto pair = kv.toArray();
            if (pair.size() != 2 || !valid(pair[0]) || !valid(pair[1]) ||
                pair[0].toDouble() < previous || pair[0].toDouble() < 0)
              return "Keyframes must be sorted [seconds, finite value] pairs.";
            previous = pair[0].toDouble();
          }
        } else if (!valid(o[key]))
          return "Layer properties must be finite numbers.";
      }
    for (auto key : {"fill", "stroke"})
      if (o.contains(key) && !QColor(o[key].toString()).isValid())
        return "Use valid CSS-style colours such as #66dddf.";
    for (auto key : {"text", "name", "path", "font"})
      if (o[key].toString().size() > 20000)
        return "Layer text is too long.";
  }
  return {};
}
void drawScene(QImage &image, const QJsonObject &s, double seconds) {
  QPainter p(&image);
  paintScene(p, image.size(), s, seconds);
}
void paintScene(QPainter &p, QSize image, const QJsonObject &s, double seconds) {
  if (s["version"].toInt() == 2) {
    motion::paint(p, image, s, seconds);
    return;
  }
  if (!validateScene(s).isEmpty())
    return;
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  for (auto v : s["layers"].toArray()) {
    auto o = v.toObject();
    if (!o["visible"].toBool(true) || seconds < o["in"].toDouble(0) ||
        seconds >= o["out"].toDouble(1e9))
      continue;
    QRectF b(value(o, "x", 0, seconds) * image.width(),
             value(o, "y", 0, seconds) * image.height(),
             std::clamp(value(o, "width", .3, seconds), 0., 4.) * image.width(),
             std::clamp(value(o, "height", .1, seconds), 0., 4.) *
                 image.height());
    p.save();
    p.setOpacity(p.opacity() * std::clamp(value(o, "opacity", 1, seconds), 0., 1.));
    p.translate(b.center());
    p.rotate(value(o, "rotation", 0, seconds));
    p.translate(-b.center());
    QColor fill = color(o, "fill", "#ffffff");
    double sw = std::clamp(value(o, "stroke_width", 0, seconds), 0., .1) *
                image.height();
    p.setPen(sw > 0 ? QPen(color(o, "stroke", "#ffffff"), sw) : Qt::NoPen);
    p.setBrush(fill);
    QString type = o["type"].toString();
    if (type == "rect")
      p.drawRoundedRect(
          b,
          std::clamp(value(o, "radius", 0, seconds), 0., .5) * image.height(),
          std::clamp(value(o, "radius", 0, seconds), 0., .5) * image.height());
    else if (type == "ellipse")
      p.drawEllipse(b);
    else if (type == "line") {
      p.setPen(QPen(fill, std::max(1., sw)));
      p.drawLine(b.topLeft(), b.bottomRight());
    } else if (type == "text") {
      p.setClipRect(b);
      QFont font(o["font"].toString("Segoe UI"));
      font.setPixelSize(
          std::clamp(int(value(o, "font_size", .035, seconds) * image.height()),
                     1, image.height()));
      font.setBold(o["bold"].toBool(false));
      p.setFont(font);
      p.setPen(fill);
      int align = o["align"].toString() == "center"  ? Qt::AlignHCenter
                  : o["align"].toString() == "right" ? Qt::AlignRight
                                                     : Qt::AlignLeft;
      if (p.paintEngine()->type() == QPaintEngine::OpenGL2) {
        // Vector glyphs avoid mixing Qt's bitmap glyph atlas with Mutant Video Producer's
        // custom texture pipeline, and retain shaped Unicode text positions.
        QString words=o["text"].toString(); words.replace('\n',QChar(0x2028));
        QTextLayout layout(words,font);
        QTextOption option; option.setWrapMode(QTextOption::WordWrap);
        option.setAlignment(Qt::Alignment(align)); layout.setTextOption(option);
        layout.beginLayout(); double height=0;
        while (true) {
          auto line=layout.createLine(); if (!line.isValid()) break;
          line.setLineWidth(std::max(1.,b.width())); line.setPosition({0,height}); height+=line.height();
        }
        layout.endLayout(); QPainterPath textPath;
        for (const auto &run : layout.glyphRuns()) {
          auto indices=run.glyphIndexes(); auto positions=run.positions();
          for (int i=0;i<indices.size();++i) {
            QTransform placement; placement.translate(b.x()+positions[i].x(), b.y()+(b.height()-height)/2+positions[i].y());
            textPath.addPath(placement.map(run.rawFont().pathForGlyph(indices[i])));
          }
        }
        p.fillPath(textPath,fill);
      } else {
        p.drawText(b, align | Qt::AlignVCenter | Qt::TextWordWrap, o["text"].toString());
      }
    } else if (type == "image") {
      QImageReader reader(o["path"].toString());
      reader.setAutoTransform(true);
      if (reader.size().width() > 8192 || reader.size().height() > 8192)
        reader.setScaledSize(
            reader.size().scaled(4096, 4096, Qt::KeepAspectRatio));
      QImage src = reader.read();
      if (!src.isNull()) {
        QSizeF fit = src.size();
        fit.scale(b.size(), Qt::KeepAspectRatio);
        p.drawImage(
            QRectF(b.center() - QPointF(fit.width() / 2, fit.height() / 2),
                   fit),
            src);
      } else {
        p.setPen(Qt::red);
        p.drawText(b, Qt::AlignCenter | Qt::TextWordWrap,
                   "Image offline: " +
                       QFileInfo(o["path"].toString()).fileName());
      }
    }
    p.restore();
  }
}
QString validateWaveform(const QJsonObject &data) {
  if (data["version"].toInt() != 1 || !data["duration"].isDouble() ||
      data["duration"].toDouble() <= 0 || data["duration"].toDouble() > 3600)
    return "Waveform duration must be between 0 and 3600 seconds.";
  const auto a = data["left"].toArray(), b = data["right"].toArray();
  if (a.isEmpty() || a.size() > 8192 || a.size() != b.size())
    return "Waveforms need matching left/right arrays with 1–8192 peaks.";
  for (const auto &array : {a, b})
    for (auto v : array)
      if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < 0 ||
          v.toDouble() > 1)
        return "Audio peaks must be finite numbers from 0 to 1.";
  return {};
}
QJsonObject buildWaveform(const QString &path, int stream, double start,
                          double duration, int bins, QString *error,
                          const std::function<bool(double)> &progress,
                          bool analyzeBeats) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  if (!std::isfinite(start) || !std::isfinite(duration) || start < 0 ||
      start > 1e7 || duration <= 0 || duration > 3600)
    return fail("Choose an audio range up to one hour.");
  bins = std::clamp(bins, 64, 8192);
  AudioDecoder decoder;
  QString e;
  if (!decoder.open(path, stream, 48000, &e))
    return fail(e);
  QJsonArray left, right;
  QJsonArray beats;
  std::unique_ptr<BeatTracker> tracker;
  if (analyzeBeats) {
    tracker = std::make_unique<BeatTracker>(48000);
    auto settings = tracker->settings();
    settings.latencyMs = 0;
    tracker->setSettings(settings);
  }
  std::vector<float> samples(8192 * 2);
  int64_t count = std::max<int64_t>(1, std::llround(duration * 48000)),
          origin = std::llround(start * 48000);
  for (int bin = 0; bin < bins; ++bin) {
    if (progress && !progress(double(bin) / bins))
      return fail("Cancelled");
    float l = 0, r = 0;
    int64_t pos = count * bin / bins, end = count * (bin + 1) / bins;
    while (pos < end) {
      int frames = int(std::min<int64_t>(8192, end - pos));
      decoder.read(origin + pos, frames, samples.data());
      if (tracker) {
        tracker->process(samples.data(), frames, double(pos) / 48000);
        for (double beat : tracker->takeBeats())
          if (beat >= 0 && beat < duration && beats.size() < 8192)
            beats.append(beat);
      }
      for (int j = 0; j < frames; ++j) {
        if (std::isfinite(samples[j * 2]))
          l = std::max(l, std::abs(samples[j * 2]));
        if (std::isfinite(samples[j * 2 + 1]))
          r = std::max(r, std::abs(samples[j * 2 + 1]));
      }
      pos += frames;
    }
    left.append(std::min(1.f, l));
    right.append(std::min(1.f, r));
  }
  QJsonObject result{{"version", 1},         {"source_path", path},
                     {"stream", stream},     {"source_in", start},
                     {"duration", duration}, {"left", left},
                     {"right", right}};
  if (tracker) {
    result["beats"] = beats;
    result["bpm"] = tracker->bpm();
    result["beat_confidence"] = tracker->confidence();
  }
  return result;
}
namespace {
// A cartoon mouth's shape for a letter: A (open), E (wide), O (round), M (closed: m b p), F (teeth on the lip: f v),
// L (the other consonants: a small open mouth).
QString letterShape(QChar c) {
  switch (c.toLower().unicode()) {
  case 'a': case 'i': case 'h': case 'y': return "A";
  case 'e': return "E";
  case 'o': case 'u': case 'w': case 'q': return "O";
  case 'm': case 'b': case 'p': return "M";
  case 'f': case 'v': return "F";
  default: return c.isLetter() ? "L" : QString();
  }
}
// The shapes of one word over [a, b) seconds: its letters share the time, repeats merged; then rest.
void wordShapes(QJsonArray &out, const QString &word, double a, double b) {
  QStringList shapes;
  for (QChar c : word)
    if (const QString s = letterShape(c); !s.isEmpty() && (shapes.isEmpty() || shapes.last() != s))
      shapes << s;
  if (shapes.isEmpty() || b <= a)
    return;
  const double each = (b - a) / shapes.size();
  for (int i = 0; i < shapes.size(); ++i)
    out.append(QJsonArray{a + each * i, shapes[i]});
  out.append(QJsonArray{b, "rest"});
}
// What a track binding depends on: the events overlapping the scene, and the scene's place.
QString trackSignature(const Track &track, double from, double length, int bins) {
  QStringList parts{QString::number(from, 'f', 4), QString::number(length, 'f', 4), QString::number(bins)};
  for (const Clip &c : track.clips) {
    const double start = double(c.start) / kUsPerSecond, end = double(c.end()) / kUsPerSecond;
    if (end <= from || start >= from + length)
      continue;
    parts << QStringLiteral("%1:%2:%3:%4:%5:%6:%7:%8:%9").arg(c.id).arg(c.start).arg(c.length).arg(c.sourceIn).arg(c.mediaId).arg(c.speed)
                 .arg(c.muted).arg(qHash(c.narration.value("text").toString(c.name))).arg(c.reverse);
  }
  return parts.join(';');
}
} // namespace

QJsonObject trackEnvelope(const Track &track, const MediaMap &media, double from, double length, int bins,
                          QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  if (!std::isfinite(from) || !std::isfinite(length) || length <= 0 || length > 3600)
    return fail("Choose an audio range up to one hour.");
  bins = std::clamp(bins, 64, 8192);
  std::vector<double> left(size_t(bins), 0), right(size_t(bins), 0);
  int used = 0;
  for (const Clip &c : track.clips) {
    if (c.muted || c.isGenerated() || c.isSequence() || c.hasRamp() || c.loopLength > 0)
      continue;
    const auto m = media.find(c.mediaId);
    if (m == media.end() || !m->second->info.hasAudio())
      continue;
    const double start = double(c.start) / kUsPerSecond, a = std::max(from, start),
                 b = std::min(from + length, double(c.end()) / kUsPerSecond);
    const int ia = std::clamp(int(std::floor((a - from) / length * bins)), 0, bins - 1),
              n = std::min(bins, std::max(ia + 1, int(std::ceil((b - from) / length * bins)))) - ia;
    if (b <= a || n <= 0)
      continue;
    int stream = c.streamIndex;
    if (!m->second->info.audioStream(stream))
      stream = m->second->info.audio.front().index;
    QString e;
    // (reversed: the window's sound is the source's span from the other end, its peaks read backwards)
    const double end = double(c.end()) / kUsPerSecond;
    const QJsonObject w = buildWaveform(m->second->path, stream, double(c.sourceIn) / kUsPerSecond + ((c.reverse ? end - b : a - start)) * c.speed,
                                        (b - a) * c.speed, n, &e);
    if (w.isEmpty())
      return fail(e);
    const QJsonArray l = w["left"].toArray(), r = w["right"].toArray(); // (at least 64 peaks: folded into n)
    for (int k = 0; k < n; ++k) {
      const qsizetype s0 = qsizetype(k) * l.size() / n, s1 = std::max(s0 + 1, qsizetype(k + 1) * l.size() / n);
      const size_t at = size_t(ia + (c.reverse ? n - 1 - k : k));
      for (qsizetype s = s0; s < std::min(s1, l.size()); ++s) {
        left[at] = std::max(left[at], l[s].toDouble());
        right[at] = std::max(right[at], r[s].toDouble());
      }
    }
    ++used;
  }
  if (!used)
    return fail("No audio events from files on that track during this scene.");
  QJsonArray la, ra;
  for (int i = 0; i < bins; ++i) {
    la.append(left[size_t(i)]);
    ra.append(right[size_t(i)]);
  }
  // Mouth shapes from each line's words: a take's word times when it has them (aligned narration), or else the words
  // spread over the part of the event with sound, by their length. And each line's words and times, for subtitles.
  QJsonArray shapes, said;
  for (const Clip &c : track.clips) {
    const double start = double(c.start) / kUsPerSecond, end = double(c.end()) / kUsPerSecond;
    if (c.muted || end <= from || start >= from + length)
      continue;
    QString text = c.narration.value("text").toString(c.name);
    if (text.startsWith(QLatin1String("[Timing] ")))
      text = text.mid(9);
    said.append(QJsonArray{start - from, end - from, text});
    const QJsonArray takes = c.narration.value("takes").toArray();
    const QJsonArray words = takes.isEmpty() ? QJsonArray{}
                                             : takes[std::clamp(c.narration.value("active_take").toInt(), 0, int(takes.size()) - 1)]
                                                   .toObject().value("words").toArray();
    if (!words.isEmpty()) {
      for (auto w : words) {
        const auto o = w.toObject();
        const double ws = o["start"].toString().toDouble() / kUsPerSecond, we = o["end"].toString().toDouble() / kUsPerSecond;
        if (c.reverse) // (said backwards: each word where its sound is)
          wordShapes(shapes, o["text"].toString(), end - from - we, end - from - ws);
        else
          wordShapes(shapes, o["text"].toString(), start - from + ws, start - from + we);
      }
      continue;
    }
    // The sound's first and last loud moments inside the event.
    const int b0 = std::clamp(int((start - from) / length * bins), 0, bins - 1), b1 = std::clamp(int((end - from) / length * bins), 0, bins - 1);
    int first = -1, last = -1;
    for (int i = b0; i <= b1; ++i)
      if (std::max(left[size_t(i)], right[size_t(i)]) > .05) {
        if (first < 0)
          first = i;
        last = i;
      }
    const QStringList list = text.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (first < 0 || list.isEmpty())
      continue;
    const double a = double(first) / bins * length, b = double(last + 1) / bins * length;
    double weight = 0;
    for (const QString &w : list)
      weight += w.size() + (w.back().isPunct() ? 3 : 1); // (a pause after punctuation)
    double at = a;
    for (const QString &w : list) {
      const double span = (b - a) * w.size() / weight, pause = (b - a) * (w.back().isPunct() ? 3 : 1) / weight;
      wordShapes(shapes, w, at, at + span);
      at += span + pause;
    }
  }
  std::vector<QJsonArray> ordered; // (in time order: a reversed line's words were added from its end)
  for (auto v : shapes)
    ordered.push_back(v.toArray());
  std::stable_sort(ordered.begin(), ordered.end(), [](const QJsonArray &x, const QJsonArray &y) { return x[0].toDouble() < y[0].toDouble(); });
  shapes = {};
  for (const auto &v : ordered)
    shapes.append(v);
  return {{"version", 1}, {"source_in", 0}, {"duration", length}, {"left", la}, {"right", ra}, {"rate", 1}, {"offset", 0},
          {"track_id", track.id}, {"signature", trackSignature(track, from, length, bins)}, {"visemes", shapes}, {"lines", said}};
}

QString characterFolder() {
  const auto home = Profiles::instance().active().home;
  return home.isEmpty() ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Graphics")) + "/Characters"
                        : home + "/Media/Graphics Templates/Characters";
}
namespace {
std::vector<CharacterFile> libraryFiles(const QString &builtInFolder, const QString &userFolder) {
  std::vector<CharacterFile> out;
  for (const auto &[folder, builtIn] : {std::pair{QDir(QCoreApplication::applicationDirPath()).filePath(builtInFolder), true},
                                        std::pair{userFolder, false}})
    for (const QFileInfo &f : QDir(folder).entryInfoList({"*.json"}, QDir::Files, QDir::Name)) {
      QFile file(f.filePath());
      if (f.size() > 4 * 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        continue;
      const QJsonObject c = QJsonDocument::fromJson(file.readAll()).object();
      if (c["version"].toInt() == 1 && !c["layers"].toArray().isEmpty())
        out.push_back({f.filePath(), c, builtIn});
    }
  return out;
}
} // namespace
std::vector<CharacterFile> characterLibrary() { return libraryFiles("characters", characterFolder()); }
QString setFolder() {
  const auto home = Profiles::instance().active().home;
  return home.isEmpty() ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Graphics")) + "/Sets"
                        : home + "/Media/Graphics Templates/Sets";
}
std::vector<CharacterFile> setLibrary() { return libraryFiles("sets", setFolder()); }
std::vector<CharacterFile> propLibrary() {
  const auto home = Profiles::instance().active().home;
  return libraryFiles("props", home.isEmpty() ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Graphics")) + "/Props"
                                              : home + "/Media/Graphics Templates/Props");
}
bool saveCharacter(const QJsonObject &character, QString *path, QString *error) {
  QString name;
  for (QChar c : character["name"].toString())
    name += c.isLetterOrNumber() || c == ' ' || c == '-' ? c : QChar('_');
  name = name.trimmed().isEmpty() ? QStringLiteral("Character") : name.trimmed();
  if (!QDir().mkpath(characterFolder())) {
    if (error)
      *error = QStringLiteral("Can't create %1.").arg(characterFolder());
    return false;
  }
  QString target = QDir(characterFolder()).filePath(name + ".json");
  for (int n = 2; QFileInfo::exists(target); ++n)
    target = QDir(characterFolder()).filePath(QStringLiteral("%1 %2.json").arg(name).arg(n));
  QSaveFile file(target);
  if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(character).toJson(QJsonDocument::Compact)) < 0 || !file.commit()) {
    if (error)
      *error = QStringLiteral("Can't save %1.").arg(target);
    return false;
  }
  if (path)
    *path = target;
  return true;
}
bool trackBindingsStale(const Timeline &tl) {
  for (const Track &track : tl.tracks)
    for (const Clip &c : track.clips) {
      if (!isScene(c.generator.type) || !c.generator.data.contains("audio"))
        continue;
      const QJsonObject audio = c.generator.data["audio"].toObject();
      for (auto it = audio.begin(); it != audio.end(); ++it) {
        const QJsonObject binding = it.value().toObject();
        if (!binding.contains("track_id"))
          continue;
        const auto source = std::find_if(tl.tracks.begin(), tl.tracks.end(), [&](const Track &t) { return t.id == binding["track_id"].toInt(); });
        if (source != tl.tracks.end()
            && trackSignature(*source, double(c.start) / kUsPerSecond, double(c.length) / kUsPerSecond,
                              std::clamp(int(binding["left"].toArray().size()), 64, 8192)) != binding["signature"].toString())
          return true;
      }
    }
  return false;
}
bool followTrackBindings(Timeline &tl, const MediaMap &media) {
  bool changed = false;
  for (Track &track : tl.tracks)
    for (Clip &c : track.clips) {
      if (!isScene(c.generator.type) || !c.generator.data.contains("audio"))
        continue;
      QJsonObject audio = c.generator.data["audio"].toObject();
      bool touched = false;
      for (const QString &name : audio.keys()) {
        const QJsonObject binding = audio[name].toObject();
        if (!binding.contains("track_id"))
          continue;
        const auto source = std::find_if(tl.tracks.begin(), tl.tracks.end(), [&](const Track &t) { return t.id == binding["track_id"].toInt(); });
        if (source == tl.tracks.end())
          continue; // (the track was deleted: the mouth keeps what it had)
        const double from = double(c.start) / kUsPerSecond, length = double(c.length) / kUsPerSecond;
        const int bins = std::clamp(int(binding["left"].toArray().size()), 64, 8192);
        const QString signature = trackSignature(*source, from, length, bins);
        if (signature == binding["signature"].toString())
          continue;
        QString error;
        QJsonObject fresh = trackEnvelope(*source, media, from, length, bins, &error);
        if (fresh.isEmpty()) { // nothing on the track during the scene any more: silence
          QJsonArray quiet;
          for (int i = 0; i < bins; ++i)
            quiet.append(0);
          fresh = {{"version", 1}, {"source_in", 0}, {"duration", length}, {"left", quiet}, {"right", quiet}, {"rate", 1}, {"offset", 0},
                   {"track_id", source->id}, {"signature", signature}, {"visemes", QJsonArray{}}};
        }
        audio[name] = fresh;
        touched = true;
      }
      if (touched) {
        c.generator.data["audio"] = audio;
        changed = true;
      }
    }
  return changed;
}
namespace {
// Playing a scene in the preview: while its frames are asked for one after another, the next few are drawn ahead on other
// cores, so a heavy scene (several characters, motion blur) keeps up. Scrubbing, stills and renders draw as before.
class Lookahead {
public:
  std::optional<QImage> take(const QJsonObject &scene, QSize size, const GeneratorContext &c) {
    const TimeUs step = c.fps.frameDuration();
    if (step <= 0 || size.isEmpty())
      return std::nullopt;
    std::unique_lock lock(m_mutex);
    std::optional<std::shared_future<QImage>> ready;
    for (auto it = m_jobs.begin(); it != m_jobs.end();) {
      const bool same = it->size == size && it->scene == scene;
      if (same && it->time == c.time)
        ready = it->image;
      if (same && it->time <= c.time)
        it = m_jobs.erase(it);
      else
        ++it;
    }
    // Playing: this scene was last asked for a moment before (playback skips frames when it falls behind).
    auto seen = std::find_if(m_seen.begin(), m_seen.end(), [&](const Seen &v) { return v.size == size && v.scene == scene; });
    const bool playing = seen != m_seen.end() && c.time > seen->time && c.time - seen->time <= 8 * step;
    if (seen == m_seen.end()) {
      if (m_seen.size() >= 8)
        m_seen.pop_front();
      m_seen.push_back({scene, size, c.time});
    } else {
      seen->time = c.time;
    }
    if (playing) {
      const int ahead = std::clamp(QThread::idealThreadCount() - 2, 1, 6);
      for (int k = 1; k <= ahead; ++k) {
        const TimeUs t = c.time + k * step;
        if (std::any_of(m_jobs.begin(), m_jobs.end(), [&](const Job &j) { return j.time == t && j.size == size && j.scene == scene; }))
          continue;
        if (m_pending >= 2 * ahead) // (never more than a little work waiting)
          break;
        auto promise = std::make_shared<std::promise<QImage>>();
        m_jobs.push_back({scene, size, t, promise->get_future().share()});
        ++m_pending;
        pool().start([this, promise, scene, size, t] {
          QImage image(size, QImage::Format_ARGB32_Premultiplied);
          image.fill(Qt::transparent);
          const bool was = motion::setDraft(true);
          drawScene(image, scene, double(t) / kUsPerSecond);
          motion::setDraft(was);
          promise->set_value(std::move(image));
          --m_pending;
        });
      }
      while (m_jobs.size() > 12) // (several scenes on screen at once each keep their own)
        m_jobs.pop_front();
    }
    lock.unlock();
    if (!ready)
      return std::nullopt;
    return ready->get();
  }

private:
  struct Job {
    QJsonObject scene;
    QSize size;
    TimeUs time = 0;
    std::shared_future<QImage> image;
  };
  static QThreadPool &pool() {
    static QThreadPool *threads = [] {
      auto *p = new QThreadPool; // (kept for the program's life: its threads keep their open video decoders)
      p->setMaxThreadCount(std::clamp(QThread::idealThreadCount() - 2, 1, 6));
      p->setExpiryTimeout(-1);
      return p;
    }();
    return *threads;
  }
  struct Seen {
    QJsonObject scene;
    QSize size;
    TimeUs time = 0;
  };
  std::mutex m_mutex;
  std::deque<Job> m_jobs;
  std::deque<Seen> m_seen;
  std::atomic<int> m_pending{0};
};
Lookahead &lookahead() {
  static Lookahead *ahead = new Lookahead; // (never destroyed: its pool's threads may still be drawing at exit)
  return *ahead;
}
} // namespace
void addGenerators(std::vector<GeneratorDef> &list) {
  GeneratorDef d;
  d.id = "graphics_overlay";
  d.name = "Graphics Overlay";
  d.category = "Graphics";
  d.description =
      "Text and graphics over the picture: titles, lower thirds, cards and "
      "frames, with keyframes. Open Compose Graphics to edit layers; presets "
      "provide broadcast layouts.";
  d.params = {number("opacity", "Opacity", 0, 1, 1)};
  d.render = [](QImage &img, const Params &p, const GeneratorContext &c) {
    const QJsonObject &s = c.data && !c.data->isEmpty() ? *c.data : scene("broadcast");
    QImage drawn;
    if (!c.highQuality && s["version"].toInt() == 2)
      if (auto ahead = lookahead().take(s, img.size(), c))
        drawn = std::move(*ahead);
    if (drawn.isNull()) {
      drawn = QImage(img.size(), QImage::Format_ARGB32_Premultiplied);
      drawn.fill(Qt::transparent);
      const bool was = motion::setDraft(!c.highQuality);
      drawScene(drawn, s, double(c.time) / kUsPerSecond);
      motion::setDraft(was);
    }
    QPainter q(&img);
    q.setOpacity(p.num("opacity"));
    q.drawImage(0, 0, drawn);
  };
  d.isAnimating = [](const Params &, const GeneratorContext &c) {
    if (!c.data)
      return false;
    if ((*c.data)["version"].toInt() == 2)
      return motion::animating(*c.data);
    for (auto v : (*c.data)["layers"].toArray()) {
      auto o = v.toObject();
      if (o.contains("in") || o.contains("out"))
        return true;
      for (auto it = o.begin(); it != o.end(); ++it)
        if (it.value().isObject() &&
            it.value().toObject().contains("keyframes"))
          return true;
    }
    return false;
  };
  d.paint = [](QPainter &painter, QSize size, const Params &p, const GeneratorContext &c) {
    painter.setOpacity(p.num("opacity"));
    const QJsonObject &s = c.data && !c.data->isEmpty() ? *c.data : scene("broadcast");
    // (Painting through OpenGL, the GPU draws each frame faster than the look-ahead draws them on the CPU's cores;
    // MVP_CPU_LOOKAHEAD keeps the look-ahead there too.)
    const bool cpuAhead = qEnvironmentVariableIsSet("MVP_CPU_LOOKAHEAD");
    if (!c.highQuality && s["version"].toInt() == 2 && (cpuAhead || painter.paintEngine()->type() != QPaintEngine::OpenGL2))
      if (auto ahead = lookahead().take(s, size, c)) {
        painter.drawImage(QRect(QPoint(), size), *ahead);
        return true;
      }
    const bool was = motion::setDraft(!c.highQuality);
    paintScene(painter, size, s, double(c.time) / kUsPerSecond);
    motion::setDraft(was);
    return true;
  };
  list.push_back(d);
  d.id = "animation"; // (the same scene drawing, with the full animation tools in its composer)
  d.name = "Animation";
  d.category = "Animation";
  d.description =
      "2D animation: characters with moves, expressions and lip sync, sets and "
      "props, groups, curves, cameras, particles, reusable objects and audio "
      "bindings. Open Compose Animation to edit; presets provide examples.";
  list.push_back(d);
  d = {};
  d.id = "audio_waveform";
  d.paintScale = 2;
  d.name = "Audio Waveform Graphic";
  d.category = "Graphics";
  d.description =
      "A waveform from decoded source audio, stored in the project. Choose "
      "Audio Source, then style bars, a filled envelope or a line; enable a "
      "playhead or scrolling window. Does not add or change audio.";
  d.params = {number("x", "Left", -1, 2, .1),
              number("y", "Top", -1, 2, .72),
              number("width", "Width", .01, 2, .8),
              number("height", "Height", .01, 2, .18),
              number("amplitude", "Amplitude", .01, 10, 1),
              number("thickness", "Line width (pixels)", 1, 20, 2),
              number("spacing", "Bar spacing (pixels)", 1, 30, 4),
              tint("color", "Wave colour", 0x66dddf),
              tint("played_color", "Played / playhead colour", 0xffffff),
              tint("background", "Panel colour", 0x091322),
              number("background_opacity", "Panel opacity", 0, 1, .85),
              flag("stereo", "Separate stereo channels", false),
              flag("normalize", "Normalize visual height", true),
              flag("progress", "Show playback progress", true),
              flag("scroll", "Scrolling window", false),
              number("window", "Window (seconds)", .1, 3600, 10),
              number("rate", "Playback rate", .05, 20, 1),
              number("offset", "Playhead offset (seconds)", 0, 3600, 0)};
  auto style = number("style", "Style", 0, 2, 0);
  style.type = ParamType::Choice;
  style.choices = {"Bars", "Filled envelope", "Line"};
  d.params.push_back(style);
  d.render = [](QImage &image, const Params &p, const GeneratorContext &c) {
    QPainter painter(&image);
    paintWave(painter, image.size(), p, c);
  };
  d.paint = [](QPainter &painter, QSize size, const Params &p, const GeneratorContext &c) {
    paintWave(painter, size, p, c);
    return true;
  };
  d.isAnimating = [](const Params &p, const GeneratorContext &c) {
    return c.data && validateWaveform(*c.data).isEmpty() &&
           (p.flag("progress") || p.flag("scroll"));
  };
  list.push_back(d);
}
void addPresets(std::vector<GeneratorPreset> &list) {
  for (const auto &id : presets()) {
    GeneratorPreset p;
    const bool animation = id.startsWith("motion_");
    p.id = animation ? "animation_" + id.mid(7) : "graphics_" + id;
    p.name = id == "broadcast"     ? "Broadcast Frame"
             : id == "lower_third" ? "Broadcast Lower Third"
             : id == "info_card"   ? "Information Card"
             : id == "timeline"    ? "History Timeline"
             : animation ? QString("Animation: %1").arg(id.mid(7).replace('_', ' '))
                         : "Blank Graphics Canvas";
    p.category = animation ? "Animation" : "Graphics";
    p.generator = animation ? "animation" : "graphics_overlay";
    p.texts["preset"] = id;
    p.length = 10 * kUsPerSecond;
    list.push_back(p);
  }
  GeneratorPreset blank;
  blank.id = "animation_blank";
  blank.name = "Blank Animation";
  blank.category = "Animation";
  blank.generator = "animation";
  blank.texts["preset"] = "motion_blank";
  blank.length = 10 * kUsPerSecond;
  list.push_back(blank);
  GeneratorPreset p;
  p.id = "audio_waveform";
  p.name = "Audio Waveform Graphic";
  p.category = "Graphics";
  p.generator = "audio_waveform";
  p.length = 10 * kUsPerSecond;
  list.push_back(p);
}
} // namespace graphics
