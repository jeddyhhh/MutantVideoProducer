// Tools > Render Selection to New Track (render in place) and File > Export Stems.
#include "ui/MainWindow.h"
#include "ui/TimelineWidget.h"
#include "ui/RenderStatusDialog.h"
#include "core/Exporter.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/Profiles.h"
#include "core/TimelineOps.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QOffscreenSurface>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSettings>
#include <QStatusBar>
#include <QThread>
#include <atomic>
#include <thread>

namespace {
// What renders are made for: the project's own folder (or the profile's Exports before it is saved).
QString renderFolder(const Project& project, const QString& kind)
{
    const QString file = project.filePath();
    if (!file.isEmpty())
        return QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + QStringLiteral("-") + kind);
    const QString exports = Profiles::instance().folder(QStringLiteral("Exports"));
    return QDir(exports.isEmpty() ? QDir::homePath() : exports).filePath(kind);
}

QString clock(TimeUs t)
{
    const qint64 s = t / kUsPerSecond;
    return QStringLiteral("%1.%2").arg(s / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

void MainWindow::renderInPlace()
{
    stopPlayback(false);
    // The time selection, else the selected events' span.
    TimeUs from = 0, to = 0;
    if (m_timeline->hasRegion()) {
        from = m_timeline->regionStart();
        to = m_timeline->regionEnd();
    } else {
        bool any = false;
        for (int id : m_timeline->selection())
            if (const Clip* c = m_project->timeline().findClip(id)) {
                from = any ? std::min(from, c->start) : c->start;
                to = any ? std::max(to, c->end()) : c->end();
                any = true;
            }
    }
    if (to <= from) {
        QMessageBox::information(this, tr("Render Selection to New Track"),
                                 tr("Make a time selection (drag in the bar above the ruler) or select events first: that stretch is rendered."));
        return;
    }
    const ExportSettings s = inPlaceSettings(from, to);
    const auto snap = inPlaceSnapshot();
    std::unique_ptr<QOffscreenSurface> gpuSurface = hw::settings().gpuCompositing ? createGpuSurface() : nullptr;
    auto monitor = std::make_shared<RenderMonitor>();
    bool ok = false;
    QString error;
    std::thread worker([&] {
        Exporter exporter;
        ok = exporter.run(*snap, s, [&](double) { return !monitor->cancel.load(); }, gpuSurface.get(),
                          [&](const ExportStatus& value) { monitor->update(value); });
        error = exporter.error();
        monitor->done = true;
    });
    RenderStatusDialog status(s, *snap, monitor, this);
    status.setWindowModality(Qt::WindowModal);
    status.exec();
    monitor->cancel = true;
    worker.join();
    if (!ok) {
        QFile::remove(s.path);
        if (!error.isEmpty())
            statusBar()->showMessage(tr("Render in place stopped: %1").arg(error), 8000);
        return;
    }
    placeRendered(s.path, from, to);
}

ExportSettings MainWindow::inPlaceSettings(TimeUs from, TimeUs to) const
{
    const auto preset = std::find_if(exportPresets().begin(), exportPresets().end(),
                                     [](const ExportPreset& p) { return p.name == QLatin1String("MOV - ProRes 422 LT"); });
    ExportSettings s = settingsFromPreset(*preset, m_project->settings());
    s.audioEncoder.clear(); // (the picture only: the sound keeps playing live from the events)
    s.rangeStart = from;
    s.rangeEnd = to;
    const QString folder = renderFolder(*m_project, QStringLiteral("rendered"));
    QDir().mkpath(folder);
    const QString base = m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName();
    s.path = QDir(folder).filePath(QStringLiteral("%1 %2-%3.mov").arg(base, clock(from), clock(to)));
    for (int n = 2; QFileInfo::exists(s.path); ++n)
        s.path = QDir(folder).filePath(QStringLiteral("%1 %2-%3 (%4).mov").arg(base, clock(from), clock(to)).arg(n));
    return s;
}

std::shared_ptr<ProjectSnapshot> MainWindow::inPlaceSnapshot() const
{
    // The whole picture, as it looks, but without Output FX (they apply to the rendered track again).
    auto snap = std::make_shared<ProjectSnapshot>(*m_project->snapshot());
    snap->timeline.outputEffects.clear();
    return snap;
}

bool MainWindow::placeRendered(const QString& path, TimeUs from, TimeUs to)
{
    const MediaInfo info = probeMedia(path);
    if (!info.ok() || info.video.empty())
        return false;
    Timeline tl = m_project->timeline();
    const int mediaId = m_project->addMedia(info);
    // On a "Rendered" track on top (one with room, else a new one): it hides what it was made from, so playback
    // decodes only it there. Delete or mute it to see (and change) the events underneath.
    int ti = -1;
    for (int i = 0; i < int(tl.tracks.size()) && ti < 0; ++i) {
        const Track& t = tl.tracks[size_t(i)];
        if (t.type == TrackType::Video && t.name.startsWith(QStringLiteral("Rendered"))
            && std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.start < to && from < c.end(); }))
            ti = i;
    }
    if (ti < 0) {
        ti = ops::addTrack(tl, TrackType::Video, 0);
        tl.tracks[size_t(ti)].name = tr("Rendered");
    }
    Clip c;
    c.id = tl.allocId();
    c.mediaId = mediaId;
    c.streamIndex = info.video.front().index;
    c.start = from;
    c.length = to - from;
    c.name = tr("Rendered %1-%2").arg(clock(from), clock(to));
    Track& track = tl.tracks[size_t(ti)];
    track.clips.push_back(c);
    tl.sortClips();
    m_project->applyEdit(tl, tr("Render Selection to New Track"));
    statusBar()->showMessage(tr("Rendered %1 s onto the Rendered track: it plays from that file now. Mute or delete it to edit what's under it.")
                                 .arg(double(to - from) / kUsPerSecond, 0, 'f', 1),
                             10000);
    return true;
}

void MainWindow::exportStems()
{
    stopPlayback(false);
    const Timeline& tl = m_project->timeline();
    std::vector<const Track*> tracks;
    for (const Track& t : tl.tracks)
        if (t.type == TrackType::Audio && !t.muted && !t.clips.empty())
            tracks.push_back(&t);
    if (tracks.empty()) {
        QMessageBox::information(this, tr("Export Stems"), tr("There are no audio tracks with sound to export."));
        return;
    }
    QSettings settings;
    const QString start = settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/stems")), renderFolder(*m_project, QStringLiteral("stems"))).toString();
    QDir().mkpath(start);
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Export Stems to"), start);
    if (folder.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/stems")), folder);
    const TimeUs from = m_timeline->hasRegion() ? m_timeline->regionStart() : 0;
    const TimeUs to = m_timeline->hasRegion() ? m_timeline->regionEnd() : tl.duration();
    const auto snap = stemSnapshot();
    const std::vector<ExportSettings> jobs = stemSettings(folder, from, to);
    QProgressDialog progress(tr("Exporting stems..."), tr("Stop"), 0, int(jobs.size()) * 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    std::atomic<int> done{0}, percent{0};
    std::atomic<bool> cancel{false}, finished{false};
    QStringList failed;
    std::thread worker([&] {
        for (const ExportSettings& s : jobs) {
            Exporter exporter;
            const bool ok = exporter.run(*snap, s, [&](double p) {
                percent = int(p * 100);
                return !cancel.load();
            });
            if (!ok && !cancel)
                failed << QFileInfo(s.path).fileName() + QStringLiteral(": ") + exporter.error();
            if (cancel)
                break;
            ++done;
        }
        finished = true;
    });
    while (!finished) {
        progress.setValue(done * 100 + percent);
        progress.setLabelText(tr("Exporting stem %1 of %2...").arg(std::min(int(jobs.size()), done + 1)).arg(jobs.size()));
        if (progress.wasCanceled())
            cancel = true;
        QApplication::processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }
    worker.join();
    progress.close();
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Export Stems"), failed.join(QLatin1Char('\n')));
    else if (!cancel)
        statusBar()->showMessage(tr("Exported %n stem(s) to %1", nullptr, int(jobs.size())).arg(QDir::toNativeSeparators(folder)), 10000);
}

std::shared_ptr<ProjectSnapshot> MainWindow::stemSnapshot() const
{
    // Each stem: its track alone, after its own FX, level, pan and ducking, before Master FX and the master volume
    // (the stems add up to the mix as it goes into the master).
    auto snap = std::make_shared<ProjectSnapshot>(*m_project->snapshot());
    snap->timeline.audioMasterEffects.clear();
    snap->timeline.audioMasterLevel = 1.0f;
    return snap;
}

std::vector<ExportSettings> MainWindow::stemSettings(const QString& folder, TimeUs from, TimeUs to) const
{
    const QString base = m_project->filePath().isEmpty() ? tr("Untitled") : QFileInfo(m_project->filePath()).completeBaseName();
    static const QRegularExpression illegal(QStringLiteral("[\\/:*?\"<>|]"));
    std::vector<ExportSettings> jobs;
    for (const Track& t : m_project->timeline().tracks) {
        if (t.type != TrackType::Audio || t.muted || t.clips.empty())
            continue;
        ExportSettings s;
        s.muxer = QStringLiteral("wav");
        s.audioEncoder = QStringLiteral("pcm_s24le");
        s.sampleRate = 48000;
        s.channels = 2;
        s.rangeStart = from;
        s.rangeEnd = to;
        s.onlyAudioTrack = t.id;
        s.path = QDir(folder).filePath(QStringLiteral("%1 - %2.wav").arg(base, QString(t.name).replace(illegal, QStringLiteral(" ")).simplified()));
        jobs.push_back(s);
    }
    return jobs;
}
