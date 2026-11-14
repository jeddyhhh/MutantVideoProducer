#include "core/FileNames.h"
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
    return QDir(directory()).filePath(safeFileName(base, QStringLiteral("set")) + QStringLiteral(".json"));
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

// The file holding `name` (by its file name, or one renamed by hand: the name inside); empty: none.
QString find(const QString& name)
{
    if (const auto set = read(fileFor(name)); set && set->name == name)
        return fileFor(name);
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (const auto set = read(dir.filePath(file)); set && set->name == name)
            return dir.filePath(file);
    return {};
}

// Where `name` is saved: its file already, else its own file (numbered when that holds another name: "a/b" and
// "a_b", or "Set" and "set" on Windows, share a file name).
QString target(const QString& name)
{
    if (const QString path = find(name); !path.isEmpty())
        return path;
    QString path = fileFor(name);
    for (int n = 2; QFile::exists(path); ++n)
        path = fileFor(name + QStringLiteral(" %1").arg(n));
    return path;
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
    const QString path = find(name);
    return path.isEmpty() ? std::nullopt : read(path);
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
    QSaveFile f(target(set.name.trimmed()));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool remove(const QString& name)
{
    const QString path = find(name); // (not just its file name's: that may hold another)
    return !path.isEmpty() && QFile::remove(path);
}

} // namespace padsets
