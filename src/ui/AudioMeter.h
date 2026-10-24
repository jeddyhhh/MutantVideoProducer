#pragma once

#include <QElapsedTimer>
#include <QWidget>

// Stereo peak meter (-60..0 dBFS) with decay, peak hold and clip lights.
// Click to reset the clip lights. With setVolume, a line on the same scale
// is the master volume: drag it (0 dB at the right, silence at the left),
// double-click it for 0 dB.
class AudioMeter : public QWidget {
    Q_OBJECT
public:
    explicit AudioMeter(QWidget* parent = nullptr);

    // Feed the linear peaks measured since the last call (0 when idle).
    void setPeaks(float left, float right);
    // Shows the master volume line at `level` (linear, 0..1).
    void setVolume(float level);

signals:
    // While dragging (`dragging`), then once more when the drag ends or on a double-click.
    void volumeChanged(float level, bool dragging);

public:

    QSize sizeHint() const override { return {260, 26}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;

private:
    struct Channel {
        double level = -90;   // displayed dB (with decay)
        double hold = -90;    // peak-hold dB
        qint64 holdTime = 0;
        bool clipped = false;
    };
    QRectF track() const;
    double volumeX() const;
    bool nearVolume(const QPointF& p) const;
    void dragVolume(const QPointF& p, bool dragging);
    void volumeToolTip();

    Channel m_ch[2];
    float m_volume = -1; // -1: no volume line
    bool m_dragging = false, m_moved = false;
    QElapsedTimer m_clock;
    qint64 m_last = 0;
};
