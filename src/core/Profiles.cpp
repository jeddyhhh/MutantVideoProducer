#include "core/Profiles.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>
#include <QHash>

Profiles& Profiles::instance() { static Profiles store; return store; }
Profiles::Profiles()
{
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { watchLibrary(); emit libraryChanged(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { watchLibrary(); emit libraryChanged(); });
    watchLibrary();
}
QString Profiles::cleanPath(const QString& path)
{
    QFileInfo fi(path);
    QString p = QDir::cleanPath(fi.canonicalFilePath().isEmpty() ? fi.absoluteFilePath() : fi.canonicalFilePath());
#ifdef Q_OS_WIN
    p = p.toLower();
#endif
    return p;
}
std::vector<ProducerProfile> Profiles::list() const
{
    std::vector<ProducerProfile> result;
    const auto values = QJsonDocument::fromJson(QSettings().value(QStringLiteral("profiles/list")).toByteArray()).array();
    for (const auto& value : values) {
        const auto o = value.toObject();
        if (!o.value("id").toString().isEmpty() && QDir::isAbsolutePath(o.value("home").toString()))
            result.push_back({o.value("id").toString(), o.value("name").toString(), o.value("home").toString()});
    }
    return result;
}
ProducerProfile Profiles::active() const
{
    const QString id = QSettings().value(QStringLiteral("profiles/active")).toString();
    for (const auto& p : list()) if (p.id == id) return p;
    return {};
}
QString Profiles::folder(const QString& kind) const
{
    const auto p = active();
    return p.id.isEmpty() ? QString() : QDir(p.home).filePath(kind);
}
QString Profiles::settingsKey(const QString& key) const
{
    const auto p = active();
    return p.id.isEmpty() ? key : QStringLiteral("profiles/data/") + p.id + QLatin1Char('/') + key;
}
bool Profiles::saveAndActivate(ProducerProfile profile, QString* error)
{
    auto fail = [&](const QString& text) { if (error) *error = text; return false; };
    if (profile.name.trimmed().isEmpty() || profile.home.isEmpty() || !QDir::isAbsolutePath(profile.home))
        return fail(tr("Choose a profile name and an absolute home folder."));
    if (profile.id.isEmpty()) profile.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    profile.name = profile.name.trimmed(); profile.home = QDir::cleanPath(profile.home);
    auto profiles = list();
    bool homeChanged = false;
    for (const auto& p : profiles) if (p.id == profile.id && cleanPath(p.home) != cleanPath(profile.home)) homeChanged = true;
    for (const auto& p : profiles)
        if (p.id != profile.id && cleanPath(p.home) == cleanPath(profile.home)) return fail(tr("That home folder belongs to another profile."));
    for (const QString& kind : {QStringLiteral("Projects"), QStringLiteral("YouTube"), QStringLiteral("Live"), QStringLiteral("Voice"), QStringLiteral("Media"), QStringLiteral("Exports"), QStringLiteral("Voice Assets"), QStringLiteral("Autosave"), QStringLiteral("Speech Models"), QStringLiteral("Pad Sets")})
        if (!QDir().mkpath(QDir(profile.home).filePath(kind))) return fail(tr("Could not create the profile folders. Choose a writable home folder."));
    QSaveFile probe(QDir(profile.home).filePath(QStringLiteral(".mvp-write-") + QUuid::createUuid().toString(QUuid::WithoutBraces)));
    if (!probe.open(QIODevice::WriteOnly) || probe.write("ok") != 2 || !probe.commit()) return fail(tr("The profile home folder is not writable."));
    QFile::remove(probe.fileName());
    bool found = false;
    QJsonArray values;
    for (auto& p : profiles) {
        if (p.id == profile.id) { p = profile; found = true; }
        values.append(QJsonObject{{"id", p.id}, {"name", p.name}, {"home", p.home}});
    }
    if (!found) values.append(QJsonObject{{"id", profile.id}, {"name", profile.name}, {"home", profile.home}});
    QSettings settings;
    if (homeChanged) settings.remove(QStringLiteral("profiles/data/") + profile.id + QStringLiteral("/dirs"));
    settings.setValue(QStringLiteral("profiles/list"), QJsonDocument(values).toJson(QJsonDocument::Compact));
    settings.setValue(QStringLiteral("profiles/active"), profile.id); settings.sync();
    watchLibrary(); emit changed(); emit libraryChanged(); return true;
}
bool Profiles::useLegacy(QString*)
{
    QSettings().remove(QStringLiteral("profiles/active"));
    watchLibrary(); emit changed(); emit libraryChanged(); return true;
}
void Profiles::watchLibrary()
{
    // The shared library filename is a disk contract with existing profile homes.
    if (!m_watcher.files().isEmpty()) m_watcher.removePaths(m_watcher.files());
    if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
    const auto p = active();
    if (p.id.isEmpty()) return;
    if (QFileInfo::exists(p.home)) m_watcher.addPath(p.home);
    const QString file = QDir(p.home).filePath(QStringLiteral("montage-library.json"));
    if (QFileInfo::exists(file)) m_watcher.addPath(file);
}
QJsonArray Profiles::media() const
{
    const auto p = active(); if (p.id.isEmpty()) return {};
    QFile f(QDir(p.home).filePath(QStringLiteral("montage-library.json")));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object().value("media").toArray();
}
QJsonObject Profiles::findMedia(const QString& path) const
{
    const QString clean = cleanPath(path);
    for (const auto& r : media()) if (cleanPath(r.toObject().value("path").toString()) == clean) return r.toObject();
    return {};
}
bool Profiles::remember(const QJsonArray& records, QString* error)
{
    const auto p = active(); if (p.id.isEmpty() || records.isEmpty()) return true;
    auto fail = [&](const QString& text) { if (error) *error = text; return false; };
    const QString path = QDir(p.home).filePath(QStringLiteral("montage-library.json"));
    QLockFile lock(path + QStringLiteral(".lock"));
    if (!lock.tryLock(100)) return fail(tr("The profile media library is busy. Try again."));
    QJsonArray values;
    QFile existing(path);
    if (existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly)) return fail(tr("Could not read the profile media library."));
        QJsonParseError parse;
        const auto doc = QJsonDocument::fromJson(existing.readAll(), &parse);
        if (parse.error != QJsonParseError::NoError || !doc.object().value("media").isArray()) return fail(tr("The profile media library is invalid; it has been preserved."));
        values = doc.object().value("media").toArray(); existing.close();
    }
    const auto before = values;
    QHash<QString, int> positions;
    for (int i = 0; i < values.size(); ++i) positions.insert(cleanPath(values[i].toObject().value("path").toString()), i);
    for (const auto& value : records) {
        auto record = value.toObject(); const QString path = record.value("path").toString();
        if (path.isEmpty()) continue;
        record["path"] = QFileInfo(path).absoluteFilePath();
        const QString key = cleanPath(path); const int found = positions.value(key, -1);
        if (found < 0) { positions.insert(key, values.size()); values.append(record); } else values[found] = record;
    }
    if (values == before) return true;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(QJsonObject{{"version", 1}, {"media", values}}).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return fail(tr("Could not save the profile media library."));
    watchLibrary(); emit libraryChanged(); return true;
}
