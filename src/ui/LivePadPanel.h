#pragma once

#include "core/Project.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <array>
#include <vector>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QDoubleSpinBox;
class QToolButton;

// The Live tab's 4x4 pads, left of the picture. Each pad holds an effect
// chain (saved with the project) that is applied on top of the Live FX:
// pressing a pad switches its chain on, with the chosen transition from
// whatever was on before; pressing it again switches it off. Effects and
// presets can be dropped on a pad; right-click a pad to edit, rename, lock,
// copy the Live FX into it or clear it. Locked pads take no edits.
//
// Sets: whole grids saved by name (core/LivePadSets.h) and loaded or stepped
// through from the Set row. Auto switch: steps to the next pad (in order or
// at random) every few beats, from a tempo (typed or tapped) or from beats
// heard on the Live tab's Audio input.
class LivePadPanel : public QWidget {
    Q_OBJECT
public:
    explicit LivePadPanel(Project* project, QWidget* parent = nullptr);

    int activePad() const { return m_active; }
    // The pad being previewed in the Cue window (-1 = none; just shows it).
    void setCuedPad(int pad);
    int cuedPad() const { return m_cued; }
    void setActivePad(int pad); // -1 = none (just shows it)
    // Momentary pads held down now (just shows them).
    void setHeldPads(const std::vector<int>& pads);
    bool isMomentary(int pad) const;
    void setMomentary(int pad, bool momentary);
    // Chain shown in the device chain panel: 0 = Live FX, 1..16 = pad, 17.. = the layers' FX (layer 1 first).
    static constexpr int kLayerTarget = kLivePads + 1;
    int editing() const { return m_editing; }
    void setEditing(int target);
    // Follow: switching a pad on shows its chain (and off, the Live FX again).
    bool followsActivePad() const;
    // True while auto switch is stepping (its switches aren't followed).
    bool autoStepping() const { return m_stepping; }
    // Transition between pads (type empty = cut) and its length.
    TransitionSpec transition() const;
    double transitionSeconds() const;

    bool isLocked(int pad) const;
    // Fills a pad with a random effect chain of tier 0 Low .. 3 Insane (and
    // names it). seed 0 = a new random one. False if the pad is locked.
    bool randomizePad(int pad, int tier, uint32_t seed = 0);
    // The same for every empty, unlocked pad; returns how many were filled.
    int randomizeEmptyPads(int tier, bool all = false); // (all: every unlocked pad, its effects replaced)
    void setLocked(int pad, bool locked);

    // Sets.
    QString currentSet() const { return m_setName; }
    bool loadSet(const QString& name);
    bool saveSetAs(const QString& name, QString* error = nullptr);
    bool setModified() const;

    // Auto switch: 0 off, 1 tempo, 2 audio beats.
    enum AutoMode { AutoOff, AutoTempo, AutoAudio, AutoMidi };
    void setAutoMode(int mode);
    void setRunning(bool running); // steps only while capturing
    // A beat heard on the audio input (the Live engine's beat tracker).
    void audioBeat(double bpm, double confidence);
    // A beat of a MIDI clock (Auto: MIDI clock), `barBeat` into its bar (0 = the 1).
    void clockBeat(double bpm, int barBeat);
    // Presses a pad (mouse, key or MIDI): at once, or on the next beat / bar
    // (Launch), so switches land in time. Momentary pads are always at once.
    void pressPad(int pad);
    enum Launch { LaunchNow, LaunchBeat, LaunchBar };
    QComboBox* launchBox() const { return m_launch; }
    int pendingPad() const { return m_pendingPad; }
    // The tracker the Tune window adjusts and shows.
    void setBeatTracker(class BeatTracker* tracker);
    void showBeatTuner();
    // Lock BPM: the tempo heard stops changing (the beats still follow the music).
    void setTempoLocked(bool locked);
    // Find Tempo Again: forgets the tempo (and the lock, and an octave set by hand) and listens afresh.
    void findTempoAgain();
    // A song's tempo (setlists): the last one forgotten, then `bpm` locked, or
    // shown and listened for from there; 0 = Find Tempo Again.
    void setSongTempo(double bpm, bool lock);
    // x2 / /2 (of the tempo heard, or the one typed).
    void scaleTempo(double factor);
    // Set 1: the beat just heard (or the next) is the 1 of a bar; beats well off move to it.
    void setDownbeat();
    int barBeat() const; // 1..4: where the last beat was in its bar
    void tap();
    double bpm() const;

    QPushButton* padButton(int pad) const { return m_buttons[size_t(pad)]; }
    QComboBox* transitionBox() const { return m_transition; }
    QComboBox* durationBox() const { return m_duration; }
    QComboBox* everyBox() const { return m_every; }
    QDoubleSpinBox* bpmBox() const { return m_bpm; }
    class LiveSequencer* sequencer() const { return m_sequencer; }
    QCheckBox* randomBox() const { return m_random; }
    QToolButton* lockBpmButton() const { return m_lockBpm; }
    QToolButton* findTempoButton() const { return m_findTempo; }

signals:
    void padPressed(int pad);
    // Shift+click (or right-click > Cue): preview a pad without putting it live.
    void padCued(int pad);
    // A momentary pad pressed down / let go (mouse).
    void padHeld(int pad, bool down);
    // The beat clock: a beat at `bpm`, `barBeat` into its bar (0 = the 1). It
    // always runs: from the audio beats when there are any (Auto off or Audio
    // beats), from the tempo otherwise; Auto decides whether pads switch.
    void beatTicked(double bpm, int barBeat);
    // The tempo the clock runs at changed (typed, tapped, halved...).
    void tempoChanged(double bpm);
    // Right-click > Learn MIDI on a pad ("pad:3") or a button ("tap", "setone", "findtempo", "lockbpm").
    void midiLearnRequested(const QString& action, const QString& label);
    void editRequested(int target);
    // An effect (EffectsBrowser::kEffectMime) or preset (kPresetMime) dropped on a pad.
    void dropped(int pad, const QString& mimeType, const QString& id);
    // A set replaced the pads.
    void setLoaded(const QString& name);
    // Something worth a line in the status bar ("pad 3 is locked").
    void message(const QString& text);

private:
    void refresh();
    void refreshSets();
    void showMenu(int pad, const QPoint& globalPos);
    void showSetMenu();
    void editPad(int pad, const std::function<void(LivePad&)>& change, const QString& description);
    void stepSet(int delta);
    void beat();
    void flash(bool downbeat);
    double currentBpm() const;
    void step();
    void updateAutoTimer();

    Project* m_project;
    std::array<QPushButton*, 16> m_buttons{};
    QComboBox* m_edit;
    QCheckBox* m_follow;
    bool m_stepping = false;
    QComboBox* m_transition;
    QComboBox* m_duration;
    int m_active = -1;
    int m_cued = -1;
    int m_editing = 0;
    // Sets.
    QComboBox* m_set;
    QToolButton* m_setMenu;
    QString m_setName;
    LivePads m_loadedPads; // the pads as the set put them (to show unsaved changes)
    // Auto switch.
    QComboBox* m_auto;
    QComboBox* m_every;
    QDoubleSpinBox* m_bpm;
    QPushButton* m_tap;
    QCheckBox* m_random;
    QLabel* m_led;
    QToolButton* m_tune;
    QToolButton* m_lockBpm;
    QToolButton* m_findTempo;
    QComboBox* m_launch;
    class LiveSequencer* m_sequencer;
    int m_sequenced = -1; // the pad the sequencer last switched on
    int m_pendingPad = -1; // pressed, waiting for the next beat / bar
    QToolButton* m_half;
    QToolButton* m_double;
    QToolButton* m_downbeat;
    class BeatTuner* m_tuner = nullptr;
    class BeatTracker* m_tracker = nullptr;
    QTimer m_beatTimer, m_ledTimer;
    QElapsedTimer m_clock;
    std::vector<qint64> m_taps;
    int m_beatIndex = 15;  // beats since the 1 (0 = on it), over 16
    std::vector<int> m_heldPads;
    qint64 m_lastAudioBeatMs = -1000000;
    qint64 m_lastBeatMs = -1;
    bool m_running = false;
};
