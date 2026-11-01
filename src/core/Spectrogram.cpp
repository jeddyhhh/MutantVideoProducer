#include "core/Spectrogram.h"

#include "core/AudioMixer.h"
#include "core/Fft.h"

#include <algorithm>
#include <cmath>

namespace spectrogram {

double rowOf(double hz, int rate, int height)
{
    const double top = rate / 2.0;
    return (1.0 - std::log(std::clamp(hz, kLowHz, top) / kLowHz) / std::log(top / kLowHz)) * (height - 1);
}

double hzAt(double row, int rate, int height)
{
    const double top = rate / 2.0;
    return kLowHz * std::pow(top / kLowHz, 1.0 - std::clamp(row / std::max(1, height - 1), 0.0, 1.0));
}

QImage render(const std::vector<float>& mono, int rate, QSize size)
{
    QImage img(size, QImage::Format_RGB32);
    img.fill(Qt::black);
    constexpr int kN = 4096; // (12 Hz a bin at 48 kHz: the hum's harmonics apart)
    if (mono.size() < size_t(kN) || size.isEmpty())
        return img;
    // The colours: black, violet, red, orange, yellow, white.
    static const QRgb stops[] = {0x000000, 0x2a0a4a, 0x8a1a5a, 0xd8402a, 0xf89a20, 0xfff2a0};
    auto colour = [](double v) {
        v = std::clamp(v, 0.0, 1.0) * (std::size(stops) - 1);
        const int i = std::min(int(v), int(std::size(stops)) - 2);
        const double f = v - i;
        const QColor a(stops[i]), b(stops[i + 1]);
        return qRgb(int(a.red() + (b.red() - a.red()) * f), int(a.green() + (b.green() - a.green()) * f), int(a.blue() + (b.blue() - a.blue()) * f));
    };
    std::vector<float> window(kN);
    for (int i = 0; i < kN; ++i)
        window[size_t(i)] = float(0.5 - 0.5 * std::cos(2 * 3.14159265358979 * i / (kN - 1)));
    double gain = 0; // (a full-scale sine reads 0 dB)
    for (float w : window)
        gain += w;
    gain /= 2;
    // Each row's bins: from its lower edge to its upper one (at least one).
    std::vector<std::pair<int, int>> rows(size_t(size.height()));
    for (int y = 0; y < size.height(); ++y) {
        const double lo = hzAt(y + 0.5, rate, size.height()), hi = hzAt(y - 0.5, rate, size.height());
        const int b0 = std::clamp(int(std::floor(lo * kN / rate)), 1, kN / 2), b1 = std::clamp(int(std::ceil(hi * kN / rate)), b0 + 1, kN / 2 + 1);
        rows[size_t(y)] = {b0, b1};
    }
    std::vector<std::complex<float>> buf(kN);
    std::vector<float> db(kN / 2 + 1);
    const double hop = double(mono.size() - kN) / std::max(1, size.width() - 1);
    for (int x = 0; x < size.width(); ++x) {
        const size_t at = size_t(std::llround(x * hop));
        for (int i = 0; i < kN; ++i)
            buf[size_t(i)] = {mono[at + size_t(i)] * window[size_t(i)], 0.0f};
        fftInPlace(buf);
        for (int k = 0; k <= kN / 2; ++k)
            db[size_t(k)] = float(20 * std::log10(std::max(1e-9, double(std::abs(buf[size_t(k)])) / gain)));
        for (int y = 0; y < size.height(); ++y) {
            float loud = -400;
            if (rows[size_t(y)].second - rows[size_t(y)].first <= 2) {
                // Narrower than the bins (the low notes): between them, on a parabola through the nearest three, so a
                // tone is brightest at its own row.
                const double x = hzAt(y, rate, size.height()) * kN / rate;
                const int k = std::clamp(int(std::lround(x)), 1, kN / 2 - 1);
                const double d = x - k, a = db[size_t(k - 1)], b = db[size_t(k)], c = db[size_t(k + 1)];
                loud = float(b + 0.5 * d * (c - a) + 0.5 * d * d * (a - 2 * b + c));
            } else {
                for (int k = rows[size_t(y)].first; k < rows[size_t(y)].second; ++k)
                    loud = std::max(loud, db[size_t(k)]);
            }
            img.setPixel(x, y, colour((loud - kFloorDb) / (kTopDb - kFloorDb)));
        }
    }
    return img;
}

std::vector<float> eventSound(const ProjectSnapshot& snap, int clipId, bool beforeEffects, int rate, double maxSeconds)
{
    int ti = -1;
    const Clip* found = snap.timeline.findClip(clipId, &ti);
    if (!found || isVisualTrack(snap.timeline.tracks[size_t(ti)].type))
        return {};
    Clip c = *found;
    if (beforeEffects)
        c.effects.clear();
    c.muted = false;
    Track track;
    track.id = 1;
    track.type = TrackType::Audio;
    track.clips = {c};
    ProjectSnapshot alone = snap;
    alone.timeline.tracks = {track};
    alone.timeline.audioMasterEffects.clear();
    alone.timeline.audioMasterLevel = 1.0f;
    AudioMixer mixer(rate);
    const int total = int(std::min(double(c.length) / kUsPerSecond, maxSeconds) * rate);
    const int64_t start = int64_t(std::llround(double(c.start) * rate / kUsPerSecond));
    std::vector<float> out(size_t(std::max(0, total))), chunk(4096 * 2);
    for (int i = 0; i < total; i += 4096) {
        const int n = std::min(4096, total - i);
        mixer.mix(alone, start + i, n, chunk.data());
        for (int k = 0; k < n; ++k)
            out[size_t(i + k)] = 0.5f * (chunk[size_t(k) * 2] + chunk[size_t(k) * 2 + 1]);
    }
    return out;
}

} // namespace spectrogram
