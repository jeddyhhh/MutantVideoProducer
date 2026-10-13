#pragma once

#include "core/Effects.h"

#include <QWidget>

// Rotary control for a numeric parameter: drag up/down (Shift
// for fine steps) or use the wheel; double-click resets to the default.
// Shows the parameter name above and the value below; compact knobs show
// only the value, to the right.
class Knob : public QWidget {
    Q_OBJECT
public:
    explicit Knob(const ParamDef& def, bool compact = false, QWidget* parent = nullptr);

    void setValue(double v);  // no signal
    double value() const { return m_value; }
    void setAnimated(bool animated); // has keyframes: value shown in the keyframe colour
    void setModulated(bool modulated); // Live: moves by itself (a dotted ring)
    const ParamDef& def() const { return m_def; }
    QString valueText() const;

    QSize sizeHint() const override;

signals:
    void valueEdited(double v); // user change (dragging, wheel, reset)

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    double toPos(double v) const; // 0..1 along the arc (log scale aware)
    double fromPos(double pos) const;
    void editTo(double v);

    ParamDef m_def;
    bool m_compact;
    double m_value = 0;
    bool m_animated = false;
    bool m_modulated = false;
    bool m_dragging = false;
    double m_pressPos = 0;
    int m_pressY = 0;
};
