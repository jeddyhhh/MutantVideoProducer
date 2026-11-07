#pragma once

// The Live tab's pad sequencer: a looping piano roll whose rows are the 16 pads. A block on a row switches that pad
// on for its length, in time with the beat clock (the tempo typed, tapped, heard or from MIDI). Times are ticks,
// 384 a whole note (96 a quarter), so changing the snap or the time signature never moves the blocks.

#include <QJsonObject>
#include <vector>

struct LiveSequenceBlock {
    int pad = 0;    // 0..15
    int start = 0;  // ticks from the loop's start
    int length = 0; // ticks
    int end() const { return start + length; }
    bool operator==(const LiveSequenceBlock&) const = default;
};

struct LiveSequence {
    static constexpr int kTicksPerWhole = 384;
    bool on = false;      // follows the beat clock while the Live tab runs
    int bars = 4;         // 1..64
    int beatsPerBar = 4;  // the time signature's top: 1..16
    int beatUnit = 4;     // its bottom: 2, 4, 8 or 16 (a beat of the clock is one of these)
    int snap = 16;        // the grid: a 1/snap note; 0 = a beat (the time signature's unit); -8, -16: triplets
    std::vector<LiveSequenceBlock> blocks;

    int beatTicks() const { return kTicksPerWhole / beatUnit; }
    int barTicks() const { return beatsPerBar * beatTicks(); }
    int loopTicks() const { return bars * barTicks(); }
    int snapTicks() const;
    // The pad switched on at `tick` (wrapped into the loop): the block covering it that started last (the lower pad
    // on a tie); -1 for none.
    int padAt(int tick) const;
    bool operator==(const LiveSequence&) const = default;
};

QJsonObject liveSequenceToJson(const LiveSequence& s);
LiveSequence liveSequenceFromJson(const QJsonObject& o);
