#pragma once
#include <QJsonObject>
#include <QPointF>
#include <QRectF>
#include <QStringList>
#include <QVector>

namespace graphics_workflow {
QJsonObject prepare(const QJsonObject& scene);
bool locked(const QJsonObject& scene, const QString& id);
QRectF bounds(const QJsonObject& scene, const QJsonObject& layer, double time);
// All-or-nothing edits; geometry is evaluated in scene space and mapped back to each parent.
QJsonObject layout(const QJsonObject& scene, const QStringList& ids, const QString& operation,
                   const QString& reference, double time, QString* error);
QJsonObject translate(const QJsonObject& scene, const QStringList& ids, QPointF delta,
                      double time, QString* error);
QPointF snap(const QJsonObject& scene, const QStringList& ids, QPointF delta,
             double time, double tolerance, QVector<double>* xGuides = nullptr,
             QVector<double>* yGuides = nullptr);
QJsonObject setLocked(const QJsonObject& scene, const QStringList& ids, bool value, QString* error);
QString validateBrand(const QJsonObject& brand);
QJsonObject applyBrand(const QJsonObject& scene, const QJsonObject& brand, QString* error);
}
