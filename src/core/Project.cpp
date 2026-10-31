#include "core/Project.h"
#include "core/Graphics.h"
#include "core/ProjectCollection.h"
#include "core/ProjectFormat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <unordered_map>

// ---------------------------------------------------------------------------
// Timeline
// ---------------------------------------------------------------------------

TimeUs Timeline::soundEnd() const
{
    TimeUs end = 0;
    for (const Track& t : tracks)
        if (t.type == TrackType::Audio)
            for (const Clip& c : t.clips)
                end = std::max(end, c.end());
    return end;
}

TimeUs Timeline::duration() const
{
    TimeUs end = 0;
    for (const auto& t : tracks)
        for (const auto& c : t.clips)
            end = std::max(end, c.end());
    return end;
}

Clip* Timeline::findClip(int clipId, int* trackIndex)
{
    for (size_t ti = 0; ti < tracks.size(); ++ti)
        for (auto& c : tracks[ti].clips)
            if (c.id == clipId) {
                if (trackIndex)
                    *trackIndex = int(ti);
                return &c;
            }
    return nullptr;
}

const Clip* Timeline::findClip(int clipId, int* trackIndex) const
{
    return const_cast<Timeline*>(this)->findClip(clipId, trackIndex);
}

Marker* Timeline::findMarker(int markerId)
{
    for (auto& m : markers)
        if (m.id == markerId)
            return &m;
    return nullptr;
}

const Marker* Timeline::findMarker(int markerId) const
{
    return const_cast<Timeline*>(this)->findMarker(markerId);
}

int Timeline::trackIndex(int trackId) const
{
    for (size_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].id == trackId)
            return int(i);
    return -1;
}

void Timeline::sortClips()
{
    for (auto& t : tracks)
        std::stable_sort(t.clips.begin(), t.clips.end(),
                         [](const Clip& a, const Clip& b) { return a.start < b.start; });
}

void Timeline::sortMarkers()
{
    std::stable_sort(markers.begin(), markers.end(), [](const Marker& a, const Marker& b) { return a.time < b.time; });
}

Timeline defaultTimeline()
{
    Timeline t;
    Track v;
    v.id = t.allocId();
    v.type = TrackType::Video;
    v.name = QStringLiteral("Video");
    Track a;
    a.id = t.allocId();
    a.type = TrackType::Audio;
    a.name = QStringLiteral("Audio");
    t.tracks = {v, a};
    return t;
}

QString MediaItem::displayName() const
{
    return QFileInfo(path).fileName();
}

// ---------------------------------------------------------------------------
// Envelopes
// ---------------------------------------------------------------------------

double Envelope::at(TimeUs t) const
{
    if (points.empty())
        return value;
    if (t <= points.front().time)
        return points.front().value;
    if (t >= points.back().time)
        return points.back().value;
    auto next = std::upper_bound(points.begin(), points.end(), t, [](TimeUs v, const Keyframe& k) { return v < k.time; });
    const Keyframe& a = *std::prev(next);
    const Keyframe& b = *next;
    return keyframeValue(a, b, double(t - a.time) / double(std::max<TimeUs>(1, b.time - a.time)));
}

namespace {

// Area under one segment from its start to fraction f (0..1) of it.
double segmentArea(const Keyframe& a, const Keyframe& b, double f)
{
    const double d = double(b.time - a.time), dv = b.value - a.value;
    switch (a.interp) {
    case Keyframe::Interp::Hold: return d * a.value * f;
    case Keyframe::Interp::Smooth: return d * (a.value * f + dv * (f * f * f - 0.5 * f * f * f * f)); // smoothstep
    case Keyframe::Interp::Linear: return d * (a.value * f + dv * 0.5 * f * f);
    default: break;
    }
    // Other curves: Simpson's rule (exact for the cubic eases, within 1e-6 of the rest).
    const int n = 128;
    const double h = f / n;
    double sum = keyframeValue(a, b, 0.0) + keyframeValue(a, b, f);
    for (int i = 1; i < n; ++i)
        sum += (i % 2 ? 4.0 : 2.0) * keyframeValue(a, b, i * h);
    return d * sum * h / 3.0;
}

// Antiderivative of the line, zero at the first point.
double antiderivative(const std::vector<Keyframe>& pts, double t)
{
    const Keyframe& first = pts.front();
    if (t <= double(first.time))
        return first.value * (t - double(first.time));
    double acc = 0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const Keyframe& a = pts[i];
        const Keyframe& b = pts[i + 1];
        if (b.time <= a.time)
            continue;
        if (t < double(b.time))
            return acc + segmentArea(a, b, (t - double(a.time)) / double(b.time - a.time));
        acc += segmentArea(a, b, 1.0);
    }
    return acc + pts.back().value * (t - double(pts.back().time));
}

} // namespace

double Envelope::integral(double t) const
{
    if (points.empty())
        return value * t;
    return antiderivative(points, t) - antiderivative(points, 0.0);
}

double Envelope::maxIntegral(double t) const
{
    if (t <= 0)
        return 0.0;
    double best = std::max(0.0, integral(t));
    // The integral peaks where the line crosses from forwards to backwards.
    auto consider = [&](double x) {
        if (x > 0 && x < t)
            best = std::max(best, integral(x));
    };
    for (size_t i = 0; i < points.size(); ++i) {
        consider(double(points[i].time));
        if (i + 1 >= points.size())
            continue;
        const Keyframe& a = points[i];
        const Keyframe& b = points[i + 1];
        if (a.interp == Keyframe::Interp::Hold)
            continue;
        // Wherever the line crosses from forwards to backwards (Bezier, Bounce and Elastic can
        // cross more than once, and between keys of the same sign).
        const int n = 64;
        for (int s = 0; s < n; ++s) {
            double lo = double(s) / n, hi = double(s + 1) / n;
            if (!(keyframeValue(a, b, lo) > 0 && keyframeValue(a, b, hi) <= 0))
                continue;
            for (int k = 0; k < 40; ++k) {
                const double mid = (lo + hi) / 2;
                (keyframeValue(a, b, mid) > 0 ? lo : hi) = mid;
            }
            consider(double(a.time) + (lo + hi) / 2 * double(b.time - a.time));
        }
    }
    return best;
}

TimeUs Clip::sourceSpan() const
{
    if (!velocity.active)
        return TimeUs(double(length) * speed + 0.5);
    return TimeUs(speed * velocity.maxIntegral(double(length)) + 0.5);
}

TimeUs Clip::sourceTimeAt(TimeUs t) const
{
    TimeUs s;
    if (!velocity.active) {
        const double local = double(t - start) * speed;
        s = !reverse ? sourceIn + TimeUs(local) : sourceIn + std::max<TimeUs>(0, TimeUs(double(length) * speed - local) - 1);
    } else {
        const double span = double(sourceSpan());
        const double off = std::clamp(rampOffset(double(t - start)), 0.0, span);
        s = !reverse ? sourceIn + TimeUs(std::llround(off)) : sourceIn + std::max<TimeUs>(0, TimeUs(std::llround(span - off)) - 1);
    }
    if (loopLength > 0) // round again from the start of the file
        s = ((s % loopLength) + loopLength) % loopLength;
    return s;
}

ClipEnvelope effectiveEnvelope(const Track& track, size_t i)
{
    const Clip& c = track.clips[i];
    ClipEnvelope env{c.fadeIn, c.fadeOut};
    if (i > 0) {
        const TimeUs overlap = std::min(track.clips[i - 1].end() - c.start, c.length);
        if (overlap > 0)
            env.fadeIn = std::max(env.fadeIn, overlap);
    }
    if (i + 1 < track.clips.size()) {
        const TimeUs overlap = std::min(c.end() - track.clips[i + 1].start, c.length);
        if (overlap > 0)
            env.fadeOut = std::max(env.fadeOut, overlap);
    }
    return env;
}

double fadeShape(int curve, double x)
{
    x = std::clamp(x, 0.0, 1.0);
    switch (FadeCurve(curve)) {
    case FadeCurve::Smooth: return x * x * (3.0 - 2.0 * x);
    case FadeCurve::Fast: return 1.0 - (1.0 - x) * (1.0 - x);   // rises quickly, then eases in
    case FadeCurve::Slow: return x * x;                         // starts slowly
    case FadeCurve::ConstantPower: return std::sin(x * 1.5707963267948966); // equal-power crossfades
    case FadeCurve::Linear: break;
    }
    return x;
}

QString fadeCurveName(int curve)
{
    static const char* names[kFadeCurves] = {"Linear", "Smooth (S-curve)", "Fast", "Slow", "Constant Power"};
    return QString::fromLatin1(names[std::clamp(curve, 0, kFadeCurves - 1)]);
}

int matteAbove(const Timeline& tl, int ti)
{
    for (int i = ti - 1; i >= 0; --i) {
        const Track& t = tl.tracks[size_t(i)];
        if (isVisualTrack(t.type))
            return t.matte && !t.muted ? i : -1;
    }
    return -1;
}

int coveringTrack(const ProjectSnapshot& snap, TimeUs t)
{
    const auto& tracks = snap.timeline.tracks;
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(), [](const Track& tr) { return isVisualTrack(tr.type) && tr.solo; });
    const double shape = double(snap.settings.width) / std::max(1, snap.settings.height);
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const Track& tr = tracks[size_t(ti)];
        if (!isVisualTrack(tr.type) || tr.muted || (anySolo && !tr.solo) || tr.matte || matteAbove(snap.timeline, ti) >= 0)
            continue;
        const Clip* only = nullptr;
        int count = 0;
        for (const Clip& c : tr.clips) {
            if (c.start > t)
                break;
            if (!c.muted && t < c.end()) {
                only = &c;
                ++count;
            }
        }
        if (count != 1)
            continue;
        const Clip& c = *only;
        if (isAdjustment(c) || tr.blend != 0 || tr.compositeAt(t) < 0.999 || std::any_of(tr.effects.begin(), tr.effects.end(), [](const EffectInstance& e) { return e.enabled; }))
            continue;
        if (c.isGenerated() || c.isSequence() || c.gain < 0.999f || !c.panCrop.empty() || c.stabilize > 0
            || std::any_of(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) { return e.enabled; })
            || t < c.start + c.fadeIn || t >= c.end() - c.fadeOut)
            continue;
        const auto m = snap.media.find(c.mediaId);
        if (m == snap.media.end() || !m->second)
            continue;
        const auto& video = m->second->info.video;
        const auto v = std::find_if(video.begin(), video.end(), [&](const VideoStreamInfo& s) { return s.index == c.streamIndex; });
        if (v == video.end() || v->hasAlpha || m->second->info.isStill)
            continue;
        const QSize d = v->displaySize();
        if (d.isEmpty() || std::abs(double(d.width()) / d.height() - shape) > shape * 0.005)
            continue;
        return ti;
    }
    return -1;
}

double envelopeGain(const Clip& c, const ClipEnvelope& env, TimeUs t)
{
    double g = 1.0;
    if (env.fadeIn > 0 && t < c.start + env.fadeIn)
        g *= fadeShape(c.fadeInCurve, double(t - c.start) / double(env.fadeIn));
    if (env.fadeOut > 0 && t > c.end() - env.fadeOut)
        g *= fadeShape(c.fadeOutCurve, double(c.end() - t) / double(env.fadeOut));
    return std::clamp(g, 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

// A timeline as the undo history keeps it: what it has in common with the
// state it was packed against (events, tracks, the rest) is shared, not
// copied, so a step costs only what the edit changed (a big project's
// thousands of events would otherwise be copied twice for every edit).
struct PackedTimeline {
    struct PackedTrack {
        std::shared_ptr<const Track> header; // without its events
        std::vector<std::shared_ptr<const Clip>> clips;
    };
    std::shared_ptr<const Timeline> rest; // without its tracks
    std::vector<PackedTrack> tracks;

    static std::shared_ptr<const PackedTimeline> pack(Timeline tl, const PackedTimeline* like)
    {
        auto p = std::make_shared<PackedTimeline>();
        std::unordered_map<int, std::shared_ptr<const Clip>> clips; // like's, by id
        std::unordered_map<int, std::shared_ptr<const Track>> headers;
        if (like)
            for (const PackedTrack& t : like->tracks) {
                headers.emplace(t.header->id, t.header);
                for (const auto& c : t.clips)
                    clips.emplace(c->id, c);
            }
        for (Track& t : tl.tracks) {
            PackedTrack pt;
            pt.clips.reserve(t.clips.size());
            for (Clip& c : t.clips) {
                const auto it = clips.find(c.id);
                pt.clips.push_back(it != clips.end() && *it->second == c ? it->second : std::make_shared<const Clip>(std::move(c)));
            }
            t.clips.clear();
            const auto it = headers.find(t.id);
            pt.header = it != headers.end() && *it->second == t ? it->second : std::make_shared<const Track>(std::move(t));
            p->tracks.push_back(std::move(pt));
        }
        tl.tracks.clear();
        p->rest = like && *like->rest == tl ? like->rest : std::make_shared<const Timeline>(std::move(tl));
        return p;
    }

    Timeline unpack() const
    {
        Timeline tl = *rest;
        tl.tracks.reserve(tracks.size());
        for (const PackedTrack& pt : tracks) {
            Track t = *pt.header;
            t.clips.reserve(pt.clips.size());
            for (const auto& c : pt.clips)
                t.clips.push_back(*c);
            tl.tracks.push_back(std::move(t));
        }
        return tl;
    }
};

namespace {

class TimelineEditCommand : public QUndoCommand {
public:
    TimelineEditCommand(Project* p, std::shared_ptr<const PackedTimeline> before, std::shared_ptr<const PackedTimeline> after,
                        const QString& text)
        : QUndoCommand(text), m_project(p), m_before(std::move(before)), m_after(std::move(after))
    {
    }
    void undo() override { m_project->restorePacked(m_before); }
    void redo() override
    {
        if (m_firstRedo) { // already applied when pushed
            m_firstRedo = false;
            return;
        }
        m_project->restorePacked(m_after);
    }

private:
    Project* m_project;
    std::shared_ptr<const PackedTimeline> m_before;
    std::shared_ptr<const PackedTimeline> m_after;
    bool m_firstRedo = true;
};

} // namespace

// ---------------------------------------------------------------------------
// Project
// ---------------------------------------------------------------------------

Project::Project(QObject* parent) : QObject(parent), m_timeline(defaultTimeline())
{
}

void Project::touch()
{
    m_snapshot.reset();
    emit changed();
}

void Project::setSettings(const ProjectSettings& s)
{
    if (s == m_settings)
        return;
    m_settings = s;
    m_dirtyExtra = true;
    m_snapshot.reset();
    emit settingsChanged();
    emit changed();
}

std::shared_ptr<const MediaItem> Project::mediaItem(int id) const
{
    auto it = m_media.find(id);
    return it == m_media.end() ? nullptr : it->second;
}

int Project::addMedia(const MediaInfo& info)
{
    // QFileInfo equality can treat distinct missing files as the same file.
    // Offline items still need separate identities so imports cannot alias them.
    auto identity = [](const QString& path) {
        const QFileInfo file(path);
        const QString canonical = file.canonicalFilePath();
        return QDir::cleanPath(canonical.isEmpty() ? file.absoluteFilePath() : canonical);
    };
    const QString incoming = identity(info.path);
#ifdef Q_OS_WIN
    constexpr auto pathCase = Qt::CaseInsensitive;
#else
    constexpr auto pathCase = Qt::CaseSensitive;
#endif
    for (const auto& [id, m] : m_media)
        if (identity(m->path).compare(incoming, pathCase) == 0)
            return id;
    auto item = std::make_shared<MediaItem>();
    item->id = m_nextMediaId++;
    // Imported relative files are relative to the current working directory, not
    // whichever folder the project is later saved into. Keep FFmpeg URLs intact.
    const bool url = QUrl(info.path).scheme().size() > 1 && !QDir::isAbsolutePath(info.path);
    item->path = info.path.isEmpty() || url ? info.path : QFileInfo(info.path).absoluteFilePath();
    item->info = info;
    item->info.path = item->path;
    m_media[item->id] = item;
    m_dirtyExtra = true;
    m_snapshot.reset();
    mediaChangedNow();
    return item->id;
}

void Project::setMediaDetails(int id, const QString& folder, int rating, TimeUs markIn, TimeUs markOut, const QString& origin)
{
    auto it = m_media.find(id);
    if (it == m_media.end())
        return;
    auto item = std::make_shared<MediaItem>(*it->second);
    item->folder = folder.trimmed();
    if (!origin.isEmpty()) item->origin = origin;
    item->rating = std::clamp(rating, 0, 5);
    item->markIn = std::max<TimeUs>(0, markIn);
    item->markOut = markOut > item->markIn ? markOut : 0;
    it->second = std::move(item);
    m_dirtyExtra = true;
    m_snapshot.reset();
    mediaChangedNow();
}

bool Project::isMediaUsed(int id) const
{
    std::function<bool(const Timeline&)> used = [&](const Timeline& timeline) {
        for (const auto& t : timeline.tracks) for (const auto& c : t.clips)
            if (c.mediaId == id || (c.isSequence() && used(*c.sequence.value))) return true;
            else for(auto value:c.narration["takes"].toArray())if(value.toObject()["media"].toInt()==id)return true;
        return false;
    };
    return used(m_timeline);
}

bool Project::removeMedia(int id)
{
    if (isMediaUsed(id) || !m_media.erase(id))
        return false;
    m_dirtyExtra = true;
    m_snapshot.reset();
    mediaChangedNow();
    return true;
}

std::vector<int> Project::offlineMedia() const
{
    std::vector<int> out;
    for (const auto& [id, m] : m_media)
        if (!m->info.ok())
            out.push_back(id);
    return out;
}

bool Project::relinkMedia(int id, const QString& path, QString* error)
{
    auto it = m_media.find(id);
    if (it == m_media.end()) {
        if (error)
            *error = QStringLiteral("unknown media");
        return false;
    }
    MediaInfo info = probeMedia(path);
    if (!info.ok()) {
        if (error)
            *error = info.error.isEmpty() ? QStringLiteral("no usable streams") : info.error;
        return false;
    }
    // Events whose stream the new file lacks switch to its first stream of that kind.
    Timeline tl = m_timeline;
    bool streamsChanged = false;
    std::function<void(Timeline&)> remapStreams=[&](Timeline& timeline) {
    for (auto& t : timeline.tracks)
        for (auto& c : t.clips) {
            if(c.isSequence()) { Timeline child=*c.sequence.value; remapStreams(child); c.sequence.value=std::make_shared<const Timeline>(std::move(child)); }
            if (c.mediaId != id)
                continue;
            if (isVisualTrack(t.type) && !info.videoStream(c.streamIndex) && info.hasVideo()) {
                c.streamIndex = info.video.front().index;
                streamsChanged = true;
            } else if (t.type == TrackType::Audio && !info.audioStream(c.streamIndex) && info.hasAudio()) {
                c.streamIndex = info.audio.front().index;
                streamsChanged = true;
            }
        }
    }; remapStreams(tl);
    auto item = std::make_shared<MediaItem>(*it->second);
    item->path = info.path.isEmpty() ? path : info.path;
    item->info = std::move(info);
    ++item->revision;
    it->second = std::move(item);
    m_dirtyExtra = true;
    m_snapshot.reset();
    emit mediaRelinked(id);
    mediaChangedNow();
    if (streamsChanged)
        applyEdit(tl, QStringLiteral("Relink Media"));
    else
        touch();
    return true;
}

void Project::setTimelineLive(const Timeline& t)
{
    if (t == m_timeline)
        return;
    m_timeline = t;
    touch();
}

void Project::restoreTimeline(const Timeline& t)
{
    m_timeline = t;
    touch();
}

void Project::restorePacked(const std::shared_ptr<const PackedTimeline>& t)
{
    m_timeline = t->unpack();
    m_packed = t;
    touch();
}

void Project::commitEdit(const Timeline& before, const QString& description)
{
    if (before == m_timeline)
        return;
    // Lip sync follows a voice track's edits (drags included), in this same undo step.
    if (graphics::trackBindingsStale(m_timeline)) {
        Timeline followed = m_timeline;
        graphics::followTrackBindings(followed, m_media);
        setTimelineLive(followed);
    }
    auto packedBefore = PackedTimeline::pack(before, m_packed.get());
    m_packed = PackedTimeline::pack(m_timeline, packedBefore.get());
    m_undo.push(new TimelineEditCommand(this, std::move(packedBefore), m_packed, description));
}

void Project::applyEdit(const Timeline& after, const QString& description)
{
    const Timeline before = m_timeline;
    setTimelineLive(after);
    commitEdit(before, description);
}

std::shared_ptr<const ProjectSnapshot> Project::snapshot() const
{
    if (!m_snapshot) {
        auto s = std::make_shared<ProjectSnapshot>();
        s->session = m_session;
        s->settings = m_settings;
        s->media = m_media;
        s->timeline = m_timeline;
        m_snapshot = std::move(s);
    }
    return m_snapshot;
}

void Project::clear(const ProjectSettings& settings)
{
    m_session = std::make_shared<const int>(0);
    m_undo.clear();
    m_packed.reset();
    m_media.clear();
    m_nextMediaId = 1;
    m_settings = settings;
    m_timeline = defaultTimeline();
    m_filePath.clear();
    m_portablePaths = false;
    m_deliver = {};
    m_dirtyExtra = false;
    m_snapshot.reset();
    mediaChangedNow();
    emit settingsChanged();
    emit changed();
}

// ---------------------------------------------------------------------------
// Persistence (JSON)
// ---------------------------------------------------------------------------

namespace {


QJsonArray chainToJson(const EffectChain& chain)
{
    QJsonArray a;
    for (const auto& e : chain)
        a.append(effectToJson(e));
    return a;
}

EffectChain chainFromJson(const QJsonValue& v)
{
    EffectChain chain;
    for (const auto& e : v.toArray())
        chain.push_back(effectFromJson(e.toObject()));
    return chain;
}

QJsonObject envelopeToJson(const Envelope& e)
{
    QJsonObject o;
    o[QStringLiteral("active")] = e.active;
    o[QStringLiteral("value")] = e.value;
    keysToJson(e.points, o, QStringLiteral("points"));
    return o;
}

Envelope envelopeFromJson(const QJsonValue& v, double neutral)
{
    Envelope e;
    e.value = neutral;
    if (!v.isObject())
        return e;
    const QJsonObject o = v.toObject();
    e.active = o[QStringLiteral("active")].toBool();
    e.value = o[QStringLiteral("value")].toDouble(neutral);
    e.points = keysFromJson(o, QStringLiteral("points"));
    return e;
}

QJsonObject clipToJson(const Clip& c)
{
    QJsonObject o;
    o[QStringLiteral("id")] = c.id;
    o[QStringLiteral("media")] = c.mediaId;
    o[QStringLiteral("stream")] = c.streamIndex;
    o[QStringLiteral("start")] = QString::number(c.start);
    o[QStringLiteral("length")] = QString::number(c.length);
    o[QStringLiteral("sourceIn")] = QString::number(c.sourceIn);
    o[QStringLiteral("fadeIn")] = QString::number(c.fadeIn);
    o[QStringLiteral("fadeOut")] = QString::number(c.fadeOut);
    o[QStringLiteral("gain")] = c.gain;
    if (c.fadeInCurve)
        o[QStringLiteral("fadeInCurve")] = c.fadeInCurve;
    if (c.fadeOutCurve)
        o[QStringLiteral("fadeOutCurve")] = c.fadeOutCurve;
    o[QStringLiteral("group")] = c.groupId;
    o[QStringLiteral("muted")] = c.muted;
    o[QStringLiteral("name")] = c.name;
    if (!c.effects.empty())
        o[QStringLiteral("effects")] = chainToJson(c.effects);
    if (!c.transitionIn.type.isEmpty())
        o[QStringLiteral("transitionIn")] = transitionToJson(c.transitionIn);
    if (!c.transitionOut.type.isEmpty())
        o[QStringLiteral("transitionOut")] = transitionToJson(c.transitionOut);
    if (c.isSequence()) o[QStringLiteral("sequence")] = nestedTimelineToJson(*c.sequence.value);
    if (!c.narration.isEmpty()) o[QStringLiteral("narration")] = c.narration;
    if (c.isGenerated())
        o[QStringLiteral("generator")] = generatorToJson(c.generator);
    if (!c.panCrop.empty())
        o[QStringLiteral("panCrop")] = paramsToJson(c.panCrop);
    if (c.speed != 1.0)
        o[QStringLiteral("speed")] = c.speed;
    if (c.reverse)
        o[QStringLiteral("reverse")] = true;
    if (!c.preservePitch)
        o[QStringLiteral("preservePitch")] = false;
    if (c.frameBlend)
        o[QStringLiteral("frameBlend")] = true;
    if (c.opticalFlow)
        o[QStringLiteral("opticalFlow")] = true;
    if (c.stabilize > 0) {
        o[QStringLiteral("stabilize")] = c.stabilize;
        o[QStringLiteral("stabilizeZoom")] = c.stabilizeZoom;
    }
    if (c.denoise > 0)
        o[QStringLiteral("denoise")] = c.denoise;
    if (c.loopLength > 0)
        o[QStringLiteral("loopLength")] = QString::number(c.loopLength);
    if (c.velocity.active || !c.velocity.points.empty())
        o[QStringLiteral("velocityEnvelope")] = envelopeToJson(c.velocity);
    return o;
}

// Times in files are within ±kMaxFileTime (11.5 days): far beyond any
// project, and sums and sample counts (t * rate) can't overflow.
constexpr TimeUs kMaxFileTime = 1'000'000'000'000;

TimeUs jsonTime(const QJsonValue& v)
{
    const double t = v.isString() ? double(v.toString().toLongLong()) : v.toDouble();
    return std::isfinite(t) && std::abs(t) <= double(kMaxFileTime) ? TimeUs(t) : 0; // out of range: damaged
}

double finiteOr(double v, double fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// A damaged or hand-edited file can hold what the rest of the program counts
// on never happening: events without length, ids used twice (a new item
// would share its id with an old one), values out of range. Repairs them.
void repairTimeline(Timeline& tl)
{
    constexpr int kMaxId = std::numeric_limits<int>::max() / 2; // room to allocate more
    std::vector<int*> ids; // must be unique
    int maxId = 0;
    auto note = [&](int id) { maxId = std::max(maxId, id < kMaxId ? id : 0); };
    std::function<void(EffectChain&, bool)> chainIds = [&](EffectChain& chain, bool unique) {
        for (EffectInstance& e : chain) {
            note(e.id);
            if (unique)
                ids.push_back(&e.id);
            for (RackChain& rc : e.chains)
                chainIds(rc.effects, unique);
        }
    };
    auto clampEnvelope = [](Envelope& e, double lo, double hi) {
        e.value = std::clamp(finiteOr(e.value, lo), lo, hi);
        for (Keyframe& k : e.points)
            k.value = std::clamp(finiteOr(k.value, lo), lo, hi);
    };
    for (Track& t : tl.tracks) {
        clampEnvelope(t.volume, 0.0, 2.0);
        clampEnvelope(t.panEnvelope, -1.0, 1.0);
        clampEnvelope(t.composite, 0.0, 1.0);
        t.level = float(std::clamp(finiteOr(t.level, 1.0), 0.0, 4.0));
        t.pan = float(std::clamp(finiteOr(t.pan, 0.0), -1.0, 1.0));
        std::erase_if(t.clips, [](const Clip& c) { return c.length <= 0; });
        for (Clip& c : t.clips) {
            c.start = std::max<TimeUs>(0, c.start);
            c.sourceIn = std::max<TimeUs>(0, c.sourceIn);
            c.fadeIn = std::clamp<TimeUs>(c.fadeIn, 0, c.length);
            c.fadeOut = std::clamp<TimeUs>(c.fadeOut, 0, c.length);
            c.gain = float(std::clamp(finiteOr(c.gain, 1.0), 0.0, 16.0));
            if (c.groupId < 0 || c.groupId >= kMaxId)
                c.groupId = 0;
            note(c.id);
            note(c.groupId);
            ids.push_back(&c.id);
            chainIds(c.effects, true);
        }
        note(t.id);
        ids.push_back(&t.id);
        chainIds(t.effects, true);
    }
    for (Marker& m : tl.markers) {
        note(m.id);
        ids.push_back(&m.id);
    }
    for (EffectChain* chain : {&tl.outputEffects, &tl.audioMasterEffects, &tl.liveEffects})
        chainIds(*chain, true);
    for (EffectChain& chain : tl.liveLayerEffects)
        chainIds(chain, true);
    for (LivePad& pad : tl.livePads)
        chainIds(pad.effects, true);
    for (LiveScene& scene : tl.liveScenes) { // copies of the Live chains: their ids repeat on purpose
        chainIds(scene.liveEffects, false);
        for (LiveLayer& layer : scene.layers)
            chainIds(layer.effects, false);
    }
    tl.nextId = std::clamp(tl.nextId, maxId + 1, kMaxId);
    std::map<int, int> occurrences;
    std::vector<std::pair<int, int*>> originals;
    for (int* id : ids) { ++occurrences[*id]; originals.emplace_back(*id, id); }
    std::set<int> seen;
    for (int* id : ids)
        if (*id <= 0 || *id >= kMaxId || !seen.insert(*id).second)
            *id = tl.nextId++;
    std::map<int, int> remapped;
    for (auto [old, id] : originals)
        if (old > 0 && occurrences[old] == 1) remapped[old] = *id;
    std::set<int> audioTracks, clips, markers;
    for (const auto& t : tl.tracks) {
        if (t.type == TrackType::Audio) audioTracks.insert(t.id);
        for (const auto& c : t.clips) clips.insert(c.id);
    }
    for (const auto& m : tl.markers) markers.insert(m.id);
    auto reference = [&](int old, const std::set<int>& targets) {
        auto it = remapped.find(old);
        return it != remapped.end() && targets.count(it->second) ? it->second : 0;
    };
    for (auto& t : tl.tracks) {
        t.duckUnder = reference(t.duckUnder, audioTracks);
        if (t.duckUnder == t.id) t.duckUnder = 0;
        if (!t.duckUnder) t.duckAmount = -12.0f; // inactive ducking is omitted when saved
        for (auto& c : t.clips) if (c.narration.contains("caption_owner"))
            c.narration["caption_owner"] = reference(c.narration["caption_owner"].toInt(), clips);
    }
    for (int i = 0; i < tl.chapters.size(); ++i) {
        auto chapter = tl.chapters[i].toObject();
        if (int id = reference(chapter["id"].toInt(), markers)) chapter["id"] = id;
        for (const auto& key : {"clips", "stretch"}) {
            QJsonArray repaired; std::set<int> added;
            for (auto value : chapter[key].toArray())
                if (int id = reference(value.toInt(), clips); id && added.insert(id).second) repaired.append(id);
            if (chapter.contains(key)) chapter[key] = repaired;
        }
        tl.chapters[i] = chapter;
    }
}

Clip clipFromJson(const QJsonObject& o)
{
    Clip c;
    c.narration = o[QStringLiteral("narration")].toObject();
    if (o.contains(QStringLiteral("sequence"))) {
        static thread_local int depth = 0;
        if (depth < 16) {
            ++depth;
            c.sequence.value = std::make_shared<const Timeline>(nestedTimelineFromJson(o[QStringLiteral("sequence")].toObject()));
            --depth;
        }
    }
    c.id = o[QStringLiteral("id")].toInt();
    c.mediaId = o[QStringLiteral("media")].toInt();
    c.streamIndex = o[QStringLiteral("stream")].toInt(-1);
    c.start = jsonTime(o[QStringLiteral("start")]);
    c.length = jsonTime(o[QStringLiteral("length")]);
    c.sourceIn = jsonTime(o[QStringLiteral("sourceIn")]);
    c.fadeIn = jsonTime(o[QStringLiteral("fadeIn")]);
    c.fadeOut = jsonTime(o[QStringLiteral("fadeOut")]);
    c.gain = float(o[QStringLiteral("gain")].toDouble(1.0));
    c.fadeInCurve = std::clamp(o[QStringLiteral("fadeInCurve")].toInt(0), 0, kFadeCurves - 1);
    c.fadeOutCurve = std::clamp(o[QStringLiteral("fadeOutCurve")].toInt(0), 0, kFadeCurves - 1);
    c.groupId = o[QStringLiteral("group")].toInt();
    c.muted = o[QStringLiteral("muted")].toBool();
    c.name = o[QStringLiteral("name")].toString();
    c.effects = chainFromJson(o[QStringLiteral("effects")]);
    c.transitionIn = transitionFromJson(o[QStringLiteral("transitionIn")].toObject());
    c.transitionOut = transitionFromJson(o[QStringLiteral("transitionOut")].toObject());
    c.generator = generatorFromJson(o[QStringLiteral("generator")].toObject());
    if (c.generator.isEmpty())
        c.generator = {}; // settings without a type are never saved
    if (c.generator.type == QLatin1String("graphics_overlay") && c.generator.data.value(QStringLiteral("version")).toInt() == 2)
        c.generator.type = QStringLiteral("animation"); // (animated scenes from before Animation was its own clip type)
    c.panCrop = paramsFromJson(o[QStringLiteral("panCrop")].toObject());
    c.speed = std::clamp(o[QStringLiteral("speed")].toDouble(1.0), 0.01, 100.0);
    c.reverse = o[QStringLiteral("reverse")].toBool(false);
    c.preservePitch = o[QStringLiteral("preservePitch")].toBool(true);
    c.frameBlend = o[QStringLiteral("frameBlend")].toBool(false);
    c.opticalFlow = o[QStringLiteral("opticalFlow")].toBool(false);
    c.stabilize = std::clamp(o[QStringLiteral("stabilize")].toDouble(0), 0.0, 10.0);
    c.stabilizeZoom = o[QStringLiteral("stabilizeZoom")].toBool(true);
    c.denoise = std::clamp(o[QStringLiteral("denoise")].toDouble(0), 0.0, 1.0);
    c.loopLength = std::max<TimeUs>(0, jsonTime(o[QStringLiteral("loopLength")]));
    c.velocity = envelopeFromJson(o[QStringLiteral("velocityEnvelope")], 1.0);
    for (auto& k : c.velocity.points)
        k.value = std::clamp(k.value, kMinVelocity, kMaxVelocity);
    c.velocity.value = std::clamp(c.velocity.value, kMinVelocity, kMaxVelocity);
    return c;
}

} // namespace

QJsonArray livePadsToJson(const LivePads& pads)
{
    QJsonArray a;
    for (const LivePad& pad : pads) {
        QJsonObject o;
        if (!pad.name.isEmpty())
            o[QStringLiteral("name")] = pad.name;
        if (!pad.effects.empty())
            o[QStringLiteral("effects")] = chainToJson(pad.effects);
        if (pad.locked)
            o[QStringLiteral("locked")] = true;
        if (pad.momentary)
            o[QStringLiteral("momentary")] = true;
        a.append(o);
    }
    return a;
}

QJsonArray liveOverlaysToJson(const std::vector<LiveOverlay>& overlays)
{
    QJsonArray a;
    for (const LiveOverlay& ov : overlays) {
        QJsonObject o;
        o[QStringLiteral("kind")] = int(ov.kind);
        o[QStringLiteral("enabled")] = ov.enabled;
        if (!ov.text.isEmpty())
            o[QStringLiteral("text")] = ov.text;
        if (!ov.image.isEmpty())
            o[QStringLiteral("image")] = ov.image;
        o[QStringLiteral("anchor")] = ov.anchor;
        o[QStringLiteral("size")] = ov.size;
        o[QStringLiteral("width")] = ov.width;
        o[QStringLiteral("color")] = ov.color;
        o[QStringLiteral("opacity")] = ov.opacity;
        if (ov.pulse)
            o[QStringLiteral("pulse")] = true;
        a.append(o);
    }
    return a;
}

std::vector<LiveOverlay> liveOverlaysFromJson(const QJsonArray& a)
{
    std::vector<LiveOverlay> out;
    for (const auto& v : a) {
        const QJsonObject o = v.toObject();
        LiveOverlay ov;
        ov.kind = LiveOverlay::Kind(std::clamp(o[QStringLiteral("kind")].toInt(), 0, int(LiveOverlay::Kind::Circle)));
        ov.enabled = o[QStringLiteral("enabled")].toBool(true);
        ov.text = o[QStringLiteral("text")].toString();
        ov.image = o[QStringLiteral("image")].toString();
        ov.anchor = std::clamp(o[QStringLiteral("anchor")].toInt(2), 0, 8);
        ov.size = std::clamp(o[QStringLiteral("size")].toDouble(8), 1.0, 100.0);
        ov.width = std::clamp(o[QStringLiteral("width")].toDouble(60), 5.0, 100.0);
        ov.color = o[QStringLiteral("color")].toInt(0xffffff) & 0xffffff;
        ov.opacity = std::clamp(o[QStringLiteral("opacity")].toDouble(1), 0.0, 1.0);
        ov.pulse = o[QStringLiteral("pulse")].toBool();
        out.push_back(ov);
    }
    return out;
}

QString liveLayerKey(const QString& name, int layer)
{
    return layer == 0 ? name : name + QString::number(layer + 1);
}

QJsonArray liveScenesToJson(const LiveScenes& scenes)
{
    QJsonArray a;
    for (const LiveScene& s : scenes) {
        QJsonObject o;
        if (s.used) {
            o[QStringLiteral("name")] = s.name;
            o[QStringLiteral("liveEffects")] = chainToJson(s.liveEffects);
            o[QStringLiteral("pad")] = s.pad;
            for (int i = 0; i < kLiveLayers; ++i) { // ("layerSource", "layer2Source"...)
                const LiveLayer& l = s.layers[size_t(i)];
                auto key = [i](const char* what) { return liveLayerKey(QStringLiteral("layer"), i) + QLatin1String(what); };
                if (i > 0 && l == LiveLayer())
                    continue;
                o[key("Source")] = l.source;
                o[key("Mode")] = int(l.settings.mode);
                o[key("Corner")] = l.settings.corner;
                if (l.settings.corner == 5) {
                    o[key("X")] = l.settings.x;
                    o[key("Y")] = l.settings.y;
                }
                o[key("Size")] = l.settings.size;
                o[key("Opacity")] = l.settings.opacity;
                o[key("Blend")] = l.settings.blend;
                if (l.settings.noEffects)
                    o[key("NoEffects")] = true;
                o[key("Effects")] = chainToJson(l.effects);
            }
            o[QStringLiteral("overlays")] = liveOverlaysToJson(s.overlays);
        }
        a.append(o);
    }
    return a;
}

QJsonArray liveSetlistToJson(const std::vector<LiveSong>& songs)
{
    QJsonArray a;
    for (const LiveSong& s : songs) {
        QJsonObject o;
        o[QStringLiteral("name")] = s.name;
        if (!s.padSet.isEmpty())
            o[QStringLiteral("padSet")] = s.padSet;
        o[QStringLiteral("bpm")] = s.bpm;
        o[QStringLiteral("lockBpm")] = s.lockBpm;
        o[QStringLiteral("scene")] = s.scene;
        if (!s.notes.isEmpty())
            o[QStringLiteral("notes")] = s.notes;
        a.append(o);
    }
    return a;
}

std::vector<LiveSong> liveSetlistFromJson(const QJsonArray& a)
{
    std::vector<LiveSong> songs;
    for (const QJsonValue& v : a) {
        const QJsonObject o = v.toObject();
        LiveSong s;
        s.name = o[QStringLiteral("name")].toString();
        s.padSet = o[QStringLiteral("padSet")].toString();
        s.bpm = std::clamp(o[QStringLiteral("bpm")].toDouble(), 0.0, 400.0);
        s.lockBpm = o[QStringLiteral("lockBpm")].toBool(true);
        s.scene = std::clamp(o[QStringLiteral("scene")].toInt(-1), -1, kLiveScenes - 1);
        s.notes = o[QStringLiteral("notes")].toString();
        songs.push_back(s);
    }
    return songs;
}

LiveScenes liveScenesFromJson(const QJsonArray& a)
{
    LiveScenes scenes;
    for (int i = 0; i < std::min<int>(kLiveScenes, int(a.size())); ++i) {
        const QJsonObject o = a[i].toObject();
        LiveScene& s = scenes[size_t(i)];
        s.used = o.contains(QStringLiteral("liveEffects"));
        if (!s.used)
            continue;
        s.name = o[QStringLiteral("name")].toString();
        s.liveEffects = chainFromJson(o[QStringLiteral("liveEffects")]);
        s.pad = std::clamp(o[QStringLiteral("pad")].toInt(-1), -1, kLivePads - 1);
        for (int i = 0; i < kLiveLayers; ++i) {
            LiveLayer& l = s.layers[size_t(i)];
            auto at = [&](const char* what) { return o[liveLayerKey(QStringLiteral("layer"), i) + QLatin1String(what)]; };
            l.source = at("Source").toString();
            l.settings.mode = LiveLayerSettings::Mode(std::clamp(at("Mode").toInt(), 0, 1));
            l.settings.corner = std::clamp(at("Corner").toInt(3), 0, 5);
            l.settings.x = std::clamp(at("X").toDouble(0.5), 0.0, 1.0);
            l.settings.y = std::clamp(at("Y").toDouble(0.5), 0.0, 1.0);
            l.settings.size = std::clamp(at("Size").toDouble(0.3), 0.05, 1.0);
            l.settings.opacity = std::clamp(at("Opacity").toDouble(1), 0.0, 1.0);
            l.settings.blend = std::clamp(at("Blend").toInt(), 0, 7);
            l.settings.noEffects = at("NoEffects").toBool();
            l.effects = chainFromJson(at("Effects"));
        }
        s.overlays = liveOverlaysFromJson(o[QStringLiteral("overlays")].toArray());
    }
    return scenes;
}

LivePads livePadsFromJson(const QJsonArray& a)
{
    LivePads pads;
    for (int i = 0; i < std::min<int>(kLivePads, int(a.size())); ++i) {
        const QJsonObject o = a[i].toObject();
        pads[size_t(i)].name = o[QStringLiteral("name")].toString();
        pads[size_t(i)].effects = chainFromJson(o[QStringLiteral("effects")]);
        pads[size_t(i)].locked = o[QStringLiteral("locked")].toBool();
        pads[size_t(i)].momentary = o[QStringLiteral("momentary")].toBool();
    }
    return pads;
}

QByteArray Project::toJson(const QString& baseDir, const QString& backupOf, bool backup) const
{
    QJsonObject root;
    root[QStringLiteral("format")] = QLatin1String(project_format::identifier);
    root[QStringLiteral("version")] = project_format::version;
    if (backup)
        root[QStringLiteral("autosaveOf")] = backupOf;

    QJsonObject s;
    s[QStringLiteral("width")] = m_settings.width;
    s[QStringLiteral("height")] = m_settings.height;
    s[QStringLiteral("fpsNum")] = m_settings.fps.num;
    s[QStringLiteral("fpsDen")] = m_settings.fps.den;
    s[QStringLiteral("sampleRate")] = m_settings.sampleRate;
    root[QStringLiteral("settings")] = s;

    const QDir dir(baseDir);
    QJsonArray media;
    for (const auto& [id, m] : m_media) {
        QJsonObject o;
        o[QStringLiteral("id")] = id;
        o[QStringLiteral("path")] = m->path;
        if (!baseDir.isEmpty())
            o[QStringLiteral("relativePath")] = dir.relativeFilePath(m->path);
        if (!m->folder.isEmpty())
            o[QStringLiteral("folder")] = m->folder;
        if (!m->origin.isEmpty()) o[QStringLiteral("origin")] = m->origin;
        if (m->rating)
            o[QStringLiteral("rating")] = m->rating;
        if (m->markOut > m->markIn) {
            o[QStringLiteral("markIn")] = QString::number(m->markIn);
            o[QStringLiteral("markOut")] = QString::number(m->markOut);
        }
        media.append(o);
    }
    root[QStringLiteral("media")] = media;

    QJsonArray tracks;
    for (const auto& t : m_timeline.tracks) {
        QJsonObject o;
        o[QStringLiteral("id")] = t.id;
        o[QStringLiteral("type")] = trackTypeName(t.type);
        o[QStringLiteral("name")] = t.name;
        o[QStringLiteral("muted")] = t.muted;
        o[QStringLiteral("solo")] = t.solo;
        if (t.locked)
            o[QStringLiteral("locked")] = true;
        o[QStringLiteral("level")] = t.level;
        o[QStringLiteral("pan")] = t.pan;
        if (t.blend)
            o[QStringLiteral("blend")] = t.blend;
        if (t.matte)
            o[QStringLiteral("matte")] = t.matte;
        if (t.duckUnder) {
            o[QStringLiteral("duckUnder")] = t.duckUnder;
            o[QStringLiteral("duckAmount")] = t.duckAmount;
        }
        QJsonArray clips;
        for (const auto& c : t.clips)
            clips.append(clipToJson(c));
        o[QStringLiteral("clips")] = clips;
        if (!t.effects.empty())
            o[QStringLiteral("effects")] = chainToJson(t.effects);
        if (t.volume.active || !t.volume.points.empty())
            o[QStringLiteral("volumeEnvelope")] = envelopeToJson(t.volume);
        if (t.panEnvelope.active || !t.panEnvelope.points.empty())
            o[QStringLiteral("panEnvelope")] = envelopeToJson(t.panEnvelope);
        if (t.composite.active || !t.composite.points.empty())
            o[QStringLiteral("compositeEnvelope")] = envelopeToJson(t.composite);
        tracks.append(o);
    }
    root[QStringLiteral("tracks")] = tracks;
    root["chapters"] = m_timeline.chapters;
    root["narrationSettings"] = m_timeline.narrationSettings;
    if (!m_timeline.multicam.isEmpty())
        root["multicam"] = m_timeline.multicam;
    if (!m_deliver.isEmpty())
        root[QStringLiteral("deliver")] = m_deliver;
    if (!m_timeline.markers.empty()) {
        QJsonArray markers;
        for (const Marker& m : m_timeline.markers) {
            QJsonObject o;
            o[QStringLiteral("id")] = m.id;
            o[QStringLiteral("time")] = QString::number(m.time);
            if (m.length > 0)
                o[QStringLiteral("length")] = QString::number(m.length);
            if (!m.name.isEmpty())
                o[QStringLiteral("name")] = m.name;
            if (m.color)
                o[QStringLiteral("color")] = m.color;
            markers.append(o);
        }
        root[QStringLiteral("markers")] = markers;
    }
    root[QStringLiteral("outputEffects")] = chainToJson(m_timeline.outputEffects);
    root[QStringLiteral("audioMasterEffects")] = chainToJson(m_timeline.audioMasterEffects);
    if (m_timeline.audioMasterLevel != 1.0f)
        root[QStringLiteral("audioMasterLevel")] = m_timeline.audioMasterLevel;
    if (!m_timeline.liveEffects.empty())
        root[QStringLiteral("liveEffects")] = chainToJson(m_timeline.liveEffects);
    if (std::any_of(m_timeline.livePads.begin(), m_timeline.livePads.end(), [](const LivePad& p) { return !p.isEmpty(); }))
        root[QStringLiteral("livePads")] = livePadsToJson(m_timeline.livePads);
    for (int i = 0; i < kLiveLayers; ++i) // ("liveLayerEffects", "liveLayerEffects2"...)
        if (!m_timeline.liveLayerEffects[size_t(i)].empty())
            root[liveLayerKey(QStringLiteral("liveLayerEffects"), i)] = chainToJson(m_timeline.liveLayerEffects[size_t(i)]);
    if (!m_timeline.liveOverlays.empty())
        root[QStringLiteral("liveOverlays")] = liveOverlaysToJson(m_timeline.liveOverlays);
    if (std::any_of(m_timeline.liveScenes.begin(), m_timeline.liveScenes.end(), [](const LiveScene& s) { return s.used; }))
        root[QStringLiteral("liveScenes")] = liveScenesToJson(m_timeline.liveScenes);
    if (!m_timeline.liveSetlist.empty())
        root[QStringLiteral("liveSetlist")] = liveSetlistToJson(m_timeline.liveSetlist);
    root[QStringLiteral("nextId")] = m_timeline.nextId;
    if(m_portablePaths && !baseDir.isEmpty()) {
        collection::rewriteFiles(root,[&](const QString& path,collection::Role role) {
            if(role==collection::Role::Font||role==collection::Role::Plugin||QDir::isRelativePath(path))return path;
            const QString relative=dir.relativeFilePath(QFileInfo(path).absoluteFilePath());
            return relative==".."||relative.startsWith("../")||QDir::isAbsolutePath(relative)?path:relative;
        });
        root["collection"]=QJsonObject{{"version",1},{"relativeAssets",true}};
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

QJsonObject Project::collectionDocument() const
{
    // Capture runtime references before portable-save rewriting. A relative path
    // introduced by an edit currently renders against the working directory.
    auto document=QJsonDocument::fromJson(toJson({}, {},false)).object();
    collection::rewriteFiles(document,[](const QString& path,collection::Role role) {
        return role==collection::Role::Font||role==collection::Role::Plugin||QDir::isAbsolutePath(path)
            ?path:QFileInfo(path).absoluteFilePath();
    });
    return document;
}

namespace {
bool writeFile(const QString& path, const QByteArray& data, QString* error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}
} // namespace

void Project::setDeliver(const QJsonObject& deliver, bool modified)
{
    if (deliver == m_deliver)
        return;
    m_deliver = deliver;
    m_dirtyExtra |= modified;
}

bool Project::save(const QString& path, QString* error)
{
    if (!project_format::hasExtension(path)) {
        if (error) *error = QStringLiteral("Project files must use the .mvp extension.");
        return false;
    }
    if (!writeFile(path, toJson(QFileInfo(path).absolutePath(), {}, false), error))
        return false;
    m_filePath = path;
    m_undo.setClean();
    m_dirtyExtra = false;
    return true;
}

bool Project::saveBackup(const QString& path, QString* error) const
{
    if (!project_format::hasExtension(path)) {
        if (error) *error = QStringLiteral("Project files must use the .mvp extension.");
        return false;
    }
    // Relative paths are kept relative to the project's own folder.
    const QString base = m_filePath.isEmpty() ? QString() : QFileInfo(m_filePath).absolutePath();
    return writeFile(path, toJson(base, m_filePath, true), error);
}

bool Project::recover(const QString& backupPath, QString* error, QStringList* missingMedia)
{
    QString original;
    if (!loadFile(backupPath, error, missingMedia, &original))
        return false;
    m_filePath = original;
    m_dirtyExtra = true; // not saved to its own file yet
    emit changed();
    return true;
}

bool Project::load(const QString& path, QString* error, QStringList* missingMedia)
{
    return loadFile(path, error, missingMedia, nullptr);
}

bool Project::loadFile(const QString& path, QString* error, QStringList* missingMedia, QString* backupOf)
{
    if (!project_format::hasExtension(path)) {
        if (error) *error = QStringLiteral("Project files must use the .mvp extension.");
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull()) {
        if (error)
            *error = perr.errorString();
        return false;
    }
    QJsonObject root = doc.object();
    if (root[QStringLiteral("format")].toString() != QLatin1String(project_format::identifier)) {
        if (error)
            *error = QStringLiteral("Not a Mutant Video Producer project file");
        return false;
    }

    const auto version = root[QStringLiteral("version")];
    if (!version.isDouble() || version.toDouble() != project_format::version) {
        if (error) *error = QStringLiteral("Unsupported or invalid Mutant Video Producer project version.");
        return false;
    }
    // Validate before probing media or changing any live project/undo state.
    std::set<int> mediaIds;
    for (const auto& value : root[QStringLiteral("media")].toArray()) {
        const int id = value.toObject()[QStringLiteral("id")].toInt();
        if (id <= 0 || id >= std::numeric_limits<int>::max() / 2 || !mediaIds.insert(id).second) {
            if (error) *error = QStringLiteral("Invalid or duplicate media ID %1.").arg(id);
            return false;
        }
    }
    std::function<bool(const QJsonObject&, int)> sequenceDepth = [&](const QJsonObject& content, int depth) {
        if (depth > 16) return false;
        for (auto track : content[QStringLiteral("tracks")].toArray())
            for (auto clip : track.toObject()[QStringLiteral("clips")].toArray()) {
                auto c = clip.toObject();
                if (c.contains(QStringLiteral("sequence")) && !sequenceDepth(c[QStringLiteral("sequence")].toObject(), depth + 1)) return false;
            }
        return true;
    };
    if (!sequenceDepth(root, 0)) {
        if (error) *error = QStringLiteral("Sequence nesting exceeds sixteen levels.");
        return false;
    }
    ProjectSettings settings;
    const QJsonObject s = root[QStringLiteral("settings")].toObject();
    settings.width = s[QStringLiteral("width")].toInt(1920);
    settings.height = s[QStringLiteral("height")].toInt(1080);
    settings.fps = FrameRate{s[QStringLiteral("fpsNum")].toInt(30), s[QStringLiteral("fpsDen")].toInt(1)};
    settings.sampleRate = s[QStringLiteral("sampleRate")].toInt(48000);
    // Out of range (a damaged or hand-edited file): the defaults.
    if (settings.width < 16 || settings.width > 16384 || settings.height < 16 || settings.height > 16384) {
        settings.width = 1920;
        settings.height = 1080;
    }
    if (!settings.fps.valid() || settings.fps.toDouble() > 1000)
        settings.fps = FrameRate{30, 1};
    if (settings.sampleRate < 8000 || settings.sampleRate > 384000)
        settings.sampleRate = 48000;

    // An autosaved copy keeps paths relative to the project's own folder.
    const QString original = root[QStringLiteral("autosaveOf")].toString();
    if (backupOf)
        *backupOf = original;
    const QDir dir = backupOf ? QFileInfo(original).absoluteDir() : QFileInfo(path).absoluteDir();
    collection::rewriteFiles(root,[&](const QString& asset,collection::Role role) {
        return role==collection::Role::Font||role==collection::Role::Plugin||QDir::isAbsolutePath(asset)
            ?asset:QDir::cleanPath(dir.absoluteFilePath(asset));
    });
    MediaMap media;
    int nextMediaId = 1;
    for (const auto& v : root[QStringLiteral("media")].toArray()) {
        const QJsonObject o = v.toObject();
        auto item = std::make_shared<MediaItem>();
        item->id = o[QStringLiteral("id")].toInt();
        item->path = o[QStringLiteral("path")].toString();
        if (!QFileInfo::exists(item->path) && o.contains(QStringLiteral("relativePath")) && !(backupOf && original.isEmpty())) {
            const QString rel = dir.absoluteFilePath(o[QStringLiteral("relativePath")].toString());
            if (QFileInfo::exists(rel))
                item->path = rel;
        }
        item->info = probeMedia(item->path);
        item->folder = o[QStringLiteral("folder")].toString();
        item->origin = o[QStringLiteral("origin")].toString();
        if (item->origin.isEmpty() && item->folder == QLatin1String("YouTube")) item->origin = QStringLiteral("youtube");
        item->rating = std::clamp(o[QStringLiteral("rating")].toInt(), 0, 5);
        item->markIn = jsonTime(o[QStringLiteral("markIn")]);
        item->markOut = jsonTime(o[QStringLiteral("markOut")]);
        if (!item->info.ok() && missingMedia)
            missingMedia->append(item->path);
        nextMediaId = std::max(nextMediaId, item->id + 1);
        media[item->id] = item;
    }

    Timeline timeline;
    timeline.chapters = root["chapters"].toArray();
    timeline.narrationSettings = root["narrationSettings"].toObject();
    timeline.multicam = root["multicam"].toObject();
    for (const auto& v : root[QStringLiteral("tracks")].toArray()) {
        const QJsonObject o = v.toObject();
        Track t;
        t.id = o[QStringLiteral("id")].toInt();
        const QString type = o[QStringLiteral("type")].toString();
        t.type = type == QLatin1String("image") ? TrackType::Image
               : type == QLatin1String("audio") ? TrackType::Audio : TrackType::Video;
        t.name = o[QStringLiteral("name")].toString();
        t.muted = o[QStringLiteral("muted")].toBool();
        t.solo = o[QStringLiteral("solo")].toBool();
        t.locked = o[QStringLiteral("locked")].toBool();
        t.level = float(o[QStringLiteral("level")].toDouble(1.0));
        t.pan = float(o[QStringLiteral("pan")].toDouble(0.0));
        t.blend = std::clamp(o[QStringLiteral("blend")].toInt(0), 0, 7);
        t.matte = std::clamp(o[QStringLiteral("matte")].toInt(0), 0, int(MatteLumaInverted));
        t.duckUnder = o[QStringLiteral("duckUnder")].toInt(0);
        if (t.duckUnder) // only saved then
            t.duckAmount = float(std::clamp(o[QStringLiteral("duckAmount")].toDouble(-12.0), -60.0, 0.0));
        for (const auto& cv : o[QStringLiteral("clips")].toArray())
            t.clips.push_back(clipFromJson(cv.toObject()));
        t.effects = chainFromJson(o[QStringLiteral("effects")]);
        t.volume = envelopeFromJson(o[QStringLiteral("volumeEnvelope")], 1.0);
        t.panEnvelope = envelopeFromJson(o[QStringLiteral("panEnvelope")], 0.0);
        t.composite = envelopeFromJson(o[QStringLiteral("compositeEnvelope")], 1.0);
        timeline.tracks.push_back(std::move(t));
    }
    for (const auto& v : root[QStringLiteral("markers")].toArray()) {
        const QJsonObject o = v.toObject();
        Marker m;
        m.id = o[QStringLiteral("id")].toInt();
        m.time = std::max<TimeUs>(0, jsonTime(o[QStringLiteral("time")]));
        m.length = std::max<TimeUs>(0, jsonTime(o[QStringLiteral("length")]));
        m.name = o[QStringLiteral("name")].toString();
        m.color = std::clamp(o[QStringLiteral("color")].toInt(0), 0, kMarkerColors - 1);
        timeline.markers.push_back(m);
    }
    timeline.sortMarkers();
    timeline.outputEffects = chainFromJson(root[QStringLiteral("outputEffects")]);
    timeline.audioMasterEffects = chainFromJson(root[QStringLiteral("audioMasterEffects")]);
    timeline.audioMasterLevel = float(std::clamp(root[QStringLiteral("audioMasterLevel")].toDouble(1.0), 0.0, 1.0));
    timeline.liveEffects = chainFromJson(root[QStringLiteral("liveEffects")]);
    timeline.livePads = livePadsFromJson(root[QStringLiteral("livePads")].toArray());
    for (int i = 0; i < kLiveLayers; ++i)
        timeline.liveLayerEffects[size_t(i)] = chainFromJson(root[liveLayerKey(QStringLiteral("liveLayerEffects"), i)]);
    timeline.liveOverlays = liveOverlaysFromJson(root[QStringLiteral("liveOverlays")].toArray());
    timeline.liveScenes = liveScenesFromJson(root[QStringLiteral("liveScenes")].toArray());
    timeline.liveSetlist = liveSetlistFromJson(root[QStringLiteral("liveSetlist")].toArray());
    timeline.nextId = root[QStringLiteral("nextId")].toInt(1);
    repairTimeline(timeline);
    timeline.sortClips();

    m_undo.clear();
    m_packed.reset();
    m_session = std::make_shared<const int>(0);
    m_settings = settings;
    m_media = std::move(media);
    m_nextMediaId = nextMediaId;
    m_timeline = std::move(timeline);
    m_filePath = path;
    m_portablePaths = root["collection"].toObject()["relativeAssets"].toBool();
    m_deliver = root[QStringLiteral("deliver")].toObject();
    m_dirtyExtra = false;
    m_snapshot.reset();
    mediaChangedNow();
    emit settingsChanged();
    emit changed();
    return true;
}

QString timelineProblem(const Timeline& tl)
{
    static thread_local int depth=0;if(depth>16)return QStringLiteral("Sequence nesting exceeds sixteen levels.");
    struct DepthGuard {int& value;DepthGuard(int& v):value(v){++value;}~DepthGuard(){--value;}} guard(depth);
    std::vector<std::pair<int, QString>> ids; // and what has it
    auto collectIds = [&](const EffectChain& chain, const QString& where) {
        forEachEffect(chain, [&](const EffectInstance& e) { ids.emplace_back(e.id, QStringLiteral("%1 on %2").arg(e.type, where)); });
    };
    for (const Track& t : tl.tracks) {
        ids.emplace_back(t.id, QStringLiteral("track"));
        collectIds(t.effects, QStringLiteral("track %1").arg(t.id));
        for (size_t i = 0; i < t.clips.size(); ++i) {
            const Clip& c = t.clips[i];
            if (c.length <= 0)
                return QStringLiteral("event %1 has length %2").arg(c.id).arg(c.length);
            if (c.start < 0)
                return QStringLiteral("event %1 starts at %2").arg(c.id).arg(c.start);
            if (c.sourceIn < 0)
                return QStringLiteral("event %1 starts %2 into its file").arg(c.id).arg(c.sourceIn);
            if (c.fadeIn < 0 || c.fadeOut < 0)
                return QStringLiteral("event %1 has a negative fade").arg(c.id);
            if (!std::isfinite(c.gain) || !std::isfinite(c.speed) || c.speed <= 0)
                return QStringLiteral("event %1 has gain %2, speed %3").arg(c.id).arg(c.gain).arg(c.speed);
            if (i > 0 && t.clips[i - 1].start > c.start)
                return QStringLiteral("events out of order on track %1").arg(t.id);
            if (c.end() < c.start)
                return QStringLiteral("event %1 ends before it starts (overflow)").arg(c.id);
            if(c.isSequence()){const QString issue=timelineProblem(*c.sequence.value);if(!issue.isEmpty())return QStringLiteral("Sequence %1: %2").arg(c.id).arg(issue);}
            ids.emplace_back(c.id, QStringLiteral("event"));
            collectIds(c.effects, QStringLiteral("event %1").arg(c.id));
        }
        if (!std::isfinite(t.level) || !std::isfinite(t.pan))
            return QStringLiteral("track %1 has level %2, pan %3").arg(t.id).arg(t.level).arg(t.pan);
    }
    for (const Marker& m : tl.markers)
        ids.emplace_back(m.id, QStringLiteral("marker"));
    collectIds(tl.outputEffects, QStringLiteral("the video output"));
    collectIds(tl.audioMasterEffects, QStringLiteral("the audio master"));
    collectIds(tl.liveEffects, QStringLiteral("Live FX"));
    for (int i = 0; i < kLiveLayers; ++i)
        collectIds(tl.liveLayerEffects[size_t(i)], QStringLiteral("Live layer %1").arg(i + 1));
    for (const LivePad& p : tl.livePads)
        collectIds(p.effects, QStringLiteral("a pad"));
    std::map<int, QString> seen;
    for (const auto& [id, what] : ids) {
        if (id >= tl.nextId)
            return QStringLiteral("id %1 (%2) is not below nextId %3 (a new item would reuse it)").arg(id).arg(what).arg(tl.nextId);
        if (id > 0 && !seen.emplace(id, what).second)
            return QStringLiteral("id %1 is used twice: %2 and %3").arg(id).arg(seen[id], what);
    }
    return {};
}

bool NestedTimeline::operator==(const NestedTimeline& other) const
{
    if (value == other.value) return true;
    return value && other.value && *value == *other.value;
}
QJsonObject nestedTimelineToJson(const Timeline& timeline)
{
    QJsonObject root;
    QJsonArray tracks;
    for (const auto& t : timeline.tracks) {
        QJsonObject o;
        o[QStringLiteral("id")] = t.id;
        o[QStringLiteral("type")] = trackTypeName(t.type);
        o[QStringLiteral("name")] = t.name;
        o[QStringLiteral("muted")] = t.muted;
        o[QStringLiteral("solo")] = t.solo;
        if (t.locked)
            o[QStringLiteral("locked")] = true;
        o[QStringLiteral("level")] = t.level;
        o[QStringLiteral("pan")] = t.pan;
        if (t.blend)
            o[QStringLiteral("blend")] = t.blend;
        if (t.matte)
            o[QStringLiteral("matte")] = t.matte;
        if (t.duckUnder) {
            o[QStringLiteral("duckUnder")] = t.duckUnder;
            o[QStringLiteral("duckAmount")] = t.duckAmount;
        }
        QJsonArray clips;
        for (const auto& c : t.clips)
            clips.append(clipToJson(c));
        o[QStringLiteral("clips")] = clips;
        if (!t.effects.empty())
            o[QStringLiteral("effects")] = chainToJson(t.effects);
        if (t.volume.active || !t.volume.points.empty())
            o[QStringLiteral("volumeEnvelope")] = envelopeToJson(t.volume);
        if (t.panEnvelope.active || !t.panEnvelope.points.empty())
            o[QStringLiteral("panEnvelope")] = envelopeToJson(t.panEnvelope);
        if (t.composite.active || !t.composite.points.empty())
            o[QStringLiteral("compositeEnvelope")] = envelopeToJson(t.composite);
        tracks.append(o);
    }
    root[QStringLiteral("tracks")] = tracks;
    QJsonArray markers; for(const auto& m:timeline.markers) markers.append(QJsonObject{{"id",m.id},{"time",QString::number(m.time)},{"length",QString::number(m.length)},{"name",m.name},{"color",m.color}});
    root["markers"]=markers;
    root["nextId"] = timeline.nextId;
    root["outputEffects"] = chainToJson(timeline.outputEffects);
    root["audioMasterEffects"] = chainToJson(timeline.audioMasterEffects);
    if (timeline.audioMasterLevel != 1.0f)
        root["audioMasterLevel"] = timeline.audioMasterLevel;
    root["chapters"] = timeline.chapters;
    root["narrationSettings"] = timeline.narrationSettings;
    if (!timeline.multicam.isEmpty())
        root["multicam"] = timeline.multicam;
    return root;
}
Timeline nestedTimelineFromJson(const QJsonObject& root)
{
    Timeline timeline;
    for (const auto& v : root[QStringLiteral("tracks")].toArray()) {
        const QJsonObject o = v.toObject();
        Track t;
        t.id = o[QStringLiteral("id")].toInt();
        const QString type = o[QStringLiteral("type")].toString();
        t.type = type == QLatin1String("image") ? TrackType::Image
               : type == QLatin1String("audio") ? TrackType::Audio : TrackType::Video;
        t.name = o[QStringLiteral("name")].toString();
        t.muted = o[QStringLiteral("muted")].toBool();
        t.solo = o[QStringLiteral("solo")].toBool();
        t.locked = o[QStringLiteral("locked")].toBool();
        t.level = float(o[QStringLiteral("level")].toDouble(1.0));
        t.pan = float(o[QStringLiteral("pan")].toDouble(0.0));
        t.blend = std::clamp(o[QStringLiteral("blend")].toInt(0), 0, 7);
        t.matte = std::clamp(o[QStringLiteral("matte")].toInt(0), 0, int(MatteLumaInverted));
        t.duckUnder = o[QStringLiteral("duckUnder")].toInt(0);
        if (t.duckUnder) // only saved then
            t.duckAmount = float(std::clamp(o[QStringLiteral("duckAmount")].toDouble(-12.0), -60.0, 0.0));
        for (const auto& cv : o[QStringLiteral("clips")].toArray())
            t.clips.push_back(clipFromJson(cv.toObject()));
        t.effects = chainFromJson(o[QStringLiteral("effects")]);
        t.volume = envelopeFromJson(o[QStringLiteral("volumeEnvelope")], 1.0);
        t.panEnvelope = envelopeFromJson(o[QStringLiteral("panEnvelope")], 0.0);
        t.composite = envelopeFromJson(o[QStringLiteral("compositeEnvelope")], 1.0);
        timeline.tracks.push_back(std::move(t));
    }
    for(auto value:root["markers"].toArray()) { auto o=value.toObject(); Marker m; m.id=o["id"].toInt(); m.time=o["time"].toString().toLongLong(); m.length=o["length"].toString().toLongLong(); m.name=o["name"].toString(); m.color=o["color"].toInt(); timeline.markers.push_back(m); }
    timeline.nextId = root["nextId"].toInt(1);
    timeline.outputEffects = chainFromJson(root["outputEffects"]);
    timeline.audioMasterEffects = chainFromJson(root["audioMasterEffects"]);
    timeline.audioMasterLevel = float(std::clamp(root["audioMasterLevel"].toDouble(1.0), 0.0, 1.0));
    timeline.chapters = root["chapters"].toArray();
    timeline.narrationSettings = root["narrationSettings"].toObject();
    timeline.multicam = root["multicam"].toObject();
    repairTimeline(timeline); timeline.sortClips();
    return timeline;
}

void Project::shareMediaPool(const MediaMap& media)
{
    m_media = media; m_nextMediaId = 1;
    for(const auto& [id,item]:media) m_nextMediaId = std::max(m_nextMediaId,id+1);
    m_snapshot.reset(); mediaChangedNow();
}
