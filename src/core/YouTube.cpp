#include "core/Brand.h"
#include "core/YouTube.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>

TimeUs parseClock(const QString& text)
{
    const QStringList parts = text.trimmed().split(QLatin1Char(':'));
    if (text.trimmed().isEmpty() || parts.size() > 3)
        return -1;
    double seconds = 0;
    for (const QString& p : parts) {
        bool ok = false;
        const double v = p.toDouble(&ok);
        if (!ok || v < 0)
            return -1;
        seconds = seconds * 60 + v;
    }
    return TimeUs(seconds * kUsPerSecond);
}

QString ytdlpPath()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("yt-dlp.exe");
#else
    const QString name = QStringLiteral("yt-dlp");
    if (const QString installed = QStandardPaths::findExecutable(name); !installed.isEmpty())
        return installed;
#endif
    return QDir(brand::storageLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("tools/") + name);
}

QUrl ytdlpDownloadUrl()
{
#ifdef Q_OS_WIN
    return QUrl(QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"));
#else
    return QUrl(QStringLiteral("https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux"));
#endif
}

QString ffmpegFolder()
{
#ifdef Q_OS_WIN
    const QString name = QStringLiteral("ffmpeg.exe");
#else
    const QString name = QStringLiteral("ffmpeg");
#endif
    const QDir app(QCoreApplication::applicationDirPath());
    if (QFileInfo::exists(app.filePath(name)))
        return app.absolutePath();
    const QString onPath = QStandardPaths::findExecutable(name);
    return onPath.isEmpty() ? QString() : QFileInfo(onPath).absolutePath();
}

QString youtubeJsRuntime()
{
    const QDir app(QCoreApplication::applicationDirPath());
#ifdef Q_OS_WIN
    const QString suffix = QStringLiteral(".exe");
#else
    const QString suffix;
#endif
    const QString bundled = app.filePath(QStringLiteral("tools/deno") + suffix);
    if (QFileInfo::exists(bundled))
        return QStringLiteral("deno:") + bundled;
    for (const QString& name : {QStringLiteral("deno"), QStringLiteral("node")}) {
        const QString path = QStandardPaths::findExecutable(name);
        if (path.isEmpty())
            continue;
        QProcess version;
        version.start(path, {QStringLiteral("--version")});
        if (!version.waitForFinished(2000)) {
            version.kill();
            version.waitForFinished();
            continue;
        }
        const QString output = QString::fromUtf8(version.readAllStandardOutput());
        static const QRegularExpression re(QStringLiteral("(?:deno\\s+|v)(\\d+)\\.(\\d+)"));
        const auto match = re.match(output);
        const int major = match.captured(1).toInt(), minor = match.captured(2).toInt();
        if (match.hasMatch() && (name == QLatin1String("node") ? major >= 22 : major > 2 || (major == 2 && minor >= 3)))
            return name + QLatin1Char(':') + path;
    }
    return {};
}

QStringList youtubeArguments(const QString& ffmpegDir, const YouTubeOptions& o)
{
    // (--encoding: UTF-8 out, else the console's code page, and a title like "Song*" no longer names the file)
    QStringList args = {QStringLiteral("--ignore-config"),
                        QStringLiteral("--encoding"),
                        QStringLiteral("utf-8"),
                        QStringLiteral("--no-playlist"),
                        QStringLiteral("--newline"),
                        QStringLiteral("--no-colors"),
                        QStringLiteral("--progress"),
                        QStringLiteral("-P"),
                        o.folder,
                        QStringLiteral("-o"),
                        QStringLiteral("%(title).80B [%(id)s].%(ext)s"),
                        QStringLiteral("--print"),
                        QStringLiteral("after_move:filepath"),
                        QStringLiteral("--progress-template"),
                        QStringLiteral("download:PROGRESS %(progress._percent_str)s")};
    args << QStringLiteral("--retries") << QStringLiteral("5") << QStringLiteral("--fragment-retries") << QStringLiteral("5")
         << QStringLiteral("--socket-timeout") << QStringLiteral("30") << QStringLiteral("--sleep-requests") << QStringLiteral("1");
    const QString runtime = o.jsRuntime.isEmpty() ? youtubeJsRuntime() : o.jsRuntime;
    if (!runtime.isEmpty())
        args << QStringLiteral("--js-runtimes") << runtime;
    if (!o.cookiesFile.isEmpty())
        args << QStringLiteral("--cookies") << o.cookiesFile;
    else if (!o.cookiesBrowser.isEmpty())
        args << QStringLiteral("--cookies-from-browser") << o.cookiesBrowser;
    if (!ffmpegDir.isEmpty())
        args << QStringLiteral("--ffmpeg-location") << ffmpegDir;
    if (o.maxHeight > 0) // H.264 and AAC first: the easiest to edit
        args << QStringLiteral("-f") << QStringLiteral("bv*+ba/b") << QStringLiteral("-S")
             << QStringLiteral("res:%1,vcodec:h264,acodec:aac").arg(o.maxHeight) << QStringLiteral("--merge-output-format")
             << QStringLiteral("mp4");
    else
        args << QStringLiteral("-f") << QStringLiteral("ba/b") << QStringLiteral("-S") << QStringLiteral("acodec:aac");
    args << QStringLiteral("--") << o.url;
    return args;
}

YouTubeFailure youtubeFailure(const QString& error)
{
    const QString e = error.toLower();
    if (e.contains(QStringLiteral("cookie"))
        && (e.contains(QStringLiteral("decrypt")) || e.contains(QStringLiteral("database")) || e.contains(QStringLiteral("dpapi"))))
        return YouTubeFailure::Cookies;
    if (e.contains(QStringLiteral("not a bot")) || e.contains(QStringLiteral("sign in")) || e.contains(QStringLiteral("login required")))
        return YouTubeFailure::Authentication;
    if (e.contains(QStringLiteral("429")) || e.contains(QStringLiteral("rate limit")) || e.contains(QStringLiteral("try again later")))
        return YouTubeFailure::RateLimit;
    if (e.contains(QStringLiteral("javascript")) || e.contains(QStringLiteral("js runtime"))
        || e.contains(QStringLiteral("challenge solving")) || e.contains(QStringLiteral("signature extraction")))
        return YouTubeFailure::JavaScript;
    if (e.contains(QStringLiteral("po token")) || e.contains(QStringLiteral("po_token")) || e.contains(QStringLiteral("403")))
        return YouTubeFailure::Token;
    if (e.contains(QStringLiteral("unavailable")) || e.contains(QStringLiteral("private video")) || e.contains(QStringLiteral("removed")))
        return YouTubeFailure::Unavailable;
    return YouTubeFailure::Other;
}
QString youtubeFailureHelp(const QString& error)
{
    switch (youtubeFailure(error)) {
    case YouTubeFailure::Authentication:
        return QStringLiteral("YouTube is asking to verify your session. Open the video in your browser and complete the check, then "
                              "select that browser under Browser session, or choose an exported YouTube cookies file. Updating the "
                              "downloader alone will not fix this. If your network is still blocked, wait and try again later.");
    case YouTubeFailure::Cookies:
        return QStringLiteral("The browser cookies could not be read. Close the browser and try again. Chrome/Edge encryption can prevent "
                              "access; use Firefox or an exported Netscape-format YouTube cookies file instead.");
    case YouTubeFailure::RateLimit:
        return QStringLiteral("YouTube is limiting this session or network. Wait before trying again. Repeated retries or downloader "
                              "updates will not resolve a rate limit.");
    case YouTubeFailure::JavaScript:
        return QStringLiteral("YouTube's JavaScript challenge could not be solved. Mutant Video Producer uses bundled Deno, or Deno 2.3+ / "
                              "Node 22+ on PATH. Check the runtime, then update yt-dlp if needed.");
    case YouTubeFailure::Token:
        return QStringLiteral(
            "YouTube rejected the media request. This can require a fresh browser session or a video-specific proof-of-origin token. See "
            "the YouTube troubleshooting guide; another downloader cannot guarantee access either.");
    case YouTubeFailure::Unavailable:
        return QStringLiteral("This video is unavailable or restricted. Check that you can open it in your browser. Restricted videos need "
                              "a session with access.");
    default:
        return QStringLiteral(
            "Check your internet connection and the video link. Updating yt-dlp can help when YouTube changes its format.");
    }
}
QString youtubeDownload(const QString& ytdlp, const QString& ffmpegDir, const YouTubeOptions& o,
                        const std::function<bool(double)>& progress, QString* error)
{
    if (error)
        error->clear();
    if (!QDir().mkpath(o.folder)) {
        if (error)
            *error = QStringLiteral("Could not create the download folder.");
        return {};
    }
    static const QStringList browsers{QStringLiteral("firefox"), QStringLiteral("chrome"), QStringLiteral("edge"), QStringLiteral("brave"),
                                      QStringLiteral("chromium")};
    if ((!o.cookiesBrowser.isEmpty() && !browsers.contains(o.cookiesBrowser))
        || (!o.cookiesFile.isEmpty() && !QFileInfo::exists(o.cookiesFile))) {
        if (error)
            *error = QStringLiteral("Choose an installed supported browser or an existing cookies file.");
        return {};
    }
    const QStringList args = youtubeArguments(ffmpegDir, o);

    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(ytdlp, args);
    if (!p.waitForStarted()) {
        if (error)
            *error = QStringLiteral("could not run %1").arg(ytdlp);
        return {};
    }
    static const QRegularExpression percent(QStringLiteral("PROGRESS\\s+([0-9.]+)%"));
    QString path, errors, last;
    bool cancelled = false;
    while (p.state() != QProcess::NotRunning) {
        p.waitForReadyRead(100);
        while (p.canReadLine()) {
            const QString line = QString::fromUtf8(p.readLine()).trimmed();
            if (const auto m = percent.match(line); m.hasMatch()) {
                cancelled = !progress(m.captured(1).toDouble() / 100.0);
            } else if (!line.isEmpty()) {
                last = line;
                if (QFileInfo::exists(line))
                    path = line;
            }
        }
        errors += QString::fromUtf8(p.readAllStandardError());
        if (!cancelled)
            cancelled = !progress(-1);
        if (cancelled) {
            p.kill();
            p.waitForFinished();
            if (error)
                *error = QStringLiteral("cancelled");
            return {};
        }
    }
    errors += QString::fromUtf8(p.readAllStandardError());
    for (const QString& line : QString::fromUtf8(p.readAllStandardOutput()).split(QLatin1Char('\n')))
        if (QFileInfo::exists(line.trimmed()))
            path = line.trimmed();
    if (p.exitCode() != 0 || path.isEmpty()) {
        QStringList lines;
        for (const QString& l : errors.split(QLatin1Char('\n')))
            if (l.startsWith(QLatin1String("ERROR")))
                lines << l.trimmed();
        if (error)
            *error = lines.isEmpty()
                ? (errors.isEmpty() ? (last.isEmpty() ? QStringLiteral("yt-dlp failed (%1)").arg(p.exitCode()) : last) : errors.right(6000))
                : lines.join(QLatin1Char('\n')).right(6000);
        return {};
    }
    return path;
}
