#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QProcessEnvironment>
#include <QSettings>
#include <QStandardPaths>

namespace brand {
inline constexpr const char* name = "Mutant Video Producer";

// Keep the previous installation's data in place. Projects can reference these
// folders directly, and copying multi-gigabyte models would waste disk space.
inline QString storageLocation(QStandardPaths::StandardLocation location)
{
    const QString current = QStandardPaths::writableLocation(location);
    if (QCoreApplication::applicationName() != QLatin1String(name)) return current;
    QString previous = current;
    const QString suffix = QStringLiteral("/Mutant Video Producer");
    if (previous.endsWith(suffix)) {
        previous.chop(suffix.size());
        if (previous.endsWith(suffix)) {
            previous.chop(suffix.size());
            previous += QStringLiteral("/Montage");
        }
        previous += QStringLiteral("/Montage");
    }
    return previous != current && QDir(previous).exists() ? previous : current;
}

inline QString userFolder(QStandardPaths::StandardLocation location, const QString& label)
{
    const QDir root(QStandardPaths::writableLocation(location));
    const QString previous = root.filePath(QStringLiteral("Montage ") + label);
    return QDir(previous).exists() ? previous : root.filePath(QString::fromLatin1(name) + QLatin1Char(' ') + label);
}

// Import missing settings once; never overwrite new preferences or remove old
// ones. This includes profile IDs/home folders, recent files and MCP options.
inline bool migratePreferences(QSettings& previous, QSettings& current)
{
    if (current.value(QStringLiteral("migration/legacyPreferences")).toBool()) return true;
    for (const QString& key : previous.allKeys())
        if (!current.contains(key)) current.setValue(key, previous.value(key));
    current.sync();
    if (current.status() != QSettings::NoError) return false;
    current.setValue(QStringLiteral("migration/legacyPreferences"), true);
    current.sync();
    return current.status() == QSettings::NoError;
}
inline void initializeCompatibility()
{
    for (const QString& key : QProcessEnvironment::systemEnvironment().keys()) {
        if (!key.startsWith(QStringLiteral("MONTAGE_"))) continue;
        const QByteArray renamed = (QStringLiteral("MVP_") + key.mid(8)).toUtf8();
        if (!qEnvironmentVariableIsSet(renamed.constData())) qputenv(renamed.constData(), qgetenv(key.toUtf8().constData()));
    }
    if (QCoreApplication::applicationName() == QLatin1String(name)) {
        QSettings previous(QStringLiteral("Montage"), QStringLiteral("Montage"));
        QSettings current;
        migratePreferences(previous, current);
    }
}
} // namespace brand
