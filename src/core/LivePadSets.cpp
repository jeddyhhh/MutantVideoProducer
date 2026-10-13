#include "core/Brand.h"
#include "core/LivePadSets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include "core/Profiles.h"
#include <algorithm>

namespace padsets {
namespace {

// File for a set: its name with anything unsafe in a file name replaced.
QString fileFor(const QString& name)
{
    QString base;
    for (const QChar c : name.trimmed())
        base += c.isLetterOrNumber() || c == QLatin1Char(' ') || c == QLatin1Char('-') || c == QLatin1Char('_') ? c : QLatin1Char('_');
    if (base.isEmpty())
        base = QStringLiteral("set");
    return QDir(directory()).filePath(base + QStringLiteral(".json"));
}

std::optional<LivePadSet> read(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject() || !doc.object().contains(QStringLiteral("pads")))
        return std::nullopt;
    LivePadSet set;
    set.name = doc.object()[QStringLiteral("name")].toString();
    if (set.name.isEmpty())
        set.name = QFileInfo(path).completeBaseName();
    set.pads = livePadsFromJson(doc.object()[QStringLiteral("pads")].toArray());
    return set;
}

} // namespace

QString directory()
{
    const QString dir = qEnvironmentVariableIsSet("MVP_PADSETS_DIR")
                            ? qEnvironmentVariable("MVP_PADSETS_DIR")
                            : !Profiles::instance().folder(QStringLiteral("Pad Sets")).isEmpty() ? Profiles::instance().folder(QStringLiteral("Pad Sets"))
                            : brand::storageLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/pad-sets");
    QDir().mkpath(dir);
    return dir;
}

QStringList names()
{
    QStringList out;
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (const auto set = read(dir.filePath(file)))
            out << set->name;
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    out.removeDuplicates();
    return out;
}

std::optional<LivePadSet> load(const QString& name)
{
    if (auto set = read(fileFor(name)); set && set->name == name)
        return set;
    const QDir dir(directory()); // a file renamed by hand: find it by the name inside
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (auto set = read(dir.filePath(file)); set && set->name == name)
            return set;
    return std::nullopt;
}

bool save(const LivePadSet& set, QString* error)
{
    if (set.name.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("the set needs a name");
        return false;
    }
    QJsonObject o;
    o[QStringLiteral("name")] = set.name.trimmed();
    o[QStringLiteral("pads")] = livePadsToJson(set.pads);
    QSaveFile f(fileFor(set.name));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool remove(const QString& name)
{
    if (QFile::exists(fileFor(name)))
        return QFile::remove(fileFor(name));
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (auto set = read(dir.filePath(file)); set && set->name == name)
            return QFile::remove(dir.filePath(file));
    return false;
}

} // namespace padsets
