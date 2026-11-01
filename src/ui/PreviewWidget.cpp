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
    if (m_guides)
        paintGuides(p, r);
    if (m_overlay) {
        p.setRenderHint(QPainter::Antialiasing);
        m_overlay->paint(p, r);
    }
}

void PreviewWidget::setGuides(int guides)
{
    if (guides == m_guides)
        return;
    m_guides = guides;
    update();
}

void PreviewWidget::paintGuides(QPainter& p, const QRectF& r) const
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing, false);
    // Crops for other shapes, centred: what is outside them is shaded, each with its own colour of border.
    const struct {
        Guide guide;
        double aspect;
        QColor colour;
        const char* name;
    } crops[] = {{Crop9x16, 9.0 / 16.0, QColor(255, 196, 64), "9:16"}, {Crop4x5, 4.0 / 5.0, QColor(110, 220, 255), "4:5"}, {Crop1x1, 1.0, QColor(170, 255, 120), "1:1"}};
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    for (const auto& c : crops) {
        if (!(m_guides & c.guide))
            continue;
        QRectF box = r;
        if (c.aspect < r.width() / r.height())
            box.setWidth(r.height() * c.aspect);
        else
            box.setHeight(r.width() / c.aspect);
        box.moveCenter(r.center());
        p.fillRect(QRectF(r.left(), r.top(), box.left() - r.left(), r.height()), QColor(0, 0, 0, 70));
        p.fillRect(QRectF(box.right(), r.top(), r.right() - box.right(), r.height()), QColor(0, 0, 0, 70));
        p.fillRect(QRectF(box.left(), r.top(), box.width(), box.top() - r.top()), QColor(0, 0, 0, 70));
        p.fillRect(QRectF(box.left(), box.bottom(), box.width(), r.bottom() - box.bottom()), QColor(0, 0, 0, 70));
        p.setPen(QPen(c.colour, 1.5));
        p.drawRect(box.adjusted(0, 0, -1, -1));
        p.drawText(box.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, QString::fromLatin1(c.name));
    }
    const QColor line(255, 255, 255, 150);
    if (m_guides & SafeAreas) { // (EBU R 95: action safe 93%, title safe 90% of the picture)
        for (const auto& [share, style] : {std::pair{0.93, Qt::DashLine}, std::pair{0.90, Qt::SolidLine}}) {
            QRectF safe(0, 0, r.width() * share, r.height() * share);
            safe.moveCenter(r.center());
            p.setPen(QPen(line, 1, style));
            p.drawRect(safe);
        }
    }
    if (m_guides & Thirds) {
        p.setPen(QPen(line, 1, Qt::DotLine));
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(r.left() + r.width() * i / 3, r.top()), QPointF(r.left() + r.width() * i / 3, r.bottom()));
            p.drawLine(QPointF(r.left(), r.top() + r.height() * i / 3), QPointF(r.right(), r.top() + r.height() * i / 3));
        }
    }
    if (m_guides & Centre) {
        const double s = std::min(r.width(), r.height()) * 0.04;
        p.setPen(QPen(line, 1));
        p.drawLine(QPointF(r.center().x() - s, r.center().y()), QPointF(r.center().x() + s, r.center().y()));
        p.drawLine(QPointF(r.center().x(), r.center().y() - s), QPointF(r.center().x(), r.center().y() + s));
    }
    p.restore();
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
