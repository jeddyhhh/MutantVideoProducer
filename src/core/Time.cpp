#include "core/Time.h"

#include "core/FFmpeg.h"

#include <cstdlib>

TimeUs FrameRate::timeOf(int64_t frame) const
{
    return av_rescale_rnd(frame, int64_t(den) * kUsPerSecond, num, AV_ROUND_UP);
}

int64_t FrameRate::frameAt(TimeUs t) const
{
    return av_rescale_rnd(t, num, int64_t(den) * kUsPerSecond, AV_ROUND_DOWN);
}

TimeUs FrameRate::snap(TimeUs t) const
{
    return timeOf(av_rescale_rnd(t, num, int64_t(den) * kUsPerSecond, AV_ROUND_NEAR_INF));
}

QString FrameRate::toString() const
{
    if (den == 1)
        return QString::number(num);
    return QString::number(toDouble(), 'f', 3);
}

QString formatTimecode(TimeUs t, const FrameRate& fps)
{
    const bool negative = t < 0;
    if (negative)
        t = -t;
    const int64_t secs = t / kUsPerSecond;
    const int64_t frame = fps.frameAt(t) - fps.frameAt(secs * kUsPerSecond);
    const int64_t h = secs / 3600, m = (secs / 60) % 60, s = secs % 60;
    return QStringLiteral("%1%2:%3:%4;%5")
        .arg(negative ? QStringLiteral("-") : QString())
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(frame, 2, 10, QLatin1Char('0'));
}

QString formatDuration(TimeUs t)
{
    const double secs = double(t) / kUsPerSecond;
    if (secs < 60)
        return QStringLiteral("%1 s").arg(secs, 0, 'f', 2);
    const int64_t whole = t / kUsPerSecond;
    const int64_t h = whole / 3600, m = (whole / 60) % 60;
    const double s = secs - double(whole - whole % 60);
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(int(s), 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 5, 'f', 2, QLatin1Char('0'));
}
