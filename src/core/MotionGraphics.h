#pragma once
#include <QImage>
#include <QHash>
#include <QJsonObject>
#include <QPainterPath>
#include <QVector>
class QPainter;

namespace motion {
// Scene v2: normalized coordinates, event-local seconds, deterministic seeking.
QJsonValue evaluate(const QJsonValue &value, double time,
                    const QJsonObject &scene = {});
double ease(double progress, const QJsonValue &curve);
QString validate(const QJsonObject &scene);
bool animating(const QJsonValue &value);
void render(QImage &image, const QJsonObject &scene, double time);
void paint(QPainter &painter, QSizeF size, const QJsonObject &scene, double time);
QJsonObject preset(const QString &name);
QStringList presets();
// Two cut-out characters whose mouths, arms and bodies follow the bindings voice_a and voice_b.
QJsonObject charactersScene();
QJsonObject capabilities();
QJsonObject withKeyframe(const QJsonObject &scene, const QString &layerId,
                         const QString &property, double time,
                         const QJsonValue &value, const QJsonValue &easing,
                         QString *error);
QPainterPath path(const QJsonArray &commands);
// Ready-made moves: keyframes for a layer around `time` (event-local seconds) that land back on its own animation.
// hop, jump, shake, nod, wave (for an arm's group: it turns at its anchor), spin, walk (to x = `to`, with a bob),
// pop (scales in), squash, blink (for an eyes group). duration 0: the move's natural length; amount scales it.
// A character (a library file, or one saved with characterFrom) added to a scene: its layers' ids start with its name,
// its colour settings become scene variables name_<colour>, its voice binding is called by its name (lower case, the
// name to bind its voice track under), its feet stand on `ground` with its left edge at x. Two of the same name get
// numbered.
QJsonObject withCharacter(const QJsonObject &scene, const QJsonObject &character, const QString &name, double x, double ground,
                          QString *error);
// A scene's group (and the layers in it) as a character: its fill colours become settings, its most used audio binding
// its voice.
QJsonObject characterFrom(const QJsonObject &scene, const QString &groupId, const QString &title, QString *error);
QStringList moves();
QJsonObject withMove(const QJsonObject &scene, const QString &layerId, const QString &move, double time, double duration,
                     double amount, double to, QString *error);
struct PathHandle {
  int command = 0, coordinate = 1;
  QPointF position;
};
QRectF bounds(const QJsonObject &scene, const QString &id, double time,
              QSizeF size);
QHash<QString,QRectF> allBounds(const QJsonObject& scene, double time, QSizeF size);
QVector<PathHandle> handles(const QJsonObject &scene, const QString &id,
                            double time, QSizeF size);
QPointF editDelta(const QJsonObject &scene, const QString &id, double time,
                  QSizeF size, QPointF delta, bool pathPoint = false);
} // namespace motion
