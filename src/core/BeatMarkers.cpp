#include "core/BeatMarkers.h"

#include "core/AudioMixer.h"
#include "core/TimelineOps.h"

#include <algorithm>
#include <cmath>
#include <random>

BeatScan scanBeats(const ProjectSnapshot& snap, TimeUs from, TimeUs to, const std::set<int>& onlyClips, const BeatSettings& settings,
                   const std::function<bool(double)>& progress)
{
    BeatScan out;
    if (to <= from)
        return out;
    // Just the chosen events' sound (other audio events left out), or the whole mix.
    ProjectSnapshot s = snap;
    if (!onlyClips.empty())
        for (Track& t : s.timeline.tracks)
            if (t.type == TrackType::Audio) {
                std::erase_if(t.clips, [&](const Clip& c) { return !onlyClips.count(c.id); });
                t.muted = false;
                t.solo = false;
            }
    // Nothing to hear after the last audio event; and at most kMaxScan at a time
    // (an event far out on the timeline would otherwise mean hours of mixing).
    constexpr TimeUs kMaxScan = 4 * 3600 * kUsPerSecond;
    to = std::min({to, s.timeline.soundEnd(), from + kMaxScan});
    if (to <= from)
        return out;
    constexpr int kRate = 48000, kChunk = 4800;
    AudioMixer mixer(kRate);
    BeatTracker tracker(kRate);
    BeatSettings bs = settings;
    bs.locked = true;
    bs.latencyMs = 0;
    tracker.setSettings(bs);
    std::vector<float> buf(size_t(kChunk) * 2);
    const int64_t first = int64_t(double(from) * kRate / 1e6), last = int64_t(double(to) * kRate / 1e6);
    std::vector<double> locked; // beats of the tempo grid (seconds)
    for (int64_t pos = first; pos < last; pos += kChunk) {
        const int n = int(std::min<int64_t>(kChunk, last - pos));
        std::fill(buf.begin(), buf.end(), 0.0f);
        mixer.mix(s, pos, n, buf.data());
        const bool hadTempo = tracker.bpm() > 0;
        tracker.process(buf.data(), n, double(pos) / kRate);
        for (double b : tracker.takeBeats())
            if (hadTempo) // (before the tempo is found it reports every hit)
                locked.push_back(b);
        if (progress && !progress(double(pos - first) / double(last - first)))
            return out;
    }
    out.bpm = tracker.bpm();
    out.confidence = tracker.confidence();
    if (locked.empty() || out.bpm <= 0)
        return out;
    // The tracker needs a few seconds to find the tempo: carry the grid back to the start.
    const double period = 60.0 / out.bpm, start = double(from) / 1e6;
    std::vector<double> beats;
    for (double b = locked.front() - period; b >= start - 1e-6; b -= period)
        beats.push_back(b);
    std::reverse(beats.begin(), beats.end());
    beats.insert(beats.end(), locked.begin(), locked.end());
    for (double b : beats)
        if (b >= start - 1e-6 && b < double(to) / 1e6)
            out.beats.push_back(TimeUs(std::llround(b * 1e6)));
    return out;
}

int addBeatMarkers(Timeline& tl, const std::vector<TimeUs>& beats, int every, bool replace, TimeUs from, TimeUs to)
{
    every = std::max(1, every);
    if (replace)
        std::erase_if(tl.markers, [&](const Marker& m) { return !m.isRegion() && m.time >= from && m.time < to; });
    int added = 0;
    for (size_t i = 0; i < beats.size(); i += size_t(every)) {
        Marker m;
        m.id = tl.allocId();
        m.time = beats[i];
        const bool bar = i % 4 == 0;
        if (bar)
            m.name = QString::number(i / 4 + 1); // bar numbers
        m.color = bar ? 1 : 4;
        tl.markers.push_back(m);
        ++added;
    }
    tl.sortMarkers();
    return added;
}

namespace ops {

std::vector<int> beatMontage(Timeline& tl, const std::vector<std::shared_ptr<const MediaItem>>& media, const std::vector<TimeUs>& cuts,
                             bool shuffle, uint32_t seed)
{
    std::vector<int> ids;
    std::vector<std::shared_ptr<const MediaItem>> usable;
    for (const auto& m : media)
        if (m && m->info.hasVideo())
            usable.push_back(m);
    if (usable.empty() || cuts.size() < 2)
        return ids;
    const int index = addTrack(tl, TrackType::Video, 0); // on top
    if (index < 0 || index >= int(tl.tracks.size()))
        return ids;
    Track* t = &tl.tracks[size_t(index)];
    t->name = QStringLiteral("Beat montage");
    std::mt19937 rng(seed);
    std::vector<size_t> order(usable.size());
    for (size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        if (shuffle && i % order.size() == 0)
            std::shuffle(order.begin(), order.end(), rng);
        const auto& m = usable[order[i % order.size()]];
        const TimeUs length = cuts[i + 1] - cuts[i];
        if (length <= 0)
            continue;
        Clip c;
        c.id = tl.allocId();
        c.mediaId = m->id;
        c.streamIndex = m->info.video.front().index;
        c.start = cuts[i];
        c.length = length;
        if (!m->info.isStill && m->info.duration > length) // a random part of the file
            c.sourceIn = TimeUs(std::uniform_int_distribution<int64_t>(0, m->info.duration - length)(rng));
        else if (!m->info.isStill)
            c.length = std::max<TimeUs>(1, std::min(length, m->info.duration));
        t->clips.push_back(c);
        ids.push_back(c.id);
    }
    tl.sortClips();
    return ids;
}

std::vector<int> splitAtMarkers(Timeline& tl, const std::set<int>& ids)
{
    std::vector<int> pieces;
    std::set<int> current = ids;
    for (const Marker& m : tl.markers) {
        std::set<int> hit;
        for (const Track& t : tl.tracks)
            for (const Clip& c : t.clips)
                if (current.count(c.id) && m.time > c.start && m.time < c.end())
                    hit.insert(c.id);
        if (hit.empty())
            continue;
        for (int id : splitClips(tl, hit, m.time)) { // the new right-hand pieces carry on being split
            current.insert(id);
            pieces.push_back(id);
        }
    }
    return pieces;
}

} // namespace ops
