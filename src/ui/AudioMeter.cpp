#include "ui/AudioMeter.h"

#include "ui/Theme.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kFloorDb = -60.0;
constexpr double kDecayDbPerSec = 24.0;
constexpr qint64 kHoldMs = 1500;
}

AudioMeter::AudioMeter(QWidget* parent) : QWidget(parent)
{
    setMinimumWidth(180);
    setFixedHeight(26);
    setToolTip(tr("Master level (peak, dBFS). Red = clipping; click to reset."));
    m_clock.start();
}

void AudioMeter::setPeaks(float left, float right)
{
    const qint64 now = m_clock.elapsed();
    const double dt = (now - m_last) / 1000.0;
    m_last = now;
    const float peaks[2] = {left, right};
    for (int i = 0; i < 2; ++i) {
        Channel& c = m_ch[i];
        const double db = peaks[i] > 1e-6f ? 20.0 * std::log10(peaks[i]) : -90.0;
        c.level = std::max(db, c.level - kDecayDbPerSec * dt);
        if (db >= c.hold || now - c.holdTime > kHoldMs) {
            c.hold = db;
            c.holdTime = now;
        }
        if (peaks[i] >= 0.999f)
            c.clipped = true;
    }
    update();
}

void AudioMeter::mousePressEvent(QMouseEvent*)
{
    m_ch[0].clipped = m_ch[1].clipped = false;
    update();
}

void AudioMeter::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), theme::c().field);
    const int labelW = 12, clipW = 8;
    const QRectF track(labelW, 2, width() - labelW - clipW - 4, height() - 4);
    auto xFor = [&](double db) { return track.left() + track.width() * std::clamp((db - kFloorDb) / -kFloorDb, 0.0, 1.0); };
    const double barH = (track.height() - 2) / 2;
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.75);
    p.setFont(f);
    for (int i = 0; i < 2; ++i) {
        const Channel& c = m_ch[i];
        const QRectF bar(track.left(), track.top() + i * (barH + 2), track.width(), barH);
        p.fillRect(bar, theme::c().panel);
        // Green to -12 dB, yellow to -3 dB, red above.
        const double x = xFor(c.level);
        const double x12 = xFor(-12), x3 = xFor(-3);
        p.fillRect(QRectF(bar.left(), bar.top(), std::min(x, x12) - bar.left(), bar.height()), theme::c().good);
        if (x > x12)
            p.fillRect(QRectF(x12, bar.top(), std::min(x, x3) - x12, bar.height()), theme::c().warn);
        if (x > x3)
            p.fillRect(QRectF(x3, bar.top(), x - x3, bar.height()), theme::c().bad);
        if (c.hold > kFloorDb) {
            const double hx = xFor(c.hold);
            p.fillRect(QRectF(hx - 1, bar.top(), 2, bar.height()), theme::c().bright);
        }
        p.fillRect(QRectF(width() - clipW - 2, bar.top(), clipW, bar.height()), c.clipped ? theme::c().bad : theme::c().field.lighter(130));
        p.setPen(theme::c().dim);
        p.drawText(QRectF(0, bar.top() - 1, labelW, bar.height() + 2), Qt::AlignCenter, i == 0 ? QStringLiteral("L") : QStringLiteral("R"));
    }
    // dB ticks.
    p.setPen(QColor(0, 0, 0, 120));
    for (double db : {-48.0, -36.0, -24.0, -12.0, -6.0, -3.0})
        p.drawLine(QPointF(xFor(db), track.top()), QPointF(xFor(db), track.bottom()));
}
