#include "core/AudioSync.h"

#include "core/AudioDecoder.h"
#include "core/Fft.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <set>

namespace audiosync {
namespace {
constexpr int kDecodeRate = 8000, kPerValue = kDecodeRate / kRate;
constexpr TimeUs kMaxLength = 20 * 60 * kUsPerSecond; // (twenty minutes of each is plenty to line them up)
} // namespace

std::vector<float> envelope(const QString& path, int stream, TimeUs sourceIn, TimeUs length)
{
    AudioDecoder dec;
    if (!dec.open(path, stream, kDecodeRate))
        return {};
    length = std::min(length, kMaxLength);
    const int64_t first = sourceIn * kDecodeRate / kUsPerSecond, count = length * kDecodeRate / kUsPerSecond;
    std::vector<float> level(size_t(count / kPerValue), 0.f);
    std::vector<float> buf(size_t(kDecodeRate) * 2);
    for (int64_t done = 0; done < count; done += kDecodeRate) {
        const int n = int(std::min<int64_t>(kDecodeRate, count - done));
        dec.read(first + done, n, buf.data());
        for (int i = 0; i < n; ++i) {
            const size_t v = size_t((done + i) / kPerValue);
            if (v < level.size())
                level[v] += std::abs(buf[size_t(i) * 2]) + std::abs(buf[size_t(i) * 2 + 1]);
        }
    }
    // Onsets: how much louder than a moment ago (claps, hits, the start of words line up; a steady hum doesn't).
    std::vector<float> out(level.size(), 0.f);
    float slow = 0;
    for (size_t i = 0; i < level.size(); ++i) {
        const float v = std::log1p(level[i] * 50.f);
        out[i] = std::max(0.f, v - slow);
        slow += (v - slow) * 0.15f;
    }
    return out;
}

Match correlate(const std::vector<float>& a, const std::vector<float>& b)
{
    Match m;
    if (a.size() < size_t(kRate) || b.size() < size_t(kRate)) // (a second of sound at least)
        return m;
    size_t n = 1;
    while (n < a.size() + b.size())
        n <<= 1;
    auto spectrum = [n](const std::vector<float>& v) {
        double mean = 0;
        for (float x : v)
            mean += x;
        mean /= double(v.size());
        std::vector<std::complex<float>> s(n);
        for (size_t i = 0; i < v.size(); ++i)
            s[i] = float(v[i] - mean);
        fftInPlace(s);
        return s;
    };
    auto norm = [](const std::vector<float>& v) {
        double mean = 0, sq = 0;
        for (float x : v)
            mean += x;
        mean /= double(v.size());
        for (float x : v)
            sq += (x - mean) * (x - mean);
        return std::sqrt(sq);
    };
    std::vector<std::complex<float>> A = spectrum(a), B = spectrum(b);
    // corr[k] = sum a[i] b[i + k]: the inverse FFT of conj(A) B, through the forward one (conj in, conj out).
    for (size_t i = 0; i < n; ++i)
        A[i] = std::conj(std::conj(A[i]) * B[i]);
    fftInPlace(A);
    const double scale = 1.0 / (double(n) * std::max(1e-9, norm(a) * norm(b)));
    std::vector<double> corr(n);
    for (size_t i = 0; i < n; ++i)
        corr[i] = double(A[i].real()) * scale; // (conj of a real value: itself)
    size_t best = 0;
    for (size_t i = 1; i < n; ++i)
        if (corr[i] > corr[best])
            best = i;
    // The peak against the rest: a real match stands far out of the noise.
    double mean = 0, sq = 0;
    for (double c : corr)
        mean += c;
    mean /= double(n);
    for (double c : corr)
        sq += (c - mean) * (c - mean);
    const double sd = std::sqrt(sq / double(n));
    const long long k = best < n / 2 ? (long long)best : (long long)best - (long long)n;
    // (a sub-millisecond peak from its neighbours)
    double frac = 0;
    const double y0 = corr[(best + n - 1) % n], y1 = corr[best], y2 = corr[(best + 1) % n];
    if (const double d = y0 - 2 * y1 + y2; d < 0)
        frac = std::clamp(0.5 * (y0 - y2) / d, -0.5, 0.5);
    m.lag = (double(k) + frac) / kRate;
    m.score = std::clamp(y1, 0.0, 1.0);
    // ...and against the next best match more than a quarter of a second away: a sound with one onset (a steady
    // tone starting) matches every loud moment of the other about as well, a real match only one.
    double second = 0;
    const size_t apart = size_t(kRate / 4);
    for (size_t i = 0; i < n; ++i) {
        const size_t d = i > best ? i - best : best - i;
        if (std::min(d, n - d) > apart)
            second = std::max(second, corr[i]);
    }
    m.ok = y1 > 0.3 && sd > 0 && (y1 - mean) / sd > 8 && y1 > 1.4 * second;
    return m;
}

Result syncByAudio(Timeline& tl, const MediaMap& media, const std::vector<int>& clipIds)
{
    Result r;
    // The groups of the events chosen (an event alone is its own group).
    struct Member {
        int track;
        Clip* clip;
    };
    std::map<int, std::vector<Member>> groups; // key: group id, or -event id for an ungrouped event
    const std::set<int> chosen(clipIds.begin(), clipIds.end());
    for (int ti = 0; ti < int(tl.tracks.size()); ++ti)
        for (Clip& c : tl.tracks[size_t(ti)].clips)
            if (chosen.count(c.id) || (c.groupId && std::any_of(clipIds.begin(), clipIds.end(), [&](int id) {
                    const Clip* o = tl.findClip(id);
                    return o && o->groupId == c.groupId;
                })))
                groups[c.groupId ? c.groupId : -c.id].push_back({ti, &c});
    if (groups.size() < 2) {
        r.error = QStringLiteral("Select two or more events with sound (a recording, and the others to line up with it).");
        return r;
    }
    // The sound each group is matched by: an audio event's, else a video event's own.
    struct Sound {
        const MediaItem* item = nullptr;
        int stream = -1;
        const Clip* clip = nullptr;
    };
    auto soundOf = [&](const std::vector<Member>& g) {
        Sound s;
        for (int pass = 0; pass < 2 && !s.item; ++pass)
            for (const Member& m : g) {
                const auto it = media.find(m.clip->mediaId);
                if (it == media.end() || !it->second || it->second->info.audio.empty() || m.clip->isGenerated())
                    continue;
                const bool audioTrack = tl.tracks[size_t(m.track)].type == TrackType::Audio;
                if (pass == 0 && !audioTrack)
                    continue;
                const bool streamIsAudio = std::any_of(it->second->info.audio.begin(), it->second->info.audio.end(),
                                                       [&](const AudioStreamInfo& a) { return a.index == m.clip->streamIndex; });
                s = {it->second.get(), streamIsAudio ? m.clip->streamIndex : it->second->info.audio.front().index, m.clip};
                break;
            }
        return s;
    };
    // The reference: the chosen event on the topmost track (the earliest on a tie).
    const std::vector<Member>* ref = nullptr;
    std::pair<int, TimeUs> refKey{1 << 30, 0};
    for (const auto& [key, g] : groups)
        for (const Member& m : g)
            if (chosen.count(m.clip->id) && std::make_pair(m.track, m.clip->start) < refKey) {
                refKey = {m.track, m.clip->start};
                ref = &g;
            }
    const Sound refSound = soundOf(*ref);
    if (!refSound.item) {
        r.error = QStringLiteral("The event on the top track has no sound to line the others up with.");
        return r;
    }
    const std::vector<float> a = envelope(refSound.item->path, refSound.stream, refSound.clip->sourceIn, refSound.clip->length);
    for (auto& [key, g] : groups) {
        if (&g == ref)
            continue;
        const Sound s = soundOf(g);
        const bool plain = s.clip && s.clip->speed == 1.0 && !s.clip->reverse && !s.clip->hasRamp();
        const Match m = plain ? correlate(a, envelope(s.item->path, s.stream, s.clip->sourceIn, s.clip->length)) : Match{};
        const TimeUs delta = m.ok ? refSound.clip->start - s.clip->start - TimeUs(std::llround(m.lag * kUsPerSecond)) : 0;
        const bool fits = m.ok && std::all_of(g.begin(), g.end(), [&](const Member& member) { return member.clip->start + delta >= 0; });
        if (!fits) {
            r.unmatched.push_back(g.front().clip->id);
            continue;
        }
        for (Member& member : g)
            member.clip->start += delta;
        r.moved[g.front().clip->id] = delta;
    }
    tl.sortClips();
    return r;
}

} // namespace audiosync
