#pragma once
#include "core/Generators.h"
#include <QJsonArray>
#include <map>
#include <memory>

struct Track;
struct Timeline;
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
// Its "visemes" are mouth shapes ([[seconds, shape], ...]: A, E, O, M, F, L, rest) from the lines' words, and
// "track_id" / "signature" let followTrackBindings bring it up to date when the track changes.
// Track bindings follow their tracks: every graphics scene whose binding's track (or the scene's place) changed is
// analysed again. True when any changed. (Project::commitEdit calls it, so the update is part of the same undo step.)
bool trackBindingsStale(const Timeline &tl);
bool followTrackBindings(Timeline &tl, const std::map<int, std::shared_ptr<const MediaItem>> &media);
// The character library: the ones that come with the program (characters next to it) and the ones saved by the user
// (in their profile's Graphics Templates, Characters), by name.
struct CharacterFile {
  QString path;
  QJsonObject character;
  bool builtIn = false;
};
QString characterFolder();
std::vector<CharacterFile> characterLibrary();
// The set library (backgrounds): the program's (sets next to it) and the user's (Graphics Templates, Sets).
QString setFolder();
std::vector<CharacterFile> setLibrary();
// The prop library (things to hold): the program's (props next to it) and the user's (Graphics Templates, Props).
std::vector<CharacterFile> propLibrary();
// Saves a character (motion::characterFrom) to the user's library; its path in *path.
bool saveCharacter(const QJsonObject &character, QString *path, QString *error);
void addGenerators(std::vector<GeneratorDef> &list);
void addPresets(std::vector<GeneratorPreset> &list);
} // namespace graphics
