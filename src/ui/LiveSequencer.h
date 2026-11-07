#pragma once

#include "core/LiveSequence.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <functional>

class Project;
class QComboBox;
class QScrollArea;
class QSpinBox;
class QToolButton;

namespace livesequencer {
class Roll;
class Keys;
} // namespace livesequencer

// The Live tab's pad sequencer, under the pads: a looping piano roll whose rows are the 16 pads (their names down
// the left). Click (and drag) on the grid to put a block on a pad's row, drag a block to move it or its right edge
// to change its length, right-click (or right-drag) to remove. With Sequence on and the Live tab running, the loop
// plays in time with the beat clock from its next 1, switching each block's pad on for its length. The blocks,
// bars, time signature and snap are saved with the project (core/LiveSequence.h).
class LiveSequencer : public QWidget {
    Q_OBJECT
public:
    explicit LiveSequencer(Project* project, QWidget* parent = nullptr);

    // The beat clock (LivePadPanel::beatTicked): a beat at `bpm`, `barBeat` into its bar (0 = the 1).
    void beat(double bpm, int barBeat);
    void setRunning(bool running);   // plays only while the Live tab runs
    void setActivePad(int pad);      // what's live now (shown on its row's name)
    bool playing() const { return m_playing; }
    int playhead() const;            // ticks into the loop (-1: not playing)

signals:
    // The pad the loop has on now (-1: none): LivePadPanel switches it (and off only what it switched on).
    void switchPad(int pad);
    // A pad's name clicked: press it, as on the pad.
    void padPressed(int pad);

private:
    void refresh();
    void edit(const std::function<void(LiveSequence&)>& change, const QString& description);
    void tick();

    Project* m_project;
    QToolButton* m_on;
    QSpinBox* m_bars;
    QSpinBox* m_top;
    QComboBox* m_bottom;
    QComboBox* m_snap;
    QToolButton* m_clear;
    QScrollArea* m_scroll;
    livesequencer::Roll* m_roll;
    livesequencer::Keys* m_keys;
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_running = false;
    bool m_playing = false;
    int m_beats = 0;          // beats since the loop started
    qint64 m_lastBeatMs = 0;
    double m_bpm = 120;
    int m_lastPad = -1;       // the pad the loop last asked for
};
