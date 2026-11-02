// Tools > Editing: line events up by their sound, and multicam editing (the angles window, cutting with 1-9).
#include "ui/MainWindow.h"
#include "ui/TimelineWidget.h"
#include "ui/Theme.h"
#include "core/AudioSync.h"
#include "core/Multicam.h"
#include "engine/MediaCache.h"
#include "engine/PlaybackEngine.h"

#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QStatusBar>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
// The angles, side by side: each at the time of the cut (the playhead, else the cursor); the one on the
// Multicam track there is outlined. Click one (or press its number) to cut to it.
class MulticamWindow : public QWidget {
public:
    MulticamWindow(Project* project, MediaCache* cache, std::function<TimeUs()> now, std::function<void(int)> pick, QWidget* parent)
        : QWidget(parent, Qt::Tool), m_project(project), m_cache(cache), m_now(std::move(now)), m_pick(std::move(pick))
    {
        setObjectName(QStringLiteral("multicamWindow"));
        setWindowTitle(tr("Multicam"));
        setToolTip(tr("Click an angle (or press its number) to cut to it: while playing, from the playhead on; when stopped, the "
                      "stretch under the cursor"));
        resize(560, 260);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
        timer->start(100);
        connect(cache, &MediaCache::updated, this, qOverload<>(&QWidget::update));
    }

protected:
    QRect cell(int i, int n) const
    {
        const int cols = n <= 4 ? n : (n + 1) / 2, rows = (n + cols - 1) / cols;
        const int w = width() / std::max(1, cols), h = height() / std::max(1, rows);
        return QRect((i % cols) * w, (i / cols) * h, w, h).adjusted(4, 4, -4, -4);
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::c().window);
        const Timeline& tl = m_project->timeline();
        const auto list = multicam::angles(tl, m_project->media());
        if (list.empty()) {
            p.setPen(theme::c().dim);
            p.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap, tr("No multicam yet: select the cameras' video events and choose\n"
                                                                     "Tools > Multicam > Make Multicam from Selected Events."));
            return;
        }
        const TimeUs t = m_now();
        int showing = 0;
        if (const int mi = multicam::trackIndex(tl); mi >= 0)
            for (const Clip& c : tl.tracks[size_t(mi)].clips)
                if (c.start <= t && t < c.end())
                    showing = multicam::angleOf(c);
        for (int i = 0; i < int(list.size()); ++i) {
            const QRect r = cell(i, int(list.size()));
            p.fillRect(r, Qt::black);
            const Clip* c = tl.findClip(list[size_t(i)].clipId);
            const auto media = c ? m_project->mediaItem(c->mediaId) : nullptr;
            if (c && media && t >= c->start && t < c->end()) {
                const TimeUs src = c->sourceIn + (t - c->start);
                const QImage img = m_cache->thumbnail(media, c->streamIndex, src - src % 200000); // (five a second)
                if (!img.isNull()) {
                    const QSize s = img.size().scaled(r.size() - QSize(0, 18), Qt::KeepAspectRatio);
                    p.drawImage(QRect(r.center().x() - s.width() / 2, r.top() + (r.height() - 18 - s.height()) / 2, s.width(), s.height()), img);
                }
            } else {
                p.setPen(theme::c().dim);
                p.drawText(r, Qt::AlignCenter, tr("(no picture here)"));
            }
            p.setPen(theme::c().text);
            p.drawText(QRect(r.left() + 4, r.bottom() - 17, r.width() - 8, 16), Qt::AlignLeft | Qt::AlignVCenter,
                       QStringLiteral("%1  %2").arg(list[size_t(i)].number).arg(list[size_t(i)].name));
            if (list[size_t(i)].number == showing) {
                p.setPen(QPen(theme::c().bad, 3));
                p.setBrush(Qt::NoBrush);
                p.drawRect(r.adjusted(1, 1, -2, -2));
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const auto list = multicam::angles(m_project->timeline(), m_project->media());
        for (int i = 0; i < int(list.size()); ++i)
            if (cell(i, int(list.size())).contains(e->position().toPoint()))
                m_pick(list[size_t(i)].number);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9 && !e->modifiers())
            m_pick(e->key() - Qt::Key_0);
        else
            QWidget::keyPressEvent(e);
    }

private:
    Project* m_project;
    MediaCache* m_cache;
    std::function<TimeUs()> m_now;
    std::function<void(int)> m_pick;
};
} // namespace

void MainWindow::syncByAudio()
{
    const auto& sel = m_timeline->selection();
    Timeline tl = m_project->timeline();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const audiosync::Result r = audiosync::syncByAudio(tl, m_project->media(), {sel.begin(), sel.end()});
    QApplication::restoreOverrideCursor();
    if (!r.error.isEmpty()) {
        statusBar()->showMessage(r.error, 8000);
        return;
    }
    if (!r.moved.empty())
        m_project->applyEdit(tl, tr("Sync by Audio"));
    QStringList moves;
    for (const auto& [id, delta] : r.moved)
        moves << QStringLiteral("%1%2 s").arg(delta >= 0 ? QStringLiteral("+") : QString()).arg(double(delta) / kUsPerSecond, 0, 'f', 3);
    statusBar()->showMessage(r.unmatched.empty() ? tr("Lined up %n event(s) by their sound (%1)", nullptr, int(r.moved.size())).arg(moves.join(QStringLiteral(", ")))
                                                 : tr("Lined up %n event(s) by their sound; %1 couldn't be matched (not the same moment, too quiet, or "
                                                      "not at normal speed)", nullptr, int(r.moved.size())).arg(r.unmatched.size()),
                             10000);
}

void MainWindow::makeMulticam()
{
    // The selected video events, top track first: angle 1, 2...
    std::vector<std::pair<std::pair<int, TimeUs>, int>> picked;
    const Timeline& now = m_project->timeline();
    for (int id : m_timeline->selection()) {
        int ti = -1;
        if (const Clip* c = now.findClip(id, &ti); c && ti >= 0 && isVisualTrack(now.tracks[size_t(ti)].type) && !c->isSequence())
            picked.push_back({{ti, c->start}, id});
    }
    std::sort(picked.begin(), picked.end());
    std::vector<int> ids;
    for (const auto& p : picked)
        ids.push_back(p.second);
    Timeline tl = now;
    QString error;
    if (!multicam::create(tl, m_project->media(), ids, &error)) {
        statusBar()->showMessage(error, 8000);
        return;
    }
    m_project->applyEdit(tl, tr("Make Multicam"));
    statusBar()->showMessage(tr("Multicam: %n angle(s). Play, and press 1-%n (on the timeline or in the Multicam window) to cut between them",
                                nullptr, int(ids.size())),
                             10000);
    showMulticam();
}

void MainWindow::multicamAngle(int angle)
{
    if (multicam::trackIndex(m_project->timeline()) < 0)
        return; // (the number keys only cut when there is a multicam)
    Timeline tl = m_project->timeline();
    QString error;
    const auto playhead = m_engine->isPlaying() ? m_timeline->playhead() : std::nullopt;
    const bool ok = playhead ? multicam::cut(tl, m_project->media(), angle, *playhead, &error)
                             : multicam::switchAngle(tl, m_project->media(), angle, m_timeline->cursorTime(), &error);
    if (!ok) {
        statusBar()->showMessage(error, 5000);
        return;
    }
    if (!(tl == m_project->timeline()))
        m_project->applyEdit(tl, tr("Multicam: Angle %1").arg(angle));
}

void MainWindow::showMulticam()
{
    auto* w = findChild<QWidget*>(QStringLiteral("multicamWindow"));
    if (!w)
        w = new MulticamWindow(m_project, m_cache, [this] { return m_engine->isPlaying() && m_timeline->playhead() ? *m_timeline->playhead() : m_timeline->cursorTime(); },
                               [this](int angle) { multicamAngle(angle); }, this);
    w->show();
    w->raise();
}
