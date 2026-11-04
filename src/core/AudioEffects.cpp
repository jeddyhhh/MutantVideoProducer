// Built-in audio effects: native DSP (filters, dynamics, reverb, delay,
// modulation, stereo tools) plus FFmpeg-filter based ones (noise
// reduction, pitch shift, any filter chain).

#include "core/AudioEffects.h"
#include "core/Vst3Host.h"

#include "core/FFmpeg.h"
#include "core/Fft.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <array>
#include <deque>
#include <numbers>
#include <map>
#include <mutex>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

inline double dbToLin(double db) { return std::pow(10.0, db / 20.0); }
inline double linToDb(double v) { return 20.0 * std::log10(std::max(v, 1e-12)); }
inline double timeCoef(double ms, int rate) { return ms <= 0 ? 0.0 : std::exp(-1.0 / (ms * 0.001 * rate)); }

// ---- Parameter helpers -------------------------------------------------------

ParamDef num(const char* id, const char* name, double min, double max, double def, int decimals = 1, const char* suffix = "",
             bool log = false)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.min = min;
    p.max = max;
    p.def = def;
    p.decimals = decimals;
    p.suffix = QString::fromUtf8(suffix);
    p.logScale = log;
    return p;
}

ParamDef choice(const char* id, const char* name, QStringList choices, int def = 0)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Choice;
    p.choices = std::move(choices);
    p.max = p.choices.size() - 1;
    p.def = def;
    return p;
}

ParamDef flag(const char* id, const char* name, bool def)
{
    ParamDef p;
    p.id = QString::fromLatin1(id);
    p.name = QString::fromLatin1(name);
    p.type = ParamType::Bool;
    p.max = 1;
    p.def = def ? 1 : 0;
    return p;
}

// ---- Biquad (RBJ cookbook), stereo, transposed direct form II ------------------------

struct Biquad {
    enum Type { LowPass, HighPass, BandPass, Notch, Peak, LowShelf, HighShelf };
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1[2] = {0, 0}, z2[2] = {0, 0};

    void reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }

    void set(Type type, double rate, double freq, double q, double gainDb)
    {
        freq = std::clamp(freq, 1.0, rate * 0.49);
        q = std::max(q, 0.05);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2 * kPi * freq / rate, c = std::cos(w0), s = std::sin(w0);
        const double alpha = s / (2 * q);
        double a0 = 1;
        switch (type) {
        case LowPass: b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
        case HighPass: b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
        case BandPass: b0 = alpha; b1 = 0; b2 = -alpha; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
        case Notch: b0 = 1; b1 = -2 * c; b2 = 1; a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha; break;
        case Peak: b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A; a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A; break;
        case LowShelf:
        case HighShelf: {
            const double sa = 2 * std::sqrt(A) * (s / std::sqrt(2.0)); // shelf slope 1
            if (type == LowShelf) {
                b0 = A * ((A + 1) - (A - 1) * c + sa);
                b1 = 2 * A * ((A - 1) - (A + 1) * c);
                b2 = A * ((A + 1) - (A - 1) * c - sa);
                a0 = (A + 1) + (A - 1) * c + sa;
                a1 = -2 * ((A - 1) + (A + 1) * c);
                a2 = (A + 1) + (A - 1) * c - sa;
            } else {
                b0 = A * ((A + 1) + (A - 1) * c + sa);
                b1 = -2 * A * ((A - 1) + (A + 1) * c);
                b2 = A * ((A + 1) + (A - 1) * c - sa);
                a0 = (A + 1) - (A - 1) * c + sa;
                a1 = 2 * ((A - 1) - (A + 1) * c);
                a2 = (A + 1) - (A - 1) * c - sa;
            }
            break;
        }
        }
        b0 /= a0;
        b1 /= a0;
        b2 /= a0;
        a1 /= a0;
        a2 /= a0;
    }

    // What it does to a steady tone at `freq` Hz, in dB.
    double responseDb(double freq, double rate) const
    {
        const std::complex<double> z1 = std::polar(1.0, -2 * kPi * freq / rate), z2 = z1 * z1;
        return 20 * std::log10(std::max(1e-12, std::abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2))));
    }

    inline double run(int ch, double x)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    void process(float* s, int frames)
    {
        for (int i = 0; i < frames; ++i) {
            s[i * 2] = float(run(0, s[i * 2]));
            s[i * 2 + 1] = float(run(1, s[i * 2 + 1]));
        }
    }
};

// ---- Gain ------------------------------------------------------------------------

class GainProcessor : public AudioProcessor {
public:
    void reset(int) override { m_started = false; }
    void process(float* s, int frames, const Params& p) override
    {
        double target = dbToLin(p.num(QStringLiteral("gain")));
        if (p.flag(QStringLiteral("invert")))
            target = -target;
        // Ramp from the previous block's gain so automation and edits don't click.
        const double start = m_started ? m_gain : target;
        for (int i = 0; i < frames; ++i) {
            const double g = start + (target - start) * (i + 1) / frames;
            s[i * 2] = float(s[i * 2] * g);
            s[i * 2 + 1] = float(s[i * 2 + 1] * g);
        }
        m_gain = target;
        m_started = true;
    }

private:
    double m_gain = 1;
    bool m_started = false;
};

// ---- Parametric EQ ---------------------------------------------------------------

// The Parametric EQ's six bands with these settings (bands at 0 dB, and cuts at the ends, are off).
struct EqBand {
    Biquad::Type type;
    double freq, q, gain;
    bool active;
};
std::array<EqBand, 6> eqBands(const Params& p)
{
    return {{
            {Biquad::HighPass, p.num(QStringLiteral("lowCut")), 0.707, 0, p.num(QStringLiteral("lowCut")) > 20.5},
            {Biquad::LowShelf, p.num(QStringLiteral("lowFreq")), 0.707, p.num(QStringLiteral("lowGain")), p.num(QStringLiteral("lowGain")) != 0},
            {Biquad::Peak, p.num(QStringLiteral("mid1Freq")), p.num(QStringLiteral("mid1Q")), p.num(QStringLiteral("mid1Gain")),
             p.num(QStringLiteral("mid1Gain")) != 0},
            {Biquad::Peak, p.num(QStringLiteral("mid2Freq")), p.num(QStringLiteral("mid2Q")), p.num(QStringLiteral("mid2Gain")),
             p.num(QStringLiteral("mid2Gain")) != 0},
            {Biquad::HighShelf, p.num(QStringLiteral("highFreq")), 0.707, p.num(QStringLiteral("highGain")),
             p.num(QStringLiteral("highGain")) != 0},
            {Biquad::LowPass, p.num(QStringLiteral("highCut")), 0.707, 0, p.num(QStringLiteral("highCut")) < 19999.5},
    }};
}

class EqProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        for (auto& b : m_bands)
            b.reset();
    }
    void process(float* s, int frames, const Params& p) override
    {
        const std::array<EqBand, 6> bands = eqBands(p);
        for (int i = 0; i < 6; ++i) {
            if (!bands[i].active) {
                m_bands[i].reset();
                continue;
            }
            m_bands[i].set(bands[i].type, m_rate, bands[i].freq, bands[i].q, bands[i].gain);
            m_bands[i].process(s, frames);
        }
    }

private:
    int m_rate = 48000;
    Biquad m_bands[6];
};

// ---- Filter (LP/HP/BP/notch) ---------------------------------------------------------

class FilterProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_a.reset();
        m_b.reset();
    }
    void process(float* s, int frames, const Params& p) override
    {
        static const Biquad::Type types[] = {Biquad::LowPass, Biquad::HighPass, Biquad::BandPass, Biquad::Notch};
        const Biquad::Type type = types[std::clamp(p.choice(QStringLiteral("type")), 0, 3)];
        const double f = p.num(QStringLiteral("frequency")), q = p.num(QStringLiteral("q"));
        m_a.set(type, m_rate, f, q, 0);
        m_a.process(s, frames);
        if (p.choice(QStringLiteral("slope")) == 1) {
            m_b.set(type, m_rate, f, q, 0);
            m_b.process(s, frames);
        }
    }

private:
    int m_rate = 48000;
    Biquad m_a, m_b;
};

// ---- De-hum: notches on the mains frequency and its harmonics ---------------------------------

// The notches with these settings: the mains frequency (50 or 60 Hz) and the harmonics above it, each as narrow as `q`.
std::vector<std::pair<double, double>> humNotches(const Params& p)
{
    const double base = p.choice(QStringLiteral("mains")) == 1 ? 60.0 : 50.0;
    const int harmonics = std::clamp(int(p.num(QStringLiteral("harmonics"))), 1, 8);
    std::vector<std::pair<double, double>> out;
    for (int k = 1; k <= harmonics; ++k)
        out.emplace_back(base * k, p.num(QStringLiteral("q")) * k); // (as wide in Hz at every harmonic)
    return out;
}

class DehumProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        for (auto& b : m_notches)
            b.reset();
    }
    void process(float* s, int frames, const Params& p) override
    {
        const auto notches = humNotches(p);
        for (size_t i = 0; i < std::size(m_notches); ++i) {
            if (i >= notches.size()) {
                m_notches[i].reset();
                continue;
            }
            m_notches[i].set(Biquad::Notch, m_rate, notches[i].first, notches[i].second, 0);
            m_notches[i].process(s, frames);
        }
    }

private:
    int m_rate = 48000;
    Biquad m_notches[8];
};

// ---- De-reverb --------------------------------------------------------------------------

// Takes out a room's late reverberation: the sound is cut into overlapping frames (an STFT), and in each frequency the
// part that is only the tail of what came before (its level from 50 ms earlier, decayed at the room's reverb time:
// Lebart's late-reverb estimate) is turned down, by one gain per frequency for both channels (the stereo picture
// stays). The direct sound, louder than its own tail, passes.
class DereverbProcessor : public AudioProcessor {
public:
    static constexpr int kSize = 2048, kHop = kSize / 4;

    void reset(int rate) override
    {
        m_rate = rate;
        m_window.resize(kSize);
        for (int i = 0; i < kSize; ++i)
            m_window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / kSize));
        for (auto& b : m_in)
            b.assign(kSize, 0.0f);
        for (auto& b : m_out)
            b.assign(kSize, 0.0f);
        m_fill = 0;
        m_read = 0;
        m_delay = std::max(1, int(std::lround(0.05 * rate / kHop)));
        m_history.assign(size_t(m_delay), std::vector<float>(kSize / 2 + 1, 0.0f));
        m_smoothed.assign(kSize / 2 + 1, 0.0f);
        m_gain.assign(kSize / 2 + 1, 1.0f);
        m_frames = 0;
    }
    int latency(const Params&) override { return kSize - kHop; }
    void process(float* s, int frames, const Params& p) override
    {
        const double amount = std::clamp(p.num(QStringLiteral("amount")), 0.0, 1.0);
        const double t60 = std::clamp(p.num(QStringLiteral("reverb")), 0.1, 6.0);
        const float floorGain = float(std::pow(10.0, std::clamp(p.num(QStringLiteral("floor")), -60.0, 0.0) / 20.0));
        // The tail's share after the delay: e^(-2 * delta * Td), delta = 3 ln 10 / T60.
        const float decay = float(std::exp(-2.0 * 3.0 * std::log(10.0) / t60 * (double(m_delay) * kHop / m_rate)));
        for (int i = 0; i < frames; ++i) {
            for (int c = 0; c < 2; ++c) {
                m_in[c][size_t(kSize - kHop + m_fill)] = s[2 * i + c];
                s[2 * i + c] = m_out[c][size_t(m_read)];
            }
            ++m_read;
            if (++m_fill == kHop) {
                frame(amount, decay, floorGain);
                m_fill = 0;
                m_read = 0;
            }
        }
    }

private:
    void frame(double amount, float decay, float floorGain)
    {
        const size_t bins = kSize / 2 + 1;
        std::array<std::vector<std::complex<float>>, 2> spec;
        for (int c = 0; c < 2; ++c) {
            spec[size_t(c)].resize(kSize);
            for (int i = 0; i < kSize; ++i)
                spec[size_t(c)][size_t(i)] = {m_in[c][size_t(i)] * m_window[size_t(i)], 0.0f};
            fftInPlace(spec[size_t(c)]);
        }
        std::vector<float>& before = m_history[size_t(m_frames % m_delay)]; // (the level m_delay frames ago)
        for (size_t k = 0; k < bins; ++k) {
            const float power = 0.5f * (std::norm(spec[0][k]) + std::norm(spec[1][k]));
            m_smoothed[k] = 0.6f * m_smoothed[k] + 0.4f * power;
            const float tail = decay * before[k];
            float g = 1.0f - float(amount) * tail / (std::max(power, m_smoothed[k]) * 0.5f + m_smoothed[k] * 0.5f + 1e-12f);
            g = std::clamp(g, floorGain, 1.0f);
            m_gain[k] = 0.5f * m_gain[k] + 0.5f * g; // (smoothed: no warble)
            before[k] = m_smoothed[k];
        }
        ++m_frames;
        for (int c = 0; c < 2; ++c) {
            auto& x = spec[size_t(c)];
            for (size_t k = 0; k < bins; ++k) {
                x[k] *= m_gain[k];
                if (k > 0 && k < kSize / 2)
                    x[kSize - k] = std::conj(x[k]);
            }
            for (auto& v : x)
                v = std::conj(v);
            fftInPlace(x); // (the inverse: conjugate, forward, conjugate, scale)
            // Overlap-add: Hann in and out at 75% overlap adds up to 1.5.
            std::vector<float>& out = m_out[c];
            std::move(out.begin() + kHop, out.end(), out.begin());
            std::fill(out.end() - kHop, out.end(), 0.0f);
            for (int i = 0; i < kSize; ++i)
                out[size_t(i)] += x[size_t(i)].real() / kSize * m_window[size_t(i)] / 1.5f;
            std::vector<float>& in = m_in[c];
            std::move(in.begin() + kHop, in.end(), in.begin());
        }
    }

    int m_rate = 48000, m_fill = 0, m_read = 0, m_delay = 1;
    int64_t m_frames = 0;
    std::vector<float> m_window, m_smoothed, m_gain;
    std::vector<std::vector<float>> m_history;
    std::array<std::vector<float>, 2> m_in, m_out;
};

// ---- Compressor -------------------------------------------------------------------------

// The gain change (dB, <= 0) for a level `over` dB above the threshold: soft knee `knee` dB wide.
double compressorGainDb(double over, double ratio, double knee)
{
    const double slope = 1.0 / std::max(1.0, ratio) - 1.0;
    if (2 * over < -knee)
        return 0;
    if (knee > 0 && 2 * std::abs(over) <= knee)
        return slope * (over + knee / 2) * (over + knee / 2) / (2 * knee);
    return slope * over;
}

class CompressorProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_env = 0;
    }
    void process(float* s, int frames, const Params& p) override
    {
        const double th = p.num(QStringLiteral("threshold")), ratio = std::max(1.0, p.num(QStringLiteral("ratio")));
        const double knee = p.num(QStringLiteral("knee")), makeup = p.num(QStringLiteral("makeup"));
        const double att = timeCoef(p.num(QStringLiteral("attack")), m_rate);
        const double rel = timeCoef(p.num(QStringLiteral("release")), m_rate);
        for (int i = 0; i < frames; ++i) {
            const double level = linToDb(std::max(std::abs(s[i * 2]), std::abs(s[i * 2 + 1])));
            const double gr = compressorGainDb(level - th, ratio, knee); // target gain change in dB (<= 0)
            m_env = gr < m_env ? att * m_env + (1 - att) * gr : rel * m_env + (1 - rel) * gr;
            const double g = dbToLin(m_env + makeup);
            s[i * 2] = float(s[i * 2] * g);
            s[i * 2 + 1] = float(s[i * 2 + 1] * g);
        }
    }

private:
    int m_rate = 48000;
    double m_env = 0;
};

// ---- Limiter (lookahead, no overs) ---------------------------------------------------------

class LimiterProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_look = std::max(1, rate / 500); // 2 ms
        m_delay.assign(size_t(m_look) * 2, 0.0f);
        m_req.assign(size_t(m_look), 1.0);
        m_pos = 0;
        m_gain = 1;
        m_window.clear();
        m_index = 0;
        m_history = {};
    }
    void process(float* s, int frames, const Params& p) override
    {
        const double ceiling = dbToLin(p.num(QStringLiteral("ceiling")));
        const double rel = timeCoef(p.num(QStringLiteral("release")), m_rate);
        for (int i = 0; i < frames; ++i) {
            double peak = std::max(std::abs(s[i * 2]), std::abs(s[i * 2 + 1]));
            // True peak: where the waveform swings between samples (oversampled four times), which a meter counts and
            // a converter plays, though no sample holds it.
            // (32 taps a point, Blackman-Harris windowed, as the loudness meter's resampler: fewer underestimated near
            // the top of the spectrum, where clicks and cymbals swing furthest between samples)
            static const auto taps = [] {
                constexpr int N = kTruePeakTaps;
                std::array<std::array<double, N>, 3> c{};
                for (int k = 0; k < 3; ++k) {
                    double sum = 0;
                    for (int j = 0; j < N; ++j) {
                        const double x = (N / 2 - 1) + (k + 1) / 4.0 - j; // (the point sits between taps N/2-1 and N/2)
                        const double sinc = std::abs(x) < 1e-9 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                        const double u = (x + N / 2.0) / N; // 0 to 1 across the filter
                        const double window = 0.35875 - 0.48829 * std::cos(2 * std::numbers::pi * u) + 0.14128 * std::cos(4 * std::numbers::pi * u)
                                              - 0.01168 * std::cos(6 * std::numbers::pi * u);
                        c[size_t(k)][size_t(j)] = sinc * window;
                        sum += c[size_t(k)][size_t(j)];
                    }
                    for (auto& v : c[size_t(k)])
                        v /= sum;
                }
                return c;
            }();
            for (int ch = 0; ch < 2; ++ch) {
                auto& h = m_history[size_t(ch)];
                std::copy(h.begin() + 1, h.end(), h.begin());
                h[kTruePeakTaps - 1] = s[i * 2 + ch];
                for (const auto& c : taps) {
                    double v = 0;
                    for (int j = 0; j < kTruePeakTaps; ++j)
                        v += c[size_t(j)] * h[size_t(j)];
                    peak = std::max(peak, std::abs(v));
                }
            }
            peak *= 1.0175; // (a 0.15 dB margin for what the estimate still misses)
            const double req = peak > ceiling ? ceiling / peak : 1.0;
            // Sliding-window minimum of the required gain over the lookahead
            // (the sample leaving the delay line included).
            while (!m_window.empty() && m_window.back().second >= req)
                m_window.pop_back();
            m_window.emplace_back(m_index, req);
            while (m_window.front().first < m_index - m_look)
                m_window.pop_front();
            ++m_index;
            const double target = m_window.front().second;
            m_gain = target < m_gain ? target : target + (m_gain - target) * rel;
            // Output the delayed sample with the gain that already sees it coming.
            const float inL = s[i * 2], inR = s[i * 2 + 1];
            float& dl = m_delay[size_t(m_pos) * 2];
            float& dr = m_delay[size_t(m_pos) * 2 + 1];
            const float c = float(ceiling);
            s[i * 2] = std::clamp(float(dl * m_gain), -c, c);
            s[i * 2 + 1] = std::clamp(float(dr * m_gain), -c, c);
            dl = inL;
            dr = inR;
            m_pos = (m_pos + 1) % m_look;
        }
    }

    int latency(const Params&) override { return m_look; }

private:
    int m_rate = 48000, m_look = 96, m_pos = 0;
    std::vector<float> m_delay;
    std::vector<double> m_req;
    std::deque<std::pair<int64_t, double>> m_window;
    int64_t m_index = 0;
    double m_gain = 1;
    static constexpr int kTruePeakTaps = 32;
    std::array<std::array<double, kTruePeakTaps>, 2> m_history{}; // each channel's last input samples
};

// ---- Noise gate ------------------------------------------------------------------------------

class GateProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_env = 0;
        m_gain = 0;
        m_hold = 0;
    }
    void process(float* s, int frames, const Params& p) override
    {
        const double th = dbToLin(p.num(QStringLiteral("threshold")));
        const double floorGain = dbToLin(p.num(QStringLiteral("range")));
        const double att = timeCoef(p.num(QStringLiteral("attack")), m_rate);
        const double rel = timeCoef(p.num(QStringLiteral("release")), m_rate);
        const int hold = int(p.num(QStringLiteral("hold")) * 0.001 * m_rate);
        const double detDecay = timeCoef(10.0, m_rate);
        for (int i = 0; i < frames; ++i) {
            const double peak = std::max(std::abs(s[i * 2]), std::abs(s[i * 2 + 1]));
            m_env = std::max(peak, m_env * detDecay);
            double target;
            if (m_env > th) {
                m_hold = hold;
                target = 1.0;
            } else if (m_hold > 0) {
                --m_hold;
                target = 1.0;
            } else {
                target = floorGain;
            }
            m_gain = target > m_gain ? att * m_gain + (1 - att) * target : rel * m_gain + (1 - rel) * target;
            s[i * 2] = float(s[i * 2] * m_gain);
            s[i * 2 + 1] = float(s[i * 2 + 1] * m_gain);
        }
    }

private:
    int m_rate = 48000;
    double m_env = 0, m_gain = 0;
    int m_hold = 0;
};

// ---- Reverb (Freeverb) --------------------------------------------------------------------------

class ReverbProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        static const int combTuning[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
        static const int allpassTuning[4] = {556, 441, 341, 225};
        const double k = rate / 44100.0;
        for (int ch = 0; ch < 2; ++ch) {
            const int spread = ch ? 23 : 0;
            for (int i = 0; i < 8; ++i) {
                m_comb[ch][i].buf.assign(size_t(std::max(1.0, (combTuning[i] + spread) * k)), 0.0f);
                m_comb[ch][i].pos = 0;
                m_comb[ch][i].store = 0;
            }
            for (int i = 0; i < 4; ++i) {
                m_allpass[ch][i].buf.assign(size_t(std::max(1.0, (allpassTuning[i] + spread) * k)), 0.0f);
                m_allpass[ch][i].pos = 0;
            }
        }
        m_pre.assign(size_t(rate / 5 + 1) * 2, 0.0f); // up to 200 ms pre-delay
        m_prePos = 0;
    }
    void process(float* s, int frames, const Params& p) override
    {
        const float feedback = float(0.7 + 0.28 * p.num(QStringLiteral("room")));
        const float damp = float(0.4 * p.num(QStringLiteral("damping")));
        const float wet = float(3.0 * p.num(QStringLiteral("wet")));
        const float width = float(p.num(QStringLiteral("width")));
        const float dry = float(p.num(QStringLiteral("dry")));
        const float wet1 = wet * (width / 2 + 0.5f), wet2 = wet * ((1 - width) / 2);
        const int preFrames = std::clamp(int(p.num(QStringLiteral("predelay")) * 0.001 * m_rate), 0, int(m_pre.size() / 2) - 1);
        const int preLen = int(m_pre.size() / 2);
        for (int i = 0; i < frames; ++i) {
            const float inL = s[i * 2], inR = s[i * 2 + 1];
            // Pre-delay line.
            m_pre[size_t(m_prePos) * 2] = inL;
            m_pre[size_t(m_prePos) * 2 + 1] = inR;
            const int rd = (m_prePos - preFrames + preLen) % preLen;
            const float input = (m_pre[size_t(rd) * 2] + m_pre[size_t(rd) * 2 + 1]) * 0.015f;
            m_prePos = (m_prePos + 1) % preLen;
            float out[2] = {0, 0};
            for (int ch = 0; ch < 2; ++ch) {
                for (auto& c : m_comb[ch]) {
                    float& b = c.buf[size_t(c.pos)];
                    c.store = b * (1 - damp) + c.store * damp;
                    const float y = b;
                    b = input + c.store * feedback;
                    c.pos = (c.pos + 1) % int(c.buf.size());
                    out[ch] += y;
                }
                for (auto& a : m_allpass[ch]) {
                    float& b = a.buf[size_t(a.pos)];
                    const float y = -out[ch] + b;
                    b = out[ch] + b * 0.5f;
                    a.pos = (a.pos + 1) % int(a.buf.size());
                    out[ch] = y;
                }
            }
            s[i * 2] = out[0] * wet1 + out[1] * wet2 + inL * dry;
            s[i * 2 + 1] = out[1] * wet1 + out[0] * wet2 + inR * dry;
        }
    }

private:
    struct Comb {
        std::vector<float> buf;
        int pos = 0;
        float store = 0;
    };
    struct Allpass {
        std::vector<float> buf;
        int pos = 0;
    };
    int m_rate = 48000;
    Comb m_comb[2][8];
    Allpass m_allpass[2][4];
    std::vector<float> m_pre;
    int m_prePos = 0;
};

// ---- Delay / echo -----------------------------------------------------------------------------

class DelayProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_buf.assign(size_t(rate * 2 + 2) * 2, 0.0f); // 2 s max
        m_pos = 0;
        m_lp[0] = m_lp[1] = 0;
    }
    void process(float* s, int frames, const Params& p) override
    {
        const int len = int(m_buf.size() / 2);
        const int d = std::clamp(int(p.num(QStringLiteral("time")) * 0.001 * m_rate), 1, len - 1);
        const float fb = float(p.num(QStringLiteral("feedback")));
        const float mix = float(p.num(QStringLiteral("mix")));
        const bool ping = p.flag(QStringLiteral("pingpong"));
        const float a = float(std::exp(-2 * kPi * p.num(QStringLiteral("damping")) / m_rate)); // one-pole low-pass
        for (int i = 0; i < frames; ++i) {
            const int rd = (m_pos - d + len) % len;
            float echo[2] = {m_buf[size_t(rd) * 2], m_buf[size_t(rd) * 2 + 1]};
            for (int ch = 0; ch < 2; ++ch)
                m_lp[ch] = echo[ch] + (m_lp[ch] - echo[ch]) * a;
            const float inL = s[i * 2], inR = s[i * 2 + 1];
            if (ping) {
                // Mono input bounces between the channels.
                m_buf[size_t(m_pos) * 2] = (inL + inR) * 0.5f + m_lp[1] * fb;
                m_buf[size_t(m_pos) * 2 + 1] = m_lp[0] * fb;
            } else {
                m_buf[size_t(m_pos) * 2] = inL + m_lp[0] * fb;
                m_buf[size_t(m_pos) * 2 + 1] = inR + m_lp[1] * fb;
            }
            m_pos = (m_pos + 1) % len;
            s[i * 2] = inL * (1 - mix * 0.5f) + echo[0] * mix;
            s[i * 2 + 1] = inR * (1 - mix * 0.5f) + echo[1] * mix;
        }
    }

private:
    int m_rate = 48000, m_pos = 0;
    std::vector<float> m_buf;
    float m_lp[2] = {0, 0};
};

// ---- Chorus / flanger (modulated delay) ---------------------------------------------------------

class ModDelayProcessor : public AudioProcessor {
public:
    void reset(int rate) override
    {
        m_rate = rate;
        m_buf.assign(size_t(rate / 10 + 4) * 2, 0.0f); // 100 ms
        m_pos = 0;
        m_phase = 0;
    }
    void process(float* s, int frames, const Params& p) override
    {
        const int len = int(m_buf.size() / 2);
        const double rate = p.num(QStringLiteral("rate"));
        const double depth = p.num(QStringLiteral("depth")) * 0.001 * m_rate;
        const double base = p.num(QStringLiteral("delay")) * 0.001 * m_rate;
        const float fb = float(p.num(QStringLiteral("feedback")));
        const float mix = float(p.num(QStringLiteral("mix")));
        const double inc = 2 * kPi * rate / m_rate;
        for (int i = 0; i < frames; ++i) {
            float wetOut[2];
            for (int ch = 0; ch < 2; ++ch) {
                // Right channel LFO is 90 degrees ahead for stereo width.
                const double lfo = std::sin(m_phase + (ch ? kPi / 2 : 0));
                const double d = std::clamp(base + depth * (0.5 + 0.5 * lfo), 1.0, double(len - 2));
                double rp = m_pos - d;
                while (rp < 0)
                    rp += len;
                const int i0 = int(rp), i1 = (i0 + 1) % len;
                const float f = float(rp - i0);
                wetOut[ch] = m_buf[size_t(i0) * 2 + ch] * (1 - f) + m_buf[size_t(i1) * 2 + ch] * f;
            }
            for (int ch = 0; ch < 2; ++ch)
                m_buf[size_t(m_pos) * 2 + ch] = s[i * 2 + ch] + wetOut[ch] * fb;
            m_pos = (m_pos + 1) % len;
            m_phase += inc;
            if (m_phase > 2 * kPi)
                m_phase -= 2 * kPi;
            for (int ch = 0; ch < 2; ++ch)
                s[i * 2 + ch] = s[i * 2 + ch] * (1 - mix) + wetOut[ch] * mix;
        }
    }

private:
    int m_rate = 48000, m_pos = 0;
    std::vector<float> m_buf;
    double m_phase = 0;
};

// ---- Stereo tools -------------------------------------------------------------------------------

class StereoProcessor : public AudioProcessor {
public:
    void reset(int) override {}
    void process(float* s, int frames, const Params& p) override
    {
        const float width = float(p.num(QStringLiteral("width")));
        const float bal = float(p.num(QStringLiteral("balance")));
        const bool swap = p.flag(QStringLiteral("swap")), mono = p.flag(QStringLiteral("mono"));
        const float invL = p.flag(QStringLiteral("invertL")) ? -1.f : 1.f, invR = p.flag(QStringLiteral("invertR")) ? -1.f : 1.f;
        const float gl = bal > 0 ? 1 - bal : 1, gr = bal < 0 ? 1 + bal : 1;
        for (int i = 0; i < frames; ++i) {
            float l = s[i * 2] * invL, r = s[i * 2 + 1] * invR;
            if (swap)
                std::swap(l, r);
            const float mid = (l + r) * 0.5f;
            const float side = (l - r) * 0.5f * (mono ? 0.0f : width);
            s[i * 2] = (mid + side) * gl;
            s[i * 2 + 1] = (mid - side) * gr;
        }
    }
};

// ---- FFmpeg filter based -------------------------------------------------------------------------

// Streams audio through an FFmpeg filter graph built from the parameters.
// The graph keeps its state between blocks; output is aligned by keeping a
// FIFO (the filter's own latency appears as a short delay).
class FfmpegAudioProcessor : public AudioProcessor {
public:
    using SpecFn = std::function<QString(const Params&, int rate)>;
    explicit FfmpegAudioProcessor(SpecFn spec) : m_spec(std::move(spec)) {}
    ~FfmpegAudioProcessor() override { release(); }

    void reset(int rate) override
    {
        m_rate = rate;
        release();
        m_fifo.clear();
        m_current.clear();
    }

    int latency(const Params& p) override
    {
        ensure(m_spec(p, m_rate).trimmed());
        return m_failed || m_current.isEmpty() ? 0 : m_latency;
    }
    bool failed() const { return m_failed; }
    // (m_fifo starts with m_queue samples of silence; see ensure().)

    void process(float* s, int frames, const Params& p) override
    {
        const QString spec = m_spec(p, m_rate).trimmed();
        if (spec.isEmpty())
            return;
        ensure(spec);
        if (m_failed)
            return; // invalid chain: pass audio through untouched
        ff::FramePtr f = ff::makeFrame();
        f->format = AV_SAMPLE_FMT_FLT;
        f->sample_rate = m_rate;
        f->nb_samples = frames;
        const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        av_channel_layout_copy(&f->ch_layout, &stereo);
        if (av_frame_get_buffer(f.get(), 0) < 0)
            return;
        std::memcpy(f->data[0], s, sizeof(float) * 2 * size_t(frames));
        f->pts = m_pts;
        m_pts += frames;
        if (av_buffersrc_add_frame_flags(m_src, f.get(), 0) < 0)
            return;
        ff::FramePtr out = ff::makeFrame();
        while (av_buffersink_get_frame(m_sink, out.get()) >= 0) {
            const float* d = reinterpret_cast<const float*>(out->data[0]);
            m_fifo.insert(m_fifo.end(), d, d + size_t(out->nb_samples) * 2);
            av_frame_unref(out.get());
        }
        // Oldest available samples, right-aligned (silence while the filter primes).
        const size_t want = size_t(frames) * 2;
        const size_t have = std::min(m_fifo.size(), want);
        std::memset(s, 0, sizeof(float) * (want - have));
        std::memcpy(s + (want - have), m_fifo.data(), sizeof(float) * have);
        m_fifo.erase(m_fifo.begin(), m_fifo.begin() + have);
    }

private:
    // (Re)builds the graph for `spec`. Output is a fixed delay line: the
    // filter's worst-case buffering (m_queue) is pre-filled with silence so
    // the FIFO never runs dry, and m_latency = buffering + the filter's own
    // signal delay, both measured once per spec.
    void ensure(const QString& spec)
    {
        if (spec == m_current && (m_graph || m_failed))
            return;
        release();
        m_fifo.clear();
        m_current = spec;
        if (spec.isEmpty())
            return;
        const Probe probe = measure(spec);
        m_failed = probe.queue < 0 || !build(spec);
        m_latency = probe.queue + probe.delay;
        if (!m_failed)
            m_fifo.assign(size_t(probe.queue) * 2, 0.0f);
    }

    struct Probe {
        int queue = -1;   // most samples ever held back by the graph
        int delay = 0;    // where an impulse comes out
    };

    Probe measure(const QString& spec)
    {
        static std::mutex mutex;
        static std::map<std::pair<QString, int>, Probe> cache;
        {
            std::lock_guard lock(mutex);
            auto it = cache.find({spec, m_rate});
            if (it != cache.end())
                return it->second;
        }
        Probe result;
        if (build(spec)) {
            // Feed silence with one impulse after a warm-up (some filters emit a
            // start-up transient), pushing and draining in the same block
            // pattern the mixer uses.
            constexpr int kBlock = 1024;
            constexpr int kImpulseBlock = 4;
            constexpr int kProbeSeconds = 8; // loudnorm looks 3 s ahead
            std::vector<float> in(size_t(kBlock) * 2, 0.0f);
            std::vector<float> outStream;
            int64_t fed = 0, produced = 0, worst = 0;
            ff::FramePtr out = ff::makeFrame();
            for (int b = 0; b < m_rate * kProbeSeconds / kBlock; ++b) {
                std::fill(in.begin(), in.end(), 0.0f);
                if (b == kImpulseBlock)
                    in[0] = in[1] = 0.9f;
                ff::FramePtr f = ff::makeFrame();
                f->format = AV_SAMPLE_FMT_FLT;
                f->sample_rate = m_rate;
                f->nb_samples = kBlock;
                const AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
                av_channel_layout_copy(&f->ch_layout, &stereo);
                if (av_frame_get_buffer(f.get(), 0) < 0)
                    break;
                std::memcpy(f->data[0], in.data(), in.size() * sizeof(float));
                f->pts = fed;
                fed += kBlock;
                if (av_buffersrc_add_frame_flags(m_src, f.get(), 0) < 0)
                    break;
                while (av_buffersink_get_frame(m_sink, out.get()) >= 0) {
                    const float* d = reinterpret_cast<const float*>(out->data[0]);
                    outStream.insert(outStream.end(), d, d + size_t(out->nb_samples) * 2);
                    produced += out->nb_samples;
                    av_frame_unref(out.get());
                }
                worst = std::max(worst, fed - produced);
            }
            int peakAt = 0;
            float peak = 0;
            for (size_t i = size_t(kImpulseBlock) * kBlock; i < outStream.size() / 2; ++i)
                if (std::abs(outStream[i * 2]) > peak) {
                    peak = std::abs(outStream[i * 2]);
                    peakAt = int(i);
                }
            result.queue = int(std::clamp<int64_t>(worst, 0, int64_t(m_rate) * 6));
            result.delay = peak > 1e-4f ? std::clamp(peakAt - kImpulseBlock * kBlock, 0, m_rate * 6) : 0;
            release();
        }
        std::lock_guard lock(mutex);
        cache[{spec, m_rate}] = result;
        return result;
    }

    bool build(const QString& spec)
    {
        m_graph = avfilter_graph_alloc();
        m_pts = 0;
        const QByteArray args = QStringLiteral("sample_rate=%1:sample_fmt=flt:channel_layout=stereo:time_base=1/%1").arg(m_rate).toLatin1();
        if (avfilter_graph_create_filter(&m_src, avfilter_get_by_name("abuffer"), "in", args.constData(), nullptr, m_graph) < 0
            || avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, m_graph) < 0)
            return false;
        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        outputs->name = av_strdup("in");
        outputs->filter_ctx = m_src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = m_sink;
        const QByteArray desc = (spec + QStringLiteral(",aformat=sample_fmts=flt:channel_layouts=stereo:sample_rates=%1").arg(m_rate)).toUtf8();
        int ret = avfilter_graph_parse_ptr(m_graph, desc.constData(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (ret >= 0)
            ret = avfilter_graph_config(m_graph, nullptr);
        return ret >= 0;
    }
    void release()
    {
        avfilter_graph_free(&m_graph);
        m_src = m_sink = nullptr;
    }

    SpecFn m_spec;
    int m_rate = 48000;
    AVFilterGraph* m_graph = nullptr;
    AVFilterContext* m_src = nullptr;
    AVFilterContext* m_sink = nullptr;
    QString m_current;
    bool m_failed = false;
    int m_latency = 0;
    int64_t m_pts = 0;
    std::vector<float> m_fifo;
};

bool hasFilter(const char* name)
{
    return avfilter_get_by_name(name) != nullptr;
}

// ---- Registry ------------------------------------------------------------------------------------

std::vector<AudioEffectDef> buildAudioEffects()
{
    std::vector<AudioEffectDef> list;
    AudioEffectDef d;
    auto make = [](auto proc) { return [proc] { return std::unique_ptr<AudioProcessor>(proc()); }; };

    d = {};
    d.id = QStringLiteral("a_gain");
    d.name = QStringLiteral("Gain");
    d.category = QStringLiteral("Utility");
    d.description = QStringLiteral("Volume change in dB, optional phase invert. Normalize adds one of these.");
    d.params = {num("gain", "Gain", -60, 24, 0, 1, " dB"), flag("invert", "Invert phase", false)};
    d.create = make([] { return new GainProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_eq");
    d.name = QStringLiteral("Parametric EQ");
    d.category = QStringLiteral("EQ & Filters");
    d.description = QStringLiteral("Low cut, low shelf, two bell bands, high shelf and high cut. Bands at 0 dB are off.");
    d.params = {num("lowCut", "Low cut", 20, 1000, 20, 0, " Hz", true),
                num("lowFreq", "Low shelf frequency", 20, 1000, 100, 0, " Hz", true),
                num("lowGain", "Low shelf gain", -24, 24, 0, 1, " dB"),
                num("mid1Freq", "Mid 1 frequency", 40, 16000, 500, 0, " Hz", true),
                num("mid1Gain", "Mid 1 gain", -24, 24, 0, 1, " dB"),
                num("mid1Q", "Mid 1 width (Q)", 0.1, 10, 1, 2, "", true),
                num("mid2Freq", "Mid 2 frequency", 40, 16000, 2500, 0, " Hz", true),
                num("mid2Gain", "Mid 2 gain", -24, 24, 0, 1, " dB"),
                num("mid2Q", "Mid 2 width (Q)", 0.1, 10, 1, 2, "", true),
                num("highFreq", "High shelf frequency", 1000, 20000, 8000, 0, " Hz", true),
                num("highGain", "High shelf gain", -24, 24, 0, 1, " dB"),
                num("highCut", "High cut", 1000, 20000, 20000, 0, " Hz", true)};
    d.create = make([] { return new EqProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_filter");
    d.name = QStringLiteral("Filter (low/high/band-pass, notch)");
    d.category = QStringLiteral("EQ & Filters");
    d.description = QStringLiteral("Resonant filter; 24 dB/octave stacks two stages.");
    d.params = {choice("type", "Type", {QStringLiteral("Low-pass"), QStringLiteral("High-pass"), QStringLiteral("Band-pass"), QStringLiteral("Notch")}),
                num("frequency", "Frequency", 20, 20000, 1000, 0, " Hz", true), num("q", "Resonance (Q)", 0.1, 20, 0.707, 3, "", true),
                choice("slope", "Slope", {QStringLiteral("12 dB/octave"), QStringLiteral("24 dB/octave")})};
    d.create = make([] { return new FilterProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_compressor");
    d.name = QStringLiteral("Compressor");
    d.category = QStringLiteral("Dynamics");
    d.description = QStringLiteral("Evens out loud and quiet parts. Stereo-linked, soft knee.");
    d.params = {num("threshold", "Threshold", -60, 0, -18, 1, " dB"), num("ratio", "Ratio", 1, 20, 4, 1, ":1"),
                num("attack", "Attack", 0.1, 200, 10, 1, " ms", true), num("release", "Release", 5, 2000, 150, 0, " ms", true),
                num("knee", "Knee", 0, 24, 6, 1, " dB"), num("makeup", "Makeup gain", 0, 24, 0, 1, " dB")};
    d.create = make([] { return new CompressorProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_limiter");
    d.name = QStringLiteral("Limiter");
    d.category = QStringLiteral("Dynamics");
    d.description = QStringLiteral("Brick-wall limiter with 2 ms lookahead: nothing exceeds the ceiling, between samples (true peak) included.");
    d.params = {num("ceiling", "Ceiling", -24, 0, -0.3, 1, " dB"), num("release", "Release", 1, 1000, 60, 0, " ms", true)};
    d.create = make([] { return new LimiterProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_gate");
    d.name = QStringLiteral("Noise Gate");
    d.category = QStringLiteral("Dynamics");
    d.description = QStringLiteral("Silences audio below the threshold (hum, room noise between words).");
    d.params = {num("threshold", "Threshold", -90, 0, -45, 1, " dB"), num("range", "Range (closed level)", -90, 0, -80, 0, " dB"),
                num("attack", "Attack", 0.1, 100, 1, 1, " ms", true), num("hold", "Hold", 0, 1000, 50, 0, " ms"),
                num("release", "Release", 5, 3000, 150, 0, " ms", true)};
    d.create = make([] { return new GateProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_reverb");
    d.name = QStringLiteral("Reverb");
    d.category = QStringLiteral("Reverb & Delay");
    d.description = QStringLiteral("Room reverb (Freeverb). On a track, the tail rings out past the end of events.");
    d.params = {num("room", "Room size", 0, 1, 0.6, 2), num("damping", "Damping", 0, 1, 0.4, 2), num("wet", "Wet", 0, 1, 0.25, 2),
                num("dry", "Dry", 0, 1, 1, 2), num("width", "Stereo width", 0, 1, 1, 2), num("predelay", "Pre-delay", 0, 200, 10, 0, " ms")};
    d.create = make([] { return new ReverbProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_delay");
    d.name = QStringLiteral("Delay / Echo");
    d.category = QStringLiteral("Reverb & Delay");
    d.description = QStringLiteral("Repeating echoes, optionally bouncing between left and right.");
    d.params = {num("time", "Delay time", 1, 2000, 350, 0, " ms", true), num("feedback", "Feedback", 0, 0.95, 0.35, 2),
                num("mix", "Mix", 0, 1, 0.3, 2), num("damping", "Echo brightness", 500, 20000, 8000, 0, " Hz", true),
                flag("pingpong", "Ping-pong", false)};
    d.create = make([] { return new DelayProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_chorus");
    d.name = QStringLiteral("Chorus");
    d.category = QStringLiteral("Modulation");
    d.description = QStringLiteral("Thickens the sound with slowly modulated copies.");
    d.params = {num("rate", "Rate", 0.01, 10, 0.8, 2, " Hz", true), num("depth", "Depth", 0, 15, 3, 1, " ms"),
                num("delay", "Delay", 0.5, 40, 15, 1, " ms"), num("feedback", "Feedback", -0.95, 0.95, 0, 2), num("mix", "Mix", 0, 1, 0.5, 2)};
    d.create = make([] { return new ModDelayProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_flanger");
    d.name = QStringLiteral("Flanger");
    d.category = QStringLiteral("Modulation");
    d.description = QStringLiteral("Jet-plane sweep from a short modulated delay with feedback.");
    d.params = {num("rate", "Rate", 0.01, 10, 0.25, 2, " Hz", true), num("depth", "Depth", 0, 10, 2, 1, " ms"),
                num("delay", "Delay", 0.1, 10, 1.5, 1, " ms"), num("feedback", "Feedback", -0.95, 0.95, 0.6, 2), num("mix", "Mix", 0, 1, 0.5, 2)};
    d.create = make([] { return new ModDelayProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_stereo");
    d.name = QStringLiteral("Stereo Tools");
    d.category = QStringLiteral("Utility");
    d.description = QStringLiteral("Stereo width, balance, channel swap, per-channel phase invert, mono.");
    d.params = {num("width", "Width", 0, 2, 1, 2), num("balance", "Balance", -1, 1, 0, 2), flag("swap", "Swap left/right", false),
                flag("invertL", "Invert left phase", false), flag("invertR", "Invert right phase", false), flag("mono", "Mono", false)};
    d.create = make([] { return new StereoProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_denoise");
    d.name = QStringLiteral("Noise Reduction");
    d.category = QStringLiteral("Restoration");
    d.description = QStringLiteral("Removes steady background noise (hiss, hum, fans) with FFmpeg's FFT denoiser.");
    d.params = {num("amount", "Reduction", 1, 60, 12, 0, " dB"), num("floor", "Noise floor", -80, -20, -40, 0, " dB"),
                flag("adaptive", "Re-estimate noise continuously (for changing noise; weaker)", false)};
    d.create = [] {
        return std::unique_ptr<AudioProcessor>(new FfmpegAudioProcessor([](const Params& p, int) {
            return QStringLiteral("afftdn=nr=%1:nf=%2:tn=%3")
                .arg(p.num(QStringLiteral("amount")), 0, 'f', 1)
                .arg(p.num(QStringLiteral("floor")), 0, 'f', 1)
                .arg(p.flag(QStringLiteral("adaptive")) ? 1 : 0);
        }));
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_dehum");
    d.name = QStringLiteral("De-hum");
    d.category = QStringLiteral("Restoration");
    d.description = QStringLiteral("Takes out mains hum (a steady buzz at 50 or 60 Hz and its harmonics) with narrow notches, "
                                   "leaving the rest of the sound alone. The spectrogram shows the hum as flat lines.");
    d.params = {choice("mains", "Mains", {QStringLiteral("50 Hz"), QStringLiteral("60 Hz")}),
                num("harmonics", "Harmonics", 1, 8, 4, 0), num("q", "Narrowness (Q)", 2, 60, 18, 0, "", true)};
    d.create = make([] { return new DehumProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_dereverb");
    d.name = QStringLiteral("De-reverb");
    d.category = QStringLiteral("Restoration");
    d.description = QStringLiteral("Dries out a voice recorded in an echoey room (a kitchen, a hall, a laptop's microphone across the "
                                   "room): the room's tail after each sound is turned down, the voice itself stays. Set Reverb time "
                                   "to how long the room rings.");
    d.params = {num("amount", "Reduction", 0, 1, 0.85, 2), num("reverb", "Reverb time", 0.1, 4, 0.8, 2, " s"),
                num("floor", "Most taken out", -40, 0, -18, 0, " dB")};
    d.create = make([] { return new DereverbProcessor; });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_declick");
    d.name = QStringLiteral("De-click");
    d.category = QStringLiteral("Restoration");
    d.description = QStringLiteral("Repairs clicks, pops and crackle (vinyl, mouth clicks, digital glitches) with FFmpeg's de-click: "
                                   "short bursts are found and filled in from the sound either side. Can also repair clipping.");
    d.params = {num("threshold", "Sensitivity (lower finds more)", 1, 20, 2, 1), num("window", "Window", 10, 100, 55, 0, " ms"),
                flag("declip", "Repair clipping too", false)};
    d.create = [] {
        return std::unique_ptr<AudioProcessor>(new FfmpegAudioProcessor([](const Params& p, int) {
            QString f = QStringLiteral("adeclick=w=%1:o=75:t=%2").arg(p.num(QStringLiteral("window")), 0, 'f', 0).arg(p.num(QStringLiteral("threshold")), 0, 'f', 1);
            if (p.flag(QStringLiteral("declip")))
                f += QStringLiteral(",adeclip");
            return f;
        }));
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_pitch");
    d.name = QStringLiteral("Pitch Shift");
    d.category = QStringLiteral("Restoration");
    d.description = QStringLiteral("Changes pitch without changing speed.");
    d.params = {num("semitones", "Semitones", -24, 24, 0, 1), num("cents", "Fine tune", -100, 100, 0, 0, " cents")};
    d.create = [] {
        return std::unique_ptr<AudioProcessor>(new FfmpegAudioProcessor([](const Params& p, int rate) {
            const double k = std::pow(2.0, (p.num(QStringLiteral("semitones")) + p.num(QStringLiteral("cents")) / 100.0) / 12.0);
            if (std::abs(k - 1.0) < 1e-4)
                return QString();
            if (hasFilter("rubberband"))
                return QStringLiteral("rubberband=pitch=%1").arg(k, 0, 'f', 6);
            // Fallback: resample (pitch + speed), then restore the speed.
            return QStringLiteral("asetrate=%1,aresample=%2,atempo=%3").arg(int(std::lround(rate * k))).arg(rate).arg(1.0 / k, 0, 'f', 6);
        }));
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_vst3");
    d.name = QStringLiteral("VST3 Plugin");
    d.category = QStringLiteral("Plugins");
    d.description = QStringLiteral("Runs a VST3 audio effect plugin. Pick it with the plug button on the device (installed plugins) "
                                   "or Browse; the button also opens the plugin's own window. Its settings are saved with the project.");
    {
        ParamDef plugin;
        plugin.id = QStringLiteral("plugin");
        plugin.name = QStringLiteral("Plugin file");
        plugin.type = ParamType::Text;
        plugin.fileFilter = QStringLiteral("VST3 plugins (*.vst3);;All files (*)");
        ParamDef cls = plugin;
        cls.id = QStringLiteral("class");
        cls.name = QStringLiteral("Plugin");
        cls.fileFilter.clear();
        cls.hidden = true;
        ParamDef state = cls;
        state.id = QStringLiteral("state");
        state.name = QStringLiteral("Plugin state");
        ParamDef mix;
        mix.id = QStringLiteral("mix");
        mix.name = QStringLiteral("Dry / wet");
        mix.def = 1;
        d.params = {plugin, cls, state, mix};
    }
    d.create = createVst3Processor;
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("a_ffmpeg");
    d.name = QStringLiteral("FFmpeg Audio Filter (advanced)");
    d.category = QStringLiteral("Advanced");
    d.description = QStringLiteral("Runs any FFmpeg audio filter chain, e.g. \"loudnorm\", \"dynaudnorm\", \"deesser\", "
                                   "\"acompressor=threshold=0.1\", \"highpass=f=80\". Filters that change length (atempo) "
                                   "don't belong here - use Playback Speed instead.");
    {
        ParamDef t;
        t.id = QStringLiteral("filter");
        t.name = QStringLiteral("Filter chain");
        t.type = ParamType::Text;
        t.defText = QStringLiteral("highpass=f=80,lowpass=f=14000");
        t.tooltip = d.description;
        d.params = {t};
    }
    d.create = [] {
        return std::unique_ptr<AudioProcessor>(
            new FfmpegAudioProcessor([](const Params& p, int) { return p.text(QStringLiteral("filter")); }));
    };
    list.push_back(d);
    return list;
}

} // namespace

bool audioFilterSpecValid(const QString& spec, int rate)
{
    FfmpegAudioProcessor p([spec](const Params&, int) { return spec; });
    p.reset(rate);
    p.latency(Params{});
    return !p.failed();
}

const std::vector<AudioEffectDef>& audioEffects()
{
    static const std::vector<AudioEffectDef> list = buildAudioEffects();
    return list;
}

const AudioEffectDef* findAudioEffect(const QString& id)
{
    for (const auto& e : audioEffects())
        if (e.id == id)
            return &e;
    return nullptr;
}

std::optional<EffectMeta> effectMeta(const QString& type)
{
    if (const EffectDef* v = findEffect(type))
        return EffectMeta{v->name, v->category, v->description, &v->params, false};
    if (const AudioEffectDef* a = findAudioEffect(type))
        return EffectMeta{a->name, a->category, a->description, &a->params, true};
    return std::nullopt;
}

EffectInstance makeEffectOfType(const QString& type, int id)
{
    EffectInstance e;
    e.id = id;
    e.type = type;
    if (auto meta = effectMeta(type))
        e.params = defaultParams(*meta->params);
    return e;
}

double audioEffectResponseDb(const QString& type, const Params& p, double freq, int rate)
{
    Biquad b;
    if (type == QLatin1String("a_eq")) {
        double db = 0;
        for (const EqBand& band : eqBands(p))
            if (band.active) {
                b.set(band.type, rate, band.freq, band.q, band.gain);
                db += b.responseDb(freq, rate);
            }
        return db;
    }
    if (type == QLatin1String("a_dehum")) {
        double db = 0;
        for (const auto& [f, q] : humNotches(p)) {
            b.set(Biquad::Notch, rate, f, q, 0);
            db += b.responseDb(freq, rate);
        }
        return db;
    }
    if (type == QLatin1String("a_filter")) {
        static const Biquad::Type types[] = {Biquad::LowPass, Biquad::HighPass, Biquad::BandPass, Biquad::Notch};
        b.set(types[std::clamp(p.choice(QStringLiteral("type")), 0, 3)], rate, p.num(QStringLiteral("frequency")), p.num(QStringLiteral("q")), 0);
        return b.responseDb(freq, rate) * (p.choice(QStringLiteral("slope")) == 1 ? 2 : 1);
    }
    return std::nan("");
}

double audioEffectTransferDb(const QString& type, const Params& p, double in)
{
    if (type == QLatin1String("a_compressor"))
        return in + compressorGainDb(in - p.num(QStringLiteral("threshold")), p.num(QStringLiteral("ratio")), p.num(QStringLiteral("knee")))
               + p.num(QStringLiteral("makeup"));
    if (type == QLatin1String("a_gate"))
        return in > p.num(QStringLiteral("threshold")) ? in : in + p.num(QStringLiteral("range"));
    if (type == QLatin1String("a_limiter"))
        return std::min(in, p.num(QStringLiteral("ceiling")));
    return std::nan("");
}
