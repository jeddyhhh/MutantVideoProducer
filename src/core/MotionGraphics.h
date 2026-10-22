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
// pop (scales in), squash, blink (for an eyes group); point, arms up, shrug (on a character's own group: its arms). duration 0: the move's natural length; amount scales it.
// A character (a library file, or one saved with characterFrom) added to a scene: its layers' ids start with its name,
// its colour settings become scene variables name_<colour>, its voice binding is called by its name (lower case, the
// name to bind its voice track under), its feet stand on `ground` with its left edge at x. Two of the same name get
// numbered.
QJsonObject withCharacter(const QJsonObject &scene, const QJsonObject &character, const QString &name, double x, double ground,
                          QString *error);
// A set (library background) behind everything in the scene, its front part in front of everything, replacing the set
// added before. The back is one group, "set", with depth 1: the camera's depth of field softens it in close-ups.
QJsonObject withSet(const QJsonObject &scene, const QJsonObject &set, QString *error);
// A scene's group (and the layers in it) as a character: its fill colours become settings, its most used audio binding
// its voice.
QJsonObject characterFrom(const QJsonObject &scene, const QString &groupId, const QString &title, QString *error);
// Preview quality for this thread's renders (fewer motion blur moments, faster video frames). Returns the previous one.
bool setDraft(bool draft);
QStringList moves();
// A character's expression from `time` on (their <id>_mood variable, held): neutral, angry, sad, surprised, smug.
QStringList expressions();
QJsonObject withExpression(const QJsonObject &scene, const QString &characterId, const QString &mood, double time, QString *error);
// Small changes in one step: each {layer_id, property, value} sets a layer's property (a dotted path such as
// "shadow.blur"), or with remove: true takes it away; layer_id may also be $camera, $variables or $scene (the scene's own
// keys). Structural properties (id, type, parent, children) stay as they are. Empty on error.
QJsonObject withPatch(const QJsonObject &scene, const QJsonArray &operations, QString *error);
// The most layers a version 2 scene stores, reusable definitions included.
constexpr int kMaxLayers = 1024;
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
