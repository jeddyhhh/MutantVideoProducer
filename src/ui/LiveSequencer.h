#pragma once

#include "core/LiveSequence.h"
#include "core/Project.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <set>
#include <vector>

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
    void setHeldPads(const std::vector<int>& pads); // the momentary pads held now (recorded as hold blocks)
    bool playing() const { return m_playing; }
    // In a window of its own (its rows as tall as the window lets them be); closing that window puts it back.
    void setPoppedOut(bool popped);
    // Record: while the loop plays, the pads pressed are written into it at the playhead (to the snap), each for as
    // long as it stays on, over what was there.
    void setRecording(bool on);
    bool recording() const;
    // The saved sequences (core/LiveSequence.h): this one saved by name; one loaded in its place (Undo brings it back).
    bool saveSequence(const QString& name, QString* error = nullptr);
    bool loadSequence(const QString& name);
    bool poppedOut() const { return m_window != nullptr; }
    int playhead() const;            // ticks into the loop (-1: not playing)

signals:
    // The pad the loop has on now (-1: none): LivePadPanel switches it (and off only what it switched on).
    void switchPad(int pad);
    // A momentary pad's block starts (down) or ends: held on top of the active pad for its length.
    void holdPad(int pad, bool down);
    // A pad's name clicked: press it, as on the pad.
    void padPressed(int pad);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void refresh();
    void fitGrid();
    void startTake(int pad, bool hold);
    void endTake(int pad, bool hold); // (-1, false: the switched-on one)
    void endTakes();                  // every take
    void commitTakes();               // the takes since none were going, as one undo step
    void releaseHolds();              // the loop's holds let go (it stopped)
    bool isHold(int pad) const;
    void edit(const std::function<void(LiveSequence&)>& change, const QString& description);
    void tick();

    Project* m_project;
    QToolButton* m_on;
    QComboBox* m_bars;
    QToolButton* m_record;
    QSpinBox* m_top;
    QComboBox* m_bottom;
    QComboBox* m_snap;
    QToolButton* m_clear;
    QToolButton* m_popOut;
    QToolButton* m_library;
    QString m_name;           // the saved sequence last loaded or saved
    QWidget* m_content;       // the toolbar and the roll: here, or in m_window
    QWidget* m_placeholder;   // what's here while it's in its window
    QWidget* m_window = nullptr;
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
    bool m_switching = false; // (the loop's own switch or hold: not recorded)
    // Recording: the blocks growing now, a pad switched on (one) and pads held (any); found by pad and start.
    struct Take {
        int pad;
        int start;
        bool hold;
    };
    std::vector<Take> m_takes;
    std::set<int> m_loopHeld;  // momentary pads the loop holds now
    std::vector<int> m_held;   // all held now (the loop's and the player's)
    int m_lastAt = 0;          // the playhead at the last tick (to see the loop come round)
    Timeline m_takeBefore;    // the project before it (one undo step a take)
    std::weak_ptr<const int> m_takeSession; // (Project::session(): the take's project)
};
