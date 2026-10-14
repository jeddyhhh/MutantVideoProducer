#pragma once
#include <QJsonObject>
#include <QStringList>
#include <functional>
#include <vector>
struct AiRuntime { QString id, name, python, lock, lockSha256, cudaLock, cudaSha256; };
using AiRuntimeProgress = std::function<bool(double, const QString&)>;
std::vector<AiRuntime> parseAiRuntimes(const QJsonObject&, QString* error = nullptr);
QJsonObject aiRuntimeCatalog();
QString aiRuntimeRoot();
QString aiRuntimeInstalledPath(const AiRuntime&);
QString aiRuntimeWorkerPath(const QString& id, const QString& bundled);
QStringList aiRuntimeWorkerArguments(const QString& id, const QString& program);
QString aiRuntimeCudaLibraries();
// Venvs stay at their permanent location; only the active pointer is published.
bool installAiRuntime(const AiRuntime&, const QString& device, const AiRuntimeProgress&, QString* error);
