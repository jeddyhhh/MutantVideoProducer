#include "ui/PreviewWidget.h"

#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>

PreviewWidget::PreviewWidget(QWidget* parent) : QWidget(parent)
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 90);
}

void PreviewWidget::setImage(const QImage& image)
{
    m_image = image;
    update();
}

void PreviewWidget::setProjectAspect(double aspect)
{
    if (aspect > 0)
        m_aspect = aspect;
    update();
}

void PreviewWidget::setBackground(const QColor& color)
{
    m_background = color;
    update();
}

void PreviewWidget::setOverlay(PreviewOverlay* overlay)
{
    if (m_overlay == overlay)
        return;
    if (m_dragging && m_overlay)
        m_overlay->release();
    m_dragging = false;
    m_overlay = overlay;
    setMouseTracking(overlay != nullptr);
    if (!overlay)
        unsetCursor();
    update();
}

QRectF PreviewWidget::pictureRect() const
{
    const double aspect = m_image.isNull() ? m_aspect : double(m_image.width()) / m_image.height();
    QRectF r(0, 0, width(), width() / aspect);
    if (r.height() > height())
        r = QRectF(0, 0, height() * aspect, height());
    r.moveCenter(QRectF(rect()).center());
    return r;
}

void PreviewWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), m_background);
    const QRectF r = pictureRect();
    if (m_image.isNull()) {
        p.fillRect(r, Qt::black);
        return;
    }
    // Smooth scaling only when shrinking a lot; nearest is cheaper and fine otherwise.
    p.setRenderHint(QPainter::SmoothPixmapTransform, r.width() < m_image.width() * 0.75);
    p.drawImage(r, m_image);
    if (m_overlay) {
        p.setRenderHint(QPainter::Antialiasing);
        m_overlay->paint(p, r);
    }
}

void PreviewWidget::mousePressEvent(QMouseEvent* e)
{
    if (m_overlay && e->button() == Qt::LeftButton && m_overlay->press(e->position(), pictureRect(), e->modifiers())) {
        m_dragging = true;
        return;
    }
    QWidget::mousePressEvent(e);
}

void PreviewWidget::mouseMoveEvent(QMouseEvent* e)
{
    if (!m_overlay)
        return QWidget::mouseMoveEvent(e);
    if (m_dragging)
        m_overlay->move(e->position(), pictureRect());
    else
        setCursor(m_overlay->cursor(e->position(), pictureRect()));
}

void PreviewWidget::wheelEvent(QWheelEvent* e)
{
    if (m_overlay && m_overlay->wheel(e->position(), pictureRect(), e->angleDelta().y() / 120.0)) {
        e->accept();
        update();
        return;
    }
    QWidget::wheelEvent(e);
}

void PreviewWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (m_dragging && m_overlay) {
        m_dragging = false;
        m_overlay->release();
        return;
    }
    QWidget::mouseReleaseEvent(e);
}
