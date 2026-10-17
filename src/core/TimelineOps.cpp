#include "core/TimelineOps.h"

#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace ops {

TimeUs maxClipLength(const MediaItem* media, TimeUs sourceIn, double speed)
{
    if (!media || media->info.isStill)
        return std::numeric_limits<TimeUs>::max() / 4;
    return TimeUs(double(std::max<TimeUs>(0, media->info.duration - sourceIn)) / std::max(speed, 1e-6));
}

TimeUs maxClipLength(const MediaItem* media, const Clip& c)
{
    MediaItem nested;
    if(c.isSequence()) { nested.info.duration=c.sequence.value->duration(); media=&nested; }
    if (!c.hasRamp())
        return maxClipLength(media, c.sourceIn, c.speed);
    if (!media || media->info.isStill)
        return std::numeric_limits<TimeUs>::max() / 4;
    // Reversed events use the same span, just backwards.
    const double avail = double(std::max<TimeUs>(0, media->info.duration - c.sourceIn));
    TimeUs hi = std::max<TimeUs>(c.length, kUsPerSecond);
    while (c.rampOffset(double(hi)) <= avail) {
        if (hi > TimeUs(1e12))
            return std::numeric_limits<TimeUs>::max() / 4; // the ramp ends frozen or backwards
        hi *= 2;
    }
    return rampTimeForOffset(c, avail, 0, hi);
}

void setClipSpeed(Clip& c, const MediaItem* media, double speed, bool keepSpan, const FrameRate& fps)
{
    speed = std::clamp(speed, kMinSpeed, kMaxSpeed);
    TimeUs length = c.length;
    if (c.hasRamp()) {
        // The ramp keeps its shape: with keepSpan it stretches with the event.
        if (keepSpan) {
            const double factor = c.speed / speed;
            scaleRampTime(c, factor);
            length = TimeUs(double(c.length) * factor);
        }
        c.speed = speed;
        length = std::min(length, maxClipLength(media, c));
    } else {
        const TimeUs span = c.sourceSpan();
        if (keepSpan)
            length = TimeUs(double(span) / speed);
        c.speed = speed;
        length = std::min(length, maxClipLength(media, c));
    }
    c.length = std::max(fps.frameDuration(), fps.snap(length));
    clampFades(c);
    trimRampPoints(c);
}

void clampFades(Clip& c)
{
    c.fadeIn = std::clamp<TimeUs>(c.fadeIn, 0, std::max<TimeUs>(0, c.length));
    c.fadeOut = std::clamp<TimeUs>(c.fadeOut, 0, std::max<TimeUs>(0, c.length - c.fadeIn));
}

void trimClipStart(Clip& c, TimeUs d)
{
    // Reversed events show the end of their span at the start: sourceIn stays.
    if (!c.reverse)
        c.sourceIn = std::max<TimeUs>(0, c.sourceIn + (c.hasRamp() ? TimeUs(std::llround(c.rampOffset(double(d)))) : TimeUs(double(d) * c.speed)));
    c.start += d;
    c.length -= d;
    clampFades(c);
    shiftClipKeyframes(c, -d); // keyframes (and the ramp) stay with the picture
}

void trimClipEnd(Clip& c, TimeUs d)
{
    if (c.reverse) {
        const TimeUs used = c.hasRamp() ? TimeUs(std::llround(c.rampOffset(double(c.length + d)) - c.rampOffset(double(c.length))))
                                        : TimeUs(double(d) * c.speed);
        c.sourceIn = std::max<TimeUs>(0, c.sourceIn - used);
    }
    c.length += d;
    clampFades(c);
}

std::pair<TimeUs, TimeUs> trimRange(const Clip& c, const MediaItem* media, bool startEdge)
{
    constexpr TimeUs kBig = std::numeric_limits<TimeUs>::max() / 4;
    MediaItem nested;
    if(c.isSequence()) { nested.info.duration=c.sequence.value->duration(); media=&nested; }
    if (!media || media->info.isStill || c.isGenerated())
        return {-kBig, kBig};
    const double s = c.speed;
    if (!c.hasRamp()) {
        if (startEdge)
            return {!c.reverse ? TimeUs(-double(c.sourceIn) / s) : c.length - maxClipLength(media, c.sourceIn, s), kBig};
        return {-kBig, !c.reverse ? maxClipLength(media, c.sourceIn, s) - c.length : TimeUs(double(c.sourceIn) / s)};
    }
    // Earliest event-local time whose media offset reaches `target`.
    auto earliest = [&](double target) {
        const TimeUs x = rampTimeForOffset(c, target, -TimeUs(1e12), 0);
        return c.rampOffset(double(x)) >= target ? x : x + 1;
    };
    const double avail = double(std::max<TimeUs>(0, media->info.duration - c.sourceIn));
    if (startEdge)
        return {earliest(!c.reverse ? -double(c.sourceIn) : double(c.sourceSpan()) - avail), kBig};
    if (!c.reverse)
        return {-kBig, maxClipLength(media, c) - c.length};
    const double len = double(c.length);
    return {-kBig, rampTimeForOffset(c, c.rampOffset(len) + double(c.sourceIn), c.length, TimeUs(1e12)) - c.length};
}

TimeUs rampTimeForOffset(const Clip& c, double offset, TimeUs lo, TimeUs hi)
{
    if (c.rampOffset(double(hi)) <= offset)
        return hi;
    while (hi - lo > 1) {
        const TimeUs mid = lo + (hi - lo) / 2;
        (c.rampOffset(double(mid)) <= offset ? lo : hi) = mid;
    }
    return lo;
}

void scaleRampTime(Clip& c, double factor)
{
    for (auto& k : c.velocity.points)
        k.time = TimeUs(std::llround(double(k.time) * factor));
}

void trimRampPoints(Clip& c)
{
    auto& pts = c.velocity.points;
    if (pts.empty())
        return;
    // Keep one point at or before 0 and one at or after the end.
    size_t first = 0;
    while (first + 1 < pts.size() && pts[first + 1].time <= 0)
        ++first;
    size_t last = pts.size() - 1;
    while (last > first && pts[last - 1].time >= c.length)
        --last;
    pts = std::vector<Keyframe>(pts.begin() + std::ptrdiff_t(first), pts.begin() + std::ptrdiff_t(last) + 1);
}

QString rampPresetName(RampPreset p)
{
    switch (p) {
    case RampPreset::SpeedUp: return QObject::tr("Speed Up (100% to 300%)");
    case RampPreset::SlowDown: return QObject::tr("Slow Down (100% to 25%)");
    case RampPreset::SlowMiddle: return QObject::tr("Slow Motion in the Middle");
    case RampPreset::FastMiddle: return QObject::tr("Fast in the Middle");
    case RampPreset::FreezeMiddle: return QObject::tr("Freeze in the Middle");
    }
    return {};
}

void applyRampPreset(Clip& c, const MediaItem* media, RampPreset preset, const FrameRate& fps)
{
    // Shapes over a unit length (time fraction, velocity), smooth curves.
    std::vector<std::pair<double, double>> shape;
    switch (preset) {
    case RampPreset::SpeedUp: shape = {{0.0, 1.0}, {1.0, 3.0}}; break;
    case RampPreset::SlowDown: shape = {{0.0, 1.0}, {1.0, 0.25}}; break;
    case RampPreset::SlowMiddle: shape = {{0.0, 1.0}, {0.3, 0.25}, {0.7, 0.25}, {1.0, 1.0}}; break;
    case RampPreset::FastMiddle: shape = {{0.0, 1.0}, {0.3, 4.0}, {0.7, 4.0}, {1.0, 1.0}}; break;
    case RampPreset::FreezeMiddle: shape = {{0.0, 1.0}, {0.4, 1.0}, {0.4, 0.0}, {0.6, 0.0}, {0.6, 1.0}, {1.0, 1.0}}; break;
    }
    const double span = double(c.sourceSpan());
    Clip probe = c;
    probe.velocity.active = true;
    probe.velocity.points.clear();
    for (size_t i = 0; i < shape.size(); ++i) {
        Keyframe k;
        k.time = TimeUs(shape[i].first * kUsPerSecond);
        // A point at the same time as the next one is a jump (freeze): no ramp into it.
        if (i > 0 && shape[i].first == shape[i - 1].first)
            k.time += 1;
        k.value = shape[i].second;
        k.interp = preset == RampPreset::FreezeMiddle ? Keyframe::Interp::Linear : Keyframe::Interp::Smooth;
        probe.velocity.points.push_back(k);
    }
    // Media used per second of event with this shape -> length that keeps the footage.
    const double perSecond = probe.rampOffset(double(kUsPerSecond));
    if (perSecond <= 0)
        return;
    const double length = std::max(double(fps.frameDuration()), span / perSecond * kUsPerSecond);
    scaleRampTime(probe, length / kUsPerSecond);
    probe.length = std::min(TimeUs(length), maxClipLength(media, probe));
    probe.length = std::max(fps.frameDuration(), fps.snap(probe.length));
    clampFades(probe);
    c = probe;
}

void syncGroupRamps(Timeline& tl, int sourceClipId)
{
    const Clip* src = tl.findClip(sourceClipId);
    if (!src || !src->groupId)
        return;
    const Clip copy = *src;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips)
            if (c.id != copy.id && c.groupId == copy.groupId && c.start == copy.start && !c.isGenerated()) {
                c.velocity = copy.velocity;
                c.speed = copy.speed;
                c.length = copy.length;
                clampFades(c);
            }
    tl.sortClips();
}

int addTrack(Timeline& tl, TrackType type, int index)
{
    int count = 0;
    for (const auto& t : tl.tracks)
        if (t.type == type)
            ++count;
    Track t;
    t.id = tl.allocId();
    t.type = type;
    const QString label = type == TrackType::Image ? QStringLiteral("Image %1")
                        : type == TrackType::Video ? QStringLiteral("Video %1") : QStringLiteral("Audio %1");
    int number = count + 1;
    while (std::any_of(tl.tracks.begin(), tl.tracks.end(), [&](const Track& existing) { return existing.name == label.arg(number); }))
        ++number;
    t.name = label.arg(number);
    if (index < 0 || index > int(tl.tracks.size()))
        index = int(tl.tracks.size());
    tl.tracks.insert(tl.tracks.begin() + index, std::move(t));
    return index;
}

void removeTrack(Timeline& tl, int index)
{
    if (index >= 0 && index < int(tl.tracks.size()))
        tl.tracks.erase(tl.tracks.begin() + index);
}

void moveTrack(Timeline& tl, int from, int to)
{
    const int n = int(tl.tracks.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    Track t = std::move(tl.tracks[size_t(from)]);
    tl.tracks.erase(tl.tracks.begin() + from);
    tl.tracks.insert(tl.tracks.begin() + to, std::move(t));
}

std::vector<int> insertMedia(Timeline& tl, const MediaItem& media, TimeUs at, int preferredTrack, const FrameRate& fps, bool newTracks)
{
    std::vector<int> created;
    const MediaInfo& info = media.info;
    const bool wantVideo = info.hasVideo();
    const bool wantAudio = info.hasAudio();
    const TrackType pictureType = info.isStill ? TrackType::Image : TrackType::Video;
    if (!wantVideo && !wantAudio)
        return created;
    if (preferredTrack >= int(tl.tracks.size()))
        preferredTrack = -1;

    QString clipTrackName;
    if (newTracks && pictureType != TrackType::Image) {
        // One number per imported clip, shared by its picture and sound.
        // Continue after named pairs, including ones saved in an older project.
        static const QRegularExpression namePattern(QStringLiteral("^Clip([1-9][0-9]*)-(?:Video|Audio)$"));
        qint64 number = 1;
        int occupiedVideoTracks = 0;
        for (const Track& track : tl.tracks) {
            if (track.type == TrackType::Video && !track.clips.empty())
                ++occupiedVideoTracks;
            const auto match = namePattern.match(track.name);
            if (match.hasMatch()) {
                bool ok = false;
                const qint64 previous = match.captured(1).toLongLong(&ok);
                if (ok && previous < std::numeric_limits<qint64>::max())
                    number = std::max(number, previous + 1);
            }
        }
        number = std::max(number, qint64(occupiedVideoTracks) + 1);
        clipTrackName = QStringLiteral("Clip%1").arg(number);
    }

    auto firstOfType = [&](TrackType type, int from) {
        for (int i = std::max(from, 0); i < int(tl.tracks.size()); ++i)
            if (tl.tracks[i].type == type)
                return i;
        return -1;
    };

    int vTrack = -1, aTrack = -1;
    if (wantVideo) {
        // A still may go on any picture track asked for (as a drop or paste can put it), not a new Image track.
        if (!newTracks && preferredTrack >= 0 && sameTrackEffectKind(tl.tracks[preferredTrack].type, pictureType))
            vTrack = preferredTrack;
        else if (!newTracks)
            vTrack = firstOfType(pictureType, 0);
        if (vTrack < 0) {
            vTrack = addTrack(tl, pictureType, 0);
            if (preferredTrack >= 0)
                ++preferredTrack;
        }
    }
    if (wantAudio) {
        if (!newTracks && preferredTrack >= 0 && tl.tracks[preferredTrack].type == TrackType::Audio)
            aTrack = preferredTrack;
        else if (!newTracks) {
            aTrack = firstOfType(TrackType::Audio, vTrack + 1);
            if (aTrack < 0)
                aTrack = firstOfType(TrackType::Audio, 0);
        }
        if (aTrack < 0)
            aTrack = addTrack(tl, TrackType::Audio, newTracks && vTrack >= 0 ? vTrack + 1 : -1);
    }
    if (newTracks && pictureType != TrackType::Image) {
        if (vTrack >= 0)
            tl.tracks[vTrack].name = clipTrackName + QStringLiteral("-Video");
        if (aTrack >= 0)
            tl.tracks[aTrack].name = clipTrackName + QStringLiteral("-Audio");
    }

    at = std::max<TimeUs>(0, fps.snap(at));
    // The part marked in Project Media, else all of it.
    const bool marked = !info.isStill && media.markOut > media.markIn;
    TimeUs length = info.isStill ? kDefaultStillDuration : (marked ? media.markOut - media.markIn : info.duration);
    length = std::max(fps.timeOf(std::max<int64_t>(1, fps.frameAt(length))), fps.frameDuration());
    const int group = (wantVideo && wantAudio) ? tl.allocId() : 0;

    auto makeClip = [&](int stream) {
        Clip c;
        c.id = tl.allocId();
        c.mediaId = media.id;
        c.streamIndex = stream;
        c.start = at;
        c.length = length;
        c.sourceIn = marked ? media.markIn : 0;
        c.groupId = group;
        c.name = media.displayName();
        return c;
    };
    if (wantVideo) {
        Clip c = makeClip(info.video.front().index);
        created.push_back(c.id);
        tl.tracks[vTrack].clips.push_back(c);
    }
    if (wantAudio) {
        Clip c = makeClip(info.audio.front().index);
        created.push_back(c.id);
        tl.tracks[aTrack].clips.push_back(c);
    }
    tl.sortClips();
    return created;
}

std::vector<int> splitClips(Timeline& tl, const std::set<int>& ids, TimeUs t)
{
    std::vector<int> created;
    std::map<int, int> newGroups;
    for (auto& track : tl.tracks) {
        const size_t n = track.clips.size();
        for (size_t i = 0; i < n; ++i) {
            Clip& c = track.clips[i];
            if (t <= c.start || t >= c.end())
                continue;
            if (ids.empty() ? track.locked : !ids.count(c.id))
                continue;
            Clip right = c;
            right.id = tl.allocId();
            right.start = t;
            if (c.hasRamp()) {
                // Media used up to the cut, along the ramp.
                const double span = double(c.sourceSpan());
                const double cut = std::clamp(c.rampOffset(double(t - c.start)), 0.0, span);
                if (c.reverse) {
                    right.sourceIn = c.sourceIn;
                    c.sourceIn = c.sourceIn + TimeUs(span - cut);
                } else {
                    right.sourceIn = c.sourceIn + TimeUs(cut);
                }
            } else if (c.reverse) {
                // Reversed: the left piece shows the end of the source span.
                right.sourceIn = c.sourceIn;
                c.sourceIn = c.sourceIn + TimeUs(double(c.end() - t) * c.speed);
            } else {
                right.sourceIn = c.sourceIn + TimeUs(double(t - c.start) * c.speed);
            }
            right.length = c.end() - t;
            right.fadeIn = 0;
            right.fadeOut = std::min(c.fadeOut, right.length);
            right.transitionIn = {};
            shiftClipKeyframes(right, -(t - c.start));
            reassignEffectIds(right.effects, [&] { return tl.allocId(); }); // effect ids are unique (runtime state key)
            if (c.groupId) {
                auto it = newGroups.find(c.groupId);
                if (it == newGroups.end())
                    it = newGroups.emplace(c.groupId, tl.allocId()).first;
                right.groupId = it->second;
            }
            c.length = t - c.start;
            c.fadeOut = 0;
            c.transitionOut = {};
            c.fadeIn = std::min(c.fadeIn, c.length);
            trimRampPoints(c);
            trimRampPoints(right);
            created.push_back(right.id);
            track.clips.push_back(right); // may reallocate: `c` is not used after this
        }
    }
    tl.sortClips();
    return created;
}

void shiftClipKeyframes(Clip& c, TimeUs delta)
{
    shiftKeyframes(c.effects, delta);
    shiftKeyframes(c.generator.params, delta);
    for (auto& k : c.velocity.points)
        k.time += delta;
}

int insertGenerated(Timeline& tl, const GeneratorPreset& preset, TimeUs at, int preferredTrack, const FrameRate& fps)
{
    int track = -1;
    if (preferredTrack >= 0 && preferredTrack < int(tl.tracks.size()) && tl.tracks[preferredTrack].type == TrackType::Video)
        track = preferredTrack;
    for (int i = 0; i < int(tl.tracks.size()) && track < 0; ++i)
        if (tl.tracks[i].type == TrackType::Video)
            track = i;
    if (track < 0)
        track = addTrack(tl, TrackType::Video, 0);
    Clip c;
    c.id = tl.allocId();
    c.start = std::max<TimeUs>(0, fps.snap(at));
    c.length = std::max(fps.snap(preset.length), fps.frameDuration());
    // Not inside another event (that would crossfade over all of it): on a free track above.
    auto inside = [&](int ti) {
        for (const Clip& x : tl.tracks[size_t(ti)].clips)
            if (x.start <= c.start && x.end() >= c.end())
                return true;
        return false;
    };
    auto free = [&](int ti) {
        for (const Clip& x : tl.tracks[size_t(ti)].clips)
            if (x.start < c.end() && x.end() > c.start)
                return false;
        return true;
    };
    if (inside(track)) {
        int above = -1;
        for (int i = track - 1; i >= 0 && above < 0; --i)
            if (tl.tracks[size_t(i)].type == TrackType::Video && free(i))
                above = i;
        track = above >= 0 ? above : addTrack(tl, TrackType::Video, track);
    }
    c.generator = makeGenerator(preset);
    c.name = preset.name;
    tl.tracks[track].clips.push_back(c);
    tl.sortClips();
    return c.id;
}

void removeClips(Timeline& tl, const std::set<int>& ids)
{
    for (auto& track : tl.tracks)
        track.clips.erase(std::remove_if(track.clips.begin(), track.clips.end(),
                                         [&](const Clip& c) { return ids.count(c.id) > 0; }),
                          track.clips.end());
}

bool overwriteContained(Timeline& tl, int clipId)
{
    int ti = -1;
    const Clip* x = tl.findClip(clipId, &ti);
    if (!x)
        return false;
    const TimeUs a = x->start, b = x->end();
    std::vector<int> around;
    for (const Clip& p : tl.tracks[size_t(ti)].clips)
        if (p.id != clipId && p.start <= a && p.end() >= b && (p.start < a || p.end() > b))
            around.push_back(p.id);
    for (int pid : around) {
        if (const Clip* p = tl.findClip(pid); p && p->end() > b)
            splitClips(tl, {pid}, b); // the part after it: an event of its own
        Clip* p = tl.findClip(pid);
        if (!p)
            continue;
        if (p->start < a) {
            trimClipEnd(*p, a - p->end()); // the part before ends where it starts
            p->fadeIn = std::min(p->fadeIn, p->length);
            p->fadeOut = 0;
            p->transitionOut = {};
        } else {
            removeClips(tl, {pid}); // it started together with it: nothing before
        }
    }
    if (Clip* c = tl.findClip(clipId)) { // no crossfade left to have
        c->fadeIn = 0;
        c->fadeOut = 0;
        c->transitionIn = {};
        c->transitionOut = {};
    }
    tl.sortClips();
    return !around.empty();
}

void overwriteOverlaps(Timeline& tl, const std::set<int>& ids)
{
    for (int id : ids) {
        int ti = -1;
        const Clip* x = tl.findClip(id, &ti);
        if (!x)
            continue;
        const TimeUs a = x->start, b = x->end();
        std::vector<int> under;
        for (const Clip& p : tl.tracks[size_t(ti)].clips)
            if (!ids.count(p.id) && p.start < b && p.end() > a)
                under.push_back(p.id);
        for (int pid : under) {
            Clip* p = tl.findClip(pid);
            if (!p)
                continue;
            if (p->start >= a && p->end() <= b) {
                removeClips(tl, {pid}); // wholly under it
            } else if (p->start < a && p->end() > b) {
                overwriteContained(tl, id); // around it
            } else if (p->start < a) {
                trimClipEnd(*p, a - p->end()); // runs into it from before
                clampFades(*p);
            } else {
                trimClipStart(*p, b - p->start); // starts under its end
                clampFades(*p);
            }
        }
    }
    tl.sortClips();
}

void rippleRemoveClips(Timeline& tl, const std::set<int>& ids)
{
    for (auto& track : tl.tracks) {
        std::vector<Clip> kept;
        TimeUs shift = 0;
        for (const Clip& c : track.clips) {
            if (ids.count(c.id)) {
                shift += c.length;
                continue;
            }
            Clip k = c;
            k.start = std::max<TimeUs>(0, k.start - shift);
            kept.push_back(k);
        }
        track.clips = std::move(kept);
    }
    tl.sortClips();
}

void rippleRemoveClipsAll(Timeline& tl, const std::set<int>& ids, bool markers)
{
    // The time the removed events used, merged into separate spans.
    std::vector<std::pair<TimeUs, TimeUs>> spans;
    for (const auto& track : tl.tracks)
        for (const Clip& c : track.clips)
            if (ids.count(c.id))
                spans.emplace_back(c.start, c.end());
    if (spans.empty())
        return;
    std::sort(spans.begin(), spans.end());
    std::vector<std::pair<TimeUs, TimeUs>> merged{spans.front()};
    for (const auto& s : spans) {
        if (s.first <= merged.back().second)
            merged.back().second = std::max(merged.back().second, s.second);
        else
            merged.push_back(s);
    }
    removeClips(tl, ids);
    // Latest first, so earlier spans still find their events in place.
    for (auto it = merged.rbegin(); it != merged.rend(); ++it)
        rippleShift(tl, it->second, it->first - it->second, {}, {}, markers);
}

void rippleShift(Timeline& tl, TimeUs from, TimeUs delta, const std::set<int>& trackIds, const std::set<int>& exclude,
                 bool markers)
{
    if (delta == 0)
        return;
    for (auto& track : tl.tracks) {
        if ((!trackIds.empty() && !trackIds.count(track.id)) || track.locked)
            continue;
        for (Clip& c : track.clips)
            if (c.start >= from && !exclude.count(c.id))
                c.start = std::max<TimeUs>(0, c.start + delta);
    }
    if (markers)
        for (Marker& m : tl.markers)
            if (m.time >= from)
                m.time = std::max<TimeUs>(0, m.time + delta);
    tl.sortClips();
    tl.sortMarkers();
}

void rippleInsert(Timeline& tl, TimeUs at, TimeUs length, const std::set<int>& trackIds, const std::set<int>& exclude,
                  bool markers)
{
    if (length <= 0)
        return;
    std::set<int> crossing;
    for (const auto& track : tl.tracks) {
        if ((!trackIds.empty() && !trackIds.count(track.id)) || track.locked)
            continue;
        for (const Clip& c : track.clips)
            if (c.start < at && c.end() > at && !exclude.count(c.id))
                crossing.insert(c.id);
    }
    if (!crossing.empty())
        splitClips(tl, crossing, at);
    rippleShift(tl, at, length, trackIds, exclude, markers);
}

namespace {
// Index of the event on track `ti` that overlaps the end of clip index i (or -1).
int overlappingNext(const Track& t, size_t i)
{
    return i + 1 < t.clips.size() && t.clips[i + 1].start < t.clips[i].end() ? int(i + 1) : -1;
}
} // namespace

bool hasTransition(const Timeline& tl, int clipId, bool atStart)
{
    int ti = -1;
    const Clip* c = tl.findClip(clipId, &ti);
    if (!c)
        return false;
    const Track& t = tl.tracks[size_t(ti)];
    const size_t i = size_t(c - t.clips.data());
    if (atStart)
        return c->fadeIn > 0 || (i > 0 && t.clips[i - 1].end() > c->start);
    return c->fadeOut > 0 || overlappingNext(t, i) >= 0;
}

void syncGroupFades(Timeline& tl, int clipId)
{
    const Clip* src = tl.findClip(clipId);
    if (!src || !src->groupId)
        return;
    const Clip c = *src;
    for (auto& t : tl.tracks)
        for (Clip& x : t.clips) {
            if (x.id == c.id || x.groupId != c.groupId)
                continue;
            if (x.start == c.start) {
                x.fadeIn = std::min(c.fadeIn, x.length);
                x.fadeInCurve = c.fadeInCurve;
            }
            if (x.end() == c.end()) {
                x.fadeOut = c.fadeOut;
                x.fadeOutCurve = c.fadeOutCurve;
            }
            clampFades(x);
        }
}

bool removeTransition(Timeline& tl, int clipId, bool atStart)
{
    int ti = -1;
    Clip* c = tl.findClip(clipId, &ti);
    if (!c)
        return false;
    const Track& track = tl.tracks[size_t(ti)];
    const size_t i = size_t(c - track.clips.data());
    if (!atStart) {
        if (const int next = overlappingNext(track, i); next >= 0)
            return removeTransition(tl, track.clips[size_t(next)].id, true);
        // The same end of its grouped events (a video file's audio) goes too.
        bool had = false;
        const TimeUs end = c->end();
        const std::set<int> group = expandToGroups(tl, {clipId});
        for (auto& t : tl.tracks)
            for (Clip& x : t.clips)
                if (group.count(x.id) && x.end() == end) {
                    had |= x.fadeOut > 0 || !x.transitionOut.type.isEmpty();
                    x.fadeOut = 0;
                    x.transitionOut = {};
                }
        return had;
    }
    // Wholly inside another event (a crossfade over all of it): cut that one around it.
    for (const Clip& p : track.clips)
        if (p.id != clipId && p.start <= c->start && p.end() >= c->end() && (p.start < c->start || p.end() > c->end()))
            return overwriteContained(tl, clipId);
    bool changed = false;
    for (auto& t : tl.tracks)
        for (Clip& x : t.clips)
            if (x.start == c->start && (x.id == clipId || (c->groupId && x.groupId == c->groupId))) {
                changed |= x.fadeIn > 0 || !x.transitionIn.type.isEmpty();
                x.fadeIn = 0;
                x.transitionIn = {};
            }
    // A crossfade: cut the event before where this one (and its grouped
    // events starting with it) begins.
    const TimeUs at = c->start;
    const std::set<int> ids = expandToGroups(tl, {clipId});
    for (auto& t : tl.tracks)
        for (const Clip& x : t.clips) {
            if (!ids.count(x.id) || x.start != at)
                continue;
            for (Clip& p : t.clips)
                if (p.id != x.id && p.start < at && p.end() > at && p.end() <= x.end()) {
                    trimClipEnd(p, at - p.end());
                    clampFades(p);
                    changed = true;
                }
        }
    return changed;
}

std::set<int> expandToGroups(const Timeline& tl, const std::set<int>& ids)
{
    std::set<int> groups;
    for (const auto& track : tl.tracks)
        for (const auto& c : track.clips)
            if (c.groupId && ids.count(c.id))
                groups.insert(c.groupId);
    std::set<int> out = ids;
    if (groups.empty())
        return out;
    for (const auto& track : tl.tracks)
        for (const auto& c : track.clips)
            if (c.groupId && groups.count(c.groupId))
                out.insert(c.id);
    return out;
}

void groupClips(Timeline& tl, const std::set<int>& ids)
{
    if (ids.size() < 2)
        return;
    const int group = tl.allocId();
    for (auto& track : tl.tracks)
        for (auto& c : track.clips)
            if (ids.count(c.id))
                c.groupId = group;
}

void ungroupClips(Timeline& tl, const std::set<int>& ids)
{
    for (auto& track : tl.tracks)
        for (auto& c : track.clips)
            if (ids.count(c.id))
                c.groupId = 0;
}

} // namespace ops
