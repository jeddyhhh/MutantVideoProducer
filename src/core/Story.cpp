#include "core/Story.h"
#include "core/TimelineOps.h"
#include <QJsonArray>
#include <QRegularExpression>
#include <random>
#include <limits>

namespace {
TimeUs time(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().toLongLong(); }
std::set<int> ids(const QJsonObject& o)
{
    std::set<int> r;
    for (auto v : o["clips"].toArray())
        r.insert(v.toInt());
    return r;
}
// The chapter's clips that are still in it: one dragged out of its range stays assigned (dragged back, it is the
// chapter's again) but is not moved, stretched or deleted with it. (20 ms: older projects' times drifted a little.)
std::set<int> ids(const Timeline& tl, const QJsonObject& o)
{
    constexpr TimeUs kNear = 20000;
    const TimeUs at = time(o, "start"), end = at + time(o, "narration") + time(o, "listening");
    std::set<int> r;
    for (int id : ids(o))
        if (const Clip* c = tl.findClip(id); c && c->start >= at - kNear && c->start < end + kNear)
            r.insert(id);
    return r;
}
QJsonArray array(const std::set<int>& values)
{
    QJsonArray a;
    for (int id : values)
        a.append(id);
    return a;
}
int chapterIndex(const Timeline& tl, int id)
{
    for (int i = 0; i < tl.chapters.size(); ++i)
        if (tl.chapters[i].toObject()["id"].toInt() == id)
            return i;
    return -1;
}
bool fail(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return false;
}
void shiftTrack(Track& track, TimeUs delta)
{
    for (auto& c : track.clips)
        c.start += delta;
    shiftKeyframes(track.effects, delta); // (a rack chain's own settings too)
    for (auto* e : {&track.volume, &track.panEnvelope, &track.composite})
        for (auto& k : e->points)
            k.time += delta;
}
// A chapter's footage sound: down 22 dB under the narration window, back up over its last tenth of a second. (Keys at
// one time each: a chapter with a short window or none got two or three at 0, and opened again it kept another one.)
void duckUnderNarration(ParamValue& p, TimeUs narration)
{
    p.value = narration > 0 ? -22 : 0;
    p.keys.clear();
    if (narration <= 0)
        return;
    p.keys.push_back({0, -22, Keyframe::Interp::Linear});
    if (narration > 100000)
        p.keys.push_back({narration - 100000, -22, Keyframe::Interp::Linear});
    p.keys.push_back({narration, 0, Keyframe::Interp::Linear});
}
void shiftChapter(QJsonObject& chapter, TimeUs delta) { chapter["start"] = QString::number(time(chapter, "start") + delta); }
// Each chapter's region where the chapter is (by id: an operation that moves regions by time range misses one cut
// down to nothing, as Remove Silences can leave it).
void regions(Timeline& tl)
{
    for (const auto value : tl.chapters) {
        const auto c = value.toObject();
        if (Marker* m = tl.findMarker(c["id"].toInt())) {
            m->time = time(c, "start");
            m->length = std::max<TimeUs>(1, time(c, "narration") + time(c, "listening"));
        }
    }
    tl.sortMarkers();
}
} // namespace
namespace story {
int addChapter(Timeline& tl, const QString& name, const QString& script, TimeUs narration, TimeUs listening, const MediaItem* source,
               TimeUs sourceIn, const FrameRate& fps, QString* error)
{
    if (name.trimmed().isEmpty() || narration < 0 || listening < 0 || narration + listening <= 0
        || narration + listening > 3600 * kUsPerSecond) {
        fail(error, "Give the chapter a name and a duration up to one hour.");
        return 0;
    }
    TimeUs chapterEnd = tl.duration();
    for (auto value : tl.chapters) {
        auto c = value.toObject();
        chapterEnd = std::max(chapterEnd, time(c, "start") + time(c, "narration") + time(c, "listening"));
    }
    // Never round back into the previous chapter: a legacy end 1 us off the grid is abutted exactly.
    const TimeUs at = std::max(fps.snap(chapterEnd), chapterEnd);
    const TimeUs narrationEnd = std::max(at, fps.snap(at + narration)), end = std::max(narrationEnd, fps.snap(at + narration + listening));
    if (end <= at) {
        fail(error, "Give the chapter a name and a duration up to one hour.");
        return 0;
    }
    narration = narrationEnd - at;
    listening = end - narrationEnd;
    const int id = tl.allocId();
    std::set<int> clips;
    if (source) {
        const auto placed = ops::insertMedia(tl, *source, at, -1, fps, true);
        for (int cid : placed)
            if (auto* c = tl.findClip(cid)) {
                c->sourceIn = std::max<TimeUs>(0, sourceIn);
                c->length = narration + listening;
                if (!source->info.isStill && c->sourceIn + c->length > source->info.duration) {
                    c->sourceIn = std::clamp<TimeUs>(c->sourceIn, 0, std::max<TimeUs>(0, source->info.duration - 1));
                    c->loopLength = source->info.duration;
                }
                c->name = name;
                c->narration["story_source_duration"] = QString::number(source->info.duration);
                c->narration["story_source_still"] = source->info.isStill;
                clips.insert(cid);
                int ti = -1;
                tl.findClip(cid, &ti);
                if (tl.tracks[size_t(ti)].type == TrackType::Audio) {
                    auto effect = makeEffectOfType("a_gain", tl.allocId());
                    effect.params["storyGain"].value = 1;
                    duckUnderNarration(effect.params["gain"], narration);
                    c->effects.push_back(effect);
                }
            }
    }
    if (!source) {
        int ti = ops::addTrack(tl, TrackType::Video, 0);
        tl.tracks[size_t(ti)].name = "Storyboard";
        Clip c;
        c.id = tl.allocId();
        c.start = at;
        c.length = narration + listening;
        c.name = name + " / footage placeholder";
        c.generator.type = "text";
        c.generator.params["text"].text = name;
        c.generator.params["size"].value = 8;
        tl.tracks[size_t(ti)].clips.push_back(c);
        clips.insert(c.id);
    }
    QJsonObject chapter{{"id", id},
                        {"name", name.trimmed()},
                        {"script", script},
                        {"start", QString::number(at)},
                        {"narration", QString::number(narration)},
                        {"listening", QString::number(listening)},
                        {"clips", array(clips)},
                        {"stretch", array(clips)}};
    tl.chapters.append(chapter);
    Marker marker;
    marker.id = id;
    marker.name = "Chapter: " + name.trimmed();
    marker.time = at;
    marker.length = narration + listening;
    marker.color = 5;
    tl.markers.push_back(marker);
    tl.sortMarkers();
    return id;
}
bool updateChapter(Timeline& tl, int id, const QString& name, const QString& script, TimeUs narration, TimeUs listening,
                   const FrameRate& fps, QString* error)
{
    const int i = chapterIndex(tl, id);
    if (i < 0)
        return fail(error, "Chapter not found.");
    if (name.trimmed().isEmpty() || narration < 0 || listening < 0 || narration + listening <= 0
        || narration + listening > 3600 * kUsPerSecond)
        return fail(error, "Use a chapter name and a positive duration up to one hour.");
    auto chapter = tl.chapters[i].toObject();
    const TimeUs at = time(chapter, "start"), old = time(chapter, "narration") + time(chapter, "listening");
    const TimeUs narrationEnd = std::max(at, fps.snap(at + narration)), end = std::max(narrationEnd, fps.snap(at + narration + listening));
    if (end <= at)
        return fail(error, "Use a chapter name and a positive duration up to one hour.");
    narration = narrationEnd - at;
    listening = end - narrationEnd;
    const TimeUs duration = end - at, delta = duration - old;
    // Material starting within half a frame of the old end belongs to what follows (older projects drifted by 1 us);
    // the next chapter is moved to abut the new end exactly, healing that drift.
    const TimeUs oldEnd = at + old, from = oldEnd - fps.frameDuration() / 2;
    TimeUs next = std::numeric_limits<TimeUs>::max();
    for (int j = 0; j < tl.chapters.size(); ++j)
        if (j != i) {
            const TimeUs s = time(tl.chapters[j].toObject(), "start");
            if (s >= from)
                next = std::min(next, s);
        }
    const TimeUs shift = next <= oldEnd + fps.frameDuration() / 2 ? end - next : delta;
    const auto own = ids(tl, chapter);
    std::set<int> stretch;
    for (auto value : chapter["stretch"].toArray())
        stretch.insert(value.toInt());
    for (int cid : own)
        if (const auto* clip = tl.findClip(cid)) {
            if (clip->narration.contains("takes") && clip->end() > at + narration)
                return fail(error, "Existing narration would enter the listening section. Trim or regenerate it first.");
            if (delta < 0 && clip->end() > at + duration
                && !(stretch.count(cid) && clip->start == at && clip->length == old && !clip->narration.contains("takes")))
                return fail(error, "Trim the assigned clips to the new chapter end before shortening it.");
        }
    // A duration change must not cut or silently change unrelated overlapping material.
    if (delta > 0)
        for (int cid : own)
            if (const auto* c = tl.findClip(cid); c && c->isSequence() && c->end() == at + old)
                return fail(error, "Unpack the chapter compound before extending its listening time, then pack it again after the change.");
    if (delta)
        for (const auto& t : tl.tracks)
            for (const auto& c : t.clips)
                if (!own.count(c.id) && c.start < from && c.end() > oldEnd)
                    return fail(error, "An unassigned clip crosses this chapter's end. Split and assign it before changing the duration.");
    if (shift)
        ops::rippleShift(tl, from, shift, {}, own, true, false);
    for (int cid : own)
        if (auto* c = tl.findClip(cid)) {
            if (stretch.count(cid) && c->start == at && c->length == old) {
                c->length = duration;
                const auto available = c->narration["story_source_duration"].toString().toLongLong();
                if (available > 0 && !c->narration["story_source_still"].toBool() && c->sourceIn + c->sourceSpan() > available)
                    c->loopLength = available;
            }
            if (c->narration.contains("takes"))
                continue;
            for (auto& effect : c->effects)
                if (effect.type == "a_gain" && effect.params.count("storyGain")) {
                    effect.params["storyGain"].value = 1;
                    duckUnderNarration(effect.params["gain"], narration);
                }
        }
    for (int j = 0; j < tl.chapters.size(); ++j)
        if (j != i) {
            auto c = tl.chapters[j].toObject();
            if (time(c, "start") >= from) {
                shiftChapter(c, shift);
                tl.chapters[j] = c;
            }
        }
    chapter["name"] = name.trimmed();
    chapter["script"] = script;
    chapter["narration"] = QString::number(narration);
    chapter["listening"] = QString::number(listening);
    tl.chapters[i] = chapter;
    if (auto* m = tl.findMarker(id)) {
        m->name = "Chapter: " + name.trimmed();
        m->time = at;
        m->length = duration;
    }
    tl.sortClips();
    tl.sortMarkers();
    regions(tl);
    return true;
}
bool bindChapter(Timeline& tl, int id, const std::set<int>& selection, QString* error)
{
    int i = chapterIndex(tl, id);
    if (i < 0)
        return fail(error, "Chapter not found.");
    auto chapter = tl.chapters[i].toObject();
    auto bound = ids(chapter);
    const auto expanded = ops::expandToGroups(tl, selection);
    const auto at = time(chapter, "start"), end = at + time(chapter, "narration") + time(chapter, "listening");
    for (int cid : expanded) {
        const auto* c = tl.findClip(cid);
        if (!c || c->start < at || c->end() > end)
            return fail(error, "Assigned clips must fit entirely inside the chapter.");
        bound.insert(cid);
    }
    chapter["clips"] = array(bound);
    tl.chapters[i] = chapter;
    return true;
}
bool moveChapter(Timeline& tl, int id, int direction, QString* error)
{
    int i = chapterIndex(tl, id), j = i + (direction < 0 ? -1 : 1);
    if (i < 0 || j < 0 || j >= tl.chapters.size())
        return fail(error, "There is no adjacent chapter in that direction.");
    auto a = tl.chapters[std::min(i, j)].toObject(), b = tl.chapters[std::max(i, j)].toObject();
    const auto start = time(a, "start"), la = time(a, "narration") + time(a, "listening"), lb = time(b, "narration") + time(b, "listening");
    if (time(b, "start") != start + la)
        return fail(error, "Only adjacent chapter ranges can be swapped.");
    auto ai = ids(tl, a), bi = ids(tl, b);
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (c.start < start + la + lb && c.end() > start && !ai.count(c.id) && !bi.count(c.id))
                return fail(error, "Assign all material in both chapters before swapping them.");
    for (auto& t : tl.tracks)
        for (auto& c : t.clips) {
            if (ai.count(c.id))
                c.start += lb;
            else if (bi.count(c.id))
                c.start -= la;
        }
    for (auto& m : tl.markers) {
        if (m.time >= start && m.time < start + la)
            m.time += lb;
        else if (m.time >= start + la && m.time < start + la + lb)
            m.time -= la;
    }
    shiftChapter(a, lb);
    shiftChapter(b, -la);
    tl.chapters[std::min(i, j)] = b;
    tl.chapters[std::max(i, j)] = a;
    tl.sortClips();
    tl.sortMarkers();
    regions(tl);
    return true;
}
bool removeChapter(Timeline& tl, int id, QString* error)
{
    int i = chapterIndex(tl, id);
    if (i < 0)
        return fail(error, "Chapter not found.");
    const auto c = tl.chapters[i].toObject();
    const auto at = time(c, "start"), length = time(c, "narration") + time(c, "listening");
    const auto owned = ids(tl, c);
    for (const auto& t : tl.tracks)
        for (const auto& clip : t.clips)
            if (clip.start < at + length && clip.end() > at && !owned.count(clip.id))
                return fail(error, "Assign all material in the chapter before deleting its range.");
    ops::removeClips(tl, owned);
    ops::rippleShift(tl, at + length, -length, {}, {}, true, false);
    std::erase_if(tl.markers, [&](const Marker& m) { return m.id == id; });
    tl.chapters.removeAt(i);
    for (int j = 0; j < tl.chapters.size(); ++j) {
        auto q = tl.chapters[j].toObject();
        if (time(q, "start") >= at + length) {
            shiftChapter(q, -length);
            tl.chapters[j] = q;
        }
    }
    regions(tl);
    return true;
}
int adoptChapter(Timeline& tl, const QString& name, const QString& script, TimeUs from, TimeUs to, TimeUs narration, QString* error)
{
    if (name.trimmed().isEmpty() || from < 0 || to <= from || narration < 0 || narration > to - from || to - from > 3600 * kUsPerSecond) {
        fail(error, "Select a positive region up to one hour and a narration window within it.");
        return 0;
    }
    for (auto value : tl.chapters) {
        auto c = value.toObject();
        const auto at = time(c, "start"), end = at + time(c, "narration") + time(c, "listening");
        if (from < end && to > at) {
            fail(error, "Chapter ranges must not overlap.");
            return 0;
        }
    }
    ops::splitClips(tl, {}, from);
    ops::splitClips(tl, {}, to);
    std::set<int> owned;
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (c.start >= from && c.end() <= to)
                owned.insert(c.id);
    int id = tl.allocId();
    QJsonObject chapter{{"id", id},
                        {"name", name.trimmed()},
                        {"script", script},
                        {"start", QString::number(from)},
                        {"narration", QString::number(narration)},
                        {"listening", QString::number(to - from - narration)},
                        {"clips", array(owned)}};
    int i = 0;
    while (i < tl.chapters.size() && time(tl.chapters[i].toObject(), "start") < from)
        ++i;
    tl.chapters.insert(i, chapter);
    Marker marker;
    marker.id = id;
    marker.name = "Chapter: " + name.trimmed();
    marker.time = from;
    marker.length = to - from;
    marker.color = 5;
    tl.markers.push_back(marker);
    tl.sortMarkers();
    return id;
}
std::vector<int> compound(Timeline& tl, const std::set<int>& selected, const QString& name, QString* error)
{
    auto selection = ops::expandToGroups(tl, selected);
    TimeUs start = std::numeric_limits<TimeUs>::max(), end = 0;
    int firstVisual = int(tl.tracks.size()), lastVisual = -1;
    bool picture = false, sound = false;
    for (size_t ti = 0; ti < tl.tracks.size(); ++ti)
        for (const auto& c : tl.tracks[ti].clips)
            if (selection.count(c.id)) {
                start = std::min(start, c.start);
                end = std::max(end, c.end());
                if (isVisualTrack(tl.tracks[ti].type)) {
                    picture = true;
                    firstVisual = std::min(firstVisual, int(ti));
                    lastVisual = int(ti);
                } else
                    sound = true;
            }
    if (end <= start) {
        fail(error, "Select clips to make a compound sequence.");
        return {};
    }
    for (int ti = firstVisual; ti <= lastVisual; ++ti)
        for (const auto& c : tl.tracks[size_t(ti)].clips)
            if (!selection.count(c.id) && c.start < end && c.end() > start) {
                // (on a selected track itself, typically a neighbour crossfading into the first or last clip)
                const QString what = c.name.isEmpty() ? QStringLiteral("An event") : QStringLiteral("\"%1\"").arg(c.name);
                if (error)
                    *error = QStringLiteral("%1 at %2 s on track %3 overlaps the selection: include it (or remove the crossfade) to keep "
                                            "the compositing order.")
                                 .arg(what).arg(double(c.start) / kUsPerSecond, 0, 'f', 2).arg(ti + 1);
                return {};
            }
    std::set<int> tracks;
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (selection.count(c.id))
                tracks.insert(t.id);
    std::function<int(const Timeline&)> depth = [&](const Timeline& t) {
        int d = 0;
        for (const auto& tr : t.tracks)
            for (const auto& c : tr.clips)
                if (c.isSequence())
                    d = std::max(d, 1 + depth(*c.sequence.value));
        return d;
    };
    if (depth(tl) >= 15) {
        fail(error, "The maximum sequence nesting depth is fifteen.");
        return {};
    }
    for (const auto& t : tl.tracks) {
        if (t.duckUnder && tracks.count(t.id) != tracks.count(t.duckUnder)) {
            fail(error, "Include both sides of audio ducking before making a compound.");
            return {};
        }
        if (t.solo) {
            fail(error, "Turn off track solos before making a compound.");
            return {};
        }
        if (tracks.count(t.id) && isVisualTrack(t.type) && t.blend) {
            fail(error, "Reset the selected tracks to Normal blend mode before packing their layers.");
            return {};
        }
    }
    Timeline nested;
    nested.nextId = tl.nextId;
    nested.narrationSettings = tl.narrationSettings;
    for (auto& t : tl.tracks) {
        Track inner = t;
        std::erase_if(inner.clips, [&](const Clip& c) { return !selection.count(c.id); });
        if (inner.clips.empty())
            continue;
        shiftTrack(inner, -start);
        nested.tracks.push_back(std::move(inner));
        std::erase_if(t.clips, [&](const Clip& c) { return selection.count(c.id); });
    }
    const int originalInsert = picture ? firstVisual : int(tl.tracks.size());
    int insert = 0;
    for (int i = 0; i < originalInsert; ++i)
        if (!tl.tracks[size_t(i)].clips.empty() || !tracks.count(tl.tracks[size_t(i)].id))
            ++insert;
    std::erase_if(tl.tracks, [&](const Track& t) { return tracks.count(t.id) && t.clips.empty(); });
    auto content = std::make_shared<const Timeline>(std::move(nested));
    int group = tl.allocId();
    std::vector<int> result;
    std::set<int> containers;
    for (auto type : {TrackType::Video, TrackType::Audio}) {
        if ((type == TrackType::Video && !picture) || (type == TrackType::Audio && !sound))
            continue;
        Track t;
        t.id = tl.allocId();
        t.type = type;
        t.name = name + (type == TrackType::Video ? " / Sequence" : " / Sequence audio");
        Clip c;
        c.id = tl.allocId();
        c.groupId = group;
        c.name = name;
        c.start = start;
        c.length = end - start;
        c.sequence.value = content;
        t.clips.push_back(c);
        result.push_back(c.id);
        containers.insert(c.id);
        tl.tracks.insert(tl.tracks.begin() + insert++, std::move(t));
    }
    for (int i = 0; i < tl.chapters.size(); ++i) {
        auto chapter = tl.chapters[i].toObject();
        auto own = ids(chapter);
        if (!own.empty() && std::all_of(own.begin(), own.end(), [&](int cid) { return selection.count(cid); })) {
            chapter["clips"] = array(containers);
            tl.chapters[i] = chapter;
        }
    }
    return result;
}
bool unpack(Timeline& tl, int clipId, QString* error)
{
    const auto* found = tl.findClip(clipId);
    if (!found || !found->isSequence())
        return fail(error, "Select a compound sequence.");
    const Clip container = *found;
    if (container.speed != 1 || container.reverse || container.hasRamp() || container.gain != 1 || !container.effects.empty()
        || !container.panCrop.empty() || container.fadeIn || container.fadeOut)
        return fail(error, "Edit inside this compound to retain its applied transformations. Reset them before unpacking.");
    const auto group = ops::expandToGroups(tl, {clipId});
    int index = 0;
    tl.findClip(clipId, &index);
    const auto nested = *container.sequence.value;
    if (container.sourceIn != 0 || container.length != nested.duration())
        return fail(error, "Restore the compound's full source range before unpacking.");
    for (int id : group) {
        int ti = 0;
        const auto* c = tl.findClip(id, &ti);
        if (!c || !c->isSequence() || c->sequence != container.sequence)
            return fail(error, "Ungroup unrelated clips before unpacking.");
        const auto& t = tl.tracks[size_t(ti)];
        if (t.level != 1 || t.pan != 0 || t.muted || t.solo || t.blend || t.duckUnder || !t.effects.empty() || t.volume.active
            || t.panEnvelope.active || t.composite.active)
            return fail(error, "Reset the container track's effects and controls before unpacking.");
    }
    if (!nested.outputEffects.empty() || !nested.audioMasterEffects.empty())
        return fail(error, "Keep this compound to retain its output effects.");
    std::set<int> containerTracks;
    for (int id : group) {
        int ti = 0;
        tl.findClip(id, &ti);
        containerTracks.insert(tl.tracks[size_t(ti)].id);
    }
    ops::removeClips(tl, group);
    int insertion = 0;
    for (int i = 0; i < index; ++i)
        if (!tl.tracks[size_t(i)].clips.empty() || !containerTracks.count(tl.tracks[size_t(i)].id))
            ++insertion;
    std::erase_if(tl.tracks, [&](const Track& t) { return containerTracks.count(t.id) && t.clips.empty(); });
    index = insertion;
    std::map<int, int> mapping, groups;
    std::set<int> restored;
    Timeline content = nested;
    for (auto& t : content.tracks) {
        const int old = t.id;
        t.id = tl.allocId();
        mapping[old] = t.id;
        for (auto& c : t.clips) {
            int oldClip = c.id;
            c.id = tl.allocId();
            mapping[oldClip] = c.id;
            restored.insert(c.id);
            if (c.groupId && !groups.count(c.groupId))
                groups[c.groupId] = tl.allocId();
        }
    }
    std::function<void(EffectChain&)> effects = [&](EffectChain& chain) {
        for (auto& e : chain) {
            e.id = tl.allocId();
            for (auto& rc : e.chains)
                effects(rc.effects);
        }
    };
    for (auto& t : content.tracks) {
        if (t.duckUnder)
            t.duckUnder = mapping[t.duckUnder];
        effects(t.effects);
        for (auto& c : t.clips) {
            if (c.groupId)
                c.groupId = groups[c.groupId];
            effects(c.effects);
            const int owner = c.narration.value(QLatin1String("caption_owner")).toInt(); // (value(): [] would add the key)
            if (owner)
                c.narration["caption_owner"] = mapping.count(owner) ? mapping[owner] : 0;
        }
        shiftTrack(t, container.start);
        tl.tracks.insert(tl.tracks.begin() + index++, std::move(t));
    }
    for (int i = 0; i < tl.chapters.size(); ++i) {
        auto chapter = tl.chapters[i].toObject();
        auto own = ids(chapter);
        if (std::any_of(own.begin(), own.end(), [&](int cid) { return group.count(cid); })) {
            chapter["clips"] = array(restored);
            std::set<int> stretch;
            for (auto value : chapter["stretch"].toArray())
                if (mapping.count(value.toInt()))
                    stretch.insert(mapping[value.toInt()]);
            chapter["stretch"] = array(stretch);
            tl.chapters[i] = chapter;
        }
    }
    tl.sortClips();
    return true;
}
bool replaceCompound(Timeline& tl, int clipId, const Timeline& content, bool ripple, QString* error)
{
    const auto* found = tl.findClip(clipId);
    if (!found || !found->isSequence())
        return fail(error, "Choose a compound sequence.");
    if (content.duration() <= 0)
        return fail(error, "A sequence must contain at least one clip.");
    const QString issue = timelineProblem(content);
    if (!issue.isEmpty())
        return fail(error, issue);
    const Clip container = *found;
    const bool full = container.sourceIn == 0 && container.speed == 1 && !container.hasRamp() && !container.reverse
        && container.length == container.sequence.value->duration();
    const TimeUs delta = full ? content.duration() - container.length : 0;
    const auto group = ops::expandToGroups(tl, {clipId});
    std::set<int> resized; // (the chapter that is the compound: its listening time follows the content)
    for (int i = 0; i < tl.chapters.size(); ++i) {
        auto chapter = tl.chapters[i].toObject();
        const auto own = ids(chapter);
        const auto start = time(chapter, "start");
        if (std::any_of(group.begin(), group.end(), [&](int id) { return own.count(id); }) && start == container.start
            && start + time(chapter, "narration") + time(chapter, "listening") == container.end()) {
            const TimeUs listening = time(chapter, "listening") + delta;
            if (listening < 0)
                return fail(
                    error,
                    "The sequence is shorter than the chapter's narration window. Shorten that window before trimming the compound.");
            if (!ripple && delta)
                for (auto v : tl.chapters) {
                    auto other = v.toObject();
                    if (other["id"] != chapter["id"] && time(other, "start") >= container.end()
                        && time(other, "start") < container.end() + delta)
                        return fail(error, "Enable ripple to keep the following chapter clear.");
                }
            chapter["listening"] = QString::number(listening);
            tl.chapters[i] = chapter;
            resized.insert(chapter["id"].toInt());
            if (auto* marker = tl.findMarker(chapter["id"].toInt()))
                marker->length = time(chapter, "narration") + listening;
        }
    }
    if (ripple && delta) {
        // The others as any ripple edit at the compound's end moves them: later ones along, one it ends inside of
        // longer or shorter (moved back past it, the next would overlap it).
        ops::rippleShift(tl, container.end(), delta, {}, group, true, false);
        ops::rippleChapters(tl, container.end(), delta, resized);
    }
    const auto immutable = std::make_shared<const Timeline>(content);
    for (int id : group)
        if (auto* c = tl.findClip(id); c && c->isSequence() && c->sequence == container.sequence) {
            c->sequence.value = immutable;
            c->length = full ? immutable->duration() : std::min(c->length, ops::maxClipLength(nullptr, *c));
            if (c->length <= 0)
                return fail(error, "A trimmed container would have no source left after this edit.");
        }
    tl.nextId = std::max(tl.nextId, content.nextId);
    tl.sortClips();
    return true;
}
int directorsDice(Timeline& tl, const std::set<int>& selection, unsigned seed, int style, double intensity)
{
    intensity = std::clamp(intensity, 0.0, 1.0);
    std::mt19937 rng(seed);
    int count = 0;
    for (auto& t : tl.tracks)
        if (isVisualTrack(t.type))
            for (auto& c : t.clips)
                if (selection.count(c.id) && !c.isGenerated()) {
                    c.panCrop["mode"].value = 1;
                    auto& zoom = c.panCrop["zoom"];
                    const double base = std::max(1.0, zoom.value);
                    const double extra = (style + 1) * .045 * intensity;
                    const bool in = bool(rng() % 2);
                    zoom.value = base;
                    zoom.keys = {{0, base + (in ? 0 : extra), Keyframe::Interp::Smooth},
                                 {c.length, base + (in ? extra : 0), Keyframe::Interp::Smooth}};
                    if (style >= 1) {
                        auto e = makeEffectOfType("rgb_split", tl.allocId());
                        e.params["redX"].value = -8;
                        e.params["blueX"].value = 8;
                        e.params["fxMix"].value = 0;
                        e.params["fxMix"].keys = {{0, .18 * intensity, Keyframe::Interp::Linear},
                                                  {std::min<TimeUs>(150000, c.length), 0, Keyframe::Interp::Linear}};
                        c.effects.push_back(e);
                    }
                    if (style == 2 && intensity > 0) {
                        c.fadeIn = std::min<TimeUs>(120000, c.length / 4);
                        c.transitionIn.type = (rng() % 2) ? "glitch" : "slices";
                    }
                    ++count;
                }
    return count;
}
} // namespace story
