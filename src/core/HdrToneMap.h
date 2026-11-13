#pragma once

// HDR video (phones' HLG, HDR10's PQ) brought down to the SDR BT.709 picture the rest of the program works in: the
// signal made linear (PQ: absolute nits; HLG: the scene light through its OOTF on a 1000-nit display), BT.2020's
// colours turned into BT.709's, the highlights rolled off above a knee (diffuse white, 203 nits, lands at 0.8), and
// BT.1886's 2.4 gamma put back. The GPU's YUV shader (GpuCompositor.cpp, kFragYuv) does the same sums.

#include <algorithm>
#include <cmath>

namespace hdr {

enum class Transfer { Sdr, Pq, Hlg };

// From an AVFrame's color_trc (AVCOL_TRC_SMPTE2084 = 16, AVCOL_TRC_ARIB_STD_B67 = 18).
inline Transfer transferOf(int trc) { return trc == 16 ? Transfer::Pq : trc == 18 ? Transfer::Hlg : Transfer::Sdr; }

constexpr double kWhiteNits = 203;  // HDR's diffuse white (BT.2408)
constexpr double kPeakNits = 1000;  // ponytail: assumed mastering peak; read the stream's mastering metadata if it matters
constexpr double kKnee = 0.6;       // linear below this (1.0 = SDR white)
constexpr double kHlgGamma = 1.2;   // HLG's system gamma on a 1000-nit display

// The signal (0..1) as linear light, 1.0 = diffuse white. HLG: scene light 0..1 (hlgOotf makes it display light).
inline double toLinear(double e, Transfer t)
{
    e = std::clamp(e, 0.0, 1.0);
    if (t == Transfer::Pq) {
        constexpr double m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
        const double p = std::pow(e, 1 / m2);
        return std::pow(std::max(p - c1, 0.0) / (c2 - c3 * p), 1 / m1) * 10000 / kWhiteNits;
    }
    if (t == Transfer::Hlg) {
        constexpr double a = 0.17883277, b = 0.28466892, c = 0.55991073;
        return e <= 0.5 ? e * e / 3 : (std::exp((e - c) / a) + b) / 12;
    }
    return e;
}

// HLG's OOTF: the gain on scene light of BT.2020 luminance `y` (0..1) to display light (1.0 = diffuse white).
inline double hlgOotfGain(double y) { return kPeakNits / kWhiteNits * std::pow(std::max(y, 1e-6), kHlgGamma - 1); }

// BT.2020 to BT.709 (linear light), row by row.
constexpr double k2020To709[9] = {1.6605, -0.5876, -0.0728, -0.1246, 1.1329, -0.0083, -0.0182, -0.1006, 1.1187};

// The highlight roll-off: 1:1 below kKnee, then easing towards 1.0 (slope 1 at the knee).
inline double toneMap(double m)
{
    if (m <= kKnee)
        return m;
    const double r = 1 - kKnee, x = (m - kKnee) / r;
    return kKnee + r * x / (1 + x);
}

inline double encode(double linear) { return std::pow(std::clamp(linear, 0.0, 1.0), 1 / 2.4); }

} // namespace hdr
