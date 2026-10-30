#pragma once

// Match Colour: gives video events the colours of a reference shot. Each event's red, green and blue are mapped so
// their spread of levels (in frames across the event) matches the reference picture's (histogram matching), as the
// curves of a Color Match effect at the end of the event's FX.

#include "core/Project.h"
#include <QImage>
#include <array>
#include <set>

namespace colormatch {

constexpr int kPoints = 9; // curve points a channel, at 0, 1/8 .. 1 (the effect's red0..red8, green0.., blue0..)
using Curves = std::array<std::array<double, kPoints>, 3>;

// How many of the opaque pixels of pictures are at each level (256 a channel).
struct Histogram {
    std::array<std::array<double, 256>, 3> bins{};
    double pixels = 0;
    void add(const QImage& img);
};

// The curves that take `source`'s levels to `reference`'s.
Curves curves(const Histogram& source, const Histogram& reference);

// Matches the video events `ids` of tl (snap: the project as it is) to the picture at timeline time `reference`:
// each gets a Color Match effect at the end of its FX (in place of one it had). Returns how many were matched; 0
// with `error` saying why.
int match(Timeline& tl, const ProjectSnapshot& snap, const std::set<int>& ids, TimeUs reference, QString* error);

} // namespace colormatch
