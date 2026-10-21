#include "core/Cuts.h"

#include "core/TimelineOps.h"

#include <QJsonArray>
#include <QJsonObject>

#include <algorithm>
#include <functional>
#include <cmath>
#include <set>

namespace cuts {

std::vector<Range> silences(const std::vector<float>& mono, int rate, TimeUs start, double thresholdDb, TimeUs minLength, TimeUs keep)
{
    std::vector<Range> out;
    if (rate <= 0 || mono.empty())
        return out;
    const size_t window = std::max<size_t>(1, size_t(rate) / 50); // 20 ms
    const double threshold = std::pow(10.0, thresholdDb / 20.0);
    auto at = [&](size_t sample) { return start + TimeUs(double(sample) * kUsPerSecond / rate); };
    size_t quietFrom = 0;
    bool quiet = false;
    auto close = [&](size_t end) {
        const TimeUs a = at(quietFrom) + keep, b = at(end) - keep;
        if (at(end) - at(quietFrom) >= minLength && b > a)
            out.push_back({a, b});
    };
    for (size_t i = 0; i < mono.size(); i += window) {
        const size_t n = std::min(window, mono.size() - i);
        double sum = 0;
        for (size_t k = 0; k < n; ++k)
            sum += double(mono[i + k]) * mono[i + k];
        const bool below = std::sqrt(sum / double(n)) < threshold;
        if (below && !quiet)
            quietFrom = i;
        else if (!below && quiet)
            close(i);
        quiet = below;
    }
    if (quiet)
        close(mono.size());
    return out;
}

std::vector<Range> wordCuts(const std::vector<Caption>& words, const std::vector<bool>& removed)
{
    std::vector<Range> out;
    for (size_t i = 0; i < words.size() && i < removed.size(); ++i) {
        if (!removed[i])
            continue;
        size_t j = i;
        while (j + 1 < words.size() && j + 1 < removed.size() && removed[j + 1])
            ++j;
        const TimeUs to = j + 1 < words.size() ? words[j + 1].start : words[j].end;
        if (to > words[i].start)
            out.push_back({words[i].start, to});
        i = j;
    }
    return out;
}

namespace {
// Splits every unlocked track at `at`; each piece joins the story chapter of the event it was cut from.
void splitAt(Timeline& tl, TimeUs at)
{
    for (int piece : ops::splitClips(tl, {}, at)) {
        int ti = -1;
        const Clip* right = tl.findClip(piece, &ti);
        const auto left = std::find_if(tl.tracks[size_t(ti)].clips.begin(), tl.tracks[size_t(ti)].clips.end(),
                                       [&](const Clip& c) { return c.id != piece && c.end() == right->start; });
        if (left == tl.tracks[size_t(ti)].clips.end())
            continue;
        for (int i = 0; i < tl.chapters.size(); ++i) {
            QJsonObject chapter = tl.chapters[i].toObject();
            QJsonArray clips = chapter.value(QStringLiteral("clips")).toArray();
            if (clips.contains(left->id)) {
                clips.append(piece);
                chapter[QStringLiteral("clips")] = clips;
                tl.chapters[i] = chapter;
            }
        }
    }
}

// Every time in the edit (markers, regions, story chapters) through `moved`.
void moveTimes(Timeline& tl, const std::function<TimeUs(TimeUs)>& moved)
{
    for (Marker& m : tl.markers) {
        const TimeUs end = moved(m.end());
        m.time = moved(m.time);
        m.length = m.isRegion() ? std::max<TimeUs>(1, end - m.time) : 0;
    }
    for (int i = 0; i < tl.chapters.size(); ++i) {
        QJsonObject c = tl.chapters[i].toObject();
        const auto get = [&](const char* key) { return c.value(QLatin1String(key)).toString().toLongLong(); };
        const TimeUs start = get("start"), spoken = start + get("narration"), end = spoken + get("listening");
        c[QStringLiteral("start")] = QString::number(moved(start));
        c[QStringLiteral("narration")] = QString::number(moved(spoken) - moved(start));
        c[QStringLiteral("listening")] = QString::number(moved(end) - moved(spoken));
        tl.chapters[i] = c;
    }
    tl.sortMarkers();
}
} // namespace

TimeUs insertTime(Timeline& tl, TimeUs at, TimeUs length, const FrameRate& fps)
{
    at = std::max<TimeUs>(0, fps.snap(at));
    length = fps.snap(length);
    if (length <= 0)
        return 0;
    splitAt(tl, at);
    ops::rippleShift(tl, at, length, {}, {}, false);
    // Times from `at` on move later; a region or chapter running across it grows.
    moveTimes(tl, [&](TimeUs t) { return t < at ? t : t + length; });
    return length;
}

TimeUs removeRanges(Timeline& tl, std::vector<Range> ranges, const FrameRate& fps)
{
    for (Range& r : ranges) {
        r.from = std::max<TimeUs>(0, fps.snap(r.from));
        r.to = fps.snap(r.to);
    }
    std::erase_if(ranges, [](const Range& r) { return r.to <= r.from; });
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.from < b.from; });
    std::vector<Range> merged;
    for (const Range& r : ranges) {
        if (!merged.empty() && r.from <= merged.back().to)
            merged.back().to = std::max(merged.back().to, r.to);
        else
            merged.push_back(r);
    }
    TimeUs removed = 0;
    // Latest first, so the earlier ranges still find their events where they were.
    for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
        splitAt(tl, it->from);
        splitAt(tl, it->to);
        std::set<int> inside;
        for (const Track& t : tl.tracks)
            if (!t.locked)
                for (const Clip& c : t.clips)
                    if (c.start >= it->from && c.end() <= it->to)
                        inside.insert(c.id);
        ops::removeClips(tl, inside);
        for (int i = 0; i < tl.chapters.size(); ++i) { // (what was cut out leaves its chapter)
            QJsonObject chapter = tl.chapters[i].toObject();
            QJsonArray kept;
            for (const QJsonValue v : chapter.value(QStringLiteral("clips")).toArray())
                if (!inside.count(v.toInt()))
                    kept.append(v);
            if (chapter.contains(QStringLiteral("clips")))
                chapter[QStringLiteral("clips")] = kept;
            tl.chapters[i] = chapter;
        }
        ops::rippleShift(tl, it->to, it->from - it->to, {}, {}, false);
        // Times after the cut move back; times inside it land on its start (regions and story chapters shrink).
        moveTimes(tl, [&](TimeUs t) { return t <= it->from ? t : t < it->to ? it->from : t - (it->to - it->from); });
        removed += it->to - it->from;
    }
    return removed;
}

} // namespace cuts
