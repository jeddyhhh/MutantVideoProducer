#include "core/Brand.h"
// File > Create Clip from YouTube: a link (and optionally a part of it) to a
// file next to the project, placed on the timeline at the cursor.

#include "ui/MainWindow.h"

#include "ui/Theme.h"

#include "ui/Help.h"

#include "core/YouTube.h"
#include "ui/TimelineWidget.h"
#include "ui/MediaBin.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QProgressDialog>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include "core/Profiles.h"
#include <QThread>
#include <QTimer>
#include <QTabWidget>
#include <atomic>

bool MainWindow::downloadFile(QWidget* parent, const QUrl& url, const QString& dest, const QString& what, QString* error)
{
    QDir().mkpath(QFileInfo(dest).absolutePath());
    QSaveFile out(dest);
    if (!out.open(QIODevice::WriteOnly)) {
        *error = out.errorString();
        return false;
    }
    QNetworkAccessManager net;
    QNetworkReply* reply = net.get(QNetworkRequest(url));
    QProgressDialog progress(tr("Downloading %1...").arg(what), tr("Cancel"), 0, 1000, parent);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    connect(reply, &QNetworkReply::readyRead, [&] { out.write(reply->readAll()); });
    connect(reply, &QNetworkReply::downloadProgress, [&](qint64 got, qint64 total) {
        if (total > 0)
            progress.setValue(int(got * 1000 / total));
    });
    connect(&progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    out.write(reply->readAll());
    const bool ok = reply->error() == QNetworkReply::NoError;
    if (!ok)
        *error = reply->errorString();
    reply->deleteLater();
    return ok && out.commit();
}

void MainWindow::createClipFromYouTube()
{
    stopPlayback(false);
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Create Clip from YouTube"));
    dlg.setObjectName(QStringLiteral("youtubeDialog"));
    dlg.setMinimumWidth(520);
    auto* form = new QFormLayout(&dlg);
    auto* link = new QLineEdit;
    link->setPlaceholderText(QStringLiteral("https://www.youtube.com/watch?v=..."));
    if (const QString clip = QApplication::clipboard()->text().trimmed(); clip.startsWith(QLatin1String("http")) && !clip.contains(QLatin1Char('\n')))
        link->setText(clip);
    auto* from = new QLineEdit;
    from->setPlaceholderText(tr("the start"));
    auto* to = new QLineEdit;
    to->setPlaceholderText(tr("the end"));
    auto* part = new QHBoxLayout;
    part->addWidget(from);
    part->addWidget(new QLabel(tr("to")));
    part->addWidget(to);
    auto* quality = new QComboBox;
    for (int h : {1080, 720, 480, 360})
        quality->addItem(tr("Up to %1p").arg(h), h);
    quality->addItem(tr("Sound only"), 0);
    QSettings settings;
    quality->setCurrentIndex(std::max(0, quality->findData(settings.value(QStringLiteral("youtube/quality"), 1080))));
    const QString profileFolder = Profiles::instance().folder(QStringLiteral("YouTube"));
    const QString folder = !profileFolder.isEmpty() ? profileFolder : m_project->filePath().isEmpty()
                               ? brand::userFolder(QStandardPaths::MoviesLocation, QStringLiteral("YouTube"))
                               : QDir(QFileInfo(m_project->filePath()).absolutePath()).filePath(QStringLiteral("YouTube"));
    form->addRow(tr("Link"), link);
    form->addRow(tr("Part (optional, e.g. 1:30)"), part);
    form->addRow(tr("Quality"), quality);
    auto* session = new QComboBox;
    session->addItem(tr("None (public videos)"), QString());
    session->addItem(QStringLiteral("Firefox"), QStringLiteral("firefox"));
    session->addItem(QStringLiteral("Chrome"), QStringLiteral("chrome"));
    session->addItem(QStringLiteral("Edge"), QStringLiteral("edge"));
    session->addItem(QStringLiteral("Brave"), QStringLiteral("brave"));
    session->addItem(tr("Cookies file..."), QStringLiteral("file"));
    session->setCurrentIndex(std::max(0, session->findData(settings.value(QStringLiteral("youtube/session")))));
    form->addRow(tr("Browser session (optional)"), session);
    auto* cookies = new QLineEdit(settings.value(QStringLiteral("youtube/cookiesFile")).toString());
    cookies->setPlaceholderText(tr("Netscape-format YouTube cookies.txt"));
    auto* browseCookies = new QPushButton(tr("Browse..."));
    auto* cookieRow = new QHBoxLayout; cookieRow->addWidget(cookies, 1); cookieRow->addWidget(browseCookies);
    form->addRow(tr("Cookies file"), cookieRow);
    auto updateSession = [&] { const bool file = session->currentData() == QStringLiteral("file"); cookies->setEnabled(file); browseCookies->setEnabled(file); };
    connect(session, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&](int) { updateSession(); }); updateSession();
    connect(browseCookies, &QPushButton::clicked, &dlg, [&] {
        const QString path = QFileDialog::getOpenFileName(&dlg, tr("YouTube Cookies File"), cookies->text(), tr("Cookies (*.txt);;All files (*)"));
        if (!path.isEmpty()) cookies->setText(path);
    });
    const QString runtime = youtubeJsRuntime();
    auto* runtimeNote = new QLabel(runtime.isEmpty() ? tr("JavaScript runtime missing: install Deno 2.3+ or Node 22+ to improve YouTube support.")
                                                    : tr("JavaScript challenges: %1 ready").arg(runtime.section(QLatin1Char(':'), 0, 0)));
    runtimeNote->setWordWrap(true); form->addRow(runtimeNote);
    auto* sessionNote = new QLabel(tr("If YouTube asks to verify you're not a bot, open the video in your browser first, then choose that browser or a cookies file. Chrome/Edge cookie encryption can require Firefox or an exported file. Session details are not saved in the project."));
    sessionNote->setWordWrap(true); form->addRow(sessionNote);
    auto* note = new QLabel(tr("Saved in %1 and placed on the timeline at the cursor. Only use videos you have the right to "
                               "use (your own, Creative Commons, or with permission).").arg(QDir::toNativeSeparators(folder)));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("media.md#create-clip-from-youtube"));
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create Clip"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    while (true) {
    if (dlg.exec() != QDialog::Accepted || link->text().trimmed().isEmpty())
        return;
    settings.setValue(QStringLiteral("youtube/quality"), quality->currentData());
    YouTubeOptions o;
    o.url = link->text().trimmed();
    o.maxHeight = quality->currentData().toInt();
    o.folder = folder;
    o.jsRuntime = runtime;
    if (session->currentData() == QStringLiteral("file")) {
        if (!QFileInfo(cookies->text()).isFile()) { QMessageBox::warning(this, tr("YouTube"), tr("Choose an existing cookies file.")); continue; }
        o.cookiesFile = cookies->text();
    } else o.cookiesBrowser = session->currentData().toString();
    settings.setValue(QStringLiteral("youtube/session"), session->currentData());
    settings.setValue(QStringLiteral("youtube/cookiesFile"), cookies->text());
    const TimeUs partFrom = parseClock(from->text()), partTo = parseClock(to->text());
    if ((!from->text().trimmed().isEmpty() && partFrom < 0) || (!to->text().trimmed().isEmpty() && partTo < 0) || (partTo >= 0 && partTo <= partFrom)) {
        QMessageBox::warning(this, tr("Create Clip from YouTube"), tr("The part should be times like 1:30 or 90, the end after the start."));
        continue;
    }

    // yt-dlp: fetched once (it is updated often, as the sites change).
    const QString ytdlp = ytdlpPath();
    QString error;
    if (!QFileInfo::exists(ytdlp)) {
        if (QMessageBox::question(this, tr("Create Clip from YouTube"),
                                  tr("This uses yt-dlp, a free downloader (about 18 MB). Download it from github.com now?")) != QMessageBox::Yes)
            return;
        if (!downloadFile(this, ytdlpDownloadUrl(), ytdlp, QStringLiteral("yt-dlp"), &error)) {
            QMessageBox::warning(this, tr("Create Clip from YouTube"), tr("yt-dlp could not be downloaded: %1").arg(error));
            return;
        }
        QFile(ytdlp).setPermissions(QFile(ytdlp).permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    }
    auto fetch = [&]() -> QString {
        std::atomic<int> permille{-1};
        std::atomic<bool> cancel{false};
        QString path;
        QThread* worker = QThread::create([&] {
            path = youtubeDownload(ytdlp, ffmpegFolder(), o, [&](double p) {
                if (p >= 0)
                    permille = int(p * 1000);
                return !cancel.load();
            }, &error);
        });
        QProgressDialog progress(tr("Downloading from %1...").arg(QUrl(o.url).host()), tr("Cancel"), 0, 0, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        connect(&progress, &QProgressDialog::canceled, this, [&] { cancel = true; });
        QTimer tick;
        connect(&tick, &QTimer::timeout, this, [&] {
            if (permille >= 0) { // (parts come through ffmpeg, without a percentage)
                progress.setMaximum(1000);
                progress.setValue(permille);
            }
        });
        tick.start(100);
        QEventLoop loop;
        connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
        worker->start();
        loop.exec();
        worker->wait();
        delete worker;
        return cancel ? QString() : path;
    };
    QString path = fetch();
    if (!path.isEmpty()) {
        placeDownloadedClip(path, partFrom, partTo);
        return;
    }
    if (error == QLatin1String("cancelled") || error.isEmpty()) return;
    QMessageBox failure(QMessageBox::Warning, tr("Create Clip from YouTube"), youtubeFailureHelp(error), QMessageBox::NoButton, this);
    failure.setDetailedText(error);
    auto* change = failure.addButton(tr("Change Download Options"), QMessageBox::AcceptRole);
    auto* guide = failure.addButton(tr("Troubleshooting Guide"), QMessageBox::HelpRole);
    QPushButton* updateButton = nullptr;
    if (youtubeFailure(error) == YouTubeFailure::Other || youtubeFailure(error) == YouTubeFailure::JavaScript)
        updateButton = failure.addButton(tr("Update Downloader"), QMessageBox::ActionRole);
    failure.addButton(QMessageBox::Close);
    failure.exec();
    if (failure.clickedButton() == change) continue;
    if (failure.clickedButton() == guide) { showHelp(QStringLiteral("media.md#youtube-troubleshooting")); return; }
    if (updateButton && failure.clickedButton() == updateButton) {
        // Download atomically with cancellable progress instead of silently ignoring -U failures.
        QString updateError;
        if (!downloadFile(this, ytdlpDownloadUrl(), ytdlp, QStringLiteral("yt-dlp update"), &updateError))
            QMessageBox::warning(this, tr("YouTube"), tr("The downloader update failed: %1").arg(updateError));
        else QFile(ytdlp).setPermissions(QFile(ytdlp).permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
        continue;
    }
    return;
    }
}

int MainWindow::placeDownloadedClip(const QString& path, TimeUs from, TimeUs to)
{
    // Into Project Media's YouTube folder, the part marked (Mark In / Out), then on the timeline at the cursor.
    importFiles({path});
    for (const auto& [id, media] : m_project->media())
        if (QFileInfo(media->path) == QFileInfo(path)) {
            const bool part = from > 0 || to >= 0;
            const TimeUs end = to >= 0 ? std::min(to, media->info.duration) : media->info.duration;
            m_project->setMediaDetails(id, QStringLiteral("YouTube"), media->rating, part ? std::max<TimeUs>(0, from) : 0, part ? end : 0, QStringLiteral("youtube"));
            m_timeline->insertMedia({id}, m_timeline->cursorTime(), -1);
            if (auto* tabs = findChild<QTabWidget*>(QStringLiteral("dockTabs")))
                for (int i = 0; i < tabs->count(); ++i)
                    if (tabs->widget(i)->isAncestorOf(m_youtubeBin)) { tabs->setCurrentIndex(i); break; }
            return id;
        }
    return 0;
}
