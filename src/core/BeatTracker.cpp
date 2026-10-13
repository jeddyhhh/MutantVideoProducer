#include "core/BeatTracker.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace {
constexpr int kLookahead = 3;       // frames a hit must stay the local peak for (~16 ms)
constexpr double kSilence = 1e-7;   // mean square below this (-70 dBFS) is silence
constexpr double kStateStep = 1.004; // tempo states are 0.4% apart
constexpr int kFluxLag = 3;         // spectral flux compares with the frame ~16 ms earlier
constexpr double kSlowest = 40;     // tempos (BPM) looked for, in any octave
constexpr double kFastest = 250;

// Where a tempo is in its octave (0..1): octaves of a tempo have the same class.
double tempoClass(double bpm)
{
    const double u = std::log2(bpm / 60.0);
    return u - std::floor(u);
}

// The distance between two classes around the circle (0..0.5).
double classDistance(double a, double b)
{
    double d = std::abs(a - b);
    d -= std::floor(d);
    return std::min(d, 1 - d);
}

// In-place radix-2 FFT (size a power of two).
void fft(std::vector<std::complex<float>>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2 * std::numbers::pi / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1, 0);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = a[i + k];
                const std::complex<float> v = a[i + k + len / 2] * std::complex<float>(w);
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}
} // namespace

BeatTracker::BeatTracker(int sampleRate) : m_rate(std::max(8000, sampleRate))
{
    m_hop = std::max(32, int(std::lround(m_rate * 256.0 / 48000.0))); // ~5.3 ms
    m_fps = double(m_rate) / m_hop;
    setupFilters();
    setupSpectrum();
    setupTempoStates();
}

void BeatTracker::setSettings(const BeatSettings& s)
{
    std::lock_guard lock(m_mutex);
    const bool bandChanged = s.band != m_s.band;
    m_s = s;
    m_s.minBpm = std::clamp(m_s.minBpm, 30.0, 290.0);
    m_s.maxBpm = std::clamp(m_s.maxBpm, m_s.minBpm + 10, 300.0);
    m_s.windowSeconds = std::clamp(m_s.windowSeconds, 2.0, 12.0);
    if (bandChanged) {
        setupFilters();
        setupSpectrum();
    }
    // (A new range only changes the octave the tempo is shown in: the next estimate.)
}

BeatSettings BeatTracker::settings() const
{
    std::lock_guard lock(m_mutex);
    return m_s;
}

void BeatTracker::reset()
{
    std::lock_guard lock(m_mutex);
    const bool locked = m_tempoLocked; // a locked tempo (and an octave set by hand) stays: a new input, the same music
    const double lockedBpm = m_bpm;
    setupFilters();
    setupSpectrum();
    m_acc = 0;
    m_accN = 0;
    m_accStart = -1;
    m_level.clear();
    m_onset.clear();
    m_flux.clear();
    m_onsetTime.clear();
    m_threshold.clear();
    m_hitFlags.clear();
    m_beatFlags.clear();
    m_lastHit = -1e9;
    m_bpm = locked ? lockedBpm : 0;
    m_confidence = 0;
    m_tempoLocked = locked;
    m_octavePinned = m_octavePinned && locked; // (with no tempo kept there is no octave to keep)
    m_nextBeat = -1;
    m_kfT = 0;
    m_phaseByHand = false;
    m_phaseVotes = 0;
    m_out.clear();
    setupTempoStates();
}

void BeatTracker::setupFilters()
{
    // RBJ cookbook biquads, Q = 0.707.
    auto make = [&](bool highPass, double freq) {
        Biquad f;
        const double w = 2 * std::numbers::pi * freq / m_rate, cw = std::cos(w), alpha = std::sin(w) / (2 * 0.7071);
        const double a0 = 1 + alpha;
        if (highPass) {
            f.b0 = (1 + cw) / 2 / a0;
            f.b1 = -(1 + cw) / a0;
        } else {
            f.b0 = (1 - cw) / 2 / a0;
            f.b1 = (1 - cw) / a0;
        }
        f.b2 = f.b0;
        f.a1 = -2 * cw / a0;
        f.a2 = (1 - alpha) / a0;
        return f;
    };
    m_filters.clear();
    // A hit shows up about one frame after it starts, plus the filters' delay.
    const double frame = double(m_hop) / m_rate;
    m_detectDelay = frame * 0.9 - 0.003 + (m_s.band == BeatSettings::Bass ? 0.0025 : m_s.band == BeatSettings::Mids ? 0.0006 : 0.0002);
    switch (m_s.band) {
    case BeatSettings::Bass: m_filters = {make(false, 150), make(false, 150)}; break;    // kick drum
    case BeatSettings::Mids: m_filters = {make(true, 300), make(false, 3000)}; break;    // snare, claps, vocals
    case BeatSettings::Highs: m_filters = {make(true, 6000), make(true, 6000)}; break;   // hi-hats, shakers
    default: break;
    }
}

void BeatTracker::setupSpectrum()
{
    m_fftSize = m_rate >= 32000 ? 2048 : 1024; // ~43 ms
    const size_t n = size_t(m_fftSize);
    if (m_ring.size() != n) {
        m_ring.assign(n, 0.0f);
        m_ringPos = 0;
    }
    m_window.resize(n);
    for (size_t i = 0; i < n; ++i)
        m_window[i] = float(0.5 - 0.5 * std::cos(2 * std::numbers::pi * double(i) / double(n - 1)));
    m_fft.resize(n);
    // Bands a quarter of an octave wide from 30 Hz (narrow ones merged up to a bin),
    // in four regions; the one chosen in Listen to counts double.
    m_bands.clear();
    m_regionWeight = {m_s.band == BeatSettings::Bass ? 2.0 : 1.0, m_s.band == BeatSettings::Mids ? 2.0 : 1.0,
                      m_s.band == BeatSettings::Mids ? 2.0 : 1.0, m_s.band == BeatSettings::Highs ? 2.0 : 1.0};
    const double top = std::min(16000.0, 0.45 * m_rate);
    auto bin = [&](double f) { return int(std::ceil(f * double(n) / m_rate)); };
    int from = std::max(1, bin(30));
    for (double f = 30; f < top;) {
        const double next = std::min(top, f * std::pow(2.0, 0.25));
        const int to = bin(next);
        if (to > from) {
            const double centre = std::sqrt(f * next);
            m_bands.push_back({from, to, centre < 160 ? 0 : centre < 800 ? 1 : centre < 4000 ? 2 : 3});
            from = to;
        }
        f = next;
    }
    m_spectra.clear();
}

void BeatTracker::setupTempoStates()
{
    // One octave of tempos, 60 to 120 BPM, standing for all their octaves (the range
    // picks the octave): a circle, since 120 is 60 again.
    const int count = int(std::lround(std::log(2.0) / std::log(kStateStep)));
    m_states.resize(size_t(count));
    for (int i = 0; i < count; ++i)
        m_states[size_t(i)] = 60.0 * std::pow(2.0, double(i) / count);
    m_belief.assign(m_states.size(), 1.0 / double(count));
    if (m_bpm > 0) { // carry on from the current tempo
        const double u = tempoClass(m_bpm);
        double sum = 0;
        for (int i = 0; i < count; ++i) {
            const double d = classDistance(double(i) / count, u) * count;
            sum += m_belief[size_t(i)] = 0.02 / double(count) + std::exp(-0.5 * d * d / 4);
        }
        for (double& v : m_belief)
            v /= sum;
    }
}

BeatTracker::Flux BeatTracker::spectralFlux()
{
    const size_t n = m_ring.size();
    for (size_t i = 0; i < n; ++i)
        m_fft[i] = std::complex<float>(m_ring[(m_ringPos + i) % n] * m_window[i], 0.0f);
    fft(m_fft);
    const float norm = 4.0f / float(n); // a full-scale sine reads about 1
    const size_t top = size_t(m_bands.empty() ? 1 : m_bands.back().to);
    Spectrum spec;
    spec.mag.resize(top);
    for (size_t k = 0; k < top; ++k)
        spec.mag[k] = std::abs(m_fft[k]) * norm;
    spec.voice.assign(top, 0);
    findVoice(spec);
    m_spectra.push_back(std::move(spec));
    while (m_spectra.size() > size_t(kFluxLag) + 1)
        m_spectra.pop_front();
    Flux flux{};
    if (m_spectra.size() <= size_t(kFluxLag))
        return flux;
    // Each band's rise in loudness since ~16 ms before, leaving out the bins the
    // voice's harmonics are in, now or then (so a note starting or stopping adds
    // nothing, and losing the voice for a frame doesn't either).
    const Spectrum& now = m_spectra.back();
    const Spectrum& before = m_spectra.front();
    std::array<int, kRegions> bands{};
    for (const Band& band : m_bands) {
        double e = 0, eBefore = 0;
        int kept = 0;
        for (int k = band.from; k < band.to; ++k)
            if (!now.voice[size_t(k)] && !before.voice[size_t(k)]) {
                e += double(now.mag[size_t(k)]) * now.mag[size_t(k)];
                eBefore += double(before.mag[size_t(k)]) * before.mag[size_t(k)];
                ++kept;
            }
        ++bands[size_t(band.region)];
        if (kept == 0)
            continue;
        const double scale = double(band.to - band.from) / kept; // as if the band were whole
        flux[size_t(band.region)] += float(std::max(0.0, std::log1p(100.0 * std::sqrt(e * scale)) - std::log1p(100.0 * std::sqrt(eBefore * scale))));
    }
    for (int g = 0; g < kRegions; ++g)
        if (bands[size_t(g)] > 0)
            flux[size_t(g)] /= float(bands[size_t(g)]);
    return flux;
}

void BeatTracker::findVoice(Spectrum& spec)
{
    // A singing voice (or any one strong held pitch) has evenly spaced harmonics:
    // a ripple across the log spectrum, which its cepstrum (the spectrum of the
    // log spectrum: "quefrency", from looking for echoes in seismograms) shows as
    // a peak at the pitch's period. Pitches of 80 Hz to 1 kHz, harmonics above
    // ~160 Hz (the kick drum's range is left alone) up to 5 kHz.
    constexpr double threshold = 3.0, width = 0.03; // peak height in deviations; notch half-width
    const size_t n = m_fft.size();
    std::vector<std::complex<float>>& c = m_cepstrum;
    c.resize(n);
    for (size_t k = 0; k <= n / 2; ++k) {
        const float v = std::log(std::abs(m_fft[k]) + 1e-4f);
        c[k] = v;
        if (k > 0 && k < n / 2)
            c[n - k] = v;
    }
    fft(c); // (real and even: the forward transform is the inverse, scaled)
    const int qLo = int(m_rate / 1000.0), qHi = std::min(int(n / 2) - 2, int(m_rate / 80.0));
    double mean = 0, sq = 0;
    int best = qLo;
    for (int q = qLo; q <= qHi; ++q) {
        const double v = c[size_t(q)].real();
        mean += v;
        sq += v * v;
        if (v > c[size_t(best)].real())
            best = q;
    }
    const int count = qHi - qLo + 1;
    mean /= count;
    const double dev = std::sqrt(std::max(1e-12, sq / count - mean * mean));
    if ((c[size_t(best)].real() - mean) / dev < threshold)
        return; // no clear pitch
    // Sub-sample period (a parabola), then mark the bins around each harmonic.
    double q = best;
    const double a = c[size_t(best) - 1].real(), b = c[size_t(best)].real(), d = c[size_t(best) + 1].real();
    if (const double denom = a - 2 * b + d; denom < 0)
        q += std::clamp(0.5 * (a - d) / denom, -0.5, 0.5);
    const double f0 = m_rate / q;
    const double binHz = double(m_rate) / double(n);
    for (int h = 1; h * f0 < 5000; ++h) {
        const double f = h * f0;
        if (f < 160)
            continue;
        const double half = std::max(1.0, f * width / binHz);
        const int lo = std::max(0, int(std::floor(f / binHz - half))), hi = std::min(int(spec.voice.size()) - 1, int(std::ceil(f / binHz + half)));
        for (int k = lo; k <= hi; ++k)
            spec.voice[size_t(k)] = 1;
    }
}

void BeatTracker::process(const float* stereo, int frames, double time)
{
    std::lock_guard lock(m_mutex);
    for (int i = 0; i < frames; ++i) {
        if (m_accN == 0)
            m_accStart = time + double(i) / m_rate;
        double x = stereo ? 0.5 * (double(stereo[i * 2]) + double(stereo[i * 2 + 1])) : 0.0;
        m_ring[m_ringPos] = float(x);
        m_ringPos = (m_ringPos + 1) % m_ring.size();
        for (Biquad& f : m_filters)
            x = f.run(x);
        m_acc += x * x;
        if (++m_accN == m_hop) {
            frame(m_acc / m_hop, spectralFlux(), m_accStart);
            m_acc = 0;
            m_accN = 0;
        }
    }
}

void BeatTracker::frame(double energy, const Flux& flux, double time)
{
    m_now = time + double(m_hop) / m_rate;
    // Onset strength: how far the (log) level rose above the last few frames.
    const double level = std::log(energy + 1e-12);
    double onset = 0;
    if (energy > kSilence) {
        m_lastSound = time;
        if (m_level.size() >= 4) {
            double prev = 0;
            for (size_t i = m_level.size() - 4; i < m_level.size(); ++i)
                prev += m_level[i];
            onset = std::max(0.0, level - prev / 4);
        }
    }
    m_level.push_back(level);
    if (m_level.size() > 8)
        m_level.pop_front();

    // Adaptive threshold: mean + k * deviation of the last second of onsets.
    const size_t span = std::min(m_onset.size(), size_t(m_fps));
    double mean = 0, var = 0;
    for (size_t i = m_onset.size() - span; i < m_onset.size(); ++i)
        mean += m_onset[i];
    mean = span ? mean / double(span) : 0.0;
    for (size_t i = m_onset.size() - span; i < m_onset.size(); ++i)
        var += (m_onset[i] - mean) * (m_onset[i] - mean);
    const double dev = span ? std::sqrt(var / double(span)) : 0.0;
    const double k = 2.6 - 2.2 * std::clamp(m_s.sensitivity, 0.0, 1.0);
    const double floor = 0.6 - 0.5 * std::clamp(m_s.sensitivity, 0.0, 1.0); // ~2.6 dB .. 0.4 dB rises
    const double thr = std::max(floor, mean + k * dev);

    m_onset.push_back(float(onset));
    m_flux.push_back(flux);
    m_onsetTime.push_back(time);
    m_threshold.push_back(float(thr));
    m_hitFlags.push_back(false);
    m_beatFlags.push_back(false);
    const size_t keep = size_t(std::max(m_s.windowSeconds * m_fps, 700.0)) + kLookahead;
    while (m_onset.size() > keep) {
        m_onset.pop_front();
        m_flux.pop_front();
        m_onsetTime.pop_front();
        m_threshold.pop_front();
        m_hitFlags.pop_front();
        m_beatFlags.pop_front();
    }

    // A hit: a local peak (over +-kLookahead frames) above its threshold.
    const size_t n = m_onset.size();
    if (n > 2 * kLookahead) {
        const size_t c = n - 1 - kLookahead;
        const float v = m_onset[c];
        bool peak = v > m_threshold[c];
        for (size_t i = c - kLookahead; peak && i <= c + kLookahead; ++i)
            if (i != c && (m_onset[i] > v || (i < c && m_onset[i] == v)))
                peak = false;
        const double t = m_onsetTime[c] - m_detectDelay;
        if (peak && (t - m_lastHit) * 1000.0 >= m_s.minGapMs) {
            m_lastHit = t;
            m_hitFlags[c] = true;
            if (!m_s.locked || m_bpm <= 0) {
                emitBeat(t);
                m_beatFlags[c] = true;
            }
            if (m_s.locked && m_bpm > 0) {
                if (m_nextBeat < 0) { // the grid starts on this hit (which offset is right is checked later)
                    m_nextBeat = t + 60.0 / m_bpm;
                    resetGrid();
                    emitBeat(t);
                    m_beatFlags[c] = true;
                } else {
                    kalmanHit(t);
                }
            }
        }
    }

    if (++m_sinceTempo >= int(m_fps / 4)) { // four times a second
        m_sinceTempo = 0;
        estimateTempo();
        // The grid hears the tempo too: a precise look at its period.
        if (m_kfT > 0 && m_bpm > 0) {
            const double t = 60.0 / m_bpm;
            if (m_tempoLocked) {
                m_kfT = t;
                m_kfP[1] = m_kfP[2] = m_kfP[3] = 0;
            } else if (std::abs(t / m_kfT - 1) > 0.04) {
                resetGrid(); // another tempo
            } else {
                const double r = std::pow(0.003 * t, 2), sv = m_kfP[3] + r;
                const double k0 = m_kfP[1] / sv, k1 = m_kfP[3] / sv, y = t - m_kfT;
                m_nextBeat += k0 * y;
                m_kfT += k1 * y;
                const std::array<double, 4> p = m_kfP;
                m_kfP = {p[0] - k0 * p[2], p[1] - k0 * p[3], p[2] - k1 * p[2], p[3] - k1 * p[3]};
            }
        }
    }

    // Locked beats: fire each grid beat when the audio reaches it, early by the latency.
    if (m_s.locked && m_bpm > 0 && m_nextBeat > 0) {
        const double period = gridPeriod();
        const double now = time + double(m_hop) / m_rate;
        if (now - m_nextBeat > 4 * period)
            m_nextBeat = -1; // fell far behind (a gap in the input): wait for the next hit
        while (m_nextBeat > 0 && m_nextBeat - m_s.latencyMs / 1000.0 <= now) {
            if (now - m_lastSound < 2.0) { // stop on silence
                emitBeat(m_nextBeat);
                m_beatFlags.back() = true;
            }
            // Predict: a beat on, and less sure of when the next one is (4 ms) and of the period.
            m_nextBeat += period;
            const std::array<double, 4> p = m_kfP;
            m_kfP = {p[0] + p[1] + p[2] + p[3] + 0.004 * 0.004, p[1] + p[3], p[2] + p[3],
                     p[3] + std::pow(0.004 * period, 2) * (1.2 - std::clamp(m_s.stability, 0.0, 1.0))};
            if (m_tempoLocked)
                m_kfP[1] = m_kfP[2] = m_kfP[3] = 0;
        }
    }
}

double BeatTracker::gridPeriod() const
{
    return m_tempoLocked || m_kfT <= 0 ? 60.0 / m_bpm : m_kfT;
}

void BeatTracker::resetGrid()
{
    m_kfT = m_bpm > 0 ? 60.0 / m_bpm : 0;
    m_kfP = {0.02 * 0.02, 0, 0, std::pow(0.01 * m_kfT, 2)};
}

void BeatTracker::kalmanHit(double t)
{
    // The beat grid is a Kalman filter (as steered Apollo to the Moon): its state is
    // when the next beat is and the period, each with how sure it is of it. A hit
    // near a beat is a noisy look at that beat (m periods before the next one:
    // H = [1, -m]); how far the grid moves depends on how sure it is against how
    // noisy hits are (more so at high Stability), not on a fixed gain.
    const double period = gridPeriod();
    const double m = std::round((m_nextBeat - t) / period);
    const double predicted = m_nextBeat - m * period, y = t - predicted;
    if (std::abs(y) >= 0.22 * period)
        return; // not on a beat: an off-beat hit or a fill
    const double sigma = 0.006 + 0.03 * std::clamp(m_s.stability, 0.0, 1.0); // how far hits are off their beat
    const std::array<double, 4>& p = m_kfP; // [tt, tT, Tt, TT]
    const double ph0 = p[0] - m * p[1], ph1 = p[2] - m * p[3]; // P H'
    const double sv = ph0 - m * ph1 + sigma * sigma;
    const double k0 = ph0 / sv, k1 = m_tempoLocked ? 0.0 : ph1 / sv;
    m_nextBeat += k0 * y;
    m_kfT += k1 * y;
    // P = (I - K H) P
    const double h0 = 1, h1 = -m;
    const std::array<double, 4> q = p;
    m_kfP = {q[0] - k0 * (h0 * q[0] + h1 * q[2]), q[1] - k0 * (h0 * q[1] + h1 * q[3]),
             q[2] - k1 * (h0 * q[0] + h1 * q[2]), q[3] - k1 * (h0 * q[1] + h1 * q[3])};
}

void BeatTracker::estimateTempo()
{
    const size_t n = m_flux.size();
    const size_t window = std::min(n, size_t(m_s.windowSeconds * m_fps));
    if (window < size_t(2.0 * m_fps) || m_states.empty())
        return;
    // Each region's flux over the window, less its local average (slow swells and
    // builds aren't beats), scaled to a similar size and added up: a region with
    // little going on still counts for less.
    const int maxLag = std::min(int(double(window) * 0.7), int(4 * 60.0 * m_fps / kSlowest) + 2);
    std::vector<double> o(window, 0.0), r(size_t(maxLag) + 1, 0.0), phaseCurve(window, 0.0);
    {
        std::array<std::vector<double>, kRegions> x;
        std::array<double, kRegions> dev{};
        double topDev = 0;
        std::vector<double> prefix(window + 1);
        const size_t half = size_t(m_fps / 4);
        for (int g = 0; g < kRegions; ++g) {
            std::vector<double>& xg = x[size_t(g)];
            xg.resize(window);
            prefix[0] = 0;
            for (size_t i = 0; i < window; ++i)
                prefix[i + 1] = prefix[i] + m_flux[n - window + i][size_t(g)];
            double sq = 0;
            for (size_t i = 0; i < window; ++i) {
                const size_t a = i > half ? i - half : 0, b = std::min(window, i + half + 1);
                xg[i] = m_flux[n - window + i][size_t(g)] - (prefix[b] - prefix[a]) / double(b - a);
                sq += xg[i] * xg[i];
            }
            dev[size_t(g)] = std::sqrt(sq / double(window));
            topDev = std::max(topDev, dev[size_t(g)]);
        }
        if (topDev <= 1e-9)
            return; // silence: keep the tempo
        for (int g = 0; g < kRegions; ++g) {
            const double w = m_regionWeight[size_t(g)] / (dev[size_t(g)] + 0.25 * topDev);
            const double wp = w * (g == 0 ? 2.0 : 1.0); // the beat is mostly where the kicks are
            for (size_t i = 0; i < window; ++i) {
                o[i] += w * x[size_t(g)][i];
                phaseCurve[i] += wp * std::max(0.0, x[size_t(g)][i]);
            }
        }
    }
    for (int lag = 0; lag <= maxLag; ++lag) {
        double s = 0;
        for (size_t i = size_t(lag); i < window; ++i)
            s += o[i] * o[i - size_t(lag)];
        r[size_t(lag)] = s / double(window - size_t(lag));
    }
    const double r0 = r[0];
    if (r0 <= 1e-12)
        return; // silence: keep the tempo
    auto rf = [&](double lag) {
        if (lag < 0 || lag >= maxLag)
            return 0.0;
        const int i = int(lag);
        const double f = lag - i;
        return r[size_t(i)] * (1 - f) + r[size_t(i) + 1] * f;
    };
    // A tempo's score: its beat and the multiples of it (bars repeat too), and, when
    // telling tempos apart that aren't octaves of each other, its eighth notes (a
    // dotted-eighth echo repeats too, but doesn't divide in two).
    auto score = [&](double bpm, bool eighths) {
        static constexpr double weights[4] = {1.0, 0.6, 0.4, 0.5};
        const double lag = 60.0 * m_fps / bpm;
        double s = 0, w = 0;
        for (int k = 1; k <= 4; ++k)
            if (k * lag < maxLag) {
                s += weights[k - 1] * rf(k * lag);
                w += weights[k - 1];
            }
        if (eighths) {
            s += 0.4 * rf(lag / 2);
            w += 0.4;
        }
        return w > 0 ? s / w : 0.0;
    };
    // A preference for tempos near 120 BPM: mild between tempos, stronger between octaves.
    auto prior = [](double bpm, double amount) {
        return 1 - amount + amount * std::exp(-0.5 * std::pow(std::log2(bpm / 120.0) / 1.4, 2));
    };
    const size_t count = m_states.size();
    std::vector<double> obs(count, 0.0);
    double top = 0;
    for (size_t i = 0; i < count; ++i) {
        for (double b = m_states[i] / 2; b <= kFastest; b *= 2)
            if (b >= kSlowest)
                obs[i] = std::max(obs[i], std::max(0.0, score(b, true)) * prior(b, 0.15));
        top = std::max(top, obs[i]);
    }
    if (top <= 0)
        return;
    m_confidence = std::clamp(top / r0, 0.0, 1.0);

    // The tracker. Predict: the tempo drifts a little, or (rarely) jumps anywhere;
    // jumps to the usual mistakes (2:3, 3:4, 4:5, 3:5 and their inverses) are ten
    // times rarer still.
    const double stability = std::clamp(m_s.stability, 0.0, 1.0);
    const double jump = std::pow(10.0, -(1.5 + 4.0 * stability));
    std::vector<double> next(count, 0.0);
    static constexpr double kernel[5] = {0.05, 0.2, 0.5, 0.2, 0.05};
    for (size_t i = 0; i < count; ++i)
        for (int d = -2; d <= 2; ++d)
            next[(i + count + size_t(d + 2) - 2) % count] += m_belief[i] * kernel[d + 2];
    std::vector<double> landing(count, 1.0);
    if (m_bpm > 0) {
        const double current = tempoClass(m_bpm);
        for (size_t i = 0; i < count; ++i)
            for (double q : {3.0 / 2, 4.0 / 3, 5.0 / 4, 5.0 / 3, 8.0 / 5, 6.0 / 5})
                if (classDistance(double(i) / double(count), current + std::log2(q)) < 0.0215) // within 1.5%
                    landing[i] = 0.1;
    }
    double landingSum = 0;
    for (double v : landing)
        landingSum += v;
    // Update: the scores as evidence. Successive windows share most of their audio,
    // so each counts for a quarter second's worth; a hazy rhythm (a breakdown with
    // only an arp or a pad) counts for less than a clear one.
    const double strength = 16.0 * (0.25 / m_s.windowSeconds) * std::clamp((m_confidence - 0.1) / 0.8, 0.0, 1.0);
    double sum = 0;
    for (size_t i = 0; i < count; ++i) {
        m_belief[i] = ((1 - jump) * next[i] + jump * landing[i] / landingSum) * std::exp(strength * obs[i] / top);
        sum += m_belief[i];
    }
    size_t best = 0;
    for (size_t i = 0; i < count; ++i) {
        m_belief[i] = std::max(m_belief[i] / sum, 1e-300);
        if (m_belief[i] > m_belief[best])
            best = i;
    }
    if (m_tempoLocked) {
        choosePhase(phaseCurve, m_onsetTime[n - window]);
        return;
    }

    // The octave: the range's, or, with more than one in it, the one whose beat is
    // clearest (with the preference for 120); the current one stays unless another
    // is clearly better. With none in the range, the nearest to it.
    std::vector<double> octaves;
    for (double b = m_states[best] / 8; b <= 400; b *= 2)
        if (b >= m_s.minBpm * 0.99 && b <= m_s.maxBpm * 1.01)
            octaves.push_back(b);
    double chosen = 0;
    if (m_octavePinned && m_bpm > 0) { // x2 or /2 was used: the octave nearest the tempo shown
        double nearest = 1e9;
        for (double b = m_states[best] / 8; b <= 400; b *= 2)
            if (const double d = std::abs(std::log(b / m_bpm)); d < nearest) {
                nearest = d;
                chosen = b;
            }
    } else if (octaves.empty()) {
        double nearest = 1e9;
        for (double b = m_states[best] / 8; b <= 400; b *= 2) {
            const double d = b < m_s.minBpm ? std::log(m_s.minBpm / b) : std::log(b / m_s.maxBpm);
            if (d < nearest) {
                nearest = d;
                chosen = b;
            }
        }
    } else {
        auto clarity = [&](double b) { return std::max(0.0, score(b, false)) * prior(b, 0.3); };
        chosen = octaves.front();
        for (double b : octaves)
            if (clarity(b) > clarity(chosen))
                chosen = b;
        if (m_bpm > 0)
            for (double b : octaves)
                if (std::abs(b / m_bpm - 1) < 0.04 && clarity(chosen) < 1.15 * clarity(b))
                    chosen = b;
    }
    const double estimate = refineTempo(o, chosen);
    if (m_bpm <= 0 || std::abs(estimate / m_bpm - 1) >= 0.04) {
        m_bpm = estimate; // found, or the tracker moved to another tempo (the grid follows below)
    } else {
        m_bpm += (estimate - m_bpm) * (1.0 - 0.9 * stability);
    }
    choosePhase(phaseCurve, m_onsetTime[n - window]);
}

void BeatTracker::choosePhase(const std::vector<double>& curve, double start)
{
    // The beat grid starts on the first hit it hears, which may be a snare, a hi-hat or
    // an off-beat stab, and following the hits only corrects small errors. So: which
    // offset of the grid lines up best with the onsets of the last few seconds (the
    // kicks counting double)? If another one is clearly better for a couple of seconds,
    // the grid moves there. Not after Set 1: the user said where the beat is.
    if (!m_s.locked || m_bpm <= 0 || m_nextBeat <= 0 || m_phaseByHand || curve.empty())
        return;
    constexpr double delay = 0.015, ratio = 1.25; // the flux's lag behind the hits; how much better
    constexpr int votesNeeded = 8;                // two seconds of estimates in a row
    const double period = gridPeriod();
    const double t0 = start - delay; // the time of curve[0]'s onsets
    auto at = [&](double t) { // the curve near a time (+-2 frames: hits wander a little)
        const double f = (t - t0) * m_fps;
        const int i = int(std::lround(f));
        double v = 0;
        for (int d = -2; d <= 2; ++d)
            if (i + d >= 0 && i + d < int(curve.size()))
                v = std::max(v, curve[size_t(i + d)]);
        return v;
    };
    auto salience = [&](double phase) { // phase: seconds after t0
        double sum = 0;
        for (double t = t0 + phase; t < t0 + double(curve.size()) / m_fps; t += period)
            sum += at(t);
        return sum;
    };
    constexpr int kSteps = 48;
    double best = 0, bestValue = -1;
    for (int j = 0; j < kSteps; ++j) {
        const double ph = period * j / kSteps;
        if (const double v = salience(ph); v > bestValue) {
            bestValue = v;
            best = ph;
        }
    }
    const double current = std::fmod(std::fmod(m_nextBeat - t0, period) + period, period);
    double shift = best - current;
    shift -= period * std::round(shift / period);
    if (std::abs(shift) > 0.12 * period && bestValue > ratio * std::max(1e-9, salience(current))) {
        if (++m_phaseVotes >= votesNeeded) {
            m_phaseVotes = 0;
            m_nextBeat += shift;
            while (m_nextBeat < m_now)
                m_nextBeat += period;
            m_kfP = {0.02 * 0.02, 0, 0, m_kfP[3]};
        }
    } else {
        m_phaseVotes = 0;
    }
}

double BeatTracker::refineTempo(const std::vector<double>& o, double bpm) const
{
    // The tracker's states are 0.4% apart: find the autocorrelation peak near the
    // tempo, then, since the beat repeats, the peak near the furthest multiple of it
    // that fits in the window (a parabola through it and its neighbours) and divide:
    // k periods, k times finer.
    const size_t window = o.size();
    auto r = [&](int lag) {
        if (lag <= 0 || lag >= int(window))
            return 0.0;
        double s = 0;
        for (size_t i = size_t(lag); i < window; ++i)
            s += o[i] * o[i - size_t(lag)];
        return s / double(window - size_t(lag));
    };
    const double lag0 = 60.0 * m_fps / bpm;
    int best = int(std::lround(lag0));
    double bestValue = r(best);
    for (int l = int(std::floor(lag0 * 0.985)); l <= int(std::ceil(lag0 * 1.015)); ++l)
        if (const double v = r(l); v > bestValue) {
            best = l;
            bestValue = v;
        }
    const int k = std::max(1, int((window / 2) / size_t(std::max(1, best))));
    int m = k * best;
    double mv = r(m);
    for (int d = -k; d <= k; ++d)
        if (const double v = r(k * best + d); v > mv) {
            mv = v;
            m = k * best + d;
        }
    double frac = m;
    const double a = r(m - 1), c = r(m + 1), denom = a - 2 * mv + c;
    if (denom < 0)
        frac += std::clamp(0.5 * (a - c) / denom, -0.5, 0.5);
    const double refined = 60.0 * m_fps / (frac / k);
    return std::abs(refined / bpm - 1) < 0.02 ? refined : bpm;
}

void BeatTracker::setTempoLocked(bool locked, double bpm)
{
    std::lock_guard lock(m_mutex);
    m_tempoLocked = locked;
    if (locked && bpm > 0 && std::abs(bpm - m_bpm) > 1e-6) {
        m_bpm = bpm;
        if (m_kfT > 0)
            resetGrid();
    }
    if (!locked && m_bpm > 0)
        setupTempoStates(); // carry on from the locked tempo
}

bool BeatTracker::tempoLocked() const
{
    std::lock_guard lock(m_mutex);
    return m_tempoLocked;
}

double BeatTracker::scaleTempo(double factor)
{
    std::lock_guard lock(m_mutex);
    if (m_bpm <= 0 || factor <= 0)
        return m_bpm;
    m_bpm = std::clamp(m_bpm * factor, 20.0, 400.0);
    if (m_kfT > 0)
        resetGrid();
    m_octavePinned = true;
    return m_bpm;
}

bool BeatTracker::beatIsNow()
{
    std::lock_guard lock(m_mutex);
    if (m_bpm <= 0)
        return false;
    const double period = gridPeriod();
    if (m_nextBeat > 0) {
        double nearest = m_nextBeat;
        while (nearest - m_now > period / 2)
            nearest -= period;
        if (std::abs(m_now - nearest) < 0.2 * period) {
            m_phaseByHand = true;
            return false; // the grid is on it already
        }
    }
    m_nextBeat = m_now + period; // this beat was now: the next one a period on
    m_phaseByHand = true;
    return true;
}

void BeatTracker::forget()
{
    {
        std::lock_guard lock(m_mutex);
        m_tempoLocked = false;
        m_octavePinned = false;
    }
    reset();
}

void BeatTracker::emitBeat(double t)
{
    m_out.push_back(t);
    if (m_out.size() > 64)
        m_out.erase(m_out.begin());
}

std::vector<double> BeatTracker::takeBeats()
{
    std::lock_guard lock(m_mutex);
    std::vector<double> out;
    out.swap(m_out);
    return out;
}

double BeatTracker::bpm() const
{
    std::lock_guard lock(m_mutex);
    return m_bpm;
}

double BeatTracker::confidence() const
{
    std::lock_guard lock(m_mutex);
    return m_confidence;
}

BeatTracker::Trace BeatTracker::trace(int frames) const
{
    std::lock_guard lock(m_mutex);
    Trace t;
    t.frameSeconds = 1.0 / m_fps;
    const size_t n = m_onset.size(), count = std::min(n, size_t(std::max(1, frames)));
    float top = 1e-6f;
    for (size_t i = n - count; i < n; ++i)
        top = std::max({top, m_onset[i], m_threshold[i]});
    for (size_t i = n - count; i < n; ++i) {
        t.strength.push_back(m_onset[i] / top);
        t.threshold.push_back(m_threshold[i] / top);
        t.hit.push_back(m_hitFlags[i]);
        t.beat.push_back(m_beatFlags[i]);
    }
    return t;
}
