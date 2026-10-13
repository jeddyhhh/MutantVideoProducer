#pragma once

#include "core/BeatTracker.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>

class QComboBox;
class QLabel;
class Knob;
class BeatGraph;

// "Beat Detection" (Live tab, Auto: Audio beats > Tune): the knobs of the
// beat tracker with a live view of what it hears (the onset curve, the
// threshold a hit must cross, hits and beats), the tempo and how sure it is.
// Settings are kept between sessions.
class BeatTuner : public QWidget {
    Q_OBJECT
public:
    explicit BeatTuner(QWidget* parent = nullptr);

    // The tracker being tuned (the Live engine's); may be null.
    void setTracker(BeatTracker* tracker);
    BeatSettings settings() const { return m_settings; }
    void setSettings(const BeatSettings& s); // updates the controls and the tracker

    // UI-thread time spent updating and drawing the window (ms, for tests).
    double busyMs() const;

    static BeatSettings loadSettings();
    static void saveSettings(const BeatSettings& s);

signals:
    void settingsChanged(const BeatSettings& s);
    // Find Tempo Again: the tempo was forgotten (and unlocked).
    void tempoForgotten();

private:
    void apply();
    void refresh();

    BeatTracker* m_tracker = nullptr;
    BeatSettings m_settings;
    QComboBox* m_preset;
    QComboBox* m_band;
    QComboBox* m_mode;
    Knob* m_sensitivity;
    Knob* m_gap;
    Knob* m_minBpm;
    Knob* m_maxBpm;
    Knob* m_stability;
    Knob* m_window;
    Knob* m_latency;
    QLabel* m_bpm;
    QLabel* m_confidence;
    BeatGraph* m_graph;
    QTimer m_timer;
    bool m_updating = false;
    double m_busyMs = 0;
    int m_confidenceLevel = -1; // colour shown: 0 low, 1 fair, 2 good
};
