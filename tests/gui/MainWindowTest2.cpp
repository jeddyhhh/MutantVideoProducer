// The production tools' checks (pre-rendering, colour labels, the spectrogram): a part of runEditorTest.

#include "ui/MainWindow.h"

#include "core/Compositor.h"
#include "core/PreviewCache.h"
#include "core/Proxy.h"
#include "core/TimelineOps.h"
#include "engine/PlaybackEngine.h"
#include "ui/TimelineLayout.h"
#include "ui/TimelineWidget.h"

#include <QApplication>
#include <QDialog>
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
        bool ready = false, again = false, keyChanged = false;
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
            m_engine->requestFrame(at);
            for (int i = 0; i < 300 && !doneAt(at); ++i)
                wait(100);
            ready = doneAt(at);
            const QSize size(m_project->settings().width / m_engine->previewDivisor(), m_project->settings().height / m_engine->previewDivisor());
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
            m_engine->requestFrame(at);
            for (int i = 0; i < 300 && !doneAt(at); ++i)
                wait(100);
            again = doneAt(at) && m_engine->preRenderStore().has(a1->key, m_project->settings().fps.frameAt(at));
            m_project->undoStack()->undo();
            qunsetenv("MVP_PRERENDER_ALL");
            m_engine->clearPreRender();
            disconnect(watch);
        }
        check(shot && ready && off < 6 && keyChanged && again,
              QStringLiteral("pre-rendering: frames near the cursor made in the background and shown done on the ruler (%1), "
                             "the same picture as rendering it (%2 off), an edit makes them new and they are made again (%3)")
                  .arg(ready ? "yes" : "no").arg(off, 0, 'f', 1).arg(keyChanged && again ? "yes" : "no"));
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
                dlg->grab().save(work.filePath(QStringLiteral("spectrogram.png")));
                spectrum = dlg->isVisible();
                dlg->close();
            }
        }
        check(labelled && saved && spectrum,
              QStringLiteral("colour labels on an event and its group (drawn in its colour, one undo step, saved with the "
                             "project); the Spectrogram window opens for an audio event"));
    }
}
