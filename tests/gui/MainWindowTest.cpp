// MainWindow's scripted GUI test, kept out of the editor's own source.
#include "core/Brand.h"
#include "core/ProjectFormat.h"
#include "core/Profiles.h"
#include "core/Voice.h"
#include "core/LiveRecorder.h"
#include <QUuid>
#include "ui/MainWindow.h"
#include "ui/RenderQueue.h"
#include "ui/DeliverView.h"
#include <QLineEdit>
#include <QCryptographicHash>
#include <QPlainTextEdit>
#include <QTreeWidget>
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/FFmpeg.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/FormatRegistry.h"
#include "core/Project.h"
#include "engine/MediaCache.h"
#include "engine/PlaybackEngine.h"
#include "ui/AudioLevels.h"
#include "ui/AudioMeter.h"
#include "ui/VoiceOverDialog.h"
#include "core/Multicam.h"
#include "core/TimelineOps.h"
#include "ui/RegisteredTo.h"
#include "ui/AudioFinishingWindow.h"
#include "ui/DeviceChainView.h"
#include "ui/EffectEditor.h"
#include "ui/KeyframeEditor.h"
#include "ui/EventPropertiesDialog.h"
#include "ui/EffectsBrowser.h"
#include "ui/NodeEffectEditor.h"
#include "core/NodeGraph.h"
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
#include <QSet>
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
#include <QRandomGenerator>
#include <QTimer>
#include <QTreeWidget>
#include <QToolBar>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <atomic>
#include <thread>

int MainWindow::runEditorTest(QStringList& log)
{
    int failures = 0;
    // Each result as it comes, so a test that hangs or crashes shows where.
    QFile progress(QDir(QFileInfo(autosaveDir()).absolutePath()).filePath(QStringLiteral("progress.txt")));
    progress.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    // No result for 90 s: where the window's thread is stuck goes into it too (Windows).
    auto lastNote = std::make_shared<std::atomic<qint64>>(QDateTime::currentMSecsSinceEpoch());
    auto testDone = std::make_shared<std::atomic<bool>>(false);
    struct Done {
        std::shared_ptr<std::atomic<bool>> flag;
        ~Done() { *flag = true; }
    } done{testDone};
#ifdef Q_OS_WIN
    std::thread([lastNote, testDone, path = progress.fileName(), thread = GetCurrentThreadId()] {
        bool reported = false;
        while (!*testDone) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!reported && QDateTime::currentMSecsSinceEpoch() - *lastNote > 90000) {
                reported = true;
                const QString report = crash::reportHang(thread);
                QFile f(path);
                if (f.open(QIODevice::Append | QIODevice::Text))
                    f.write("STUCK for 90 s:\n" + report.toUtf8());
            }
        }
    }).detach();
#endif
    auto note = [&, lastNote](const QString& line) {
        *lastNote = QDateTime::currentMSecsSinceEpoch();
        progress.write(line.toUtf8() + '\n');
        progress.flush();
    };
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        note(log.back());
        if (!ok)
            ++failures;
    };
    auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    if (qEnvironmentVariableIsSet("MVP_ADVERSARIAL_TRANSPORT_ONLY"))
        return runTransportHardening(m_mcp, log);
    if (m_project->media().empty()) {
        check(false, QStringLiteral("editor test: give it a media file"));
        return failures;
    }
    if (qEnvironmentVariableIsSet("MVP_COLLECTION_ONLY"))
        return runCollectionUiTests(this, log);
    const QString mediaPath = m_project->media().begin()->second->path;
    const QDir work(QFileInfo(autosaveDir()).absolutePath()); // the test's own folder
    QSettings().clear();

    {
        const Timeline initial = m_project->timeline();
        Timeline tl;
        const auto media = m_project->media().begin()->second;
        auto placed = ops::insertMedia(tl, *media, 0, -1, m_project->settings().fps, true);
        for (int id : placed)
            tl.findClip(id)->length = 2 * kUsPerSecond;
        QString error;
        auto compounds = story::compound(tl, std::set<int>(placed.begin(), placed.end()), "Test compound", &error);
        m_project->setTimelineLive(tl);
        check(!compounds.empty(), "GUI fixture creates compound");
        if (!compounds.empty()) {
            const int id = compounds.front();
            const Timeline original = tl;
            QTimer::singleShot(300, this, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                auto* child = dialog ? dialog->findChild<TimelineWidget*>("sequenceTimeline") : nullptr;
                check(child && dialog->objectName() == "sequenceEditor", "sequence opens editable child timeline");
                if (child) {
                    child->selectAll();
                    child->setCursorTime(kUsPerSecond);
                    child->splitAtCursor();
                    dialog->grab().save(work.filePath("sequence-editor.png"));
                }
                if (dialog)
                    dialog->accept();
            });
            editCompound(id);
            const auto* changed = m_project->timeline().findClip(id);
            check(changed && changed->sequence.value->tracks.front().clips.size() == 2, "saving embedded edit retains child split");
            m_project->undoStack()->undo();
            check(m_project->timeline() == original, "compound edit is one parent undo step");
            QTimer::singleShot(300, this, [&] {
                auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
                if (dialog) {
                    if (auto* child = dialog->findChild<TimelineWidget*>("sequenceTimeline")) {
                        child->selectAll();
                        child->deleteSelection(false);
                    }
                    dialog->reject();
                }
            });
            editCompound(id);
            check(m_project->timeline() == original, "cancelling embedded edits preserves parent");
            m_project->setTimelineLive(initial);
            m_timeline->selectClips(std::set<int>(placed.begin(), placed.end()));
        }
        // Use ordinary footage to exercise preview rollback and atomic undo.
        Timeline footage;
        auto clips = ops::insertMedia(footage, *media, 0, -1, m_project->settings().fps, true);
        m_project->setTimelineLive(footage);
        m_timeline->selectClips(std::set<int>(clips.begin(), clips.end()));
        const int undoCount = m_project->undoStack()->count();
        QTimer::singleShot(250, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                for (auto* b : dialog->findChildren<QPushButton*>())
                    if (b->text() == tr("Roll / Preview"))
                        b->click();
                wait(150);
                if (auto* preview = dialog->findChild<PreviewWidget*>("dicePreview")) {
                    const auto frame = preview->image();
                    bool colour = false;
                    for (int y = 0; y < frame.height(); y += 20)
                        for (int x = 0; x < frame.width(); x += 20)
                            colour |= qGray(frame.pixel(x, y)) > 20;
                    check(colour, "dice previews selected footage when the cursor is outside it");
                }
                dialog->grab().save(work.filePath("directors-dice.png"));
                dialog->reject();
            }
        });
        directorsDice();
        check(m_project->timeline() == footage && m_project->undoStack()->count() == undoCount,
              "dice preview cancel restores original without undo entry");
        QTimer::singleShot(250, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog) {
                for (auto* b : dialog->findChildren<QPushButton*>())
                    if (b->text() == tr("Roll / Preview"))
                        b->click();
                dialog->accept();
            }
        });
        directorsDice();
        check(m_project->timeline() != footage, "dice apply commits variation");
        m_project->undoStack()->undo();
        check(m_project->timeline() == footage, "dice variation undoes atomically");
        QTimer::singleShot(250, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(dialog && dialog->objectName() == "narrationStudio" && dialog->findChild<QPlainTextEdit*>("narrationScript"),
                  "narration studio exposes script editor");
            if (dialog) {
                dialog->grab().save(work.filePath("narration-studio.png"));
                dialog->reject();
            }
        });
        narrationStudio();
        m_project->setTimelineLive(initial);
        {
            // Tools > Effect Builder: a graph built in the editor (Source -> Pixelate and Invert, blended) shows live, undoes,
            // and saved, is an effect in the Effects tab under User Created.
            std::vector<std::pair<QString, QString>> videos;
            for (const auto& [id, m] : m_project->media())
                if (m && !m->info.video.empty() && !m->info.isStill)
                    videos.emplace_back(QFileInfo(m->path).fileName(), m->path);
            NodeEffectEditor editor(videos, videos.empty() ? QString() : videos.front().second, m_project->settings().width, this);
            editor.resize(1500, 860);
            editor.show();
            const int src = editor.graph().nodes[0].id, out = editor.graph().nodes[1].id;
            const int pix = editor.addNode(QStringLiteral("fx.pixelate"), QPointF(250, -170));
            const int inv = editor.addNode(QStringLiteral("fx.invert"), QPointF(250, 170));
            const int blend = editor.addNode(QStringLiteral("blend"), QPointF(500, 0));
            const bool wired = editor.connectNodes(src, pix, 0) && editor.connectNodes(src, inv, 0) && editor.connectNodes(pix, blend, 0)
                               && editor.connectNodes(inv, blend, 1) && editor.connectNodes(blend, out, 0) && !editor.connectNodes(blend, pix, 0);
            editor.setParam(blend, QStringLiteral("opacity"), 0.5);
            editor.setExposed(blend, QStringLiteral("opacity"), true);
            editor.step();
            const QImage built = editor.result();
            editor.undo(); // (the exposed setting)
            editor.undo(); // (the opacity)
            editor.undo(); // (the wire to the Output)
            editor.step();
            const QImage undone = editor.result();
            editor.redo();
            editor.redo();
            editor.redo();
            editor.step();
            const QString file = work.filePath(QStringLiteral("GUI Test Mosaic.mvpfx"));
            nodes::Graph named = editor.graph();
            named.name = QStringLiteral("GUI Test Mosaic");
            named.find(out)->pos = QPointF(760, 0);
            editor.setGraph(named);
            QApplication::processEvents();
            editor.step();
            editor.grab().save(work.filePath("effect-builder.png"));
            QString error;
            const bool saved = editor.saveTo(file, &error);
            const EffectDef* def = findEffect(nodes::effectId(named.name));
            EffectsBrowser browser(EffectsBrowser::Kind::Effects);
            const bool listed = !browser.findItems(QStringLiteral("GUI Test Mosaic"), Qt::MatchExactly | Qt::MatchRecursive).isEmpty();
            check(wired && !built.isNull() && built != undone && saved && def && listed
                      && std::any_of(def->params.begin(), def->params.end(), [](const ParamDef& p) { return p.name == QLatin1String("Opacity"); }),
                  QStringLiteral("Effect Builder: a graph wired in the editor shows live (%1), undoes, and saved is in the Effects tab, its "
                                 "shown setting with it (%2)")
                      .arg(editor.status(), error));
            editor.close();
            {
                // On an event, the device says to double-click it; double-clicked, the Effect Builder opens on it, and saved
                // there, the event's copy of the graph changes.
                Timeline tl = m_project->timeline();
                Clip* clip = nullptr;
                for (Track& t : tl.tracks)
                    if (t.type == TrackType::Video)
                        for (Clip& c : t.clips)
                            if (!clip)
                                clip = &c;
                bool hinted = false, opened = false, updated = false;
                if (clip && def) {
                    EffectInstance fx = makeEffect(*def, tl.allocId());
                    const int effectId = fx.id, clipId = clip->id;
                    const QString before = fx.params[QStringLiteral("graph")].text;
                    clip->effects.push_back(fx);
                    m_project->applyEdit(tl, QStringLiteral("test"));
                    m_devices->setTarget({FxTarget::Kind::Clip, clipId});
                    QApplication::processEvents();
                    const auto hints = m_devices->findChildren<QLabel*>(QStringLiteral("userEffectHint"));
                    hinted = hints.size() == 1 && hints.front()->text().contains(QStringLiteral("Double-click"));
                    m_devices->grab().save(work.filePath("user-effect-device.png"));
                    const auto editorsBefore = findChildren<NodeEffectEditor*>().size();
                    if (hinted) {
                        QWidget* label = hints.front();
                        const QPointF at(label->width() / 2.0, label->height() / 2.0);
                        QMouseEvent dbl(QEvent::MouseButtonDblClick, at, label->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                        QApplication::sendEvent(label, &dbl);
                    }
                    const auto editors = findChildren<NodeEffectEditor*>();
                    NodeEffectEditor* opened2 = editors.size() > editorsBefore ? editors.back() : nullptr;
                    opened = opened2 && opened2->graph().nodes.size() == editor.graph().nodes.size();
                    if (opened2) {
                        opened2->setParam(blend, QStringLiteral("opacity"), 0.2);
                        opened2->saveTo(file);
                        const Clip* after = m_project->timeline().findClip(clipId);
                        const auto it = after && !after->effects.empty() ? after->effects.back().params.find(QStringLiteral("graph"))
                                                                         : ParamMap::const_iterator{};
                        updated = after && !after->effects.empty() && after->effects.back().id == effectId && it->second.text != before;
                        opened2->close();
                    }
                }
                check(hinted && opened && updated, QStringLiteral("a user effect on an event says to double-click it (%1); that opens the Effect Builder "
                                                                   "on it (%2), and saving there changes the event's effect (%3)")
                                                       .arg(hinted)
                                                       .arg(opened)
                                                       .arg(updated));
                m_project->setTimelineLive(initial);
            }
            if (def) {
                // The same on the Live tab's FX chain: double-clicked, it opens the Effect Builder.
                Timeline tl = m_project->timeline();
                EffectInstance fx = makeEffect(*def, tl.allocId());
                const int effectId = fx.id;
                tl.liveEffects.push_back(fx);
                m_project->applyEdit(tl, QStringLiteral("test"));
                DeviceChainView* live = m_live->devices();
                QWidget* tabBefore = m_modes->currentWidget();
                m_modes->setCurrentWidget(m_live);
                live->setTarget({FxTarget::Kind::Live, 0});
                QApplication::processEvents();
                const auto hints = live->findChildren<QLabel*>(QStringLiteral("userEffectHint"));
                const auto editorsBefore = findChildren<NodeEffectEditor*>().size();
                if (hints.size() == 1) {
                    QWidget* label = hints.front();
                    const QPointF at(label->width() / 2.0, label->height() / 2.0);
                    QMouseEvent dbl(QEvent::MouseButtonDblClick, at, label->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                    QApplication::sendEvent(label, &dbl);
                }
                const auto editors = findChildren<NodeEffectEditor*>();
                NodeEffectEditor* opened = editors.size() > editorsBefore ? editors.back() : nullptr;
                bool updated = false, liveFirst = false;
                QStringList blankPatterns;
                if (opened) {
                    // Opened on the Live tab, the example is what is live (nothing here: the color bars), and every
                    // pattern in the list draws.
                    auto varied = [](const QImage& img) {
                        QSet<QRgb> seen;
                        for (int y = 0; y < 8 && !img.isNull(); ++y)
                            for (int x = 0; x < img.width(); ++x)
                                seen.insert(img.pixel(x, y * (img.height() - 1) / 7));
                        return seen.size() > 1;
                    };
                    for (QComboBox* box : opened->findChildren<QComboBox*>())
                        if (box->count() && box->itemData(0).toString() == QLatin1String("live")) {
                            liveFirst = box->currentIndex() == 0;
                            opened->step();
                            if (!varied(opened->result()))
                                blankPatterns << box->itemText(0) + (m_live->engine()->isRunning() ? QStringLiteral(" (capturing)") : QString());
                            for (int i = 0; i < box->count(); ++i)
                                if (box->itemData(i).toString().startsWith(QLatin1String("pattern:"))) {
                                    box->setCurrentIndex(i);
                                    emit box->activated(i);
                                    opened->step();
                                    if (!varied(opened->result()))
                                        blankPatterns << box->itemText(i);
                                }
                        }
                    opened->setParam(blend, QStringLiteral("opacity"), 0.3);
                    opened->saveTo(file);
                    const EffectChain& after = m_project->timeline().liveEffects;
                    updated = !after.empty() && after.back().id == effectId;
                    opened->close();
                }
                check(hints.size() == 1 && opened && updated, QStringLiteral("a user effect in the Live FX says to double-click it (%1); that opens the "
                                                                              "Effect Builder (%2), and saving there keeps it in the Live FX (%3)")
                                                                  .arg(hints.size())
                                                                  .arg(opened != nullptr)
                                                                  .arg(updated));
                check(liveFirst && blankPatterns.isEmpty(), QStringLiteral("from the Live tab the Effect Builder's example is the Live picture (%1); "
                                                                           "examples that drew nothing: %2")
                                                                .arg(liveFirst)
                                                                .arg(blankPatterns.isEmpty() ? QStringLiteral("none") : blankPatterns.join(QStringLiteral(", "))));
                m_modes->setCurrentWidget(tabBefore);
                m_project->setTimelineLive(initial);
            }
            QFile::remove(file);
        }
        {
            // Render Selection to New Track: the time selection becomes a file on a Rendered track on top, which then hides
            // the tracks under it (the compositors start there).
            m_project->setTimelineLive(initial);
            m_timeline->setRegion(kUsPerSecond / 2, kUsPerSecond * 3 / 2);
            const int undoBefore = m_project->undoStack()->index();
            QTimer closer;
            connect(&closer, &QTimer::timeout, this, [] {
                if (auto* d = qobject_cast<QDialog*>(QApplication::activeModalWidget()))
                    d->reject(); // (it closes once the render is done)
            });
            closer.start(200);
            renderInPlace();
            closer.stop();
            const Timeline& after = m_project->timeline();
            const Track* rendered = !after.tracks.empty() && after.tracks[0].name == QStringLiteral("Rendered") ? &after.tracks[0] : nullptr;
            const Clip* c = rendered && rendered->clips.size() == 1 ? &rendered->clips[0] : nullptr;
            const auto media = c ? m_project->mediaItem(c->mediaId) : nullptr;
            const bool placed = c && c->start == kUsPerSecond / 2 && c->length == kUsPerSecond && media && media->info.ok()
                                && m_project->undoStack()->index() == undoBefore + 1;
            const bool covers = placed && coveringTrack(*m_project->snapshot(), kUsPerSecond) == 0;
            check(placed && covers, QStringLiteral("Render Selection to New Track: a 1 s render on a Rendered track on top, hiding what's under it (%1)")
                                        .arg(media ? QFileInfo(media->path).fileName() : QStringLiteral("nothing")));
            const QString file = media ? media->path : QString();
            m_project->undoStack()->undo();
            if (media)
                m_project->removeMedia(media->id);
            QFile::remove(file);
            QDir().rmdir(QFileInfo(file).path()); // (only if nothing else is in it)
            m_timeline->setRegion(0, 0);
            m_project->setTimelineLive(initial);
        }
        {
            // Multicam: two video events of the clip on two tracks become angles under a Multicam track (the angles window
            // opens); with playback stopped, pressing 2 on the timeline switches the stretch under the cursor to angle 2.
            Timeline tl = initial;
            const auto clipMedia = m_project->media().begin()->second;
            std::vector<int> picks;
            for (int k = 0; k < 2; ++k) {
                const int ti = ops::addTrack(tl, TrackType::Video, 0);
                Clip c;
                c.id = tl.allocId();
                c.mediaId = clipMedia->id;
                c.streamIndex = clipMedia->info.video.front().index;
                c.start = 0;
                c.length = std::min<TimeUs>(clipMedia->info.duration, 3 * kUsPerSecond);
                tl.tracks[size_t(ti)].clips.push_back(c);
                picks.push_back(c.id);
            }
            m_project->setTimelineLive(tl);
            m_timeline->setSelection({picks.begin(), picks.end()});
            const int undoBefore = m_project->undoStack()->index();
            makeMulticam();
            const int mi = multicam::trackIndex(m_project->timeline());
            auto* window = findChild<QWidget*>(QStringLiteral("multicamWindow"));
            const bool made = mi == 0 && m_project->timeline().tracks[0].name == QStringLiteral("Multicam") && window && window->isVisible()
                              && m_project->undoStack()->index() == undoBefore + 1;
            m_timeline->setCursorTime(kUsPerSecond, false);
            m_timeline->setFocus();
            QKeyEvent two(QEvent::KeyPress, Qt::Key_2, Qt::NoModifier, QStringLiteral("2"));
            QApplication::sendEvent(m_timeline, &two);
            const auto& cuts = m_project->timeline().tracks[0].clips;
            const bool switched = cuts.size() == 1 && multicam::angleOf(cuts[0]) == 2 && m_project->undoStack()->index() == undoBefore + 2;
            QWidget* shot = window ? window : this;
            if (window)
                window->grab().save(work.filePath("multicam.png"));
            check(made && switched && shot,
                  QStringLiteral("Make Multicam: a Multicam track on top and the angles window; 2 on the timeline switches to angle 2 (%1 cut(s))")
                      .arg(cuts.size()));
            if (window)
                window->close();
            m_project->undoStack()->undo();
            m_project->undoStack()->undo();
            m_project->setTimelineLive(initial);
        }
        {
            // Record Voice-Over: a take (a test tone, throttled to real time) lands on a new Voice-over track where the cursor
            // was, as one undo step, and the edit stops playing and goes back there.
            m_timeline->setCursorTime(2 * kUsPerSecond);
            auto* recorder = recordVoiceOver();
            recorder->setSource({"Test tone", "lavfi", "sine=frequency=440:sample_rate=48000,arealtime"});
            const int undoBefore = m_project->undoStack()->index();
            recorder->startTake();
            const bool playing = m_engine->isPlaying() == recorder->playsEdit();
            wait(1200);
            recorder->stopTake();
            const Track* track = nullptr;
            for (const auto& t : m_project->timeline().tracks)
                if (t.name == "Voice-over")
                    track = &t;
            const Clip* take = track && track->clips.size() == 1 ? &track->clips.front() : nullptr;
            check(take && take->start == 2 * kUsPerSecond && take->length > 800000 && take->length < 1700000 && playing
                      && m_project->undoStack()->index() == undoBefore + 1 && !m_engine->isPlaying()
                      && m_timeline->cursorTime() == 2 * kUsPerSecond,
                  QStringLiteral("Record Voice-Over: a %1 s take on the Voice-over track at the cursor, one undo step")
                      .arg(take ? double(take->length) / kUsPerSecond : 0.0, 0, 'f', 2));
            const auto media = take ? m_project->mediaItem(take->mediaId) : nullptr;
            const QString file = media ? media->path : QString();
            recorder->close();
            m_project->undoStack()->undo();
            m_project->setTimelineLive(initial);
            if (media)
                m_project->removeMedia(media->id);
            QFile::remove(file); // (an unsaved project's takes go to the Documents folder)
        }
        check(findChild<QWidget*>("storyPanel") != nullptr, "Story panel is available in editor");
        if (auto* panel = findChild<QWidget*>("storyPanel")) {
            Timeline storyTimeline = initial;
            QString error;
            const int chapter = story::addChapter(storyTimeline, "Draft test", "", 2 * kUsPerSecond, 2 * kUsPerSecond, nullptr, 0,
                                                  m_project->settings().fps, &error);
            m_project->setTimelineLive(storyTimeline);
            auto* list = panel->findChild<QListWidget*>("storyChapters");
            auto* script = panel->findChild<QPlainTextEdit*>("chapterScript");
            if (list)
                list->setCurrentRow(0);
            if (script)
                script->setPlainText("Keep this unsaved draft");
            Clip extra;
            extra.id = storyTimeline.allocId();
            extra.start = initial.duration();
            extra.length = kUsPerSecond;
            extra.generator.type = "text";
            extra.generator.params["text"].text = "Extra shot";
            storyTimeline.tracks.front().clips.push_back(extra);
            m_project->setTimelineLive(storyTimeline);
            m_timeline->selectClips({extra.id});
            for (auto* button : panel->findChildren<QPushButton*>())
                if (button->text() == tr("Assign selected clips"))
                    button->click();
            check(script && script->toPlainText() == "Keep this unsaved draft",
                  "assigning chapter footage preserves the unsaved script draft");
            for (auto* button : panel->findChildren<QPushButton*>())
                if (button->text() == tr("Save changes"))
                    button->click();
            bool saved = false;
            for (auto value : m_project->timeline().chapters)
                if (value.toObject()["id"].toInt() == chapter)
                    saved = value.toObject()["script"] == "Keep this unsaved draft";
            check(saved, "Story Save changes commits the script draft");
            m_project->setTimelineLive(initial);
        }
    }
    {
        ExportSettings es;
        es.path = work.filePath("render-window-test.mp4");
        es.rangeEnd = kUsPerSecond;
        auto monitor = std::make_shared<RenderMonitor>();
        ExportStatus info;
        info.stage = "Rendering";
        info.detail = "Compositing a test frame";
        info.frames = 12;
        info.totalFrames = 30;
        info.progress = .4;
        info.renderer = "CPU";
        monitor->update(info);
        RenderStatusDialog dialog(es, *m_project->snapshot(), monitor, this);
        dialog.show();
        wait(250);
        check(dialog.isVisible(), "render status window opens immediately");
        dialog.reject();
        check(dialog.isVisible(), "running render cannot close before worker releases resources");
        auto buttons = dialog.findChildren<QPushButton*>();
        QPushButton* close = nullptr;
        for (auto* b : buttons) {
            if (b->text() == tr("Close"))
                close = b;
            if (b->text() == tr("Cancel render"))
                b->click();
        }
        check(monitor->cancel && close && !close->isEnabled(), "render Cancel requests worker cancellation; Close waits for completion");
        info.stage = "Complete";
        info.detail = "File finalized and closed";
        info.progress = 1;
        monitor->update(info);
        monitor->done = true;
        wait(250);
        dialog.grab().save(work.filePath("render-status-window.png"));
        check(close && close->isEnabled(), "completed render keeps report available and enables Close");
        dialog.reject();
        check(!dialog.isVisible(), "completed status window closes normally");
    }
    {
        // Render Queue: two short renders one after another, a duplicate file refused, one stopped.
        RenderQueue* queue = renderQueue();
        ExportSettings s = settingsFromPreset(exportPresets().front(), m_project->settings());
        s.width = 320;
        s.height = 180;
        s.rangeStart = 0;
        s.rangeEnd = kUsPerSecond / 2;
        const QString first = work.filePath("queue-1.mp4"), second = work.filePath("queue-2.mp4"), third = work.filePath("queue-3.mp4");
        for (const QString& f : {first, second, third})
            QFile::remove(f);
        s.path = first;
        const bool added = queue->add("one", m_project->snapshot(), s);
        const bool refused = !queue->add("again", m_project->snapshot(), s);
        s.path = second;
        queue->add("two", m_project->snapshot(), s);
        queue->start();
        for (int i = 0; i < 600 && (queue->busy() || queue->waiting()); ++i)
            wait(50);
        auto rendered = [](const QString& f) { return probeMedia(f).ok(); };
        check(added && refused && !queue->busy() && !queue->waiting() && rendered(first) && rendered(second),
              "render queue renders its jobs one after another and refuses a second job to the same file");
        s.path = third;
        s.rangeEnd = 30 * kUsPerSecond;
        queue->add("three", m_project->snapshot(), s);
        queue->start();
        wait(300);
        queue->stop();
        for (int i = 0; i < 200 && queue->busy(); ++i)
            wait(50);
        check(!queue->busy() && queue->waiting() == 0 && !QFileInfo::exists(third), "stopping the queue cancels the running render and leaves no file");
        queue->close();
    }
    {
        // Deliver tab: checks, two outputs rendered through the queue into History, the publish pack saved.
        m_modes->setCurrentWidget(m_deliver);
        wait(200);
        auto* folder = m_deliver->findChild<QLineEdit*>("deliverFolder");
        folder->setText(work.filePath("deliver"));
        emit folder->editingFinished();
        m_deliver->setOutputs({"MP3 - 192 kb/s", "WAV - 16-bit PCM"});
        const int checks = int(m_deliver->findChildren<QWidget*>("deliverCheck").size());
        const auto* mp3 = [] {
            for (const auto& p : exportPresets())
                if (p.name == "MP3 - 192 kb/s")
                    return &p;
            return static_cast<const ExportPreset*>(nullptr);
        }();
        const QString mp3Path = mp3 ? m_deliver->outputPath(*mp3) : QString();
        QDir(work.filePath("deliver")).removeRecursively();
        QString why;
        const int queued = m_deliver->renderOutputs(&why);
        for (int i = 0; i < 1200 && (renderQueue()->busy() || renderQueue()->waiting()); ++i)
            wait(50);
        wait(200);
        const auto* history = m_deliver->findChild<QTreeWidget*>("deliverHistory");
        bool listed = false;
        for (int i = 0; i < history->topLevelItemCount(); ++i)
            listed = listed || (history->topLevelItem(i)->text(2) == "MP3 - 192 kb/s" && history->topLevelItem(i)->text(1) == QFileInfo(mp3Path).fileName());
        m_deliver->findChild<QPlainTextEdit*>("deliverDescription")->setPlainText("Chapters below.");
        QString packError;
        const bool pack = m_deliver->savePack(&packError);
        m_modes->grab().save(work.filePath("deliver-tab.png"));
        const QString description = QDir(work.filePath("deliver")).filePath(QFileInfo(mp3Path).completeBaseName().section(" - ", 0, -2) + " - description.txt");
        bool exact = false;
        const double mp3Bytes = DeliverView::estimateBytes(*mp3, 1920, 1080, 30, 60 * kUsPerSecond, &exact);
        m_deliver->findChild<QPlainTextEdit*>("deliverDescription")->setPlainText("{title}: {length}");
        const bool expanded = !m_deliver->expandedDescription().contains('{');
        check(checks >= 5 && mp3Path.endsWith(" - MP3.mp3") && queued == 2 && probeMedia(mp3Path).ok() && listed && pack
                  && QFileInfo::exists(description) && exact && std::abs(mp3Bytes - 192000.0 * 60 / 8) < 1 && expanded,
              QStringLiteral("Deliver tab: %1 checks, 2 outputs rendered (%2) and listed in History, publish pack saved %3")
                  .arg(checks)
                  .arg(why.isEmpty() ? QFileInfo(mp3Path).fileName() : why)
                  .arg(packError));
        {
            // Stopped (here, or by the AI): the waiting outputs no longer read "Rendering", with a time left that grows.
            m_deliver->renderOutputs(&why);
            renderQueue()->stop();
            for (int i = 0; i < 100 && renderQueue()->busy(); ++i)
                wait(50);
            wait(1000);
            const QString status = m_deliver->findChild<QLabel*>("deliverRenderStatus")->text();
            check(status.startsWith("Stopped") && !status.contains("2 of 2"), QStringLiteral("Deliver: a stopped queue reads \"%1\"").arg(status));
        }
        renderQueue()->close();
        m_modes->setCurrentIndex(0);
    }

    failures += m_timeline->runEditTest(log);

    // ---- J / K / L ----
    const TimeUs duration = m_project->timeline().duration();
    seekTo(duration / 2);
    const TimeUs start = m_timeline->cursorTime();
    shuttle(1);
    const bool playing = m_engine->isPlaying();
    shuttle(1);
    const bool fast = m_shuttleRate == 2.0 && !m_engine->isPlaying();
    wait(400);
    const TimeUs forward = m_timeline->cursorTime() - start;
    shuttle(0);
    const bool stopped = m_shuttleRate == 0 && !m_engine->isPlaying();
    const TimeUs at = m_timeline->cursorTime();
    shuttle(-1);
    shuttle(-1);
    const bool back2 = m_shuttleRate == -2.0;
    wait(250);
    const TimeUs backward = at - m_timeline->cursorTime();
    shuttle(0);
    check(playing && fast && stopped && back2 && forward > 500000 && backward > 250000,
          QStringLiteral("L plays, L again shuttles 2x (%1 s in 0.4 s), K stops, J J shuttles back 2x (%2 s in 0.25 s)")
              .arg(double(forward) / kUsPerSecond, 0, 'f', 2)
              .arg(double(backward) / kUsPerSecond, 0, 'f', 2));
    stopPlayback(false);

    // ---- Audio Sync with Picture: the picture waits for a slow output's sound ----
    {
        seekTo(0);
        m_engine->setOutputLatency(400000);
        togglePlay();
        wait(250);
        const TimeUs held = m_engine->position();
        wait(500);
        const TimeUs moving = m_engine->position();
        togglePlay();
        m_engine->setOutputLatency(0);
        check(held == 0 && moving > 100000 && moving < 600000,
              QStringLiteral("with 400 ms picture delay, playback holds the first frame for it, then runs (%1 s at 0.25 s, %2 s at 0.75 s)")
                  .arg(double(held) / kUsPerSecond, 0, 'f', 2)
                  .arg(double(moving) / kUsPerSecond, 0, 'f', 2));
    }

    // ---- The picture follows the sound even when the mix starts late: an effect never used before is measured on
    // first use (a loudness normaliser takes a while), and the device's silence meanwhile used to count as played ----
    {
        Timeline tl = m_project->timeline();
        auto audio = std::find_if(tl.tracks.begin(), tl.tracks.end(), [](const Track& t) { return t.type == TrackType::Audio && !t.clips.empty(); });
        if (audio != tl.tracks.end()) {
            EffectInstance slow = makeEffectOfType(QStringLiteral("a_ffmpeg"), tl.allocId());
            slow.params[QStringLiteral("filter")].text =
                QStringLiteral("aecho=0.8:0.88:%1:0.3,loudnorm").arg(QRandomGenerator::global()->bounded(20, 400)); // (never measured)
            audio->effects.push_back(slow);
            m_project->applyEdit(tl, QStringLiteral("slow effect"));
            seekTo(0);
            togglePlay();
            wait(1500);
            const TimeUs shown = m_engine->position(), heard = m_engine->heardPosition();
            togglePlay();
            m_project->undoStack()->undo();
            check(std::abs(shown - heard) < 40000,
                  QStringLiteral("the picture stays with the sound when the mix starts late (picture %1 s, sound %2 s)")
                      .arg(double(shown) / kUsPerSecond, 0, 'f', 3)
                      .arg(double(heard) / kUsPerSecond, 0, 'f', 3));
        }
    }

    // ---- Moving the cursor while playing carries on from there; Space stops there ----
    {
        seekTo(0);
        togglePlay();
        wait(150);
        m_timeline->setCursorTime(duration / 2); // as a click on the timeline does
        emit m_timeline->cursorMoved(duration / 2);
        wait(300);
        const bool still = m_engine->isPlaying();
        const TimeUs pos = m_engine->position();
        togglePlay();
        check(
            still && pos >= duration / 2 && !m_engine->isPlaying() && m_timeline->cursorTime() == duration / 2,
            QStringLiteral("a click on the timeline while playing jumps there and keeps playing (at %1 s); Space stops, back at the click")
                .arg(double(pos) / kUsPerSecond, 0, 'f', 2));
        m_clickJumpAction->setChecked(false);
        seekTo(0);
        togglePlay();
        wait(150);
        m_timeline->setCursorTime(duration / 2);
        emit m_timeline->cursorMoved(duration / 2);
        wait(300);
        const bool playing2 = m_engine->isPlaying();
        const TimeUs pos2 = m_engine->position();
        togglePlay();
        const bool stopped2 = !m_engine->isPlaying() && m_timeline->cursorTime() == duration / 2;
        togglePlay();
        const bool fromClick = m_engine->isPlaying() && m_playStart == duration / 2;
        stopPlayback(false);
        m_clickJumpAction->setChecked(true);
        check(playing2 && pos2 < duration / 2 && stopped2 && fromClick,
              QStringLiteral(
                  "Jumps There off: a click while playing carries on (at %1 s) and sets the start; Space stops there and plays from it")
                  .arg(double(pos2) / kUsPerSecond, 0, 'f', 2));
        // The sound output changing while playing (headphones plugged in): it carries on, from where it was.
        seekTo(0);
        togglePlay();
        wait(400);
        const TimeUs before = m_engine->position();
        m_engine->reopenAudioOutput();
        wait(400);
        const TimeUs after = m_engine->position();
        const bool stillPlaying = m_engine->isPlaying();
        stopPlayback(false);
        check(stillPlaying && after > before + 150000 && after < before + 1000000,
              QStringLiteral("the sound output reopened while playing (as when headphones are plugged in): it plays on from %1 s to %2 s")
                  .arg(double(before) / kUsPerSecond, 0, 'f', 2)
                  .arg(double(after) / kUsPerSecond, 0, 'f', 2));
    }

    // ---- Proxies: made in the background, played by the preview, marked in Project Media, deleted ----
    if (auto media = m_project->media().begin()->second; media->info.hasVideo() && !media->info.isStill) {
        qputenv("MVP_PROXY_DIR", work.filePath(QStringLiteral("proxies")).toLocal8Bit());
        const int stream = media->info.video.front().index;
        proxy::remove(media->path, stream);
        createProxies({media->id}, false, true);
        const bool shown = m_proxyStatus->isVisible() || m_proxies->busy();
        QElapsedTimer t;
        t.start();
        while ((m_proxies->busy() || m_proxyStatus->isVisible()) && t.elapsed() < 60000)
            wait(50);
        bool marked = false;
        for (int i = 0; i < m_bin->count(); ++i)
            marked |= m_bin->item(i)->data(Qt::UserRole).toInt() == media->id && m_bin->item(i)->toolTip().contains(tr("Proxy: ready"));
        m_proxyAction->setChecked(false);
        const bool off = !proxy::enabled() && proxy::playbackPath(media->path, stream) == media->path;
        m_proxyAction->setChecked(true);
        const bool on = proxy::enabled() && proxy::playbackPath(media->path, stream) == proxy::file(media->path, stream);
        check(shown && proxy::exists(media->path, stream) && !m_proxyStatus->isVisible() && marked && off && on,
              QStringLiteral("Create Proxy makes one in the background (status bar while it works), Project Media marks it, "
                             "the Proxies button switches the preview between it and the file (%1 s)")
                  .arg(t.elapsed() / 1000.0, 0, 'f', 1));
        deleteProxies({media->id});
        check(!proxy::exists(media->path, stream), QStringLiteral("Delete Proxy removes it"));
        qunsetenv("MVP_PROXY_DIR");
    }

    // ---- AI control (MCP): the HTTP endpoint, the tools, the stdio bridge ----
    {
        const quint16 port = 47899;
        QSettings().setValue(QStringLiteral("mcp/port"), port);
        setAiControl(true);
        int nextId = 1;
        // A raw HTTP client on this thread (the server runs here too: events keep flowing while it waits).
        auto http = [&](const QByteArray& method, const QByteArray& body, const QByteArray& extra, int* status) {
            QTcpSocket s;
            s.connectToHost(QHostAddress::LocalHost, port);
            QElapsedTimer t;
            t.start();
            while (s.state() != QAbstractSocket::ConnectedState && s.state() != QAbstractSocket::UnconnectedState && t.elapsed() < 3000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
            *status = 0;
            if (s.state() != QAbstractSocket::ConnectedState)
                return QByteArray(); // nothing listening
            s.write(method + " /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + QByteArray::number(port) + "\r\nContent-Type: application/json\r\n"
                    + extra + "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            QByteArray in;
            *status = 0;
            while (t.elapsed() < 120000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
                in += s.readAll();
                const int end = int(in.indexOf("\r\n\r\n"));
                if (end < 0)
                    continue;
                int length = 0;
                for (const QByteArray& l : in.left(end).split('\n'))
                    if (l.toLower().startsWith("content-length:"))
                        length = l.mid(15).trimmed().toInt();
                if (in.size() >= end + 4 + length) {
                    *status = in.mid(9, 3).toInt();
                    return in.mid(end + 4, length);
                }
            }
            return QByteArray();
        };
        auto call = [&](const QString& tool, const QJsonObject& args) {
            int status = 0;
            const QJsonObject msg{
                {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                {QStringLiteral("id"), nextId++},
                {QStringLiteral("method"), QStringLiteral("tools/call")},
                {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), tool}, {QStringLiteral("arguments"), args}}}};
            const QJsonObject r =
                QJsonDocument::fromJson(http("POST", QJsonDocument(msg).toJson(QJsonDocument::Compact), {}, &status)).object();
            return r[QStringLiteral("result")].toObject();
        };
        auto content = [](const QJsonObject& result) { return result[QStringLiteral("structuredContent")].toObject(); };
        auto failed = [](const QJsonObject& result) { return result[QStringLiteral("isError")].toBool(); };

        const auto voiceList = content(call(QStringLiteral("list_voices"), {}));
        const auto voiceState = content(call(QStringLiteral("voice_status"), {}));
        check(voiceList.value(QStringLiteral("voices")).toArray().size() == 28 && !voiceState.value(QStringLiteral("busy")).toBool()
                  && failed(call(QStringLiteral("generate_voice"), {{QStringLiteral("voice"), QStringLiteral("missing-voice")}})),
              QStringLiteral("AI control exposes the voice library and async status, and rejects unknown voices without edits"));
        {
            // node_effect: the node types, a graph previewed on the picture, a loop refused, saved as an effect and read back.
            const QJsonObject graph{
                {QStringLiteral("name"), QStringLiteral("GUI Test Node Tool")},
                {QStringLiteral("nodes"), QJsonArray{QJsonObject{{QStringLiteral("id"), 1}, {QStringLiteral("type"), QStringLiteral("source")}},
                                                     QJsonObject{{QStringLiteral("id"), 2}, {QStringLiteral("type"), QStringLiteral("fx.invert")}},
                                                     QJsonObject{{QStringLiteral("id"), 3}, {QStringLiteral("type"), QStringLiteral("mix")},
                                                                 {QStringLiteral("params"), QJsonObject{{QStringLiteral("amount"), 0.75}}}},
                                                     QJsonObject{{QStringLiteral("id"), 4}, {QStringLiteral("type"), QStringLiteral("output")}}}},
                {QStringLiteral("links"), QJsonArray{QJsonArray{1, 2, 0}, QJsonArray{1, 3, 0}, QJsonArray{2, 3, 1}, QJsonArray{3, 4, 0}}},
                {QStringLiteral("exposed"), QJsonArray{QJsonObject{{QStringLiteral("node"), 3}, {QStringLiteral("param"), QStringLiteral("amount")}}}}};
            const int types = content(call(QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("nodes")}}))
                                  .value(QStringLiteral("count")).toInt();
            const QJsonObject preview = call(QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("preview")}, {QStringLiteral("graph"), graph}});
            const bool image = std::any_of(preview[QStringLiteral("content")].toArray().begin(), preview[QStringLiteral("content")].toArray().end(),
                                           [](const QJsonValue& c) { return c.toObject()[QStringLiteral("type")].toString() == QLatin1String("image"); });
            QJsonObject looped = graph;
            QJsonArray links = graph[QStringLiteral("links")].toArray();
            links.append(QJsonArray{3, 2, 0});
            looped[QStringLiteral("links")] = links;
            const bool refused = failed(call(QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("save")}, {QStringLiteral("graph"), looped}}));
            const QJsonObject saved = content(call(QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("save")},
                                                                                    {QStringLiteral("graph"), graph},
                                                                                    {QStringLiteral("overwrite"), true}}));
            const QJsonObject back = content(call(QStringLiteral("node_effect"), {{QStringLiteral("action"), QStringLiteral("get")},
                                                                                   {QStringLiteral("effect"), QStringLiteral("GUI Test Node Tool")}}));
            check(types > 200 && image && refused && saved[QStringLiteral("effect")].toString() == QLatin1String("user.gui_test_node_tool")
                      && findEffect(QStringLiteral("user.gui_test_node_tool")) && back[QStringLiteral("links")].toArray().size() == 4,
                  QStringLiteral("AI control: node_effect lists %1 node types, previews a graph, refuses a loop, saves it as an effect and reads it back")
                      .arg(types));
            QFile::remove(saved[QStringLiteral("file")].toString());
        }
        {
            // The rest of the menus through AI control: envelopes, groups, track pan and blend, duplicating, the media,
            // presets, normalizing, saving a frame, Stabilize, Beat Montage, Track Motion and the YouTube clip's checks.
            const Timeline before = m_project->timeline();
            const QJsonObject proj = content(call(QStringLiteral("get_project"), {}));
            const int media = proj[QStringLiteral("media")].toArray().first().toObject()[QStringLiteral("media_id")].toInt();
            const QJsonArray placed = content(call(QStringLiteral("add_media_to_timeline"), {{QStringLiteral("media_id"), media}, {QStringLiteral("at"), 60}}))
                                          [QStringLiteral("event_ids")].toArray();
            int video = 0, audio = 0, audioTrack = -1, videoTrack = -1;
            for (const QJsonValue v : placed) {
                int ti = -1;
                if (m_project->timeline().findClip(v.toInt(), &ti)) {
                    (m_project->timeline().tracks[size_t(ti)].type == TrackType::Audio ? audio : video) = v.toInt();
                    (m_project->timeline().tracks[size_t(ti)].type == TrackType::Audio ? audioTrack : videoTrack) = ti;
                }
            }
            QStringList broken;
            auto ok = [&](bool good, const char* what) {
                if (!good)
                    broken << QString::fromLatin1(what);
            };
            // Envelopes.
            const QJsonObject env = content(call(QStringLiteral("envelope"), {{QStringLiteral("kind"), QStringLiteral("volume")}, {QStringLiteral("track"), audioTrack},
                                                                              {QStringLiteral("points"), QJsonArray{QJsonArray{60, 1.0}, QJsonArray{62, 0.2, QStringLiteral("smooth")}}}}));
            ok(env[QStringLiteral("points")].toArray().size() == 2 && m_project->timeline().tracks[size_t(audioTrack)].volume.active, "envelope");
            ok(content(call(QStringLiteral("envelope"), {{QStringLiteral("kind"), QStringLiteral("velocity")}, {QStringLiteral("event_id"), video},
                                                         {QStringLiteral("points"), QJsonArray{QJsonArray{0, 1.0}, QJsonArray{1, 2.0}}}}))[QStringLiteral("active")].toBool(),
               "velocity");
            ok(failed(call(QStringLiteral("envelope"), {{QStringLiteral("kind"), QStringLiteral("pan")}, {QStringLiteral("track"), videoTrack}})), "pan on video refused");
            // Groups.
            call(QStringLiteral("group"), {{QStringLiteral("action"), QStringLiteral("ungroup")}, {QStringLiteral("event_ids"), QJsonArray{video, audio}}});
            const bool apart = m_project->timeline().findClip(video)->groupId == 0;
            call(QStringLiteral("group"), {{QStringLiteral("action"), QStringLiteral("group")}, {QStringLiteral("event_ids"), QJsonArray{video, audio}}});
            ok(apart && m_project->timeline().findClip(video)->groupId != 0
                   && m_project->timeline().findClip(video)->groupId == m_project->timeline().findClip(audio)->groupId, "group");
            // Track pan and blend.
            call(QStringLiteral("edit_track"), {{QStringLiteral("track"), audioTrack}, {QStringLiteral("pan"), -0.5}});
            call(QStringLiteral("edit_track"), {{QStringLiteral("track"), videoTrack}, {QStringLiteral("blend"), QStringLiteral("screen")}});
            ok(m_project->timeline().tracks[size_t(audioTrack)].pan == -0.5f && m_project->timeline().tracks[size_t(videoTrack)].blend == 2, "pan / blend");
            // Duplicating.
            const QJsonArray copies = content(call(QStringLiteral("duplicate_events"), {{QStringLiteral("event_ids"), QJsonArray{video, audio}}, {QStringLiteral("at"), 90}}))
                                          [QStringLiteral("event_ids")].toArray();
            const Clip* copy = copies.isEmpty() ? nullptr : m_project->timeline().findClip(copies.first().toInt());
            ok(copies.size() == 2 && copy && copy->start == 90 * kUsPerSecond && copy->groupId != 0 && copy->groupId != m_project->timeline().findClip(video)->groupId,
               "duplicate");
            // The media: organize (and its checks).
            const QJsonObject org = content(call(QStringLiteral("media"), {{QStringLiteral("action"), QStringLiteral("organize")}, {QStringLiteral("media_id"), media},
                                                                           {QStringLiteral("folder"), QStringLiteral("AI/Test")}, {QStringLiteral("rating"), 4}}));
            ok(org[QStringLiteral("folder")].toString() == QLatin1String("AI/Test") && org[QStringLiteral("rating")].toInt() == 4
                   && failed(call(QStringLiteral("media"), {{QStringLiteral("action"), QStringLiteral("remove")}, {QStringLiteral("media_id"), media}})),
               "media");
            call(QStringLiteral("media"), {{QStringLiteral("action"), QStringLiteral("organize")}, {QStringLiteral("media_id"), media}, {QStringLiteral("folder"), QString()},
                                           {QStringLiteral("rating"), 0}});
            // Presets: list, apply one onto the event, save the chain, remove it.
            const QJsonArray presets = content(call(QStringLiteral("presets"), {{QStringLiteral("action"), QStringLiteral("list")}}))[QStringLiteral("presets")].toArray();
            QString chainPreset;
            for (const QJsonValue v : presets)
                if (chainPreset.isEmpty() && v.toObject()[QStringLiteral("chain")].toBool())
                    chainPreset = v.toObject()[QStringLiteral("id")].toString();
            const QJsonObject added = content(call(QStringLiteral("presets"), {{QStringLiteral("action"), QStringLiteral("apply")}, {QStringLiteral("preset"), chainPreset},
                                                                               {QStringLiteral("target"), QStringLiteral("event")}, {QStringLiteral("id"), video}}));
            const QJsonObject savedPreset = content(call(QStringLiteral("presets"), {{QStringLiteral("action"), QStringLiteral("save")}, {QStringLiteral("name"), QStringLiteral("GUI Test Chain")},
                                                                                     {QStringLiteral("target"), QStringLiteral("event")}, {QStringLiteral("id"), video}}));
            const bool removed = !failed(call(QStringLiteral("presets"), {{QStringLiteral("action"), QStringLiteral("remove")},
                                                                          {QStringLiteral("preset"), savedPreset[QStringLiteral("preset")].toString()}}));
            ok(presets.size() > 20 && !added[QStringLiteral("effect_ids")].toArray().isEmpty() && savedPreset[QStringLiteral("preset")].toString().startsWith(QLatin1String("u:"))
                   && removed, "presets");
            // Normalizing, and a frame saved.
            const QJsonObject norm = content(call(QStringLiteral("normalize_audio"), {{QStringLiteral("event_ids"), QJsonArray{audio}}}));
            ok(norm[QStringLiteral("normalized")].toBool(), "normalize");
            const QString still = work.filePath(QStringLiteral("ai-frame.png"));
            call(QStringLiteral("get_frame"), {{QStringLiteral("time"), 61}, {QStringLiteral("save_to"), still}});
            ok(QImage(still).width() == m_project->settings().width, "save_to");
            QFile::remove(still);
            // Stabilize (analysing the file), Track Motion (a masked effect followed), Beat Montage (on markers).
            const QJsonObject stab = content(call(QStringLiteral("stabilize"), {{QStringLiteral("event_id"), video}, {QStringLiteral("smoothing"), 1.5}}));
            ok(stab[QStringLiteral("smoothing")].toDouble() == 1.5 && m_project->timeline().findClip(video)->stabilize == 1.5, "stabilize");
            call(QStringLiteral("stabilize"), {{QStringLiteral("event_id"), video}, {QStringLiteral("smoothing"), 0}});
            call(QStringLiteral("envelope"), {{QStringLiteral("kind"), QStringLiteral("velocity")}, {QStringLiteral("event_id"), video}, {QStringLiteral("clear"), true}});
            const int fx = content(call(QStringLiteral("add_effect"), {{QStringLiteral("target"), QStringLiteral("event")}, {QStringLiteral("id"), video},
                                                                       {QStringLiteral("effect"), QStringLiteral("pixelate")},
                                                                       {QStringLiteral("params"), QJsonObject{{QStringLiteral("mask.shape"), QStringLiteral("Ellipse")},
                                                                                                              {QStringLiteral("mask.w"), 0.3}, {QStringLiteral("mask.h"), 0.3}}}}))
                               [QStringLiteral("effect_id")].toInt();
            const QJsonObject tracked = content(call(QStringLiteral("track_motion"), {{QStringLiteral("event_id"), video}, {QStringLiteral("effect_id"), fx}, {QStringLiteral("from"), 60}}));
            ok(tracked[QStringLiteral("keyframes")].toInt() > 5, "track_motion");
            for (double t : {60.0, 61.0, 62.0, 63.0})
                call(QStringLiteral("add_marker"), {{QStringLiteral("time"), t}});
            const QJsonObject montage = content(call(QStringLiteral("beat_montage"), {{QStringLiteral("every"), 1}, {QStringLiteral("from"), 59.5}, {QStringLiteral("to"), 63.5}}));
            ok(montage[QStringLiteral("event_ids")].toArray().size() == 3, "beat_montage");
            // The YouTube clip: its checks (no download here).
            ok(failed(call(QStringLiteral("youtube_clip"), {{QStringLiteral("url"), QStringLiteral("not a link")}})), "youtube_clip");
            check(broken.isEmpty(), QStringLiteral("AI control: envelopes, groups, track pan and blend, duplicating, media, presets, normalize, "
                                                   "saving a frame, stabilize, track motion, beat montage and the YouTube clip %1")
                                        .arg(broken.join(QStringLiteral(", "))));
            m_project->setTimelineLive(before);
        }

        int status = 0;
        const QJsonObject init =
            QJsonDocument::fromJson(
                http(
                    "POST",
                    R"({"jsonrpc":"2.0","id":0,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})",
                    {}, &status))
                .object();
        int notifyStatus = 0, getStatus = 0, foreignStatus = 0, listStatus = 0;
        http("POST", R"({"jsonrpc":"2.0","method":"notifications/initialized"})", {}, &notifyStatus);
        http("GET", {}, {}, &getStatus);
        http("POST", R"({"jsonrpc":"2.0","id":5,"method":"ping"})", "Origin: http://evil.example\r\n", &foreignStatus);
        const QJsonArray tools = QJsonDocument::fromJson(http("POST", R"({"jsonrpc":"2.0","id":6,"method":"tools/list"})", {}, &listStatus))
                                     .object()[QStringLiteral("result")]
                                     .toObject()[QStringLiteral("tools")]
                                     .toArray();
        bool schemas = tools.size() >= 25;
        for (const QJsonValue& t : tools)
            schemas = schemas && t.toObject()[QStringLiteral("inputSchema")].toObject()[QStringLiteral("type")] == QStringLiteral("object");
        check(status == 200 && init[QStringLiteral("result")].toObject()[QStringLiteral("protocolVersion")] == QStringLiteral("2025-06-18")
                  && notifyStatus == 202 && getStatus == 405 && foreignStatus == 403 && listStatus == 200 && schemas,
              QStringLiteral("AI control: MCP over HTTP on 127.0.0.1 (initialize, a notification 202, GET 405, a web page's request 403); "
                             "%1 tools listed"
                             " (statuses %2 %3 %4 %5 %6)")
                  .arg(tools.size())
                  .arg(status)
                  .arg(notifyStatus)
                  .arg(getStatus)
                  .arg(foreignStatus)
                  .arg(listStatus));

        {
            // The User Guide through AI control: the page list, a page, nothing outside the guide; the instructions point to it.
            const QJsonArray pages = content(call("user_guide", {}))["pages"].toArray();
            const bool listed = std::any_of(pages.begin(), pages.end(), [](const QJsonValue& v) { return v.toObject()["page"].toString() == "ai-workflow.md"; });
            const QString page = call("user_guide", {{"page", "ai-workflow.md"}})["content"].toArray()[0].toObject()["text"].toString();
            const bool outside = failed(call("user_guide", {{"page", "../CMakeLists.txt"}})) && failed(call("user_guide", {{"page", "../../README.md"}}));
            const QString instructions = init["result"].toObject()["instructions"].toString();
            check(listed && page.startsWith("# Making a whole video") && outside && instructions.contains("ai-workflow.md"),
                  QStringLiteral("AI control: user_guide lists %1 pages and reads ai-workflow.md (%2 characters), nothing outside the guide, "
                                 "and the instructions point to it")
                      .arg(pages.size())
                      .arg(page.size()));
        }
        // A generated event that can't use the track asked for says where it went (and that indices moved).
        {
            const int undoIndex = m_project->undoStack()->index();
            const auto first = content(call("add_text", {{"preset", "title"}, {"text", "A"}, {"at", 900}, {"length", 30}, {"track", 0}}));
            const auto inside = content(call("add_text", {{"preset", "title"}, {"text", "B"}, {"at", 905}, {"length", 20}, {"track", first["track"].toInt()}}));
            QJsonObject scene{{"version", 2}, {"layers", QJsonArray{}}};
            const auto graphic = content(call("graphics_overlay", {{"action", "create"}, {"scene", scene}, {"at", 950}, {"length", 2}, {"track", 0}}));
            int ti = -1;
            m_project->timeline().findClip(inside["event_id"].toInt(), &ti);
            check(first.contains("track") && inside["track"].toInt() == ti && ti != first["track"].toInt() + 1 && graphic.contains("track"),
                  QStringLiteral("add_text / graphics_overlay report the track they used (%1, %2 at %3)")
                      .arg(QString::fromUtf8(QJsonDocument(first).toJson(QJsonDocument::Compact)),
                           QString::fromUtf8(QJsonDocument(inside).toJson(QJsonDocument::Compact)))
                      .arg(ti));
            while (m_project->undoStack()->index() > undoIndex)
                m_project->undoStack()->undo();
        }

        // Finishing and graphics workflow use the same HTTP surface as external clients.
        {
            const auto original = m_project->timeline();
            const int undoIndex = m_project->undoStack()->index();
            const auto report = content(call("audio_finishing", {{"action", "analyze"}}));
            check(report["ok"].toBool() && report.contains("true_peak_dbtp") && m_project->timeline() == original
                      && failed(call("audio_finishing", {{"action", "apply_gain"}, {"lufs", 0}})),
                  "AI audio finishing measures the final mix without edits and rejects invalid targets");
            QJsonObject shape{{"type", "rect"}, {"id", "shape"}, {"x", .2},           {"y", .2},
                              {"width", .2},    {"height", .2},  {"fill", "#ffffff"}, {"brand_role", "accent"}};
            QJsonObject scene{{"version", 2}, {"layers", QJsonArray{shape}}};
            auto created = call("graphics_overlay", {{"action", "create"}, {"scene", scene}, {"length", 2}});
            int id = content(created)["event_id"].toInt();
            auto arrangement = call(
                "graphics_overlay",
                {{"action", "layout"}, {"event_id", id}, {"layer_ids", QJsonArray{"shape"}}, {"operation", "left"}, {"reference", "safe"}});
            auto arranged = content(arrangement);
            auto locked =
                call("graphics_overlay", {{"action", "lock"}, {"event_id", id}, {"layer_ids", QJsonArray{"shape"}}, {"locked", true}});
            const auto protectedTimeline = m_project->timeline();
            bool refused = failed(call(
                "graphics_overlay", {{"action", "layout"}, {"event_id", id}, {"layer_ids", QJsonArray{"shape"}}, {"operation", "right"}}));
            bool untouched = m_project->timeline() == protectedTimeline;
            call("graphics_overlay", {{"action", "lock"}, {"event_id", id}, {"layer_ids", QJsonArray{"shape"}}, {"locked", false}});
            QJsonObject kit{{"version", 1},           {"name", "Test"},      {"font", "Arial"},         {"primary", "#00ff00"},
                            {"secondary", "#0000ff"}, {"accent", "#ff6600"}, {"foreground", "#ffffff"}, {"background", "#000000"}};
            auto branded = content(call("graphics_overlay", {{"action", "brand"}, {"event_id", id}, {"brand", kit}}));
            bool styled = branded["data"].toObject()["layers"].toArray().first().toObject()["fill"] == "#ff6600";
            {
                // patch and batch: small edits and many steps, each one undo step, answered briefly
                const bool patched = !failed(call("graphics_overlay", {{"action", "patch"}, {"event_id", id}, {"operations", QJsonArray{
                    QJsonObject{{"layer_id", "shape"}, {"property", "fill"}, {"value", "#123456"}}}}}));
                const int before = m_project->undoStack()->index();
                const auto batched = call("graphics_overlay", {{"action", "batch"}, {"event_id", id}, {"operations", QJsonArray{
                    QJsonObject{{"action", "keyframe"}, {"layer_id", "shape"}, {"property", "x"}, {"time", 1}, {"value", 0.5}},
                    QJsonObject{{"action", "move"}, {"layer_id", "shape"}, {"move", "hop"}, {"time", 0.2}},
                    QJsonObject{{"action", "patch"}, {"operations", QJsonArray{QJsonObject{{"layer_id", "$camera"}, {"property", "zoom"},
                                                                                           {"value", 1.2}}}}}}}});
                const QJsonObject sceneData = m_project->timeline().findClip(id)->generator.data;
                const QJsonObject layer = sceneData["layers"].toArray().first().toObject();
                const auto beforeBad = m_project->timeline();
                const bool badRefused = failed(call("graphics_overlay", {{"action", "batch"}, {"event_id", id}, {"operations", QJsonArray{
                    QJsonObject{{"action", "move"}, {"layer_id", "shape"}, {"move", "hop"}, {"time", 0.5}},
                    QJsonObject{{"action", "fly"}, {"layer_id", "shape"}}}}}));
                check(patched && !failed(batched) && layer["fill"] == "#123456" && layer["x"].isObject() && layer["y"].isObject()
                          && sceneData["camera"].toObject()["zoom"].toDouble() == 1.2 && m_project->undoStack()->index() == before + 1
                          && !content(batched).contains("data") && badRefused && m_project->timeline() == beforeBad,
                      "AI graphics patch and batch: edits by path, many steps as one undo step, a bad step changes nothing");
            }
            {
                // dance: moves on the beat of the music under a scene (a click track at 120 bpm)
                const QString clicks = QDir(QDir::tempPath()).filePath(QStringLiteral("mvp-dance-clicks.wav"));
                {
                    const int rate = 48000, seconds = 8;
                    QByteArray pcm(rate * seconds * 4, 0);
                    auto* out = reinterpret_cast<qint16*>(pcm.data());
                    for (int i = 0; i < rate * seconds; ++i) {
                        const int into = i % (rate / 2); // a click every half second
                        const double v = into < 2400 ? std::sin(2 * 3.14159265 * 1000 * i / rate) * std::exp(-into / 400.0) * 0.8 : 0;
                        out[2 * i] = out[2 * i + 1] = qint16(v * 32767);
                    }
                    QFile f(clicks);
                    f.open(QIODevice::WriteOnly);
                    QDataStream d(&f);
                    d.setByteOrder(QDataStream::LittleEndian);
                    f.write("RIFF");
                    d << quint32(36 + pcm.size());
                    f.write("WAVEfmt ");
                    d << quint32(16) << quint16(1) << quint16(2) << quint32(rate) << quint32(rate * 4) << quint16(4) << quint16(16);
                    f.write("data");
                    d << quint32(pcm.size());
                    f.write(pcm);
                }
                const auto imported = call("import_media", {{"paths", QJsonArray{clicks}}});
                const int clickId = content(imported)["media_ids"].toArray().first().toInt(content(imported)["imported"].toArray().first().toObject()["media_id"].toInt());
                const int clickEvent = content(call("add_media_to_timeline", {{"media_id", clickId}, {"at", 300}}))["event_ids"].toArray().first().toInt();
                {
                    // a length past the end of the file: shortened, and said so; with loop: true, the file repeats to fill it
                    const auto tooLong = content(call("edit_event", {{"event_id", clickEvent}, {"length", 20}}));
                    const auto looped = content(call("edit_event", {{"event_id", clickEvent}, {"length", 20}, {"loop", true}}));
                    check(tooLong.contains("warning") && tooLong["length"].toDouble() < 8.1 && !looped.contains("warning")
                              && std::abs(looped["length"].toDouble() - 20) < 0.05,
                          "AI edit_event warns when a file is shorter than the length asked for, and loops it on request");
                    call("edit_event", {{"event_id", clickEvent}, {"length", 8}, {"loop", false}});
                }
                const QJsonObject dancer{{"type", "rect"}, {"id", "dancer"}, {"x", .4}, {"y", .4}, {"width", .2}, {"height", .2}, {"fill", "#ffffff"}};
                const int danceId = content(call("graphics_overlay", {{"action", "create"}, {"scene", QJsonObject{{"version", 2}, {"layers", QJsonArray{dancer}}}},
                                                                      {"at", 300}, {"length", 8}}))["event_id"].toInt();
                const auto danced = content(call("graphics_overlay", {{"action", "dance"}, {"event_id", danceId}, {"dancers", QJsonArray{"dancer"}}}));
                const QJsonObject moved = m_project->timeline().findClip(danceId)
                                              ? m_project->timeline().findClip(danceId)->generator.data["layers"].toArray().first().toObject()
                                              : QJsonObject{};
                if (std::abs(danced["bpm"].toDouble() - 120) > 6 || danced["moves"].toInt() < 8)
                    note("dance reply: " + QString::fromUtf8(QJsonDocument(danced).toJson(QJsonDocument::Compact)) + " import: "
                         + QString::fromUtf8(QJsonDocument(imported).toJson(QJsonDocument::Compact)));
                check(std::abs(danced["bpm"].toDouble() - 120) <= 6 && danced["moves"].toInt() >= 8 && (moved["y"].isObject() || moved["rotation"].isObject()),
                      "AI graphics dance: moves land on the beats of the music under the scene");
                // (the file stays: the project still lists it, and a missing file would stop a later test at the relink dialog)
            }
            if (!id || failed(arrangement) || failed(locked) || !styled)
                note("Workflow replies: "
                     + QString::fromUtf8(
                         QJsonDocument(QJsonObject{{"create", created}, {"layout", arrangement}, {"lock", locked}, {"brand", branded}})
                             .toJson(QJsonDocument::Compact)));
            m_project->undoStack()->setIndex(undoIndex);
            check(id > 0 && !failed(locked) && refused && untouched && styled
                      && std::abs(arranged["data"].toObject()["layers"].toArray().first().toObject()["x"].toDouble() - .05) < .00001
                      && m_project->timeline() == original,
                  "AI graphics workflow aligns, locks, refuses protected edits, applies brands and undoes exactly");
        }
        // Edit through the tools.
        const QJsonObject project = content(call(QStringLiteral("get_project"), {}));
        const int mediaId = project[QStringLiteral("media")].toArray().first().toObject()[QStringLiteral("media_id")].toInt();
        const QJsonArray placed =
            content(call(QStringLiteral("add_media_to_timeline"),
                      {{QStringLiteral("media_id"), mediaId}, {QStringLiteral("at"), 30}}))[QStringLiteral("event_ids")]
                .toArray();
        const int eventId = placed.isEmpty() ? 0 : placed.first().toInt();
        const Clip* placedClip = m_project->timeline().findClip(eventId);
        const bool atThirty = placedClip && placedClip->start == 30 * kUsPerSecond;
        auto frame = [&](double time) {
            const QJsonObject r = call(QStringLiteral("get_frame"), {{QStringLiteral("time"), time}, {QStringLiteral("width"), 320}});
            QImage img;
            for (const QJsonValue& c : r[QStringLiteral("content")].toArray())
                if (c.toObject()[QStringLiteral("type")] == QStringLiteral("image"))
                    img.loadFromData(QByteArray::fromBase64(c.toObject()[QStringLiteral("data")].toString().toLatin1()));
            return img;
        };
        const QImage plainFrame = frame(31);
        const QJsonObject added = content(call(QStringLiteral("add_effect"),
                                            {{QStringLiteral("target"), QStringLiteral("event")},
                                             {QStringLiteral("id"), eventId},
                                             {QStringLiteral("effect"), QStringLiteral("invert")}}));
        const int effectId = added[QStringLiteral("effect_id")].toInt();
        const QImage invertedFrame = frame(31);
        double change = 0;
        if (plainFrame.size() == invertedFrame.size() && !plainFrame.isNull()) {
            int64_t sum = 0, n = 0;
            for (int y = 0; y < plainFrame.height(); y += 4)
                for (int x = 0; x < plainFrame.width(); x += 4, ++n)
                    sum += std::abs(qGray(plainFrame.pixel(x, y)) - qGray(invertedFrame.pixel(x, y)));
            change = n ? double(sum) / double(n) : 0;
        }
        const QJsonObject masked =
            content(call(QStringLiteral("set_effect"),
                      {{QStringLiteral("effect_id"), effectId},
                       {QStringLiteral("params"), QJsonObject{{QStringLiteral("mask.shape"), QStringLiteral("Ellipse")}}}}));
        const bool maskSet = masked[QStringLiteral("params")].toObject()[QStringLiteral("mask.shape")] == QStringLiteral("Ellipse");
        // A choice by its short name ("Off" for "Off (everywhere)") and a freehand point, as add_effect's description has them.
        const QJsonObject freehand = content(call(QStringLiteral("set_effect"),
                                                  {{QStringLiteral("effect_id"), effectId},
                                                   {QStringLiteral("params"), QJsonObject{{QStringLiteral("mask.shape"), QStringLiteral("Freehand")},
                                                                                          {QStringLiteral("mask.p0x"), 0.25}}}}));
        const QJsonObject off = content(call(QStringLiteral("set_effect"),
                                             {{QStringLiteral("effect_id"), effectId},
                                              {QStringLiteral("params"), QJsonObject{{QStringLiteral("mask.shape"), QStringLiteral("Off")}}}}));
        check(freehand[QStringLiteral("params")].toObject()[QStringLiteral("mask.p0x")].toDouble() == 0.25
                  && off[QStringLiteral("params")].toObject()[QStringLiteral("mask.shape")].toString().startsWith(QLatin1String("Off")),
              QStringLiteral("AI control sets freehand mask points and takes a choice by its short name"));
        call(QStringLiteral("set_effect"),
             {{QStringLiteral("effect_id"), effectId}, {QStringLiteral("params"), QJsonObject{{QStringLiteral("mask.shape"), QStringLiteral("Ellipse")}}}});
        const bool audioRefused = failed(call(QStringLiteral("add_effect"),
                                              {{QStringLiteral("target"), QStringLiteral("event")},
                                               {QStringLiteral("id"), eventId},
                                               {QStringLiteral("effect"), audioEffects().front().id}}));
        const bool badSetting = failed(
            call(QStringLiteral("set_effect"),
                 {{QStringLiteral("effect_id"), effectId}, {QStringLiteral("params"), QJsonObject{{QStringLiteral("nonsense"), 1}}}}));
        check(plainFrame.size() == QSize(320, 180) && atThirty && effectId > 0 && change > 40 && maskSet && audioRefused && badSetting,
              QStringLiteral("AI tools: place media at 30 s, add Invert to it (the frame it sees changes by %1), give it an ellipse mask; "
                             "an audio effect on a picture and an unknown setting are refused")
                  .arg(change, 0, 'f', 1));
        const QJsonArray pieces =
            content(call(QStringLiteral("split"),
                      {{QStringLiteral("time"), 32}, {QStringLiteral("event_ids"), QJsonArray{eventId}}}))[QStringLiteral("new_event_ids")]
                .toArray();
        call(QStringLiteral("add_marker"), {{QStringLiteral("time"), 33}, {QStringLiteral("name"), QStringLiteral("from AI")}});
        const bool marker = std::any_of(m_project->timeline().markers.begin(), m_project->timeline().markers.end(),
                                        [](const Marker& m) { return m.name == QLatin1String("from AI"); });
        {
            // remove_markers takes it away again (then undone, so the steps below are as they were).
            const int removed = content(call(QStringLiteral("remove_markers"), {{QStringLiteral("from"), 32.5}, {QStringLiteral("to"), 33.5}}))
                                    [QStringLiteral("removed")].toInt();
            const bool gone = std::none_of(m_project->timeline().markers.begin(), m_project->timeline().markers.end(),
                                           [](const Marker& m) { return m.name == QLatin1String("from AI"); });
            call(QStringLiteral("undo"), {});
            check(removed == 1 && gone, QStringLiteral("AI control: remove_markers takes markers out of a stretch of time (%1 removed)").arg(removed));
        }
        const QJsonObject blur =
            content(call(QStringLiteral("list_effects"),
                      {{QStringLiteral("kind"), QStringLiteral("video")}, {QStringLiteral("search"), QStringLiteral("blur")}}));
        const bool listed = blur[QStringLiteral("count")].toInt() > 0
            && !blur[QStringLiteral("effects")].toArray().first().toObject()[QStringLiteral("params")].toArray().isEmpty();
        const QString undoneText =
            content(call(QStringLiteral("undo"), {{QStringLiteral("steps"), 2}}))[QStringLiteral("undone")].toArray().first().toString();
        const bool undone = !std::any_of(m_project->timeline().markers.begin(), m_project->timeline().markers.end(),
                                         [](const Marker& m) { return m.name == QLatin1String("from AI"); });
        check(pieces.size() == 2 /* the event and its grouped sound */ && marker && listed && undone && undoneText.startsWith(QStringLiteral("AI: ")),
              QStringLiteral("AI tools: split an event, add a marker, search the effects; undo takes back \"%1\"").arg(undoneText));
        // Captions in and out, the plugin list.
        {
            const QString srt = work.filePath(QStringLiteral("ai_in.srt")), back = work.filePath(QStringLiteral("ai_out.vtt"));
            QFile f(srt);
            f.open(QIODevice::WriteOnly);
            f.write("1\n00:00:01,000 --> 00:00:02,500\nFrom the AI\n\n2\n00:00:03,000 --> 00:00:04,000\nSecond line\n");
            f.close();
            const int in = content(call(
                QStringLiteral("captions"),
                {{QStringLiteral("action"), QStringLiteral("import")}, {QStringLiteral("path"), srt}}))[QStringLiteral("captions")]
                               .toInt();
            const int out = content(call(
                QStringLiteral("captions"),
                {{QStringLiteral("action"), QStringLiteral("export")}, {QStringLiteral("path"), back}}))[QStringLiteral("captions")]
                                .toInt();
            QFile r(back);
            const bool vtt = r.open(QIODevice::ReadOnly) && r.readAll().startsWith("WEBVTT");
            const bool noModel =
                failed(call(QStringLiteral("captions"),
                            {{QStringLiteral("action"), QStringLiteral("auto")}, {QStringLiteral("model"), QStringLiteral("nonsense")}}));
            const QJsonObject plugins = content(call(QStringLiteral("list_vst3_plugins"), {}));
            const int preview =
                content(call(QStringLiteral("live_switcher"),
                          {{QStringLiteral("action"), QStringLiteral("preview")}, {QStringLiteral("input"), 3}}))[QStringLiteral("preview")]
                    .toInt();
            m_project->undoStack()->undo(); // (the imported captions)
            check(in == 2 && out == 2 && vtt && noModel && plugins.contains(QStringLiteral("plugins")) && preview == 3,
                  QStringLiteral("AI tools: captions imported from SRT and exported as WebVTT, an unknown model refused, the switcher's "
                                 "preview set, %1 VST3 plugin(s) listed")
                      .arg(plugins[QStringLiteral("plugins")].toArray().size()));
        }
        // Render a second in the background.
        const QString out = work.filePath(QStringLiteral("ai_render.mp4"));
        QFile::remove(out);
        const bool started = !failed(
            call(QStringLiteral("render"), {{QStringLiteral("path"), out}, {QStringLiteral("from"), 30}, {QStringLiteral("to"), 31}}));
        QJsonObject rs;
        QElapsedTimer rt;
        rt.start();
        do {
            wait(200);
            rs = content(call(QStringLiteral("render_status"), {}));
        } while (!rs[QStringLiteral("done")].toBool() && rt.elapsed() < 120000);
        const MediaInfo rendered = probeMedia(out);
        const bool noOverwrite = failed(call(QStringLiteral("render"), {{QStringLiteral("path"), out}}));
        check(started && rs[QStringLiteral("ok")].toBool() && rendered.ok()
                  && std::abs(double(rendered.duration) / kUsPerSecond - 1.0) < 0.2 && noOverwrite,
              QStringLiteral("AI render: a second to %1 in the background (%2 s, %3); it won't overwrite a file unless asked")
                  .arg(QFileInfo(out).fileName())
                  .arg(double(rendered.duration) / kUsPerSecond, 0, 'f', 2)
                  .arg(rs[QStringLiteral("template")].toString()));
        {
            // "MP3" for a .mp3 is the MP3 template (the DivX AVI, with MP3 sound, comes first in the list).
            // (into a folder that isn't there yet: the render makes it)
            QDir(work.filePath(QStringLiteral("new render folder"))).removeRecursively();
            const QString mp3 = work.filePath(QStringLiteral("new render folder/ai_render.mp3"));
            const QString chosen = content(call(QStringLiteral("render"), {{QStringLiteral("path"), mp3}, {QStringLiteral("preset"), QStringLiteral("MP3")},
                                                                         {QStringLiteral("from"), 30}, {QStringLiteral("to"), 31}}))[QStringLiteral("template")]
                                       .toString();
            QElapsedTimer mt;
            mt.start();
            while (!content(call(QStringLiteral("render_status"), {}))[QStringLiteral("done")].toBool() && mt.elapsed() < 60000)
                wait(200);
            check(chosen.startsWith(QStringLiteral("MP3")) && probeMedia(mp3).ok(),
                  QStringLiteral("AI render: \"MP3\" to a .mp3 in a new folder uses an MP3 template (%1)").arg(chosen));
        }
        {
            // add_track with no index: pictures on top, sound at the bottom (all went to the bottom).
            const int undoIndex = m_project->undoStack()->index();
            const int video = content(call(QStringLiteral("add_track"), {{QStringLiteral("type"), QStringLiteral("video")}}))[QStringLiteral("index")].toInt(-1);
            const int audio = content(call(QStringLiteral("add_track"), {{QStringLiteral("type"), QStringLiteral("audio")}}))[QStringLiteral("index")].toInt(-1);
            const auto& tracks = m_project->timeline().tracks;
            check(video == 0 && tracks.front().type == TrackType::Video && audio == int(tracks.size()) - 1 && tracks.back().type == TrackType::Audio,
                  QStringLiteral("AI add_track: a video track on top (%1), an audio track at the bottom (%2 of %3)").arg(video).arg(audio).arg(tracks.size()));
            while (m_project->undoStack()->index() > undoIndex)
                m_project->undoStack()->undo();
        }
        {
            // Lip sync: a scene bound to a whole voice track, under a binding name; a missing track refused.
            const Timeline tl = m_project->timeline();
            int index = -1;
            const Clip* line = nullptr;
            for (int i = 0; i < int(tl.tracks.size()) && !line; ++i)
                for (const Clip& c : tl.tracks[size_t(i)].clips)
                    if (tl.tracks[size_t(i)].type == TrackType::Audio && !c.isGenerated() && !c.isSequence() && !line) {
                        line = &c;
                        index = i;
                    }
            const int undoIndex = m_project->undoStack()->index();
            const int scene = line ? content(call(QStringLiteral("graphics_overlay"), {{QStringLiteral("action"), QStringLiteral("create")},
                                                                                       {QStringLiteral("preset"), QStringLiteral("motion_audio")},
                                                                                       {QStringLiteral("at"), double(line->start) / kUsPerSecond},
                                                                                       {QStringLiteral("length"), 4}}))[QStringLiteral("event_id")].toInt()
                                   : 0;
            const auto bound = call(QStringLiteral("graphics_overlay"), {{QStringLiteral("action"), QStringLiteral("bind_audio")}, {QStringLiteral("event_id"), scene},
                                                                         {QStringLiteral("source_track"), index}, {QStringLiteral("binding"), QStringLiteral("mouth")}});
            const Clip* g = m_project->timeline().findClip(scene);
            double loudest = 0;
            for (const auto v : g ? g->generator.data[QStringLiteral("audio")][QStringLiteral("mouth")][QStringLiteral("left")].toArray() : QJsonArray())
                loudest = std::max(loudest, v.toDouble());
            const bool missing = failed(call(QStringLiteral("graphics_overlay"), {{QStringLiteral("action"), QStringLiteral("bind_audio")}, {QStringLiteral("event_id"), scene},
                                                                                  {QStringLiteral("source_track"), 99}}));
            check(scene && !failed(bound) && loudest > 0 && missing,
                  QStringLiteral("AI lip sync: a scene bound to voice track %1 (loudest %2), a missing track refused").arg(index).arg(loudest));
            while (m_project->undoStack()->index() > undoIndex)
                m_project->undoStack()->undo();
        }
        {
            // A generator's settings through AI control: on adding and on editing; an unknown one refused.
            const int undoIndex = m_project->undoStack()->index();
            const auto added = content(call(QStringLiteral("add_text"), {{QStringLiteral("preset"), QStringLiteral("title")}, {QStringLiteral("text"), QStringLiteral("Hi")},
                                                                        {QStringLiteral("at"), 700}, {QStringLiteral("length"), 2},
                                                                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("size"), 3.5}, {QStringLiteral("color"), QStringLiteral("#ff0000")}}}}));
            const int gid = added[QStringLiteral("event_id")].toInt();
            const bool edited = !failed(call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), gid}, {QStringLiteral("params"), QJsonObject{{QStringLiteral("bold"), true}}}}));
            const bool unknown = failed(call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), gid}, {QStringLiteral("params"), QJsonObject{{QStringLiteral("nope"), 1}}}}));
            const Clip* g = m_project->timeline().findClip(gid);
            const bool stored = g && std::abs(g->generator.params.at(QStringLiteral("size")).value - 3.5) < 1e-6
                && g->generator.params.at(QStringLiteral("color")).value == double(0xff0000) && g->generator.params.at(QStringLiteral("bold")).value == 1.0;
            check(gid && edited && unknown && stored, QStringLiteral("AI generator settings: add_text and edit_event params (size, colour, bold), an unknown one refused"));
            while (m_project->undoStack()->index() > undoIndex)
                m_project->undoStack()->undo();
        }
        {
            // AI edits keep a picture and its sound together, on whole frames and inside the source; a locked track
            // refuses them; an unknown dice style and a filter chain FFmpeg can't use are refused.
            const int undoIndex = m_project->undoStack()->index();
            int vid = 0, aud = 0;
            for (const Track& t : m_project->timeline().tracks)
                for (const Clip& c : t.clips)
                    if (!vid && c.groupId && t.type == TrackType::Video)
                        for (const Track& u : m_project->timeline().tracks)
                            for (const Clip& d : u.clips)
                                if (u.type == TrackType::Audio && d.groupId == c.groupId)
                                    vid = c.id, aud = d.id;
            bool together = false, framed = false, split = false, inside = false, lockRefused = false, diceRefused = false, filterChecked = false;
            if (vid && aud) {
                const Clip* before = m_project->timeline().findClip(vid);
                const TimeUs lengthBefore = before ? before->length : 0;
                call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), vid}, {QStringLiteral("speed"), 2}});
                const Clip* doubled = m_project->timeline().findClip(vid);
                const bool follows = doubled && std::llabs(doubled->length - lengthBefore / 2) <= m_project->settings().fps.frameDuration();
                call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), vid}, {QStringLiteral("length"), 1.2345}});
                const Clip* v = m_project->timeline().findClip(vid);
                const Clip* s = m_project->timeline().findClip(aud);
                together = follows && v && s && v->length == s->length && v->speed == s->speed && v->start == s->start;
                framed = v && v->length == m_project->settings().fps.snap(v->length);
                const double middle = v ? double(v->start + v->length / 2) / kUsPerSecond : 0;
                split = content(call(QStringLiteral("split"), {{QStringLiteral("time"), middle}, {QStringLiteral("event_ids"), QJsonArray{vid}}}))
                            [QStringLiteral("new_event_ids")].toArray().size() == 2; // the sound too
                call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), vid}, {QStringLiteral("source_in"), 1e6}});
                v = m_project->timeline().findClip(vid);
                const auto media = v ? m_project->mediaItem(v->mediaId) : nullptr;
                inside = v && media && v->sourceIn < media->info.duration && v->length > 0;
                int ti = -1;
                m_project->timeline().findClip(vid, &ti);
                call(QStringLiteral("edit_track"), {{QStringLiteral("track"), ti}, {QStringLiteral("locked"), true}});
                lockRefused = failed(call(QStringLiteral("edit_event"), {{QStringLiteral("event_id"), vid}, {QStringLiteral("start"), 0.5}}))
                    && failed(call(QStringLiteral("add_effect"), {{QStringLiteral("target"), QStringLiteral("event")}, {QStringLiteral("id"), vid},
                                                                 {QStringLiteral("effect"), QStringLiteral("invert")}}));
                call(QStringLiteral("edit_track"), {{QStringLiteral("track"), ti}, {QStringLiteral("locked"), false}});
                diceRefused = failed(call(QStringLiteral("directors_dice"), {{QStringLiteral("event_ids"), QJsonArray{vid}}, {QStringLiteral("style"), QStringLiteral("chaos")}}));
                auto filter = [&](const QString& chain) {
                    return !failed(call(QStringLiteral("add_effect"), {{QStringLiteral("target"), QStringLiteral("event")}, {QStringLiteral("id"), aud},
                                                                      {QStringLiteral("effect"), QStringLiteral("a_ffmpeg")},
                                                                      {QStringLiteral("params"), QJsonObject{{QStringLiteral("filter"), chain}}}}));
                };
                filterChecked = !filter(QStringLiteral("not a filter")) && filter(QStringLiteral("highpass=f=80"));
            }
            check(vid && aud && together && framed && split && inside && lockRefused && diceRefused && filterChecked,
                  QStringLiteral("AI edits: speed and length reach the grouped sound (%1), on whole frames (%2), Split cuts both (%3), source_in stays "
                                 "inside the file (%4), a locked track refuses edits (%5), unknown dice style (%6) and a bad FFmpeg filter (%7) refused")
                      .arg(together).arg(framed).arg(split).arg(inside).arg(lockRefused).arg(diceRefused).arg(filterChecked));
            while (m_project->undoStack()->index() > undoIndex)
                m_project->undoStack()->undo();
        }
        {
        // The Deliver tab through AI control.
        const auto setup = content(call("deliver", {{"action", "outputs"}, {"pack", "audio"}, {"ticked", QJsonArray{"MP3 320"}},
                                                    {"trim", QJsonObject{{"MP3 320", 1}}}, {"folder", work.filePath("deliver-ai")}}));
        bool trimmed = false, onlyMp3 = true;
        for (const auto& v : setup["outputs"].toArray()) {
            const QJsonObject o = v.toObject();
            trimmed = trimmed || (o["template"].toString() == "MP3 - 320 kb/s" && o["trimmed_to"].toDouble() == 1 && o["ticked"].toBool());
            onlyMp3 = onlyMp3 && (o["ticked"].toBool() == (o["template"].toString() == "MP3 - 320 kb/s"));
        }
        const auto packed = content(call("deliver", {{"action", "pack"}, {"title", "AI title"}, {"description", "{title} - {length}"},
                                                     {"tags", "a, b"}, {"thumbnail_time", 1}, {"save", true}}));
        const auto report = content(call("deliver", {{"action", "check"}}));
        const bool refused = failed(call("deliver", {{"action", "outputs"}, {"add", QJsonArray{"no such template anywhere"}}}))
            && failed(call("deliver", {{"action", "pack"}, {"thumbnail_time", 1e9}}));
        const bool rendered = !failed(call("deliver", {{"action", "render"}}));
        for (int i = 0; i < 1200 && (renderQueue()->busy() || renderQueue()->waiting()); ++i)
            wait(50);
        QString aiMp3;
        QStringList files;
        for (const auto& v : setup["outputs"].toArray()) {
            files << v.toObject()["file"].toString();
            if (v.toObject()["template"].toString() == "MP3 - 320 kb/s")
                aiMp3 = v.toObject()["file"].toString();
        }
        const bool distinct = QSet<QString>(files.begin(), files.end()).size() == files.size();
        const MediaInfo aiFile = probeMedia(aiMp3);
        const auto past = content(call("deliver", {{"action", "history"}}));
        check(trimmed && onlyMp3 && distinct && packed["description_filled_in"].toString().startsWith("AI title - ") && packed.contains("thumbnail_time")
                  && QFileInfo::exists(QDir(work.filePath("deliver-ai")).filePath(QFileInfo(aiMp3).completeBaseName().section(" - ", 0, -2) + " - thumbnail.jpg"))
                  && report.contains("ready") && report["checks"].toArray().size() >= 5 && refused && rendered && aiFile.ok()
                  && std::abs(double(aiFile.duration) / kUsPerSecond - 1.0) < 0.3 && !past["renders"].toArray().isEmpty(),
              QStringLiteral("AI deliver tool: a pack, ticks and a 1 s trim, the publish pack written, checks read, one output rendered (%1 s) "
                             "into History")
                  .arg(double(aiFile.duration) / kUsPerSecond, 0, 'f', 2));
            renderQueue()->close();
        }
        // The Live tab.
        call(QStringLiteral("set_pad"),
             {{QStringLiteral("pad"), 3},
              {QStringLiteral("name"), QStringLiteral("AI pad")},
              {QStringLiteral("effects"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("invert")}}}}});
        call(QStringLiteral("live_action"), {{QStringLiteral("action"), QStringLiteral("pad")}, {QStringLiteral("number"), 3}});
        call(QStringLiteral("set_tempo"), {{QStringLiteral("bpm"), 128}});
        const QJsonObject liveNow = content(call(QStringLiteral("live_status"), {}));
        const bool pad = m_project->timeline().livePads[2].name == QLatin1String("AI pad")
            && m_project->timeline().livePads[2].effects.size() == 1 && liveNow[QStringLiteral("active_pad")].toInt() == 3
            && std::abs(liveNow[QStringLiteral("bpm")].toDouble() - 128) < 0.01 && liveNow[QStringLiteral("tempo_locked")].toBool();
        call(QStringLiteral("live_action"), {{QStringLiteral("action"), QStringLiteral("pad")}, {QStringLiteral("number"), 3}}); // off
        check(pad, QStringLiteral("AI Live tools: fill pad 3 with Invert, switch it on, lock the tempo at 128 BPM, and read it all back"));

        // Garbage in: every tool (but those that read or write files, go online or
        // touch the Live tab's devices) called with arguments of the wrong kind or far
        // out of range. Each must answer, and the timeline must stay sound. Then all of
        // it is undone.
        {
            const int undoBefore = m_project->undoStack()->index();
            const ProjectSettings settingsBefore = m_project->settings();
            int listStatus = 0;
            const QJsonArray tools =
                QJsonDocument::fromJson(http("POST", R"({"jsonrpc":"2.0","id":77,"method":"tools/list"})", {}, &listStatus))
                    .object()[QStringLiteral("result")]
                    .toObject()[QStringLiteral("tools")]
                    .toArray();
            const QRegularExpression skip(
                QStringLiteral("render|export|save|open|new_project|import|caption|youtube|stream|record|live|switcher|pad|tempo|deliver"));
            std::mt19937 rng(5);
            auto garbage = [&]() -> QJsonValue {
                switch (rng() % 14) {
                case 0: return -1;
                case 1: return 0;
                case 2: return 1e18;
                case 3: return -1e18;
                case 4: return 1e308;
                case 5: return 2147483647;
                case 6: return QStringLiteral("garbage");
                case 7: return QString(5000, QLatin1Char('x'));
                case 8: return QJsonArray{-1, QStringLiteral("x"), 1e308};
                case 9: return QJsonObject{{QStringLiteral("a"), 1}};
                case 10: return QJsonValue(QJsonValue::Null);
                case 11: return true;
                case 12: return 0.5;
                default: return int(rng() % 200);
                }
            };
            int calls = 0, unanswered = 0, fuzzed = 0;
            QString broken, lastCall;
            for (const QJsonValue& tv : tools) {
                const QJsonObject tool = tv.toObject();
                const QString name = tool[QStringLiteral("name")].toString();
                const QJsonObject props = tool[QStringLiteral("inputSchema")].toObject()[QStringLiteral("properties")].toObject();
                if (skip.match(name).hasMatch() || props.contains(QStringLiteral("path")) || props.contains(QStringLiteral("url")))
                    continue;
                ++fuzzed;
                for (int k = 0; k < 25 && broken.isEmpty(); ++k) {
                    QJsonObject args;
                    for (auto it = props.begin(); it != props.end(); ++it)
                        if (rng() % 3)
                            args[it.key()] = garbage();
                    if (rng() % 10 == 0)
                        args[QStringLiteral("not_a_parameter")] = garbage();
                    lastCall =
                        QStringLiteral("%1 %2").arg(name, QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)).left(200));
                    note(QStringLiteral("    AI fuzz: ") + lastCall);
                    int st = 0;
                    const QJsonObject msg{
                        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                        {QStringLiteral("id"), nextId++},
                        {QStringLiteral("method"), QStringLiteral("tools/call")},
                        {QStringLiteral("params"), QJsonObject{{QStringLiteral("name"), name}, {QStringLiteral("arguments"), args}}}};
                    const QJsonObject r =
                        QJsonDocument::fromJson(http("POST", QJsonDocument(msg).toJson(QJsonDocument::Compact), {}, &st)).object();
                    ++calls;
                    if (st != 200 || (!r.contains(QStringLiteral("result")) && !r.contains(QStringLiteral("error"))))
                        ++unanswered;
                    if (const QString p = timelineProblem(m_project->timeline()); !p.isEmpty())
                        broken = QStringLiteral("after %1: %2").arg(lastCall, p);
                }
            }
            // Not JSON at all, the wrong JSON-RPC, a method that doesn't exist.
            int odd = 0;
            for (const QByteArray& body :
                 {QByteArray("not json"), QByteArray("[]"), QByteArray("{}"), QByteArray(R"({"jsonrpc":"2.0","id":1})"),
                  QByteArray(R"({"jsonrpc":"2.0","id":1,"method":"no/such"})"),
                  QByteArray(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":"x"})"),
                  QByteArray(R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":42}})"),
                  QByteArray(R"({"jsonrpc":"2.0","id":{"a":[1]},"method":"ping"})"), QByteArray(1000000, '{')}) {
                int st = 0;
                http("POST", body, {}, &st);
                odd += st != 0;
            }
            m_project->undoStack()->setIndex(undoBefore);
            m_project->setSettings(settingsBefore);
            check(fuzzed >= 15 && unanswered == 0 && broken.isEmpty() && odd == 9,
                  QStringLiteral("AI control under garbage: %1 tools called %2 times with wrong kinds and extreme values, all answered%3; "
                                 "%4/9 malformed requests answered")
                      .arg(fuzzed)
                      .arg(calls)
                      .arg(broken.isEmpty() ? QStringLiteral(", the timeline stays sound")
                                            : QStringLiteral(", but the timeline broke ") + broken)
                      .arg(odd));
        }

        // The stdio bridge (what Claude Desktop starts): to this Mutant Video Producer, and when it can't be reached.
        auto bridge = [&](quint16 bridgePort, const QList<QByteArray>& lines) {
            QProcess p;
            p.start(QCoreApplication::applicationFilePath(),
                    {QStringLiteral("--mcp-stdio"), QStringLiteral("--port"), QString::number(bridgePort)});
            p.waitForStarted(5000);
            for (const QByteArray& l : lines)
                p.write(l + '\n');
            QList<QJsonObject> answers;
            QElapsedTimer bt;
            bt.start();
            while (answers.size() < lines.size() && bt.elapsed() < 30000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 10); // (the server is on this thread)
                p.waitForReadyRead(5);
                while (p.canReadLine())
                    answers << QJsonDocument::fromJson(p.readLine()).object();
            }
            p.closeWriteChannel();
            if (!p.waitForFinished(3000))
                p.kill();
            return answers;
        };
        const QList<QByteArray> lines{R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})",
                                      R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
                                      R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_project","arguments":{}}})"};
        const QList<QJsonObject> through = bridge(port, lines);
        const bool bridged = through.size() == 2
            && through[0][QStringLiteral("result")].toObject()[QStringLiteral("protocolVersion")] == QStringLiteral("2025-03-26")
            && through[1][QStringLiteral("result")]
                    .toObject()[QStringLiteral("structuredContent")]
                    .toObject()[QStringLiteral("width")]
                    .toInt()
                == m_project->settings().width;
        const QList<QJsonObject> offline = bridge(47898, lines);
        const bool explained = offline.size() == 2 && offline[0].contains(QStringLiteral("result"))
            && offline[1][QStringLiteral("result")].toObject()[QStringLiteral("isError")].toBool();
        setAiControl(false);
        int offStatus = 0;
        http("POST", R"({"jsonrpc":"2.0","id":9,"method":"ping"})", {}, &offStatus);
        check(bridged && explained && offStatus == 0,
              QStringLiteral("MutantVideoProducer --mcp-stdio passes messages to the running Mutant Video Producer (%1 answers) and, when "
                             "it isn't reachable, still "
                             "connects and says why; with AI Control off nothing answers")
                  .arg(through.size()));
        QSettings().remove(QStringLiteral("mcp/port"));
    }

    {
        // Help > About: "registered" to a prank caller from the list.
        QString text;
        QTimer::singleShot(200, this, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                text = box->text();
                box->accept();
            }
        });
        about();
        const QString name = text.section(QStringLiteral("Registered to: <i>"), 1).section(QStringLiteral("</i>"), 0, 0);
        check(std::find_if(std::begin(kRegisteredTo), std::end(kRegisteredTo), [&](const char* n) { return QString::fromUtf8(n).toHtmlEscaped() == name; })
                  != std::end(kRegisteredTo) && std::size(kRegisteredTo) > 100,
              QStringLiteral("About is registered to %1 (one of %2 names)").arg(name).arg(std::size(kRegisteredTo)));
    }
    {
        // The master volume line on the transport meter: dragged to half way (-30 dB) in one undo step; double-click back to 0 dB.
        auto mouse = [&](QEvent::Type type, double x) {
            const QPointF pos(x, m_meter->height() / 2.0);
            QMouseEvent ev(type, pos, m_meter->mapToGlobal(pos), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), Qt::NoModifier);
            QApplication::sendEvent(m_meter, &ev);
        };
        const double left = 12, right = m_meter->width() - 12; // (the meter's scale, between the L/R labels and the clip lights)
        const int undoBefore = m_project->undoStack()->index();
        const float before = m_project->timeline().audioMasterLevel;
        mouse(QEvent::MouseButtonPress, right - 2);
        mouse(QEvent::MouseMove, right - 40);
        mouse(QEvent::MouseMove, (left + right) / 2);
        const bool live = std::abs(m_project->timeline().audioMasterLevel - std::pow(10.0f, -1.5f)) < 1e-3f
                          && m_project->undoStack()->index() == undoBefore;
        mouse(QEvent::MouseButtonRelease, (left + right) / 2);
        const float dragged = m_project->timeline().audioMasterLevel;
        const bool oneStep = m_project->undoStack()->index() == undoBefore + 1;
        mouse(QEvent::MouseButtonDblClick, (left + right) / 2);
        const float reset = m_project->timeline().audioMasterLevel;
        m_project->undoStack()->undo();
        m_project->undoStack()->undo();
        check(before == 1.0f && live && oneStep && std::abs(dragged - std::pow(10.0f, -1.5f)) < 1e-3f && reset == 1.0f
                  && m_project->timeline().audioMasterLevel == 1.0f,
              QStringLiteral("master volume line: dragged to %1 live, one undo step, double-click back to 0 dB")
                  .arg(levels::volumeText(dragged)));
    }

    // ---- Properties: transitions and generated media in the bottom section, handles on the preview ----
    {
        auto mouse = [&](QEvent::Type type, QPointF pos) {
            QMouseEvent ev(type, pos, m_preview->mapToGlobal(pos), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), Qt::NoModifier);
            QApplication::sendEvent(m_preview, &ev);
        };
        auto drag = [&](QPointF from, QPointF to) {
            mouse(QEvent::MouseButtonPress, from);
            mouse(QEvent::MouseMove, (from + to) / 2);
            mouse(QEvent::MouseMove, to);
            mouse(QEvent::MouseButtonRelease, to);
            wait(50);
        };
        auto paramAt = [&](int clipId, const QString& id, bool generator, bool in) {
            const Clip* c = m_project->timeline().findClip(clipId);
            if (!c)
                return -999.0;
            const ParamMap& pm = generator ? c->generator.params : (in ? c->transitionIn.params : c->transitionOut.params);
            const auto it = pm.find(id);
            return it == pm.end() ? -999.0 : it->second.value;
        };
        stopPlayback(false);
        // A title at the start: selecting it shows its properties, with a position handle and a size on the wheel.
        Timeline tl = m_project->timeline();
        const int title = ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("title")), 0, -1, m_project->settings().fps);
        m_project->applyEdit(tl, QStringLiteral("Test title"));
        seekTo(kUsPerSecond);
        m_timeline->selectClips({title});
        wait(100);
        const bool titleShown = m_bottomTabs->currentWidget() == m_fxEditor && m_fxEditor->target().kind == FxTarget::Kind::Generator
            && m_fxEditor->target().id == title;
        const QStringList titleHandles = m_fxEditor->handles();
        const QRectF pic = m_preview->pictureRect();
        const double x0 =
            paramAt(title, QStringLiteral("x"), true, false) == -999.0 ? 0.5 : paramAt(title, QStringLiteral("x"), true, false);
        drag(pic.center(), pic.center() + QPointF(pic.width() / 10, 0));
        const double x1 = paramAt(title, QStringLiteral("x"), true, false);
        const double size0 = paramAt(title, QStringLiteral("size"), true, false);
        QWheelEvent wheel(pic.center(), m_preview->mapToGlobal(pic.center()), QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(m_preview, &wheel);
        wait(800); // (the edit is committed after a pause)
        const double size1 = paramAt(title, QStringLiteral("size"), true, false);
        check(titleShown && titleHandles.value(0) == QStringLiteral("x") && titleHandles.value(3) == QStringLiteral("size")
                  && std::abs(x1 - x0 - 0.1) < 0.01 && std::abs(size1 / size0 - 1.21) < 0.02,
              QStringLiteral("selecting a title opens its Properties below (not a window); dragging on the preview moves it "
                             "(x %1 -> %2), two wheel notches size it (%3 -> %4)")
                  .arg(x0, 0, 'f', 3)
                  .arg(x1, 0, 'f', 3)
                  .arg(size0, 0, 'f', 2)
                  .arg(size1, 0, 'f', 2));
        // A media event: back to its effects.
        int mediaClip = 0;
        for (const Track& t : m_project->timeline().tracks)
            for (const Clip& c : t.clips)
                if (!c.isGenerated() && t.type == TrackType::Video && !mediaClip)
                    mediaClip = c.id;
        m_timeline->selectClips({mediaClip});
        wait(50);
        const bool effectsBack = m_bottomTabs->currentWidget() == m_devices && m_fxEditor->handles().isEmpty();
        // An iris into it: its Properties, the centre dragged on the preview.
        tl = m_project->timeline();
        if (Clip* c = tl.findClip(mediaClip)) {
            c->fadeIn = kUsPerSecond;
            c->transitionIn = makeTransition(*findTransition(QStringLiteral("iris")));
        }
        m_project->applyEdit(tl, QStringLiteral("Test iris"));
        m_timeline->selectClips({mediaClip});
        m_fxEditor->setTarget({FxTarget::Kind::TransitionIn, mediaClip}); // (what clicking its label does)
        wait(50);
        const bool irisShown = m_bottomTabs->currentWidget() == m_fxEditor && m_fxEditor->handles().value(0) == QStringLiteral("centerX");
        drag(pic.center(), pic.center() + QPointF(-pic.width() / 5, pic.height() / 10));
        const double cx = paramAt(mediaClip, QStringLiteral("centerX"), false, true),
                     cy = paramAt(mediaClip, QStringLiteral("centerY"), false, true);
        // A wipe: its angle on a knob.
        tl = m_project->timeline();
        if (Clip* c = tl.findClip(mediaClip))
            c->transitionIn = makeTransition(*findTransition(QStringLiteral("wipe")));
        m_project->applyEdit(tl, QStringLiteral("Test wipe"));
        m_fxEditor->setTarget({FxTarget::Kind::TransitionIn, mediaClip});
        wait(50);
        const bool angleHandle = m_fxEditor->handles().value(2) == QStringLiteral("angle");
        const QPointF knob = pic.center() + QPointF(std::min(pic.width(), pic.height()) * 0.18, 0); // angle 0: to the right
        drag(knob, pic.center() + QPointF(0, std::min(pic.width(), pic.height()) * 0.18)); // straight down: 90 degrees
        const double angle = paramAt(mediaClip, QStringLiteral("angle"), false, true);
        check(effectsBack && irisShown && std::abs(cx - 0.3) < 0.02 && std::abs(cy - 0.6) < 0.02 && angleHandle && std::abs(angle - 90) < 2,
              QStringLiteral("a media event shows its Effects again; an iris's Properties: its centre dragged on the preview "
                             "(%1, %2); a wipe's angle turned with the knob (%3 degrees)")
                  .arg(cx, 0, 'f', 2)
                  .arg(cy, 0, 'f', 2)
                  .arg(angle, 0, 'f', 1));
        // Pan/Crop on the preview: drag pans, the wheel zooms.
        m_fxEditor->setTarget({FxTarget::Kind::PanCrop, mediaClip});
        wait(50);
        drag(pic.center(), pic.center() + QPointF(pic.width() / 10, 0));
        QApplication::sendEvent(m_preview, &wheel);
        wait(800);
        const Clip* pc = m_project->timeline().findClip(mediaClip);
        const double pcx = pc && pc->panCrop.count(QStringLiteral("centerX")) ? pc->panCrop.at(QStringLiteral("centerX")).value : 0.5;
        const double pz = pc && pc->panCrop.count(QStringLiteral("zoom")) ? pc->panCrop.at(QStringLiteral("zoom")).value : 1;
        check(m_bottomTabs->currentWidget() == m_fxEditor && pcx < 0.45 && std::abs(pz - 1.21) < 0.02,
              QStringLiteral("Pan/Crop on the preview: a drag to the right pans (centre %1), two wheel notches zoom (%2x)")
                  .arg(pcx, 0, 'f', 3)
                  .arg(pz, 0, 'f', 2));
        // Remove Transition puts the panel away.
        m_fxEditor->setTarget({FxTarget::Kind::TransitionIn, mediaClip});
        for (QPushButton* b : m_fxEditor->findChildren<QPushButton*>())
            if (b->text() == tr("Remove Transition"))
                b->click();
        wait(50);
        check(m_bottomTabs->currentWidget() == m_devices && m_fxEditor->handles().isEmpty() && m_preview->overlay() == nullptr,
              QStringLiteral("Remove Transition goes back to the Effects tab and takes the handles off the preview"));
        while (m_project->undoStack()->canUndo() && m_project->undoStack()->undoText() != QStringLiteral("Test title"))
            m_project->undoStack()->undo();
        m_project->undoStack()->undo();
    }

    // ---- Video scopes beside the preview ----
    {
        m_scopesAction->setChecked(true);
        seekTo(kUsPerSecond);
        refreshPreview();
        wait(400);
        const bool visible = m_scopes->isVisible() && m_scopes->width() > 100;
        QStringList drawn;
        for (int k = 0; k < 4; ++k) {
            m_scopes->setKind(scopes::Kind(k));
            wait(150);
            const QImage img = m_scopes->scopeImage();
            // Something more than the graticule (a black picture's scope).
            QImage black(64, 36, QImage::Format_RGB32);
            black.fill(Qt::black);
            const QImage empty = scopes::render(black, scopes::Kind(k), img.size());
            int lit = 0;
            for (int y = 0; y < img.height(); y += 2)
                for (int x = 0; x < img.width(); x += 2)
                    lit += std::abs(qGray(img.pixel(x, y)) - qGray(empty.pixel(x, y))) > 25;
            if (lit > 30)
                drawn << scopes::names()[k];
        }
        m_scopesAction->setChecked(false);
        check(visible && drawn.size() == 4 && !m_scopes->isVisible(),
              QStringLiteral("Scopes shows beside the preview and draws its picture as %1; hidden again")
                  .arg(drawn.join(QStringLiteral(", "))));
    }

    // ---- Audio Mixer, scrubbing, frame export, device panels ----
    {
        showMixer();
        int audioTracks = 0;
        for (const Track& t : m_project->timeline().tracks)
            audioTracks += t.type == TrackType::Audio;
        const bool strips = m_mixer->stripCount() == audioTracks && audioTracks > 0;
        m_mixer->findChild<QSlider*>()->setValue(500);
        float level = -1;
        for (const Track& t : m_project->timeline().tracks)
            if (t.type == TrackType::Audio) {
                level = t.level;
                break;
            }
        check(strips && level > 0.0f && level < 0.5f,
              QStringLiteral("View > Audio Mixer has a strip per audio track; its fader sets the track volume (%1 dB)")
                  .arg(20 * std::log10(std::max(1e-6f, level)), 0, 'f', 1));
        m_project->undoStack()->undo();
        m_mixer->hide();
        m_engine->scrubAudio(kUsPerSecond); // no audio device here: must simply not get in the way
        seekTo(kUsPerSecond);
        QApplication::clipboard()->clear();
        exportFrame(true);
        const QImage still = QApplication::clipboard()->image();
        check(still.size() == QSize(m_project->settings().width, m_project->settings().height),
              QStringLiteral("Copy Frame puts the frame at the cursor on the clipboard at full size (%1x%2)")
                  .arg(still.width())
                  .arg(still.height()));
        const int before = m_devices->deviceBodyHeight();
        m_mainSplit->setSizes({100, 200, 600});
        QApplication::processEvents();
        wait(50);
        const int after = m_devices->deviceBodyHeight();
        check(after > before && after >= 140 + 74,
              QStringLiteral("a taller device panel stacks more controls per column (device height %1 -> %2 px)").arg(before).arg(after));
        m_mainSplit->setSizes({330, 360, 270});
    }

    // ---- Autosave ----
    const QString projectPath = work.filePath(QStringLiteral("project.mvp"));
    const QStringList untitledOutputs = m_deliver->outputs();
    m_deliver->setPackText(std::nullopt, std::nullopt, QStringLiteral("snowbank, deliver test"));
    m_project->save(projectPath, nullptr);
    {
        // The Deliver choices are part of the project: saved in its file (with the outputs chosen while it was untitled,
        // named after it), back when it is opened, and not in a new project.
        m_deliver->refresh();
        const ExportPreset* first = nullptr;
        for (const auto& p : exportPresets())
            if (!m_deliver->outputs().isEmpty() && p.name == m_deliver->outputs().front())
                first = &p;
        Project reopened;
        const bool loaded = reopened.load(projectPath, nullptr);
        const QJsonObject saved = reopened.deliver();
        reopened.clear();
        check(!untitledOutputs.isEmpty() && m_deliver->outputs() == untitledOutputs && first
                  && QFileInfo(m_deliver->outputPath(*first)).fileName().startsWith(QStringLiteral("project - ")) && loaded
                  && saved[QStringLiteral("tags")].toString() == QStringLiteral("snowbank, deliver test")
                  && saved[QStringLiteral("outputs")].toVariant().toStringList() == untitledOutputs && reopened.deliver().isEmpty()
                  && !m_project->isModified(),
              QStringLiteral("Deliver choices are saved in the project file (%1, tags \"%2\") and a new project starts without them")
                  .arg(first ? QFileInfo(m_deliver->outputPath(*first)).fileName() : QStringLiteral("none"), saved[QStringLiteral("tags")].toString()));
        m_deliver->setPackText(std::nullopt, std::nullopt, QString());
        m_project->save(projectPath, nullptr);
        // A project from before 0.8.0 (its choices in this computer's settings) brings them in, without asking to save.
        const QJsonObject kept = m_project->deliver();
        const QString old = QStringLiteral("deliver/")
                            + QString::fromLatin1(QCryptographicHash::hash(QDir::cleanPath(projectPath).toLower().toUtf8(), QCryptographicHash::Md5).toHex());
        QSettings().setValue(old + QStringLiteral("/title"), QStringLiteral("An older pack"));
        QSettings().setValue(old + QStringLiteral("/outputs"), QStringList{QStringLiteral("YouTube 720p")});
        m_project->setDeliver({}, false);
        m_deliver->refresh();
        const auto* title = m_deliver->findChild<QLineEdit*>(QStringLiteral("deliverTitle"));
        check(title && title->text() == QStringLiteral("An older pack") && m_deliver->outputs() == QStringList{QStringLiteral("YouTube 720p")}
                  && !m_project->isModified(),
              QStringLiteral("an older project's Deliver choices come in from this computer's settings (%1)").arg(title ? title->text() : QString()));
        QSettings().remove(old);
        m_project->setDeliver(kept, false);
        m_deliver->refresh();
    }
    m_timeline->addMarker(kUsPerSecond, QStringLiteral("autosaved"));
    autosave();
    QString autosaveOf;
    {
        QFile f(m_autosavePath);
        if (f.open(QIODevice::ReadOnly))
            autosaveOf = QJsonDocument::fromJson(f.readAll()).object()[QStringLiteral("autosaveOf")].toString();
    }
    check(QFileInfo::exists(m_autosavePath) && autosaveOf == projectPath && m_project->filePath() == projectPath && m_project->isModified(),
          QStringLiteral("autosave writes a copy that remembers project.mvp, without saving the project itself"));
    save();
    check(!QFileInfo::exists(m_autosavePath), QStringLiteral("saving removes the autosaved copy"));
    check(QSettings().value(Profiles::instance().settingsKey(QStringLiteral("recent/projects"))).toStringList().value(0) == projectPath,
          QStringLiteral("saved projects go to the top of Open Recent"));
    rebuildRecentMenu();
    bool listed = false;
    for (QAction* a : m_recentMenu->actions())
        listed |= a->text().contains(QStringLiteral("project.mvp"));
    check(listed, QStringLiteral("File > Open Recent lists project.mvp"));

    // ---- Recovery ----
    // A copy left by a crashed session (no live lock) is offered; one whose
    // session is still running (lock held) is not.
    m_timeline->addMarker(2 * kUsPerSecond, QStringLiteral("unsaved"));
    const QString crashed = QDir(autosaveDir()).filePath(QStringLiteral("session-1-1.mvp"));
    const QString running = QDir(autosaveDir()).filePath(QStringLiteral("session-2-2.mvp"));
    m_project->saveBackup(crashed, nullptr);
    m_project->saveBackup(running, nullptr);
    QLockFile alive(QDir(autosaveDir()).filePath(QStringLiteral("session-2-2.lock")));
    alive.tryLock(0);
    autosave();
    const QStringList orphans = orphanedAutosaves();
    check(orphans == QStringList{crashed},
          QStringLiteral("after a crash its autosave is found; a running session's and our own are not (%1 found)").arg(orphans.size()));
    const auto markersBefore = m_project->timeline().markers;
    m_project->clear(); // as after a restart
    const bool recovered = recoverFrom(crashed);
    check(recovered && m_project->filePath() == projectPath && m_project->isModified() && m_project->timeline().markers == markersBefore
              && !QFileInfo::exists(crashed),
          QStringLiteral("Recover opens the copy as project.mvp, unsaved, with the unsaved marker"));
    alive.unlock();
    QFile::remove(running);

    // ---- Relink ----
    const QDir before(work.filePath(QStringLiteral("media-a"))), after(work.filePath(QStringLiteral("media-b")));
    QDir().mkpath(before.absolutePath());
    QFile::copy(mediaPath, before.filePath(QStringLiteral("one.mp4")));
    QFile::copy(mediaPath, before.filePath(QStringLiteral("two.mp4")));
    m_project->clear();
    importFiles({before.filePath(QStringLiteral("one.mp4")), before.filePath(QStringLiteral("two.mp4"))}, TimeUs(0));
    const QString relinkProject = work.filePath(QStringLiteral("relink.mvp"));
    m_project->save(relinkProject, nullptr);
    m_project->clear();
    bool moved = false;
    for (int i = 0; i < 10 && !(moved = QDir().rename(before.absolutePath(), after.absolutePath())); ++i)
        wait(100);
    if (!moved) {
        // Windows can't move a folder while the renderers' cached decoders still have its
        // files open: copy them instead and point the saved project at a folder that's gone.
        QDir().mkpath(after.absolutePath());
        QFile::copy(before.filePath(QStringLiteral("one.mp4")), after.filePath(QStringLiteral("one.mp4")));
        QFile::copy(before.filePath(QStringLiteral("two.mp4")), after.filePath(QStringLiteral("two.mp4")));
        QFile f(relinkProject);
        if (f.open(QIODevice::ReadOnly)) {
            QByteArray json = f.readAll();
            f.close();
            json.replace(before.absolutePath().toUtf8(), work.filePath(QStringLiteral("media-gone")).toUtf8());
            json.replace("\"media-a/", "\"media-gone/"); // the paths relative to the project

            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
                f.write(json);
        }
    }
    QStringList missing;
    m_project->load(relinkProject, nullptr, &missing);
    const bool offline = missing.size() == 2 && m_project->offlineMedia().size() == 2;
    if (!offline) {
        check(false, QStringLiteral("moved media goes offline (%1 missing)").arg(missing.size()));
        return failures;
    }
    {
        RelinkDialog dlg(m_project);
        const int one = m_project->offlineMedia().front();
        const int n = dlg.relink(one, after.filePath(QStringLiteral("one.mp4")));
        check(offline && n == 2 && m_project->offlineMedia().empty() && dlg.missingCount() == 0,
              QStringLiteral("moved media: locating one file finds the other next to it (%1 relinked)").arg(n));
    }
    m_project->load(relinkProject, nullptr, &missing);
    {
        RelinkDialog dlg(m_project);
        const int n = dlg.searchFolder(moved ? work.absolutePath() : after.absolutePath()); // (the copied-from folder has the same names)
        bool online = true;
        for (const auto& [id, m] : m_project->media())
            online &= m->info.ok() && m->path.startsWith(after.absolutePath());
        int clips = 0;
        for (const Track& t : m_project->timeline().tracks)
            clips += int(t.clips.size());
        check(n == 2 && online && clips == 4,
              QStringLiteral("Search Folder finds both moved files by name; their events are back (%1 relinked)").arg(n));
    }
    // ---- Create Clip from YouTube: a downloaded file goes in the YouTube folder, the part asked for placed ----
    {
        const QString copy = work.filePath(QStringLiteral("downloaded.mp4"));
        QFile::copy(mediaPath, copy);
        seekTo(200 * kUsPerSecond);
        const int id = placeDownloadedClip(copy, kUsPerSecond, 2 * kUsPerSecond);
        const Clip* placed = nullptr;
        for (const Track& t : m_project->timeline().tracks)
            for (const Clip& c : t.clips)
                if (c.mediaId == id && c.start == 200 * kUsPerSecond)
                    placed = &c;
        auto m = m_project->mediaItem(id);
        auto* youtubeTabs = findChild<QTabWidget*>(QStringLiteral("dockTabs"));
        int liveTab = -1, youtubeTab = -1;
        for (int i = 0; i < youtubeTabs->count(); ++i) {
            if (youtubeTabs->widget(i) == m_liveClips)
                liveTab = i;
            if (youtubeTabs->widget(i)->isAncestorOf(m_youtubeBin))
                youtubeTab = i;
        }
        check(m && m->origin == QLatin1String("youtube") && m_youtubeBin->count() == 1 && youtubeTab == liveTab + 1
                  && youtubeTabs->currentIndex() == youtubeTab && m_youtubeBin->item(0)->data(Qt::UserRole).toInt() == id,
              QStringLiteral("downloaded clips appear in From YouTube next to From Live, and the tab opens after import"));
        m_project->setMediaDetails(id, QStringLiteral("Other Folder"), m->rating, m->markIn, m->markOut);
        const QString savedYoutube = work.filePath(QStringLiteral("youtube-origin.mvp"));
        QString youtubeError;
        Project restoredYoutube;
        check(m_youtubeBin->count() == 1 && m_project->save(savedYoutube, &youtubeError)
                  && restoredYoutube.load(savedYoutube, &youtubeError) && restoredYoutube.mediaItem(id)->origin == QLatin1String("youtube"),
              QStringLiteral("YouTube origin survives folder changes and saving/reopening the project"));
        m_project->setMediaDetails(id, QStringLiteral("YouTube"), m->rating, m->markIn, m->markOut);
        m = m_project->mediaItem(id);
        check(id && m && m->folder == QStringLiteral("YouTube") && placed && placed->sourceIn == kUsPerSecond
                  && placed->length == kUsPerSecond,
              QStringLiteral("Create Clip from YouTube: the file goes in Project Media's YouTube folder and the part asked for (1-2 s) "
                             "is placed at the cursor"));
        m_project->undoStack()->undo();
    }
    // ---- Project Media: ratings (keys), folders, filters, in / out marks; History; shortcuts ----
    {
        const int id = m_project->media().begin()->first;
        m_bin->clearSelection();
        for (int i = 0; i < m_bin->count(); ++i)
            if (m_bin->item(i)->data(Qt::UserRole).toInt() == id)
                m_bin->item(i)->setSelected(true);
        QKeyEvent four(QEvent::KeyPress, Qt::Key_4, Qt::NoModifier, QStringLiteral("4"));
        QApplication::sendEvent(m_bin, &four);
        const bool rated = m_project->mediaItem(id)->rating == 4 && m_bin->selectedMediaIds() == QList<int>{id};
        auto m = m_project->mediaItem(id);
        m_project->setMediaDetails(id, QStringLiteral("Footage/Day 1"), m->rating, kUsPerSecond, 2 * kUsPerSecond);
        auto visible = [&] {
            int n = 0;
            for (int i = 0; i < m_bin->count(); ++i)
                n += !m_bin->item(i)->isHidden();
            return n;
        };
        m_bin->setFilter(QString(), QStringLiteral("Footage"), 5); // five stars only: hidden
        const int fiveStars = visible();
        m_bin->setFilter(QStringLiteral("day 1"), QStringLiteral("Footage"), 4); // the folder found by a search, inside "Footage"
        const int found = visible();
        m_bin->setFilter({}, {}, 0);
        Timeline tl = m_project->timeline();
        const std::vector<int> placed = ops::insertMedia(tl, *m_project->mediaItem(id), 100 * kUsPerSecond, -1, m_project->settings().fps);
        const Clip* c = placed.empty() ? nullptr : tl.findClip(placed.front());
        const bool marked = c && c->sourceIn == kUsPerSecond && c->length == kUsPerSecond;
        const QString saved = work.filePath(QStringLiteral("bin.mvp"));
        Project reloaded;
        QString saveError;
        const bool kept = m_project->save(saved, &saveError) && reloaded.load(saved, &saveError)
            && reloaded.mediaItem(id)->folder == QStringLiteral("Footage/Day 1") && reloaded.mediaItem(id)->rating == 4
            && reloaded.mediaItem(id)->markOut == 2 * kUsPerSecond;
        m_project->setMediaDetails(id, {}, 0, 0, 0);
        check(rated && fiveStars == 0 && found == 1 && marked && kept,
              QStringLiteral("Project Media: 4 rates a file, the rating and folder filters and the search find it, its in / out "
                             "marks the part placed, and all of it is saved"));
        auto* history = findChild<QUndoView*>(QStringLiteral("history"));
        const QList<QAction*> commands = ShortcutsDialog::commands(this);
        QAction* split = nullptr;
        for (QAction* a : commands)
            if (a->text().remove(QLatin1Char('&')).startsWith(QStringLiteral("Split")) && !a->shortcut().isEmpty())
                split = a;
        const QKeySequence before = split ? split->shortcut() : QKeySequence();
        if (split) {
            QSettings().setValue(QStringLiteral("shortcuts/")
                                     + split->text().remove(QLatin1Char('&')).remove(QStringLiteral("...")).trimmed(),
                                 QStringLiteral("Ctrl+Alt+K"));
            ShortcutsDialog::applySaved(commands);
        }
        const bool changed = split && split->shortcut() == QKeySequence(QStringLiteral("Ctrl+Alt+K"));
        QSettings().remove(QStringLiteral("shortcuts"));
        ShortcutsDialog::applySaved(commands);
        check(history && history->stack() == m_project->undoStack() && commands.size() > 60 && changed && split->shortcut() == before,
              QStringLiteral("the History tab lists the undo steps; %1 commands have editable shortcuts (%2 moved to Ctrl+Alt+K and back)")
                  .arg(commands.size())
                  .arg(split ? split->text() : QStringLiteral("no Split")));
    }
    // ---- Captions: added on a Captions track, as subtitles or kinetic text ----
    {
        const int tracksBefore = int(m_project->timeline().tracks.size());
        addCaptions({{kUsPerSecond, 3 * kUsPerSecond, QStringLiteral("Hello there")},
                     {4 * kUsPerSecond, 5 * kUsPerSecond, QStringLiteral("one two three")}},
                    2 * kUsPerSecond, false);
        const Track& top = m_project->timeline().tracks.front();
        const bool subs = int(m_project->timeline().tracks.size()) == tracksBefore + 1 && top.name == tr("Captions")
            && top.clips.size() == 2 && top.clips[0].start == 3 * kUsPerSecond && top.clips[0].length == 2 * kUsPerSecond
            && top.clips[0].generator.type == QLatin1String("text")
            && top.clips[1].generator.params.at(QStringLiteral("text")).text == QStringLiteral("one two three");
        m_project->undoStack()->undo();
        addCaptions({{0, 2 * kUsPerSecond, QStringLiteral("one two three four")}}, 0, true);
        const Clip& k = m_project->timeline().tracks.front().clips.front();
        const bool kinetic =
            k.generator.type == QLatin1String("kinetic_text") && std::abs(k.generator.params.at(QStringLiteral("rate")).value - 2.5) < 0.01;
        m_project->undoStack()->undo();
        check(subs && kinetic && int(m_project->timeline().tracks.size()) == tracksBefore,
              QStringLiteral(
                  "captions go on a new Captions track on top, as subtitles at their times or kinetic text paced to them (and undo)"));
    }
    // ---- A VST3 plugin on a track (when one is installed) ----
    if (const QStringList plugins = vst3InstalledPlugins(); vst3Available() && !plugins.isEmpty()) {
        int audioTrack = -1;
        for (int i = 0; i < int(m_project->timeline().tracks.size()) && audioTrack < 0; ++i)
            if (m_project->timeline().tracks[size_t(i)].type == TrackType::Audio)
                audioTrack = i;
        if (audioTrack < 0) {
            Timeline tl = m_project->timeline();
            audioTrack = ops::addTrack(tl, TrackType::Audio);
            m_project->applyEdit(tl, QStringLiteral("audio track"));
        }
        const QString path = plugins.front();
        Timeline tl = m_project->timeline();
        EffectInstance fx = makeEffectOfType(QStringLiteral("a_vst3"), tl.allocId());
        fx.params[QStringLiteral("plugin")].text = path;
        tl.tracks[size_t(audioTrack)].effects.push_back(fx);
        m_project->applyEdit(tl, QStringLiteral("vst3"));
        m_devices->setTarget({FxTarget::Kind::Track, tl.tracks[size_t(audioTrack)].id});
        wait(300);
        bool named = false, stateHidden = true;
        for (QLabel* l : m_devices->findChildren<QLabel*>()) {
            named |= l->text() == tr("VST3: %1").arg(QFileInfo(path).completeBaseName());
            stateHidden &= l->text() != QStringLiteral("Plugin state");
        }
        check(named && stateHidden,
              QStringLiteral("a VST3 Plugin device is named after its plugin (%1) and keeps its state out of sight")
                  .arg(QFileInfo(path).fileName()));
        m_project->undoStack()->undo();
    }

    // ---- Help: the user guide, a heading link, the Tutorials menu, an example project (last: it replaces the project) ----
    {
        showHelp(QStringLiteral("README.md"));
        const bool guide = m_helpBrowser && m_helpBrowser->window()->isVisible()
            && m_helpBrowser->toPlainText().contains(QStringLiteral("Mutant Video Producer User Guide"));
        showHelp(QStringLiteral("editing.md#auto-ripple"));
        wait(100);
        const bool scrolled = m_helpBrowser && m_helpBrowser->verticalScrollBar()->value() > 0;
        int tutorials = 0;
        for (QAction* a : menuBar()->actions())
            if (a->menu() && a->text() == tr("&Help"))
                for (QAction* b : a->menu()->actions())
                    if (b->menu() && b->text() == tr("&Tutorials"))
                        for (QAction* t : b->menu()->actions())
                            tutorials += !t->isSeparator() && t->text() != tr("All Tutorials");
        if (m_helpBrowser)
            m_helpBrowser->window()->close();
        save(); // (nothing to ask about before the example opens)
        // A picture of the first file, to tell it from the example's file with the same media id.
        auto picture = [&] {
            const auto m = m_project->media().begin()->second;
            const int stream = m->info.video.empty() ? -1 : m->info.video.front().index;
            QImage img;
            for (int i = 0; i < 100 && img.isNull() && stream >= 0; ++i) {
                img = m_cache->thumbnail(m, stream, kUsPerSecond);
                if (img.isNull())
                    wait(50);
            }
            return std::pair{m->id, img};
        };
        const auto [beforeId, before] = picture();
        const QString into = work.filePath(QStringLiteral("examples"));
        openExample(QStringLiteral("effects-and-keyframes"), into);
        wait(300);
        const auto [afterId, after] = picture();
        check(
            beforeId == afterId && !before.isNull() && !after.isNull() && before != after,
            QStringLiteral("thumbnails follow the file, not the media id: another project's file %1 with the same id shows its own picture")
                .arg(m_project->media().begin()->second->displayName()));
        openExample(QStringLiteral("first-edit"), into);
        bool titles = false;
        for (const Track& t : m_project->timeline().tracks)
            titles |= t.name == QLatin1String("Titles");
        bool online = !m_project->media().empty();
        for (const auto& [id, m] : m_project->media())
            online &= m->info.ok() && m->path.startsWith(into);
        check(guide && scrolled && tutorials >= 16 && m_project->filePath() == QDir(into).filePath(QStringLiteral("first-edit.mvp"))
                  && titles && online,
              QStringLiteral("Help > User Guide shows the guide and scrolls to a heading's link; Help > Tutorials lists %1 tutorials; "
                             "an example project opens from a copy, its media with it")
                  .arg(tutorials));
    }

    // ---- A crash report is offered once; files from another start of Mutant Video Producer open here ----
    {
        crash::markSeen();
        wait(1100); // (file times have a second's resolution here and there)
        QDir().mkpath(crash::folder());
        const QString fake = QDir(crash::folder()).filePath(QStringLiteral("crash-test.txt"));
        if (QFile f(fake); f.open(QIODevice::WriteOnly))
            f.write("MvpEditorTest 0.0 on a test\nCrashed: an exception\nTime: now\nException: 0xC0000005 (access violation) at "
                    "MutantVideoProducer.exe+0x1234\n");
        const QStringList found = crash::newReports();
        const QString summary = found.isEmpty() ? QString() : crash::summary(found.front());
        crash::markSeen();
        const bool once = crash::newReports().isEmpty();
        QFile::remove(fake);
        check(found.size() == 1 && summary.contains(QStringLiteral("(access violation) at MutantVideoProducer.exe+0x1234")) && once,
              QStringLiteral("a crash report is found on the next start and summed up (%1), then not offered again").arg(summary));

        save();
        openFromElsewhere({QDir(work.filePath(QStringLiteral("examples"))).filePath(QStringLiteral("first-edit.mvp"))});
        check(m_project->filePath().endsWith(QStringLiteral("first-edit.mvp")),
              QStringLiteral("a project passed on by another start of Mutant Video Producer (a double-click on a .mvp) opens here"));
    }

    // ---- Find a Command (Ctrl+K): typing narrows the list to the command ----
    {
        QString found, menu;
        int listed = 0;
        QTimer::singleShot(300, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            auto* list = dlg->findChild<QTreeWidget*>();
            listed = list ? list->topLevelItemCount() : 0;
            if (auto* search = dlg->findChild<QLineEdit*>())
                search->setText(QStringLiteral("ripple del"));
            if (list && list->currentItem()) {
                found = list->currentItem()->text(0);
                menu = list->currentItem()->text(1);
            }
            dlg->reject();
        });
        findCommand();
        check(listed > 80 && found == QStringLiteral("Ripple Delete") && menu == QStringLiteral("Edit"),
              QStringLiteral("Find a Command lists %1 commands; typing \"ripple del\" picks %2 (%3 menu)").arg(listed).arg(found, menu));
    }

    // ---- Themes: View > Theme switches the palette, the widgets' style sheets and the icons at once ----
    {
        const QString before = QString::fromLatin1(theme::c().id);
        const QIcon playBefore = m_playAction->icon();
        QStringList wrong;
        for (const theme::Colors& t : theme::themes()) {
            theme::apply(*qApp, QString::fromLatin1(t.id));
            if (qApp->palette().color(QPalette::Window) != t.window || !m_previewInfo->styleSheet().contains(t.dim.name())
                || !qApp->styleSheet().contains(t.accent.name()))
                wrong << QString::fromLatin1(t.id);
        }
        const bool iconRedrawn = m_playAction->icon().cacheKey() != playBefore.cacheKey();
        theme::apply(*qApp, before);
        int menuEntries = 0;
        for (QMenu* m : findChildren<QMenu*>(QStringLiteral("themeMenu")))
            menuEntries = int(m->actions().size());
        check(wrong.isEmpty() && iconRedrawn && menuEntries == int(theme::themes().size())
                  && qApp->palette().color(QPalette::Window) == theme::c().window,
              QStringLiteral(
                  "View > Theme: all %1 themes recolour the palette, the widgets' style sheets and the transport icons, and switch back %2")
                  .arg(menuEntries)
                  .arg(wrong.join(QStringLiteral(", "))));
    }

    // ---- Starting out: the update check's versions, File > New's shapes, the welcome window, the system check, context help ----
    {
        check(isNewerVersion(QStringLiteral("v0.1.5"), QStringLiteral("0.1.4"))
                  && isNewerVersion(QStringLiteral("v0.1.10"), QStringLiteral("0.1.9"))
                  && !isNewerVersion(QStringLiteral("v0.1.4"), QStringLiteral("0.1.4"))
                  && !isNewerVersion(QStringLiteral("v0.1.3"), QStringLiteral("0.1.4"))
                  && !isNewerVersion(QStringLiteral("v9.0.0"), QStringLiteral("dev-1234abc"))
                  && !isNewerVersion(QStringLiteral("nightly"), QStringLiteral("0.1.4")),
              QStringLiteral("a newer release is told apart (0.1.10 after 0.1.9; never for a dev build)"));

        // File > New: discard unsaved work once, then make Vertical 9:16 at 60 fps.
        m_timeline->addMarker(0, QStringLiteral("Unsaved new-project test"));
        int savePrompts = 0;
        QTimer answerNewProject;
        connect(&answerNewProject, &QTimer::timeout, this, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++savePrompts;
                if (auto* discard = box->button(QMessageBox::Discard))
                    discard->click();
                return;
            }
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg || dlg->objectName() != QStringLiteral("newProjectDialog"))
                return;
            if (auto* shapes = dlg->findChild<QListWidget*>(QStringLiteral("newProjectShapes")))
                shapes->setCurrentRow(4);
            if (auto* fps = dlg->findChild<QComboBox*>(QStringLiteral("newProjectFps")))
                fps->setCurrentIndex(fps->findData(QStringLiteral("60")));
            dlg->accept();
        });
        answerNewProject.start(50);
        newProjectDialog();
        answerNewProject.stop();
        check(savePrompts == 1,
              QStringLiteral("File > New asks to save unsaved work once, before the New Project dialog (%1 prompts)").arg(savePrompts));
        const ProjectSettings s = m_project->settings();
        check(s.width == 1080 && s.height == 1920 && std::abs(s.fps.toDouble() - 60) < 1e-6 && m_project->media().empty()
                  && !m_project->isModified(),
              QStringLiteral("File > New makes an empty vertical 9:16 project at 60 fps (%1x%2), not marked as changed")
                  .arg(s.width)
                  .arg(s.height));

        // The welcome window, with what this computer can do.
        QString summary;
        int buttons = 0;
        QTimer::singleShot(400, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            if (auto* l = dlg->findChild<QLabel*>(QStringLiteral("welcomeCheck")))
                summary = l->text();
            buttons = int(dlg->findChildren<QPushButton*>().size());
            dlg->reject();
        });
        showWelcome();
        check(buttons >= 8 && (summary.startsWith(QStringLiteral("✓")) || summary.startsWith(QStringLiteral("✗"))),
              QStringLiteral("the welcome window offers starting and learning (%1 buttons) and sums up this computer: %2")
                  .arg(buttons)
                  .arg(summary));
        const std::vector<CheckItem> items = systemCheck();
        bool complete = items.size() >= 9;
        for (const CheckItem& c : items)
            complete &= !c.what.isEmpty() && !c.status.isEmpty() && (c.state != CheckItem::Problem || !c.fix.isEmpty());
        check(complete, QStringLiteral("the system check covers %1 features, with a fix for each problem").arg(items.size()));

        // Shift+F1, and a dialog's Help button (its help window works while the dialog is open).
        const bool pages = help::pageFor(m_timeline) == QLatin1String("editing.md") && help::pageFor(nullptr) == QLatin1String("README.md")
            && help::pageFor(m_devices->findChild<QWidget*>()) == QLatin1String("effects.md#the-device-chain");
        bool dialogHelp = false;
        QTimer::singleShot(300, this, [&] {
            auto* dlg = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dlg)
                return;
            if (auto* box = dlg->findChild<QDialogButtonBox*>())
                if (QPushButton* b = box->button(QDialogButtonBox::Help))
                    b->click();
            auto* window = dlg->findChild<QWidget*>(QStringLiteral("helpWindow"), Qt::FindDirectChildrenOnly);
            auto* browser = window ? window->findChild<QTextBrowser*>() : nullptr;
            dialogHelp = window && window->isVisible() && browser && browser->source().toString().contains(QStringLiteral("options.md"));
            dlg->reject();
        });
        projectProperties();
        check(pages && dialogHelp,
              QStringLiteral("Shift+F1 finds the page of the panel with the focus; a dialog's Help button opens its page in a window "
                             "the dialog doesn't block"));
    }
    {
        const QString root = work.filePath(QStringLiteral("profile-test-") + QUuid::createUuid().toString(QUuid::WithoutBraces));
        QString error;
        const QString homeA = QDir(root).filePath(QStringLiteral("A")), homeB = QDir(root).filePath(QStringLiteral("B"));
        auto& profiles = Profiles::instance();
        check(profiles.saveAndActivate({QStringLiteral("test-a"), QStringLiteral("Profile A"), homeA}, &error)
                  && profiles.folder(QStringLiteral("YouTube")) == QDir(homeA).filePath(QStringLiteral("YouTube"))
                  && liveRecordingsFolder() == QDir(homeA).filePath(QStringLiteral("Live")),
              QStringLiteral("profiles create selectable homes and route downloads and live recordings"));
        m_project->clear();
        QTimer::singleShot(50, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("newProjectName")))
                name->setText(QStringLiteral("Profile Project"));
            dialog->accept();
        });
        newProjectDialog();
        check(m_project->filePath() == QDir(homeA).filePath(QStringLiteral("Projects/Profile Project.mvp"))
                  && QFileInfo::exists(m_project->filePath()),
              QStringLiteral("new projects are named and created inside the active profile home"));
        const QString clipPath = QDir(homeA).filePath(QStringLiteral("YouTube/shared.mp4"));
        QFile::copy(mediaPath, clipPath);
        const int imported = placeDownloadedClip(clipPath, kUsPerSecond, 2 * kUsPerSecond);
        check(imported && profiles.findMedia(clipPath).value("origin").toString() == QLatin1String("youtube"),
              QStringLiteral("profile media library persists download origins and marks"));
        {
            // One write, one announcement: the file watcher seeing our own write (or a write in the home folder, such
            // as an autosave) used to rebuild every shared bin again, which stalled the editor on big libraries.
            int announced = 0;
            const auto counting = connect(&profiles, &Profiles::libraryChanged, this, [&] { ++announced; });
            QJsonObject rated = profiles.findMedia(clipPath); // (a change to the one entry: the checks below count entries)
            rated["rating"] = 3;
            profiles.remember({rated});
            QFile other(QDir(homeA).filePath(QStringLiteral("unrelated.txt")));
            other.open(QIODevice::WriteOnly);
            other.write("x");
            other.close();
            QEventLoop wait;
            QTimer::singleShot(1500, &wait, &QEventLoop::quit);
            wait.exec();
            disconnect(counting);
            check(announced == 1, QStringLiteral("a library write is announced once, and other files in the home don't count (%1)").arg(announced));
        }
        newProject();
        auto* others = findChild<QCheckBox*>(QStringLiteral("youtubeOtherProjects"));
        check(others && !others->isChecked() && m_youtubeBin->count() == 0,
              QStringLiteral("From YouTube shows only this project's downloads until other projects' are asked for"));
        if (others)
            others->setChecked(true);
        auto* sharedBin = findChild<MediaBin*>(QStringLiteral("profileMedia"));
        check(m_project->media().empty() && m_youtubeBin->count() == 1 && sharedBin && sharedBin->count() == 1
                  && m_youtubeBin->item(0)->data(Qt::UserRole).toInt() == 0,
              QStringLiteral("another project can browse shared bins without importing every file"));
        m_youtubeBin->sharedToTimelineRequested(clipPath);
        const auto shared = m_project->media().empty() ? std::shared_ptr<const MediaItem>{} : m_project->media().begin()->second;
        check(shared && shared->origin == QLatin1String("youtube") && shared->markIn == kUsPerSecond && shared->markOut == 2 * kUsPerSecond
                  && m_project->timeline().duration() > 0,
              QStringLiteral("shared clip double-click imports and places it with its original marks"));
        addRecent(m_project->filePath());
        const QString keyA = profiles.settingsKey(QStringLiteral("recent/projects"));
        check(profiles.saveAndActivate({QStringLiteral("test-b"), QStringLiteral("Profile B"), homeB}, &error),
              QStringLiteral("a second profile can be activated"));
        newProject();
        check(m_youtubeBin->count() == 0 && profiles.media().empty() && keyA != profiles.settingsKey(QStringLiteral("recent/projects"))
                  && voiceDataDir() == QDir(homeB).filePath(QStringLiteral("Voice Assets")),
              QStringLiteral("media libraries, recent-project keys and downloaded voices are isolated by profile"));
        QFile broken(QDir(homeB).filePath(QStringLiteral("montage-library.json")));
        broken.open(QIODevice::WriteOnly);
        broken.write("invalid");
        broken.close();
        check(!profiles.remember({QJsonObject{{"path", mediaPath}}}, &error),
              QStringLiteral("a corrupt profile library is preserved instead of overwritten"));
        profiles.saveAndActivate({QStringLiteral("test-a"), QStringLiteral("Profile A"), homeA}, &error);
        newProject();
        check(m_youtubeBin->count() == 1 && profiles.list().size() == 2,
              QStringLiteral("returning to a profile restores its shared library"));
        if (others)
            others->setChecked(false);
        check(!profiles.saveAndActivate({{}, QStringLiteral("Duplicate"), homeA}, &error)
                  && profiles.active().id == QLatin1String("test-a"),
              QStringLiteral("two profiles cannot accidentally share the same home folder"));
        Timeline dirty = m_project->timeline();
        dirty.markers.push_back({dirty.allocId(), 0, 0, QStringLiteral("Keep this edit")});
        m_project->applyEdit(dirty, QStringLiteral("Profile rename test"));
        QTimer::singleShot(50, this, [&] {
            auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (!dialog)
                return;
            if (auto* name = dialog->findChild<QLineEdit*>(QStringLiteral("profileName")))
                name->setText(QStringLiteral("Renamed A"));
            if (auto* buttons = dialog->findChild<QDialogButtonBox*>())
                buttons->button(QDialogButtonBox::Ok)->click();
        });
        profilesDialog();
        check(profiles.active().name == QLatin1String("Renamed A") && m_project->timeline() == dirty && m_project->isModified(),
              QStringLiteral("renaming the current profile preserves the unsaved project without a save prompt"));
        int savePrompts = 0;
        bool requestedSwitch = false;
        QTimer answer;
        answer.setInterval(50);
        connect(&answer, &QTimer::timeout, this, [&] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                ++savePrompts;
                if (auto* discard = box->button(QMessageBox::Discard))
                    discard->click();
                else
                    box->reject();
            } else if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget()); dialog && !requestedSwitch) {
                requestedSwitch = true;
                if (auto* choices = dialog->findChild<QComboBox*>(QStringLiteral("profileChoice")))
                    choices->setCurrentIndex(choices->findData(QStringLiteral("test-b")));
                if (auto* buttons = dialog->findChild<QDialogButtonBox*>())
                    QTimer::singleShot(0, dialog, [buttons] { buttons->button(QDialogButtonBox::Ok)->click(); });
            }
        });
        answer.start();
        profilesDialog();
        answer.stop();
        check(profiles.active().id == QLatin1String("test-b") && savePrompts == 1 && !m_project->isModified()
                  && m_project->timeline().markers.empty(),
              QStringLiteral("switching profiles asks once about unsaved changes and starts an empty project"));
        profiles.useLegacy();
        m_project->clear();
        QSettings().remove(QStringLiteral("profiles/list"));
        QSettings().remove(QStringLiteral("profiles/data"));
    }
    // Reused IDs across a successful load must not inherit transient editor state.
    {
        const QString fixture = work.filePath("session-isolation.mvp");
        Timeline tl = m_project->timeline();
        QString error;
        const int one =
            story::addChapter(tl, "Session one", "First saved", kUsPerSecond, kUsPerSecond, nullptr, 0, m_project->settings().fps, &error);
        const int two =
            story::addChapter(tl, "Session two", "Second saved", kUsPerSecond, kUsPerSecond, nullptr, 0, m_project->settings().fps, &error);
        m_project->setTimelineLive(tl);
        auto* panel = findChild<QWidget*>("storyPanel");
        auto* chapters = panel ? panel->findChild<QListWidget*>("storyChapters") : nullptr;
        auto* script = panel ? panel->findChild<QPlainTextEdit*>("chapterScript") : nullptr;
        auto selectChapter = [&](int id) {
            if (chapters)
                for (int i = 0; i < chapters->count(); ++i)
                    if (chapters->item(i)->data(Qt::UserRole).toInt() == id)
                        chapters->setCurrentRow(i);
        };
        selectChapter(two);
        if (script)
            script->setPlainText("Draft from OLD project");
        selectChapter(one);
        const int trackId = tl.tracks.front().id;
        m_timeline->m_trackHeights[trackId] = 222;
        m_timeline->m_meters[trackId] = {.75f, .5f};
        m_timeline->selectAll();
        m_timeline->copySelection();
        auto same = tl;
        same.markers.clear();
        m_project->setTimelineLive(same);
        check(m_timeline->m_trackHeights[trackId] == 222, "ordinary edits retain custom track heights");
        selectChapter(two);
        check(script && script->toPlainText() == "Draft from OLD project", "ordinary edits retain chapter drafts");
        selectChapter(one);
        auto* staleWindow = new Vst3Window(m_project, 900, this);
        staleWindow->setAttribute(Qt::WA_DontShowOnScreen);
        staleWindow->show();
        QPointer<Vst3Window> stale = staleWindow;
        const bool loaded = m_project->save(fixture, &error) && m_project->load(fixture, &error);
        check(loaded, "project isolation fixture reloads with the same IDs");
        check(m_timeline->selection().empty() && m_timeline->m_trackHeights.empty() && m_timeline->m_meters.empty(),
              "opening a project clears selections, track heights and meters");
        check(m_timeline->m_clipboard.empty() && m_timeline->m_generatedThumbs.empty(),
              "opening a project drops copied media IDs and generated thumbnail caches");
        selectChapter(two);
        check(script && script->toPlainText() == "Second saved", "matching chapter IDs cannot recover another project's draft");
        check(stale && !stale->isVisible() && stale->m_session != m_project->snapshot()->session,
              "an old VST editor closes when the project session changes");
        const auto before = m_project->timeline();
        if (stale) {
            stale->save();
            stale->close();
        }
        check(m_project->timeline() == before, "stale VST save and close leave the new project unchanged");
        m_timeline->selectAll();
        m_timeline->copySelection();
        m_timeline->m_trackHeights[trackId] = 200;
        m_project->clear();
        check(m_timeline->selection().empty() && m_timeline->m_clipboard.empty() && m_timeline->m_trackHeights.empty(),
              "new project clears transient timeline state");
        check(script && script->toPlainText().isEmpty(), "new project clears displayed Story draft");
        m_project->load(fixture, &error);
    }
    failures += runCollectionUiTests(this, log);
    return failures;
}
