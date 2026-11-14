#pragma once

#include <QRegularExpression>
#include <QString>

// `name` as a file name (no folder, no extension) that works on Windows, macOS and Linux: \ / : * ? " < > | and control
// characters become spaces, it ends in no dot or space (Windows drops them), Windows' device names (CON, PRN, AUX, NUL,
// COM1-9, LPT1-9, with or without an extension) get an underscore in front, and it is at most 120 characters;
// `fallback` when nothing is left.
inline QString safeFileName(const QString& name, const QString& fallback)
{
    static const QRegularExpression unsafe(QStringLiteral("[\\\\/:*?\"<>|\\x00-\\x1f]"));
    static const QRegularExpression device(QStringLiteral("^(con|prn|aux|nul|com[1-9]|lpt[1-9])(\\..*)?$"),
                                           QRegularExpression::CaseInsensitiveOption);
    QString s = QString(name).replace(unsafe, QStringLiteral(" ")).simplified().left(120);
    while (s.endsWith(QLatin1Char('.')) || s.endsWith(QLatin1Char(' ')))
        s.chop(1);
    if (s.isEmpty())
        return fallback;
    if (device.match(s).hasMatch())
        s.prepend(QLatin1Char('_'));
    return s;
}
