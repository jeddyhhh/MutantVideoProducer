#pragma once

#include "core/Captions.h"
#include <QJsonObject>
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
