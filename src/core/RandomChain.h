#pragma once

// Random effect chains for the Live tab's pads ("Create Random Effects
// Chain"): picked from curated pools of effects with parameter ranges that
// look good, in four intensity tiers. Each candidate is rendered on a
// moving test picture and rejected if it comes out too subtle for its tier,
// too wild for it, or blank / flat; the best of a few tries is kept.

#include "core/Effects.h"

#include <QString>
#include <cstdint>
#include <functional>

enum class RandomTier { Low, Medium, High, Insane };
QString randomTierName(RandomTier tier);

struct RandomChain {
    EffectChain effects;
    QString name;        // e.g. "Neon Prism"
    double change = 0;   // how much it changed the test picture (mean difference 0..255)
    double detail = 0;   // how much detail is left (deviation of the result)
};

// Effect ids come from allocId. The same seed gives the same chain.
RandomChain makeRandomChain(RandomTier tier, uint32_t seed, const std::function<int()>& allocId);

// For tests: the "effect.param" names in the pools that no effect has.
QStringList randomChainUnknownParams();
