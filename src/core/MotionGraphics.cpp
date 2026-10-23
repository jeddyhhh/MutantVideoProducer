#include "core/MotionGraphics.h"
#include "core/Graphics.h"
#include "core/Effects.h"
#include "core/FrameConverter.h"
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOpenGLPaintDevice>
#include <QMap>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QRawFont>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <private/qopenglpaintengine_p.h>

namespace motion {
namespace {
constexpr double pi = 3.14159265358979323846;
bool finite(const QJsonValue &v) {
  return v.isDouble() && std::isfinite(v.toDouble()) &&
         std::abs(v.toDouble()) <= 1e7;
}
int maximumCount(QJsonValue v, int fallback, int cap) {
  if (v.isUndefined())
    return fallback;
  if (v.isDouble())
    return std::clamp(int(v.toDouble()), 0, cap);
  auto o = v.toObject();
  if (o.contains("keyframes")) {
    int maximum = 0;
    for (auto k : o["keyframes"].toArray())
      maximum = std::max(maximum, int(k.toArray()[1].toDouble()));
    return std::clamp(maximum, 0, cap);
  }
  return cap;
}
QJsonValue interpolate(const QJsonValue &a, const QJsonValue &b, double u) {
  if (a.isDouble() && b.isDouble())
    return a.toDouble() + (b.toDouble() - a.toDouble()) * u;
  if (a.isString() && b.isString()) {
    QColor ca(a.toString()), cb(b.toString());
    if (ca.isValid() && cb.isValid()) {
      QColor c;
      c.setRgbF(
          std::clamp(ca.redF() + (cb.redF() - ca.redF()) * u, 0., 1.),
          std::clamp(ca.greenF() + (cb.greenF() - ca.greenF()) * u, 0., 1.),
          std::clamp(ca.blueF() + (cb.blueF() - ca.blueF()) * u, 0., 1.),
          std::clamp(ca.alphaF() + (cb.alphaF() - ca.alphaF()) * u, 0., 1.));
      return c.name(QColor::HexArgb);
    }
  }
  if (a.isArray() && b.isArray() && a.toArray().size() == b.toArray().size()) {
    QJsonArray out, aa = a.toArray(), bb = b.toArray();
    for (int i = 0; i < aa.size(); ++i)
      out.append(interpolate(aa[i], bb[i], u));
    return out;
  }
  if (a.isObject() && b.isObject()) {
    QJsonObject out = a.toObject(), bb = b.toObject();
    for (auto it = out.begin(); it != out.end(); ++it)
      if (bb.contains(it.key()))
        it.value() = interpolate(it.value(), bb[it.key()], u);
    return out;
  }
  return u < 1 ? a : b; // Text, font and discrete settings change on their key.
}
double audioLevel(const QJsonObject &binding, const QJsonObject &scene,
                  double time) {
  auto audio =
      scene["audio"].toObject()[binding["audio"].toString()].toObject();
  double at = time * audio["rate"].toDouble(1) + audio["offset"].toDouble();
  if (const QString wanted = binding["viseme"].toString(); !wanted.isEmpty()) {
    // The mouth shape being spoken: 1 for the layer of that shape, 0 for the
    // others. Quiet is "rest"; with sound, the shape from the words (a track
    // binding's "visemes", [[seconds, shape], ...]), or "A" without them.
    QJsonObject loud = binding;
    loud.remove("viseme");
    loud["channel"] = "peak";
    loud["attack"] = .02;
    loud["release"] = .05;
    QString now = audioLevel(loud, scene, time) < binding["quiet"].toDouble(.05)
                      ? QStringLiteral("rest")
                      : QStringLiteral("A");
    const auto shapes = audio["visemes"].toArray();
    if (now != "rest" && !shapes.isEmpty()) {
      auto after = std::upper_bound(shapes.begin(), shapes.end(), at,
                                    [](double x, const QJsonValue &v) {
                                      return x < v.toArray()[0].toDouble();
                                    });
      now = after == shapes.begin()
                ? QStringLiteral("rest")
                : (after - 1)->toArray()[1].toString(QStringLiteral("rest"));
    }
    return now == wanted ? 1 : 0;
  }
  const auto mode = binding["channel"].toString("peak");
  if (mode == "beat") {
    double level = 0, release = std::max(.001, binding["release"].toDouble(.2));
    for (auto beat : audio["beats"].toArray())
      if (at >= beat.toDouble())
        level = std::max(level, std::exp(-(at - beat.toDouble()) / release));
    return level;
  }
  auto left = audio[mode == "right" ? "right" : "left"].toArray(),
       right = audio["right"].toArray();
  const double duration = audio["duration"].toDouble();
  if (left.isEmpty() || duration <= 0 || at < 0 || at >= duration)
    return 0;
  const double attack = std::max(.001, binding["attack"].toDouble(.03)),
               release = std::max(.001, binding["release"].toDouble(.12));
  double peak = 0;
  // A bounded, time-based envelope; no state to drift on seek/export.
  for (int k = 0; k < 24; ++k) {
    const double age = release * k / 23., sample = at - age;
    if (sample < 0)
      continue;
    int i = std::clamp(int(sample / duration * left.size()), 0,
                       int(left.size()) - 1);
    double v = left[i].toDouble();
    if (mode == "peak" && !right.isEmpty())
      v = std::max(v, right[std::min(i, int(right.size()) - 1)].toDouble());
    peak = std::max(peak, v * std::exp(-age / release));
  }
  const double ahead = std::min(duration - 1e-8, at + attack);
  int i =
      std::clamp(int(ahead / duration * left.size()), 0, int(left.size()) - 1);
  return std::clamp(std::max(peak, left[i].toDouble() * .5), 0., 1.);
}
QString checkValue(const QJsonValue &v, int depth = 0) {
  if (depth > 24)
    return "Animation data is nested too deeply.";
  if (v.isDouble() && !finite(v))
    return "Use finite values with magnitude at most 10 million.";
  if (v.isString() && v.toString().size() > 20000)
    return "Text is limited to 20,000 characters.";
  if (v.isArray()) {
    if (v.toArray().size() > 8192)
      return "Array is too large.";
    for (auto x : v.toArray())
      if (auto e = checkValue(x, depth + 1); !e.isEmpty())
        return e;
  }
  if (v.isObject()) {
    auto o = v.toObject();
    if (o.contains("easing")) {
      QJsonArray keys;
      keys.append(QJsonArray{0, 0, o["easing"]});
      auto spec = QJsonObject{{"keyframes", keys}};
      if (auto error = checkValue(spec, depth + 1); !error.isEmpty())
        return error;
    }
    if (o.contains("keyframes")) {
      auto ks = o["keyframes"].toArray();
      double previous = -1;
      if (!o["keyframes"].isArray() || ks.isEmpty() || ks.size() > 256)
        return "Use 1–256 keyframes.";
      for (auto k : ks) {
        auto a = k.toArray();
        if ((a.size() != 2 && a.size() != 3) || !finite(a[0]) ||
            a[0].toDouble() < 0 || a[0].toDouble() <= previous)
          return "Keys must be unique, sorted [seconds,value,easing?] entries.";
        previous = a[0].toDouble();
        if (a.size() == 3) {
          auto e = a[2];
          const QStringList names = {"linear",   "hold",        "ease_in",
                                     "ease_out", "ease_in_out", "smooth",
                                     "bounce",   "elastic"};
          if (e.isString() && !names.contains(e.toString()))
            return "Unknown easing curve.";
          if (e.isArray()) {
            auto b = e.toArray();
            if (b.size() != 4)
              return "Bezier easing needs four numbers.";
            for (auto n : b)
              if (!finite(n))
                return "Invalid Bezier easing.";
            if (b[0].toDouble() < 0 || b[0].toDouble() > 1 ||
                b[2].toDouble() < 0 || b[2].toDouble() > 1)
              return "Bezier time controls must be between zero and one.";
          } else if (!e.isString())
            return "Easing must be a name or cubic Bezier array.";
        }
      }
      if (o.contains("loop") &&
          (!finite(o["loop"]) || o["loop"].toDouble() <= 0 ||
           o["loop"].toDouble() > 3600))
        return "Loop duration must be 0–3600 seconds.";
    }
    for (auto it = o.begin(); it != o.end(); ++it)
      if (auto e = checkValue(it.value(), depth + 1); !e.isEmpty())
        return e;
  }
  return {};
}
QString checkCommands(const QJsonValue &v) {
  if (v.isObject()) {
    if (!v.toObject().contains("keyframes"))
      return "Paths need commands or keyframed commands.";
    QJsonArray signature;
    for (auto key : v.toObject()["keyframes"].toArray()) {
      auto a = key.toArray();
      if (auto e = checkCommands(a[1]); !e.isEmpty())
        return e;
      QJsonArray s;
      for (auto cmd : a[1].toArray()) {
        auto c = cmd.toArray();
        s.append(QJsonArray{c[0], c.size()});
      }
      if (!signature.isEmpty() && signature != s)
        return "Morph paths must have matching command types and counts.";
      signature = s;
    }
    return {};
  }
  auto a = v.toArray();
  if (!v.isArray() || a.isEmpty() || a.size() > 512)
    return "Paths need 1–512 M/L/Q/C/Z commands.";
  if (a.first().toArray().isEmpty() ||
      a.first().toArray().first().toString() != "M")
    return "A path must begin with M.";
  for (auto command : a) {
    auto c = command.toArray();
    if (c.isEmpty())
      return "Empty path command.";
    auto op = c.first().toString();
    int n = op == "M" || op == "L" ? 3
            : op == "Q"            ? 5
            : op == "C"            ? 7
            : op == "Z"            ? 1
                                   : 0;
    if (!n || c.size() != n)
      return "Path commands: M x y, L x y, Q cx cy x y, C c1x c1y c2x c2y x y, "
             "Z.";
    for (int j = 1; j < n; ++j)
      if (!finite(c[j]) || std::abs(c[j].toDouble()) > 16)
        return "Path coordinates must be finite fractions between -16 and 16.";
  }
  return {};
}
} // namespace
double ease(double x, const QJsonValue &curve) {
  x = std::clamp(x, 0., 1.);
  if (curve.isArray()) {
    auto a = curve.toArray();
    if (a.size() != 4)
      return x;
    auto bez = [](double t, double a, double b) {
      double s = 1 - t;
      return 3 * s * s * t * a + 3 * s * t * t * b + t * t * t;
    };
    double lo = 0, hi = 1;
    for (int i = 0; i < 24; ++i) {
      double t = (lo + hi) / 2;
      if (bez(t, a[0].toDouble(), a[2].toDouble()) < x)
        lo = t;
      else
        hi = t;
    }
    return bez((lo + hi) / 2, a[1].toDouble(), a[3].toDouble());
  }
  auto n = curve.toString("linear");
  if (n == "hold")
    return x < 1 ? 0 : 1;
  if (n == "ease_in")
    return x * x * x;
  if (n == "ease_out")
    return 1 - std::pow(1 - x, 3);
  if (n == "ease_in_out")
    return x < .5 ? 4 * x * x * x : 1 - std::pow(-2 * x + 2, 3) / 2;
  if (n == "smooth")
    return x * x * (3 - 2 * x);
  if (n == "bounce") {
    double a = 7.5625, b = 2.75;
    if (x < 1 / b)
      return a * x * x;
    if (x < 2 / b) {
      x -= 1.5 / b;
      return a * x * x + .75;
    }
    if (x < 2.5 / b) {
      x -= 2.25 / b;
      return a * x * x + .9375;
    }
    x -= 2.625 / b;
    return a * x * x + .984375;
  }
  if (n == "elastic")
    return x == 0 || x == 1
               ? x
               : std::pow(2, -10 * x) * std::sin((x * 10 - .75) * 2 * pi / 3) +
                     1;
  return x;
}
QJsonValue evaluate(const QJsonValue &v, double time,
                    const QJsonObject &scene) {
  thread_local int depth = 0;
  if (depth >= 32)
    return {};
  struct Guard {
    int &n;
    Guard(int &n) : n(n) { ++n; }
    ~Guard() { --n; }
  } guard(depth);
  if (v.isObject()) {
    auto o = v.toObject();
    if (o.contains("subtitle")) { // the words of the line playing on that binding's track (or none)
      const double hold = o.value("hold").toDouble(0.3);
      for (auto line : scene["audio"].toObject()[o["subtitle"].toString()].toObject()["lines"].toArray()) {
        const QJsonArray l = line.toArray();
        if (time >= l[0].toDouble() && time < l[1].toDouble() + hold)
          return o.value("upper").toBool() ? l[2].toString().toUpper() : l[2].toString();
      }
      return QString();
    }
    if (o.contains("audio"))
      return o["base"].toDouble() +
             o["amount"].toDouble(1) * audioLevel(o, scene, time);
    if (o.contains("variable") && o.contains("equals")) // 1 while a variable (a character's mood) has that value, else 0
      return evaluate(scene["variables"].toObject()[o["variable"].toString()], time, scene) == o["equals"] ? 1 : 0;
    if (o.contains("variable")) {
      auto value = evaluate(
          scene["variables"].toObject()[o["variable"].toString()], time, scene);
      return value.isDouble()
                 ? QJsonValue(value.toDouble() + o["offset"].toDouble())
                 : value;
    }
    if (o.contains("keyframes")) {
      auto ks = o["keyframes"].toArray();
      if (ks.isEmpty())
        return {};
      double loop = o["loop"].toDouble();
      if (loop > 0 && time >= 0) {
        double t = std::fmod(time, loop * (o["ping_pong"].toBool() ? 2 : 1));
        time = t > loop ? loop * 2 - t : t;
      }
      auto first = ks.first().toArray();
      if (time <= first[0].toDouble())
        return first[1];
      for (int i = 1; i < ks.size(); ++i) {
        auto a = ks[i - 1].toArray(), b = ks[i].toArray();
        if (time <= b[0].toDouble())
          return interpolate(
              a[1], b[1],
              ease((time - a[0].toDouble()) /
                       std::max(1e-8, b[0].toDouble() - a[0].toDouble()),
                   a.size() > 2 ? a[2] : o["easing"]));
      }
      return ks.last().toArray()[1];
    }
    QJsonObject out;
    for (auto it = o.begin(); it != o.end(); ++it)
      out[it.key()] = evaluate(it.value(), time, scene);
    return out;
  }
  if (v.isArray()) {
    QJsonArray out;
    for (auto x : v.toArray())
      out.append(evaluate(x, time, scene));
    return out;
  }
  return v;
}
bool animating(const QJsonValue &v) {
  if (v.isObject()) {
    auto o = v.toObject();
    if (o.contains("keyframes") || o.contains("audio") || o.contains("subtitle") ||
        o.contains("variable") || o.contains("in") || o.contains("out") ||
        o.contains("follow") || o.contains("text_animation") ||
        o["type"] == "particles" || o["type"] == "repeater")
      return true;
    for (auto it = o.begin(); it != o.end(); ++it)
      if (animating(it.value()))
        return true;
  } else if (v.isArray())
    for (auto x : v.toArray())
      if (animating(x))
        return true;
  return false;
}
QPainterPath path(const QJsonArray &commands) {
  QPainterPath p;
  for (auto v : commands) {
    auto a = v.toArray();
    QString op = a.first().toString();
    if (op == "M" && a.size() == 3)
      p.moveTo(a[1].toDouble(), a[2].toDouble());
    else if (op == "L" && a.size() == 3)
      p.lineTo(a[1].toDouble(), a[2].toDouble());
    else if (op == "Q" && a.size() == 5)
      p.quadTo(a[1].toDouble(), a[2].toDouble(), a[3].toDouble(),
               a[4].toDouble());
    else if (op == "C" && a.size() == 7)
      p.cubicTo(a[1].toDouble(), a[2].toDouble(), a[3].toDouble(),
                a[4].toDouble(), a[5].toDouble(), a[6].toDouble());
    else if (op == "Z")
      p.closeSubpath();
  }
  return p;
}
QString validate(const QJsonObject &s) {
  if (s["version"].toInt() != 2 || !s["layers"].isArray())
    return "Animation scenes need version: 2 and layers.";
  if (QJsonDocument(s).toJson(QJsonDocument::Compact).size() > 4 * 1024 * 1024)
    return "Scene limit: 4 MB.";
  if (auto e = checkValue(s); !e.isEmpty())
    return e;
  QMap<QString, QJsonObject> ids;
  QSet<QString> symbols;
  int count = 0;
  QVector<QMap<QString, QJsonObject>> prototypeScopes;
  auto definitions = s["symbols"].toObject();
  for (auto it = definitions.begin(); it != definitions.end(); ++it)
    symbols.insert(it.key());
  std::function<QString(QJsonArray, int, QString)> layers =
      [&](QJsonArray a, int depth, QString inherited) -> QString {
    if (depth > 16)
      return "Layer nesting limit: 16.";
    for (auto v : a) {
      if (!v.isObject() || ++count > kMaxLayers)
        return QStringLiteral("Scene limit: %1 layers (including reusable definitions).").arg(kMaxLayers);
      auto o = v.toObject();
      auto type = o["type"].toString();
      if (!QStringList{"rect", "ellipse", "line", "text", "image", "video", "path",
                       "group", "instance", "repeater", "particles", "chart", "noise", "flash"}
               .contains(type))
        return "Unknown animation layer type.";
      auto id = o["id"].toString();
      if (id.startsWith('$'))
        return "Layer IDs beginning with $ are reserved.";
      if (id.isEmpty())
        id = "$" + QString::number(count);
      if (ids.contains(id))
        return "Layer IDs must be unique across the scene.";
      if (!o.contains("parent") && !inherited.isEmpty())
        o["parent"] = inherited;
      ids[id] = o;
      if (type == "path")
        if (auto e = checkCommands(o["commands"]); !e.isEmpty())
          return e;
      if (type == "chart") { // (the renderer drew an unknown dataset as nothing and cut rows after 128 silently)
        if (!o["data"].isArray() && o.contains("dataset") &&
            !s["datasets"].toObject().contains(o["dataset"].toString()))
          return "Chart dataset \"" + o["dataset"].toString() + "\" isn't in the scene's datasets.";
        const auto rows = o["data"].isArray() ? o["data"].toArray()
                                              : s["datasets"].toObject()[o["dataset"].toString()].toArray();
        if (rows.size() > 128)
          return "Charts: up to 128 rows.";
      }
      if (o.contains("children") && !o["children"].isArray())
        return "children must be a layer array.";
      if (o.contains("children")) {
        if (o["type"] == "group") {
          if (auto e = layers(o["children"].toArray(), depth + 1, id);
              !e.isEmpty())
            return e;
        } else {
          auto outer = ids;
          ids.clear();
          if (auto e = layers(o["children"].toArray(), depth + 1, {});
              !e.isEmpty())
            return e;
          prototypeScopes.append(ids);
          ids = outer;
        }
      }
      if (type == "instance" && !symbols.contains(o["symbol"].toString()))
        return "Instance references an unknown reusable symbol.";
      if (type == "repeater" || type == "particles") {
        auto n = o.value("count");
        int cap = type == "particles" ? 2048 : 128;
        auto valid = [&](QJsonValue x) {
          return finite(x) && x.toDouble() >= 0 && x.toDouble() <= cap;
        };
        if (!n.isUndefined()) {
          if (n.isDouble()) {
            if (!valid(n))
              return "Particles: up to 2048; repeaters: up to 128 copies.";
          } else if (n.isObject()) {
            auto spec = n.toObject();
            if (spec.contains("keyframes")) {
              for (auto k : spec["keyframes"].toArray())
                if (!valid(k.toArray()[1]))
                  return "Animated counts exceed the supported range.";
            } else if (!spec.contains("audio") && !spec.contains("variable"))
              return "Counts need a number or animation binding.";
          } else
            return "Counts need numeric values.";
        }
      }
      if (o.contains("commands") && type != "path")
        if (auto e = checkCommands(o["commands"]); !e.isEmpty())
          return e;
      if (auto mask = o["mask"];
          mask.isObject() && mask.toObject()["type"] == "path")
        if (auto e = checkCommands(mask.toObject()["commands"]); !e.isEmpty())
          return e;
      for (auto property : {"fill", "stroke"})
        if (o.contains(property)) {
          auto c = o[property];
          if (c.isString() && !QColor(c.toString()).isValid())
            return "Use valid fill/stroke colours.";
          if (c.isObject() && c.toObject().contains("keyframes"))
            for (auto k : c.toObject()["keyframes"].toArray())
              if (!QColor(k.toArray()[1].toString()).isValid())
                return "Animated colours must be valid colours.";
        }
    }
    return {};
  };
  if (auto e = layers(s["layers"].toArray(), 0, {}); !e.isEmpty())
    return e;
  // Definitions may contain local IDs; do not mix their namespaces with the
  // main scene.
  auto mainIds = ids;
  QVector<QMap<QString, QJsonObject>> scopes{mainIds};
  for (auto it = definitions.begin(); it != definitions.end(); ++it) {
    ids.clear();
    if (!it.value().isObject() || !it.value().toObject()["layers"].isArray())
      return "Symbols need a layers array.";
    if (auto e = layers(it.value().toObject()["layers"].toArray(), 0, {});
        !e.isEmpty())
      return e;
    scopes.append(ids);
  }
  scopes += prototypeScopes;
  for (auto scope : scopes) {
    ids = scope;
    for (auto it = ids.begin(); it != ids.end(); ++it) {
      auto parent = it.value()["parent"].toString();
      if (!parent.isEmpty() &&
          (!ids.contains(parent) || ids[parent]["type"] != "group"))
        return "Parent must reference a group layer ID.";
      auto follow = it.value()["follow"].toObject()["path"].toString();
      if (!follow.isEmpty() &&
          (!ids.contains(follow) || ids[follow]["type"] != "path"))
        return "Path following must reference a path layer ID.";
    }
    QSet<QString> visiting, done;
    std::function<bool(QString)> cycle = [&](QString id) {
      if (done.contains(id))
        return false;
      if (visiting.contains(id))
        return true;
      visiting.insert(id);
      auto o = ids[id];
      for (auto next :
           {o["parent"].toString(), o["follow"].toObject()["path"].toString()})
        if (!next.isEmpty() && cycle(next))
          return true;
      visiting.remove(id);
      done.insert(id);
      return false;
    };
    for (auto it = ids.begin(); it != ids.end(); ++it)
      if (cycle(it.key()))
        return "Parent/path references must not form a cycle.";
  }
  auto camera = s["camera"].toObject()["follow"].toString();
  if (!camera.isEmpty() && !mainIds.contains(camera))
    return "Camera follows an unknown layer ID.";
  // Bound expanded symbol/repeater work and reject recursive symbols before
  // rendering.
  int expanded = 0;
  QSet<QString> stack;
  std::function<bool(QJsonArray, int)> expansion = [&](QJsonArray a,
                                                       int depth) {
    if (depth > 16)
      return false;
    for (auto v : a) {
      auto o = v.toObject();
      int repeats =
          o["type"] == "repeater" ? maximumCount(o.value("count"), 8, 128) : 1;
      for (int i = 0; i < repeats; ++i) {
        if (++expanded > 4096)
          return false;
        if (!expansion(o["children"].toArray(), depth + 1))
          return false;
        auto symbol = o["symbol"].toString();
        if (!symbol.isEmpty()) {
          if (stack.contains(symbol) || !definitions.contains(symbol))
            return false;
          stack.insert(symbol);
          bool ok = expansion(
              definitions[symbol].toObject()["layers"].toArray(), depth + 1);
          stack.remove(symbol);
          if (!ok)
            return false;
        }
      }
    }
    return true;
  };
  if (!expansion(s["layers"].toArray(), 0))
    return "Recursive symbols or too much expanded work (4096 layers / depth "
           "16).";
  for (auto it = definitions.begin(); it != definitions.end(); ++it) {
    stack = {it.key()};
    if (!expansion(it.value().toObject()["layers"].toArray(), 0))
      return "Recursive or excessively expanded symbol.";
  }
  auto audio = s["audio"].toObject();
  for (auto it = audio.begin(); it != audio.end(); ++it)
    if (auto e = graphics::validateWaveform(it.value().toObject());
        !e.isEmpty())
      return "Audio binding: " + e;
  return {};
}
namespace {
double number(const QJsonObject &o, const char *key, double fallback, double t,
              const QJsonObject &s) {
  auto v = evaluate(o[key], t, s);
  return std::clamp(v.toDouble(fallback), -1e7, 1e7);
}
QColor colour(const QJsonValue &v, double t, const QJsonObject &s,
              QString fallback = "#ffffff") {
  QColor c(evaluate(v, t, s).toString(fallback));
  return c.isValid() ? c : QColor(fallback);
}
QPainterPath inBox(const QPainterPath &p, const QRectF &b) {
  QTransform m;
  m.translate(b.x(), b.y());
  m.scale(b.width(), b.height());
  return m.map(p);
}
QPainterPath shape(const QJsonObject &o, const QRectF &b, double t,
                   const QJsonObject &s) {
  QPainterPath p;
  QString type = o["type"].toString();
  if (type == "ellipse")
    p.addEllipse(b);
  else if (type == "path")
    p = inBox(path(evaluate(o["commands"], t, s).toArray()), b);
  else if (type == "line") {
    p.moveTo(b.topLeft());
    p.lineTo(b.bottomRight());
  } else
    p.addRoundedRect(b, number(o, "radius", 0, t, s) * b.height(),
                     number(o, "radius", 0, t, s) * b.height());
  return p;
}
QBrush brush(const QJsonObject &o, const QRectF &b, double t,
             const QJsonObject &s) {
  auto g = evaluate(o["gradient"], t, s).toObject();
  if (g.isEmpty())
    return colour(o["fill"], t, s);
  QGradientStops stops;
  for (auto v : g["stops"].toArray()) {
    auto a = v.toArray();
    if (a.size() == 2)
      stops.append(
          {std::clamp(a[0].toDouble(), 0., 1.), QColor(a[1].toString())});
  }
  if (g["type"] == "radial") {
    QRadialGradient grad(QPointF(b.x() + b.width() * g["x1"].toDouble(.5),
                                 b.y() + b.height() * g["y1"].toDouble(.5)),
                         std::max(1., b.width() * g["radius"].toDouble(.6)));
    grad.setStops(stops);
    return grad;
  }
  QLinearGradient grad(b.x() + b.width() * g["x1"].toDouble(),
                       b.y() + b.height() * g["y1"].toDouble(),
                       b.x() + b.width() * g["x2"].toDouble(1),
                       b.y() + b.height() * g["y2"].toDouble(1));
  grad.setStops(stops);
  return grad;
}
QPainterPath portion(const QPainterPath &p, double a, double b) {
  a = std::clamp(a, 0., 1.);
  b = std::clamp(b, 0., 1.);
  if (a == 0 && b == 1)
    return p;
  QPainterPath out;
  double length = p.length();
  if (b <= a || length <= 0)
    return out;
  out.moveTo(p.pointAtPercent(p.percentAtLength(a * length)));
  int samples = std::clamp(int((b - a) * 256), 2, 256);
  for (int i = 1; i <= samples; ++i)
    out.lineTo(p.pointAtPercent(
        p.percentAtLength((a + (b - a) * i / samples) * length)));
  return out;
}
void decoration(QPainter &p, const QPainterPath &body, const QJsonObject &style,
                const QSizeF &size, double t, const QJsonObject &s, bool glow) {
  if (style.isEmpty())
    return;
  auto o = evaluate(style, t, s).toObject();
  QColor c(o["color"].toString(glow ? "#53e3d0" : "#000000"));
  p.save();
  p.translate(o["x"].toDouble() * size.width(),
              o["y"].toDouble() * size.height());
  double blur = std::clamp(o["blur"].toDouble(.008), 0., .08) * size.height(),
         opacity = std::clamp(o["opacity"].toDouble(.4), 0., 1.);
  if (blur > 0)
    for (int i = 8; i >= 1; --i) {
      QColor soft = c;
      soft.setAlphaF(c.alphaF() * opacity * .08 * (9 - i));
      p.setPen(
          QPen(soft, blur * i / 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      p.setBrush(Qt::NoBrush);
      p.drawPath(body);
    }
  c.setAlphaF(c.alphaF() * opacity);
  p.setPen(Qt::NoPen);
  p.setBrush(c);
  p.drawPath(body);
  p.restore();
}
// The exact average of several pictures (motion blur's moments), summed pixel by pixel.
struct Average {
  std::vector<quint32> sum;
  QSize size;
  int count = 0;
  void add(const QImage &image) {
    const QImage im = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (sum.empty()) {
      size = im.size();
      sum.assign(size_t(size.width()) * size.height() * 4, 0);
    }
    for (int y = 0; y < size.height(); ++y) {
      const quint32 *line = reinterpret_cast<const quint32 *>(im.constScanLine(y));
      quint32 *s = sum.data() + size_t(y) * size.width() * 4;
      for (int x = 0; x < size.width(); ++x)
        for (int c = 0; c < 4; ++c)
          s[x * 4 + c] += (line[x] >> (c * 8)) & 0xff;
    }
    ++count;
  }
  QImage result() const {
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < size.height(); ++y) {
      quint32 *line = reinterpret_cast<quint32 *>(out.scanLine(y));
      const quint32 *s = sum.data() + size_t(y) * size.width() * 4;
      for (int x = 0; x < size.width(); ++x) {
        quint32 v = 0;
        for (int c = 0; c < 4; ++c)
          v |= quint32((s[x * 4 + c] + quint32(count / 2)) / quint32(std::max(1, count))) << (c * 8);
        line[x] = v;
      }
    }
    return out;
  }
};
// Draft rendering (the editor's preview): motion blur takes fewer moments. Set per render thread by the graphics
// generator from its context's quality.
thread_local bool draftRender = false;
// A video layer's picture at `seconds` into its file. Decoders are kept between frames in one pool that the render
// threads borrow from (not one per thread: a thread's own were freed as it ended, inside Windows' loader lock, where
// FFmpeg's decoding threads can't finish, and closing the program hung).
QImage videoFrame(const QString &file, double seconds, bool loop, QSize want) {
  struct Source {
    std::unique_ptr<VideoDecoder> decoder;
    FrameConverter converter;
    double duration = 0;
    bool failed = false;
  };
  struct Pool {
    std::mutex mutex;
    std::multimap<QString, std::unique_ptr<Source>> idle;
  };
  static Pool *pool = new Pool; // (never destroyed: the program's end frees it)
  std::unique_ptr<Source> s;
  {
    std::lock_guard lock(pool->mutex);
    if (auto it = pool->idle.find(file); it != pool->idle.end()) {
      s = std::move(it->second);
      pool->idle.erase(it);
    }
  }
  if (!s) {
    s = std::make_unique<Source>();
    const MediaInfo info = probeMedia(file);
    s->decoder = std::make_unique<VideoDecoder>();
    if (info.video.empty() || !s->decoder->open(file, info.video.front().index, nullptr, false)) {
      s->decoder.reset();
      s->failed = true;
    } else
      s->duration = double(info.duration) / kUsPerSecond;
  }
  QImage picture;
  if (s->decoder) {
    if (loop && s->duration > 0)
      seconds = std::fmod(std::max(0., seconds), s->duration);
    if (const AVFrame *frame = s->decoder->frameAt(TimeUs(std::clamp(seconds, 0., 86400.) * kUsPerSecond)))
      picture = s->converter.convert(frame, want, !draftRender);
  }
  std::unique_ptr<Source> spare; // (freed outside the lock)
  {
    std::lock_guard lock(pool->mutex);
    pool->idle.emplace(file, std::move(s));
    if (pool->idle.size() > 16) { // (a few files at a time; the rest open again when needed)
      auto other = std::find_if(pool->idle.begin(), pool->idle.end(), [&](const auto &e) { return e.first != file; });
      if (other == pool->idle.end())
        other = pool->idle.begin();
      spare = std::move(other->second);
      pool->idle.erase(other);
    }
  }
  return picture;
}
// A layer's own video effects ([{"type": id, "params": {setting: value}}]), each with its settings, for a picture of
// `picture` pixels.
void eachLayerEffect(const QJsonArray &effects, double t, double scale, QSize picture,
                     const std::function<void(const EffectDef &, const Params &, const RenderContext &)> &apply) {
  for (const auto &v : effects) {
    const QJsonObject e = v.toObject();
    const EffectDef *def = findEffect(e.value("type").toString());
    if (!def || !def->apply)
      continue;
    ParamMap values = makeEffect(*def, 0).params;
    const QJsonObject given = e.value("params").toObject();
    for (const ParamDef &d : def->params) {
      const QJsonValue x = given.value(d.id);
      if (x.isUndefined())
        continue;
      double value = x.toDouble();
      if (x.isBool())
        value = x.toBool() ? 1 : 0;
      else if (x.isString() && d.type == ParamType::Choice)
        value = std::max<qsizetype>(0, d.choices.indexOf(x.toString()));
      else if (x.isString() && d.type == ParamType::Color)
        value = double(QColor(x.toString()).rgb() & 0xffffffu);
      else if (x.isString())
        continue;
      values[d.id].value = d.type == ParamType::Color ? value : std::clamp(value, d.min, d.max);
    }
    RenderContext ctx;
    ctx.time = TimeUs(t * kUsPerSecond);
    ctx.timelineTime = ctx.time;
    ctx.scale = scale;
    ctx.highQuality = !draftRender;
    ctx.contentRect = QRectF(QPointF(), QSizeF(picture));
    apply(*def, resolveParams(def->params, values, ctx.time), ctx);
  }
}
// ...on a picture on the CPU.
void layerEffects(QImage &image, const QJsonArray &effects, double t, double scale) {
  eachLayerEffect(effects, t, scale, image.size(), [&](const EffectDef &def, const Params &params, const RenderContext &ctx) {
    std::unique_ptr<EffectState> state; // (each frame fresh: effects that remember earlier frames start over)
    image.bits();
    def.apply(image, params, ctx, state);
  });
}
// One box blur pass of `r` pixels along rows (or columns) of a premultiplied image.
void boxPass(QImage &image, int r, bool rows) {
  const int w = image.width(), h = image.height();
  const int lines = rows ? h : w, length = rows ? w : h;
  std::vector<quint32> line(static_cast<size_t>(length));
  for (int k = 0; k < lines; ++k) {
    auto px = [&](int i) -> quint32 & {
      return rows ? reinterpret_cast<quint32 *>(image.scanLine(k))[i]
                  : reinterpret_cast<quint32 *>(image.scanLine(i))[k];
    };
    for (int i = 0; i < length; ++i)
      line[size_t(i)] = px(i);
    int sum[4] = {0, 0, 0, 0};
    auto add = [&](int i, int sign) {
      const quint32 v = line[size_t(std::clamp(i, 0, length - 1))];
      for (int c = 0; c < 4; ++c)
        sum[c] += sign * int((v >> (c * 8)) & 0xff);
    };
    for (int i = -r; i <= r; ++i)
      add(i, 1);
    const int span = 2 * r + 1;
    for (int i = 0; i < length; ++i) {
      quint32 out = 0;
      for (int c = 0; c < 4; ++c)
        out |= quint32(sum[c] / span) << (c * 8);
      px(i) = out;
      add(i + r + 1, 1);
      add(i - r, -1);
    }
  }
}
// A soft blur of `radius` pixels: shrunk, box blurred three times each way,
// and grown back (cheap enough for a whole frame).
// The part of a picture that has anything in it (empty: nothing drawn).
QRect drawnBounds(const QImage &image) {
  int top = image.height(), bottom = -1, left = image.width(), right = -1;
  for (int y = 0; y < image.height(); ++y) {
    const auto *row = reinterpret_cast<const quint32 *>(image.constScanLine(y));
    int first = 0;
    while (first < image.width() && !(row[first] >> 24))
      ++first;
    if (first == image.width())
      continue;
    int last = image.width() - 1;
    while (!(row[last] >> 24))
      --last;
    top = std::min(top, y);
    bottom = y;
    left = std::min(left, first);
    right = std::max(right, last);
  }
  return bottom < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
}
QImage blurred(const QImage &image, double radius) {
  if (radius < .5 || image.isNull())
    return image;
  const double shrink = std::max(1., radius / 3);
  QImage small =
      image
          .scaled(std::max(1, int(image.width() / shrink)),
                  std::max(1, int(image.height() / shrink)),
                  Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
          .convertToFormat(QImage::Format_ARGB32_Premultiplied);
  const int r = std::max(1, int(std::lround(radius / shrink / 1.7)));
  for (int pass = 0; pass < 3; ++pass) {
    boxPass(small, r, true);
    boxPass(small, r, false);
  }
  return small.scaled(image.size(), Qt::IgnoreAspectRatio,
                      Qt::SmoothTransformation);
}
QString substitute(QString words, const QJsonObject &s, double t) {
  QRegularExpression re("\\{\\{([A-Za-z0-9_.-]+)\\}\\}");
  auto matches = re.globalMatch(words);
  QList<QRegularExpressionMatch> found;
  while (matches.hasNext())
    found.prepend(matches.next());
  for (auto m : found) {
    auto v = evaluate(s["variables"].toObject()[m.captured(1)], t, s);
    QString value = v.isString()   ? v.toString()
                    : v.isDouble() ? QString::number(v.toDouble(), 'g', 8)
                    : v.isBool()   ? (v.toBool() ? "true" : "false")
                                   : QString();
    words.replace(m.capturedStart(), m.capturedLength(), value);
  }
  return words;
}
struct Pose {
  QRectF box;
  QTransform world;
};
struct Node {
  QJsonObject o;
  QString id, parent;
  QVector<int> children;
};
void applyBlend(QPainter &p, const QString &mode) {
  if (mode == "add")
    p.setCompositionMode(QPainter::CompositionMode_Plus);
  else if (mode == "screen")
    p.setCompositionMode(QPainter::CompositionMode_Screen);
  else if (mode == "multiply")
    p.setCompositionMode(QPainter::CompositionMode_Multiply);
}
// ---- The scene's own pictures on the GPU (GpuPictures) -----------------------------------------------------------
thread_local GpuPictures *gpuPictures = nullptr;
thread_local int cpuPictureCount = 0;
// The GPU's pictures for a painter: only while it paints through OpenGL (a QImage's painter makes QImages).
GpuPictures *gpuFor(QPainter &p) {
  if (!gpuPictures || !p.paintEngine() || p.paintEngine()->type() != QPaintEngine::OpenGL2 || !QOpenGLContext::currentContext())
    return nullptr;
  return gpuPictures;
}
// A picture made on the CPU where the GPU could have (counted for cpuPictures()).
void cpuPicture(QPainter &p) {
  if (gpuPictures && p.paintEngine() && p.paintEngine()->type() == QPaintEngine::OpenGL2)
    ++cpuPictureCount;
}
// GL work between a painter's own drawing (the painter takes up its state again after).
struct Native {
  QPainter &p;
  explicit Native(QPainter &p) : p(p) { p.beginNativePainting(); }
  ~Native() { p.endNativePainting(); }
};
// Paints into a picture with `p`'s render hints (row 0 at the top, as the GPU compositor's textures).
void paintInto(const GpuPictures::Picture &picture, QPainter &p, const std::function<void(QPainter &)> &draw) {
  FramebufferPaintDevice device(picture.size, picture.fbo);
  QPainter q(&device);
  q.setRenderHints(p.renderHints());
  draw(q);
}
// A picture drawn by `p` with its top left at `at` (the picture's pixels: no world transform), with p's opacity,
// blend and clip.
void drawPicture(QPainter &p, const GpuPictures::Picture &picture, QPoint at = {}) {
  p.save();
  p.resetTransform();
  const QSize s = picture.size;
  static_cast<QOpenGL2PaintEngineEx *>(p.paintEngine())->drawTexture(QRectF(at, QSizeF(s)), picture.tex, s, QRectF(0, s.height(), s.width(), -s.height()));
  p.restore();
}
// The exact average of `samples` pictures drawn by `draw` (motion blur's moments), summed in a float picture.
GpuPictures::Picture average(GpuPictures &gpu, QPainter &p, QSize pixels, int samples, const std::function<void(QPainter &, int)> &draw) {
  GpuPictures::Picture sum;
  {
    Native n(p);
    sum = gpu.acquire(pixels, true);
  }
  FramebufferPaintDevice device(sum.size, sum.fbo);
  QPainter add(&device);
  add.setCompositionMode(QPainter::CompositionMode_Plus);
  add.setOpacity(1. / samples);
  for (int j = 0; j < samples; ++j) {
    GpuPictures::Picture moment;
    {
      Native n(add);
      moment = gpu.acquire(pixels, false);
    }
    paintInto(moment, p, [&](QPainter &mp) { draw(mp, j); });
    drawPicture(add, moment);
    Native n(add);
    gpu.release(moment);
  }
  return sum;
}
class Graph {
public:
  QJsonObject scene;
  QSizeF size;
  QVector<Node> nodes;
  QMap<QString, int> ids;
  QVector<int> roots;
  int &budget;
  int unblurred = -1; // the layer being drawn into its blur's own picture
  double speakerTime = NAN; // the frame's own time while motion blur draws its moments (the speaker camera cuts once)
  Graph(QJsonObject scene, QSizeF size, int &budget)
      : scene(std::move(scene)), size(size), budget(budget) {
    collect(this->scene["layers"].toArray(), {});
    for (int i = 0; i < nodes.size(); ++i) {
      auto parent = nodes[i].parent;
      if (parent.isEmpty() || !ids.contains(parent))
        roots.append(i);
      else
        nodes[ids[parent]].children.append(i);
    }
  }
  void collect(QJsonArray a, QString inherited) {
    for (auto v : a) {
      Node n;
      n.o = v.toObject();
      n.id = n.o["id"].toString();
      if (n.id.isEmpty())
        n.id = "$" + QString::number(nodes.size());
      n.parent = n.o["parent"].toString(inherited);
      int i = nodes.size();
      nodes.append(n);
      ids[n.id] = i;
      if (n.o["type"] == "group")
        collect(n.o["children"].toArray(), n.id);
    }
  }
  QTransform childSpace(const Pose &pose) const {
    QTransform local;
    local.translate(pose.box.x(), pose.box.y());
    local.scale(pose.box.width() / size.width(),
                pose.box.height() / size.height());
    return local * pose.world;
  }
  Pose pose(int i, double t, int depth = 0) const {
    if (depth > 16 || i < 0 || i >= nodes.size())
      return {};
    const auto &o = nodes[i].o;
    bool group = o["type"] == "group";
    QRectF b(number(o, "x", 0, t, scene) * size.width(),
             number(o, "y", 0, t, scene) * size.height(),
             std::clamp(number(o, "width", group ? 1 : .3, t, scene), 0., 4.) *
                 size.width(),
             std::clamp(number(o, "height", group ? 1 : .1, t, scene), 0., 4.) *
                 size.height());
    QTransform parent;
    if (ids.contains(nodes[i].parent))
      parent = childSpace(pose(ids[nodes[i].parent], t, depth + 1));
    auto follow = o["follow"].toObject();
    double angle = number(o, "rotation", 0, t, scene);
    auto ref = follow["path"].toString();
    if (!ref.isEmpty() && ids.contains(ref)) {
      auto target = pose(ids[ref], t, depth + 1);
      auto route =
          target.world.map(shape(nodes[ids[ref]].o, target.box, t, scene));
      double progress = number(follow, "progress", 0, t, scene);
      progress = follow["loop"].toBool() ? progress - std::floor(progress)
                                         : std::clamp(progress, 0., 1.);
      double u = route.percentAtLength(route.length() * progress);
      QPointF at = parent.inverted().map(route.pointAtPercent(u));
      b.moveCenter(
          at +
          QPointF(number(follow, "offset_x", 0, t, scene) * size.width(),
                  number(follow, "offset_y", 0, t, scene) * size.height()));
      if (follow["orient"].toBool(true)) {
        double du = progress >= .999 ? -1e-4 : 1e-4;
        QPointF next =
            parent.inverted().map(route.pointAtPercent(route.percentAtLength(
                route.length() * std::clamp(progress + du, 0., 1.))));
        QPointF tangent = (next - at) * (du < 0 ? -1 : 1);
        angle += std::atan2(tangent.y(), tangent.x()) * 180 / pi +
                 number(follow, "angle_offset", 0, t, scene);
      }
    }
    QPointF pivot(b.x() + b.width() * number(o, "anchor_x", .5, t, scene),
                  b.y() + b.height() * number(o, "anchor_y", .5, t, scene));
    QTransform local;
    local.translate(pivot.x(), pivot.y());
    local.rotate(angle);
    local.scale(std::clamp(number(o, "scale_x", 1, t, scene), -100., 100.),
                std::clamp(number(o, "scale_y", 1, t, scene), -100., 100.));
    local.translate(-pivot.x(), -pivot.y());
    return {b, local * parent};
  }
  QTransform camera(double t, const QTransform &base) const {
    auto c = scene["camera"].toObject();
    if (c.isEmpty())
      return base;
    QPointF center(number(c, "x", .5, t, scene) * size.width(),
                   number(c, "y", .5, t, scene) * size.height());
    auto follow = c["follow"].toString();
    if (ids.contains(follow)) {
      auto target = pose(ids[follow], t);
      center = target.world.map(target.box.center());
      center += QPointF(number(c, "offset_x", 0, t, scene) * size.width(),
                        number(c, "offset_y", 0, t, scene) * size.height());
    }
    double zoom = number(c, "zoom", 1, t, scene);
    if (auto speaker = c["speaker"].toObject(); !speaker.isEmpty()) {
      // Speaker camera: the layer whose id is a voice binding's name (any
      // case: a character's group) and that has been loudest over the last
      // moment is framed head and shoulders; nobody talking, the keyed shot.
      // Its shots [[from, to, kind]] change that for a while: "wide" (the
      // keyed shot), "listener" (whoever spoke last before the speaker: a
      // reaction), "two" (speaker and listener together), "speaker".
      const double window = std::clamp(speaker["window"].toDouble(.5), .05, 3.);
      const double now = std::isfinite(speakerTime) ? speakerTime : t;
      QString kind = "speaker";
      double shotFrom = 0, shotTo = 0;
      for (auto v : speaker["shots"].toArray()) {
        const QJsonArray shot = v.toArray();
        if (now >= shot[0].toDouble() && now < shot[1].toDouble()) {
          kind = shot[2].toString("speaker");
          shotFrom = shot[0].toDouble();
          shotTo = shot[1].toDouble();
        }
      }
      const QJsonObject audio = scene["audio"].toObject();
      QVector<QPair<QString, QString>> cast; // (layer id, binding)
      for (auto it = audio.begin(); it != audio.end(); ++it)
        for (auto k = ids.constBegin(); k != ids.constEnd(); ++k)
          if (k.key().compare(it.key(), Qt::CaseInsensitive) == 0)
            cast.append({k.key(), it.key()});
      auto level = [&](const QString &binding, double from, double to, int samples) {
        const QJsonObject b{{"audio", binding}, {"attack", .02}, {"release", .1}};
        double sum = 0;
        for (int k = 0; k < samples; ++k)
          sum += audioLevel(b, scene, std::max(0., to - (to - from) * k / std::max(1, samples - 1))) / samples;
        return sum;
      };
      const double quiet = speaker["quiet"].toDouble(.06);
      double best = quiet;
      QString who;
      for (const auto &[id, binding] : cast)
        if (const double l = level(binding, now - window, now, 6); l > best) {
          best = l;
          who = id;
        }
      // A pause in a line, or between lines: the shot holds on whoever spoke last when someone speaks again within
      // `hold` seconds, instead of a glimpse of the wide shot.
      if (const double hold = std::clamp(speaker["hold"].toDouble(1.2), 0., 5.); who.isEmpty() && hold > 0) {
        auto peak = [&](const QString &binding, double from, double to) {
          const QJsonObject b{{"audio", binding}, {"attack", .02}, {"release", .1}};
          double most = 0;
          for (int k = 0; k <= 12; ++k)
            most = std::max(most, audioLevel(b, scene, std::max(0., from + (to - from) * k / 12)));
          return most;
        };
        QString before;
        double loudest = quiet;
        bool again = false;
        for (const auto &[id, binding] : cast) {
          if (const double l = peak(binding, now - hold, now); l > loudest) {
            loudest = l;
            before = id;
          }
          again = again || peak(binding, now, now + hold) > quiet;
        }
        if (again)
          who = before;
      }
      QString other; // the listener: who spoke most in the seconds before the shot
      if (kind == "listener" || kind == "two") {
        // Both chosen once for the whole shot: frame by frame, the speaker dropped out at a pause and the listener
        // swapped between the people who had spoken, and the camera jumped between them.
        const int samples = std::clamp(int((shotTo - shotFrom) * 12), 6, 96);
        double loudest = quiet;
        QString talker;
        for (const auto &[id, binding] : cast)
          if (const double l = level(binding, shotFrom, shotTo, samples); l > loudest) {
            loudest = l;
            talker = id;
          }
        if (!talker.isEmpty())
          who = talker;
        double most = quiet * .5;
        for (const auto &[id, binding] : cast)
          if (id != who)
            if (const double l = level(binding, shotFrom - 8, shotFrom, 96); l > most) {
              most = l;
              other = id;
            }
      }
      auto head = [&](const QString &id) {
        const auto target = pose(ids[id], t);
        return target.world.map(QPointF(target.box.center().x(), target.box.top() + target.box.height() * .3));
      };
      const QString framed = kind == "listener" ? (other.isEmpty() ? who : other) : kind == "wide" ? QString() : who;
      if (!framed.isEmpty()) {
        zoom = std::clamp(speaker["zoom"].toDouble(1.7), 1., 6.);
        center = head(framed);
        if (kind == "two" && !other.isEmpty()) { // both heads, with room around them
          const QPointF a = head(who), b = head(other);
          center = (a + b) / 2;
          const double spanX = std::abs(a.x() - b.x()) + size.width() * .3, spanY = std::abs(a.y() - b.y()) + size.height() * .4;
          zoom = std::clamp(std::min({zoom, size.width() / spanX, size.height() / spanY}), 1., 6.);
        }
        // (kept inside the picture)
        const double halfW = size.width() / 2 / zoom, halfH = size.height() / 2 / zoom;
        center.setX(std::clamp(center.x(), halfW, size.width() - halfW));
        center.setY(std::clamp(center.y(), halfH, size.height() - halfH));
      }
    }
    QTransform m;
    m.translate(size.width() / 2, size.height() / 2);
    m.scale(std::clamp(zoom, .01, 100.), std::clamp(zoom, .01, 100.));
    m.rotate(-number(c, "rotation", 0, t, scene));
    m.translate(-center.x(), -center.y());
    return m * base;
  }
  void mask(QPainter &p, const QJsonObject &o, const Pose &at, double t) const {
    auto m = o["mask"];
    if (m.isString() && ids.contains(m.toString())) {
      auto target = pose(ids[m.toString()], t);
      QPainterPath clip = at.world.inverted().map(target.world.map(
          shape(nodes[ids[m.toString()]].o, target.box, t, scene)));
      p.setClipPath(clip, Qt::IntersectClip);
    } else if (m.isObject()) {
      auto v = evaluate(m, t, scene).toObject();
      QRectF b(at.box.x() + at.box.width() * v["x"].toDouble(),
               at.box.y() + at.box.height() * v["y"].toDouble(),
               at.box.width() * v["width"].toDouble(1),
               at.box.height() * v["height"].toDouble(1));
      auto clip = shape(v, b, t, scene);
      if (v["invert"].toBool()) {
        QPainterPath all;
        all.addRect(QRectF(QPointF(), size));
        clip = all.subtracted(clip);
      }
      p.setClipPath(clip, Qt::IntersectClip);
    }
  }
  void drawText(QPainter &p, const QJsonObject &o, const QRectF &b, double t) {
    QString words =
        substitute(evaluate(o["text"], t, scene).toString(), scene, t);
    if (words.isEmpty())
      return;
    words.replace('\n', QChar(0x2028));
    QFont font(evaluate(o["font"], t, scene).toString("Segoe UI"));
    font.setPixelSize(std::clamp(
        int(number(o, "font_size", .035, t, scene) * size.height()), 1, 4096));
    font.setBold(evaluate(o["bold"], t, scene).toBool());
    font.setLetterSpacing(QFont::AbsoluteSpacing,
                          number(o, "tracking", 0, t, scene) * size.height());
    QTextLayout layout(words, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    auto align = evaluate(o["align"], t, scene).toString();
    option.setAlignment(align == "center"  ? Qt::AlignHCenter
                        : align == "right" ? Qt::AlignRight
                                           : Qt::AlignLeft);
    layout.setTextOption(option);
    layout.beginLayout();
    double y = 0;
    QVector<QTextLine> lines;
    while (lines.size() < 256) {
      auto line = layout.createLine();
      if (!line.isValid())
        break;
      line.setLineWidth(std::max(1., b.width()));
      line.setPosition({0, y});
      y += line.height() * number(o, "line_spacing", 1, t, scene);
      lines.append(line);
    }
    layout.endLayout();
    p.save();
    p.setClipRect(b, Qt::IntersectClip);
    double top = b.y() + std::max(0., (b.height() - y) / 2);
    const auto anim = o["text_animation"].toObject();
    QVector<QPair<int, int>> units;
    if (anim["unit"] == "line")
      for (auto l : lines)
        units.append({l.textStart(), l.textLength()});
    else if (anim["unit"] == "word") {
      QRegularExpression re("\\S+");
      auto matches = re.globalMatch(words);
      while (matches.hasNext() && units.size() < 2048) {
        auto m = matches.next();
        units.append({m.capturedStart(), m.capturedLength()});
      }
    } else {
      QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, words);
      int a = 0, boundary;
      while ((boundary = finder.toNextBoundary()) != -1 &&
             units.size() < 2048) {
        units.append({a, boundary - a});
        a = boundary;
      }
    }
    auto fill = brush(o, b, t, scene);
    // Glyphs where the layout put them (its wrapping and alignment): drawing
    // the text again with addText spaces it a little wider, so animated
    // words ran into each other and centred text sat off centre.
    auto glyphs = [&](QPainterPath &path, const QTextLine &line, int first, int length) {
      for (const QGlyphRun &run : line.glyphRuns(first, length)) {
        const QRawFont raw = run.rawFont();
        const auto indexes = run.glyphIndexes();
        const auto positions = run.positions();
        for (qsizetype g = 0; g < indexes.size(); ++g)
          path.addPath(raw.pathForGlyph(indexes[g]).translated(QPointF(b.x(), top) + positions[g]));
      }
    };
    if (anim.isEmpty()) {
      QPainterPath all;
      for (auto line : lines)
        glyphs(all, line, line.textStart(), line.textLength());
      decoration(p, all, o["shadow"].toObject(), size, t, scene, false);
      decoration(p, all, o["glow"].toObject(), size, t, scene, true);
      p.setPen(Qt::NoPen);
      p.setBrush(fill);
      p.drawPath(all);
    } else
      for (int j = 0; j < units.size() && budget > 0; ++j) {
        auto [start, length] = units[j];
        QTextLine line = layout.lineForTextPosition(start);
        if (!line.isValid())
          continue;
        double age = t - number(anim, "delay", 0, t, scene) -
                     j * number(anim, "stagger", .05, t, scene),
               duration =
                   std::max(.001, number(anim, "duration", .3, t, scene));
        if (anim["loop"].toDouble() > 0 && age >= 0)
          age = std::fmod(age, anim["loop"].toDouble());
        double u = ease(age / duration, anim["easing"].isUndefined()
                                            ? QJsonValue("ease_out")
                                            : anim["easing"]);
        if (age < 0)
          continue;
        auto style = anim["style"].toString("fade");
        QPointF at(b.x() + line.cursorToX(start),
                   top + line.y() + line.ascent());
        QPainterPath glyph;
        for (auto part : lines) {
          int first = std::max(start, part.textStart());
          int last =
              std::min(start + length, part.textStart() + part.textLength());
          if (last > first)
            glyphs(glyph, part, first, last - first);
        }
        p.save();
        p.setOpacity(p.opacity() *
                     (style == "typewriter" ? 1 : std::clamp(u, 0., 1.)));
        if (style == "rise")
          p.translate(0, (1 - u) * number(anim, "distance", .03, t, scene) *
                             size.height());
        if (style == "scale") {
          p.translate(at);
          p.scale(std::max(.001, u), std::max(.001, u));
          p.translate(-at);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawPath(glyph);
        p.restore();
        --budget;
      }
    p.restore();
  }
  // Generated texture over the box: TV static, film grain or drifting fog, new each frame (rate a second), the same every
  // time for the same seed.
  void drawNoise(QPainter &p, const QJsonObject &o, const QRectF &b, double t) {
    auto hash = [](uint32_t x, uint32_t y, uint32_t seed) {
      uint32_t h = x * 374761393u + y * 668265263u + seed * 2246822519u;
      h = (h ^ (h >> 13)) * 1274126177u;
      return (h ^ (h >> 16)) / double(0xffffffffu);
    };
    const QString style = o["style"].toString("static");
    const uint32_t seed = uint32_t(o["seed"].toInt(1));
    const double amount = std::clamp(number(o, "amount", style == "static" ? 1 : .5, t, scene), 0., 1.);
    QColor tint = o.contains("fill") ? colour(o["fill"], t, scene) : QColor(Qt::white);
    if (b.width() < 1 || b.height() < 1 || amount <= 0)
      return;
    QImage cells;
    if (style == "fog") {
      // Two octaves of smooth value noise, drifting sideways.
      const int w = 48, h = std::max(4, int(48 * b.height() / b.width()));
      cells = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
      const double drift = t * number(o, "speed", .05, t, scene), scale = std::clamp(number(o, "scale", 3, t, scene), .2, 20.);
      auto smooth = [&](double x, double y) {
        const double fx = std::floor(x), fy = std::floor(y), ux = x - fx, uy = y - fy;
        const double sx = ux * ux * (3 - 2 * ux), sy = uy * uy * (3 - 2 * uy);
        auto at = [&](double i, double j) { return hash(uint32_t(int64_t(i) & 0xffff), uint32_t(int64_t(j) & 0xffff), seed); };
        return (at(fx, fy) * (1 - sx) + at(fx + 1, fy) * sx) * (1 - sy) + (at(fx, fy + 1) * (1 - sx) + at(fx + 1, fy + 1) * sx) * sy;
      };
      for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
          const double u = double(x) / w * scale + drift * scale, v = double(y) / h * scale * h / w;
          const double n = std::clamp((smooth(u, v) * .65 + smooth(u * 2.1 + 7, v * 2.1 + 3) * .35 - .3) * 1.6, 0., 1.) * amount;
          QColor c = tint;
          c.setAlphaF(c.alphaF() * n);
          cells.setPixelColor(x, y, c);
        }
    } else {
      const double grain = std::clamp(number(o, "grain", style == "static" ? .004 : .002, t, scene), .0005, .1) * size.height();
      const int w = std::clamp(int(b.width() / grain), 1, 960), h = std::clamp(int(b.height() / grain), 1, 540);
      const uint32_t frame = uint32_t(std::floor(t * std::clamp(number(o, "rate", 24, t, scene), 1., 60.)));
      cells = QImage(w, h, QImage::Format_ARGB32_Premultiplied);
      for (int y = 0; y < h; ++y) {
        auto *row = reinterpret_cast<QRgb *>(cells.scanLine(y));
        for (int x = 0; x < w; ++x) {
          const double n = hash(uint32_t(x), uint32_t(y), seed * 7919u + frame);
          if (style == "static") { // snow: grey levels, fully covering
            const int g = int(n * 255 * tint.lightnessF() + .5);
            row[x] = qPremultiply(qRgba(g, g, g, int(255 * amount)));
          } else { // grain: light and dark specks over the picture
            const double a = std::abs(n - .5) * 2 * amount;
            const int g = n > .5 ? 255 : 0;
            row[x] = qPremultiply(qRgba(g, g, g, int(a * 160)));
          }
        }
      }
    }
    p.save();
    p.setRenderHint(QPainter::SmoothPixmapTransform, style != "static");
    p.drawImage(b, cells);
    p.restore();
  }
  void drawChart(QPainter &p, const QJsonObject &o, const QRectF &b, double t) {
    auto data =
        o["data"].isArray()
            ? o["data"].toArray()
            : scene["datasets"].toObject()[o["dataset"].toString()].toArray();
    if (data.isEmpty())
      return;
    int count = std::min(128, int(data.size()));
    double maximum = 0, total = 0;
    QVector<double> values;
    for (int i = 0; i < count; ++i) {
      double v = std::max(0., number(data[i].toObject(), "value", 0, t, scene));
      values.append(v);
      maximum = std::max(maximum, v);
      total += v;
    }
    maximum = std::max(1e-8, number(o, "maximum", maximum, t, scene));
    double progress = std::clamp(number(o, "progress", 1, t, scene), 0., 1.);
    QString style = o["style"].toString("bar");
    auto font = p.font();
    font.setPixelSize(std::clamp(
        int(number(o, "font_size", .022, t, scene) * size.height()), 1, 512));
    p.setFont(font);
    if (style == "donut") {
      double angle = -90;
      QRectF circle(b.x() + (b.width() - std::min(b.width(), b.height())) / 2,
                    b.y(), std::min(b.width(), b.height()),
                    std::min(b.width(), b.height()));
      for (int i = 0; i < count && budget > 0; ++i) {
        double span = total > 0 ? values[i] / total * 360 * progress : 0;
        QPainterPath segment;
        segment.arcMoveTo(circle, angle);
        segment.arcTo(circle, angle, -span);
        QColor c = colour(data[i].toObject()["color"], t, scene,
                          i % 2 ? "#ffb35c" : "#53e3d0");
        p.setPen(QPen(c, std::max(1., circle.width() * .12), Qt::SolidLine,
                      Qt::FlatCap));
        p.setBrush(Qt::NoBrush);
        p.drawPath(segment);
        angle -= span;
        --budget;
      }
    } else {
      QPainterPath line;
      double cell = b.width() / count, plot = b.height() * .78;
      for (int i = 0; i < count && budget > 0; ++i) {
        auto item = data[i].toObject();
        double h = std::clamp(values[i] / maximum, 0., 1.) * plot * progress;
        double x = b.x() + cell * i;
        QColor c =
            colour(item["color"], t, scene, i % 2 ? "#679fff" : "#53e3d0");
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        if (style == "line") {
          QPointF at(x + cell / 2, b.y() + plot - h);
          if (i == 0)
            line.moveTo(at);
          else
            line.lineTo(at);
          p.drawEllipse(at, 3, 3);
        } else
          p.drawRoundedRect(
              QRectF(x + cell * .1, b.y() + plot - h, cell * .8, h), 3, 3);
        p.setPen(colour(o["text_color"], t, scene));
        p.drawText(QRectF(x, b.y() + plot + 4, cell, b.height() - plot - 4),
                   Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap,
                   substitute(item["label"].toString(), scene, t));
        --budget;
      }
      if (style == "line") {
        p.setPen(QPen(colour(o["fill"], t, scene, "#53e3d0"),
                      std::max(1., number(o, "stroke_width", .003, t, scene) *
                                       size.height())));
        p.setBrush(Qt::NoBrush);
        p.drawPath(line);
      }
    }
  }
  static double random(quint32 seed, int index) {
    quint32 n = seed + 0x9e3779b9u * quint32(index + 1);
    n = (n ^ (n >> 16)) * 0x85ebca6bu;
    n = (n ^ (n >> 13)) * 0xc2b2ae35u;
    n ^= n >> 16;
    return double(n) / 4294967296.;
  }
  void drawParticles(QPainter &p, const QJsonObject &o, const QRectF &b,
                     double t) {
    int count = std::clamp(int(number(o, "count", 100, t, scene)), 0, 2048);
    quint32 seed = quint32(number(o, "seed", 1, t, scene));
    double lifetime =
        std::clamp(number(o, "lifetime", 4, t, scene), .01, 3600.);
    auto colors = o["colors"].toArray();
    for (int i = 0; i < count && budget > 0; ++i) {
      double born = o["emission"] == "burst" ? number(o, "delay", 0, t, scene)
                                             : random(seed, i * 7) * lifetime;
      if (t < born)
        continue;
      double age = t - born;
      if (o["emission"] == "burst" && age > lifetime)
        continue;
      age = std::fmod(age, lifetime);
      double direction = number(o, "direction", 90, t, scene) * pi / 180 +
                         (random(seed, i * 7 + 1) - .5) *
                             number(o, "spread", 40, t, scene) * pi / 180;
      double speed =
          number(o, "speed", .2, t, scene) * (.5 + random(seed, i * 7 + 2));
      QPointF at(b.x() + b.width() * random(seed, i * 7 + 3) +
                     std::cos(direction) * speed * age * b.width(),
                 b.y() + std::sin(direction) * speed * age * b.height() +
                     number(o, "gravity", 0, t, scene) * age * age *
                         b.height() / 2);
      double radius = std::clamp(number(o, "size", .008, t, scene), .0001, .2) *
                      size.height() * (.5 + random(seed, i * 7 + 4));
      QColor c = colors.isEmpty()
                     ? colour(o["fill"], t, scene, "#53e3d0")
                     : QColor(colors[i % colors.size()].toString());
      p.save();
      p.setOpacity(p.opacity() * std::clamp(1 - age / lifetime, 0., 1.));
      p.translate(at);
      p.rotate(random(seed, i * 7 + 5) * 360 +
               age * number(o, "spin", 90, t, scene));
      p.setPen(Qt::NoPen);
      p.setBrush(c);
      if (o["shape"] == "rect")
        p.drawRect(QRectF(-radius, -radius / 2, radius * 2, radius));
      else
        p.drawEllipse(QPointF(), radius, radius);
      p.restore();
      --budget;
    }
  }
  void draw(int i, QPainter &p, double t, const QTransform &camera,
            double opacity = 1, int depth = 0, bool noTrail = false,
            bool isolate = true) {
    if (depth > 16 || budget-- <= 0)
      return;
    auto o = nodes[i].o;
    for (auto it = o.begin(); it != o.end(); ++it)
      if (it.key() != "children" && it.key() != "type" &&
          it.key() != "parent" && it.key() != "id")
        it.value() = evaluate(it.value(), t, scene);
    if (!evaluate(o["visible"], t, scene).toBool(true) ||
        t < number(o, "in", 0, t, scene) ||
        t >= number(o, "out", 1e7, t, scene))
      return;
    if (!noTrail) {
      auto trail = o["trail"].toObject(), blur = o["motion_blur"].toObject();
      if (!trail.isEmpty()) {
        int samples =
            std::clamp(int(number(trail, "samples", 8, t, scene)), 1, 16);
        double duration =
            std::clamp(number(trail, "duration", .3, t, scene), 0., 5.);
        for (int j = samples; j >= 1; --j)
          if (t - duration * j / samples >= 0)
            draw(i, p, t - duration * j / samples, camera,
                 opacity * number(trail, "opacity", .25, t, scene) *
                     (1. - double(j) / (samples + 1)) / samples,
                 depth + 1, true);
      }
      if (!blur.isEmpty()) {
        int samples =
            std::clamp(int(number(blur, "samples", 5, t, scene)), 1, draftRender ? 2 : 8);
        double shutter =
            std::clamp(number(blur, "shutter", .03, t, scene), 0., .2);
        // Each moment drawn on its own, then averaged (drawing them all with
        // Plus also added a layer's overlapping parts together).
        if (GpuPictures *gpu = gpuFor(p)) {
          const GpuPictures::Picture sum = average(*gpu, p, size.toSize(), samples, [&](QPainter &mp, int j) {
            draw(i, mp, std::max(0., t + shutter * (double(j) / std::max(1, samples - 1) - .5)), camera, 1, depth + 1, true);
          });
          p.save();
          p.setOpacity(opacity);
          applyBlend(p, o["blend"].toString());
          drawPicture(p, sum);
          p.restore();
          Native n(p);
          gpu->release(sum);
          return;
        }
        cpuPicture(p);
        Average moments;
        QImage moment(size.toSize(), QImage::Format_ARGB32_Premultiplied);
        for (int j = 0; j < samples; ++j) {
          moment.fill(Qt::transparent);
          {
            QPainter mp(&moment);
            mp.setRenderHints(p.renderHints());
            draw(i, mp, std::max(0., t + shutter * (double(j) / std::max(1, samples - 1) - .5)), camera, 1, depth + 1, true);
          }
          moments.add(moment);
        }
        const QImage accumulated = moments.result();
        p.save();
        p.setWorldTransform({});
        p.setOpacity(opacity);
        applyBlend(p, o["blend"].toString());
        p.drawImage(0, 0, accumulated);
        p.restore();
        return;
      }
    }
    // Invisible at this moment: nothing to blur, isolate or draw (a
    // character's hidden mouth shapes and expressions cost nothing).
    if (isolate && number(o, "opacity", 1, t, scene) <= 0)
      return;
    QString type = o["type"].toString();
    // Softness: the layer's own blur, plus depth of field: with the camera's
    // depth_of_field, a layer's depth (0 the subject, 1 far away) blurs it
    // as the camera zooms in.
    if (i != unblurred) {
      double softness =
          std::clamp(number(o, "blur", 0, t, scene), 0., .2) * size.height();
      auto lens = scene["camera"].toObject();
      if (o.contains("depth") && lens.contains("depth_of_field"))
        softness += std::clamp(number(lens, "depth_of_field", 0, t, scene), 0., 10.) *
                    std::clamp(number(o, "depth", 0, t, scene), 0., 1.) *
                    std::max(0., number(lens, "zoom", 1, t, scene) - 1) *
                    .01 * size.height();
      const QJsonArray effects = o["effects"].toArray();
      if ((softness >= .5 || !effects.isEmpty()) && gpuFor(p)) {
        GpuPictures &gpu = *gpuFor(p);
        GpuPictures::Picture layer;
        {
          Native n(p);
          layer = gpu.acquire(size.toSize(), false);
        }
        paintInto(layer, p, [&](QPainter &lp) {
          const int was = unblurred;
          unblurred = i;
          draw(i, lp, t, camera, 1, depth + 1, true);
          unblurred = was;
        });
        // (as on the CPU below: only the part it covers, with its own box for its effects)
        QRect region;
        GpuPictures::Picture part;
        {
          Native n(p);
          region = gpu.drawnBounds(layer, 8);
          if (!effects.isEmpty()) {
            const auto at = pose(i, t);
            region |= (at.world * camera).mapRect(at.box).toAlignedRect();
          }
          const int spread = int(std::ceil(3 * softness)) + 2;
          region = region.adjusted(-spread, -spread, spread, spread).intersected(QRect(QPoint(), layer.size));
          if (!region.isEmpty()) {
            part = gpu.crop(layer, region);
            gpu.blur(part, softness);
            eachLayerEffect(effects, t, size.height() / 1080., part.size,
                            [&](const EffectDef &def, const Params &params, const RenderContext &ctx) { gpu.effect(part, def, params, ctx); });
          }
          gpu.release(layer);
        }
        if (part) {
          p.save();
          p.setOpacity(opacity);
          drawPicture(p, part, region.topLeft());
          p.restore();
          Native n(p);
          gpu.release(part);
        }
        return;
      }
      if (softness >= .5 || !effects.isEmpty()) {
        cpuPicture(p);
        QImage layer(size.toSize(), QImage::Format_ARGB32_Premultiplied);
        layer.fill(Qt::transparent);
        {
          QPainter lp(&layer);
          lp.setRenderHints(p.renderHints());
          const int was = unblurred;
          unblurred = i;
          draw(i, lp, t, camera, 1, depth + 1, true);
          unblurred = was;
        }
        // Only the part the layer covers is blurred and given its effects
        // (a small soft light costs little); the effects work on that and
        // the layer's own box: a CRT bends the TV's picture, not the room,
        // and noise stays on it.
        QRect region = drawnBounds(layer);
        if (!effects.isEmpty()) {
          const auto at = pose(i, t);
          region |= (at.world * camera).mapRect(at.box).toAlignedRect();
        }
        const int spread = int(std::ceil(3 * softness)) + 2;
        region = region.adjusted(-spread, -spread, spread, spread).intersected(layer.rect());
        if (region.isEmpty())
          return;
        const QPoint corner = region.topLeft();
        layer = blurred(layer.copy(region), softness);
        layerEffects(layer, effects, t, size.height() / 1080.);
        p.save();
        p.setWorldTransform({});
        p.setOpacity(opacity);
        p.drawImage(corner, layer);
        p.restore();
        return;
      }
    }
    double alpha = std::clamp(number(o, "opacity", 1, t, scene), 0., 1.);
    if (isolate && (alpha < 1 || !o["blend"].toString().isEmpty()) &&
        (type == "group" || type == "instance" || type == "repeater")) {
      if (GpuPictures *gpu = gpuFor(p)) {
        GpuPictures::Picture group;
        {
          Native n(p);
          group = gpu->acquire(size.toSize(), false);
        }
        paintInto(group, p, [&](QPainter &gp) { draw(i, gp, t, camera, 1, depth + 1, true, false); });
        p.save();
        p.setOpacity(opacity * alpha);
        applyBlend(p, o["blend"].toString());
        drawPicture(p, group);
        p.restore();
        Native n(p);
        gpu->release(group);
        return;
      }
      cpuPicture(p);
      QImage group(size.toSize(), QImage::Format_ARGB32_Premultiplied);
      group.fill(Qt::transparent);
      {
        QPainter gp(&group);
        gp.setRenderHints(p.renderHints());
        draw(i, gp, t, camera, 1, depth + 1, true, false);
      }
      p.save();
      p.setWorldTransform({});
      p.setOpacity(opacity * alpha);
      applyBlend(p, o["blend"].toString());
      p.drawImage(0, 0, group);
      p.restore();
      return;
    }
    auto at = pose(i, t);
    p.save();
    p.setWorldTransform(at.world * camera);
    p.setOpacity(std::clamp(opacity * (isolate ? alpha : 1), 0., 1.));
    if (isolate)
      applyBlend(p, o["blend"].toString());
    mask(p, o, at, t);
    if (const double shadow = std::clamp(number(o, "contact_shadow", 0, t, scene), 0., 1.); shadow > 0) {
      // A soft shadow where the layer meets the ground (the bottom of its box).
      const QPointF foot(at.box.center().x(), at.box.bottom());
      const double r = at.box.width() * .55;
      QRadialGradient soft(foot, r);
      soft.setColorAt(0, QColor(0, 0, 0, int(170 * shadow)));
      soft.setColorAt(.6, QColor(0, 0, 0, int(80 * shadow)));
      soft.setColorAt(1, QColor(0, 0, 0, 0));
      p.save();
      p.translate(foot);
      p.scale(1, .14);
      p.translate(-foot);
      p.setPen(Qt::NoPen);
      p.setBrush(soft);
      p.drawEllipse(foot, r, r);
      p.restore();
    }
    if (type == "group") {
      for (int child : nodes[i].children)
        draw(child, p, t, camera, p.opacity(), depth + 1);
    } else if (type == "instance" || type == "repeater") {
      auto definitions = scene["symbols"].toObject();
      auto symbol = definitions[o["symbol"].toString()].toObject();
      QJsonArray children = o["children"].toArray();
      if (children.isEmpty())
        children = symbol["layers"].toArray();
      int count = type == "repeater"
                      ? std::clamp(int(number(o, "count", 8, t, scene)), 0, 128)
                      : 1;
      int columns =
          std::clamp(int(number(o, "columns", count, t, scene)), 1, 128);
      for (int j = 0; j < count && budget > 0; ++j) {
        QJsonObject sub = scene;
        sub["layers"] = children;
        sub.remove("camera");
        auto vars = scene["variables"].toObject();
        auto defaults = symbol["variables"].toObject(),
             overrides = o["variables"].toObject();
        for (auto it = defaults.begin(); it != defaults.end(); ++it)
          vars[it.key()] = it.value();
        for (auto it = overrides.begin(); it != overrides.end(); ++it)
          vars[it.key()] = it.value();
        vars["index"] = j;
        sub["variables"] = vars;
        QTransform transform;
        transform.translate(
            (j % columns) *
                number(o, "spacing_x", at.box.width() / size.width(), t,
                       scene) *
                size.width(),
            (j / columns) *
                number(o, "spacing_y", at.box.height() / size.height(), t,
                       scene) *
                size.height());
        transform.rotate(j * number(o, "rotation_step", 0, t, scene));
        Graph graph(sub, size, budget);
        graph.drawRoots(
            p, std::max(0., t - j * number(o, "stagger", 0, t, scene)),
            childSpace(at) * transform * camera, p.opacity(), depth + 1);
      }
    } else if (type == "particles")
      drawParticles(p, o, at.box, t);
    else if (type == "noise")
      drawNoise(p, o, at.box, t);
    else if (type == "flash") {
      // Flashes of colour over the box: at each of its times (and on each beat of a binding), fading over decay seconds.
      const double decay = std::clamp(number(o, "decay", .25, t, scene), .01, 10.);
      double level = 0;
      auto hit = [&](double at) {
        if (t >= at)
          level = std::max(level, std::exp(-3 * (t - at) / decay));
      };
      for (auto v : o["times"].toArray())
        hit(v.toDouble());
      if (o.contains("beats")) {
        const QJsonObject audio = scene["audio"].toObject()[o["beats"].toString()].toObject();
        const double rate = audio["rate"].toDouble(1), offset = audio["offset"].toDouble();
        for (auto v : audio["beats"].toArray())
          hit((v.toDouble() - offset) / std::max(1e-6, rate));
      }
      level *= std::clamp(number(o, "strength", 1, t, scene), 0., 1.);
      if (level > .003) {
        QColor c = o.contains("fill") ? colour(o["fill"], t, scene) : QColor(Qt::white);
        c.setAlphaF(c.alphaF() * level);
        p.fillRect(at.box, c);
      }
    }
    else if (type == "chart")
      drawChart(p, o, at.box, t);
    else if (type == "text")
      drawText(p, o, at.box, t);
    else if (type == "video") {
      // A clip inside the scene: it moves with its group and the camera and takes masks. Picture only (put the clip on a
      // track too for its sound).
      const QSize want = at.box.size().toSize().expandedTo(QSize(2, 2));
      // (source_time, keyframed, remaps it: a TV that slows, rewinds or pauses)
      const double clipTime = o.contains("source_time") ? number(o, "source_time", 0, t, scene)
                                                          : number(o, "source_in", 0, t, scene) + t * number(o, "speed", 1, t, scene);
      const QImage frame = videoFrame(evaluate(o["path"], t, scene).toString(), clipTime,
                                      evaluate(o["loop"], t, scene).toBool(), want.boundedTo(QSize(3840, 2160)));
      if (!frame.isNull()) {
        QSizeF fit = frame.size();
        fit.scale(at.box.size(), o["fit"].toString() == "contain" ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
        p.save();
        p.setClipRect(at.box, Qt::IntersectClip);
        p.drawImage(QRectF(at.box.center() - QPointF(fit.width() / 2, fit.height() / 2), fit), frame);
        p.restore();
      }
    } else if (type == "image") {
      QImageReader reader(evaluate(o["path"], t, scene).toString());
      reader.setAutoTransform(true);
      if (reader.size().width() > 8192 || reader.size().height() > 8192)
        reader.setScaledSize(
            reader.size().scaled(4096, 4096, Qt::KeepAspectRatio));
      QImage image = reader.read();
      if (!image.isNull()) {
        QSizeF fit = image.size();
        fit.scale(at.box.size(), Qt::KeepAspectRatio);
        p.drawImage(
            QRectF(at.box.center() - QPointF(fit.width() / 2, fit.height() / 2),
                   fit),
            image);
      }
    } else {
      auto body = shape(o, at.box, t, scene);
      double stroke =
          std::clamp(number(o, "stroke_width", 0, t, scene), 0., .2) *
          size.height();
      QPen pen(type == "line" ? colour(o["fill"], t, scene)
                              : colour(o["stroke"], t, scene),
               std::max(1., stroke), Qt::SolidLine, Qt::RoundCap,
               Qt::RoundJoin);
      QList<qreal> dashes;
      for (auto d : evaluate(o["stroke_dash"], t, scene).toArray())
        dashes.append(std::max(.01, d.toDouble()));
      if (!dashes.isEmpty())
        pen.setDashPattern(dashes);
      pen.setDashOffset(number(o, "dash_offset", 0, t, scene));
      bool trimmed = type == "path" &&
                     (o.contains("trim_start") || o.contains("trim_end"));
      auto painted = trimmed
                         ? portion(body, number(o, "trim_start", 0, t, scene),
                                   number(o, "trim_end", 1, t, scene))
                         : body;
      // The outline a shadow or glow follows (only then: uniting paths is slow).
      const QJsonObject shadow = o["shadow"].toObject(), glow = o["glow"].toObject();
      QPainterPath silhouette;
      if (!shadow.isEmpty() || !glow.isEmpty()) {
        if (type != "line" && !trimmed &&
            (o.contains("gradient") || colour(o["fill"], t, scene).alpha() > 0))
          silhouette = body;
        if (stroke > 0 || type == "line") {
          QPainterPathStroker stroker;
          stroker.setWidth(std::max(1., stroke));
          stroker.setCapStyle(Qt::RoundCap);
          stroker.setJoinStyle(Qt::RoundJoin);
          silhouette = silhouette.united(stroker.createStroke(painted));
        }
        decoration(p, silhouette, shadow, size, t, scene, false);
        decoration(p, silhouette, glow, size, t, scene, true);
      }
      p.setBrush(brush(o, at.box, t, scene));
      p.setPen(stroke > 0 || type == "line" ? pen : QPen(Qt::NoPen));
      if (trimmed)
        p.setBrush(Qt::NoBrush);
      p.drawPath(painted);
      const double shade = std::clamp(number(o, "shading", 0, t, scene), 0., 1.);
      if (shade > 0 && type != "line" && !trimmed &&
          (o.contains("gradient") || colour(o["fill"], t, scene).alpha() > 0)) {
        // Soft light: a highlight on the side the light comes from (shading_angle,
        // degrees on the screen: 225 the top left, 270 above) fading into a
        // shadow on the other.
        const QRectF b = body.boundingRect();
        const double a = number(o, "shading_angle", 225, t, scene) * pi / 180;
        const QPointF half(std::cos(a) * b.width() / 2, std::sin(a) * b.height() / 2);
        QLinearGradient light(b.center() + half, b.center() - half);
        light.setColorAt(0, QColor(255, 255, 255, int(100 * shade)));
        light.setColorAt(.45, QColor(255, 255, 255, 0));
        light.setColorAt(.55, QColor(0, 0, 0, 0));
        light.setColorAt(1, QColor(0, 0, 0, int(150 * shade)));
        p.save();
        p.setClipPath(body, Qt::IntersectClip);
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        p.drawRect(b);
        p.restore();
      }
    }
    p.restore();
  }
  void drawRoots(QPainter &p, double t, const QTransform &base = {},
                 double opacity = 1, int depth = 0) {
    auto view = camera(t, base);
    for (int i : roots)
      draw(i, p, t, view, opacity, depth);
  }
};
} // namespace
void render(QImage &image, const QJsonObject &scene, double time) {
  QPainter p(&image);
  paint(p, image.size(), scene, time);
}
// A scene is checked once, not on every frame: checking a big one (characters, long voice envelopes) takes longer
// than drawing it. (The same scene is the same shared data, so the comparison is quick.)
bool checked(const QJsonObject &scene) {
  thread_local QJsonObject last;
  thread_local bool valid = false;
  if (last.isEmpty() || scene != last) {
    valid = validate(scene).isEmpty();
    last = scene;
  }
  return valid;
}
void paint(QPainter &p, QSizeF size, const QJsonObject &scene, double time) {
  if (!std::isfinite(time) || !checked(scene))
    return;
  // The scene's own clock: "time" keyed against the event's time plays it backwards, holds it (a freeze) or slows it.
  if (scene.contains("time"))
    time = std::clamp(evaluate(scene["time"], time, scene).toDouble(time), 0., 3600.);
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  int budget = 32768;
  const auto blur = scene["motion_blur"];
  if (blur.isObject() || blur.toBool()) {
    // The whole picture, camera included, averaged over a shutter (seconds)
    // around the time: fast moves, crash zooms and whip pans smear.
    const auto b = blur.toObject();
    const int samples = std::clamp(b["samples"].toInt(5), 2, draftRender ? 2 : 8);
    const double shutter = std::clamp(b["shutter"].toDouble(.02), 0., .2);
    if (GpuPictures *gpu = gpuFor(p)) {
      const GpuPictures::Picture sum = average(*gpu, p, size.toSize(), samples, [&](QPainter &fp, int j) {
        int each = 32768;
        Graph graph(scene, size, each);
        graph.speakerTime = time;
        graph.drawRoots(fp, std::max(0., time + shutter * (double(j) / (samples - 1) - .5)));
      });
      drawPicture(p, sum);
      Native n(p);
      gpu->release(sum);
      return;
    }
    cpuPicture(p);
    Average frames;
    QImage frame(size.toSize(), QImage::Format_ARGB32_Premultiplied);
    for (int j = 0; j < samples; ++j) {
      frame.fill(Qt::transparent);
      {
        QPainter fp(&frame);
        fp.setRenderHints(p.renderHints());
        int each = 32768;
        Graph graph(scene, size, each);
        graph.speakerTime = time;
        graph.drawRoots(fp, std::max(0., time + shutter * (double(j) / (samples - 1) - .5)));
      }
      frames.add(frame);
    }
    p.drawImage(0, 0, frames.result());
    return;
  }
  Graph graph(scene, size, budget);
  graph.drawRoots(p, time, {}, p.opacity());
}
QRectF bounds(const QJsonObject &scene, const QString &id, double time,
              QSizeF size) {
  int budget = 0;
  Graph g(scene, size, budget);
  if (!g.ids.contains(id))
    return {};
  auto pose = g.pose(g.ids[id], time);
  return (pose.world * g.camera(time, {})).mapRect(pose.box);
}
QHash<QString,QRectF> allBounds(const QJsonObject& scene, double time, QSizeF size) {
  int budget=0;Graph graph(scene,size,budget);QHash<QString,QRectF> result;
  auto camera=graph.camera(time,{});
  for(auto it=graph.ids.constBegin();it!=graph.ids.constEnd();++it){auto pose=graph.pose(it.value(),time);result.insert(it.key(),(pose.world*camera).mapRect(pose.box));}
  return result;
}
QVector<PathHandle> handles(const QJsonObject &scene, const QString &id,
                            double time, QSizeF size) {
  int budget = 0;
  Graph g(scene, size, budget);
  QVector<PathHandle> out;
  if (!g.ids.contains(id))
    return out;
  int i = g.ids[id];
  auto pose = g.pose(i, time);
  auto commands = evaluate(g.nodes[i].o["commands"], time, scene).toArray();
  QTransform unit;
  unit.translate(pose.box.x(), pose.box.y());
  unit.scale(pose.box.width(), pose.box.height());
  auto transform = unit * pose.world * g.camera(time, {});
  for (int j = 0; j < commands.size(); ++j) {
    auto a = commands[j].toArray();
    for (int k = 1; k + 1 < a.size(); k += 2)
      out.append(
          {j, k, transform.map(QPointF(a[k].toDouble(), a[k + 1].toDouble()))});
  }
  return out;
}
QPointF editDelta(const QJsonObject &scene, const QString &id, double time,
                  QSizeF size, QPointF delta, bool pathPoint) {
  int budget = 0;
  Graph g(scene, size, budget);
  if (!g.ids.contains(id))
    return delta;
  int i = g.ids[id];
  QTransform transform;
  if (pathPoint) {
    auto pose = g.pose(i, time);
    transform.translate(pose.box.x(), pose.box.y());
    transform.scale(pose.box.width(), pose.box.height());
    transform = transform * pose.world;
  } else if (g.ids.contains(g.nodes[i].parent))
    transform = g.childSpace(g.pose(g.ids[g.nodes[i].parent], time));
  transform = transform * g.camera(time, {});
  bool invertible = false;
  auto inv = transform.inverted(&invertible);
  if (!invertible)
    return {};
  auto v =
      inv.map(QPointF(delta.x() * size.width(), delta.y() * size.height())) -
      inv.map(QPointF());
  return pathPoint ? v : QPointF(v.x() / size.width(), v.y() / size.height());
}
namespace {
QJsonObject item(QString type, QString id, double x, double y, double w,
                 double h, QString fill) {
  return {{"type", type}, {"id", id},   {"name", id},  {"x", x},
          {"y", y},       {"width", w}, {"height", h}, {"fill", fill}};
}
QJsonObject title(QString words, double y = .05) {
  auto o = item("text", "title", .05, y, .90, .09, "#eff7fc");
  o["text"] = words;
  o["font_size"] = .048;
  o["bold"] = true;
  return o;
}
QJsonObject key(std::initializer_list<QJsonArray> entries,
                QString easing = "ease_in_out") {
  QJsonArray a;
  for (auto e : entries)
    a.append(e);
  return {{"keyframes", a}, {"easing", easing}};
}
QJsonObject background() {
  auto o = item("rect", "background", 0, 0, 1, 1, "#08131f");
  o["gradient"] = QJsonObject{{"type", "linear"},
                              {"stops", QJsonArray{QJsonArray{0, "#07131f"},
                                                   QJsonArray{1, "#244964"}}}};
  return o;
}
} // namespace
QStringList presets() {
  return {"motion_race",  "motion_titles",    "motion_chart",
          "motion_route", "motion_particles", "motion_morph",
          "motion_audio", "motion_reusable", "motion_characters"};
}
QJsonObject preset(const QString &name) {
  if (name == "motion_characters")
    return charactersScene();
  QJsonArray layers{background()};
  QJsonObject s{{"version", 2}, {"layers", layers}};
  if (name == "motion_race") {
    layers.append(title("{{headline}}"));
    s["variables"] = QJsonObject{{"headline", "RACE CONTROL / CURVED PATHS"}};
    QJsonArray commands{QJsonArray{"M", .1, .55},
                        QJsonArray{"C", -.05, .15, .25, .03, .42, .23},
                        QJsonArray{"C", .60, .43, .61, .08, .85, .20},
                        QJsonArray{"C", 1.12, .33, 1.02, .78, .75, .79},
                        QJsonArray{"C", .43, .80, .34, 1.02, .1, .55},
                        QJsonArray{"Z"}};
    auto edge = item("path", "track", .08, .20, .84, .68, "#00000000");
    edge["commands"] = commands;
    edge["stroke"] = "#ffb35c";
    edge["stroke_width"] = .09;
    edge["shadow"] = QJsonObject{{"blur", .015}, {"y", .012}, {"opacity", .4}};
    layers.append(edge);
    auto road = edge;
    road["id"] = "road";
    road["name"] = "Asphalt";
    road["stroke"] = "#31475a";
    road["stroke_width"] = .075;
    road.remove("shadow");
    layers.append(road);
    auto markings = road;
    markings["id"] = "markings";
    markings["stroke"] = "#abc3ce";
    markings["stroke_width"] = .002;
    markings["stroke_dash"] = QJsonArray{6, 6};
    layers.append(markings);
    QJsonArray car;
    car.append(item("rect", "rear-wheels", .08, .1, .21, .8, "#050a10"));
    car.append(item("rect", "front-wheels", .72, .1, .20, .8, "#050a10"));
    auto body = item("rect", "body", .12, .22, .78, .56, "#53e3d0");
    body["fill"] = QJsonObject{{"variable", "paint"}};
    body["radius"] = .25;
    car.append(body);
    car.append(item("rect", "cockpit", .40, .34, .26, .32, "#08131f"));
    car.append(item("rect", "nose", .83, .40, .17, .20, "#eff7fc"));
    s["symbols"] = QJsonObject{
        {"car", QJsonObject{{"layers", car},
                            {"variables", QJsonObject{{"paint", "#53e3d0"}}}}}};
    for (int i = 0; i < 3; ++i) {
      auto o = item("instance", QString("car-%1").arg(i), 0, 0, .055, .045,
                    "#ffffff");
      o["symbol"] = "car";
      o["variables"] = QJsonObject{
          {"paint", QStringList{"#53e3d0", "#ffb35c", "#679fff"}[i]}};
      auto progress = key({{0, i * .08}, {8, 1 + i * .08}}, "linear");
      progress["loop"] = 8;
      o["follow"] = QJsonObject{{"path", "track"},
                                {"progress", progress},
                                {"orient", true},
                                {"loop", true}};
      o["trail"] =
          QJsonObject{{"samples", 6}, {"duration", .18}, {"opacity", .3}};
      layers.append(o);
    }
    auto caption =
        title("Editable cars • curved racing lines • automatic steering", .88);
    caption["id"] = "caption";
    caption["font_size"] = .025;
    layers.append(caption);
  } else if (name == "motion_titles") {
    auto panel = item("group", "panel", .08, .25, .84, .48, "#ffffff");
    panel["mask"] =
        QJsonObject{{"type", "rect"}, {"width", key({{0, 0}, {1, 1}})}};
    auto backdrop = item("rect", "panel-fill", 0, 0, 1, 1, "#183750");
    auto t = title("{{headline}}", .15);
    t["id"] = "headline";
    t["x"] = .04;
    t["width"] = .92;
    t["height"] = .7;
    t["font_size"] = .072;
    t["text_animation"] = QJsonObject{{"unit", "word"},
                                      {"style", "rise"},
                                      {"stagger", .12},
                                      {"duration", .6},
                                      {"delay", .6}};
    panel["children"] = QJsonArray{backdrop, t};
    layers.append(panel);
    s["variables"] = QJsonObject{{"headline", "YOUR STORY STARTS HERE"}};
    auto sub = title("Masks, grouped layers and animated typography", .79);
    sub["id"] = "subtitle";
    sub["font_size"] = .028;
    sub["opacity"] = key({{0, 0}, {1.2, 0}, {2, 1}});
    layers.append(sub);
  } else if (name == "motion_chart") {
    s["datasets"] = QJsonObject{
        {"results", QJsonArray{QJsonObject{{"label", "A"}, {"value", 64}},
                               QJsonObject{{"label", "B"}, {"value", 88}},
                               QJsonObject{{"label", "C"}, {"value", 100}},
                               QJsonObject{{"label", "D"}, {"value", 72}}}}};
    layers.append(title("{{headline}}"));
    s["variables"] = QJsonObject{{"headline", "DATA THAT TELLS A STORY"}};
    auto chart = item("chart", "chart", .08, .25, .84, .58, "#53e3d0");
    chart["dataset"] = "results";
    chart["style"] = "bar";
    chart["progress"] = key({{0, 0}, {2, 1}});
    layers.append(chart);
  } else if (name == "motion_route") {
    layers.append(title("ANIMATED ROUTE / DRAW, REVEAL, FOLLOW"));
    auto route = item("path", "route", .12, .25, .76, .57, "#00000000");
    route["commands"] = QJsonArray{QJsonArray{"M", 0, .8},
                                   QJsonArray{"C", .25, .9, .20, 0, .5, .25},
                                   QJsonArray{"C", .65, .6, .70, .03, 1, .18}};
    route["stroke"] = "#53e3d0";
    route["stroke_width"] = .014;
    route["trim_end"] = key({{0, 0}, {6, 1}}, "linear");
    route["glow"] = QJsonObject{{"blur", .015}, {"opacity", .3}};
    layers.append(route);
    auto marker = item("ellipse", "marker", 0, 0, .025, .045, "#ffb35c");
    marker["follow"] =
        QJsonObject{{"path", "route"},
                    {"progress", key({{0, 0}, {6, 1}}, "linear")},
                    {"orient", false}};
    layers.append(marker);
  } else if (name == "motion_particles") {
    auto particles = item("particles", "confetti", 0, 0, 1, 1, "#53e3d0");
    particles["count"] = 180;
    particles["seed"] = 47;
    particles["lifetime"] = 5;
    particles["speed"] = .25;
    particles["gravity"] = .06;
    particles["spread"] = 80;
    particles["shape"] = "rect";
    particles["colors"] =
        QJsonArray{"#53e3d0", "#ffb35c", "#679fff", "#ffffff"};
    layers.append(particles);
    auto t = title("DETERMINISTIC PARTICLES", .43);
    t["align"] = "center";
    layers.append(t);
  } else if (name == "motion_morph") {
    layers.append(title("ONE PATH / DIFFERENT SHAPES"));
    QJsonArray a{QJsonArray{"M", .5, 0},
                 QJsonArray{"C", .78, 0, 1, .22, 1, .5},
                 QJsonArray{"C", 1, .78, .78, 1, .5, 1},
                 QJsonArray{"C", .22, 1, 0, .78, 0, .5},
                 QJsonArray{"C", 0, .22, .22, 0, .5, 0},
                 QJsonArray{"Z"}};
    QJsonArray b{QJsonArray{"M", .1, 0},
                 QJsonArray{"C", .8, 0, 1, 0, 1, .1},
                 QJsonArray{"C", 1, .8, 1, 1, .9, 1},
                 QJsonArray{"C", .2, 1, 0, 1, 0, .9},
                 QJsonArray{"C", 0, .2, 0, 0, .1, 0},
                 QJsonArray{"Z"}};
    auto morph = item("path", "morph", .35, .25, .30, .53, "#53e3d0");
    auto k = key({{0, a}, {3, b}});
    k["loop"] = 3;
    k["ping_pong"] = true;
    morph["commands"] = k;
    morph["fill"] = key({{0, "#53e3d0"}, {3, "#ffb35c"}});
    morph["shadow"] = QJsonObject{{"blur", .018}, {"y", .015}, {"opacity", .5}};
    layers.append(morph);
  } else if (name == "motion_audio") {
    layers.append(title("AUDIO REACTIVE / BIND AN AUDIO SOURCE"));
    auto pulse = item("ellipse", "pulse", .40, .30, .20, .355, "#53e3d0");
    pulse["scale_x"] =
        QJsonObject{{"audio", "source"}, {"base", .8}, {"amount", .6}};
    pulse["scale_y"] = pulse["scale_x"];
    pulse["glow"] = QJsonObject{{"blur", .02}, {"opacity", .6}};
    layers.append(pulse);
    auto repeats = item("repeater", "bars", .08, .80, .025, .08, "#53e3d0");
    repeats["count"] = 24;
    repeats["columns"] = 24;
    repeats["spacing_x"] = .035;
    repeats["stagger"] = .015;
    auto bar = item("rect", "bar", 0, 0, 1, 1, "#ffb35c");
    bar["scale_y"] =
        QJsonObject{{"audio", "source"}, {"base", .2}, {"amount", 2}};
    bar["anchor_y"] = 1;
    repeats["children"] = QJsonArray{bar};
    layers.append(repeats);
  } else if (name == "motion_reusable") {
    layers.append(title("REUSABLE COMPOSITIONS / EDIT THE VARIABLES"));
    auto panel = item("rect", "panel", 0, 0, 1, 1, "#19364d");
    auto t = title("{{label}}", .3);
    t["id"] = "label";
    t["x"] = .05;
    t["height"] = .4;
    t["font_size"] = .16;
    t["align"] = "center";
    s["symbols"] = QJsonObject{
        {"card", QJsonObject{{"layers", QJsonArray{panel, t}},
                             {"variables",
                              QJsonObject{{"label", "A reusable card"}}}}}};
    for (int i = 0; i < 3; ++i) {
      auto card = item("instance", QString("card-%1").arg(i), .07 + i * .30,
                       .32, .26, .28, "#ffffff");
      card["symbol"] = "card";
      card["variables"] = QJsonObject{
          {"label", QStringList{"HEADLINE", "LOCATION", "RESULT"}[i]}};
      card["opacity"] = key({{0, 0}, {i * .2 + .6, 1}});
      layers.append(card);
    }
  }
  s["layers"] = layers;
  return s;
}
QJsonObject capabilities() {
  return {
      {"scene_versions", QJsonArray{1, 2}},
      {"layer_types",
       QJsonArray{"rect", "ellipse", "line", "text", "image", "video", "path", "group",
                  "instance", "repeater", "particles", "chart", "noise", "flash"}},
      {"easing",
       QJsonArray{"linear", "hold", "ease_in", "ease_out", "ease_in_out",
                  "smooth", "bounce", "elastic", "[x1,y1,x2,y2]"}},
      {"features",
       QJsonArray{"parenting", "nested groups", "curved paths",
                  "path following + orientation", "path morphing",
                  "stroke reveal + dashes", "camera + follow", "masks",
                  "gradients", "shadows + glow", "trails + motion blur",
                  "text by character/word/line", "symbols + variables",
                  "bar/line/donut charts", "audio envelopes + beats",
                  "seeded particles", "repeaters", "template import/export",
                  "mouth shapes (viseme bindings)", "layer blur + depth of field",
                  "shading + contact shadows", "scene motion blur", "moves",
                  "characters"}},
      {"limits", QJsonObject{{"scene_bytes", 4194304},
                             {"layers", kMaxLayers},
                             {"nesting", 16},
                             {"keys_per_property", 256},
                             {"expanded_layers", 4096},
                             {"particles_per_layer", 2048},
                             {"repeater_copies", 128}}}};
}
QJsonObject withKeyframe(const QJsonObject &scene, const QString &layerId,
                         const QString &property, double time,
                         const QJsonValue &value, const QJsonValue &easing,
                         QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  if (!std::isfinite(time) || time < 0 || time > 3600 || property.isEmpty() ||
      property.split('.').size() > 8 || value.isUndefined() || value.isNull())
    return fail("Choose a property, value and event-local time from 0 to 3600 "
                "seconds.");
  if (QStringList{"id", "parent", "type", "children", "symbol"}.contains(
          property.split('.').first()))
    return fail("Structural layer properties cannot be keyframed.");
  auto result = scene;
  result["version"] = 2;
  bool found = false;
  QStringList parts = property.split('.');
  std::function<QJsonObject(QJsonObject, int)> set = [&](QJsonObject o,
                                                         int depth) {
    QString key = parts[depth];
    if (depth + 1 < parts.size()) {
      o[key] = set(o[key].toObject(), depth + 1);
      return o;
    }
    auto old = o[key];
    auto spec = old.isObject() && old.toObject().contains("keyframes")
                    ? old.toObject()
                    : QJsonObject{};
    auto ks = spec["keyframes"].toArray();
    if (ks.isEmpty() && time > 0) {
      QJsonValue initial = old.isUndefined() ? value : evaluate(old, 0, scene);
      ks.append(QJsonArray{0, initial, "linear"});
    }
    QJsonArray out;
    bool added = false;
    for (auto k : ks) {
      auto a = k.toArray();
      double at = a[0].toDouble();
      if (std::abs(at - time) < .000001)
        continue;
      if (!added && at > time) {
        out.append(QJsonArray{time, value, easing});
        added = true;
      }
      out.append(a);
    }
    if (!added)
      out.append(QJsonArray{time, value, easing});
    spec["keyframes"] = out;
    o[key] = spec;
    return o;
  };
  if (layerId == "$camera") {
    result["camera"] = set(result["camera"].toObject(), 0);
    found = true;
  } else if (layerId == "$variables") {
    result["variables"] = set(result["variables"].toObject(), 0);
    found = true;
  } else {
    std::function<QJsonArray(QJsonArray)> visit = [&](QJsonArray a) {
      for (int i = 0; i < a.size(); ++i) {
        auto o = a[i].toObject();
        if (o["id"] == layerId) {
          o = set(o, 0);
          found = true;
        }
        if (o.contains("children"))
          o["children"] = visit(o["children"].toArray());
        a[i] = o;
      }
      return a;
    };
    result["layers"] = visit(result["layers"].toArray());
  }
  if (!found)
    return fail("Choose an existing layer ID, $camera or $variables.");
  if (auto e = validate(result); !e.isEmpty())
    return fail(e);
  return result;
}
QJsonObject withPatch(const QJsonObject &scene, const QJsonArray &operations, QString *error) {
  auto fail = [&](int i, QString e) {
    if (error)
      *error = QStringLiteral("Change %1: %2").arg(i + 1).arg(e);
    return QJsonObject{};
  };
  if (operations.isEmpty() || operations.size() > 4096) {
    if (error)
      *error = "Give 1 to 4096 changes.";
    return {};
  }
  QJsonObject result = scene;
  result["version"] = 2;
  for (int i = 0; i < operations.size(); ++i) {
    const QJsonObject op = operations[i].toObject();
    const QString layerId = op.value("layer_id").toString(), property = op.value("property").toString();
    const bool remove = op.value("remove").toBool();
    const QStringList parts = property.split('.');
    if (property.isEmpty() || parts.size() > 8 || parts.contains(QString()))
      return fail(i, "Name a property (a dotted path for one inside another, such as shadow.blur).");
    if (!remove && (op.value("value").isUndefined() || op.value("value").isNull()))
      return fail(i, "Give a value, or remove: true.");
    if (layerId != "$scene" && layerId != "$variables" && layerId != "$camera" &&
        QStringList{"id", "parent", "type", "children", "symbol"}.contains(parts.first()))
      return fail(i, "Structural layer properties (id, type, parent, children) cannot be patched.");
    if (layerId == "$scene" && QStringList{"layers", "version", "symbols"}.contains(parts.first()))
      return fail(i, "Change the scene's layers through their IDs.");
    std::function<QJsonObject(QJsonObject, int)> set = [&](QJsonObject o, int depth) {
      const QString key = parts[depth];
      if (depth + 1 < parts.size())
        o[key] = set(o.value(key).toObject(), depth + 1);
      else if (remove)
        o.remove(key);
      else
        o[key] = op.value("value");
      return o;
    };
    bool found = true;
    if (layerId == "$scene")
      result = set(result, 0);
    else if (layerId == "$camera" || layerId == "$variables")
      result[layerId.mid(1)] = set(result.value(layerId.mid(1)).toObject(), 0);
    else {
      found = false;
      std::function<QJsonArray(QJsonArray)> visit = [&](QJsonArray a) {
        for (int k = 0; k < a.size(); ++k) {
          QJsonObject o = a[k].toObject();
          if (o.value("id") == layerId) {
            o = set(o, 0);
            found = true;
          }
          if (o.contains("children"))
            o["children"] = visit(o.value("children").toArray());
          a[k] = o;
        }
        return a;
      };
      result["layers"] = visit(result.value("layers").toArray());
    }
    if (!found)
      return fail(i, QStringLiteral("No layer %1 (or $camera, $variables, $scene).").arg(layerId));
  }
  if (auto e = validate(result); !e.isEmpty()) {
    if (error)
      *error = e;
    return {};
  }
  return result;
}
namespace {
// Every value in `v` with variable and audio references renamed.
QJsonValue renamed(const QJsonValue &v, const std::function<QString(const QString &)> &variable,
                   const std::function<QString(const QString &)> &audio) {
  if (v.isArray()) {
    QJsonArray a = v.toArray();
    for (int i = 0; i < a.size(); ++i)
      a[i] = renamed(a[i], variable, audio);
    return a;
  }
  if (!v.isObject())
    return v;
  QJsonObject o = v.toObject();
  for (auto it = o.begin(); it != o.end(); ++it)
    it.value() = renamed(it.value(), variable, audio);
  // (value(): a missing key read with [] would be added, as null)
  if (o.value("variable").isString())
    o["variable"] = variable(o.value("variable").toString());
  if (o.value("audio").isString())
    o["audio"] = audio(o.value("audio").toString());
  return o;
}
QString characterKey(const QString &name) {
  QString key;
  for (QChar c : name.trimmed().toLower())
    if (c.isLetterOrNumber())
      key += c;
    else if (!key.isEmpty() && !key.endsWith('_'))
      key += '_';
  while (key.endsWith('_'))
    key.chop(1);
  return key.isEmpty() ? QStringLiteral("character") : key;
}
} // namespace
QJsonObject withCharacter(const QJsonObject &scene, const QJsonObject &character, const QString &name, double x, double ground,
                          QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  const QJsonArray parts = character["layers"].toArray();
  if (parts.isEmpty() || parts[0].toObject()["type"] != "group")
    return fail("A character is a group of layers.");
  QSet<QString> taken;
  std::function<void(const QJsonArray &)> collect = [&](const QJsonArray &a) {
    for (auto v : a) {
      taken.insert(v.toObject()["id"].toString());
      collect(v.toObject()["children"].toArray());
    }
  };
  collect(scene["layers"].toArray());
  QString key = characterKey(name);
  for (int n = 2; taken.contains(key) || std::any_of(taken.begin(), taken.end(), [&](const QString &id) { return id.startsWith(key + "_"); }); ++n)
    key = characterKey(name) + "_" + QString::number(n);
  const QJsonObject colours = character["variables"].toObject();
  const QString voice = character["binding"].toString("voice");
  auto id = [&](const QString &i) { return i == "root" ? key : key + "_" + i; };
  QJsonObject result = scene;
  result["version"] = 2;
  QJsonArray layers = result["layers"].toArray();
  for (int i = 0; i < parts.size(); ++i) {
    QJsonObject o = renamed(parts[i], [&](const QString &v) { return colours.contains(v) ? key + "_" + v : v; },
                            [&](const QString &a) { return a == voice ? key : a; })
                        .toObject();
    o["id"] = id(o.value("id").toString());
    if (o.contains("parent"))
      o["parent"] = id(o.value("parent").toString());
    if (i == 0) {
      o["name"] = name.trimmed().isEmpty() ? character["name"].toString(key) : name.trimmed();
      o["x"] = std::isfinite(x) ? x : .5 - o.value("width").toDouble(.14) / 2;
      o["y"] = (std::isfinite(ground) ? ground : .9) - o.value("height").toDouble(.38);
    }
    layers.append(o);
  }
  result["layers"] = layers;
  QJsonObject variables = result["variables"].toObject();
  for (auto it = colours.begin(); it != colours.end(); ++it)
    variables[key + "_" + it.key()] = it.value();
  result["variables"] = variables;
  if (auto e = validate(result); !e.isEmpty())
    return fail(e);
  return result;
}
QJsonObject withSet(const QJsonObject &scene, const QJsonObject &set, QString *error) {
  const QJsonArray back = set["layers"].toArray(), front = set["front"].toArray();
  if (back.isEmpty()) {
    if (error)
      *error = "A set is a list of layers.";
    return {};
  }
  // The set added before goes (a scene has one set).
  QJsonArray kept;
  for (auto v : scene["layers"].toArray()) {
    const QString id = v.toObject().value("id").toString();
    if (id != "set" && id != "set_front" && !id.startsWith("set_") && !id.startsWith("setf_"))
      kept.append(v);
  }
  // Its signs (the words on it) become scene variables, set_<sign>, to reword in the Keys tab or with a patch.
  QJsonObject signs;
  auto part = [&signs](const QJsonArray &layers, const QString &group, const QString &prefix, const QString &name, double depth) {
    QJsonObject g{{"type", "group"}, {"id", group}, {"name", name}, {"x", 0}, {"y", 0}, {"width", 1}, {"height", 1}};
    if (depth > 0)
      g["depth"] = depth;
    QJsonArray out{g};
    for (auto v : layers) {
      QJsonObject o = v.toObject();
      if (o.value("type") == "text" && o.value("text").isString() && !o.value("text").toString().trimmed().isEmpty()) {
        QString key = o.value("id").toString();
        if (key.endsWith("_t"))
          key.chop(2);
        key = "set_" + key;
        signs[key] = o.value("text");
        o["text"] = QJsonObject{{"variable", key}};
      }
      o["id"] = prefix + o.value("id").toString();
      o["parent"] = o.contains("parent") ? prefix + o.value("parent").toString() : group;
      if (o.value("mask").isString())
        o["mask"] = prefix + o.value("mask").toString();
      out.append(o);
    }
    return out;
  };
  QJsonArray layers = part(back, "set", "set_", "Set: " + set["name"].toString(), 1);
  for (auto v : kept)
    layers.append(v);
  if (!front.isEmpty())
    for (auto v : part(front, "set_front", "setf_", "Set front: " + set["name"].toString(), 0))
      layers.append(v);
  QJsonObject result = scene;
  result["version"] = 2;
  result["layers"] = layers;
  QJsonObject variables = scene.value("variables").toObject();
  for (const QString &k : variables.keys()) // (the signs of the set before)
    if (k.startsWith("set_"))
      variables.remove(k);
  for (auto it = signs.begin(); it != signs.end(); ++it)
    variables[it.key()] = it.value();
  if (!variables.isEmpty())
    result["variables"] = variables;
  if (auto e = validate(result); !e.isEmpty()) {
    if (error)
      *error = e;
    return {};
  }
  return result;
}
QJsonObject withProp(const QJsonObject &scene, const QJsonObject &prop, const QString &characterId, const QString &hand, QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  const QJsonArray parts = prop["layers"].toArray();
  if (parts.isEmpty() || parts[0].toObject()["type"] != "group")
    return fail("A prop is a group of layers.");
  const QString arm = characterId + (hand.compare("L", Qt::CaseInsensitive) == 0 ? "_armL" : "_armR"), holder = arm + "_t";
  QJsonArray layers = scene["layers"].toArray();
  int last = -1;
  QSet<QString> taken;
  for (int i = 0; i < layers.size(); ++i) {
    const QString id = layers[i].toObject().value("id").toString();
    taken.insert(id);
    if (id == holder || id.startsWith(holder + "_"))
      last = i;
  }
  if (last < 0)
    return fail(QStringLiteral("%1 has no hand to hold it (choose a library character's own group).").arg(characterId));
  // The arm's size on the picture (its box, the camera aside), so the prop keeps its own size in any character's hand.
  QJsonObject still = scene;
  still.remove("camera");
  const QRectF box = bounds(still, arm, 0, QSizeF(1, 1));
  if (box.width() <= 0 || box.height() <= 0)
    return fail("The arm has no size.");
  const double w = prop["width"].toDouble(.05) / box.width(), h = prop["height"].toDouble(.05) / box.height();
  const QJsonArray grip = prop["grip"].toArray();
  const double gx = grip.size() == 2 ? grip[0].toDouble(.5) : .5, gy = grip.size() == 2 ? grip[1].toDouble(.5) : .5;
  QString key = characterId + "_" + characterKey(prop["name"].toString("prop"));
  for (int n = 2; taken.contains(key); ++n)
    key = characterId + "_" + characterKey(prop["name"].toString("prop")) + "_" + QString::number(n);
  auto id = [&](const QString &i) { return i == "root" ? key : key + "_" + i; };
  QJsonArray made;
  for (int i = 0; i < parts.size(); ++i) {
    QJsonObject o = parts[i].toObject();
    if (i == 0) { // held at the hand (the middle of the hand, near the bottom of the arm)
      o["x"] = .5 - gx * w;
      o["y"] = .83 - gy * h;
      o["width"] = w;
      o["height"] = h;
      o["parent"] = holder;
      o["name"] = prop["name"].toString("Prop") + " (" + characterId + ")";
    } else if (o.contains("parent"))
      o["parent"] = id(o["parent"].toString());
    o["id"] = id(o.value("id").toString());
    made.append(o);
  }
  for (int i = 0; i < made.size(); ++i)
    layers.insert(last + 1 + i, made[i]);
  QJsonObject result = scene;
  result["version"] = 2;
  result["layers"] = layers;
  if (auto e = validate(result); !e.isEmpty())
    return fail(e);
  return result;
}
QJsonObject characterFrom(const QJsonObject &scene, const QString &groupId, const QString &title, QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  const QJsonArray all = scene["layers"].toArray();
  QJsonArray mine;
  QSet<QString> inside{groupId};
  for (auto v : all) // (flat layers name their parent: a parent comes before its children)
    if (const auto o = v.toObject(); o.value("id") == groupId || inside.contains(o["parent"].toString())) {
      inside.insert(o["id"].toString());
      mine.append(o);
    }
  if (mine.isEmpty() || mine[0].toObject()["type"] != "group")
    return fail("Choose a group: a character is a group and the layers in it.");
  auto local = [&](const QString &i) {
    return i == groupId ? QStringLiteral("root") : i.startsWith(groupId + "_") ? i.mid(groupId.size() + 1) : i;
  };
  // Its colours become settings, named after the first layer that uses each; its most used binding is its voice.
  QJsonObject colours;
  QMap<QString, QString> byColour;
  QMap<QString, int> bindings;
  const QJsonObject sceneVariables = scene["variables"].toObject();
  QJsonArray out;
  for (int i = 0; i < mine.size(); ++i) {
    QJsonObject o = renamed(mine[i], [&](const QString &v) {
      const QString name = local(v);
      colours[name] = sceneVariables.value(v);
      return name;
    }, [&](const QString &a) { ++bindings[a]; return a; }).toObject();
    o["id"] = local(o.value("id").toString());
    if (o.contains("parent"))
      o["parent"] = local(o.value("parent").toString());
    if (const QString fill = o.value("fill").toString(); QColor::isValidColorName(fill) && fill.startsWith('#') && QColor(fill).alpha() > 0) {
      if (!byColour.contains(fill)) {
        QString name = characterKey(o["id"].toString());
        while (colours.contains(name))
          name += "_2";
        byColour[fill] = name;
        colours[name] = fill;
      }
      o["fill"] = QJsonObject{{"variable", byColour[fill]}};
    }
    if (i == 0) {
      o["x"] = 0;
      o["y"] = 0;
      o.remove("parent");
    }
    out.append(o);
  }
  QString voice = "voice";
  int most = 0;
  for (auto it = bindings.begin(); it != bindings.end(); ++it)
    if (it.value() > most) {
      most = it.value();
      voice = it.key();
    }
  const QJsonObject root = out[0].toObject();
  return {{"version", 1}, {"name", title.trimmed().isEmpty() ? root["name"].toString(groupId) : title.trimmed()}, {"binding", voice},
          {"width", root["width"]}, {"height", root["height"]}, {"variables", colours}, {"layers", out}};
}
bool setDraft(bool draft) {
  const bool was = draftRender;
  draftRender = draft;
  return was;
}
void setGpuPictures(GpuPictures *pictures) { gpuPictures = pictures; }
int cpuPictures() { return cpuPictureCount; }
FramebufferPaintDevice::FramebufferPaintDevice(QSize size, unsigned fbo) : QOpenGLPaintDevice(size), m_fbo(fbo) { setPaintFlipped(true); }
void FramebufferPaintDevice::ensureActiveTarget() { QOpenGLContext::currentContext()->functions()->glBindFramebuffer(GL_FRAMEBUFFER, m_fbo); }
QStringList moves() {
  return {"hop", "jump", "shake", "nod", "wave", "spin", "walk", "walk in", "walk out", "pop", "squash", "blink", "float", "fall",
          "get up", "sit", "stand", "point", "arms up", "shrug"};
}
QStringList expressions() { return {"neutral", "angry", "sad", "surprised", "smug"}; }
QJsonObject withExpression(const QJsonObject &scene, const QString &characterId, const QString &mood, double time, QString *error) {
  const QString key = characterId.toLower() + "_mood";
  if (!expressions().contains(mood) || !scene["variables"].toObject().contains(key)) {
    if (error)
      *error = expressions().contains(mood) ? QStringLiteral("%1 has no expressions (a library character's group has them).").arg(characterId)
                                            : "Expressions: " + expressions().join(", ") + ".";
    return {};
  }
  return withKeyframe(scene, "$variables", key, time, mood, "hold", error);
}
QJsonObject withMove(const QJsonObject &scene, const QString &layerId,
                     const QString &move, double time, double duration,
                     double amount, double to, QString *error) {
  auto fail = [&](QString e) {
    if (error)
      *error = e;
    return QJsonObject{};
  };
  if (!moves().contains(move))
    return fail("Moves: " + moves().join(", ") + ".");
  if (move == "point" || move == "arms up" || move == "shrug") {
    // Gestures: on a character's own group, they turn its arms' pose groups and bring them back.
    const double d = std::clamp(duration > 0 ? duration : (move == "shrug" ? .8 : 1.6), .1, 60.);
    QJsonObject result = scene;
    QString e;
    for (const auto &[side, angle] : {std::pair{"R", move == "point" ? -80. : move == "arms up" ? -150. : -35.},
                                      std::pair{"L", move == "point" ? 0. : move == "arms up" ? 150. : 35.}}) {
      if (angle == 0)
        continue;
      const QString arm = layerId + "_arm" + side + "_pose";
      for (const auto &[at, v, easing] : {std::tuple{0., 0., "ease_out"}, std::tuple{d * .2, angle * amount, "hold"},
                                          std::tuple{d * .8, angle * amount, "ease_in_out"}, std::tuple{d, 0., "linear"}}) {
        result = withKeyframe(result, arm, "rotation", std::max(0., time + at), v, easing, &e);
        if (result.isEmpty())
          return fail(QStringLiteral("%1 has no arms to gesture with (choose a character's own group).").arg(layerId));
      }
    }
    return result;
  }
  QJsonObject layer;
  std::function<void(const QJsonArray &)> find = [&](const QJsonArray &a) {
    for (auto v : a) {
      const auto o = v.toObject();
      if (o["id"] == layerId)
        layer = o;
      else if (o.contains("children"))
        find(o["children"].toArray());
    }
  };
  find(scene["layers"].toArray());
  if (layer.isEmpty())
    return fail("Choose an existing layer ID.");
  static const QMap<QString, double> natural{{"hop", .5}, {"jump", .8}, {"shake", .6}, {"nod", .5}, {"wave", 1.2},
                                             {"spin", .8}, {"walk", 2}, {"walk in", 2}, {"walk out", 2}, {"pop", .35}, {"squash", .4},
                                             {"blink", .14}, {"float", 4}, {"fall", .6}, {"get up", .8}, {"sit", .4}, {"stand", .4}};
  const double d = std::clamp(duration > 0 ? duration : natural[move], .05, 60.);
  amount = std::isfinite(amount) ? std::clamp(amount, -10., 10.) : 1;
  // The property's own value where the move starts and where it ends, so a
  // move lands back on whatever animation was there.
  auto value = [&](const char *property, double fallback, double at) {
    return evaluate(layer.value(property), at, scene).toDouble(fallback);
  };
  QJsonObject result = scene;
  QString e;
  auto key = [&](const char *property, double at, double v, const char *easing = "ease_in_out") {
    if (result.isEmpty())
      return;
    result = withKeyframe(result, layerId, property, std::max(0., time + at), v, easing, &e);
  };
  if (move == "hop" || move == "jump") {
    const double y0 = value("y", 0, time), y1 = value("y", 0, time + d), h = (move == "hop" ? .06 : .18) * amount;
    key("y", 0, y0, "ease_out");
    key("y", d / 2, std::min(y0, y1) - h, "ease_in");
    key("y", d, y1);
  } else if (move == "shake") {
    const double x0 = value("x", 0, time), x1 = value("x", 0, time + d);
    key("x", 0, x0, "linear");
    for (int i = 1; i < 8; ++i)
      key("x", d * i / 8, x0 + (i % 2 ? 1 : -1) * .012 * amount * (1 - i / 8.), "linear");
    key("x", d, x1);
  } else if (move == "nod" || move == "wave" || move == "spin") {
    const double r0 = value("rotation", 0, time), r1 = value("rotation", 0, time + d);
    key("rotation", 0, r0);
    if (move == "nod") {
      key("rotation", d * .35, r0 + 9 * amount);
      key("rotation", d * .7, r0 - 4 * amount);
    } else if (move == "wave") {
      for (int i = 1; i < 5; ++i)
        key("rotation", d * i / 5, r0 + (i % 2 ? -25 : 25) * amount);
    } else {
      key("rotation", d, r1 + 360 * (amount < 0 ? -1 : 1));
      return result.isEmpty() ? fail(e) : result;
    }
    key("rotation", d, r1);
  } else if (move == "walk") {
    const double x0 = value("x", 0, time), y0 = value("y", 0, time);
    const double target = std::isfinite(to) ? to : x0 + .2 * amount;
    key("x", 0, x0, "linear");
    key("x", d, target, "linear");
    for (double s = 0; s + .15 <= d; s += .3) { // a step every 0.3 s, a little bob on each
      key("y", s, y0);
      key("y", s + .15, y0 - .015 * std::abs(amount));
    }
    key("y", d, y0);
  } else if (move == "walk in" || move == "walk out") {
    // From off the frame to its place (or to x), or from its place off the frame: the left side, or the right with a
    // negative amount. Before it walks in it is off the frame; after it walks out it stays off.
    const double x0 = value("x", 0, time), y0 = value("y", 0, time), w = layer.value("width").toDouble(.15);
    const double off = amount < 0 ? 1.05 : -w - .05;
    const double from = move == "walk in" ? off : x0, target = move == "walk in" ? (std::isfinite(to) ? to : x0) : (std::isfinite(to) ? to : off);
    if (move == "walk in" && time > 0)
      result = withKeyframe(result, layerId, "x", 0, from, "hold", &e);
    key("x", 0, from, "linear");
    key("x", d, target, "linear");
    for (double s = 0; s + .15 <= d; s += .3) {
      key("y", s, y0);
      key("y", s + .15, y0 - .015);
    }
    key("y", d, y0);
  } else if (move == "float") {
    // Weightless: drifting up and down and tilting, two seconds a sway, back where it was at the end.
    const double y0 = value("y", 0, time), r0 = value("rotation", 0, time), y1 = value("y", 0, time + d), r1 = value("rotation", 0, time + d);
    key("y", 0, y0);
    key("rotation", 0, r0);
    int i = 0;
    for (double s = 1; s < d - .5; s += 1, ++i) {
      key("y", s, y0 - (i % 2 ? .02 : .06) * amount);
      key("rotation", s, r0 + (i % 2 ? -8 : 8) * amount);
    }
    key("y", d, y1);
    key("rotation", d, r1);
  } else if (move == "fall" || move == "get up") {
    // Over sideways (to the right; negative amount: the left) and stays down; get up stands it again.
    const double r0 = value("rotation", 0, time), y0 = value("y", 0, time), h = layer.value("height").toDouble(.4);
    const double down = 82 * (amount < 0 ? -1 : 1), drop = h * .35;
    if (move == "fall") {
      key("rotation", 0, r0, "ease_in");
      key("rotation", d * .7, r0 + down * 1.08, "ease_out");
      key("rotation", d, r0 + down, "hold");
      key("y", 0, y0, "ease_in");
      key("y", d * .7, y0 + drop, "ease_out");
      key("y", d, y0 + drop * .96, "hold");
    } else {
      const double upright = std::abs(r0) > 45 ? 0 : r0;
      key("rotation", 0, r0, "ease_in_out");
      key("rotation", d, upright, "hold");
      key("y", 0, y0, "ease_in_out");
      key("y", d, std::abs(r0) > 45 ? y0 - drop * .96 : y0, "hold");
    }
  } else if (move == "sit" || move == "stand") {
    // Down onto something (shorter, lower) and stays; stand gets up again.
    const double s0 = value("scale_y", 1, time), y0 = value("y", 0, time), h = layer.value("height").toDouble(.4);
    const double low = .8, sink = h * (1 - low) * .5 * amount;
    if (move == "sit") {
      key("scale_y", 0, s0, "ease_out");
      key("scale_y", d, s0 * low, "hold");
      key("y", 0, y0, "ease_out");
      key("y", d, y0 + sink, "hold");
    } else {
      key("scale_y", 0, s0, "ease_out");
      key("scale_y", d, s0 < 1 ? 1 : s0, "hold");
      key("y", 0, y0, "ease_out");
      key("y", d, s0 < 1 ? y0 - sink : y0, "hold");
    }
  } else if (move == "pop") {
    key("scale_x", 0, 0, "ease_out");
    key("scale_y", 0, 0, "ease_out");
    key("scale_x", d * .7, 1.15 * amount);
    key("scale_y", d * .7, 1.15 * amount);
    key("scale_x", d, amount);
    key("scale_y", d, amount);
  } else if (move == "squash") {
    const double s0 = value("scale_y", 1, time), s1 = value("scale_y", 1, time + d);
    key("scale_y", 0, s0, "ease_out");
    key("scale_y", d * .3, s0 * (1 - .3 * amount));
    key("scale_y", d * .65, s0 * (1 + .1 * amount));
    key("scale_y", d, s1);
  } else if (move == "blink") {
    const double s0 = value("scale_y", 1, time), s1 = value("scale_y", 1, time + d);
    key("scale_y", 0, s0, "linear");
    key("scale_y", d * .4, .08, "linear");
    key("scale_y", d, s1, "linear");
  }
  return result.isEmpty() ? fail(e) : result;
}
} // namespace motion
