#pragma once
#include <QFileInfo>
#include <QJsonObject>

namespace project_format {
inline constexpr const char* identifier = "mutant-video-producer-project";
inline constexpr int version = 1;
inline bool hasExtension(const QString& path)
{
    return QFileInfo(path).suffix().compare(QStringLiteral("mvp"), Qt::CaseInsensitive) == 0;
}
inline bool validHeader(const QJsonObject& document)
{
    const auto revision = document.value(QStringLiteral("version"));
    return document.value(QStringLiteral("format")).toString() == QLatin1String(identifier)
        && revision.isDouble() && revision.toDouble() == version;
}
} // namespace project_format
