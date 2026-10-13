#pragma once

// Live tab modulation: effect settings that move by themselves. A setting's
// Modulation (Effects.h) reads one of these inputs, sampled once per frame:
//   Beat  a pulse on every beat, fading over `beats` (at most one beat)
//   Bar   a pulse on the 1 of each bar, fading over `beats` (at most a bar)
//   Lfo   a wave (sine, triangle, saw, square, random steps) `beats` long
//   Level / Bass / Mids / Highs  how loud the audio input is (overall, or one band)
//   Midi  a MIDI controller (CC) sets the setting across its whole range
// and moves the setting from its own value by `amount` of its range.

#include "core/Effects.h"

#include <array>
#include <vector>
#include <cstdint>

struct ModInputs {
    double beats = 0;  // beats counted by the beat clock (the fraction: how far into the current one)
    int barBeat = 0;   // the current beat in its bar: 0 = the 1 .. 3
    double level = 0, bass = 0, mids = 0, highs = 0; // audio input, 0..1
    std::array<std::array<uint8_t, 128>, 16> midi{}; // last value of each controller, per channel
};

// The modulation's signal now, 0..1 (MIDI: the controller / 127).
double modulationSignal(const Modulation& m, const ModInputs& in);
// A setting's value with its modulation: base + amount x range x signal (MIDI:
// across the whole range), kept within the range.
double modulatedValue(const Modulation& m, const ParamDef& def, double base, const ModInputs& in);
// Whether any setting in the chain (racks included) is modulated.
bool hasModulation(const EffectChain& chain);
// The chain with every modulated number setting replaced by its value now.
EffectChain applyModulation(const EffectChain& chain, const ModInputs& in);

// Loudness of a stereo signal: whole, bass (< 150 Hz), mids, highs (> 4 kHz),
// followed like a VU meter (rises at once, falls over 0.15 s).
struct BandFollower {
    enum Band { Level, Bass, Mids, Highs };
    explicit BandFollower(double sampleRate) : rate(sampleRate) {}
    void process(const float* stereo, int frames); // env[] is the loudness (RMS) after these
    double rate;
    double low = 0, lowHigh = 0; // filter states
    double env[4] = {0, 0, 0, 0};
};

struct ProjectSnapshot;
// How loud the timeline's mix is in one band from `from` to `to`, a value
// every `step`, scaled so the loudest is 1.
std::vector<double> audioBandEnvelope(const ProjectSnapshot& snap, TimeUs from, TimeUs to, TimeUs step, BandFollower::Band band);

QString modulationSourceName(Modulation::Source source);
QString modulationShapeName(Modulation::Shape shape);
// "Beat pulse, 50%" / "LFO sine every 4 beats, -30%" / "MIDI ch 1 CC 21".
QString modulationSummary(const Modulation& m);
