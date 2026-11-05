#include "core/Multicam.h"

#include "core/TimelineOps.h"

#include <QJsonArray>
#include <algorithm>
#include <limits>
#include <optional>

namespace multicam {
namespace {
const QString kPrefix = QStringLiteral("Angle ");

const Clip* angleClip(const Timeline& tl, int number)
{
    const QJsonArray ids = tl.multicam.value(QStringLiteral("angles")).toArray();
    return number >= 1 && number <= ids.size() ? tl.findClip(ids[number - 1].toInt()) : nullptr;
}

// A piece of angle `number` over [t0, t1) (as much of it as the angle covers).
std::optional<Clip> piece(Timeline& tl, int number, TimeUs t0, TimeUs t1, QString* error)
{
    const Clip* a = angleClip(tl, number);
    if (!a) {
        if (error)
            *error = QStringLiteral("There is no angle %1.").arg(number);
        return std::nullopt;
    }
    t0 = std::max(t0, a->start);
    t1 = std::min(t1, a->end());
    if (t1 <= t0) {
        if (error)
            *error = QStringLiteral("Angle %1 has no picture there.").arg(number);
        return std::nullopt;
    }
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
    std::optional<Clip> start = piece(tl, 1, first->start, first->end(), error);
    if (!start)
        return false;
    tl.tracks[size_t(trackIndex(tl))].clips.push_back(*start);
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
    std::optional<Clip> p = piece(tl, angle, t, end, error);
    if (!p)
        return false;
    mc = track(tl); // (piece() allocated an id: the timeline is the same, the reference too)
    if (Clip* s = segmentAt(*mc, t)) {
        s->length = t - s->start;
        if (s->length <= 0)
            mc->clips.erase(mc->clips.begin() + (s - mc->clips.data()));
    }
    mc->clips.push_back(*p);
    std::sort(mc->clips.begin(), mc->clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
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
    std::optional<Clip> p = piece(tl, angle, s0, s1, error);
    if (!p)
        return false;
    mc = track(tl);
    s = segmentAt(*mc, t);
    *s = *p;
    std::sort(mc->clips.begin(), mc->clips.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
    return true;
}

} // namespace multicam
