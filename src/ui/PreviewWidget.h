#pragma once

#include <QImage>
#include <QWidget>

class QPainter;

// Something edited right on the picture (an effect's mask): drawn over it and
// given the mouse. Positions are in the widget; `picture` is where the
// picture is shown in it.
class PreviewOverlay {
public:
    virtual ~PreviewOverlay() = default;
    virtual void paint(QPainter& p, const QRectF& picture) = 0;
    virtual bool press(QPointF pos, const QRectF& picture, Qt::KeyboardModifiers mods) = 0; // true: a drag starts
    virtual void move(QPointF pos, const QRectF& picture) = 0;
    virtual void release() = 0;
    virtual Qt::CursorShape cursor(QPointF pos, const QRectF& picture) const = 0;
    // A turn of the wheel over the picture (in notches); true: used.
    virtual bool wheel(QPointF, const QRectF&, double) { return false; }
};

// Shows the rendered preview frame, letterboxed into the widget.
class PreviewWidget : public QWidget {
    Q_OBJECT
public:
    explicit PreviewWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    const QImage& image() const { return m_image; }
    void setProjectAspect(double aspect);
    // Colour around the picture (default: the dark UI grey).
    void setBackground(const QColor& color);
    // An editor over the picture (not owned; null: none).
    void setOverlay(PreviewOverlay* overlay);
    PreviewOverlay* overlay() const { return m_overlay; }
    QRectF pictureRect() const;
    // Lines over the picture to frame by, never rendered: title and action safe areas, thirds, the centre, and where a
    // 9:16, 1:1 or 4:5 crop of it would be (outside it shaded). Or'ed together.
    enum Guide { SafeAreas = 1, Thirds = 2, Centre = 4, Crop9x16 = 8, Crop1x1 = 16, Crop4x5 = 32 };
    void setGuides(int guides);
    int guides() const { return m_guides; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    QImage m_image;
    double m_aspect = 16.0 / 9.0;
    QColor m_background{0x18, 0x18, 0x1a};
    PreviewOverlay* m_overlay = nullptr;
    bool m_dragging = false;
    int m_guides = 0;
    void paintGuides(QPainter& p, const QRectF& r) const;
};
