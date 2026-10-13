#include "core/LiveModulation.h"

#include "core/AudioMixer.h"
#include "core/Project.h"

#include <QCoreApplication>
#include <algorithm>
#include <cmath>
#include <numbers>

namespace {

double frac(double x)
{
    return x - std::floor(x);
}

// A repeatable 0..1 value for step n (random steps).
double hashStep(int64_t n)
{
    uint64_t x = uint64_t(n) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return double(x >> 11) / double(1ull << 53);
}

QString tr(const char* s)
{
    return QCoreApplication::translate("LiveModulation", s);
}

void modulate(EffectChain& chain, const ModInputs& in)
{
    for (EffectInstance& e : chain) {
        if (e.isRack()) {
            for (RackChain& c : e.chains)
                modulate(c.effects, in);
            continue;
        }
        const auto meta = effectMeta(e.type);
        if (!meta || !meta->params)
            continue;
        for (auto& [id, pv] : e.params) {
            if (!pv.mod)
                continue;
            for (const ParamDef& def : *meta->params)
                if (def.id == id && def.type == ParamType::Number) {
                    pv.value = modulatedValue(*pv.mod, def, pv.at(0, def.type), in);
                    pv.keys.clear();
                    break;
                }
            pv.mod.reset();
        }
    }
}

} // namespace

double modulationSignal(const Modulation& m, const ModInputs& in)
{
    using S = Modulation::Source;
    const double beats = std::max(0.0, in.beats);
    switch (m.source) {
    case S::Beat: {
        const double length = std::clamp(m.beats, 0.05, 1.0);
        return std::exp(-5.0 * frac(beats) / length);
    }
    case S::Bar: {
        const double into = std::clamp(in.barBeat, 0, 3) + frac(beats); // beats since the 1
        const double length = std::clamp(m.beats, 0.05, 4.0);
        return std::exp(-5.0 * into / length);
    }
    case S::Lfo: {
        const double cycle = std::clamp(m.beats, 0.0625, 64.0);
        const double x = frac(beats / cycle);
        switch (m.shape) {
        case Modulation::Shape::Sine: return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * x);
        case Modulation::Shape::Triangle: return x < 0.5 ? 2 * x : 2 - 2 * x;
        case Modulation::Shape::SawUp: return x;
        case Modulation::Shape::SawDown: return 1 - x;
        case Modulation::Shape::Square: return x < 0.5 ? 1.0 : 0.0;
        case Modulation::Shape::Random: return hashStep(int64_t(std::floor(beats / cycle)));
        }
        return 0;
    }
    case S::Level: return std::clamp(in.level, 0.0, 1.0);
    case S::Bass: return std::clamp(in.bass, 0.0, 1.0);
    case S::Highs: return std::clamp(in.highs, 0.0, 1.0);
    case S::Mids: return std::clamp(in.mids, 0.0, 1.0);
    case S::Midi:
        if (m.cc < 0 || m.cc > 127)
            return 0;
        return in.midi[size_t(std::clamp(m.channel, 0, 15))][size_t(m.cc)] / 127.0;
    }
    return 0;
}

double modulatedValue(const Modulation& m, const ParamDef& def, double base, const ModInputs& in)
{
    const double signal = modulationSignal(m, in);
    const double range = def.max - def.min;
    const double v = m.source == Modulation::Source::Midi ? def.min + range * signal : base + m.amount * range * signal;
    return std::clamp(v, def.min, def.max);
}

bool hasModulation(const EffectChain& chain)
{
    bool any = false;
    forEachEffect(chain, [&](const EffectInstance& e) {
        for (const auto& [id, pv] : e.params)
            any |= pv.mod.has_value();
    });
    return any;
}

EffectChain applyModulation(const EffectChain& chain, const ModInputs& in)
{
    EffectChain out = chain;
    modulate(out, in);
    return out;
}

void BandFollower::process(const float* stereo, int frames)
{
    if (frames <= 0)
        return;
    const double aLow = 1 - std::exp(-2 * std::numbers::pi * 150 / rate), aHigh = 1 - std::exp(-2 * std::numbers::pi * 4000 / rate);
    double sum[4] = {0, 0, 0, 0};
    for (int i = 0; i < frames; ++i) {
        const double x = 0.5 * (double(stereo[i * 2]) + double(stereo[i * 2 + 1]));
        low += aLow * (x - low);
        lowHigh += aHigh * (x - lowHigh);
        const double mid = lowHigh - low, high = x - lowHigh;
        sum[Level] += x * x;
        sum[Bass] += low * low;
        sum[Mids] += mid * mid;
        sum[Highs] += high * high;
    }
    const double release = std::exp(-frames / rate / 0.15);
    for (int k = 0; k < 4; ++k)
        env[k] = std::max(std::sqrt(sum[k] / frames), env[k] * release);
}

std::vector<double> audioBandEnvelope(const ProjectSnapshot& snap, TimeUs from, TimeUs to, TimeUs step, BandFollower::Band band)
{
    const int rate = snap.settings.sampleRate;
    AudioMixer mixer(rate);
    BandFollower follower(rate);
    std::vector<double> out;
    std::vector<float> buf;
    for (TimeUs t = from; t < to && step > 0; t += step) {
        const int64_t a = usToSamples(t, rate);
        const int n = int(usToSamples(std::min(t + step, to), rate) - a);
        if (n <= 0)
            continue;
        buf.resize(size_t(n) * 2);
        mixer.mix(snap, a, n, buf.data());
        follower.process(buf.data(), n);
        out.push_back(follower.env[band]);
    }
    const double peak = out.empty() ? 0.0 : *std::max_element(out.begin(), out.end());
    if (peak > 1e-6)
        for (double& v : out)
            v /= peak;
    return out;
}

QString modulationSourceName(Modulation::Source source)
{
    switch (source) {
    case Modulation::Source::Beat: return tr("Beat pulse");
    case Modulation::Source::Bar: return tr("Bar pulse (the 1)");
    case Modulation::Source::Lfo: return tr("LFO (synced to the tempo)");
    case Modulation::Source::Level: return tr("Audio level");
    case Modulation::Source::Bass: return tr("Bass level");
    case Modulation::Source::Highs: return tr("Highs level");
    case Modulation::Source::Mids: return tr("Mids level");
    case Modulation::Source::Midi: return tr("MIDI controller (CC)");
    }
    return {};
}

QString modulationShapeName(Modulation::Shape shape)
{
    switch (shape) {
    case Modulation::Shape::Sine: return tr("Sine");
    case Modulation::Shape::Triangle: return tr("Triangle");
    case Modulation::Shape::SawUp: return tr("Saw up");
    case Modulation::Shape::SawDown: return tr("Saw down");
    case Modulation::Shape::Square: return tr("Square");
    case Modulation::Shape::Random: return tr("Random steps");
    }
    return {};
}

QString modulationSummary(const Modulation& m)
{
    const QString amount = QStringLiteral("%1%2%").arg(m.amount > 0 ? QStringLiteral("+") : QString()).arg(int(std::lround(m.amount * 100)));
    auto beats = [](double b) {
        return b == 1 ? tr("a beat") : b < 1 ? tr("1/%1 beat").arg(int(std::lround(1 / b))) : tr("%1 beats").arg(b);
    };
    switch (m.source) {
    case Modulation::Source::Midi: return tr("MIDI channel %1, CC %2").arg(m.channel + 1).arg(m.cc);
    case Modulation::Source::Lfo: return tr("%1 LFO, %2, %3").arg(modulationShapeName(m.shape), beats(m.beats), amount);
    case Modulation::Source::Beat:
    case Modulation::Source::Bar: return tr("%1 over %2, %3").arg(modulationSourceName(m.source), beats(m.beats), amount);
    default: return tr("%1, %2").arg(modulationSourceName(m.source), amount);
    }
}
