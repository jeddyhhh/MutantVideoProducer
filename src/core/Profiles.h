#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QString>
#include <vector>

struct ProducerProfile { QString id, name, home; };
// Profiles locate user content. Executables and temporary caches remain application-wide.
class Profiles : public QObject {
    Q_OBJECT
public:
    static Profiles& instance();
    std::vector<ProducerProfile> list() const;
    ProducerProfile active() const;
    bool saveAndActivate(ProducerProfile profile, QString* error = nullptr);
    bool useLegacy(QString* error = nullptr);
    QString folder(const QString& kind) const; // empty for legacy folders
    QString settingsKey(const QString& key) const;
    QJsonArray media() const;
    bool remember(const QJsonArray& records, QString* error = nullptr);
    QJsonObject findMedia(const QString& path) const;
    static QString cleanPath(const QString& path); // resolves links: reads the disk
    // A library file's identity without touching the disk (the library holds thousands, often on slow drives).
    static QString libraryKey(const QString& path);
signals:
    void changed();
    void libraryChanged();
private:
    Profiles();
    void watchLibrary();
    QFileSystemWatcher m_watcher;
    QDateTime m_written; // our last write of the library, to tell it from a change made elsewhere
    qint64 m_writtenSize = -1;
};
