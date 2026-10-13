#pragma once

// Fader and meter scales shared by the track headers and the Mixer window.

#include <QString>
#include <algorithm>
#include <cmath>

namespace levels {

// Volume fader: position 0..1 maps to -inf / -48 dB .. +6 dB.
inline float volumeFromPos(double pos)
{
    if (pos <= 0.005)
        return 0.0f;
    const double db = -48.0 + pos * 54.0;
    return float(std::pow(10.0, db / 20.0));
}
inline double posFromVolume(float gain)
{
    if (gain <= 0.0f)
        return 0.0;
    const double db = 20.0 * std::log10(gain);
    return std::clamp((db + 48.0) / 54.0, 0.0, 1.0);
}
inline QString volumeText(float gain)
{
    if (gain <= 0.0f)
        return QStringLiteral("-inf dB");
    return QStringLiteral("%1 dB").arg(20.0 * std::log10(gain), 0, 'f', 1);
}
inline QString panText(float pan)
{
    if (std::abs(pan) < 0.01f)
        return QStringLiteral("Center");
    return QStringLiteral("%1 %2%").arg(pan < 0 ? QStringLiteral("L") : QStringLiteral("R")).arg(int(std::lround(std::abs(pan) * 100)));
}
// Meter: a peak (linear) as 0..1 of the meter, -60 dB .. 0 dB.
inline double meterPos(float peak)
{
    if (peak <= 0.001f)
        return 0.0;
    return std::clamp((20.0 * std::log10(peak) + 60.0) / 60.0, 0.0, 1.0);
}

} // namespace levels
