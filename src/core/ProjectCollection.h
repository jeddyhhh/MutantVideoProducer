#pragma once
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>

namespace collection {
enum class Role { Media, Asset, VoiceModel, Plugin, Font };
using Rewrite = std::function<QString(const QString&, Role)>;
// Visits file references by schema, never arbitrary text or path-following layer IDs.
void rewriteFiles(QJsonObject& document, const Rewrite& rewrite);
struct Options {
    bool includeUnusedMedia = true;
    bool includeVoiceModels = true;
};
struct Result {
    bool ok = false, cancelled = false;
    QString error, projectPath, reportPath;
    int copiedFiles = 0;
    qint64 copiedBytes = 0;
    QStringList missing, warnings;
};
using Progress = std::function<bool(qint64 done, qint64 total, const QString& file)>;
// Input is an immutable serialized edit. Publishes a new folder only after all writes succeed.
Result collect(QJsonObject document, const QString& sourceDirectory, const QString& destination,
               const QString& projectName, const Options& options = {}, const Progress& progress = {});
}
