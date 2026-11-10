#include "core/Multicam.h"

#include "core/TimelineOps.h"

#include <QJsonArray>
#include <algorithm>
#include <limits>

namespace multicam {
namespace {
const QString kPrefix = QStringLiteral("Angle ");

const Clip* angleClip(const Timeline& tl, int number)
{
    const QJsonArray ids = tl.multicam.value(QStringLiteral("angles")).toArray();
    return number >= 1 && number <= ids.size() ? tl.findClip(ids[number - 1].toInt()) : nullptr;
}

// The events that are angle `number`: its own, and what was split off it since (Split with nothing selected, Remove
// Silences and Insert Time split every track): the events of the same file, or the same kind of generated picture, on
// its track, that aren't another angle.
std::vector<const Clip*> angleClips(const Timeline& tl, int number)
{
    const QJsonArray ids = tl.multicam.value(QStringLiteral("angles")).toArray();
    const Clip* base = angleClip(tl, number);
    if (!base)
        return {};
    std::vector<const Clip*> out;
    for (const Track& t : tl.tracks) {
        if (std::none_of(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return &c == base; }))
            continue;
        for (const Clip& c : t.clips) {
            const bool other = &c != base && std::any_of(ids.begin(), ids.end(), [&](const QJsonValue& v) { return v.toInt() == c.id; });
            if (!other && c.mediaId == base->mediaId && c.generator.type == base->generator.type && c.isSequence() == base->isSequence())
                out.push_back(&c);
        }
    }
    return out;
}

Clip pieceOf(Timeline& tl, const Clip* a, int number, TimeUs t0, TimeUs t1);

// Pieces of angle `number` over [t0, t1) (as much of it as the angle covers).
std::vector<Clip> pieces(Timeline& tl, int number, TimeUs t0, TimeUs t1, QString* error)
{
    const std::vector<const Clip*> from = angleClips(tl, number);
    if (from.empty()) {
        if (error)
            *error = QStringLiteral("There is no angle %1.").arg(number);
        return {};
    }
    std::vector<Clip> out;
    for (const Clip* a : from) // (pieceOf only allocates ids: the events pointed at stay where they are)
        if (std::max(t0, a->start) < std::min(t1, a->end()))
            out.push_back(pieceOf(tl, a, number, std::max(t0, a->start), std::min(t1, a->end())));
    if (out.empty() && error)
        *error = QStringLiteral("Angle %1 has no picture there.").arg(number);
    return out;
}

Clip pieceOf(Timeline& tl, const Clip* a, int number, TimeUs t0, TimeUs t1)
{
    Clip c = *a;
    c.id = tl.allocId();
    reassignEffectIds(c.effects, [&] { return tl.allocId(); }); // (effect ids are unique too)
    c.start = t0;
    c.length = t1 - t0;
    c.sourceIn = a->sourceIn + (t0 - a->start);
    ops::shiftClipKeyframes(c, -(t0 - a->start)); // (keyframes count from the event's start)
    if (a->isGenerated()) // (an animated angle: each piece carries on in the same picture)
        c.generatorSpan = a->generatedLength();
    c.groupId = 0;
    c.fadeIn = c.fadeOut = 0;
    c.transitionIn = c.transitionOut = {};
    c.muted = false;
    c.narration = {};
    c.name = kPrefix + QString::number(number);
    return c;
}

void sortByStart(Track& t)
{
    std::sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

Track* track(Timeline& tl)
{
    const int i = trackIndex(tl);
    return i >= 0 ? &tl.tracks[size_t(i)] : nullptr;
}

Clip* segmentAt(Track& t, TimeUs at)
{
    for (Clip& c : t.clips)
        if (c.start <= at && at < c.end())
            return &c;
    return nullptr;
}
} // namespace

int trackIndex(const Timeline& tl)
{
    const int id = tl.multicam.value(QStringLiteral("track")).toInt();
    return id ? tl.trackIndex(id) : -1;
}

std::vector<Angle> angles(const Timeline& tl, const MediaMap& media)
{
    std::vector<Angle> out;
    const QJsonArray ids = tl.multicam.value(QStringLiteral("angles")).toArray();
    for (int i = 0; i < ids.size(); ++i)
        if (const Clip* c = tl.findClip(ids[i].toInt())) {
            const auto m = media.find(c->mediaId);
            out.push_back({i + 1, c->id, !c->name.isEmpty() ? c->name : m != media.end() && m->second ? m->second->displayName() : QString()});
        }
    return out;
}

int angleOf(const Clip& c)
{
    return c.name.startsWith(kPrefix) ? c.name.mid(kPrefix.size()).toInt() : 0;
}

void passOn(Timeline& tl, const std::set<int>& leaving)
{
    if (tl.multicam.isEmpty() || leaving.empty())
        return;
    QJsonArray ids = tl.multicam.value(QStringLiteral("angles")).toArray();
    bool changed = false;
    for (int i = 0; i < ids.size(); ++i) {
        const Clip* base = leaving.count(ids[i].toInt()) ? tl.findClip(ids[i].toInt()) : nullptr;
        if (!base)
            continue;
        const Clip *next = nullptr, *previous = nullptr; // the piece after it, else the one before
        for (const Clip* c : angleClips(tl, i + 1)) {
            if (c == base || leaving.count(c->id))
                continue;
            if (c->start >= base->start ? !next || c->start < next->start : !previous || c->start > previous->start)
                (c->start >= base->start ? next : previous) = c;
        }
        if (const Clip* heir = next ? next : previous) {
            ids[i] = heir->id;
            changed = true;
        }
    }
    if (changed)
        tl.multicam[QStringLiteral("angles")] = ids;
}

bool create(Timeline& tl, const MediaMap& media, const std::vector<int>& clipIds, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (trackIndex(tl) >= 0)
        return fail(QStringLiteral("This project has a multicam already."));
    if (clipIds.size() < 2)
        return fail(QStringLiteral("Select the video events of two or more cameras (line them up first: Sync Selected Events by Audio)."));
    if (clipIds.size() > 9)
        return fail(QStringLiteral("Nine angles at most (one for each number key)."));
    std::vector<int> angleTracks;
    for (int id : clipIds) {
        int ti = -1;
        const Clip* c = tl.findClip(id, &ti);
        const auto m = c ? media.find(c->mediaId) : media.end();
        const bool file = m != media.end() && m->second && m->second->info.hasVideo();
        const bool animated = c && c->isGenerated() && !c->isSequence(); // (cameras on an animation scene)
        if (!c || ti < 0 || !isVisualTrack(tl.tracks[size_t(ti)].type) || !(file || animated))
            return fail(QStringLiteral("Each angle must be a video event from a file, or a generated picture."));
        if (c->speed != 1.0 || c->reverse || c->hasRamp())
            return fail(QStringLiteral("Angles play at their own speed: put the speed of %1 back to 100%.")
                            .arg(!c->name.isEmpty() ? c->name : file ? m->second->displayName() : QStringLiteral("an angle")));
        angleTracks.push_back(tl.tracks[size_t(ti)].id);
    }
    QJsonArray ids;
    for (int id : clipIds)
        ids.append(id);
    const int index = ops::addTrack(tl, TrackType::Video, 0);
    Track& mc = tl.tracks[size_t(index)];
    mc.name = QStringLiteral("Multicam");
    tl.multicam = QJsonObject{{QStringLiteral("track"), mc.id}, {QStringLiteral("angles"), ids}};
    for (int id : angleTracks) // (their pictures are the Multicam track's now)
        if (const int ti = tl.trackIndex(id); ti >= 0)
            tl.tracks[size_t(ti)].muted = true;
    const Clip* first = angleClip(tl, 1);
    const std::vector<Clip> start = pieces(tl, 1, first->start, first->end(), error);
    if (start.empty())
        return false;
    Track& made = tl.tracks[size_t(trackIndex(tl))];
    made.clips.insert(made.clips.end(), start.begin(), start.end());
    sortByStart(made);
    return true;
}

bool cut(Timeline& tl, const MediaMap&, int angle, TimeUs t, QString* error)
{
    Track* mc = track(tl);
    if (!mc) {
        if (error)
            *error = QStringLiteral("There is no multicam in this project.");
        return false;
    }
    TimeUs end = 0;
    if (Clip* s = segmentAt(*mc, t)) {
        if (angleOf(*s) == angle)
            return true;
        end = s->end();
    } else { // in a gap: to the next stretch
        end = std::numeric_limits<TimeUs>::max() / 4;
        for (const Clip& c : mc->clips)
            if (c.start > t)
                end = std::min(end, c.start);
    }
    const std::vector<Clip> p = pieces(tl, angle, t, end, error);
    if (p.empty())
        return false;
    mc = track(tl); // (pieces() allocated ids: the timeline is the same, the reference too)
    if (Clip* s = segmentAt(*mc, t)) {
        s->length = t - s->start;
        ops::clampFades(*s); // (a fade longer than what is left of it)
        if (s->length <= 0)
            mc->clips.erase(mc->clips.begin() + (s - mc->clips.data()));
    }
    mc->clips.insert(mc->clips.end(), p.begin(), p.end());
    sortByStart(*mc);
    return true;
}

bool switchAngle(Timeline& tl, const MediaMap&, int angle, TimeUs t, QString* error)
{
    Track* mc = track(tl);
    Clip* s = mc ? segmentAt(*mc, t) : nullptr;
    if (!s) {
        if (error)
            *error = mc ? QStringLiteral("The Multicam track is empty at the cursor.") : QStringLiteral("There is no multicam in this project.");
        return false;
    }
    if (angleOf(*s) == angle)
        return true;
    const TimeUs s0 = s->start, s1 = s->end();
    const std::vector<Clip> p = pieces(tl, angle, s0, s1, error);
    if (p.empty())
        return false;
    mc = track(tl);
    s = segmentAt(*mc, t);
    mc->clips.erase(mc->clips.begin() + (s - mc->clips.data()));
    mc->clips.insert(mc->clips.end(), p.begin(), p.end());
    sortByStart(*mc);
    return true;
}

} // namespace multicam
