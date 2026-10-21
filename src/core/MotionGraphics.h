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
