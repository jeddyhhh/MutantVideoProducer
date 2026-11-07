#include "ui/LiveSequencer.h"

#include "core/Project.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QCursor>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
#include <set>

namespace livesequencer {
namespace {
constexpr int kRuler = 16;  // the bar numbers along the top
constexpr int kRows = 16;
constexpr int kRowHeight = 14;
constexpr int kGridHeight = kRuler + kRows * kRowHeight;
constexpr int kEdge = 5;    // px of a block's right edge that change its length
const QColor kBlock(0xe8848f), kBlockEdge(0xa8454f), kSelected(0xffc2c8);
} // namespace

// The grid: bar numbers along the top, a row a pad, the blocks, the playhead.
class Roll : public QWidget {
public:
    Roll(Project* project, QScrollArea* scroll) : m_project(project), m_scroll(scroll)
    {
        setMouseTracking(true);
        setFocusPolicy(Qt::ClickFocus); // (Delete, Ctrl+A and Esc for the selection)
        setObjectName(QStringLiteral("liveSequencerRoll"));
    }
    // Docked under the pads the rows are small; in a window of its own they share its height.
    void setDocked(bool docked)
    {
        m_docked = docked;
        fit();
    }
    const std::set<int>& selection() const { return m_selected; }
    const LiveSequence& seq() const { return m_dragging ? m_work : m_project->timeline().liveSequence; }
    double pxPerTick() const
    {
        const int loop = std::max(1, seq().loopTicks());
        return std::max(1.0, double(m_scroll->viewport()->width() - 1)) / loop * m_zoom;
    }
    int rowHeight() const { return std::max(kRowHeight, (height() - kRuler) / kRows); }
    void fit()
    {
        if (!m_dragging && seq().blocks != m_known) { // (changed elsewhere, undone...: the indices mean other blocks now)
            m_selected.clear();
            m_known = seq().blocks;
        }
        const int w = std::max(m_scroll->viewport()->width(), int(std::ceil(seq().loopTicks() * pxPerTick())) + 1);
        resize(w, m_docked ? kGridHeight : std::max(kGridHeight, m_scroll->viewport()->height()));
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
        for (size_t i = 0; i < s.blocks.size(); ++i) {
            const LiveSequenceBlock& b = s.blocks[i];
            const QRectF r(b.start * px + 0.5, kRuler + b.pad * rh + 1.5, std::max(3.0, b.length * px - 1), rh - 3);
            const bool selected = m_selected.count(int(i)) > 0;
            p.setPen(selected ? QPen(Qt::white, 1.5) : QPen(kBlockEdge, 1));
            p.setBrush(selected ? kSelected : kBlock);
            p.drawRoundedRect(r, 2, 2);
            if (r.width() > 18 && rh >= 12) {
                p.setPen(QColor(0x3a0a12));
                QFont f = font();
                f.setPointSizeF(std::max(6.0, f.pointSizeF() - 1.5));
                p.setFont(f);
                p.drawText(r.adjusted(3, 0, -2, 0), Qt::AlignVCenter | Qt::AlignLeft, QString::number(b.pad + 1));
            }
        }
        if (m_dragging && m_mode == Select && !m_band.isNull()) {
            QColor fill = c.accent;
            fill.setAlpha(40);
            p.setPen(QPen(c.ink, 1, Qt::DashLine));
            p.setBrush(fill);
            p.drawRect(m_band.normalized());
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
            m_selected.clear();
            if (hit >= 0)
                m_work.blocks.erase(m_work.blocks.begin() + hit);
            pushLive();
            return;
        }
        if (e->button() != Qt::LeftButton) {
            m_dragging = false;
            return;
        }
        if (e->modifiers() & Qt::ControlModifier) { // select: a box round blocks (or Ctrl+click one in or out)
            m_mode = Select;
            m_index = hit;
            m_press = e->position();
            m_band = QRectF();
            return;
        }
        if (hit >= 0) {
            const LiveSequenceBlock& b = m_work.blocks[size_t(hit)];
            m_index = hit;
            m_mode = e->position().x() >= (b.end() * px) - kEdge ? Resize : Move;
            m_grab = tick - b.start;
            if (m_mode == Move && !m_selected.count(hit)) // (a block not in the selection moves on its own)
                m_selected = {hit};
            m_orig = m_work.blocks;
            update();
            return;
        }
        m_selected.clear();
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
        if (m_mode == Select) {
            m_band = QRectF(m_press, e->position());
            update();
            return;
        }
        if (m_index < 0 || m_index >= int(m_work.blocks.size()))
            return;
        LiveSequenceBlock& b = m_work.blocks[size_t(m_index)];
        if (m_mode == Resize) {
            const int end = std::clamp((tick + snap / 2) / snap * snap, b.start + snap, loop); // (to the nearest snap)
            b.length = std::max(1, end - b.start);
        } else { // the selection, by as much as the block held moves (snapped), all kept in the loop and on the rows
            const LiveSequenceBlock& held = m_orig[size_t(m_index)];
            int dt = (tick - m_grab + snap / 2) / snap * snap - held.start;
            int dr = std::clamp(int((e->position().y() - kRuler) / rowHeight()), 0, kRows - 1) - held.pad;
            for (int i : m_selected) {
                const LiveSequenceBlock& o = m_orig[size_t(i)];
                dt = std::clamp(dt, -o.start, loop - o.end());
                dr = std::clamp(dr, -o.pad, kRows - 1 - o.pad);
            }
            for (int i : m_selected) {
                m_work.blocks[size_t(i)].start = m_orig[size_t(i)].start + dt;
                m_work.blocks[size_t(i)].pad = m_orig[size_t(i)].pad + dr;
            }
        }
        pushLive();
    }

    void mouseReleaseEvent(QMouseEvent*) override
    {
        if (!m_dragging)
            return;
        m_dragging = false;
        if (m_mode == Select) {
            if (m_band.isNull() || (std::abs(m_band.width()) < 3 && std::abs(m_band.height()) < 3)) { // a click
                if (m_index >= 0 && !m_selected.erase(m_index))
                    m_selected.insert(m_index);
            } else {
                m_selected.clear();
                const QRectF band = m_band.normalized();
                const double px = pxPerTick();
                const int rh = rowHeight();
                for (size_t i = 0; i < m_work.blocks.size(); ++i) {
                    const LiveSequenceBlock& b = m_work.blocks[i];
                    if (band.intersects(QRectF(b.start * px, kRuler + b.pad * rh, b.length * px, rh)))
                        m_selected.insert(int(i));
                }
            }
            m_band = QRectF();
            m_index = -1;
            update();
            return;
        }
        if (m_mode == Resize && m_index >= 0 && m_index < int(m_work.blocks.size()))
            m_lastLength = m_work.blocks[size_t(m_index)].length;
        if (!(m_project->timeline().liveSequence == m_before.liveSequence))
            m_project->commitEdit(m_before, m_mode == Erase ? tr("Remove Sequence Blocks")
                                            : m_created           ? tr("Add Sequence Block")
                                            : m_mode == Move      ? (m_selected.size() > 1 ? tr("Move Sequence Blocks") : tr("Move Sequence Block"))
                                                                  : tr("Change Sequence Block Length"));
        m_known = m_project->timeline().liveSequence.blocks;
        m_index = -1;
        m_created = false;
        update();
    }

    bool event(QEvent* e) override
    {
        if (e->type() == QEvent::ShortcutOverride) { // (ahead of the menus' Delete, Select All, Copy...)
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Delete || k->key() == Qt::Key_Backspace || k->key() == Qt::Key_Escape || k->matches(QKeySequence::SelectAll)
                || k->matches(QKeySequence::Copy) || k->matches(QKeySequence::Cut) || k->matches(QKeySequence::Paste) || isDuplicate(k)) {
                e->accept();
                return true;
            }
        }
        return QWidget::event(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (m_dragging) {
            QWidget::keyPressEvent(e);
            return;
        }
        if (e->matches(QKeySequence::Copy) || e->matches(QKeySequence::Cut)) {
            copySelection();
            if (e->matches(QKeySequence::Cut))
                removeSelection(tr("Cut Sequence Blocks"));
        } else if (e->matches(QKeySequence::Paste)) {
            // At the snap under the pointer if it's over the grid; else after the selection (or at the 1).
            const QPoint at = mapFromGlobal(QCursor::pos());
            const int snap = seq().snapTicks();
            int where = rect().contains(at) && at.y() >= kRuler ? int(at.x() / pxPerTick()) / snap * snap : selectionSpan().second;
            insertBlocks(clipboard(), where, tr("Paste Sequence Blocks"));
        } else if (isDuplicate(e)) { // a copy of the selection straight after it (again and again: a row of them)
            const auto [first, end] = selectionSpan();
            const int snap = seq().snapTicks();
            std::vector<LiveSequenceBlock> copies;
            for (int i : m_selected) {
                LiveSequenceBlock b = seq().blocks[size_t(i)];
                b.start -= first;
                copies.push_back(b);
            }
            insertBlocks(copies, first + (end - first + snap - 1) / snap * snap, tr("Duplicate Sequence Blocks"));
        } else if (e->matches(QKeySequence::SelectAll)) {
            m_selected.clear();
            for (int i = 0; i < int(seq().blocks.size()); ++i)
                m_selected.insert(i);
        } else if (e->key() == Qt::Key_Escape) {
            m_selected.clear();
        } else if (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) {
            removeSelection(tr("Remove Sequence Blocks"));
        } else {
            QWidget::keyPressEvent(e);
            return;
        }
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
    enum Mode { Move, Resize, Erase, Select };
    // Copied blocks, their starts from the first's (shared by every sequencer: the same in its own window).
    static std::vector<LiveSequenceBlock>& clipboard()
    {
        static std::vector<LiveSequenceBlock> blocks;
        return blocks;
    }
    static bool isDuplicate(const QKeyEvent* k) { return k->key() == Qt::Key_D && (k->modifiers() & ~Qt::KeypadModifier) == Qt::ControlModifier; }
    // Where the selection starts and ends (ticks); none: {0, 0}.
    std::pair<int, int> selectionSpan() const
    {
        if (m_selected.empty())
            return {0, 0};
        int first = INT_MAX, end = 0;
        for (int i : m_selected) {
            first = std::min(first, seq().blocks[size_t(i)].start);
            end = std::max(end, seq().blocks[size_t(i)].end());
        }
        return {first, end};
    }
    void copySelection()
    {
        if (m_selected.empty())
            return;
        const int first = selectionSpan().first;
        clipboard().clear();
        for (int i : m_selected) {
            LiveSequenceBlock b = seq().blocks[size_t(i)];
            b.start -= first;
            clipboard().push_back(b);
        }
    }
    void removeSelection(const QString& description)
    {
        if (m_selected.empty())
            return;
        Timeline tl = m_project->timeline();
        std::vector<LiveSequenceBlock> kept;
        for (int i = 0; i < int(tl.liveSequence.blocks.size()); ++i)
            if (!m_selected.count(i))
                kept.push_back(tl.liveSequence.blocks[size_t(i)]);
        tl.liveSequence.blocks = std::move(kept);
        m_selected.clear();
        m_known = tl.liveSequence.blocks;
        m_project->applyEdit(tl, description);
    }
    // `blocks` (starts from `at`) added, as long as they fit in the loop; they become the selection.
    void insertBlocks(const std::vector<LiveSequenceBlock>& blocks, int at, const QString& description)
    {
        Timeline tl = m_project->timeline();
        LiveSequence& s = tl.liveSequence;
        m_selected.clear();
        for (LiveSequenceBlock b : blocks) {
            b.start += at;
            b.length = std::min(b.length, s.loopTicks() - b.start);
            if (b.start < 0 || b.length <= 0)
                continue;
            s.blocks.push_back(b);
            m_selected.insert(int(s.blocks.size()) - 1);
        }
        if (m_selected.empty())
            return;
        m_known = s.blocks;
        m_project->applyEdit(tl, description);
        update();
    }
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
    bool m_docked = true;
    std::set<int> m_selected;                 // indices into the blocks
    std::vector<LiveSequenceBlock> m_known;   // the blocks the selection is of
    std::vector<LiveSequenceBlock> m_orig;    // the blocks as a move started
    QPointF m_press;
    QRectF m_band;
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
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    m_content = new QWidget;
    outer->addWidget(m_content);
    m_placeholder = new QWidget;
    auto* placeholder = new QHBoxLayout(m_placeholder);
    placeholder->setContentsMargins(0, 4, 0, 0);
    placeholder->addWidget(new QLabel(tr("The sequencer is in its own window")), 1);
    auto* back = new QToolButton;
    back->setText(tr("Put Back"));
    back->setToolTip(tr("Put the sequencer back under the pads (or close its window)"));
    connect(back, &QToolButton::clicked, this, [this] { setPoppedOut(false); });
    placeholder->addWidget(back);
    m_placeholder->hide();
    outer->addWidget(m_placeholder);
    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(0, 4, 0, 0);
    layout->setSpacing(3);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(4);
    m_on = new QToolButton;
    m_on->setText(tr("Loop"));
    m_on->setCheckable(true);
    m_on->setObjectName(QStringLiteral("liveSequenceOn"));
    m_on->setToolTip(tr("Play the loop in time with the beat clock while the Live tab runs (from its next 1),\n"
                        "switching each block's pad on for its length"));
    m_record = new QToolButton;
    m_record->setText(QStringLiteral("\u25CF"));
    m_record->setCheckable(true);
    m_record->setObjectName(QStringLiteral("liveSequenceRecord"));
    m_record->setToolTip(tr("Record: while the loop plays, the pads you press (click, keys or MIDI) are written into it at the playhead,\n"
                            "to the snap, each for as long as it stays on, over what was there. Jam it in, then let it play"));
    connect(m_record, &QToolButton::toggled, this, &LiveSequencer::setRecording);
    theme::style(m_record, QStringLiteral("color: @bad;"));
    m_bars = new QComboBox;
    m_bars->setObjectName(QStringLiteral("liveSequenceBars"));
    for (int n : {1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 24, 32, 48, 64})
        m_bars->addItem(n == 1 ? tr("1 bar") : tr("%1 bars").arg(n), n);
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
    m_snap->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_snap->setMinimumContentsLength(4);
    m_snap->setToolTip(tr("Snap: where blocks start and end (T: triplets; a new block is as long as the last one made)"));
    m_clear = new QToolButton;
    m_clear->setText(tr("Clear"));
    m_clear->setToolTip(tr("Remove every block (Undo brings them back)"));
    m_popOut = new QToolButton;
    m_popOut->setText(QStringLiteral("⧉"));
    m_popOut->setCheckable(true);
    m_popOut->setObjectName(QStringLiteral("liveSequencePopOut"));
    m_popOut->setToolTip(tr("Pop Out: the sequencer in a window of its own, as big as you like (close it to put it back)"));
    connect(m_popOut, &QToolButton::toggled, this, &LiveSequencer::setPoppedOut);
    bar->addWidget(m_on);
    bar->addWidget(m_record);
    bar->addWidget(m_bars, 1);
    bar->addWidget(m_clear);
    bar->addWidget(m_popOut);
    layout->addLayout(bar);
    auto* bar2 = new QHBoxLayout;
    bar2->setSpacing(4);
    bar2->addWidget(new QLabel(tr("Time")));
    bar2->addWidget(m_top);
    bar2->addWidget(m_bottom);
    bar2->addSpacing(6);
    bar2->addWidget(new QLabel(tr("Snap")));
    bar2->addWidget(m_snap, 1);
    m_library = new QToolButton;
    m_library->setText(QStringLiteral("\u2261"));
    m_library->setObjectName(QStringLiteral("liveSequenceLibrary"));
    m_library->setToolTip(tr("Saved sequences: save this one, load one (its blocks, bars, time and snap), delete"));
    m_library->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(m_library);
    m_library->setMenu(menu);
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
        menu->clear();
        const QStringList saved = sequences::names();
        auto saveAs = [this](const QString& suggested) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Save Sequence"), tr("Name:"), QLineEdit::Normal, suggested, &ok).trimmed();
            if (!ok || name.isEmpty())
                return;
            if (name != m_name && sequences::names().contains(name)
                && QMessageBox::question(this, tr("Save Sequence"), tr("Replace the saved sequence \"%1\"?").arg(name)) != QMessageBox::Yes)
                return;
            QString error;
            if (!saveSequence(name, &error))
                QMessageBox::warning(this, tr("Save Sequence"), tr("Couldn't save it: %1").arg(error));
        };
        if (!m_name.isEmpty())
            menu->addAction(tr("Save \"%1\"").arg(m_name), this, [this] { saveSequence(m_name); });
        menu->addAction(tr("Save As..."), this, [saveAs, this] { saveAs(m_name); });
        menu->addSeparator();
        if (saved.isEmpty())
            menu->addAction(tr("(none saved yet)"))->setEnabled(false);
        for (const QString& name : saved) {
            QAction* a = menu->addAction(name, this, [this, name] { loadSequence(name); });
            a->setCheckable(true);
            a->setChecked(name == m_name);
        }
        if (!saved.isEmpty()) {
            menu->addSeparator();
            QMenu* del = menu->addMenu(tr("Delete"));
            for (const QString& name : saved)
                del->addAction(name, this, [this, name] {
                    if (QMessageBox::question(this, tr("Delete Sequence"), tr("Delete the saved sequence \"%1\"?").arg(name)) != QMessageBox::Yes)
                        return;
                    sequences::remove(name);
                    if (name == m_name)
                        m_name.clear();
                });
        }
    });
    bar2->addWidget(m_library);
    layout->addLayout(bar2);

    auto* body = new QHBoxLayout;
    body->setSpacing(0);
    m_scroll = new QScrollArea;
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_roll = new Roll(project, m_scroll);
    m_roll->setToolTip(tr("Click (and drag) to put a block on a pad's row; drag a block to move it, its right edge to change its length;\n"
                          "right-click (or right-drag) to remove. Ctrl+drag selects blocks (Ctrl+click one in or out, Ctrl+A all):\n"
                          "drag one to move them all, up and down onto other pads too; Delete removes them.\n"
                          "Ctrl+C / Ctrl+X copy and cut them, Ctrl+V pastes at the pointer, Ctrl+D duplicates them after themselves. Ctrl+wheel zooms"));
    m_scroll->setWidget(m_roll);
    m_scroll->setFixedHeight(kGridHeight + m_scroll->horizontalScrollBar()->sizeHint().height());
    m_scroll->viewport()->installEventFilter(this); // (its size: the zoom and, popped out, the rows)
    m_keys = new Keys(project, m_roll, [this](int pad) { emit padPressed(pad); });
    m_keys->setFixedHeight(kGridHeight);
    body->addWidget(m_keys, 0, Qt::AlignTop);
    body->addWidget(m_scroll, 1);
    layout->addLayout(body, 1);

    auto edits = [this](auto* w, auto signal, const QString& what, std::function<void(LiveSequence&)> change) {
        connect(w, signal, this, [this, what, change] { edit(change, what); });
    };
    connect(m_on, &QToolButton::toggled, this, [this](bool on) { edit([on](LiveSequence& s) { s.on = on; }, on ? tr("Sequence On") : tr("Sequence Off")); });
    edits(m_bars, qOverload<int>(&QComboBox::currentIndexChanged), tr("Sequence Length"), [this](LiveSequence& s) { s.bars = m_bars->currentData().toInt(); });
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

void LiveSequencer::setPoppedOut(bool popped)
{
    if (popped == poppedOut())
        return;
    if (popped) {
        m_window = new QWidget(this, Qt::Window);
        m_window->setAttribute(Qt::WA_DeleteOnClose);
        m_window->setWindowTitle(tr("Pad Sequencer"));
        m_window->setObjectName(QStringLiteral("liveSequencerWindow"));
        auto* layout = new QVBoxLayout(m_window);
        layout->setContentsMargins(6, 2, 6, 6);
        layout->addWidget(m_content);
        m_scroll->setMinimumHeight(kGridHeight / 2);
        m_scroll->setMaximumHeight(QWIDGETSIZE_MAX);
        m_window->installEventFilter(this); // (closed: put back)
        m_window->resize(std::max(900, width() * 2), 560);
        m_roll->setDocked(false);
        m_placeholder->show();
        m_window->show();
    } else {
        QWidget* window = m_window;
        m_window = nullptr;
        static_cast<QVBoxLayout*>(layout())->insertWidget(0, m_content);
        m_content->show();
        m_placeholder->hide();
        m_scroll->setFixedHeight(kGridHeight + m_scroll->horizontalScrollBar()->sizeHint().height());
        m_roll->setDocked(true);
        window->removeEventFilter(this);
        window->close();
    }
    const QSignalBlocker block(m_popOut);
    m_popOut->setChecked(popped);
    fitGrid();
}

bool LiveSequencer::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_window && event->type() == QEvent::Close) {
        setPoppedOut(false);
    } else if (watched == m_scroll->viewport() && event->type() == QEvent::Resize) {
        fitGrid();
    }
    return QWidget::eventFilter(watched, event);
}

void LiveSequencer::fitGrid()
{
    m_roll->fit();
    m_keys->setFixedHeight(m_roll->height());
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
    if (m_bars->findData(s.bars) < 0) { // (a length not in the list, from a file: put in its place)
        int at = 0;
        while (at < m_bars->count() && m_bars->itemData(at).toInt() < s.bars)
            ++at;
        m_bars->insertItem(at, tr("%1 bars").arg(s.bars), s.bars);
    }
    m_bars->setCurrentIndex(m_bars->findData(s.bars));
    m_top->setValue(s.beatsPerBar);
    m_bottom->setCurrentIndex(std::max(0, m_bottom->findData(s.beatUnit)));
    m_snap->setCurrentIndex(std::max(0, m_snap->findData(s.snap)));
    m_clear->setEnabled(!s.blocks.empty());
    if (!s.on && m_playing) {
        endTake();
        m_playing = false;
        m_roll->setPlayhead(-1);
    }
    m_timer.setInterval(8);
    if (s.on && m_running)
        m_timer.start();
    else
        m_timer.stop();
    fitGrid();
    m_keys->update();
}

void LiveSequencer::setRunning(bool running)
{
    endTake();
    m_running = running;
    m_playing = false; // (from the next 1)
    m_roll->setPlayhead(-1);
    refresh();
}

void LiveSequencer::setRecording(bool on)
{
    if (!on)
        endTake();
    const QSignalBlocker block(m_record);
    m_record->setChecked(on);
    theme::style(m_record, on ? QStringLiteral("background: @bad; color: white;") : QStringLiteral("color: @bad;"));
    if (on && !m_project->timeline().liveSequence.on) // (recording plays the loop)
        edit([](LiveSequence& s) { s.on = true; }, tr("Sequence On"));
}

bool LiveSequencer::saveSequence(const QString& name, QString* error)
{
    if (!sequences::save(name, m_project->timeline().liveSequence, error))
        return false;
    m_name = name.trimmed();
    return true;
}

bool LiveSequencer::loadSequence(const QString& name)
{
    const auto loaded = sequences::load(name);
    if (!loaded)
        return false;
    endTake();
    edit([&](LiveSequence& s) {
        const bool on = s.on; // (playing or not stays as it is)
        s = *loaded;
        s.on = on;
    }, tr("Load Sequence \"%1\"").arg(name));
    m_name = name;
    return true;
}

bool LiveSequencer::recording() const
{
    return m_record->isChecked();
}

// The take so far: one undo step.
void LiveSequencer::endTake()
{
    if (m_take < 0)
        return;
    m_take = -1;
    if (!(m_project->timeline().liveSequence == m_takeBefore.liveSequence))
        m_project->commitEdit(m_takeBefore, tr("Record Into Loop"));
}

void LiveSequencer::setActivePad(int pad)
{
    m_keys->setActive(pad);
    if (m_switching || !recording() || !m_playing)
        return; // (the loop's own switches, or not recording)
    endTake();
    const int at = playhead();
    if (pad < 0 || at < 0)
        return; // (switched off: the take ends there)
    Timeline tl = m_project->timeline();
    LiveSequence& s = tl.liveSequence;
    const int snap = s.snapTicks(), loop = s.loopTicks();
    const int start = std::min((at + snap / 2) / snap * snap, loop - 1) % loop; // (to the nearest snap)
    m_takeBefore = tl;
    s.blocks.push_back({pad, start, std::max(1, std::min(snap, loop - start))});
    m_take = int(s.blocks.size()) - 1;
    m_lastPad = pad;
    m_lastAt = at;
    m_project->setTimelineLive(tl);
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
    if (m_take < 0)
        m_lastAt = at;
    if (m_take >= 0 && m_take < int(m_project->timeline().liveSequence.blocks.size())) {
        // A pad held on while recording: its block grows with the playhead, over the blocks that start under it.
        Timeline tl = m_project->timeline();
        LiveSequence& s = tl.liveSequence;
        const int snap = s.snapTicks(), loop = s.loopTicks();
        LiveSequenceBlock take = s.blocks[size_t(m_take)];
        const bool wrapped = at < m_lastAt - loop / 2; // (round the loop: this take ends, another starts at its 1)
        m_lastAt = at;
        const int end = wrapped ? loop : std::clamp((at + snap - 1) / snap * snap, take.start + snap, loop);
        take.length = std::max(1, std::min(end, loop) - take.start);
        std::vector<LiveSequenceBlock> kept;
        for (int i = 0; i < int(s.blocks.size()); ++i) {
            const LiveSequenceBlock& b = s.blocks[size_t(i)];
            if (i == m_take || (b.start >= take.start && b.start < take.end()))
                continue;
            kept.push_back(b);
        }
        kept.push_back(take);
        if (kept != s.blocks) {
            s.blocks = std::move(kept);
            m_take = int(s.blocks.size()) - 1;
            m_project->setTimelineLive(tl);
        }
        if (wrapped) {
            endTake();
            m_takeBefore = m_project->timeline();
            Timeline next = m_takeBefore;
            next.liveSequence.blocks.push_back({take.pad, 0, std::max(1, std::min(snap, loop))});
            m_take = int(next.liveSequence.blocks.size()) - 1;
            m_project->setTimelineLive(next);
        }
        return; // (the pad pressed stays on: the loop doesn't switch over it)
    }
    const int pad = m_project->timeline().liveSequence.padAt(at);
    if (pad != m_lastPad) {
        m_lastPad = pad;
        m_switching = true;
        emit switchPad(pad);
        m_switching = false;
    }
}
