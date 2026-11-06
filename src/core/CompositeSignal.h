#pragma once

// Composite video as a signal, for the Effect Builder's Composite Signal nodes: a picture is turned into the NTSC
// or PAL signal a camera or console sends down a yellow cable (sync pulses, colour burst, the picture on the colour
// subcarrier), that signal goes through emulated analogue circuits (a sync separator and mixer, RC filters, a
// one-transistor video "enhancer" solved as a circuit, a delay line, a clipper...), and a TV model turns it back
// into a picture. The TV locks to whatever sync it is given, as a real one does: damaged sync rolls and tears the
// picture, a disturbed burst shifts its colours.
//
// The signal: four samples a colour subcarrier cycle, a frame at a time, in IRE (blanking 0, white 100, sync tips
// -40). NTSC: 910 samples a line (14.318 MHz) by 525 lines, 480 of picture, black at 7.5. PAL: 1135 samples a line
// (17.734 MHz) by 625 lines, 576 of picture, its V component and burst swinging from line to line. Progressive (the
// two fields drawn as one frame).

#include <QImage>
#include <cstdint>
#include <vector>

namespace nodes {
struct NodeDef;
}

namespace composite {

struct Standard {
    const char* name;
    bool pal;
    double rate;           // samples a second (4 x the subcarrier)
    int line, lines;       // samples a line, lines a frame
    int syncEnd;           // horizontal sync, 4.7 us
    int burstStart, burstEnd;
    int activeStart, activeEnd;
    int firstActive, activeLines;
    int samples() const { return line * lines; }
    int activeWidth() const { return activeEnd - activeStart; }
};
const Standard& ntsc();
const Standard& pal();

struct Signal {
    const Standard* std = &ntsc();
    std::vector<float> v; // std->samples(), in IRE
    int64_t frame = 0;    // which frame (the subcarrier's phase runs on from frame to frame)
};

struct EncodeOptions {
    const Standard* std = &ntsc();
    double saturation = 1.0;
    double chromaMHz = 1.3; // the colour's bandwidth
    bool setup = true;      // NTSC: black at 7.5 IRE (NTSC-M), else at blanking (NTSC-J); PAL has none
};
Signal encode(const QImage& picture, int64_t frame, const EncodeOptions& options = {});

// The raw signal as a grey picture of `size` (sync pulses black, white at 100 IRE): what a signal node shows.
QImage view(const Signal& s, QSize size);

// The Composite Signal nodes (NodeGraph.cpp adds them to the list).
void addNodes(std::vector<nodes::NodeDef>& list);

} // namespace composite
