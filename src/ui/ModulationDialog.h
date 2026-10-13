#pragma once

#include "core/Effects.h"

#include <QDialog>
#include <optional>

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;
class QToolButton;

// Right-click a Live tab knob > Modulate...: what moves the setting (the
// beat, the bar, an LFO synced to the tempo, the audio level, bass or highs,
// or a MIDI controller), how far and how fast.
class ModulationDialog : public QDialog {
    Q_OBJECT
public:
    ModulationDialog(const ParamDef& def, const std::optional<Modulation>& current, QWidget* parent = nullptr);

    // After exec(): the modulation to set (nullopt: none, removed).
    std::optional<Modulation> result() const { return m_result; }

private:
    void updateRows();
    Modulation current() const;

    ParamDef m_def;
    QComboBox* m_source;
    QComboBox* m_shape;
    QComboBox* m_length;
    QDoubleSpinBox* m_amount;
    QSpinBox* m_channel;
    QSpinBox* m_cc;
    QToolButton* m_learn;
    QLabel* m_help;
    QWidget* m_shapeRow;
    QWidget* m_lengthRow;
    QWidget* m_amountRow;
    QWidget* m_midiRow;
    std::optional<Modulation> m_result;
};
