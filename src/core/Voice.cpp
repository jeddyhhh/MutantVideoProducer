#include "core/Brand.h"
#include "core/Voice.h"
#include "core/AiRuntime.h"
#include "core/Profiles.h"
#include "core/Effects.h"
#include "core/Project.h"
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
#include <map>

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
    return aiRuntimeWorkerPath(QStringLiteral("rvc"), QCoreApplication::applicationDirPath() + QStringLiteral("/voice/rvc-worker/mvp-rvc-worker")
#ifdef _WIN32
        + QStringLiteral(".exe")
#endif
        );
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

std::vector<DialogueLine> dialogueLines(const QString& script)
{
    static const QRegularExpression named(QStringLiteral("^([^:\\[\\(]{1,40}):(.*)$"));
    static const QRegularExpression pause(QStringLiteral("^\\(\\s*pause(?:\\s+(\\d+(?:\\.\\d+)?)\\s*s?)?\\s*\\)$"),
                                          QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression tags(QStringLiteral("^\\s*\\[([^\\]]*)\\]\\s*"));
    std::vector<DialogueLine> lines;
    double waiting = 0; // (a pause belongs to the line after it)
    for (const QString& raw : script.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        if (const auto p = pause.match(line); p.hasMatch()) {
            waiting = std::min(600.0, waiting + (p.captured(1).isEmpty() ? 1.0 : p.captured(1).toDouble()));
            continue;
        }
        const auto m = named.match(line);
        if (m.hasMatch() && !m.captured(1).trimmed().isEmpty()) {
            DialogueLine l{m.captured(1).trimmed(), m.captured(2).trimmed(), waiting, {}};
            for (auto t = tags.match(l.text); t.hasMatch(); t = tags.match(l.text)) {
                for (const QString& name : t.captured(1).split(QLatin1Char(','), Qt::SkipEmptyParts))
                    if (!name.trimmed().isEmpty())
                        l.treatments << name.trimmed().toLower();
                l.text = l.text.mid(t.capturedLength());
            }
            lines.push_back(l);
            waiting = 0;
        } else if (!lines.empty())
            lines.back().text += (lines.back().text.isEmpty() ? QString() : QStringLiteral(" ")) + line;
    }
    std::erase_if(lines, [](const DialogueLine& l) { return l.text.isEmpty(); });
    return lines;
}
namespace {
// What each treatment does to a line: FFmpeg filters, or a reverb.
const std::map<QString, QString>& treatmentFilters()
{
    static const std::map<QString, QString> filters{
        {QStringLiteral("radio"), QStringLiteral("highpass=f=500,lowpass=f=3000,acrusher=bits=10:mode=log:aa=1")},
        {QStringLiteral("phone"), QStringLiteral("highpass=f=300,lowpass=f=3400,acrusher=bits=8:mode=log:aa=1")},
        {QStringLiteral("megaphone"), QStringLiteral("bandpass=f=1600:width_type=q:w=1.2,acrusher=bits=8:mode=log:aa=1,volume=1.4")},
        {QStringLiteral("echo"), QStringLiteral("aecho=0.8:0.7:60|120:0.35|0.2")}};
    return filters;
}
void treat(Timeline& tl, Clip& clip, const QString& name)
{
    if (name == QLatin1String("backwards")) {
        clip.reverse = true;
        return;
    }
    if (const auto f = treatmentFilters().find(name); f != treatmentFilters().end()) {
        auto fx = makeEffectOfType(QStringLiteral("a_ffmpeg"), tl.allocId());
        fx.params[QStringLiteral("filter")].text = f->second;
        clip.effects.push_back(fx);
        return;
    }
    const bool dream = name == QLatin1String("dream");
    auto reverb = makeEffectOfType(QStringLiteral("a_reverb"), tl.allocId());
    reverb.params[QStringLiteral("room")].value = dream ? 0.9 : 0.7;
    reverb.params[QStringLiteral("wet")].value = dream ? 0.45 : 0.3;
    reverb.params[QStringLiteral("predelay")].value = dream ? 60 : 20;
    clip.effects.push_back(reverb);
    if (dream) {
        auto pitch = makeEffectOfType(QStringLiteral("a_pitch"), tl.allocId());
        pitch.params[QStringLiteral("semitones")].value = -2;
        clip.effects.push_back(pitch);
    }
}
} // namespace
QStringList dialogueTreatments()
{
    return {QStringLiteral("radio"), QStringLiteral("phone"), QStringLiteral("megaphone"), QStringLiteral("echo"), QStringLiteral("hall"),
            QStringLiteral("dream"), QStringLiteral("backwards")};
}
QString unknownTreatment(const std::vector<DialogueLine>& lines)
{
    for (const auto& l : lines)
        for (const QString& t : l.treatments)
            if (!dialogueTreatments().contains(t))
                return t;
    return {};
}
QStringList dialogueSpeakers(const std::vector<DialogueLine>& lines)
{
    QStringList names;
    for (const auto& l : lines)
        if (!names.contains(l.speaker))
            names.append(l.speaker);
    return names;
}
std::vector<Caption> dialogueCaptions(const std::vector<DialogueLine>& lines, const QString& speaker)
{
    std::vector<Caption> captions;
    TimeUs at = 0;
    for (const auto& l : lines)
        if (l.speaker == speaker) {
            // Room for slow speech: 0.8 s a word plus 4 s, far more than Kokoro needs at normal speed.
            const TimeUs slot = TimeUs(l.text.split(QLatin1Char(' '), Qt::SkipEmptyParts).size() * 800000) + 4 * kUsPerSecond;
            captions.push_back({at, at + slot, l.text, 0});
            at += slot;
        }
    return captions;
}
bool arrangeDialogue(Timeline& tl, const std::vector<DialogueLine>& lines, const std::vector<DialogueCast>& cast, TimeUs start,
                     TimeUs gap, const FrameRate& fps, QString* error)
{
    Timeline out = tl;
    std::map<QString, std::pair<Track*, size_t>> next; // each character's track and their next unused event
    for (const auto& c : cast) {
        auto track = std::find_if(out.tracks.begin(), out.tracks.end(), [&](const Track& t) { return t.id == c.trackId; });
        const auto count = std::count_if(lines.begin(), lines.end(), [&](const DialogueLine& l) { return l.speaker == c.name; });
        if (track == out.tracks.end() || std::ptrdiff_t(track->clips.size()) != count) {
            if (error)
                *error = QStringLiteral("The speech for %1 came back with a different number of lines.").arg(c.name);
            return false;
        }
        track->name = c.name;
        if (std::abs(c.semitones) > 1e-6) {
            auto pitch = makeEffectOfType(QStringLiteral("a_pitch"), out.allocId());
            pitch.params[QStringLiteral("semitones")].value = c.semitones;
            track->effects.push_back(pitch);
        }
        next[c.name] = {&*track, 0};
    }
    TimeUs at = std::max<TimeUs>(0, fps.snap(start));
    for (const auto& l : lines) {
        auto found = next.find(l.speaker);
        if (found == next.end()) {
            if (error)
                *error = QStringLiteral("%1 has no voice.").arg(l.speaker);
            return false;
        }
        Clip& clip = found->second.first->clips[found->second.second++];
        at = fps.snap(at + TimeUs(std::llround(l.pause * kUsPerSecond)));
        clip.start = at;
        clip.name = l.text;
        for (const QString& t : l.treatments)
            treat(out, clip, t);
        at = fps.snap(clip.end() + gap);
    }
    for (auto& [name, slot] : next)
        std::sort(slot.first->clips.begin(), slot.first->clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    tl = std::move(out);
    return true;
}
