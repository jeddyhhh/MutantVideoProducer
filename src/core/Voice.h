#pragma once

#include "core/Captions.h"
#include <QJsonObject>
#include <QStringList>
#include <QString>
#include <vector>

struct VoiceAsset {
    QString file, url, sha256;
    qint64 size = 0;
};
struct LocalVoice {
    QString id, name, language, creator, license;
    VoiceAsset asset;
};
// The catalog is shipped with the editor; downloads are pinned and SHA-256 checked.
QString voiceDataDir();
QString voiceAssetPath(const VoiceAsset& asset);
VoiceAsset kokoroModelAsset();
VoiceAsset kokoroGpuModelAsset();
std::vector<VoiceAsset> rvcSharedAssets();
QString rvcWorkerPath();
std::vector<LocalVoice> localVoiceCatalog();
bool voiceAssetInstalled(const VoiceAsset& asset);
bool verifyVoiceAsset(const QString& path, const VoiceAsset& asset);
// Joins adjacent short cues into sentences without bridging speakers, overlaps,
// long gaps or long passages. Input need not be sorted.
std::vector<Caption> voiceSentences(std::vector<Caption> captions, bool group);

struct VoiceTake {
    QString path, text;
    TimeUs start = 0, length = 0;
    double speed = 1;
    bool overrun = false, shifted = false;
};
// Validates a worker result before anything is added to the project.
bool readVoiceTakes(const QString& folder, std::vector<VoiceTake>& takes, QString* error);

// A dialogue script: "Name: line" per line; a line without a name continues the last speaker's line. Blank lines and
// lines starting with # are skipped.
struct DialogueLine {
    QString speaker, text;
};
std::vector<DialogueLine> dialogueLines(const QString& script);
QStringList dialogueSpeakers(const std::vector<DialogueLine>& lines); // in order of first appearance
// Each character's lines spoken on their own: captions spaced so natural-length speech never overlaps.
std::vector<Caption> dialogueCaptions(const std::vector<DialogueLine>& lines, const QString& speaker);
struct DialogueCast {
    QString name;
    int trackId = 0;       // the track their speech was generated on, one event per line in order
    double semitones = 0;  // a Pitch Shift on the track when not 0
};
struct Timeline;
struct FrameRate;
// Puts every line in script order from start with gap between lines, names each track after its character and gives it
// its pitch. Fails (leaving tl alone) when a track doesn't hold exactly that character's lines.
bool arrangeDialogue(Timeline& tl, const std::vector<DialogueLine>& lines, const std::vector<DialogueCast>& cast, TimeUs start,
                     TimeUs gap, const FrameRate& fps, QString* error);
