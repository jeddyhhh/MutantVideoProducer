#pragma once
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <vector>
struct AiRuntime { QString id, name, python, lock, lockSha256, cudaLock, cudaSha256; };
using AiRuntimeProgress = std::function<bool(double, const QString&)>;
std::vector<AiRuntime> parseAiRuntimes(const QJsonObject&, QString* error = nullptr);
QJsonObject aiRuntimeCatalog();
QString aiRuntimeRoot(); // MVP_AI_RUNTIME_DIR, else the folder chosen in AI Components, else aiRuntimeDefaultRoot()
QString aiRuntimeDefaultRoot();
// Chooses the folder engines install to and load from ("" = the default); refused if it can't be written to.
bool setAiRuntimeRoot(const QString& folder, QString* error = nullptr);
QString aiRuntimeInstalledPath(const AiRuntime&);
QString aiRuntimeWorkerPath(const QString& id, const QString& bundled);
QStringList aiRuntimeWorkerArguments(const QString& id, const QString& program);
QString aiRuntimeCudaLibraries();
// Venvs stay at their permanent location; only the active pointer is published.
bool installAiRuntime(const AiRuntime&, const QString& device, const AiRuntimeProgress&, QString* error);
