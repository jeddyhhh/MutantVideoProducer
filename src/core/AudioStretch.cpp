#include "core/AudioStretch.h"

#include <algorithm>
#include <cmath>
#include <cstring>

AudioStretch::AudioStretch() : m_window(kFrame)
{
    // Periodic Hann: overlapping halves sum to exactly 1.
    for (int i = 0; i < kFrame; ++i)
        m_window[size_t(i)] = float(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * i / kFrame));
}

void AudioStretch::reset(int64_t t)
{
    // The first grain starts half a grain early so `t` is fully covered.
    m_ola.clear();
    m_olaStart = t - kHop;
    m_nextGrain = t - kHop;
    m_nextOut = t;
    m_valid = true;
    m_prevValid = false;
}

void AudioStretch::render(int64_t a, int n, float* out, const Position& position, const Fetch& fetch)
{
    if (!continuesAt(a))
        reset(a);
    // A sample is final once every grain starting at or before it is added.
    while (m_nextGrain < a + n) {
        addGrain(m_nextGrain, position, fetch);
        m_nextGrain += kHop;
    }
    const size_t off = size_t(a - m_olaStart) * 2, count = size_t(n) * 2;
    if (m_ola.size() < off + count)
        m_ola.resize(off + count, 0.0f);
    std::memcpy(out, m_ola.data() + off, sizeof(float) * count);
    m_ola.erase(m_ola.begin(), m_ola.begin() + std::ptrdiff_t(off + count));
    m_olaStart = a + n;
    m_nextOut = a + n;
}

void AudioStretch::readGrain(int64_t pos, int dir, int count, float* dst, const Fetch& fetch)
{
    // Forwards: [pos, pos + count). Backwards: pos, pos - 1, ... (reversed).
    int64_t from = dir > 0 ? pos : pos - count + 1;
    std::memset(dst, 0, sizeof(float) * 2 * size_t(count));
    int skip = 0;
    if (from < 0) { // before the start of the media: silence
        skip = int(std::min<int64_t>(count, -from));
        from = 0;
    }
    if (skip < count)
        fetch(from, count - skip, dst + size_t(skip) * 2);
    if (dir < 0)
        for (int i = 0, j = count - 1; i < j; ++i, --j) {
            std::swap(dst[size_t(i) * 2], dst[size_t(j) * 2]);
            std::swap(dst[size_t(i) * 2 + 1], dst[size_t(j) * 2 + 1]);
        }
}

void AudioStretch::addGrain(int64_t start, const Position& position, const Fetch& fetch)
{
    const size_t off = size_t(start - m_olaStart) * 2;
    if (m_ola.size() < off + size_t(kFrame) * 2)
        m_ola.resize(off + size_t(kFrame) * 2, 0.0f);

    const int64_t center = start + kHop;
    const double rate = (position(center + kHop / 2) - position(center - kHop / 2)) / kHop;
    if (std::abs(rate) < kMinRate) {
        m_prevValid = false; // frozen: the windows fade out and back in
        return;
    }
    const int dir = rate >= 0 ? 1 : -1;
    // Grain start that puts its centre on the curve.
    int64_t pos = std::llround(position(center)) - int64_t(dir) * kHop;

    if (m_prevValid && dir == m_prevDir) {
        // Line the grain up with how the previous one would have continued
        // over the overlap: search +-kSeek for the best normalised correlation.
        m_ref.resize(size_t(kHop) * 2);
        readGrain(m_prevPos + int64_t(dir) * kHop, dir, kHop, m_ref.data(), fetch);
        const int spanLen = kHop + 2 * kSeek;
        m_span.resize(size_t(spanLen) * 2);
        // span[j] is source position (pos - dir * kSeek) + dir * j; candidate
        // shift e (-kSeek .. kSeek) starts at span index e + kSeek.
        readGrain(pos - int64_t(dir) * kSeek, dir, spanLen, m_span.data(), fetch);
        std::vector<float>& ref = m_refMono;
        std::vector<float>& span = m_spanMono;
        ref.resize(size_t(kHop));
        span.resize(size_t(spanLen));
        for (int i = 0; i < kHop; ++i)
            ref[size_t(i)] = m_ref[size_t(i) * 2] + m_ref[size_t(i) * 2 + 1];
        for (int j = 0; j < spanLen; ++j)
            span[size_t(j)] = m_span[size_t(j) * 2] + m_span[size_t(j) * 2 + 1];
        auto score = [&](int e, int step) {
            double xy = 0, yy = 1e-9;
            const float* y = span.data() + (e + kSeek);
            for (int i = 0; i < kHop; i += step) {
                xy += double(ref[size_t(i)]) * y[i];
                yy += double(y[i]) * y[i];
            }
            return xy / std::sqrt(yy);
        };
        int best = 0;
        double bestScore = score(0, 2);
        for (int e = -kSeek; e <= kSeek; e += 4) {
            const double s = score(e, 2);
            if (s > bestScore) {
                bestScore = s;
                best = e;
            }
        }
        const int coarse = best;
        bestScore = score(coarse, 1);
        for (int e = std::max(-kSeek, coarse - 3); e <= std::min(kSeek, coarse + 3); ++e) {
            const double s = score(e, 1);
            if (s > bestScore) {
                bestScore = s;
                best = e;
            }
        }
        pos += int64_t(dir) * best;
    }

    m_grain.resize(size_t(kFrame) * 2);
    readGrain(pos, dir, kFrame, m_grain.data(), fetch);
    float* dst = m_ola.data() + off;
    for (int i = 0; i < kFrame; ++i) {
        const float w = m_window[size_t(i)];
        dst[i * 2] += m_grain[size_t(i) * 2] * w;
        dst[i * 2 + 1] += m_grain[size_t(i) * 2 + 1] * w;
    }
    m_prevPos = pos;
    m_prevDir = dir;
    m_prevValid = true;
}
