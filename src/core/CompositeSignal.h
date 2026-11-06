#pragma once

// Composite video as a signal, for the Effect Builder's Composite Signal nodes: a picture is turned into the NTSC
// signal a camera or console sends down a yellow cable (sync pulses, colour burst, the picture on the colour
// subcarrier), that signal goes through emulated analogue circuits (a sync separator and mixer, RC filters, a
// one-transistor video "enhancer" solved as a circuit), and a TV model turns it back into a picture. The TV locks to
// whatever sync it is given, as a real one does: damaged sync rolls and tears the picture, a disturbed burst shifts
// its colours.
//
// The signal: 910 samples a line (four a colour subcarrier cycle, 14.318 MHz) by 525 lines, a frame at a time, in
// IRE (blanking 0, black 7.5, white 100, sync tips -40). Progressive (the two fields drawn as one frame), 480 lines
// of picture.

#include <QImage>
#include <cstdint>
#include <vector>

namespace nodes {
struct NodeDef;
}

namespace composite {

constexpr int kLine = 910;                 // samples a line
constexpr int kLines = 525;                // lines a frame
constexpr int kSamples = kLine * kLines;
constexpr double kRate = 315e6 / 22.0;     // 14.318 MHz
constexpr int kSyncEnd = 67;               // horizontal sync, 4.7 us
constexpr int kBurstStart = 76, kBurstEnd = 112; // 9 cycles of colour burst
constexpr int kActiveStart = 150, kActiveEnd = 902;
constexpr int kActiveWidth = kActiveEnd - kActiveStart;
constexpr int kFirstActive = 22, kActiveLines = 480;

struct Signal {
    std::vector<float> v; // kSamples, in IRE
    int64_t frame = 0;    // which frame (the subcarrier's phase runs on from frame to frame)
};

struct EncodeOptions {
    double saturation = 1.0;
    double chromaMHz = 1.3; // the colour's bandwidth
    bool setup = true;      // black at 7.5 IRE (NTSC-M), else at blanking (NTSC-J)
};
Signal encode(const QImage& picture, int64_t frame, const EncodeOptions& options = {});

// The raw signal as a grey picture of `size` (sync pulses black, white at 100 IRE): what a signal node shows.
QImage view(const Signal& s, QSize size);

// The Composite Signal nodes (NodeGraph.cpp adds them to the list).
void addNodes(std::vector<nodes::NodeDef>& list);

} // namespace composite
