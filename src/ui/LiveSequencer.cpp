#include "ui/LiveSequencer.h"

#include "core/Project.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace livesequencer {
namespace {
constexpr int kRuler = 16;  // the bar numbers along the top
constexpr int kRows = 16;
constexpr int kRowHeight = 14;
constexpr int kGridHeight = kRuler + kRows * kRowHeight;
constexpr int kEdge = 5;    // px of a block's right edge that change its length
const QColor kBlock(0xe8848f), kBlockEdge(0xa8454f);
} // namespace

// The grid: bar numbers along the top, a row a pad, the blocks, the playhead.
class Roll : public QWidget {
public:
    Roll(Project* project, QScrollArea* scroll) : m_project(project), m_scroll(scroll)
    {
        setMouseTracking(true);
        setObjectName(QStringLiteral("liveSequencerRoll"));
    }
    const LiveSequence& seq() const { return m_dragging ? m_work : m_project->timeline().liveSequence; }
    double pxPerTick() const
    {
        const int loop = std::max(1, seq().loopTicks());
        return std::max(1.0, double(m_scroll->viewport()->width() - 1)) / loop * m_zoom;
    }
    int rowHeight() const { return kRowHeight; }
    void fit()
    {
        const int w = std::max(m_scroll->viewport()->width(), int(std::ceil(seq().loopTicks() * pxPerTick())) + 1);
        resize(w, kGridHeight);
        update();
    }
    void setPlayhead(int tick)
    {
        if (tick == m_playhead)
            return;
        m_playhead = tick;
        update();
    }
    void zoomBy(double factor, double atX)
    {
        const double tick = atX / pxPerTick();
        m_zoom = std::clamp(m_zoom * factor, 1.0, 32.0);
        fit();
        m_scroll->horizontalScrollBar()->setValue(int(tick * pxPerTick() - (atX - m_scroll->horizontalScrollBar()->value())));
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto& c = theme::c();
        const LiveSequence& s = seq();
        QPainter p(this);
        const double px = pxPerTick();
        const int rh = rowHeight(), loopX = int(std::round(s.loopTicks() * px));
        p.fillRect(rect(), c.deep);
        for (int r = 0; r < kRows; ++r) {
            const bool empty = m_project->timeline().livePads[size_t(r)].isEmpty();
            p.fillRect(QRect(0, kRuler + r * rh, loopX, rh), r % 2 ? c.base : c.panel);
            if (empty)
                p.fillRect(QRect(0, kRuler + r * rh, loopX, rh), QColor(0, 0, 0, 60));
        }
        // Lines: the snap, the beats, the bars (with their numbers).
        const int snap = s.snapTicks(), beat = s.beatTicks(), bar = s.barTicks();
        for (int t = 0; t <= s.loopTicks(); t += snap) {
            const int x = int(std::round(t * px));
            const bool isBar = t % bar == 0, isBeat = t % beat == 0;
            if (!isBar && !isBeat && snap * px < 4)
                continue; // (too close to see)
            QColor line = c.ink;
            line.setAlpha(isBar ? 110 : isBeat ? 50 : 22);
            p.setPen(QPen(line, isBar ? 1.5 : 1.0));
            p.drawLine(x, isBar ? 0 : kRuler, x, kRuler + kRows * rh);
            if (isBar && t < s.loopTicks()) {
                p.setPen(c.dim);
                p.drawText(QRect(x + 3, 0, 40, kRuler), Qt::AlignVCenter | Qt::AlignLeft, QString::number(t / bar + 1));
            }
        }
        p.setPen(c.line);
        p.drawLine(0, kRuler, loopX, kRuler);
        // The blocks.
        p.setRenderHint(QPainter::Antialiasing);
        for (const LiveSequenceBlock& b : s.blocks) {
            const QRectF r(b.start * px + 0.5, kRuler + b.pad * rh + 1.5, std::max(3.0, b.length * px - 1), rh - 3);
            p.setPen(QPen(kBlockEdge, 1));
            p.setBrush(kBlock);
            p.drawRoundedRect(r, 2, 2);
            if (r.width() > 18 && rh >= 12) {
                p.setPen(QColor(0x3a0a12));
                QFont f = font();
                f.setPointSizeF(std::max(6.0, f.pointSizeF() - 1.5));
                p.setFont(f);
                p.drawText(r.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::number(b.pad + 1));
            }
        }
        if (m_playhead >= 0) {
            const int x = int(std::round(m_playhead * px));
            p.setPen(QPen(c.playhead, 2));
            p.drawLine(x, 0, x, kRuler + kRows * rh);
        }
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        m_work = m_project->timeline().liveSequence;
        m_before = m_project->timeline();
        const double px = pxPerTick();
        const int tick = int(e->position().x() / px), row = rowAt(e->position().y());
        if (row < 0 || tick < 0 || tick >= m_work.loopTicks())
            return;
        m_dragging = true;
        const int hit = blockAt(e->position());
        if (e->button() == Qt::RightButton) { // remove (and keep removing while dragged)
            m_mode = Erase;
            if (hit >= 0)
                m_work.blocks.erase(m_work.blocks.begin() + hit);
            pushLive();
            return;
        }
        if (e->button() != Qt::LeftButton) {
            m_dragging = false;
            return;
        }
        if (hit >= 0) {
            const LiveSequenceBlock& b = m_work.blocks[size_t(hit)];
            m_index = hit;
            m_mode = e->position().x() >= (b.end() * px) - kEdge ? Resize : Move;
            m_grab = tick - b.start;
            return;
        }
        // A new block, as long as the last one made (or a snap), at the snap before the click.
        const int snap = m_work.snapTicks();
        const int start = tick / snap * snap;
        const int length = std::min(m_lastLength > 0 ? m_lastLength : snap, m_work.loopTicks() - start);
        m_work.blocks.push_back({row, start, std::max(1, length)});
        m_index = int(m_work.blocks.size()) - 1;
        m_mode = Resize;
        m_created = true;
        pushLive();
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const double px = pxPerTick();
        if (!m_dragging) { // the cursor says what a press would do
            const int hit = blockAt(e->position());
            setCursor(hit >= 0 && e->position().x() >= seq().blocks[size_t(hit)].end() * px - kEdge ? Qt::SizeHorCursor
                      : hit >= 0                                                                    ? Qt::OpenHandCursor
                                                                                                    : Qt::ArrowCursor);
            return;
        }
        const int snap = m_work.snapTicks(), loop = m_work.loopTicks();
        const int tick = std::clamp(int(e->position().x() / px), 0, loop);
        if (m_mode == Erase) {
            const int hit = blockAt(e->position(), &m_work);
            if (hit >= 0) {
                m_work.blocks.erase(m_work.blocks.begin() + hit);
                pushLive();
            }
            return;
        }
        if (m_index < 0 || m_index >= int(m_work.blocks.size()))
            return;
        LiveSequenceBlock& b = m_work.blocks[size_t(m_index)];
        if (m_mode == Resize) {
            const int end = std::clamp((tick + snap / 2) / snap * snap, b.start + snap, loop); // (to the nearest snap)
            b.length = std::max(1, end - b.start);
        } else {
            b.start = std::clamp((tick - m_grab + snap / 2) / snap * snap, 0, std::max(0, loop - b.length));
            if (const int row = rowAt(e->position().y()); row >= 0)
                b.pad = row;
        }
        pushLive();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging)
            return;
        m_dragging = false;
        if (m_mode == Resize && m_index >= 0 && m_index < int(m_work.blocks.size()))
            m_lastLength = m_work.blocks[size_t(m_index)].length;
        if (!(m_project->timeline().liveSequence == m_before.liveSequence))
            m_project->commitEdit(m_before, m_mode == Erase ? tr("Remove Sequence Blocks")
                                            : m_created           ? tr("Add Sequence Block")
                                            : m_mode == Move      ? tr("Move Sequence Block")
                                                                  : tr("Change Sequence Block Length"));
        m_index = -1;
        m_created = false;
        update();
    }

    void wheelEvent(QWheelEvent* e) override
    {
        if (e->modifiers() & Qt::ControlModifier) { // zoom in and out about the pointer
            zoomBy(e->angleDelta().y() > 0 ? 1.25 : 0.8, e->position().x());
            e->accept();
            return;
        }
        QWidget::wheelEvent(e);
    }

private:
    enum Mode { Move, Resize, Erase };
    int rowAt(double y) const
    {
        const int r = int((y - kRuler) / rowHeight());
        return y < kRuler || r >= kRows ? -1 : r;
    }
    // The block under a point (the last drawn: on top), or -1.
    int blockAt(QPointF at, const LiveSequence* s = nullptr) const
    {
        const LiveSequence& seqNow = s ? *s : seq();
        const double px = pxPerTick();
        const int row = rowAt(at.y());
        for (int i = int(seqNow.blocks.size()) - 1; i >= 0; --i) {
            const LiveSequenceBlock& b = seqNow.blocks[size_t(i)];
            if (b.pad == row && at.x() >= b.start * px && at.x() < b.end() * px)
                return i;
        }
        return -1;
    }
    void pushLive()
    {
        Timeline tl = m_project->timeline();
        tl.liveSequence = m_work;
        m_project->setTimelineLive(tl);
        update();
    }

    Project* m_project;
    QScrollArea* m_scroll;
    double m_zoom = 1.0; // 1: the loop fits the view
    int m_playhead = -1;
    bool m_dragging = false, m_created = false;
    Mode m_mode = Move;
    int m_index = -1, m_grab = 0, m_lastLength = 0;
    LiveSequence m_work;
    Timeline m_before;
};

// The pads' names down the left, in line with the rows; click one to press that pad.
class Keys : public QWidget {
public:
    Keys(Project* project, Roll* roll, std::function<void(int)> pressed) : m_project(project), m_roll(roll), m_pressed(std::move(pressed))
    {
        setFixedWidth(64);
        setObjectName(QStringLiteral("liveSequencerKeys"));
        setToolTip(tr("The pads. Click one to press it"));
    }
    void setActive(int pad)
    {
        m_active = pad;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        const auto& c = theme::c();
        QPainter p(this);
        p.fillRect(rect(), c.headerBg);
        const int rh = m_roll->rowHeight();
        QFont f = font();
        f.setPointSizeF(std::max(6.0, f.pointSizeF() - 1));
        p.setFont(f);
        for (int r = 0; r < kRows; ++r) {
            const QRect row(0, kRuler + r * rh, width(), rh);
            const LivePad& pad = m_project->timeline().livePads[size_t(r)];
            p.fillRect(row.adjusted(0, 0, 0, -1), r == m_active ? c.accent : (r % 2 ? c.raised : c.panel));
            p.setPen(r == m_active ? c.onAccent : pad.isEmpty() ? c.faint : c.text);
            const QString name = pad.name.isEmpty() ? (pad.isEmpty() ? tr("empty") : tr("pad")) : pad.name;
            p.drawText(row.adjusted(4, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft,
                       p.fontMetrics().elidedText(QStringLiteral("%1 %2").arg(r + 1).arg(name), Qt::ElideRight, width() - 6));
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int r = int((e->position().y() - kRuler) / m_roll->rowHeight());
        if (e->position().y() >= kRuler && r >= 0 && r < kRows && m_pressed)
            m_pressed(r);
    }

private:
    Project* m_project;
    Roll* m_roll;
    std::function<void(int)> m_pressed;
    int m_active = -1;
};
} // namespace livesequencer

using namespace livesequencer;

LiveSequencer::LiveSequencer(Project* project, QWidget* parent) : QWidget(parent), m_project(project)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(3);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_on = new QToolButton;
    m_on->setText(tr("Sequence"));
    m_on->setCheckable(true);
    m_on->setObjectName(QStringLiteral("liveSequenceOn"));
    m_on->setToolTip(tr("Play the loop in time with the beat clock while the Live tab runs (from its next 1),\n"
                        "switching each block's pad on for its length"));
    m_bars = new QSpinBox;
    m_bars->setRange(1, 64);
    m_bars->setSuffix(tr(" bars"));
    m_bars->setToolTip(tr("How long the loop is"));
    m_top = new QSpinBox;
    m_top->setRange(1, 16);
    m_top->setToolTip(tr("Time signature: beats in a bar"));
    m_bottom = new QComboBox;
    for (int u : {2, 4, 8, 16})
        m_bottom->addItem(QStringLiteral("/%1").arg(u), u);
    m_bottom->setToolTip(tr("Time signature: the note a beat is (a beat of the clock is one of these)"));
    m_snap = new QComboBox;
    const std::pair<QString, int> snaps[] = {{tr("Beat"), 0}, {QStringLiteral("1/1"), 1}, {QStringLiteral("1/2"), 2}, {QStringLiteral("1/4"), 4},
                                             {QStringLiteral("1/8"), 8}, {QStringLiteral("1/16"), 16}, {QStringLiteral("1/32"), 32},
                                             {tr("1/8T"), -8}, {tr("1/16T"), -16}};
    for (const auto& [name, value] : snaps)
        m_snap->addItem(name, value);
    m_snap->setToolTip(tr("Snap: where blocks start and end (T: triplets; a new block is as long as the last one made)"));
    m_clear = new QToolButton;
    m_clear->setText(tr("Clear"));
    m_clear->setToolTip(tr("Remove every block (Undo brings them back)"));
    bar->addWidget(m_on);
    bar->addWidget(m_bars);
    bar->addWidget(m_top);
    bar->addWidget(m_bottom);
    bar->addWidget(m_snap, 1);
    bar->addWidget(m_clear);
    layout->addLayout(bar);

    auto* body = new QHBoxLayout;
    body->setSpacing(0);
    m_scroll = new QScrollArea;
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_roll = new Roll(project, m_scroll);
    m_roll->setToolTip(tr("Click (and drag) to put a block on a pad's row; drag a block to move it, its right edge to change its length;\n"
                          "right-click (or right-drag) to remove. Ctrl+wheel zooms"));
    m_scroll->setWidget(m_roll);
    m_scroll->setFixedHeight(kGridHeight + m_scroll->horizontalScrollBar()->sizeHint().height());
    m_keys = new Keys(project, m_roll, [this](int pad) { emit padPressed(pad); });
    m_keys->setFixedHeight(kGridHeight);
    body->addWidget(m_keys, 0, Qt::AlignTop);
    body->addWidget(m_scroll, 1);
    layout->addLayout(body);

    auto edits = [this](auto* w, auto signal, const QString& what, std::function<void(LiveSequence&)> change) {
        connect(w, signal, this, [this, what, change] { edit(change, what); });
    };
    connect(m_on, &QToolButton::toggled, this, [this](bool on) { edit([on](LiveSequence& s) { s.on = on; }, on ? tr("Sequence On") : tr("Sequence Off")); });
    edits(m_bars, qOverload<int>(&QSpinBox::valueChanged), tr("Sequence Length"), [this](LiveSequence& s) { s.bars = m_bars->value(); });
    edits(m_top, qOverload<int>(&QSpinBox::valueChanged), tr("Time Signature"), [this](LiveSequence& s) { s.beatsPerBar = m_top->value(); });
    edits(m_bottom, qOverload<int>(&QComboBox::currentIndexChanged), tr("Time Signature"),
          [this](LiveSequence& s) { s.beatUnit = m_bottom->currentData().toInt(); });
    edits(m_snap, qOverload<int>(&QComboBox::currentIndexChanged), tr("Sequence Snap"), [this](LiveSequence& s) { s.snap = m_snap->currentData().toInt(); });
    connect(m_clear, &QToolButton::clicked, this, [this] { edit([](LiveSequence& s) { s.blocks.clear(); }, tr("Clear Sequence")); });
    connect(project, &Project::changed, this, &LiveSequencer::refresh);
    m_clock.start();
    m_timer.setInterval(8); // (the playhead and the switches between beats: a sixteenth at 180 bpm is 83 ms)
    connect(&m_timer, &QTimer::timeout, this, &LiveSequencer::tick);
    refresh();
}

void LiveSequencer::edit(const std::function<void(LiveSequence&)>& change, const QString& description)
{
    Timeline tl = m_project->timeline();
    change(tl.liveSequence);
    if (!(tl.liveSequence == m_project->timeline().liveSequence))
        m_project->applyEdit(tl, description);
}

void LiveSequencer::refresh()
{
    const LiveSequence& s = m_project->timeline().liveSequence;
    const QSignalBlocker b1(m_on), b2(m_bars), b3(m_top), b4(m_bottom), b5(m_snap);
    m_on->setChecked(s.on);
    m_bars->setValue(s.bars);
    m_top->setValue(s.beatsPerBar);
    m_bottom->setCurrentIndex(std::max(0, m_bottom->findData(s.beatUnit)));
    m_snap->setCurrentIndex(std::max(0, m_snap->findData(s.snap)));
    m_clear->setEnabled(!s.blocks.empty());
    if (!s.on && m_playing) {
        m_playing = false;
        m_roll->setPlayhead(-1);
    }
    m_timer.setInterval(8);
    if (s.on && m_running)
        m_timer.start();
    else
        m_timer.stop();
    m_roll->fit();
    m_keys->update();
}

void LiveSequencer::setRunning(bool running)
{
    m_running = running;
    m_playing = false; // (from the next 1)
    m_roll->setPlayhead(-1);
    refresh();
}

void LiveSequencer::setActivePad(int pad)
{
    m_keys->setActive(pad);
}

void LiveSequencer::beat(double bpm, int barBeat)
{
    const LiveSequence& s = m_project->timeline().liveSequence;
    if (!s.on || !m_running)
        return;
    if (bpm > 0)
        m_bpm = bpm;
    if (!m_playing) {
        if (barBeat != 0)
            return; // (the loop starts on a 1)
        m_playing = true;
        m_beats = 0;
        m_lastPad = -2; // (whatever the first block says goes)
    } else {
        ++m_beats;
        // The clock's bars are of four: in four, its 1 (moved by Downbeat or MIDI song position) is the loop's bar.
        if (s.beatsPerBar == 4 && barBeat == 0 && m_beats % 4 != 0)
            m_beats = (m_beats / 4 + (m_beats % 4 >= 2)) * 4;
    }
    m_lastBeatMs = m_clock.elapsed();
    tick();
}

int LiveSequencer::playhead() const
{
    if (!m_playing)
        return -1;
    const LiveSequence& s = m_project->timeline().liveSequence;
    // Between beats the loop runs on at the tempo, not past the next beat (which brings it in step again).
    const double frac = std::clamp(double(m_clock.elapsed() - m_lastBeatMs) * m_bpm / 60000.0, 0.0, 0.999);
    const int loop = std::max(1, s.loopTicks());
    return int(std::floor((double(m_beats) + frac) * s.beatTicks())) % loop;
}

void LiveSequencer::tick()
{
    const int at = playhead();
    m_roll->setPlayhead(at);
    if (at < 0)
        return;
    const int pad = m_project->timeline().liveSequence.padAt(at);
    if (pad != m_lastPad) {
        m_lastPad = pad;
        emit switchPad(pad);
    }
}
