// The production tools' checks (pre-rendering, colour labels, the spectrogram): a part of runEditorTest.

#include "ui/MainWindow.h"

#include "core/Compositor.h"
#include "core/Effects.h"
#include "core/PreviewCache.h"
#include "core/Proxy.h"
#include "core/TimelineOps.h"
#include "engine/LiveEngine.h"
#include "engine/PlaybackEngine.h"
#include "ui/LiveView.h"
#include "ui/TimelineLayout.h"
#include "ui/TimelineWidget.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QMenu>
#include <QMenuBar>
#include <QWidgetAction>
#include <QTabWidget>
#include <QDir>
#include <QEventLoop>
#include <QTimer>

void MainWindow::runProductionToolsTest(const std::function<void(bool, const QString&)>& check, const QDir& work)
{
    auto wait = [](int ms) {
        QEventLoop loop;
        QTimer::singleShot(ms, &loop, &QEventLoop::quit);
        loop.exec();
    };
    {
        // Pre-rendering (every stretch counted heavy, one second ahead, for the test): the frames round the
        // cursor are made in the background and kept, the ruler shows them done; an edit there makes its
        // stretch new (its frames made again), and the frames kept are the picture the preview would render.
        const Clip* shot = nullptr;
        for (const Track& t : m_project->timeline().tracks)
            if (isVisualTrack(t.type))
                for (const Clip& c : t.clips)
                    if (!shot && !c.isGenerated() && c.length > 2 * kUsPerSecond)
                        shot = &c;
        bool ready = false, again = false, keyChanged = false, made = false;
        double off = 999;
        if (shot) {
            const int shotId = shot->id;
            const TimeUs at = shot->start + kUsPerSecond / 2;
            std::vector<PlaybackEngine::PreRenderSpan> spans;
            QMetaObject::Connection watch =
                connect(m_engine, &PlaybackEngine::preRenderChanged, this, [&](const std::vector<PlaybackEngine::PreRenderSpan>& s) { spans = s; });
            auto doneAt = [&](TimeUs t) {
                return std::any_of(spans.begin(), spans.end(), [&](const auto& s) { return s.done && s.start <= t && s.end > t + kUsPerSecond / 2; });
            };
            qputenv("MVP_PRERENDER_ALL", "1");
            m_engine->setPreRender(true);
            m_engine->clearPreRender();
            // (The cursor itself: the pre-render works round the cursor, and the preview asks for the cursor's frame
            // after an edit, so a frame asked for elsewhere was forgotten when that came after it.)
            seekTo(at);
            const QSize size(m_project->settings().width / m_engine->previewDivisor(), m_project->settings().height / m_engine->previewDivisor());
            auto keptNow = [&] { // (as below: the ruler alone can be a word late)
                const auto now = precache::stretches(*m_project->snapshot(), size, proxy::enabled());
                const precache::Stretch* s = precache::stretchAt(now, at);
                return s && m_engine->preRenderStore().has(s->key, m_project->settings().fps.frameAt(at));
            };
            for (int i = 0; i < 300 && !(doneAt(at) && keptNow()); ++i)
                wait(100);
            ready = doneAt(at) && keptNow();
            const auto before = precache::stretches(*m_project->snapshot(), size, proxy::enabled());
            if (const precache::Stretch* s = precache::stretchAt(before, at)) {
                const QImage kept = m_engine->preRenderStore().load(s->key, m_project->settings().fps.frameAt(at));
                Compositor cpu;
                cpu.setUseProxies(true);
                const QImage fresh = cpu.render(*m_project->snapshot(), m_project->settings().fps.timeOf(m_project->settings().fps.frameAt(at)), size);
                if (!kept.isNull() && kept.size() == fresh.size()) {
                    double sum = 0;
                    int n = 0;
                    for (int y = 0; y < fresh.height(); y += 7)
                        for (int x = 0; x < fresh.width(); x += 7, ++n)
                            sum += std::abs(qGray(kept.pixel(x, y)) - qGray(fresh.pixel(x, y)));
                    off = n ? sum / n : 999;
                }
            }
            // An edit in the stretch: a new key, made again.
            Timeline tl = m_project->timeline();
            tl.findClip(shotId)->gain = 0.6f;
            m_project->applyEdit(tl, QStringLiteral("Test pre-render edit"));
            const auto after = precache::stretches(*m_project->snapshot(), size, proxy::enabled());
            const precache::Stretch* a0 = precache::stretchAt(before, at);
            const precache::Stretch* a1 = precache::stretchAt(after, at);
            keyChanged = a0 && a1 && a0->key != a1->key && !m_engine->preRenderStore().has(a1->key, m_project->settings().fps.frameAt(at));
            spans.clear();
            seekTo(at);
            // (Until made too: the ruler's last word from before the edit can still arrive after the clear.)
            auto madeNow = [&] { return a1 && m_engine->preRenderStore().has(a1->key, m_project->settings().fps.frameAt(at)); };
            for (int i = 0; i < 300 && !(doneAt(at) && madeNow()); ++i)
                wait(100);
            made = madeNow();
            again = doneAt(at) && made;
            m_project->undoStack()->undo();
            qunsetenv("MVP_PRERENDER_ALL");
            m_engine->clearPreRender();
            disconnect(watch);
        }
        check(shot && ready && off < 6 && keyChanged && again,
              QStringLiteral("pre-rendering: frames near the cursor made in the background and shown done on the ruler (%1), "
                             "the same picture as rendering it (%2 off), an edit makes them new (%3) and they are made again (%4: made %5)")
                  .arg(ready ? "yes" : "no").arg(off, 0, 'f', 1).arg(keyChanged ? "yes" : "no").arg(again ? "yes" : "no")
                  .arg(made ? "yes" : "no"));
    }
    {
        // Colour labels: on an event and its group, one undo step, drawn in its colour, kept in the project file;
        // and the Spectrogram window for an audio event.
        const Clip* audio = nullptr;
        for (const Track& t : m_project->timeline().tracks)
            if (t.type == TrackType::Audio && !t.clips.empty() && !audio)
                audio = &t.clips.front();
        bool labelled = false, saved = false, spectrum = false;
        if (audio) {
            const int id = audio->id;
            m_timeline->selectClips({id});
            m_timeline->setLabelOnSelection(4);
            const std::set<int> group = ops::expandToGroups(m_project->timeline(), {id});
            labelled = std::all_of(group.begin(), group.end(), [&](int g) { return m_project->timeline().findClip(g)->label == 4; })
                       && timeline_layout::clipColor(TrackType::Audio, *m_project->timeline().findClip(id)) == clipLabelColor(4)
                       && m_project->undoStack()->undoText() == tr("Colour Label: %1").arg(clipLabelName(4));
            const QString path = work.filePath(QStringLiteral("labels.mvp"));
            QString error;
            Project copy;
            saved = m_project->saveBackup(path, &error) && copy.load(path, &error) && copy.timeline().findClip(id)
                    && copy.timeline().findClip(id)->label == 4;
            m_project->undoStack()->undo();
            showSpectrogram(id);
            QApplication::processEvents();
            wait(300);
            QApplication::processEvents();
            if (auto* dlg = findChild<QDialog*>(QStringLiteral("spectrogramWindow"))) {
                // Its cursor line follows the timeline's cursor (and the playhead).
                const Clip* ac = m_project->timeline().findClip(id);
                seekTo(ac->start + ac->length / 4);
                QApplication::processEvents();
                const QImage early = dlg->grab().toImage();
                seekTo(ac->start + ac->length * 3 / 4);
                QApplication::processEvents();
                const QImage late = dlg->grab().toImage();
                dlg->grab().save(work.filePath(QStringLiteral("spectrogram.png")));
                spectrum = dlg->isVisible() && early != late;
                dlg->close();
            }
        }
        check(labelled && saved && spectrum,
              QStringLiteral("colour labels on an event and its group (drawn in its colour, one undo step, saved with the "
                             "project); the Spectrogram window opens for an audio event, its cursor following the timeline's"));
    }
    {
        // Paste Event Attributes from the Edit menu: one picture's effects and Pan/Crop onto another, one undo step.
        auto findPictures = [this] {
            std::vector<int> ids;
            for (const Track& t : m_project->timeline().tracks)
                if (isVisualTrack(t.type) && !t.locked)
                    for (const Clip& c : t.clips)
                        if (!c.isGenerated() && !c.isSequence())
                            ids.push_back(c.id);
            return ids;
        };
        std::vector<int> pictures = findPictures();
        int duplicated = 0;
        if (pictures.size() == 1) { // (a copy to paste onto)
            m_timeline->selectClips({pictures[0]});
            m_timeline->duplicateSelection();
            duplicated = 1;
            pictures = findPictures();
        }
        bool pasted = false;
        QString why = QStringLiteral("fewer than two video events");
        if (pictures.size() >= 2) {
            Timeline tl = m_project->timeline();
            Clip* from = tl.findClip(pictures[0]);
            from->effects = {makeEffectOfType(QStringLiteral("invert"), tl.allocId())};
            from->panCrop[QStringLiteral("zoom")].value = 1.4;
            m_project->applyEdit(tl, QStringLiteral("Test attributes"));
            m_timeline->selectClips({pictures[0]});
            m_timeline->copySelection();
            m_timeline->selectClips({pictures[1]});
            QAction* action = nullptr;
            for (QAction* top : menuBar()->actions())
                if (top->menu() && top->text() == tr("&Edit"))
                    for (QAction* a : top->menu()->actions())
                        if (a->text() == tr("Paste Event &Attributes"))
                            action = a;
            if (action)
                action->trigger();
            const Clip* to = m_project->timeline().findClip(pictures[1]);
            pasted = action && to && to->effects.size() == 1 && to->effects[0].type == QStringLiteral("invert")
                     && to->panCrop == m_project->timeline().findClip(pictures[0])->panCrop
                     && m_project->undoStack()->undoText() == tr("Paste Event Attributes");
            why = action ? QStringLiteral("pasted: %1").arg(pasted) : QStringLiteral("no Edit > Paste Event Attributes");
            m_project->undoStack()->undo();
            m_project->undoStack()->undo();
        }
        for (int i = 0; i < duplicated; ++i)
            m_project->undoStack()->undo();
        check(pasted, QStringLiteral("Edit > Paste Event Attributes gives one event's effects and Pan/Crop to another (%1)").arg(why));
    }
    {
        // Every menu on the menu bar starts with a heading (a label, not a command), and Options keeps nothing
        // outside its headings (the last, AI, holds just its two).
        QStringList bare;
        int afterLast = -1;
        for (QAction* top : menuBar()->actions()) {
            QMenu* menu = top->menu();
            if (!menu)
                continue;
            const QList<QAction*> items = menu->actions();
            auto isHeading = [](QAction* a) {
                auto* w = qobject_cast<QWidgetAction*>(a);
                return w && w->defaultWidget() && w->defaultWidget()->objectName() == QLatin1String("menuHeading");
            };
            if (items.isEmpty() || !isHeading(items.front()))
                bare << QString(top->text()).remove(QLatin1Char('&'));
            if (top->text() == tr("&Options")) {
                afterLast = 0;
                for (QAction* a : items)
                    afterLast = isHeading(a) ? 0 : afterLast + 1;
            }
        }
        check(bare.isEmpty() && afterLast == 2, QStringLiteral("every menu starts with a heading (without one: %1); Options keeps "
                                                               "nothing past its last heading but AI's two (%2)")
                                                    .arg(bare.join(QStringLiteral(", ")))
                                                    .arg(afterLast));
    }
    {
        // The time selection bar: right-click offers Loop Playback; with it on, Play from inside the selection
        // goes round it until stopped.
        QStringList items;
        QTimer::singleShot(200, this, [&items] {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget())) {
                for (QAction* a : menu->actions())
                    items << a->text();
                menu->close();
            }
        });
        const QPoint at(timeline_layout::kHeaderWidth + 40, timeline_layout::kRegionBarH / 2);
        QContextMenuEvent rc(QContextMenuEvent::Mouse, at, m_timeline->viewport()->mapToGlobal(at));
        QApplication::sendEvent(m_timeline->viewport(), &rc);
        const bool offered = items.contains(m_loopAction->text()) && items.contains(tr("Clear Time Selection"));
        const TimeUs a = kUsPerSecond, b = a + kUsPerSecond * 6 / 10;
        m_timeline->setRegion(a, b);
        const bool wasLooping = m_loopAction->isChecked();
        m_loopAction->setChecked(true);
        seekTo(a + kUsPerSecond / 5);
        TimeUs last = 0;
        int rounds = 0;
        QMetaObject::Connection watch = connect(m_engine, &PlaybackEngine::positionChanged, this, [&](TimeUs t) {
            rounds += t + kUsPerSecond / 10 < last;
            last = t;
        });
        play(false);
        for (int i = 0; i < 60 && rounds < 2; ++i)
            wait(100);
        const bool looped = rounds >= 2 && m_engine->isPlaying() && last >= a && last <= b + kUsPerSecond / 5;
        disconnect(watch);
        stopPlayback(false);
        m_loopAction->setChecked(wasLooping);
        m_timeline->setRegion(0, 0);
        check(offered && looped, QStringLiteral("the time selection bar's right-click offers Loop Playback (%1); on, Play from "
                                                "inside the selection goes round it (%2 times)")
                                     .arg(items.join(QStringLiteral(", ")))
                                     .arg(rounds));
    }
    {
        // The Live tab takes the Editor timeline as a source: capture starts the timeline playing, its pictures
        // arrive on the Live tab, and stopping capture stops the timeline.
        auto* source = m_live->findChild<QComboBox*>(QStringLiteral("liveSource"));
        const int before = source ? source->currentIndex() : -1;
        const int editor = source ? source->findText(QStringLiteral("Editor timeline"), Qt::MatchContains) : -1;
        bool playing = false, pictures = false, stopped = false;
        if (editor >= 0) {
            source->setCurrentIndex(editor);
            m_modes->setCurrentWidget(m_live);
            seekTo(0);
            m_live->startStop();
            for (int i = 0; i < 50 && !(m_engine->isPlaying() && !m_live->engine()->cleanFrame().isNull()); ++i)
                wait(100);
            playing = m_engine->isPlaying();
            pictures = !m_live->engine()->cleanFrame().isNull();
            m_live->startStop();
            for (int i = 0; i < 30 && m_engine->isPlaying(); ++i)
                wait(100);
            stopped = !m_engine->isPlaying();
            m_modes->setCurrentIndex(0);
            source->setCurrentIndex(before);
        }
        check(editor >= 0 && playing && pictures && stopped,
              QStringLiteral("the Live tab's Editor timeline source: listed (%1), capturing plays the timeline (%2) and its "
                             "pictures arrive (%3), stopping capture stops it (%4)")
                  .arg(editor >= 0 ? "yes" : "no").arg(playing ? "yes" : "no").arg(pictures ? "yes" : "no").arg(stopped ? "yes" : "no"));
    }
}
