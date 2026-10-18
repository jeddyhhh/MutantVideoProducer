#pragma once
#include "core/Generators.h"
#include <QJsonArray>
#include <map>
#include <memory>

struct Track;
struct MediaItem;

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
// The envelope of every straight file-backed audio event on a track over [from, from + length) seconds of the timeline
// (a character's voice track, for lip sync across all their lines), as a binding for a scene that starts at `from`.
QJsonObject trackEnvelope(const Track &track, const std::map<int, std::shared_ptr<const MediaItem>> &media, double from,
                          double length, int bins, QString *error);
void addGenerators(std::vector<GeneratorDef> &list);
void addPresets(std::vector<GeneratorPreset> &list);
} // namespace graphics
