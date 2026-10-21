#include "core/Brand.h"
#include "core/ProjectFormat.h"
#include "core/Profiles.h"
#include "core/Voice.h"
#include "core/LiveRecorder.h"
#include <QUuid>
#include "ui/MainWindow.h"
#include "ui/RenderQueue.h"
#include "ui/DeliverView.h"

#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/FFmpeg.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/FormatRegistry.h"
#include "core/Project.h"
#include "engine/MediaCache.h"
#include "engine/PlaybackEngine.h"
#include "ui/AudioMeter.h"
#include "ui/AudioFinishingWindow.h"
#include "ui/DeviceChainView.h"
#include "ui/EffectEditor.h"
#include "ui/KeyframeEditor.h"
#include "ui/EventPropertiesDialog.h"
#include "ui/EffectsBrowser.h"
#include "ui/CrashReport.h"
#include "ui/ExportDialog.h"
#include "ui/StoryPanel.h"
#include "ui/Vst3Window.h"
#include "../../tests/transport.h"
#include "../../tests/collection-ui.h"
#include "core/Story.h"
#include "ui/RenderStatusDialog.h"
#include "ui/Help.h"
#include "ui/LiveClipsBrowser.h"
#include "ui/LiveView.h"
#include "engine/LiveEngine.h"
#include "ui/MediaBin.h"
#include "ui/MixerWindow.h"
#include "ui/PreviewWidget.h"
#include "ui/ProjectSettingsDialog.h"
#include "ui/RelinkDialog.h"
#include "ui/ShortcutsDialog.h"
#include "ui/Theme.h"
#include "ui/TimelineWidget.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QSlider>
#include <QCloseEvent>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLockFile>
#include <QPushButton>
#include <QToolButton>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QOffscreenSurface>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QProgressDialog>
#include "core/BeatMarkers.h"
#include "core/Captions.h"
#include "core/Vst3Host.h"
#include "core/Proxy.h"
#include "core/Scopes.h"
#include "ui/ProxyQueue.h"
#include "ui/McpServer.h"
#include "core/AudioEffects.h"
#include "core/MediaInfo.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QProcess>
#include <QTcpSocket>
#include <QRegularExpression>
#include <random>
#include <QGuiApplication>
#include <QSpinBox>
#include <QTabWidget>
#include <QUndoView>
#include <QDoubleSpinBox>
#include <QWheelEvent>
#include "core/Generators.h"
#include "core/TimelineOps.h"
#include "ui/ScopesWidget.h"
#include "core/Stabilizer.h"
#include <atomic>
#include <thread>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QProgressDialog>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QDoubleSpinBox>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QToolBar>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <atomic>
#include <thread>

namespace {
const QString kProjectFilter = QStringLiteral("Mutant Video Producer project (*.mvp);;All files (*)");
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    {
        QSettings settings;
        hw::settings().gpuCompositing = settings.value(QStringLiteral("gpu/compositing"), true).toBool();
        hw::settings().hardwareDecoding = settings.value(QStringLiteral("gpu/hardwareDecoding"), true).toBool();
    }
    m_project = new Project(this);
    m_cache = new MediaCache(this);
    m_engine = new PlaybackEngine(this);
    m_engine->setOutputLatency(TimeUs(QSettings().value(QStringLiteral("play/outputLatencyMs"), 0).toInt()) * 1000);
    createLayout();
    createActions();
    ShortcutsDialog::applySaved(ShortcutsDialog::commands(this)); // (Options > Keyboard Shortcuts)
    connectSignals();
    updateTitle();
    updatePreviewInfo();
    refreshPreview();

    QSettings settings;
    // Autosave: a copy of the project every few minutes while it has unsaved
    // changes, in this session's own file. The lock file next to it shows
    // the session is alive; a copy without a live lock was left by a crash.
    {
        const QString base = QDir(autosaveDir()).filePath(QStringLiteral("session-%1-%2")
                                                              .arg(QCoreApplication::applicationPid())
                                                              .arg(QDateTime::currentMSecsSinceEpoch()));
        m_autosavePath = base + QStringLiteral(".mvp");
        m_autosaveLock = std::make_unique<QLockFile>(base + QStringLiteral(".lock"));
        m_autosaveLock->setStaleLockTime(0);
        m_autosaveLock->tryLock(0);
        connect(&m_autosaveTimer, &QTimer::timeout, this, &MainWindow::autosave);
        const int minutes = settings.value(QStringLiteral("autosave/minutes"), 2).toInt();
        if (minutes > 0)
            m_autosaveTimer.start(minutes * 60000);
    }
    m_shuttleTimer.setInterval(15);
    connect(&m_shuttleTimer, &QTimer::timeout, this, &MainWindow::onShuttleTick);
    m_lastRipple = std::clamp(settings.value(QStringLiteral("edit/lastRipple"), 1).toInt(), 1, 2);
    setRippleMode(std::clamp(settings.value(QStringLiteral("edit/rippleMode"), 0).toInt(), 0, 2));

    restoreGeometry(settings.value(QStringLiteral("window/geometry")).toByteArray());
    m_mainSplit->restoreState(settings.value(QStringLiteral("window/mainSplit2")).toByteArray());
    m_topSplit->restoreState(settings.value(QStringLiteral("window/topSplit")).toByteArray());
    if (!settings.contains(QStringLiteral("window/mainSplit2")))
        m_mainSplit->setSizes({330, 360, 270});
    if (!settings.contains(QStringLiteral("window/geometry")))
        resize(1500, 950);
}

MainWindow::~MainWindow()
{
    m_engine->stop();
    removeAutosave(); // a clean exit leaves nothing to recover
    // The project is a child deleted after this body, when the window's members are gone:
    // clearing its undo stack then signals cleanChanged, which must not reach updateTitle.
    m_project->undoStack()->disconnect(this);
    m_project->disconnect(this);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

void MainWindow::updateTitle()
{
    const QString name = m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).fileName();
    setWindowTitle(QStringLiteral("%1%2 - Mutant Video Producer").arg(name, m_project->isModified() ? QStringLiteral(" *") : QString()));
    const auto profile = Profiles::instance().active();
    if (!profile.id.isEmpty()) setWindowTitle(windowTitle() + QStringLiteral(" [%1]").arg(profile.name));
}

void MainWindow::updatePreviewInfo()
{
    const ProjectSettings& s = m_project->settings();
    const int d = m_quality->currentData().toInt();
    m_previewInfo->setText(tr("Project: %1x%2, %3 fps    Preview: %4x%5")
                               .arg(s.width)
                               .arg(s.height)
                               .arg(s.fps.toString())
                               .arg(s.width / d)
                               .arg(s.height / d));
    m_preview->setProjectAspect(double(s.width) / s.height);
}

void MainWindow::refreshPreview()
{
    m_engine->setSnapshot(m_project->snapshot());
    m_engine->requestFrame(m_timeline->cursorTime());
}

bool MainWindow::maybeSave()
{
    if (!m_project->isModified())
        return true;
    QMessageBox box(QMessageBox::Warning, tr("Mutant Video Producer"), tr("Save changes to the project?"),
                    QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, this);
    box.setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen));
    const auto ret = box.exec();
    if (ret == QMessageBox::Save)
        return save();
    return ret == QMessageBox::Discard;
}

void MainWindow::newProject(const std::optional<ProjectSettings>& settings)
{
    // newProjectDialog already resolved unsaved changes before choosing settings.
    stopPlayback(false);
    m_project->clear(settings.value_or(ProjectSettings{}));
    removeAutosave();
    m_settingsMatched = settings.has_value();
    m_timeline->setRegion(0, 0);
    m_timeline->setCursorTime(0);
    refreshPreview();
}

bool MainWindow::openProject(const QString& path)
{
    stopPlayback(false);
    QString error;
    QStringList missing;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = m_project->load(path, &error, &missing);
    QApplication::restoreOverrideCursor();
    if (!ok) {
        QMessageBox::critical(this, tr("Open Project"), tr("Could not open %1:\n%2").arg(path, error));
        return false;
    }
    removeAutosave();
    addRecent(path);
    m_settingsMatched = true;
    m_timeline->setRegion(0, 0);
    m_timeline->setCursorTime(0);
    m_timeline->zoomToFit();
    refreshPreview();
    updateTitle();
    if (!missing.isEmpty())
        relinkMissing();
    return true;
}

void MainWindow::openProjectDialog()
{
    if (!maybeSave())
        return;
    QSettings settings;
    const QString path = QFileDialog::getOpenFileName(this, tr("Open Project"),
                                                      settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/project")), Profiles::instance().folder(QStringLiteral("Projects"))).toString(), kProjectFilter);
    if (path.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/project")), QFileInfo(path).absolutePath());
    openProject(path);
}

bool MainWindow::save()
{
    if (m_project->filePath().isEmpty())
        return saveAs();
    QString error;
    if (!m_project->save(m_project->filePath(), &error)) {
        QMessageBox::critical(this, tr("Save"), tr("Could not save:\n%1").arg(error));
        return false;
    }
    removeAutosave();
    addRecent(m_project->filePath());
    updateTitle();
    statusBar()->showMessage(tr("Saved %1").arg(m_project->filePath()), 3000);
    return true;
}

bool MainWindow::saveAs()
{
    QSettings settings;
    QString path = QFileDialog::getSaveFileName(this, tr("Save Project As"),
                                                settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/project")), Profiles::instance().folder(QStringLiteral("Projects"))).toString(), kProjectFilter);
    if (path.isEmpty())
        return false;
    if (QFileInfo(path).suffix().compare(QStringLiteral("mvp"), Qt::CaseInsensitive) != 0)
        path += QStringLiteral(".mvp");
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/project")), QFileInfo(path).absolutePath());
    QString error;
    if (!m_project->save(path, &error)) {
        QMessageBox::critical(this, tr("Save"), tr("Could not save:\n%1").arg(error));
        return false;
    }
    removeAutosave();
    addRecent(path);
    updateTitle();
    return true;
}

void MainWindow::importDialog()
{
    QSettings settings;
    static const QString filter = formats::importFileFilter();
    const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Import Media"),
                                                            settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/media")), Profiles::instance().folder(QStringLiteral("Media"))).toString(), filter);
    if (paths.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/media")), QFileInfo(paths.front()).absolutePath());
    importFiles(paths);
}

#ifdef Q_OS_WIN
static constexpr Qt::CaseSensitivity kPathCase = Qt::CaseInsensitive;
#else
static constexpr Qt::CaseSensitivity kPathCase = Qt::CaseSensitive;
#endif

void MainWindow::importFiles(const QStringList& paths, std::optional<TimeUs> at, int trackIndex, bool newVideoTracks)
{
    QStringList failures;
    std::vector<int> imported;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (const QString& path : paths) {
        const QString clean = QFileInfo(path).absoluteFilePath();
        int existing = 0;
        for (const auto& [id, m] : m_project->media())
            if (m && QFileInfo(m->path).absoluteFilePath().compare(clean, kPathCase) == 0)
                existing = id;
        if (existing) {
            imported.push_back(existing);
            continue;
        }
        statusBar()->showMessage(tr("Importing %1...").arg(QFileInfo(path).fileName()));
        QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        const MediaInfo info = probeMedia(path);
        if (!info.ok()) {
            failures << QStringLiteral("%1: %2").arg(QFileInfo(path).fileName(), info.error);
            continue;
        }
        const auto shared = Profiles::instance().findMedia(path);
        const int id = m_project->addMedia(info);
        if (!shared.isEmpty()) m_project->setMediaDetails(id, shared.value("folder").toString(), shared.value("rating").toInt(),
            TimeUs(shared.value("in").toDouble()), TimeUs(shared.value("out").toDouble()), shared.value("origin").toString());
        imported.push_back(id);
    }
    QApplication::restoreOverrideCursor();
    statusBar()->showMessage(tr("Imported %1 file(s)").arg(imported.size()), 4000);
    if (QSettings().value(QStringLiteral("proxy/auto"), true).toBool()) { // heavy files get a proxy in the background
        QList<int> heavy;
        for (int id : imported)
            if (auto m = m_project->mediaItem(id); m && proxy::recommended(m->info))
                heavy << id;
        if (!heavy.isEmpty())
            createProxies(heavy, true);
    }

    // The first video brought into an empty project defines its format.
    if (!m_settingsMatched && m_project->timeline().duration() == 0) {
        for (int id : imported) {
            auto m = m_project->mediaItem(id);
            if (m && m->info.hasVideo() && !m->info.isStill) {
                m_project->setSettings(settingsForMedia(m->info.video.front(), m_project->settings()));
                m_settingsMatched = true;
                statusBar()->showMessage(tr("Project set to match %1 (%2x%3, %4 fps). Change in File > Properties.")
                                             .arg(m->displayName())
                                             .arg(m_project->settings().width)
                                             .arg(m_project->settings().height)
                                             .arg(m_project->settings().fps.toString()),
                                         8000);
                break;
            }
        }
    }

    if (at && !imported.empty())
        m_timeline->insertMedia(imported, *at, trackIndex, newVideoTracks);
    if (!failures.isEmpty())
        QMessageBox::warning(this, tr("Import"), tr("Some files could not be imported:\n\n%1").arg(failures.join(QLatin1Char('\n'))));
}

void MainWindow::seekTo(TimeUs t)
{
    stopPlayback(false);
    m_timeline->setCursorTime(t);
    m_engine->requestFrame(m_timeline->cursorTime());
}

void MainWindow::projectProperties()
{
    ProjectSettingsDialog dlg(m_project->settings(), m_project->media(), this);
    if (dlg.exec() != QDialog::Accepted)
        return;
    m_project->setSettings(dlg.settings());
    m_settingsMatched = true;
    refreshPreview();
}

void MainWindow::renderAs()
{
    stopPlayback(false);
    const TimeUs duration = m_project->timeline().duration();
    if (duration <= 0) {
        QMessageBox::information(this, tr("Render As"), tr("The timeline is empty."));
        return;
    }
    QSettings settings;
    QString dir = settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/render"))).toString();
    if (dir.isEmpty())
        dir = Profiles::instance().folder(QStringLiteral("Exports"));
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString base = m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName();
    ExportDialog dlg(m_project->settings(), duration, m_timeline->hasRegion(), m_timeline->regionStart(),
                     m_timeline->regionEnd(), QDir(dir).filePath(base + QStringLiteral(".mp4")), this);
    dlg.setCaptions(timelineCaptions(m_project->timeline()));
    if (dlg.exec() != QDialog::Accepted)
        return;
    const ExportSettings s = dlg.settings();
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/render")), QFileInfo(s.path).absolutePath());
    if (dlg.queued()) {
        QString error;
        if (!renderQueue()->add(base, m_project->snapshot(), s, &error))
            QMessageBox::information(this, tr("Render Queue"), error);
        renderQueue()->show();
        renderQueue()->raise();
        return;
    }

    auto snap = m_project->snapshot();
    // The GPU surface must be created here on the GUI thread; the worker owns the GL context.
    std::unique_ptr<QOffscreenSurface> gpuSurface = hw::settings().gpuCompositing ? createGpuSurface() : nullptr;
    auto monitor = std::make_shared<RenderMonitor>();
    bool ok = false;
    std::thread worker([&] {
        Exporter exporter;
        ok = exporter.run(*snap, s, [&](double) { return !monitor->cancel.load(); }, gpuSurface.get(),
                          [&](const ExportStatus& value) { monitor->update(value); });
        monitor->done = true;
    });
    RenderStatusDialog status(s, *snap, monitor, this);
    status.setWindowModality(Qt::WindowModal);
    status.exec();
    monitor->cancel = true;
    worker.join();
    if (ok) {
        DeliverView::remember(m_project->filePath(), s.path, dlg.templateName());
        Profiles::instance().remember({QJsonObject{{"path", s.path}, {"folder", "Exports"}, {"origin", "render"}}});
    }
}

RenderQueue* MainWindow::renderQueue()
{
    if (!m_renderQueue) {
        m_renderQueue = new RenderQueue(this);
        connect(m_renderQueue, &RenderQueue::addProjectRequested, this, &MainWindow::queueProjectFile);
        connect(m_renderQueue, &RenderQueue::rendered, this, [this](const QString& path) {
            DeliverView::remember(m_project->filePath(), path);
            Profiles::instance().remember({QJsonObject{{"path", path}, {"folder", "Exports"}, {"origin", "render"}}});
        });
    }
    return m_renderQueue;
}

void MainWindow::queueProjectFile()
{
    QSettings settings;
    const QString file = QFileDialog::getOpenFileName(renderQueue(), tr("Add Project to the Render Queue"),
                                                      Profiles::instance().folder(QStringLiteral("Projects")), kProjectFilter);
    if (file.isEmpty())
        return;
    Project other; // loaded on its own: the open project stays as it is
    QString error;
    QStringList missing;
    if (!other.load(file, &error, &missing)) {
        QMessageBox::warning(renderQueue(), tr("Render Queue"), error);
        return;
    }
    if (!missing.isEmpty()
        && QMessageBox::question(renderQueue(), tr("Render Queue"),
                                 tr("%n file(s) of this project can't be found and will render as blank. Queue it anyway?", nullptr, int(missing.size())))
               != QMessageBox::Yes)
        return;
    const TimeUs duration = other.timeline().duration();
    if (duration <= 0) {
        QMessageBox::information(renderQueue(), tr("Render Queue"), tr("That project's timeline is empty."));
        return;
    }
    QString dir = settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/render"))).toString();
    if (dir.isEmpty())
        dir = Profiles::instance().folder(QStringLiteral("Exports"));
    const QString base = QFileInfo(file).completeBaseName();
    ExportDialog dlg(other.settings(), duration, false, 0, 0, QDir(dir).filePath(base + QStringLiteral(".mp4")), renderQueue());
    dlg.setCaptions(timelineCaptions(other.timeline()));
    if (dlg.exec() != QDialog::Accepted)
        return; // (Render and Add to Render Queue both queue it here)
    if (!renderQueue()->add(base, other.snapshot(), dlg.settings(), &error))
        QMessageBox::information(renderQueue(), tr("Render Queue"), error);
}

void MainWindow::showFormats()
{
    const auto o = formats::overview();
    QString text;
    text += tr("Containers that can be read (%1):\n").arg(o.demuxers.size()) + o.demuxers.join('\n') + "\n\n";
    text += tr("Video decoders (%1):\n").arg(o.videoDecoders.size()) + o.videoDecoders.join('\n') + "\n\n";
    text += tr("Audio decoders (%1):\n").arg(o.audioDecoders.size()) + o.audioDecoders.join('\n') + "\n\n";
    text += tr("Containers that can be written (%1):\n").arg(formats::muxers().size());
    for (const auto& m : formats::muxers())
        text += QStringLiteral("%1 - %2\n").arg(m.name, m.longName);
    text += "\n" + tr("Video encoders (%1):\n").arg(o.videoEncoders.size()) + o.videoEncoders.join('\n') + "\n\n";
    text += tr("Audio encoders (%1):\n").arg(o.audioEncoders.size()) + o.audioEncoders.join('\n');

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Supported Formats"));
    dlg.resize(700, 600);
    auto* layout = new QVBoxLayout(&dlg);
    auto* view = new QPlainTextEdit(text);
    view->setReadOnly(true);
    layout->addWidget(view);
    dlg.exec();
}

void MainWindow::about()
{
    QMessageBox::about(this, tr("About Mutant Video Producer"),
                       tr("<b>Mutant Video Producer %1</b><br>A non-linear video editor.<br><br>"
                          "Free software under the GNU General Public License, version 3 or later, "
                          "with no warranty. <a href=\"https://github.com/jeddyhhh/MutantVideoProducer\">Source code</a><br><br>"
                          "Media I/O by FFmpeg %2 (libavcodec %3)<br>UI by Qt %4")
                           .arg(QApplication::applicationVersion(), QString::fromUtf8(av_version_info()))
                           .arg(QStringLiteral("%1.%2.%3")
                                    .arg(LIBAVCODEC_VERSION_MAJOR)
                                    .arg(LIBAVCODEC_VERSION_MINOR)
                                    .arg(LIBAVCODEC_VERSION_MICRO))
                           .arg(QString::fromLatin1(qVersion())));
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (m_renderQueue && (m_renderQueue->busy() || m_renderQueue->waiting())
        && QMessageBox::question(this, tr("Render Queue"), tr("Renders are still running or waiting. Quit and cancel them?"))
               != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (!maybeSave()) {
        e->ignore();
        return;
    }
    stopPlayback(false);
    QSettings settings;
    settings.setValue(QStringLiteral("window/geometry"), saveGeometry());
    settings.setValue(QStringLiteral("window/mainSplit2"), m_mainSplit->saveState());
    settings.setValue(QStringLiteral("window/topSplit"), m_topSplit->saveState());
    e->accept();
}

// ---------------------------------------------------------------------------
// Playback
// ---------------------------------------------------------------------------

void MainWindow::play(bool fromRegionStart)
{
    TimeUs from = m_timeline->cursorTime();
    if (m_loopAction->isChecked() && m_timeline->hasRegion()
        && (fromRegionStart || from < m_timeline->regionStart() || from >= m_timeline->regionEnd()))
        from = m_timeline->regionStart();
    m_playStart = m_timeline->cursorTime();
    m_engine->setSnapshot(m_project->snapshot());
    m_engine->play(from);
    m_timeline->setPlayhead(from);
    theme::setIcon(m_playAction, theme::Icon::Stop);
}

void MainWindow::togglePlay()
{
    if (m_modes && m_modes->currentWidget() == m_live) { // Space on the Live tab: start / stop capture
        m_live->startStop();
        return;
    }
    if (m_shuttleRate != 0) {
        stopShuttle();
        return;
    }
    if (m_engine->isPlaying())
        stopPlayback(true);
    else
        play(false);
}

void MainWindow::pausePlay()
{
    if (m_shuttleRate != 0) {
        stopShuttle();
        return;
    }
    if (!m_engine->isPlaying()) {
        play(false);
        return;
    }
    const TimeUs pos = m_project->settings().fps.snap(m_engine->position());
    stopPlayback(false);
    m_timeline->setCursorTime(pos);
    m_engine->requestFrame(pos);
}

void MainWindow::stopPlayback(bool returnToStart)
{
    stopShuttle();
    if (!m_engine->isPlaying())
        return;
    m_engine->stop();
    m_timeline->setPlayhead(std::nullopt);
    theme::setIcon(m_playAction, theme::Icon::Play);
    if (returnToStart)
        m_timeline->setCursorTime(m_playStart);
    m_engine->requestFrame(m_timeline->cursorTime());
}

void MainWindow::onPlaybackPosition(TimeUs t)
{
    if (m_loopAction->isChecked() && m_timeline->hasRegion()) {
        if (t >= m_timeline->regionEnd()) {
            m_engine->play(m_timeline->regionStart());
            return;
        }
    } else if (t >= m_project->timeline().duration()) {
        stopPlayback(false);
        m_timeline->setCursorTime(m_project->timeline().duration());
        return;
    }
    m_timeline->setPlayhead(t);
}

// ---------------------------------------------------------------------------
// J / K / L shuttle
// ---------------------------------------------------------------------------

bool MainWindow::editorActive() const
{
    return !m_modes || m_modes->currentIndex() == 0;
}

TimeUs MainWindow::currentTime() const
{
    if (m_shuttleRate == 0 && m_engine->isPlaying())
        return m_engine->position();
    return m_timeline->cursorTime();
}

void MainWindow::shuttle(int direction)
{
    if (!editorActive())
        return;
    const TimeUs pos = m_project->settings().fps.snap(currentTime());
    if (direction == 0) { // K: stop where we are
        if (m_shuttleRate != 0)
            stopShuttle();
        else if (m_engine->isPlaying())
            pausePlay();
        return;
    }
    // Each press in the same direction doubles the speed (up to 8x);
    // the other direction starts again at 1x.
    double rate = m_shuttleRate != 0 ? m_shuttleRate : (m_engine->isPlaying() ? 1.0 : 0.0);
    if (direction > 0)
        rate = rate >= 1.0 ? std::min(rate * 2.0, 8.0) : 1.0;
    else
        rate = rate <= -1.0 ? std::max(rate * 2.0, -8.0) : -1.0;
    stopPlayback(false);
    m_timeline->setCursorTime(pos);
    if (rate == 1.0) {
        play(false); // normal playback, with sound
        statusBar()->showMessage(tr("Play ▶ 1x"), 1500);
        return;
    }
    m_shuttleRate = rate;
    m_shuttleFrom = pos;
    m_shuttleClock.start();
    m_shuttleTimer.start();
    theme::setIcon(m_playAction, theme::Icon::Stop);
    statusBar()->showMessage(rate > 0 ? tr("Shuttle ▶▶ %1x (no sound; K or Space stops)").arg(rate)
                                      : tr("Shuttle ◀◀ %1x (no sound; K or Space stops)").arg(-rate));
}

void MainWindow::stopShuttle()
{
    if (m_shuttleRate == 0)
        return;
    m_shuttleTimer.stop();
    m_shuttleRate = 0;
    theme::setIcon(m_playAction, theme::Icon::Play);
    statusBar()->clearMessage();
    m_engine->requestFrame(m_timeline->cursorTime());
}

void MainWindow::onShuttleTick()
{
    const TimeUs end = m_project->timeline().duration();
    TimeUs t = m_shuttleFrom + TimeUs(m_shuttleRate * double(m_shuttleClock.nsecsElapsed()) / 1000.0);
    const bool atEdge = t <= 0 || t >= end;
    t = m_project->settings().fps.snap(std::clamp<TimeUs>(t, 0, std::max<TimeUs>(0, end)));
    m_timeline->setCursorTime(t);
    m_engine->requestFrame(t);
    if (atEdge)
        stopShuttle();
}

// ---------------------------------------------------------------------------
// Autosave, recovery, recent projects, relinking, ripple
// ---------------------------------------------------------------------------

QString MainWindow::autosaveDir()
{
    const QString dir = qEnvironmentVariableIsSet("MVP_AUTOSAVE_DIR")
                            ? qEnvironmentVariable("MVP_AUTOSAVE_DIR")
                            : !Profiles::instance().folder(QStringLiteral("Autosave")).isEmpty() ? Profiles::instance().folder(QStringLiteral("Autosave"))
                            : brand::storageLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/autosave");
    QDir().mkpath(dir);
    return dir;
}

void MainWindow::autosave()
{
    if (!m_project->isModified()) {
        removeAutosave(); // saved (or nothing done): nothing to protect
        return;
    }
    if (m_editCount == m_autosavedCount && QFileInfo::exists(m_autosavePath))
        return;
    QString error;
    if (m_project->saveBackup(m_autosavePath, &error)) {
        m_autosavedCount = m_editCount;
        statusBar()->showMessage(tr("Autosaved"), 2000);
    } else {
        statusBar()->showMessage(tr("Autosave failed: %1").arg(error), 6000);
    }
}

void MainWindow::removeAutosave()
{
    if (!m_autosavePath.isEmpty())
        QFile::remove(m_autosavePath);
    m_autosavedCount = m_editCount;
}

QStringList MainWindow::orphanedAutosaves() const
{
    QStringList out;
    const QDir dir(autosaveDir());
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.mvp")}, QDir::Files, QDir::Time)) { // newest first
        if (fi.absoluteFilePath() == QFileInfo(m_autosavePath).absoluteFilePath())
            continue;
        // A session that is still running holds its lock.
        QLockFile lock(dir.filePath(fi.completeBaseName() + QStringLiteral(".lock")));
        lock.setStaleLockTime(0);
        if (lock.tryLock(0))
            out << fi.absoluteFilePath(); // (unlocking removes the dead session's lock file)
    }
    // Locks of crashed sessions that had nothing to autosave.
    for (const QFileInfo& fi : dir.entryInfoList({QStringLiteral("*.lock")}, QDir::Files))
        if (!QFileInfo::exists(dir.filePath(fi.completeBaseName() + QStringLiteral(".mvp")))) {
            QLockFile lock(fi.absoluteFilePath());
            lock.setStaleLockTime(0);
            lock.tryLock(0); // succeeds (and cleans up) only if its session is gone
        }
    return out;
}

bool MainWindow::recoverFrom(const QString& path)
{
    stopPlayback(false);
    QString error;
    QStringList missing;
    if (!m_project->recover(path, &error, &missing)) {
        QMessageBox::critical(this, tr("Recover Work"), tr("Could not open the autosaved copy:\n%1").arg(error));
        return false;
    }
    QFile::remove(path);
    m_settingsMatched = true;
    m_timeline->setRegion(0, 0);
    m_timeline->setCursorTime(0);
    m_timeline->zoomToFit();
    refreshPreview();
    updateTitle();
    autosave(); // this session's own copy straight away
    statusBar()->showMessage(tr("Recovered. Save it to keep it (File > Save)."), 8000);
    if (!missing.isEmpty())
        relinkMissing();
    return true;
}

bool MainWindow::checkRecovery()
{
    for (const QString& path : orphanedAutosaves()) {
        QString original;
        {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly))
                original = QJsonDocument::fromJson(f.readAll()).object()[QStringLiteral("autosaveOf")].toString();
        }
        const QString name = original.isEmpty() ? tr("an untitled project") : QFileInfo(original).fileName();
        QMessageBox box(QMessageBox::Warning, tr("Recover Work"),
                        tr("Mutant Video Producer didn't close properly last time. An autosaved copy of %1 from %2 was found.\n\nRecover it?")
                            .arg(name, QLocale().toString(QFileInfo(path).lastModified(), QLocale::ShortFormat)),
                        QMessageBox::NoButton, this);
        QPushButton* recover = box.addButton(tr("Recover"), QMessageBox::AcceptRole);
        QPushButton* discard = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
        box.addButton(tr("Not Now"), QMessageBox::RejectRole);
        box.setDefaultButton(recover);
        box.exec();
        if (box.clickedButton() == discard)
            QFile::remove(path);
        else if (box.clickedButton() == recover && recoverFrom(path))
            return true;
    }
    return false;
}

void MainWindow::startup(bool projectGiven)
{
    QSettings settings;
    offerCrashReport();
    if (settings.value(QStringLiteral("updates/check"), true).toBool()
        && settings.value(QStringLiteral("updates/lastCheck")).toDate() != QDate::currentDate())
        QTimer::singleShot(3000, this, [this] { checkForUpdates(false); });
    if (!checkRecovery() && !projectGiven)
        reopenLast();
    // Nothing opened (the first start, or reopening is off): the welcome window.
    if (!projectGiven && m_project->filePath().isEmpty() && !m_project->isModified() && settings.value(QStringLiteral("startup/welcome"), true).toBool())
        QTimer::singleShot(0, this, &MainWindow::showWelcome);
}

void MainWindow::reopenLast()
{
    QSettings settings;
    const QString crashedOn = settings.value(Profiles::instance().settingsKey(QStringLiteral("startup/opening"))).toString();
    settings.remove(Profiles::instance().settingsKey(QStringLiteral("startup/opening")));
    if (!settings.value(QStringLiteral("startup/reopenLast"), true).toBool())
        return;
    const QStringList recent = settings.value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList();
    if (recent.isEmpty() || !project_format::hasExtension(recent.front()) || !QFileInfo::exists(recent.front()))
        return;
    if (!crashedOn.isEmpty()) {
        statusBar()->showMessage(tr("%1 was not reopened: Mutant Video Producer closed while opening it last time.").arg(QFileInfo(crashedOn).fileName()), 10000);
        return;
    }
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("startup/opening")), recent.front());
    settings.sync();
    openProject(recent.front());
    settings.remove(Profiles::instance().settingsKey(QStringLiteral("startup/opening")));
}

void MainWindow::addRecent(const QString& path)
{
    QSettings settings;
    QStringList recent = settings.value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList();
    const QString clean = QFileInfo(path).absoluteFilePath();
    recent.erase(std::remove_if(recent.begin(), recent.end(), [&](const QString& p) { return !project_format::hasExtension(p) || p.compare(clean, kPathCase) == 0; }),
                 recent.end());
    recent.prepend(clean);
    while (recent.size() > 10)
        recent.removeLast();
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("recent/projects")), recent);
}

void MainWindow::rebuildRecentMenu()
{
    m_recentMenu->clear();
    const QStringList recent = QSettings().value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList();
    for (int i = 0; i < int(recent.size()); ++i) {
        const QString path = recent[i];
        if (!project_format::hasExtension(path)) continue;
        const QFileInfo fi(path);
        const QString number = i < 9 ? QStringLiteral("&%1  ").arg(i + 1) : QStringLiteral("%1  ").arg(i + 1);
        QAction* a = m_recentMenu->addAction(number + fi.fileName() + QStringLiteral("   ") + QDir::toNativeSeparators(fi.absolutePath()),
                                             this, [this, path] {
                                                 if (!QFileInfo::exists(path)) {
                                                     QMessageBox::information(this, tr("Open Recent"),
                                                                              tr("%1 is no longer there.").arg(QDir::toNativeSeparators(path)));
                                                     QSettings settings;
                                                     QStringList list = settings.value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList();
                                                     list.removeAll(path);
                                                     settings.setValue(Profiles::instance().settingsKey(QStringLiteral("recent/projects")), list);
                                                     return;
                                                 }
                                                 if (maybeSave())
                                                     openProject(path);
                                             });
        a->setToolTip(QDir::toNativeSeparators(path));
        a->setEnabled(fi.exists());
    }
    if (recent.isEmpty())
        m_recentMenu->addAction(tr("(none yet)"))->setEnabled(false);
    m_recentMenu->addSeparator();
    m_recentMenu->addAction(tr("Clear List"), this, [] { QSettings().remove(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))); })->setEnabled(!recent.isEmpty());
}

void MainWindow::relinkMissing(int focusMediaId)
{
    stopPlayback(false);
    RelinkDialog dlg(m_project, focusMediaId, this);
    if (dlg.missingCount() == 0) {
        statusBar()->showMessage(tr("No missing media: every file the project uses can be opened."), 5000);
        return;
    }
    dlg.exec();
    refreshPreview();
}

void MainWindow::setRippleMode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    m_timeline->setRippleMode(TimelineWidget::RippleMode(mode));
    if (mode > 0)
        m_lastRipple = mode;
    if (mode < int(m_rippleActions.size()))
        m_rippleActions[mode]->setChecked(true);
    if (m_rippleButton) {
        const QString names[] = {tr("Ripple: Off"), tr("Ripple: Tracks"), tr("Ripple: All")};
        m_rippleButton->setText(names[mode]);
        m_rippleButton->setChecked(mode > 0);
    }
    QSettings settings;
    settings.setValue(QStringLiteral("edit/rippleMode"), mode);
    settings.setValue(QStringLiteral("edit/lastRipple"), m_lastRipple);
}

// ---------------------------------------------------------------------------
// Developer check (MutantVideoProducer --editor-test <log> <media>)
// ---------------------------------------------------------------------------

void MainWindow::setAiControl(bool on)
{
    if (!on) {
        m_mcp->stop();
        m_mcpLabel->setVisible(false);
        return;
    }
    const quint16 port = quint16(QSettings().value(QStringLiteral("mcp/port"), McpServer::kDefaultPort).toInt());
    QString error;
    if (!m_mcp->start(port, &error)) {
        statusBar()->showMessage(tr("AI Control couldn't start on port %1: %2").arg(port).arg(error), 8000);
        m_mcpLabel->setVisible(false);
        return;
    }
    m_mcpLabel->setText(tr("AI control on"));
    m_mcpLabel->setToolTip(tr("An MCP client can connect at http://127.0.0.1:%1/mcp (Options > AI Control Setup...)").arg(port));
    m_mcpLabel->setVisible(true);
}

void MainWindow::showAiControlSetup()
{
    QDialog dlg(this);
    dlg.setWindowTitle(tr("AI Control (MCP)"));
    dlg.setMinimumWidth(640);
    auto* layout = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(tr("With Options > Allow AI Control ticked, an AI assistant on this computer can look at and edit the\n"
                                "project (events, effects, markers, beats, renders) and play the Live tab, through MCP (the\n"
                                "Model Context Protocol). Only programs on this computer can connect; every edit can be undone."));
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    auto* port = new QSpinBox;
    port->setRange(1024, 65535);
    port->setValue(QSettings().value(QStringLiteral("mcp/port"), McpServer::kDefaultPort).toInt());
    form->addRow(tr("Port"), port);
    layout->addLayout(form);
    const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    auto snippet = [&](const QString& title, const QString& text) {
        layout->addWidget(new QLabel(title));
        auto* edit = new QPlainTextEdit(text);
        edit->setReadOnly(true);
        edit->setFixedHeight(text.count(QLatin1Char('\n')) * 18 + 34);
        layout->addWidget(edit);
        auto* copy = new QPushButton(tr("Copy"));
        connect(copy, &QPushButton::clicked, &dlg, [text] { QGuiApplication::clipboard()->setText(text); });
        auto* row = new QHBoxLayout;
        row->addStretch(1);
        row->addWidget(copy);
        layout->addLayout(row);
    };
    const int p = port->value();
    snippet(tr("Claude Code (run once in a terminal):"),
            QStringLiteral("claude mcp add --transport http mvp http://127.0.0.1:%1/mcp").arg(p));
    QString jsonExe = exe;
    jsonExe.replace(QLatin1String("\\"), QLatin1String("\\\\"));
    snippet(tr("Claude Desktop and other MCP clients that start a program (claude_desktop_config.json):"),
            QStringLiteral("{\n  \"mcpServers\": {\n    \"mvp\": {\n      \"command\": \"%1\",\n      \"args\": [\"--mcp-stdio\"]\n    }\n  }\n}")
                .arg(jsonExe));
    auto* note = new QLabel(tr("Mutant Video Producer has to be open with AI Control on for the tools to work."));
    theme::style(note, QStringLiteral("color: @dim;"));
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    help::addButton(buttons, QStringLiteral("ai-control.md"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    dlg.exec();
    if (port->value() != QSettings().value(QStringLiteral("mcp/port"), McpServer::kDefaultPort).toInt()) {
        QSettings().setValue(QStringLiteral("mcp/port"), port->value());
        if (m_mcp->isRunning())
            setAiControl(true); // on the new port
    }
}

void MainWindow::showMixer()
{
    if (!m_mixer) {
        m_mixer = new MixerWindow(m_project, this);
        help::setPage(m_mixer, QStringLiteral("audio.md#the-audio-mixer"));
        connect(m_mixer, &MixerWindow::trackEffectsRequested, this, [this](int id) { m_devices->setTarget({FxTarget::Kind::Track, id}); });
        connect(m_mixer, &MixerWindow::masterEffectsRequested, this, [this] { m_devices->setTarget({FxTarget::Kind::AudioMaster, 0}); });
    }
    m_mixer->show();
    m_mixer->raise();
    m_mixer->activateWindow();
}

void MainWindow::exportFrame(bool toClipboard)
{
    stopPlayback(false);
    const TimeUs t = m_timeline->cursorTime();
    const ProjectSettings& ps = m_project->settings();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Compositor renderer(true); // full size, best quality
    const QImage frame = renderer.render(*m_project->snapshot(), t, QSize(ps.width, ps.height)).convertToFormat(QImage::Format_RGB32);
    QApplication::restoreOverrideCursor();
    if (toClipboard) {
        QApplication::clipboard()->setImage(frame);
        statusBar()->showMessage(tr("Frame at %1 copied (%2x%3).").arg(formatTimecode(t, ps.fps)).arg(ps.width).arg(ps.height), 4000);
        return;
    }
    QSettings settings;
    QString dir = settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/stills"))).toString();
    if (dir.isEmpty())
        dir = Profiles::instance().folder(QStringLiteral("Media"));
    if (dir.isEmpty()) dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString base = (m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName())
                         + QStringLiteral(" ") + formatTimecode(t, ps.fps).replace(QLatin1Char(':'), QLatin1Char('-')).replace(QLatin1Char(';'), QLatin1Char('-'));
    const QString path = QFileDialog::getSaveFileName(this, tr("Export Frame"), QDir(dir).filePath(base + QStringLiteral(".png")),
                                                      tr("PNG image (*.png);;JPEG image (*.jpg);;WebP image (*.webp);;BMP image (*.bmp)"));
    if (path.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/stills")), QFileInfo(path).absolutePath());
    if (!frame.save(path, nullptr, 95))
        QMessageBox::critical(this, tr("Export Frame"), tr("Could not write %1.").arg(QDir::toNativeSeparators(path)));
    else {
        Profiles::instance().remember({QJsonObject{{"path", path}, {"folder", "Media"}, {"origin", "image"}}});
        statusBar()->showMessage(tr("Saved %1").arg(QDir::toNativeSeparators(path)), 4000);
    }
}
