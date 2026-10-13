#include "core/MotionGraphics.h"
#include "core/Graphics.h"
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPathStroker>
#include <QRadialGradient>
#include <QRegularExpression>
#include <QSet>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

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
  for (auto v : a) {
    auto c = v.toArray();
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
    if (o.contains("audio"))
      return o["base"].toDouble() +
             o["amount"].toDouble(1) * audioLevel(o, scene, time);
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
    if (o.contains("keyframes") || o.contains("audio") ||
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
      if (!v.isObject() || ++count > 512)
        return "Scene limit: 512 layers (including reusable definitions).";
      auto o = v.toObject();
      auto type = o["type"].toString();
      if (!QStringList{"rect", "ellipse", "line", "text", "image", "path",
                       "group", "instance", "repeater", "particles", "chart"}
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
class Graph {
public:
  QJsonObject scene;
  QSizeF size;
  QVector<Node> nodes;
  QMap<QString, int> ids;
  QVector<int> roots;
  int &budget;
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
    QTransform m;
    m.translate(size.width() / 2, size.height() / 2);
    m.scale(std::clamp(number(c, "zoom", 1, t, scene), .01, 100.),
            std::clamp(number(c, "zoom", 1, t, scene), .01, 100.));
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
    if (anim.isEmpty()) {
      QPainterPath all;
      for (auto line : lines)
        all.addText(QPointF(b.x() + line.cursorToX(line.textStart()),
                            top + line.y() + line.ascent()),
                    font, words.mid(line.textStart(), line.textLength()));
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
            glyph.addText(QPointF(b.x() + part.cursorToX(first),
                                  top + part.y() + part.ascent()),
                          font, words.mid(first, last - first));
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
            std::clamp(int(number(blur, "samples", 5, t, scene)), 1, 8);
        double shutter =
            std::clamp(number(blur, "shutter", .03, t, scene), 0., .2);
        QImage accumulated(size.toSize(), QImage::Format_ARGB32_Premultiplied);
        accumulated.fill(Qt::transparent);
        {
          QPainter samplesPainter(&accumulated);
          samplesPainter.setRenderHints(p.renderHints());
          samplesPainter.setCompositionMode(QPainter::CompositionMode_Plus);
          for (int j = 0; j < samples; ++j)
            draw(i, samplesPainter,
                 std::max(0.,
                          t + shutter *
                                  (double(j) / std::max(1, samples - 1) - .5)),
                 camera, 1. / samples, depth + 1, true);
        }
        p.save();
        p.setWorldTransform({});
        p.setOpacity(opacity);
        applyBlend(p, o["blend"].toString());
        p.drawImage(0, 0, accumulated);
        p.restore();
        return;
      }
    }
    QString type = o["type"].toString();
    double alpha = std::clamp(number(o, "opacity", 1, t, scene), 0., 1.);
    if (isolate && (alpha < 1 || !o["blend"].toString().isEmpty()) &&
        (type == "group" || type == "instance" || type == "repeater")) {
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
    else if (type == "chart")
      drawChart(p, o, at.box, t);
    else if (type == "text")
      drawText(p, o, at.box, t);
    else if (type == "image") {
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
      QPainterPath silhouette;
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
      decoration(p, silhouette, o["shadow"].toObject(), size, t, scene, false);
      decoration(p, silhouette, o["glow"].toObject(), size, t, scene, true);
      p.setBrush(brush(o, at.box, t, scene));
      p.setPen(stroke > 0 || type == "line" ? pen : QPen(Qt::NoPen));
      if (trimmed)
        p.setBrush(Qt::NoBrush);
      p.drawPath(painted);
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
void paint(QPainter &p, QSizeF size, const QJsonObject &scene, double time) {
  if (!std::isfinite(time) || !validate(scene).isEmpty())
    return;
  p.setRenderHint(QPainter::Antialiasing);
  p.setRenderHint(QPainter::SmoothPixmapTransform);
  int budget = 32768;
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
          "motion_audio", "motion_reusable"};
}
QJsonObject preset(const QString &name) {
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
       QJsonArray{"rect", "ellipse", "line", "text", "image", "path", "group",
                  "instance", "repeater", "particles", "chart"}},
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
                  "seeded particles", "repeaters", "template import/export"}},
      {"limits", QJsonObject{{"scene_bytes", 4194304},
                             {"layers", 512},
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
} // namespace motion
