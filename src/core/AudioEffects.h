#pragma once

#include "core/Effects.h"

#include <memory>

// Stateful audio processor working in place on interleaved stereo float.
// One instance per effect instance per mixer; reset() clears delay lines
// and envelopes (called on seeks, so tails don't leak across jumps).
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    virtual void reset(int sampleRate) = 0;
    virtual void process(float* stereo, int frames, const Params& p) = 0;
    // Output delay in samples with these settings (after reset). The mixer
    // compensates event FX by feeding their audio this far ahead.
    virtual int latency(const Params&) { return 0; }
};

struct AudioEffectDef {
    QString id;                // "a_..." (never clashes with video effect ids)
    QString name;
    QString category;
    QString description;
    std::vector<ParamDef> params;
    std::function<std::unique_ptr<AudioProcessor>()> create;
};

const std::vector<AudioEffectDef>& audioEffects();
const AudioEffectDef* findAudioEffect(const QString& id);
// For pictures of the settings (the device chain): what the Parametric EQ or the Filter does to a steady tone at `freq` Hz,
// in dB, as they filter it; NaN for other effects.
double audioEffectResponseDb(const QString& type, const Params& p, double freq, int rate = 48000);
// The Compressor's, Noise Gate's or Limiter's level out for a steady level in (dB), attack and release settled; NaN for others.
double audioEffectTransferDb(const QString& type, const Params& p, double inputDb);
// True if an FFmpeg audio filter chain (as used by the FFmpeg Audio Filter effect) builds.
bool audioFilterSpecValid(const QString& spec, int rate = 48000);
