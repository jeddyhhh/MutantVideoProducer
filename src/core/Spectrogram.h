#pragma once

// Spectrograms: a picture of sound, time across and frequency up, its loudness as colour, where hum shows as flat
// lines, clicks as thin upright ones, hiss as a haze across the top.

#include "core/Project.h"

#include <QImage>
#include <vector>

namespace spectrogram {

constexpr double kLowHz = 30;                 // the bottom row (the top is half the rate)
constexpr double kFloorDb = -100, kTopDb = 0; // the colours' range

// `mono` at `rate` drawn `size` big (frequency on a log scale).
QImage render(const std::vector<float>& mono, int rate, QSize size);
// The row (0 at the top) of a frequency, in a picture `height` high, and back.
double rowOf(double hz, int rate, int height);
double hzAt(double row, int rate, int height);

// An audio event's sound alone, mono: with its own FX and level, or before its FX (`beforeEffects`), up to
// `maxSeconds` from its start.
std::vector<float> eventSound(const ProjectSnapshot& snap, int clipId, bool beforeEffects, int rate, double maxSeconds = 600);

// The other way: a picture made into sound `seconds` long, which a spectrogram draws back as the picture (its bottom at
// `lowHz`, its top at `highHz`, on the same log scale; bright is loud, dark or see-through is quiet). Mono, peaking at
// -1 dBFS.
std::vector<float> fromPicture(const QImage& picture, int rate, double seconds, double lowHz = 150, double highHz = 16000);

} // namespace spectrogram
