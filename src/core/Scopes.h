#pragma once

// Video scopes: measure a picture while grading.
//   Waveform     - brightness (luma, Rec. 709) of every column, 0-100%
//   RGB Parade   - the same for red, green and blue side by side
//   Vectorscope  - colour (Cb / Cr): hue as the angle, saturation as the
//                  distance from the centre, with the 75% colour bar targets
//                  and the skin tone line
//   Histogram    - how many pixels at each level, per channel and luma

#include <QImage>
#include <QPointF>
#include <QSize>
#include <QStringList>
#include <array>
#include <cstdint>

namespace scopes {

enum class Kind { Waveform, Parade, Vectorscope, Histogram };
QStringList names(); // in Kind order

// Draws a scope of `picture` (any format) at `size`: dark background,
// graticule, scale labels. Reads at most about 512 x 288 of its pixels.
QImage render(const QImage& picture, Kind kind, QSize size);

// The numbers behind them (tests).
struct Levels {
    std::array<std::array<uint32_t, 256>, 4> bins{}; // red, green, blue, luma
    uint32_t samples = 0;
};
Levels histogram(const QImage& picture);
// Where a colour lands on the vectorscope: Cb (x) and Cr (y, up), each -0.5..0.5 (Rec. 709).
QPointF vectorPosition(QRgb c);
int luma(QRgb c); // 0..255, Rec. 709

} // namespace scopes
