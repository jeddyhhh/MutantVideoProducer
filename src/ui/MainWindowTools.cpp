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
#include "core/Spectrogram.h"
#include "ui/MakingOf.h"
#include <QDataStream>
#include <QDesktopServices>
#include <QFileDialog>
#include <QImageReader>
#include <QInputDialog>
#include <QProgressDialog>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include "ui/NodeEffectEditor.h"
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
#include "ui/StemJob.h"
#include "core/StemSeparation.h"
#include <QRadioButton>
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
#include <functional>
#include <QDir>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QOffscreenSurface>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QDialog>
#include <QMouseEvent>
#include <QPainter>
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

namespace {
// An effect instance anywhere in the timeline (events, tracks, the output, the Live tab, inside racks), by its id.
EffectInstance* findInstance(Timeline& tl, int id)
{
    std::function<EffectInstance*(EffectChain&)> in = [&](EffectChain& chain) -> EffectInstance* {
        for (EffectInstance& e : chain) {
            if (e.id == id)
                return &e;
            for (RackChain& c : e.chains)
                if (EffectInstance* found = in(c.effects))
                    return found;
        }
        return nullptr;
    };
    for (Track& t : tl.tracks) {
        if (EffectInstance* found = in(t.effects))
            return found;
        for (Clip& c : t.clips)
            if (EffectInstance* found = in(c.effects))
                return found;
    }
    for (LivePad& pad : tl.livePads)
        if (EffectInstance* found = in(pad.effects))
            return found;
    for (EffectChain* chain : {&tl.outputEffects, &tl.liveEffects})
        if (EffectInstance* found = in(*chain))
            return found;
    for (EffectChain& chain : tl.liveLayerEffects)
        if (EffectInstance* found = in(chain))
            return found;
    return nullptr;
}
} // namespace

void MainWindow::editUserEffect(int effectId)
{
    Timeline tl = m_project->timeline();
    const EffectInstance* e = findInstance(tl, effectId);
    if (!e)
        return;
    // This one's own graph (as the event shows it), in the file it came from when there is one.
    const auto carried = e->params.find(QStringLiteral("graph"));
    nodes::Graph graph;
    if (carried != e->params.end() && !carried->second.text.isEmpty())
        graph = nodes::fromJson(QJsonDocument::fromJson(carried->second.text.toUtf8()).object());
    else if (const EffectDef* def = findEffect(e->type))
        for (const ParamDef& p : def->params)
            if (p.id == QLatin1String("graph"))
                graph = nodes::fromJson(QJsonDocument::fromJson(p.defText.toUtf8()).object());
    QString file;
    for (const QFileInfo& fi : QDir(nodes::userEffectsFolder()).entryInfoList({QStringLiteral("*.mvpfx")}, QDir::Files)) {
        nodes::Graph g;
        if (nodes::load(fi.absoluteFilePath(), &g, nullptr) && nodes::effectId(g.name) == e->type) {
            file = fi.absoluteFilePath();
            break;
        }
    }
    NodeEffectEditor* editor = effectBuilder(file);
    if (!graph.nodes.empty())
        editor->setGraph(graph);
    // Closed with changes it didn't save as an effect: they stay in this effect (in the project, undoable), its settings
    // changed in the graph with them.
    connect(editor, &NodeEffectEditor::closedWithChanges, this,
            [this, effectId, original = editor->graph(), session = std::weak_ptr<const int>(m_project->session())](const nodes::Graph& g) {
                if (session.lock() != m_project->session())
                    return; // (another project opened meanwhile)
                Timeline now = m_project->timeline();
                EffectInstance* inst = findInstance(now, effectId);
                if (!inst)
                    return;
                inst->params[QStringLiteral("graph")].text = QString::fromUtf8(QJsonDocument(nodes::toJson(g)).toJson(QJsonDocument::Compact));
                for (const nodes::Exposed& x : g.exposed) {
                    const nodes::Node* n = g.find(x.node);
                    const nodes::Node* was = original.find(x.node);
                    if (!n || !n->params.count(x.param))
                        continue;
                    const double value = n->params.at(x.param).value;
                    if (!was || !was->params.count(x.param) || was->params.at(x.param).value != value) {
                        ParamValue& p = inst->params[x.id];
                        p.keys.clear();
                        p.value = value;
                    }
                }
                m_project->applyEdit(now, tr("Edit Effect in the Effect Builder"));
                refreshPreview();
            });
    connect(editor, &NodeEffectEditor::saved, this, [this, effectId](const QString& id) {
        Timeline now = m_project->timeline();
        EffectInstance* inst = findInstance(now, effectId);
        const EffectDef* def = findEffect(id);
        if (!inst || !def)
            return;
        // The new graph, and the settings it shows now (as they were set, for new ones).
        inst->type = id;
        for (const ParamDef& p : def->params)
            if (p.id == QLatin1String("graph"))
                inst->params[p.id].text = p.defText;
            else if (!inst->params.count(p.id))
                inst->params[p.id].value = p.def;
        m_project->applyEdit(now, tr("Edit User Effect"));
        refreshPreview();
    });
}

NodeEffectEditor* MainWindow::effectBuilder(const QString& file)
{
    stopPlayback(false);
    // The project's videos to try it on, starting with the selected event's (else the first).
    std::vector<std::pair<QString, QString>> videos;
    for (const auto& [id, m] : m_project->media())
        if (m && m->info.ok() && !m->info.video.empty() && !m->info.isStill)
            videos.emplace_back(QFileInfo(m->path).fileName(), m->path);
    QString example = videos.empty() ? QString() : videos.front().second;
    for (int id : m_timeline->selection())
        if (const Clip* c = m_project->timeline().findClip(id); c && !c->isGenerated())
            if (auto m = m_project->mediaItem(c->mediaId); m && !m->info.video.empty() && !m->info.isStill) {
                example = m->path;
                break;
            }
    auto* editor = new NodeEffectEditor(videos, example, m_project->settings().width, this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    connect(editor, &NodeEffectEditor::saved, this, [this](const QString& id) {
        const EffectDef* def = findEffect(id);
        statusBar()->showMessage(tr("Saved \"%1\": it's in the Effects tab under Node Editor").arg(def ? def->name : id), 8000);
    });
    if (m_modes && m_modes->currentWidget() == m_live) // on the Live tab: tried on what is live (before its effects)
        editor->setLiveSource([engine = QPointer<LiveEngine>(m_live->engine())] { return engine ? engine->cleanFrame() : QImage(); });
    if (!file.isEmpty())
        editor->openFile(file);
    editor->show();
    editor->raise();
    return editor;
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

namespace {
// The spectrogram, with its frequencies up the left and its time along the bottom; a click reports where (0..1 across).
class SpectrogramView : public QWidget {
public:
    std::function<void(double)> clicked;
    void setPicture(const QImage& img, int rate, double seconds)
    {
        m_img = img;
        m_rate = rate;
        m_seconds = seconds;
        update();
    }
    void setCursor01(double at)
    {
        m_cursor = at;
        update();
    }
    QRect plot() const { return rect().adjusted(46, 6, -6, -20); }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(16, 12, 24));
        const QRect r = plot();
        if (m_img.isNull()) {
            p.setPen(Qt::gray);
            p.drawText(r, Qt::AlignCenter, QObject::tr("Listening..."));
            return;
        }
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(r, m_img);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.8);
        p.setFont(f);
        for (const double hz : {50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0}) {
            if (hz > m_rate / 2.0)
                continue;
            const double y = r.top() + spectrogram::rowOf(hz, m_rate, r.height());
            p.setPen(QColor(255, 255, 255, 50));
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(QColor(220, 220, 230));
            p.drawText(QRectF(0, y - 8, r.left() - 4, 16), Qt::AlignRight | Qt::AlignVCenter,
                       hz >= 1000 ? QStringLiteral("%1k").arg(hz / 1000) : QString::number(int(hz)));
        }
        const double step = m_seconds > 120 ? 30 : m_seconds > 30 ? 10 : m_seconds > 8 ? 2 : 0.5;
        for (double s = 0; s <= m_seconds; s += step) {
            const double x = r.left() + s / std::max(1e-6, m_seconds) * r.width();
            if (x + 34 > width()) // (no room for its label)
                break;
            p.setPen(QColor(220, 220, 230));
            p.drawText(QPointF(x + 2, height() - 5), QStringLiteral("%1 s").arg(s, 0, 'f', step < 1 ? 1 : 0));
        }
        if (m_cursor >= 0 && m_cursor <= 1) {
            p.setPen(QPen(QColor(255, 255, 255, 200), 1));
            const double x = r.left() + m_cursor * r.width();
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const QRect r = plot();
        if (clicked && r.width() > 0)
            clicked(std::clamp((e->position().x() - r.left()) / r.width(), 0.0, 1.0));
    }

private:
    QImage m_img;
    int m_rate = 48000;
    double m_seconds = 0;
    double m_cursor = -1;
};
} // namespace

void MainWindow::showSpectrogram(int clipId)
{
    const Clip* c = m_project->timeline().findClip(clipId);
    if (!c)
        return;
    stopPlayback(false);
    auto* dlg = new QDialog(this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setObjectName(QStringLiteral("spectrogramWindow"));
    dlg->setWindowTitle(tr("Spectrogram: %1").arg(c->name.isEmpty() ? tr("audio event") : c->name));
    dlg->resize(1000, 520);
    auto* v = new QVBoxLayout(dlg);
    auto* view = new SpectrogramView;
    view->setMinimumSize(400, 240);
    v->addWidget(view, 1);
    auto* row = new QHBoxLayout;
    auto* before = new QCheckBox(tr("Before the event's effects"));
    before->setToolTip(tr("Off: as it is heard (its effects and level). On: the sound as it was, to compare"));
    auto* hint = new QLabel(tr("Hum: flat lines (50 or 60 Hz and above): De-hum. Clicks: thin upright lines: De-click. Hiss: a haze "
                               "across the top: Noise Reduction. Click to move the cursor there."));
    hint->setWordWrap(true);
    theme::style(hint, QStringLiteral("color: @dim;"));
    row->addWidget(before);
    row->addWidget(hint, 1);
    v->addLayout(row);
    constexpr int kRate = 48000;
    const TimeUs start = c->start;
    const double seconds = std::min(double(c->length) / kUsPerSecond, 600.0);
    auto draw = [this, view, before, clipId, seconds] {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const std::vector<float> sound = spectrogram::eventSound(*m_project->snapshot(), clipId, before->isChecked(), kRate);
        const QSize size(std::clamp(int(seconds * 40), 400, 4000), 384);
        view->setPicture(spectrogram::render(sound, kRate, size), kRate, seconds);
        QApplication::restoreOverrideCursor();
    };
    connect(before, &QCheckBox::toggled, dlg, draw);
    view->clicked = [this, view, start, seconds](double at) {
        seekTo(start + TimeUs(at * seconds * kUsPerSecond));
        view->setCursor01(at);
    };
    // The cursor line follows the timeline's cursor, and the playhead while playing.
    auto follow = [view, start, seconds](TimeUs t) { view->setCursor01(double(t - start) / (seconds * kUsPerSecond)); };
    follow(m_timeline->cursorTime());
    connect(m_timeline, &TimelineWidget::cursorMoved, view, follow);
    connect(m_timeline, &TimelineWidget::cursorSet, view, follow);
    connect(m_engine, &PlaybackEngine::positionChanged, view, follow);
    dlg->show();
    QTimer::singleShot(0, dlg, draw);
}

void MainWindow::soundFromPicture()
{
    stopPlayback(false);
    QStringList filters;
    for (const QByteArray& f : QImageReader::supportedImageFormats())
        filters << QStringLiteral("*.") + QString::fromLatin1(f);
    const QString file = QFileDialog::getOpenFileName(this, tr("Sound from a Picture"), QString(),
                                                      tr("Pictures (%1)").arg(filters.join(QLatin1Char(' '))));
    if (file.isEmpty())
        return;
    const QImage picture(file);
    if (picture.isNull()) {
        QMessageBox::warning(this, tr("Sound from a Picture"), tr("%1 could not be read as a picture.").arg(QDir::toNativeSeparators(file)));
        return;
    }
    bool ok = false;
    const double suggested = std::clamp(4.0 * picture.width() / std::max(1, picture.height()), 2.0, 30.0);
    const double seconds = QInputDialog::getDouble(this, tr("Sound from a Picture"),
                                                   tr("How long, in seconds (the picture is stretched across it; it shows "
                                                      "in the event's spectrogram, bright parts loud):"),
                                                   std::round(suggested * 10) / 10, 0.5, 120, 1, &ok);
    if (!ok)
        return;
    constexpr int kRate = 48000;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const std::vector<float> mono = spectrogram::fromPicture(picture, kRate, seconds);
    QApplication::restoreOverrideCursor();
    const QString project = m_project->filePath();
    QString folder = !project.isEmpty() ? QFileInfo(project).dir().filePath(QFileInfo(project).completeBaseName() + QStringLiteral("-sounds"))
                                        : brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Sounds"));
    QDir().mkpath(folder);
    const QString base = QDir(folder).filePath(QFileInfo(file).completeBaseName() + QStringLiteral(" (painted)"));
    QString path = base + QStringLiteral(".wav");
    for (int n = 2; QFile::exists(path); ++n)
        path = QStringLiteral("%1 %2.wav").arg(base).arg(n);
    QSaveFile out(path);
    if (out.open(QIODevice::WriteOnly)) { // 32-bit float stereo WAV
        QDataStream st(&out);
        st.setByteOrder(QDataStream::LittleEndian);
        st.setFloatingPointPrecision(QDataStream::SinglePrecision);
        const quint32 bytes = quint32(mono.size() * 8);
        st.writeRawData("RIFF", 4);
        st << quint32(36 + bytes);
        st.writeRawData("WAVEfmt ", 8);
        st << quint32(16) << quint16(3) << quint16(2) << quint32(kRate) << quint32(kRate * 8) << quint16(8) << quint16(32);
        st.writeRawData("data", 4);
        st << bytes;
        for (float v : mono)
            st << v << v;
    }
    if (!out.commit()) {
        QMessageBox::warning(this, tr("Sound from a Picture"), tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), out.errorString()));
        return;
    }
    const TimeUs at = m_timeline->cursorTime();
    importFiles({path}, at);
    // Its spectrogram, to see the picture come back.
    for (const auto& [id, m] : m_project->media())
        if (m && QFileInfo(m->path) == QFileInfo(path))
            for (const Track& t : m_project->timeline().tracks)
                for (const Clip& c : t.clips)
                    if (c.mediaId == id && t.type == TrackType::Audio) { // (a new file: its only event)
                        showSpectrogram(c.id);
                        return;
                    }
}

void MainWindow::makingOfReplay()
{
    stopPlayback(false);
    if (m_project->historySteps() == 0) {
        QMessageBox::information(this, tr("Making-of Replay"),
                                 tr("There is nothing to replay yet. The replay plays back this session's undo history: "
                                    "make some edits first (the history starts when the project is opened)."));
        return;
    }
    const QString project = m_project->filePath();
    const QString title = project.isEmpty() ? tr("Untitled") : QFileInfo(project).completeBaseName();
    const QString folder = project.isEmpty() ? liveRecordingsFolder() : QFileInfo(project).absolutePath();
    const QString path = QFileDialog::getSaveFileName(this, tr("Making-of Replay"), QDir(folder).filePath(tr("%1 - making of.mp4").arg(title)),
                                                      tr("MP4 video (*.mp4)"));
    if (path.isEmpty())
        return;
    QProgressDialog progress(tr("Replaying %n edit(s)...", nullptr, m_project->historySteps()), tr("Stop"), 0, 1000, this);
    progress.setWindowTitle(tr("Making-of Replay"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    QString error;
    const bool ok = makingof::render(*m_project, path, [&](double done) {
        progress.setValue(int(done * 1000));
        QApplication::processEvents();
        return !progress.wasCanceled();
    }, &error);
    progress.close();
    if (!ok) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Making-of Replay"), error);
        return;
    }
    if (QMessageBox::question(this, tr("Making-of Replay"), tr("Made %1. Play it now?").arg(QDir::toNativeSeparators(path)))
        == QMessageBox::Yes)
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::autoColour()
{
    stopPlayback(false);
    Timeline tl = m_project->timeline();
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const int n = colormatch::autoColourEvents(tl, *m_project->snapshot(), m_timeline->selection(), m_timeline->cursorTime(), &error);
    QApplication::restoreOverrideCursor();
    if (!n) {
        statusBar()->showMessage(error, 5000);
        return;
    }
    m_project->applyEdit(tl, tr("Auto Colour"));
    statusBar()->showMessage(tr("Auto Colour on %n event(s): each has a White Balance effect (its Amount tones it down; pick the "
                                "neutral on the preview to set it by hand)", nullptr, n),
                             8000);
}

void MainWindow::splitAtSceneChanges()
{
    Timeline tl = m_project->timeline();
    const auto before = m_project->snapshot();
    const MediaMap& media = before->media;
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
    if (m_project->timeline() != before->timeline) // (a job finished meanwhile: its edit would be undone)
        return void(QMessageBox::information(this, tr("Split at Scene Changes"), tr("The project changed while it looked: try again.")));
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
    const auto before = m_project->snapshot();
    const MediaMap& media = before->media;
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
    if (m_project->timeline() != before->timeline) // (a job finished meanwhile: its edit would be undone)
        return void(QMessageBox::information(this, tr("Auto Reframe"), tr("The project changed while it looked: try again.")));
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
    const QString path = media->path; // (copies: `clip` points into the timeline, which can change while the dialog is open)
    const int stream = clip->streamIndex;
    if (dlg.exec() != QDialog::Accepted)
        return;
    const double amount = strength->currentData().toDouble();
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

StemJob* MainWindow::stemJob()
{
    if (!m_stemJob)
        m_stemJob = new StemJob(m_project, this);
    return m_stemJob;
}

void MainWindow::separateStems(int clipId)
{
    if (stems::workerPath().isEmpty()) {
        if (QMessageBox::question(this, tr("Separate Stems"),
                                  tr("Separating stems needs the Stem Separation engine (about 1 GB, or 3 GB for NVIDIA graphics cards). "
                                     "Install it now?"))
            == QMessageBox::Yes)
            aiComponents();
        return;
    }
    QDialog dlg(this);
    dlg.setObjectName(QStringLiteral("separateStemsDialog"));
    dlg.setWindowTitle(tr("Separate Stems"));
    auto* layout = new QVBoxLayout(&dlg);
    auto* two = new QRadioButton(tr("Voice and background (dialogue from music and noise)"));
    auto* four = new QRadioButton(tr("Vocals, drums, bass and other (a song's parts)"));
    two->setChecked(QSettings().value(QStringLiteral("stems/count"), 2).toInt() != 4);
    four->setChecked(!two->isChecked());
    auto* best = new QCheckBox(tr("Best quality (about four times slower)"));
    best->setChecked(QSettings().value(QStringLiteral("stems/best"), false).toBool());
    auto* note = new QLabel(tr("Each part goes on its own track under the event, which is muted (kept, to compare). The first "
                               "separation downloads the model (about 80 MB)."));
    note->setWordWrap(true);
    theme::style(note, QStringLiteral("color: @dim;"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    for (QWidget* w : std::initializer_list<QWidget*>{two, four, best, note, buttons})
        layout->addWidget(w);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const int count = four->isChecked() ? 4 : 2;
    QSettings().setValue(QStringLiteral("stems/count"), count);
    QSettings().setValue(QStringLiteral("stems/best"), best->isChecked());
    QString error;
    if (!stemJob()->start(clipId, count, best->isChecked(), &error)) {
        QMessageBox::warning(this, tr("Separate Stems"), error);
        return;
    }
    auto* progress = new QProgressDialog(tr("Separating stems..."), tr("Cancel"), 0, 1000, this);
    progress->setWindowTitle(tr("Separate Stems"));
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, stemJob(), &StemJob::cancel);
    connect(stemJob(), &StemJob::progress, progress, [progress](double value, const QString& message) {
        progress->setValue(int(value * 1000));
        progress->setLabelText(message);
    });
    connect(stemJob(), &StemJob::completed, progress, [this, progress](bool ok, const QString& why) {
        progress->close();
        if (ok)
            statusBar()->showMessage(tr("Stems separated: each on its own track under the event, which is muted."), 8000);
        else if (why != tr("Cancelled"))
            QMessageBox::warning(this, tr("Separate Stems"), why);
    });
    progress->show();
}
