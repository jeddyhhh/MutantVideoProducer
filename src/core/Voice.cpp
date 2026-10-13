#include "core/Brand.h"
#include "core/Voice.h"
#include "core/Profiles.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {
QJsonObject catalog()
{
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/voice/catalog.json"));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}
VoiceAsset asset(const QJsonObject& o)
{
    return {o.value("file").toString(), o.value("url").toString(), o.value("sha256").toString(), qint64(o.value("size").toDouble())};
}
}
QString voiceDataDir()
{
    const QString overrideDir = qEnvironmentVariable("MVP_VOICE_DIR");
    if (!overrideDir.isEmpty()) return overrideDir;
    const QString profile = Profiles::instance().folder(QStringLiteral("Voice Assets"));
    return profile.isEmpty() ? brand::storageLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/voice") : profile;
}
VoiceAsset kokoroModelAsset() { return asset(catalog().value("model").toObject()); }
VoiceAsset kokoroGpuModelAsset() { return asset(catalog().value("gpu_model").toObject()); }
std::vector<VoiceAsset> rvcSharedAssets()
{
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/voice/rvc/catalog.json"));
    std::vector<VoiceAsset> result;
    if (f.open(QIODevice::ReadOnly))
        for (const auto& a : QJsonDocument::fromJson(f.readAll()).object().value("assets").toArray()) result.push_back(asset(a.toObject()));
    return result;
}
QString rvcWorkerPath()
{
    return QCoreApplication::applicationDirPath() + QStringLiteral("/voice/rvc-worker/mvp-rvc-worker")
#ifdef _WIN32
        + QStringLiteral(".exe")
#endif
        ;
}
QString voiceAssetPath(const VoiceAsset& a)
{
    const QString local = QDir(voiceDataDir()).filePath(a.file);
    if (QFileInfo::exists(local)) return local;
    const QString bundled = QCoreApplication::applicationDirPath() + QStringLiteral("/voice/assets/") + a.file;
    return QFileInfo::exists(bundled) ? bundled : local;
}
std::vector<LocalVoice> localVoiceCatalog()
{
    std::vector<LocalVoice> result;
    for (const auto& v : catalog().value("voices").toArray()) {
        const auto o = v.toObject();
        result.push_back({o.value("id").toString(), o.value("name").toString(), o.value("language").toString(),
                          o.value("creator").toString(), o.value("license").toString(), asset(o)});
    }
    return result;
}
bool verifyVoiceAsset(const QString& path, const VoiceAsset& a)
{
    if (a.sha256.size() != 64 || a.size <= 0) return false;
    QFile f(path);
    if (f.size() != a.size || !f.open(QIODevice::ReadOnly)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&f) && QString::fromLatin1(hash.result().toHex()) == a.sha256;
}
bool voiceAssetInstalled(const VoiceAsset& a)
{
    // Size is sufficient for listing; generation verifies the full hash again.
    return !a.file.isEmpty() && a.size > 0 && QFileInfo(voiceAssetPath(a)).size() == a.size;
}
std::vector<Caption> voiceSentences(std::vector<Caption> captions, bool group)
{
    std::sort(captions.begin(), captions.end(), [](const Caption& a, const Caption& b) { return a.start < b.start; });
    std::vector<Caption> out;
    static const QRegularExpression endSentence(QStringLiteral("[.!?][\"'”’)]*$"));
    for (Caption c : captions) {
        c.text = c.text.simplified();
        if (c.text.isEmpty() || c.start < 0 || c.end <= c.start) continue;
        if (group && !out.empty()) {
            Caption& p = out.back();
            const TimeUs gap = c.start - p.end;
            if (p.speaker == c.speaker && gap >= 0 && gap <= 350000 && c.end - p.start <= 12 * kUsPerSecond
                && p.text.size() + c.text.size() < 300 && !endSentence.match(p.text).hasMatch()) {
                p.text += QLatin1Char(' ') + c.text;
                p.end = c.end;
                continue;
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}
bool readVoiceTakes(const QString& folder, std::vector<VoiceTake>& takes, QString* error)
{
    takes.clear();
    auto fail = [&] { if (error) *error = QStringLiteral("The voice worker returned an invalid or incomplete result."); return false; };
    QFile f(QDir(folder).filePath(QStringLiteral("result.json")));
    if (!f.open(QIODevice::ReadOnly)) return fail();
    const auto json = QJsonDocument::fromJson(f.readAll()).object();
    const auto list = json.value("takes").toArray();
    if (json.value("version").toInt() != 1 || list.empty() || list.size() > 10000) return fail();
    const QString root = QFileInfo(folder).canonicalFilePath() + QLatin1Char('/');
    std::vector<VoiceTake> parsed;
    TimeUs lastEnd = 0;
    for (const auto& v : list) {
        const auto o = v.toObject();
        const QString path = QFileInfo(QDir(folder).filePath(o.value("file").toString())).canonicalFilePath();
        const double start = o.value("start").toDouble(-1), length = o.value("length").toDouble(-1), speed = o.value("speed").toDouble(-1);
        if (path.isEmpty() || !path.startsWith(root, Qt::CaseInsensitive) || !std::isfinite(start) || !std::isfinite(length)
            || !std::isfinite(speed) || start < 0 || start > 86400 || length <= 0 || length > 600 || speed < 1 || speed > 1.35001) return fail();
        VoiceTake take{path, o.value("text").toString(), TimeUs(std::llround(start * kUsPerSecond)), TimeUs(std::llround(length * kUsPerSecond)), speed,
                       o.value("overrun").toBool(), o.value("shifted").toBool()};
        if (take.start + 1 < lastEnd) return fail();
        take.start = std::max(take.start, lastEnd); // independent rounding can differ by one microsecond
        lastEnd = take.start + take.length;
        parsed.push_back(std::move(take));
    }
    takes = std::move(parsed);
    return true;
}
