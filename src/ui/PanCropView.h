#pragma once

#include "core/Effects.h"

#include <QImage>
#include <QWidget>

// Visual Event Pan/Crop editor: the source picture with the part that will
// fill the output frame outlined. Drag inside to pan, drag a corner (or use
// the mouse wheel) to zoom.
class PanCropView : public QWidget {
    Q_OBJECT
public:
    explicit PanCropView(QWidget* parent = nullptr);

    // Source picture (display oriented) and its size in source display units.
    void setPicture(const QImage& picture, QSizeF sourceSize);
    // Current pan/crop values and the output frame size.
    void setFraming(const Params& params, QSizeF canvas);

signals:
    void centerChanged(double x, double y);
    void zoomChanged(double zoom);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;

private:
    QRectF pictureRect() const;
    QPolygonF visibleRegion() const; // widget coordinates

    QImage m_picture;
    QSizeF m_source{16, 9};
    QSizeF m_canvas{16, 9};
    Params m_params;

    enum class Drag { None, Pan, Zoom } m_drag = Drag::None;
    QPointF m_pressPos;
    double m_startX = 0.5, m_startY = 0.5, m_startZoom = 1.0, m_startDist = 1.0;
};
