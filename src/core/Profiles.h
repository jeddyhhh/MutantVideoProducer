#pragma once
#include <QObject>
#include <QJsonArray>
#include <QJsonObject>
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
    static QString cleanPath(const QString& path);
signals:
    void changed();
    void libraryChanged();
private:
    Profiles();
    void watchLibrary();
    QFileSystemWatcher m_watcher;
};
