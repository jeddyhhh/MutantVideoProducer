#pragma once
#include "core/Generators.h"
#include <QJsonArray>

namespace graphics {
QStringList presets();
QJsonObject scene(const QString &preset);
QString validateScene(const QJsonObject &scene);
QString validateWaveform(const QJsonObject &data);
void drawScene(QImage &image, const QJsonObject &scene, double seconds);
void paintScene(QPainter &painter, QSize size, const QJsonObject &scene, double seconds);
QJsonObject buildWaveform(const QString &path, int stream, double start,
                          double duration, int bins, QString *error,
                          const std::function<bool(double)> &progress = {},
                          bool analyzeBeats = false);
void addGenerators(std::vector<GeneratorDef> &list);
void addPresets(std::vector<GeneratorPreset> &list);
} // namespace graphics
