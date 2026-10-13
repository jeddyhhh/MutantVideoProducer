#pragma once

#include <QElapsedTimer>
#include <QWidget>

// Stereo peak meter (-60..0 dBFS) with decay, peak hold and clip lights.
// Click to reset the clip lights.
class AudioMeter : public QWidget {
    Q_OBJECT
public:
    explicit AudioMeter(QWidget* parent = nullptr);

    // Feed the linear peaks measured since the last call (0 when idle).
    void setPeaks(float left, float right);

    QSize sizeHint() const override { return {260, 26}; }

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;

private:
    struct Channel {
        double level = -90;   // displayed dB (with decay)
        double hold = -90;    // peak-hold dB
        qint64 holdTime = 0;
        bool clipped = false;
    };
    Channel m_ch[2];
    QElapsedTimer m_clock;
    qint64 m_last = 0;
};
