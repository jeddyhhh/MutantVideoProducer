#pragma once

#include "core/Project.h"

#include <QDialog>
#include <set>

class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// Event properties: playback rate, reverse, frame blending, audio pitch.
// Applies to all given events (e.g. a grouped video + audio pair).
class EventPropertiesDialog : public QDialog {
    Q_OBJECT
public:
    EventPropertiesDialog(Project* project, std::set<int> clipIds, QWidget* parent = nullptr);

    // Applies the dialog's values as one undo step.
    void apply();

private:
    void updateInfo();

    Project* m_project;
    std::set<int> m_ids;
    QDoubleSpinBox* m_rate;
    QCheckBox* m_reverse;
    QCheckBox* m_keepSpan;
    QCheckBox* m_blend;
    QCheckBox* m_flow;
    QCheckBox* m_pitch;
    QLabel* m_info;
    TimeUs m_length = 0;
    TimeUs m_span = 0;
    double m_oldSpeed = 1.0;
};
