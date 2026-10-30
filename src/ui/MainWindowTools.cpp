// MainWindow: Tools menu work: beats, beat montage, stabilising, proxies.
#include "core/Brand.h"
#include "core/ProjectFormat.h"
#include "core/Profiles.h"
#include "core/Voice.h"
#include "core/LiveRecorder.h"
#include <QUuid>
#include "ui/MainWindow.h"
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/FFmpeg.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/FormatRegistry.h"
#include "core/Project.h"
#include "core/SceneDetect.h"
#include "core/ColorMatch.h"
#include "core/Reframe.h"
#include "core/PanCrop.h"
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
#include <QInputDialog>
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

void MainWindow::detectBeats()
{
    stopPlayback(false);
    const Timeline& tl = m_project->timeline();
    // Where: the time selection, else the selected events, else everything. What:
    // the selected audio events only, if any are selected, else the whole mix.
    TimeUs from = 0, to = tl.duration();
    std::set<int> audio;
    TimeUs selFrom = std::numeric_limits<TimeUs>::max(), selTo = 0;
    for (const Track& t : tl.tracks)
        for (const Clip& c : t.clips)
            if (m_timeline->selection().count(c.id)) {
                selFrom = std::min(selFrom, c.start);
                selTo = std::max(selTo, c.end());
                if (t.type == TrackType::Audio)
                    audio.insert(c.id);
            }
    QString where = tr("the whole timeline");
    if (m_timeline->hasRegion()) {
        from = m_timeline->regionStart();
        to = m_timeline->regionEnd();
        where = tr("the time selection");
    } else if (selTo > selFrom) {
        from = selFrom;
        to = selTo;
        where = tr("the selected events");
    }
    if (to - from < 2 * kUsPerSecond) {
        QMessageBox::information(this, tr("Detect Beats"), tr("There needs to be at least a few seconds of music to find its beats."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Detect Beats"));
    auto* form = new QFormLayout(&dlg);
    auto* every = new QComboBox;
    every->addItem(tr("Every beat"), 1);
    every->addItem(tr("Every bar (4 beats)"), 4);
    every->addItem(tr("Every 2 bars"), 8);
    every->addItem(tr("Every 4 bars"), 16);
    form->addRow(tr("Markers on"), every);
    auto* listen = new QComboBox;
    listen->addItems({tr("Everything"), tr("The kick drum")});
    form->addRow(tr("Listen to"), listen);
    auto* replace = new QCheckBox(tr("Replace the markers already there"));
    replace->setChecked(true);
    form->addRow(QString(), replace);
    auto* note = new QLabel(tr("Looks at %1 (%2), listening to %3. Bars are numbered, the first beat counting as the 1; "
                               "events snap to the markers, and Tools > Split at Markers cuts on them.")
                                .arg(where)
                                .arg(formatTimecode(to - from, m_project->settings().fps))
                                .arg(audio.empty() ? tr("the whole mix") : tr("the selected audio events")));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("audio.md#detect-beats"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    QSettings settings;
    every->setCurrentIndex(std::clamp(settings.value(QStringLiteral("beats/every"), 0).toInt(), 0, 3));
    listen->setCurrentIndex(std::clamp(settings.value(QStringLiteral("beats/listen"), 0).toInt(), 0, 1));
    if (dlg.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("beats/every"), every->currentIndex());
    settings.setValue(QStringLiteral("beats/listen"), listen->currentIndex());
    QProgressDialog progress(tr("Listening for the beat..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    BeatSettings bs;
    bs.band = listen->currentIndex() == 1 ? BeatSettings::Bass : BeatSettings::Full;
    bs.minBpm = 70;
    bs.maxBpm = 180;
    const BeatScan scan = scanBeats(*m_project->snapshot(), from, to, audio, bs, [&](double p) {
        progress.setValue(int(p * 1000));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.close();
    if (scan.beats.empty()) {
        statusBar()->showMessage(tr("No steady beat found there"), 5000);
        return;
    }
    Timeline edited = m_project->timeline();
    const int n = addBeatMarkers(edited, scan.beats, every->currentData().toInt(), replace->isChecked(), from, to);
    m_project->applyEdit(edited, tr("Beat Markers"));
    // The beat grid follows the music too (Options > Grid > Beats).
    m_timeline->setGridTempo(scan.bpm, scan.beats.front());
    QSettings().setValue(QStringLiteral("edit/gridBpm"), m_timeline->gridBpm());
    QSettings().setValue(QStringLiteral("edit/gridOffset"), double(m_timeline->gridOffset()));
    statusBar()->showMessage(tr("%1 beat markers at %2 BPM (%3 beats found)").arg(n).arg(scan.bpm, 0, 'f', 2).arg(scan.beats.size()), 8000);
}

void MainWindow::splitAtMarkers()
{
    if (m_timeline->selection().empty()) {
        statusBar()->showMessage(tr("Select the events to split first"), 4000);
        return;
    }
    Timeline tl = m_project->timeline();
    const std::vector<int> pieces = ops::splitAtMarkers(tl, ops::expandToGroups(tl, m_timeline->selection()));
    if (pieces.empty()) {
        statusBar()->showMessage(tr("No markers inside the selected events"), 4000);
        return;
    }
    m_project->applyEdit(tl, tr("Split at Markers"));
    statusBar()->showMessage(tr("Split into %1 more pieces").arg(pieces.size()), 4000);
}

void MainWindow::matchColour()
{
    stopPlayback(false);
    Timeline tl = m_project->timeline();
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int n = colormatch::match(tl, *m_project->snapshot(), m_timeline->selection(), m_timeline->cursorTime(), &error);
    QApplication::restoreOverrideCursor();
    if (!n) {
        QMessageBox::information(this, tr("Match Colour"),
                                 error + QStringLiteral("\n\n")
                                     + tr("Select the events to fix, put the cursor on the shot whose colours they should have, then "
                                          "Tools > Match Colour."));
        return;
    }
    m_project->applyEdit(tl, tr("Match Colour"));
    statusBar()->showMessage(tr("Matched %n event(s) to the frame at %1: each has a Color Match effect (its Amount tones it down)", nullptr, n)
                                 .arg(formatTimecode(m_timeline->cursorTime(), m_project->settings().fps)),
                             8000);
}

void MainWindow::splitAtSceneChanges()
{
    Timeline tl = m_project->timeline();
    const MediaMap media = m_project->snapshot()->media;
    int skipped = 0;
    const std::vector<int> events = scenes::videoEvents(tl, media, m_timeline->selection(), &skipped);
    if (events.empty()) {
        statusBar()->showMessage(skipped ? tr("Scene changes: reversed or speed-ramped events aren't split") : tr("Select video events to split first"), 5000);
        return;
    }
    QSettings settings;
    bool ok = false;
    const int sensitivity = QInputDialog::getInt(this, tr("Split at Scene Changes"),
                                                 tr("Sensitivity (1: only clear cuts, 100: subtler changes too):"),
                                                 settings.value(QStringLiteral("scenes/sensitivity"), 50).toInt(), 1, 100, 1, &ok);
    if (!ok)
        return;
    settings.setValue(QStringLiteral("scenes/sensitivity"), sensitivity);
    stopPlayback(false);
    QProgressDialog progress(tr("Looking for scene changes..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    const auto cuts = scenes::splitEvents(tl, media, events, sensitivity / 100.0, m_project->settings().fps, [&](double f) {
        progress.setValue(int(1000 * f));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.setValue(1000);
    if (!cuts)
        return;
    if (cuts->empty()) {
        statusBar()->showMessage(tr("No scene changes found (try a higher sensitivity)"), 5000);
        return;
    }
    m_project->applyEdit(tl, tr("Split at Scene Changes"));
    statusBar()->showMessage(tr("Split at %n scene change(s)", nullptr, int(cuts->size())), 5000);
}

void MainWindow::autoReframe()
{
    Timeline tl = m_project->timeline();
    const MediaMap media = m_project->snapshot()->media;
    std::vector<int> events;
    for (int id : m_timeline->selection()) {
        const Clip* c = tl.findClip(id);
        const auto m = c ? media.find(c->mediaId) : media.end();
        if (c && m != media.end() && m->second->info.videoStream(c->streamIndex) && !m->second->info.isStill)
            events.push_back(id);
    }
    if (events.empty()) {
        statusBar()->showMessage(tr("Select video events to reframe first"), 5000);
        return;
    }
    ProjectSettings ps = m_project->settings();
    if (ps.width >= ps.height) {
        QMessageBox ask(QMessageBox::Question, tr("Auto Reframe"),
                        tr("The project is %1x%2. Auto Reframe crops wide video for a tall frame: switch the project to 1080x1920 (9:16) "
                           "for Shorts, Reels and TikTok?").arg(ps.width).arg(ps.height),
                        QMessageBox::Cancel, this);
        QPushButton* portrait = ask.addButton(tr("Switch to 1080x1920"), QMessageBox::AcceptRole);
        ask.exec();
        if (ask.clickedButton() != portrait)
            return;
        ps.width = 1080;
        ps.height = 1920;
        m_project->setSettings(ps);
    }
    stopPlayback(false);
    QProgressDialog progress(tr("Following the subject..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(300);
    const int framed = reframe::apply(tl, media, events, QSize(ps.width, ps.height), [&](double f) {
        progress.setValue(int(1000 * f));
        QCoreApplication::processEvents();
        return !progress.wasCanceled();
    });
    progress.setValue(1000);
    if (framed < 0)
        return;
    if (!framed) {
        statusBar()->showMessage(tr("Nothing to reframe: the frame already shows the whole picture"), 5000);
        return;
    }
    m_project->applyEdit(tl, tr("Auto Reframe"));
    statusBar()->showMessage(tr("Reframed %n event(s): see their Pan/Crop to adjust", nullptr, framed), 5000);
}

void MainWindow::beatMontage()
{
    stopPlayback(false);
    // The media: the files selected in Project Media, or every video file.
    std::vector<std::shared_ptr<const MediaItem>> media;
    for (int id : m_bin->selectedMediaIds())
        if (auto m = m_project->mediaItem(id); m && m->info.hasVideo())
            media.push_back(m);
    const bool selected = !media.empty();
    if (media.empty())
        for (const auto& [id, m] : m_project->media())
            if (m->info.hasVideo())
                media.push_back(m);
    // The cuts: the markers in the time selection (or everywhere).
    const Timeline& tl = m_project->timeline();
    const TimeUs from = m_timeline->hasRegion() ? m_timeline->regionStart() : 0;
    const TimeUs to = m_timeline->hasRegion() ? m_timeline->regionEnd() : std::numeric_limits<TimeUs>::max();
    std::vector<TimeUs> markers;
    for (const Marker& m : tl.markers)
        if (!m.isRegion() && m.time >= from && m.time <= to)
            markers.push_back(m.time);
    if (media.empty() || markers.size() < 2) {
        QMessageBox::information(this, tr("Beat Montage"),
                                 media.empty() ? tr("Import some video files first (and select the ones to use in Project Media).")
                                               : tr("Put markers on the beat first: Tools > Detect Beats (with the music on the timeline)."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Beat Montage"));
    auto* form = new QFormLayout(&dlg);
    auto* every = new QComboBox;
    for (int n : {1, 2, 4, 8, 16})
        every->addItem(n == 1 ? tr("Every marker") : tr("Every %1 markers").arg(n), n);
    every->setCurrentIndex(2);
    form->addRow(tr("Cut"), every);
    auto* order = new QComboBox;
    order->addItems({tr("In turn"), tr("Shuffled")});
    order->setCurrentIndex(1);
    form->addRow(tr("Clips"), order);
    auto* note = new QLabel(tr("Makes a new video track on top with a clip from %1 between the cuts, each from a random part of "
                               "its file; %2 markers in %3.")
                                .arg(selected ? tr("the %1 selected file(s)").arg(media.size()) : tr("all %1 video files").arg(media.size()))
                                .arg(markers.size())
                                .arg(m_timeline->hasRegion() ? tr("the time selection") : tr("the timeline")));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("audio.md#detect-beats"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    std::vector<TimeUs> cuts;
    const size_t step = size_t(every->currentData().toInt());
    for (size_t i = 0; i < markers.size(); i += step)
        cuts.push_back(markers[i]);
    if (m_timeline->hasRegion() && cuts.back() < to)
        cuts.push_back(to);
    Timeline edited = m_project->timeline();
    const std::vector<int> ids = ops::beatMontage(edited, media, cuts, order->currentIndex() == 1, uint32_t(QDateTime::currentMSecsSinceEpoch()));
    if (ids.empty())
        return;
    m_project->applyEdit(edited, tr("Beat Montage"));
    statusBar()->showMessage(tr("Beat montage: %1 clips on a new track").arg(ids.size()), 6000);
}

void MainWindow::stabilizeClip(int clipId)
{
    stopPlayback(false);
    const Clip* clip = m_project->timeline().findClip(clipId);
    const auto media = clip && !clip->isGenerated() ? m_project->mediaItem(clip->mediaId) : nullptr;
    if (!clip || !media || !media->info.videoStream(clip->streamIndex) || media->info.isStill) {
        statusBar()->showMessage(tr("Stabilize works on video events"), 4000);
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Stabilize"));
    auto* form = new QFormLayout(&dlg);
    auto* strength = new QComboBox;
    strength->addItem(tr("Off"), 0.0);
    strength->addItem(tr("Light (keeps intended moves)"), 0.5);
    strength->addItem(tr("Medium"), 1.5);
    strength->addItem(tr("Strong"), 3.0);
    strength->addItem(tr("Very strong (nearly a tripod)"), 8.0);
    int current = 0;
    for (int i = 0; i < strength->count(); ++i)
        if (std::abs(strength->itemData(i).toDouble() - clip->stabilize) < 1e-6)
            current = i;
    strength->setCurrentIndex(clip->stabilize > 0 && current == 0 ? 2 : current);
    form->addRow(tr("Smoothing"), strength);
    auto* zoom = new QCheckBox(tr("Zoom in to hide the moving edges"));
    zoom->setChecked(clip->stabilizeZoom);
    form->addRow(QString(), zoom);
    auto* note = new QLabel(stabilize::isAnalyzed(media->path, clip->streamIndex)
                                ? tr("The footage is already analysed.")
                                : tr("The footage is analysed once first (its motion is kept with the app's cache)."));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("effects.md#stabilize"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const double amount = strength->currentData().toDouble();
    const QString path = media->path;
    const int stream = clip->streamIndex;
    if (amount > 0 && !stabilize::isAnalyzed(path, stream)) {
        QProgressDialog progress(tr("Analysing the motion of %1...").arg(media->displayName()), tr("Cancel"), 0, 1000, this);
        progress.setWindowModality(Qt::WindowModal);
        progress.setMinimumDuration(0);
        std::atomic<double> done{0};
        std::atomic<bool> stop{false}, finished{false}, ok{false};
        QString error;
        std::thread worker([&] {
            ok = stabilize::analyze(path, stream, [&](double p) {
                done = p;
                return !stop.load();
            }, &error);
            finished = true;
        });
        while (!finished) {
            progress.setValue(int(done * 1000));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 30);
            if (progress.wasCanceled())
                stop = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        worker.join();
        progress.close();
        if (!ok) {
            if (!stop)
                QMessageBox::warning(this, tr("Stabilize"), tr("Could not analyse the footage: %1").arg(error));
            return;
        }
    }
    Timeline tl = m_project->timeline();
    for (int id : ops::expandToGroups(tl, {clipId}))
        if (Clip* c = tl.findClip(id); c && c->mediaId == media->id && c->streamIndex == stream) {
            c->stabilize = amount;
            c->stabilizeZoom = zoom->isChecked();
        }
    m_project->applyEdit(tl, amount > 0 ? tr("Stabilize") : tr("Stabilize Off"));
    refreshPreview();
}

void MainWindow::createProxies(const QList<int>& mediaIds, bool heavyOnly, bool replace)
{
    int queued = 0, skipped = 0;
    for (int id : mediaIds) {
        auto m = m_project->mediaItem(id);
        if (!m || !m->info.ok() || !m->info.hasVideo() || m->info.isStill)
            continue;
        if (heavyOnly && !proxy::recommended(m->info)) {
            ++skipped;
            continue;
        }
        const int stream = m->info.video.front().index;
        if (replace)
            proxy::remove(m->path, stream);
        else if (proxy::exists(m->path, stream))
            continue;
        m_proxies->add(id, m->path, stream, m->displayName());
        ++queued;
    }
    if (queued) {
        m_proxyStatus->setText(tr("Proxy: starting..."));
        m_proxyStatus->setVisible(true);
        m_proxyCancel->setVisible(true);
    } else if (!heavyOnly || skipped) {
        statusBar()->showMessage(heavyOnly ? tr("No heavy files without a proxy (4K, HEVC, AV1, ProRes, 10-bit)")
                                           : tr("Select video files in Project Media first"),
                                 5000);
    }
}

void MainWindow::deleteProxies(const QList<int>& mediaIds)
{
    int n = 0;
    for (int id : mediaIds)
        if (auto m = m_project->mediaItem(id); m && m->info.hasVideo())
            n += proxy::remove(m->path, m->info.video.front().index) ? 1 : 0;
    statusBar()->showMessage(tr("Deleted %1 proxies").arg(n), 4000);
    m_bin->refreshIcons();
    refreshPreview();
}
