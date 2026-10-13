#include "ui/PanCropView.h"

#include "ui/Theme.h"
#include "core/PanCrop.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>

PanCropView::PanCropView(QWidget* parent) : QWidget(parent)
{
    setMinimumHeight(200);
    setMouseTracking(true);
    setCursor(Qt::OpenHandCursor);
    setToolTip(tr("Drag to pan, drag a corner or scroll to zoom"));
}

void PanCropView::setPicture(const QImage& picture, QSizeF sourceSize)
{
    m_picture = picture;
    if (!sourceSize.isEmpty())
        m_source = sourceSize;
    update();
}

void PanCropView::setFraming(const Params& params, QSizeF canvas)
{
    m_params = params;
    if (!canvas.isEmpty())
        m_canvas = canvas;
    update();
}

QRectF PanCropView::pictureRect() const
{
    // Leave a margin so a zoomed-out frame (larger than the picture) fits.
    const QRectF area = QRectF(rect()).adjusted(width() * 0.15, height() * 0.15, -width() * 0.15, -height() * 0.15);
    const double k = std::min(area.width() / m_source.width(), area.height() / m_source.height());
    QRectF r(0, 0, m_source.width() * k, m_source.height() * k);
    r.moveCenter(area.center());
    return r;
}

QPolygonF PanCropView::visibleRegion() const
{
    bool ok = false;
    const QTransform inv = panCropTransform(m_params, m_source, m_canvas).inverted(&ok);
    if (!ok)
        return {};
    const QRectF pr = pictureRect();
    const double k = pr.width() / m_source.width();
    QPolygonF poly;
    for (QPointF c : {QPointF(0, 0), QPointF(m_canvas.width(), 0), QPointF(m_canvas.width(), m_canvas.height()),
                      QPointF(0, m_canvas.height())})
        poly << pr.topLeft() + inv.map(c) * k;
    return poly;
}

void PanCropView::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::c().field);
    const QRectF pr = pictureRect();
    if (m_picture.isNull())
        p.fillRect(pr, theme::c().raised);
    else {
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(pr, m_picture);
    }
    const QPolygonF region = visibleRegion();
    if (region.size() != 4)
        return;
    // Dim everything outside the visible region.
    QPainterPath outside;
    outside.addRect(rect());
    QPainterPath inside;
    inside.addPolygon(region);
    inside.closeSubpath();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillPath(outside.subtracted(inside), QColor(0, 0, 0, 150));
    p.setPen(QPen(QColor(0xff, 0xb3, 0x47), 2));
    p.drawPolygon(region);
    // Corner handles and centre cross.
    p.setBrush(QColor(0xff, 0xb3, 0x47));
    for (const QPointF& c : region)
        p.drawRect(QRectF(c - QPointF(4, 4), QSizeF(8, 8)));
    const QPointF centre = (region[0] + region[2]) / 2;
    p.drawLine(centre - QPointF(8, 0), centre + QPointF(8, 0));
    p.drawLine(centre - QPointF(0, 8), centre + QPointF(0, 8));
    // Mark the top edge so rotation/flips are readable.
    p.setPen(QPen(Qt::white, 3));
    p.drawLine(region[0] + (region[1] - region[0]) * 0.4, region[0] + (region[1] - region[0]) * 0.6);
}

void PanCropView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton)
        return;
    m_pressPos = e->position();
    m_startX = m_params.num(QStringLiteral("centerX"));
    m_startY = m_params.num(QStringLiteral("centerY"));
    m_startZoom = m_params.num(QStringLiteral("zoom"));
    const QPolygonF region = visibleRegion();
    m_drag = Drag::Pan;
    if (region.size() == 4) {
        const QPointF centre = (region[0] + region[2]) / 2;
        for (const QPointF& c : region)
            if (QLineF(c, m_pressPos).length() < 10) {
                m_drag = Drag::Zoom;
                m_startDist = std::max(1.0, QLineF(centre, m_pressPos).length());
            }
    }
    setCursor(m_drag == Drag::Pan ? Qt::ClosedHandCursor : Qt::SizeFDiagCursor);
}

void PanCropView::mouseMoveEvent(QMouseEvent* e)
{
    if (m_drag == Drag::None) {
        const QPolygonF region = visibleRegion();
        bool corner = false;
        for (const QPointF& c : region)
            corner |= QLineF(c, e->position()).length() < 10;
        setCursor(corner ? Qt::SizeFDiagCursor : Qt::OpenHandCursor);
        return;
    }
    const QRectF pr = pictureRect();
    const double k = pr.width() / m_source.width();
    if (m_drag == Drag::Pan) {
        const QPointF d = (e->position() - m_pressPos) / k; // source display units
        emit centerChanged(m_startX + d.x() / m_source.width(), m_startY + d.y() / m_source.height());
    } else {
        const QPolygonF region = visibleRegion();
        const QPointF centre = region.size() == 4 ? (region[0] + region[2]) / 2 : pr.center();
        // Dragging a corner outwards shows more of the picture: zoom out.
        const double dist = std::max(1.0, QLineF(centre, e->position()).length());
        const double base = std::max(1.0, QLineF(centre, m_pressPos).length());
        emit zoomChanged(std::clamp(m_startZoom * base / dist, 0.05, 20.0));
    }
}

void PanCropView::mouseReleaseEvent(QMouseEvent*)
{
    m_drag = Drag::None;
    setCursor(Qt::OpenHandCursor);
}

void PanCropView::wheelEvent(QWheelEvent* e)
{
    const double zoom = m_params.num(QStringLiteral("zoom")) * std::pow(1.0015, e->angleDelta().y());
    emit zoomChanged(std::clamp(zoom, 0.05, 20.0));
    e->accept();
}
