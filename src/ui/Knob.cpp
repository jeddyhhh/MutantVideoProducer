#include "ui/Knob.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kStartDeg = 225.0; // arc runs clockwise from 225 to -45 degrees
constexpr double kSweepDeg = 270.0;
constexpr int kDragPixels = 200;    // full range per 200 px of vertical drag
} // namespace

Knob::Knob(const ParamDef& def, bool compact, QWidget* parent) : QWidget(parent), m_def(def), m_compact(compact), m_value(def.def)
{
    setToolTip(def.tooltip.isEmpty() ? def.name : def.name + QStringLiteral("\n") + def.tooltip);
    setFocusPolicy(Qt::NoFocus);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize Knob::sizeHint() const
{
    return m_compact ? QSize(84, 24) : QSize(62, 66);
}

double Knob::toPos(double v) const
{
    if (m_def.logScale && m_def.min > 0)
        return std::clamp(std::log(std::max(v, m_def.min) / m_def.min) / std::log(m_def.max / m_def.min), 0.0, 1.0);
    return std::clamp((v - m_def.min) / std::max(1e-12, m_def.max - m_def.min), 0.0, 1.0);
}

double Knob::fromPos(double pos) const
{
    pos = std::clamp(pos, 0.0, 1.0);
    double v = m_def.logScale && m_def.min > 0 ? m_def.min * std::pow(m_def.max / m_def.min, pos) : m_def.min + (m_def.max - m_def.min) * pos;
    const double step = std::pow(10.0, -m_def.decimals);
    return std::clamp(std::round(v / step) * step, m_def.min, m_def.max);
}

void Knob::setValue(double v)
{
    if (v == m_value)
        return;
    m_value = v;
    update();
}

void Knob::setAnimated(bool animated)
{
    if (animated == m_animated)
        return;
    m_animated = animated;
    update();
}

void Knob::setModulated(bool modulated)
{
    if (m_modulated == modulated)
        return;
    m_modulated = modulated;
    update();
}

QString Knob::valueText() const
{
    return QString::number(m_value, 'f', m_def.decimals) + m_def.suffix;
}

void Knob::editTo(double v)
{
    v = std::clamp(v, m_def.min, m_def.max);
    if (v == m_value)
        return;
    m_value = v;
    update();
    emit valueEdited(v);
}

void Knob::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const QColor valueColor = m_animated ? theme::c().key : theme::c().accent;

    QRectF knob;
    if (m_compact) {
        knob = QRectF(3, 3, height() - 6, height() - 6);
    } else {
        p.setPen(theme::c().text);
        p.drawText(QRectF(0, 0, width(), 14), Qt::AlignCenter, p.fontMetrics().elidedText(m_def.name, Qt::ElideRight, width()));
        const double d = 30;
        knob = QRectF((width() - d) / 2, 17, d, d);
    }
    // Track, then the value arc (from 0 for bipolar ranges).
    const double pos = toPos(m_value);
    const double origin = (m_def.min < 0 && m_def.max > 0 && !m_def.logScale) ? toPos(0.0) : 0.0;
    p.setPen(QPen(theme::c().deep, m_compact ? 3 : 4, Qt::SolidLine, Qt::FlatCap));
    p.drawArc(knob, int(kStartDeg * 16), int(-kSweepDeg * 16));
    p.setPen(QPen(isEnabled() ? valueColor : theme::c().faint, m_compact ? 3 : 4, Qt::SolidLine, Qt::FlatCap));
    const double a0 = kStartDeg - origin * kSweepDeg, a1 = kStartDeg - pos * kSweepDeg;
    p.drawArc(knob, int(a0 * 16), int((a1 - a0) * 16));
    // Needle.
    const double rad = a1 * 3.14159265358979 / 180.0;
    const QPointF c = knob.center();
    const double r = knob.width() / 2;
    p.setPen(QPen(theme::c().bright, 2, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c + QPointF(std::cos(rad) * r * 0.25, -std::sin(rad) * r * 0.25), c + QPointF(std::cos(rad) * r * 0.9, -std::sin(rad) * r * 0.9));
    if (m_modulated) { // moves by itself: a dotted ring around the knob
        p.setPen(QPen(theme::c().info, 1.5, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(knob.adjusted(-3.5, -3.5, 3.5, 3.5));
    }

    p.setPen(valueColor);
    if (m_compact)
        p.drawText(QRectF(knob.right() + 4, 0, width() - knob.right() - 4, height()), Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(valueText(), Qt::ElideRight, int(width() - knob.right() - 4)));
    else
        p.drawText(QRectF(0, height() - 16, width(), 16), Qt::AlignCenter, p.fontMetrics().elidedText(valueText(), Qt::ElideRight, width()));
}

void Knob::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    m_dragging = true;
    m_pressPos = toPos(m_value);
    m_pressY = int(e->position().y());
    setCursor(Qt::BlankCursor);
}

void Knob::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_dragging)
        return;
    const double scale = (e->modifiers() & Qt::ShiftModifier) ? 0.1 : 1.0;
    const double pos = m_pressPos + double(m_pressY - e->position().y()) / kDragPixels * scale;
    editTo(fromPos(pos));
}

void Knob::mouseReleaseEvent(QMouseEvent*)
{
    if (m_dragging)
        unsetCursor();
    m_dragging = false;
}

void Knob::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton)
        editTo(m_def.def);
}

void Knob::wheelEvent(QWheelEvent* e)
{
    const double notches = e->angleDelta().y() / 120.0;
    const double step = (e->modifiers() & Qt::ShiftModifier) ? 0.002 : 0.02;
    editTo(fromPos(toPos(m_value) + notches * step));
    e->accept();
}
