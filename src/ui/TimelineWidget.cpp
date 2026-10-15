#include "ui/TimelineWidget.h"

#include "core/AudioMixer.h"
#include "core/PanCrop.h"
#include "core/Presets.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsBrowser.h"
#include "ui/AudioLevels.h"
#include "ui/MediaBin.h"
#include "ui/Theme.h"

#include <QContextMenuEvent>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QApplication>
#include <QPainterPath>
#include <QToolTip>
#include <QTemporaryDir>
#include <QScrollBar>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace {

// A fine line or wash in the theme's contrast colour (white on dark themes, black on light).
QColor ink(int alpha)
{
    QColor c = theme::c().ink;
    c.setAlpha(alpha);
    return c;
}
constexpr int kHeaderWidth = 210;
constexpr int kRegionBarH = 14;
constexpr int kMarkerBarH = 17;
constexpr int kRulerTop = kRegionBarH + kMarkerBarH;
constexpr int kRulerH = 26;
constexpr int kTopH = kRulerTop + kRulerH;
constexpr int kEdgeGrab = 6;
constexpr int kFadeGrab = 8;
constexpr int kTitleH = 15;
constexpr int kSnapPx = 8;
constexpr int kVideoTrackH = 84;
constexpr int kAudioTrackH = 66;
constexpr TimeUs kScrollUnit = 1000; // horizontal scrollbar unit: 1 ms

QColor trackColor(TrackType type)
{
    return type == TrackType::Image ? theme::c().imageClip
         : type == TrackType::Audio ? theme::c().audioClip : theme::c().videoClip;
}

using levels::panText;
using levels::posFromVolume;
using levels::volumeFromPos;
using levels::volumeText;

// Velocity envelope axis: -100% at the bottom, 0% a quarter up, 100% in the
// middle, 1000% at the top (logarithmic above 100%).
double posFromVelocity(double v)
{
    if (v <= 0)
        return 0.25 * (std::max(v, kMinVelocity) + 1.0);
    if (v <= 1)
        return 0.25 + 0.25 * v;
    return 0.5 + 0.5 * std::log10(std::min(v, kMaxVelocity));
}
double velocityFromPos(double pos)
{
    if (std::abs(pos - 0.5) < 0.025)
        return 1.0; // detents at 100% and 0%
    if (std::abs(pos - 0.25) < 0.02)
        return 0.0;
    if (pos <= 0.25)
        return std::max(kMinVelocity, pos / 0.25 - 1.0);
    if (pos <= 0.5)
        return (pos - 0.25) / 0.25;
    return std::min(kMaxVelocity, std::pow(10.0, (pos - 0.5) / 0.5));
}

bool hasActiveEffectsList(const EffectChain& chain)
{
    return std::any_of(chain.begin(), chain.end(), [](const EffectInstance& e) { return e.enabled; });
}

QRect muteRect(const QRect& h) { return QRect(h.right() - 52, h.top() + 4, 22, 18); }
QRect soloRect(const QRect& h) { return QRect(h.right() - 27, h.top() + 4, 22, 18); }
QRect levelRect(const QRect& h) { return QRect(h.left() + 30, h.top() + 28, h.width() - 40, 14); }
QRect panRect(const QRect& h) { return QRect(h.left() + 30, h.top() + 46, h.width() - 40, 12); }
QRect fxRect(const QRect& h) { return QRect(h.right() - 77, h.top() + 4, 22, 18); }
QRect blendRect(const QRect& h) { return panRect(h); } // video tracks: compositing mode in the pan slot

// The transition at one end of event i of a track: its settings (the next
// event's start for an overlap at the end) and whether it is a crossfade.
struct EndTransition {
    const TransitionSpec* spec = nullptr;
    bool overlap = false;
    int holder = 0; // the event that holds it
    bool holderIn = true;
};
EndTransition endTransition(const Track& t, size_t i, bool atStart)
{
    const Clip& c = t.clips[i];
    if (atStart) {
        const bool overlap = i > 0 && t.clips[i - 1].end() > c.start;
        if (!overlap && c.fadeIn <= 0)
            return {};
        return {&c.transitionIn, overlap, c.id, true};
    }
    if (i + 1 < t.clips.size() && t.clips[i + 1].start < c.end())
        return {&t.clips[i + 1].transitionIn, true, t.clips[i + 1].id, true};
    if (c.fadeOut <= 0)
        return {};
    return {&c.transitionOut, false, c.id, false};
}
QString endTransitionName(const EndTransition& e, bool atStart)
{
    if (!e.spec)
        return {};
    if (!e.spec->isPlainFade()) {
        const TransitionDef* def = findTransition(e.spec->type);
        return def ? def->name : e.spec->type;
    }
    return QCoreApplication::translate("TimelineWidget", e.overlap ? "Crossfade" : atStart ? "Fade In" : "Fade Out");
}
QRectF transitionCloseRect(const QRectF& label, bool atStart)
{
    // On the side towards the middle of the event, away from its trim edge.
    return atStart ? QRectF(label.right() - 14, label.top(), 14, label.height()) : QRectF(label.left(), label.top(), 14, label.height());
}
constexpr double kTransitionCloseMinW = 46; // narrower labels have no ×

QColor markerColor(int i)
{
    static const QColor colors[kMarkerColors] = {{0xff, 0x9f, 0x3c}, {0xe8, 0xd4, 0x4d}, {0x5c, 0xcf, 0x6a}, {0x4d, 0xc9, 0xe8},
                                                 {0x5a, 0x8c, 0xff}, {0xb0, 0x7c, 0xff}, {0xff, 0x6f, 0xa8}, {0xff, 0x5a, 0x5a}};
    return colors[std::clamp(i, 0, kMarkerColors - 1)];
}
QString markerColorName(int i)
{
    static const char* names[kMarkerColors] = {"Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Pink", "Red"};
    return QCoreApplication::translate("TimelineWidget", names[std::clamp(i, 0, kMarkerColors - 1)]);
}

} // namespace

TimelineWidget::TimelineWidget(Project* project, MediaCache* cache, QWidget* parent)
    : QAbstractScrollArea(parent), m_project(project), m_cache(cache)
{
    setAcceptDrops(true);
    viewport()->setAcceptDrops(true);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::StrongFocus);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFrameShape(QFrame::NoFrame);
    setMinimumHeight(kTopH + 80);

    m_session = m_project->snapshot()->session;
    connect(m_project, &Project::changed, this, [this] {
        const auto session = m_project->snapshot()->session;
        if (m_session != session) {
            m_session = session;
            m_trackHeights.clear(); m_meters.clear(); m_generatedThumbs.clear();
            m_selectedTrack = 0; m_scrollY = 0; m_viewStart = 0;
            m_cursor = 0; m_playhead.reset(); m_regionStart = m_regionEnd = 0;
            m_drag = Drag::None; m_before = {}; m_dragClip = m_dragMarker = 0;
            m_pressTrack = -1; m_reorderGap = -1; m_dragMoved = false;
            m_bandBase.clear(); m_snapPoints.clear(); m_envDrag = {};
            m_dropX.reset(); m_fxDropClip = 0; m_fxDropTrack = -1;
            m_clipboard.clear(); // its media IDs belong to the previous project
            verticalScrollBar()->setValue(0); horizontalScrollBar()->setValue(0);
            setSelection({});
        }
        pruneSelection();
        updateScrollBars();
        viewport()->update();
    });
    connect(m_cache, &MediaCache::updated, viewport(), qOverload<>(&QWidget::update));
    updateScrollBars();
}

// ---------------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------------

double TimelineWidget::timeToX(TimeUs t) const
{
    return kHeaderWidth + double(t - m_viewStart) * m_pxPerSec / double(kUsPerSecond);
}

TimeUs TimelineWidget::xToTime(double x) const
{
    return m_viewStart + TimeUs(std::llround((x - kHeaderWidth) * double(kUsPerSecond) / m_pxPerSec));
}

int TimelineWidget::trackHeight(const Track& t) const
{
    auto it = m_trackHeights.find(t.id);
    if (it != m_trackHeights.end())
        return it->second;
    return isVisualTrack(t.type) ? kVideoTrackH : kAudioTrackH;
}

int TimelineWidget::trackTop(int index) const
{
    const auto& tracks = m_project->timeline().tracks;
    int y = kTopH - m_scrollY;
    for (int i = 0; i < index && i < int(tracks.size()); ++i)
        y += trackHeight(tracks[i]);
    return y;
}

int TimelineWidget::trackAt(int y) const
{
    if (y < kTopH)
        return -1;
    const auto& tracks = m_project->timeline().tracks;
    int top = kTopH - m_scrollY;
    for (int i = 0; i < int(tracks.size()); ++i) {
        const int h = trackHeight(tracks[i]);
        if (y >= top && y < top + h)
            return i;
        top += h;
    }
    return -1;
}

int TimelineWidget::tracksHeight() const
{
    int h = 0;
    for (const auto& t : m_project->timeline().tracks)
        h += trackHeight(t);
    return h;
}

QRect TimelineWidget::headerRect(int index) const
{
    return QRect(0, trackTop(index), kHeaderWidth, trackHeight(m_project->timeline().tracks[index]));
}

TimeUs TimelineWidget::visibleDuration() const
{
    return TimeUs(double(std::max(1, viewport()->width() - kHeaderWidth)) * kUsPerSecond / m_pxPerSec);
}

TimeUs TimelineWidget::quantize(TimeUs t) const
{
    return m_project->settings().fps.snap(t);
}

TimelineWidget::Hit TimelineWidget::hitTest(QPoint p) const
{
    Hit hit;
    if (p.x() < kHeaderWidth || p.y() < kTopH)
        return hit;
    hit.track = trackAt(p.y());
    if (hit.track < 0)
        return hit;
    const Track& track = m_project->timeline().tracks[hit.track];
    const int top = trackTop(hit.track);
    const int h = trackHeight(track);
    const double pxPerUs = m_pxPerSec / kUsPerSecond;
    // Later clips are drawn on top, so test them first.
    for (int ci = int(track.clips.size()) - 1; ci >= 0; --ci) {
        const Clip& c = track.clips[ci];
        const double x0 = timeToX(c.start), x1 = timeToX(c.end());
        if (p.x() < x0 - 1 || p.x() > x1 + 1)
            continue;
        hit.clipId = c.id;
        if (fxBadgeRect(c, top).contains(p)) {
            hit.zone = Zone::FxBadge;
            return hit;
        }
        if (isVisualTrack(track.type) && fxBadgeRect(c, top).translated(-24, 0).contains(p)) {
            hit.zone = Zone::PanCropBadge;
            return hit;
        }
        for (bool in : {true, false}) {
                const QRectF label = transitionLabelRect(track, size_t(ci), top, h, in);
                if (label.isEmpty() || !label.contains(p))
                    continue;
                hit.transitionIn = in;
                hit.zone = label.width() >= kTransitionCloseMinW && transitionCloseRect(label, in).contains(p) ? Zone::TransitionRemove
                                                                                                                : Zone::TransitionLabel;
                return hit;
            }
        const double mid = (x0 + x1) / 2;
        const double grab = std::min<double>(kEdgeGrab, (x1 - x0) / 4);
        const bool nearTop = p.y() < top + 1 + kFadeGrab;
        const double fadeInX = x0 + c.fadeIn * pxPerUs;
        const double fadeOutX = x1 - c.fadeOut * pxPerUs;
        if (nearTop && p.x() < mid && std::abs(p.x() - fadeInX) <= kFadeGrab)
            hit.zone = Zone::FadeIn;
        else if (nearTop && p.x() >= mid && std::abs(p.x() - fadeOutX) <= kFadeGrab)
            hit.zone = Zone::FadeOut;
        else if (p.x() - x0 <= grab)
            hit.zone = Zone::LeftEdge;
        else if (x1 - p.x() <= grab)
            hit.zone = Zone::RightEdge;
        else {
            const double bodyTop = top + 1 + kTitleH;
            const double bodyH = h - 2 - kTitleH;
            const double gainY = bodyTop + (1.0 - c.gain) * bodyH;
            hit.zone = std::abs(p.y() - gainY) <= 3 ? Zone::Gain : Zone::Body;
        }
        return hit;
    }
    return hit;
}

QRectF TimelineWidget::transitionLabelRect(const Track& track, size_t ci, int top, int height, bool atStart) const
{
    const Clip& c = track.clips[ci];
    const double pxPerUs = m_pxPerSec / kUsPerSecond;
    const double rx0 = timeToX(c.start), rx1 = timeToX(c.end());
    double xa, xb;
    if (atStart) {
        const ClipEnvelope env = effectiveEnvelope(track, ci);
        if (env.fadeIn <= 0)
            return {};
        xa = rx0;
        xb = rx0 + env.fadeIn * pxPerUs;
    } else {
        // An overlap at the end is labelled as the next event's start.
        if (c.fadeOut <= 0 || (ci + 1 < track.clips.size() && track.clips[ci + 1].start < c.end()))
            return {};
        xa = rx1 - c.fadeOut * pxPerUs;
        xb = rx1;
    }
    if (xb - xa < 30 || height - 2 - kTitleH < 20)
        return {};
    const double bottom = top + height - 1; // bottom of the event body
    return QRectF(xa + 2, bottom - 16, xb - xa - 4, 14);
}

QRectF TimelineWidget::fxBadgeRect(const Clip& c, int top) const
{
    const double x1 = std::min(timeToX(c.end()), double(viewport()->width()));
    const double x0 = std::max(timeToX(c.start), double(kHeaderWidth));
    if (x1 - x0 < 70)
        return {};
    return QRectF(x1 - 24, top + 2, 20, kTitleH - 2);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void TimelineWidget::setCursorTime(TimeUs t, bool ensureVisible)
{
    m_cursor = std::max<TimeUs>(0, t);
    if (ensureVisible)
        ensureTimeVisible(m_cursor, false);
    viewport()->update();
}

void TimelineWidget::setPlayhead(std::optional<TimeUs> t)
{
    m_playhead = t;
    if (t)
        ensureTimeVisible(*t, true);
    viewport()->update();
}

void TimelineWidget::setRegion(TimeUs a, TimeUs b)
{
    if (a > b)
        std::swap(a, b);
    a = std::max<TimeUs>(0, a);
    b = std::max<TimeUs>(0, b);
    if (a == m_regionStart && b == m_regionEnd)
        return;
    m_regionStart = a;
    m_regionEnd = b;
    viewport()->update();
}

void TimelineWidget::setSelection(std::set<int> sel)
{
    if (sel == m_selection)
        return;
    m_selection = std::move(sel);
    emit selectionChanged();
    viewport()->update();
}

void TimelineWidget::pruneSelection()
{
    for (auto it = m_generatedThumbs.begin(); it != m_generatedThumbs.end();)
        it = m_project->timeline().findClip(it->first) ? std::next(it) : m_generatedThumbs.erase(it);
    std::set<int> keep;
    for (int id : m_selection)
        if (m_project->timeline().findClip(id))
            keep.insert(id);
    setSelection(std::move(keep));
    const int n = int(m_project->timeline().tracks.size());
    m_selectedTrack = std::clamp(m_selectedTrack, 0, std::max(0, n - 1));
}

void TimelineWidget::splitAtCursor()
{
    Timeline tl = m_project->timeline();
    std::set<int> targets;
    for (int id : m_selection) {
        const Clip* c = tl.findClip(id);
        if (c && c->start < m_cursor && c->end() > m_cursor)
            targets.insert(id);
    }
    // With nothing selected under the cursor, split everything there.
    const auto created = ops::splitClips(tl, targets, m_cursor);
    if (created.empty())
        return;
    m_project->applyEdit(tl, tr("Split"));
}

void TimelineWidget::deleteSelection(bool ripple)
{
    if (m_selection.empty())
        return;
    Timeline tl = m_project->timeline();
    // Ctrl+Del always ripples; Del does when auto ripple is on.
    const RippleMode mode = ripple ? (m_ripple == RippleMode::All ? RippleMode::All : RippleMode::Tracks) : m_ripple;
    if (mode == RippleMode::All)
        ops::rippleRemoveClipsAll(tl, m_selection, true);
    else if (mode == RippleMode::Tracks)
        ops::rippleRemoveClips(tl, m_selection);
    else
        ops::removeClips(tl, m_selection);
    m_project->applyEdit(tl, mode != RippleMode::Off ? tr("Ripple Delete") : tr("Delete"));
}

void TimelineWidget::selectAll()
{
    std::set<int> all;
    for (const auto& t : m_project->timeline().tracks)
        for (const auto& c : t.clips)
            all.insert(c.id);
    setSelection(std::move(all));
}

void TimelineWidget::groupSelection()
{
    Timeline tl = m_project->timeline();
    ops::groupClips(tl, m_selection);
    m_project->applyEdit(tl, tr("Group"));
}

void TimelineWidget::ungroupSelection()
{
    Timeline tl = m_project->timeline();
    ops::ungroupClips(tl, m_selection);
    m_project->applyEdit(tl, tr("Ungroup"));
}

void TimelineWidget::copySelection()
{
    m_clipboard.clear();
    const auto& tl = m_project->timeline();
    TimeUs earliest = std::numeric_limits<TimeUs>::max();
    for (size_t ti = 0; ti < tl.tracks.size(); ++ti)
        for (const auto& c : tl.tracks[ti].clips)
            if (m_selection.count(c.id)) {
                m_clipboard.emplace_back(int(ti), c);
                earliest = std::min(earliest, c.start);
            }
    for (auto& [ti, c] : m_clipboard)
        c.start -= earliest;
}

void TimelineWidget::cutSelection()
{
    copySelection();
    deleteSelection(false);
}

void TimelineWidget::paste()
{
    if (m_clipboard.empty())
        return;
    Timeline tl = m_project->timeline();
    std::map<int, int> groupMap;
    std::set<int> pasted;
    for (auto [ti, c] : m_clipboard) {
        // Clips go back to the track they were copied from, if it still fits.
        auto media = m_project->mediaItem(c.mediaId);
        const TrackType wanted = media && media->info.isStill ? TrackType::Image
                               : c.isGenerated() || (media && media->info.videoStream(c.streamIndex)) ? TrackType::Video : TrackType::Audio;
        if (ti >= int(tl.tracks.size()) || isVisualTrack(tl.tracks[ti].type) != isVisualTrack(wanted))
            continue;
        c.id = tl.allocId();
        c.start += m_cursor;
        reassignEffectIds(c.effects, [&] { return tl.allocId(); });
        if (c.groupId) {
            auto it = groupMap.find(c.groupId);
            if (it == groupMap.end())
                it = groupMap.emplace(c.groupId, tl.allocId()).first;
            c.groupId = it->second;
        }
        pasted.insert(c.id);
        tl.tracks[ti].clips.push_back(c);
    }
    tl.sortClips();
    rippleAfterInsert(tl, m_cursor, pasted); // auto ripple: paste inserts
    m_project->applyEdit(tl, m_ripple != RippleMode::Off ? tr("Paste Insert") : tr("Paste"));
    setSelection(pasted);
}

void TimelineWidget::toggleMuteSelection()
{
    if (m_selection.empty())
        return;
    Timeline tl = m_project->timeline();
    bool anyUnmuted = false;
    for (int id : m_selection)
        if (const Clip* c = tl.findClip(id))
            anyUnmuted |= !c->muted;
    for (int id : m_selection)
        if (Clip* c = tl.findClip(id))
            c->muted = anyUnmuted;
    m_project->applyEdit(tl, anyUnmuted ? tr("Mute Event") : tr("Unmute Event"));
}

void TimelineWidget::insertTrack(TrackType type)
{
    Timeline tl = m_project->timeline();
    int index;
    if (isVisualTrack(type)) {
        index = 0; // new video tracks go on top
    } else {
        index = int(tl.tracks.size());
    }
    ops::addTrack(tl, type, index);
    m_project->applyEdit(tl, type == TrackType::Image ? tr("Insert Image Track")
                          : type == TrackType::Video ? tr("Insert Video Track") : tr("Insert Audio Track"));
}

void TimelineWidget::insertMedia(const std::vector<int>& mediaIds, TimeUs at, int trackIndex, bool newVideoTracks)
{
    Timeline tl = m_project->timeline();
    std::set<int> created;
    TimeUs pos = std::max<TimeUs>(0, at);
    for (int id : mediaIds) {
        auto media = m_project->mediaItem(id);
        if (!media || !media->info.ok())
            continue;
        const auto ids = ops::insertMedia(tl, *media, pos, trackIndex, m_project->settings().fps,
                                           newVideoTracks && media->info.hasVideo());
        if (ids.empty())
            continue;
        created.insert(ids.begin(), ids.end());
        if (const Clip* c = tl.findClip(ids.front()))
            pos = c->end();
    }
    if (created.empty())
        return;
    rippleAfterInsert(tl, std::max<TimeUs>(0, at), created);
    if (!m_autoCrossfade)
        ops::overwriteOverlaps(tl, created);
    m_project->applyEdit(tl, tr("Add Media"));
    setSelection(created);
    setCursorTime(pos);
    emit cursorMoved(pos);
}

void TimelineWidget::insertMediaAtCursor(int mediaId)
{
    const auto& tracks = m_project->timeline().tracks;
    insertMedia({mediaId}, m_cursor, m_selectedTrack < int(tracks.size()) ? m_selectedTrack : -1);
}

void TimelineWidget::zoomAround(double x, double factor)
{
    const TimeUs anchor = xToTime(x);
    const double maxZoom = std::max(1.0, m_project->settings().fps.toDouble()) * 60.0; // 60 px per frame
    m_pxPerSec = std::clamp(m_pxPerSec * factor, 0.02, maxZoom);
    m_viewStart = std::max<TimeUs>(0, anchor - TimeUs((x - kHeaderWidth) * kUsPerSecond / m_pxPerSec));
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::zoomIn()
{
    zoomAround(std::clamp(timeToX(m_cursor), double(kHeaderWidth), double(viewport()->width())), 1.5);
}

void TimelineWidget::zoomOut()
{
    zoomAround(std::clamp(timeToX(m_cursor), double(kHeaderWidth), double(viewport()->width())), 1.0 / 1.5);
}

void TimelineWidget::zoomToFit()
{
    const TimeUs dur = std::max<TimeUs>(m_project->timeline().duration(), 10 * kUsPerSecond);
    m_pxPerSec = std::max(0.02, double(viewport()->width() - kHeaderWidth - 20) * kUsPerSecond / double(dur));
    m_viewStart = 0;
    updateScrollBars();
    viewport()->update();
}

void TimelineWidget::stepFrames(int frames)
{
    const FrameRate fps = m_project->settings().fps;
    const TimeUs t = fps.timeOf(std::max<int64_t>(0, fps.frameAt(quantize(m_cursor)) + frames));
    setCursorTime(t);
    emit cursorMoved(t);
    emit scrubbed(t);
}

void TimelineWidget::jumpToEdit(int direction)
{
    std::vector<TimeUs> points{0};
    for (const auto& t : m_project->timeline().tracks)
        for (const auto& c : t.clips) {
            points.push_back(c.start);
            points.push_back(c.end());
        }
    if (hasRegion()) {
        points.push_back(m_regionStart);
        points.push_back(m_regionEnd);
    }
    for (const Marker& m : m_project->timeline().markers) {
        points.push_back(m.time);
        if (m.isRegion())
            points.push_back(m.end());
    }
    std::sort(points.begin(), points.end());
    TimeUs target = m_cursor;
    if (direction > 0) {
        auto it = std::upper_bound(points.begin(), points.end(), m_cursor);
        if (it != points.end())
            target = *it;
    } else {
        auto it = std::lower_bound(points.begin(), points.end(), m_cursor);
        if (it != points.begin())
            target = *std::prev(it);
    }
    setCursorTime(target);
    emit cursorMoved(target);
}

void TimelineWidget::goToStart()
{
    setCursorTime(0);
    emit cursorMoved(0);
}

void TimelineWidget::goToEnd()
{
    const TimeUs end = m_project->timeline().duration();
    setCursorTime(end);
    emit cursorMoved(end);
}

// ---------------------------------------------------------------------------
// Markers, in / out, tracks
// ---------------------------------------------------------------------------

int TimelineWidget::addMarker(TimeUs t, const QString& name)
{
    Timeline tl = m_project->timeline();
    Marker m;
    m.id = tl.allocId();
    m.time = std::max<TimeUs>(0, quantize(t));
    m.name = name;
    tl.markers.push_back(m);
    tl.sortMarkers();
    m_project->applyEdit(tl, tr("Add Marker"));
    return m.id;
}

int TimelineWidget::addRegion(TimeUs a, TimeUs b, const QString& name)
{
    a = std::max<TimeUs>(0, quantize(a));
    b = std::max<TimeUs>(0, quantize(b));
    if (a > b)
        std::swap(a, b);
    if (b <= a)
        return 0;
    Timeline tl = m_project->timeline();
    Marker m;
    m.id = tl.allocId();
    m.time = a;
    m.length = b - a;
    m.name = name;
    m.color = 2; // green
    tl.markers.push_back(m);
    tl.sortMarkers();
    m_project->applyEdit(tl, tr("Add Region"));
    return m.id;
}

bool TimelineWidget::addRegionFromSelection()
{
    if (!hasRegion()) {
        emit message(tr("Make a time selection first (drag in the bar above the ruler, or I / O), then press R."));
        return false;
    }
    return addRegion(m_regionStart, m_regionEnd) != 0;
}

void TimelineWidget::renameMarker(int markerId)
{
    Timeline tl = m_project->timeline();
    Marker* m = tl.findMarker(markerId);
    if (!m)
        return;
    bool ok = false;
    const QString name = QInputDialog::getText(this, m->isRegion() ? tr("Rename Region") : tr("Rename Marker"), tr("Name:"),
                                               QLineEdit::Normal, m->name, &ok);
    if (!ok)
        return;
    m->name = name.trimmed();
    m_project->applyEdit(tl, m->isRegion() ? tr("Rename Region") : tr("Rename Marker"));
}

void TimelineWidget::removeMarker(int markerId)
{
    Timeline tl = m_project->timeline();
    const auto it = std::find_if(tl.markers.begin(), tl.markers.end(), [&](const Marker& m) { return m.id == markerId; });
    if (it == tl.markers.end())
        return;
    const bool region = it->isRegion();
    tl.markers.erase(it);
    m_project->applyEdit(tl, region ? tr("Delete Region") : tr("Delete Marker"));
}

void TimelineWidget::jumpToMarker(int direction)
{
    std::vector<TimeUs> points;
    for (const Marker& m : m_project->timeline().markers) {
        points.push_back(m.time);
        if (m.isRegion())
            points.push_back(m.end());
    }
    std::sort(points.begin(), points.end());
    std::optional<TimeUs> target;
    if (direction > 0) {
        if (auto it = std::upper_bound(points.begin(), points.end(), m_cursor); it != points.end())
            target = *it;
    } else if (auto it = std::lower_bound(points.begin(), points.end(), m_cursor); it != points.begin()) {
        target = *std::prev(it);
    }
    if (!target) {
        emit message(points.empty() ? tr("No markers yet: press M to add one at the cursor.") : tr("No more markers that way."));
        return;
    }
    setCursorTime(*target);
    emit cursorMoved(*target);
}

void TimelineWidget::setInPoint(TimeUs t)
{
    t = std::max<TimeUs>(0, quantize(t));
    setRegion(t, std::max(t, m_regionEnd));
    emit message(tr("In point %1").arg(formatTimecode(t, m_project->settings().fps)));
}

void TimelineWidget::setOutPoint(TimeUs t)
{
    t = std::max<TimeUs>(0, quantize(t));
    setRegion(std::min(m_regionStart, t), t);
    emit message(tr("Out point %1").arg(formatTimecode(t, m_project->settings().fps)));
}

void TimelineWidget::moveTrack(int from, int to)
{
    Timeline tl = m_project->timeline();
    const int n = int(tl.tracks.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    ops::moveTrack(tl, from, to);
    m_project->applyEdit(tl, tr("Move Track"));
    m_selectedTrack = to;
    viewport()->update();
}

void TimelineWidget::setTrackBlend(int trackIndex, int mode)
{
    Timeline tl = m_project->timeline();
    if (trackIndex < 0 || trackIndex >= int(tl.tracks.size()) || !isVisualTrack(tl.tracks[size_t(trackIndex)].type))
        return;
    tl.tracks[size_t(trackIndex)].blend = std::clamp(mode, 0, int(blendModeNames().size()) - 1);
    m_project->applyEdit(tl, tr("Compositing Mode: %1").arg(blendModeNames().value(mode)));
}

void TimelineWidget::removeTransitionAt(int clipId, bool atStart)
{
    Timeline tl = m_project->timeline();
    if (ops::removeTransition(tl, clipId, atStart))
        m_project->applyEdit(tl, tr("Remove Transition"));
    else
        emit message(tr("There is no transition there."));
}

void TimelineWidget::nudgeSelection(int frames)
{
    if (m_selection.empty() || frames == 0)
        return;
    Timeline tl = m_project->timeline();
    TimeUs minStart = std::numeric_limits<TimeUs>::max();
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (m_selection.count(c.id))
                minStart = std::min(minStart, c.start);
    const TimeUs delta = std::max(TimeUs(frames) * m_project->settings().fps.frameDuration(), -minStart);
    if (delta == 0)
        return;
    const Timeline before = tl;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips)
            if (m_selection.count(c.id))
                c.start = quantize(c.start + delta);
    tl.sortClips();
    if (m_ripple != RippleMode::Off)
        ops::rippleShift(tl, minStart, delta, rippleTracks(before, m_selection), m_selection, m_ripple == RippleMode::All);
    m_project->applyEdit(tl, tr("Nudge"));
}

void TimelineWidget::trimSelectionToCursor(bool start)
{
    Timeline tl = m_project->timeline();
    const TimeUs at = quantize(m_cursor);
    struct Done {
        int track;
        TimeUs from, by; // ripple: what follows moves `by` from `from`
    };
    std::vector<Done> done;
    std::set<int> trimmed;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips) {
            if (!m_selection.count(c.id) || at <= c.start || at >= c.end())
                continue;
            const TimeUs oldStart = c.start, oldEnd = c.end(), oldLength = c.length;
            if (start) {
                ops::trimClipStart(c, at - c.start);
                c.fadeIn = std::min(c.fadeIn, c.length);
                if (m_ripple != RippleMode::Off)
                    c.start = oldStart; // the event stays put, what follows comes earlier
            } else {
                ops::trimClipEnd(c, at - c.end());
            }
            ops::clampFades(c);
            trimmed.insert(c.id);
            done.push_back({t.id, oldEnd, c.length - oldLength});
        }
    if (done.empty()) {
        emit message(tr("Put the cursor inside the selected events first."));
        return;
    }
    if (m_ripple == RippleMode::All)
        ops::rippleShift(tl, done.front().from, done.front().by, {}, trimmed, true);
    else if (m_ripple == RippleMode::Tracks)
        for (const Done& d : done)
            ops::rippleShift(tl, d.from, d.by, {d.track}, trimmed, false);
    tl.sortClips();
    m_project->applyEdit(tl, start ? tr("Trim Start to Cursor") : tr("Trim End to Cursor"));
}

int TimelineWidget::insertFreezeFrame(TimeUs length)
{
    Timeline tl = m_project->timeline();
    const TimeUs at = quantize(m_cursor);
    // The selected video event under the cursor, else the top one there.
    int clipId = 0, ti = -1;
    for (int pass = 0; pass < 2 && !clipId; ++pass)
        for (int i = 0; i < int(tl.tracks.size()) && !clipId; ++i) {
            if (!isVisualTrack(tl.tracks[size_t(i)].type))
                continue;
            for (const Clip& c : tl.tracks[size_t(i)].clips)
                if (c.start <= at && c.end() > at && !c.isGenerated() && (pass == 1 || m_selection.count(c.id))) {
                    clipId = c.id;
                    ti = i;
                }
        }
    if (!clipId) {
        emit message(tr("Put the cursor over a video event to freeze its picture there."));
        return 0;
    }
    Clip freeze = *tl.findClip(clipId);
    const TimeUs source = freeze.sourceTimeAt(at);
    // Its effects' keyframes carry on from the frame it holds.
    ops::shiftClipKeyframes(freeze, -(at - freeze.start));
    // Make room: what is at or after the cursor on this track (and the
    // tracks of its grouped events, so they stay in sync) moves later.
    std::set<int> tracks{tl.tracks[size_t(ti)].id};
    for (int id : ops::expandToGroups(tl, {clipId})) {
        int gti = -1;
        if (tl.findClip(id, &gti))
            tracks.insert(tl.tracks[size_t(gti)].id);
    }
    ops::rippleInsert(tl, at, length, tracks, {}, m_ripple == RippleMode::All);
    // A held frame: the same footage at 0% speed (a flat velocity of 0).
    freeze.id = tl.allocId();
    freeze.start = at;
    freeze.length = length;
    freeze.sourceIn = source;
    freeze.reverse = false;
    freeze.speed = 1.0;
    freeze.velocity = Envelope{true, 0.0, {}};
    freeze.fadeIn = freeze.fadeOut = 0;
    freeze.transitionIn = freeze.transitionOut = {};
    freeze.groupId = 0;
    freeze.name = tr("Freeze: %1").arg(freeze.name);
    reassignEffectIds(freeze.effects, [&] { return tl.allocId(); });
    tl.tracks[size_t(ti)].clips.push_back(freeze);
    tl.sortClips();
    m_project->applyEdit(tl, tr("Freeze Frame"));
    setSelection({freeze.id});
    return freeze.id;
}

void TimelineWidget::setFadeCurveOnSelection(bool fadeIn, int curve)
{
    Timeline tl = m_project->timeline();
    bool changed = false;
    for (int id : m_selection)
        if (Clip* c = tl.findClip(id)) {
            (fadeIn ? c->fadeInCurve : c->fadeOutCurve) = std::clamp(curve, 0, kFadeCurves - 1);
            changed = true;
        }
    if (changed)
        m_project->applyEdit(tl, fadeIn ? tr("Fade In Curve") : tr("Fade Out Curve"));
}

void TimelineWidget::setTrackMeters(const std::map<int, std::pair<float, float>>& peaks)
{
    // Meters fall back smoothly between updates.
    bool any = false;
    for (auto& [id, m] : m_meters) {
        m.first *= 0.82f;
        m.second *= 0.82f;
        any |= m.first > 0.001f || m.second > 0.001f;
    }
    for (const auto& [id, pk] : peaks) {
        auto& m = m_meters[id];
        m.first = std::max(m.first, pk.first);
        m.second = std::max(m.second, pk.second);
        any = true;
    }
    if (any)
        viewport()->update(QRect(0, kTopH, kHeaderWidth, viewport()->height() - kTopH));
}

void TimelineWidget::setTrackDuck(int trackIndex, int underTrackId, float amountDb)
{
    Timeline tl = m_project->timeline();
    if (trackIndex < 0 || trackIndex >= int(tl.tracks.size()) || tl.tracks[size_t(trackIndex)].type != TrackType::Audio)
        return;
    Track& t = tl.tracks[size_t(trackIndex)];
    t.duckUnder = underTrackId == t.id ? 0 : underTrackId;
    t.duckAmount = std::clamp(amountDb, -60.0f, 0.0f);
    m_project->applyEdit(tl, t.duckUnder ? tr("Auto Duck") : tr("Auto Duck Off"));
}

void TimelineWidget::removeTransitionsFromSelection()
{
    Timeline tl = m_project->timeline();
    bool changed = false;
    for (int id : m_selection) {
        if (!tl.findClip(id))
            continue;
        changed |= ops::removeTransition(tl, id, true);
        changed |= ops::removeTransition(tl, id, false);
    }
    if (changed)
        m_project->applyEdit(tl, tr("Remove Transitions"));
    else
        emit message(tr("The selected events have no transitions."));
}

std::set<int> TimelineWidget::rippleTracks(const Timeline& tl, const std::set<int>& clips) const
{
    if (m_ripple == RippleMode::All)
        return {};
    std::set<int> ids{-1}; // never empty: {} would mean every track
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (clips.count(c.id)) {
                ids.insert(t.id);
                break;
            }
    return ids;
}

void TimelineWidget::rippleAfterInsert(Timeline& tl, TimeUs at, const std::set<int>& created) const
{
    if (m_ripple == RippleMode::Off || created.empty())
        return;
    TimeUs end = at;
    for (const auto& t : tl.tracks)
        for (const auto& c : t.clips)
            if (created.count(c.id))
                end = std::max(end, c.end());
    ops::rippleInsert(tl, at, end - at, rippleTracks(tl, created), created, m_ripple == RippleMode::All);
}

int TimelineWidget::reorderGap(int y) const
{
    const auto& tracks = m_project->timeline().tracks;
    for (int i = 0; i < int(tracks.size()); ++i)
        if (y < trackTop(i) + trackHeight(tracks[size_t(i)]) / 2)
            return i;
    return int(tracks.size());
}

QString TimelineWidget::markerLabel(const Marker& m) const
{
    // Numbered in time order, markers and regions separately.
    int n = 0;
    for (const Marker& x : m_project->timeline().markers) {
        if (x.isRegion() == m.isRegion())
            ++n;
        if (x.id == m.id)
            break;
    }
    const QString number = m.isRegion() ? QStringLiteral("R%1").arg(n) : QString::number(n);
    return m.name.isEmpty() ? number : number + QLatin1Char(' ') + m.name;
}

int TimelineWidget::markerAt(QPoint p, int* part) const
{
    if (p.x() < kHeaderWidth || p.y() < kRegionBarH || p.y() >= kRulerTop)
        return 0;
    const auto& markers = m_project->timeline().markers;
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    const QFontMetrics fm(f);
    // Markers sit on top of regions, later ones on top of earlier ones.
    for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
        if (it->isRegion())
            continue;
        const double x = timeToX(it->time);
        const double w = std::min(160, fm.horizontalAdvance(markerLabel(*it)) + 8);
        if (p.x() >= x - 4 && p.x() <= x + w) {
            if (part)
                *part = 0;
            return it->id;
        }
    }
    for (auto it = markers.rbegin(); it != markers.rend(); ++it) {
        if (!it->isRegion())
            continue;
        const double x0 = timeToX(it->time), x1 = timeToX(it->end());
        int at;
        if (std::abs(p.x() - x1) <= 5)
            at = 2;
        else if (std::abs(p.x() - x0) <= 5)
            at = 1;
        else if (p.x() > x0 && p.x() < x1)
            at = 0;
        else
            continue;
        if (part)
            *part = at;
        return it->id;
    }
    return 0;
}

void TimelineWidget::markerMenu(QMenu& menu, QPoint p)
{
    const int id = markerAt(p);
    const TimeUs at = quantize(xToTime(p.x()));
    if (const Marker* m = id ? m_project->timeline().findMarker(id) : nullptr) {
        const bool region = m->isRegion();
        const TimeUs t0 = m->time, t1 = m->end();
        auto go = [this](TimeUs t) {
            setCursorTime(t);
            emit cursorMoved(t);
        };
        menu.addAction(region ? tr("Rename Region...") : tr("Rename Marker..."), this, [this, id] { renameMarker(id); });
        if (region) {
            menu.addAction(tr("Select Region (to loop or render it)"), this, [this, t0, t1] { setRegion(t0, t1); });
            menu.addAction(tr("Go to Region Start"), this, [go, t0] { go(t0); });
            menu.addAction(tr("Go to Region End"), this, [go, t1] { go(t1); });
        } else {
            menu.addAction(tr("Go to Marker"), this, [go, t0] { go(t0); });
        }
        QMenu* colors = menu.addMenu(tr("Color"));
        for (int i = 0; i < kMarkerColors; ++i) {
            QPixmap swatch(12, 12);
            swatch.fill(markerColor(i));
            QAction* a = colors->addAction(QIcon(swatch), markerColorName(i), this, [this, id, i] {
                Timeline tl = m_project->timeline();
                if (Marker* mm = tl.findMarker(id)) {
                    mm->color = i;
                    m_project->applyEdit(tl, tr("Marker Color"));
                }
            });
            a->setCheckable(true);
            a->setChecked(m->color == i);
        }
        menu.addSeparator();
        menu.addAction(region ? tr("Delete Region") : tr("Delete Marker"), this, [this, id] { removeMarker(id); });
        menu.addSeparator();
    }
    menu.addAction(tr("Insert Marker Here"), this, [this, at] { addMarker(at); });
    menu.addAction(tr("Insert Region from Time Selection\tR"), this, [this] { addRegionFromSelection(); })->setEnabled(hasRegion());
    if (!m_project->timeline().markers.empty())
        menu.addAction(tr("Delete All Markers and Regions"), this, [this] {
            Timeline tl = m_project->timeline();
            tl.markers.clear();
            m_project->applyEdit(tl, tr("Delete All Markers"));
        });
}

void TimelineWidget::paintMarkers(QPainter& p)
{
    const int W = viewport()->width();
    const QRect bar(kHeaderWidth, kRegionBarH, W - kHeaderWidth, kMarkerBarH);
    p.fillRect(bar, theme::c().markerBar);
    p.setPen(theme::c().deep);
    p.drawLine(bar.left(), bar.bottom(), bar.right(), bar.bottom());
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const QFontMetrics fm(f);
    const auto& markers = m_project->timeline().markers;
    for (const Marker& m : markers) {
        if (!m.isRegion())
            continue;
        const double x0 = timeToX(m.time), x1 = timeToX(m.end());
        if (x1 < kHeaderWidth || x0 > W)
            continue;
        const QColor c = markerColor(m.color);
        QColor fill = c;
        fill.setAlpha(70);
        const QRectF r(x0, bar.top() + 2, x1 - x0, bar.height() - 4);
        p.fillRect(r, fill);
        p.fillRect(QRectF(x0, bar.top(), 2, bar.height()), c);
        p.fillRect(QRectF(x1 - 2, bar.top(), 2, bar.height()), c);
        p.setPen(c.lighter(150));
        const double tx = std::max(x0, double(kHeaderWidth)) + 5;
        p.drawText(QRectF(tx, r.top(), std::max(0.0, x1 - tx - 4), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(markerLabel(m), Qt::ElideRight, int(std::max(0.0, x1 - tx - 4))));
    }
    for (const Marker& m : markers) {
        if (m.isRegion())
            continue;
        const double x = timeToX(m.time);
        const QString label = markerLabel(m);
        const int w = std::min(160, fm.horizontalAdvance(label) + 8);
        if (x + w < kHeaderWidth || x > W)
            continue;
        const QColor c = markerColor(m.color);
        p.fillRect(QRectF(x, bar.top() + 1, w, bar.height() - 3), c);
        p.fillRect(QRectF(x, bar.top(), 1.5, bar.height()), c.lighter(130));
        p.setPen(QColor(0x18, 0x18, 0x18));
        p.drawText(QRectF(x + 4, bar.top() + 1, w - 6, bar.height() - 3), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideRight, w - 6));
    }
}

// ---------------------------------------------------------------------------
// Scrolling
// ---------------------------------------------------------------------------

void TimelineWidget::updateScrollBars()
{
    const TimeUs visible = visibleDuration();
    const TimeUs content = std::max(m_project->timeline().duration() + visible / 2, m_viewStart + visible);
    QScrollBar* h = horizontalScrollBar();
    h->blockSignals(true);
    h->setRange(0, int(std::max<TimeUs>(0, content - visible) / kScrollUnit));
    h->setPageStep(int(visible / kScrollUnit));
    h->setSingleStep(std::max(1, int(visible / kScrollUnit / 20)));
    h->setValue(int(m_viewStart / kScrollUnit));
    h->blockSignals(false);

    QScrollBar* v = verticalScrollBar();
    const int avail = std::max(0, viewport()->height() - kTopH);
    // Qt uses rangeChanged to show/hide an AsNeeded scrollbar. Blocking it
    // leaves overflowing tracks without a scrollbar until the next resize.
    v->setRange(0, std::max(0, tracksHeight() + 40 - avail));
    v->setPageStep(avail);
    v->setSingleStep(20);
    m_scrollY = std::clamp(m_scrollY, 0, v->maximum());
    v->setValue(m_scrollY);
}

void TimelineWidget::scrollContentsBy(int, int)
{
    m_viewStart = TimeUs(horizontalScrollBar()->value()) * kScrollUnit;
    m_scrollY = verticalScrollBar()->value();
    viewport()->update();
}

void TimelineWidget::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    updateScrollBars();
}

void TimelineWidget::ensureTimeVisible(TimeUs t, bool page)
{
    const double x = timeToX(t);
    const int right = viewport()->width() - 12;
    if (x >= kHeaderWidth && x <= right)
        return;
    const TimeUs visible = visibleDuration();
    // Paging (playback) keeps the playhead near the left; jumps centre it.
    m_viewStart = std::max<TimeUs>(0, page && x > right ? t - visible / 20 : t - visible / 2);
    updateScrollBars();
}

// ---------------------------------------------------------------------------
// Snapping
// ---------------------------------------------------------------------------

void TimelineWidget::buildSnapPoints(const std::set<int>& exclude, int excludeMarker)
{
    m_snapPoints.clear();
    m_snapPoints.push_back(0);
    m_snapPoints.push_back(m_cursor);
    if (hasRegion()) {
        m_snapPoints.push_back(m_regionStart);
        m_snapPoints.push_back(m_regionEnd);
    }
    for (const Marker& m : m_project->timeline().markers)
        if (m.id != excludeMarker) {
            m_snapPoints.push_back(m.time);
            if (m.isRegion())
                m_snapPoints.push_back(m.end());
        }
    for (const auto& t : m_project->timeline().tracks)
        for (const auto& c : t.clips)
            if (!exclude.count(c.id)) {
                m_snapPoints.push_back(c.start);
                m_snapPoints.push_back(c.end());
            }
}

std::optional<TimeUs> TimelineWidget::snapTo(TimeUs t) const
{
    const TimeUs tolerance = TimeUs(kSnapPx * kUsPerSecond / m_pxPerSec);
    std::optional<TimeUs> best;
    for (TimeUs s : m_snapPoints) {
        const TimeUs d = std::llabs(s - t);
        if (d <= tolerance && (!best || d < std::llabs(*best - t)))
            best = s;
    }
    // The nearest grid line (events, markers and the cursor win when as close).
    if (const TimeUs g = gridStep(); g > 0) {
        const double n = std::round(double(t - m_gridOffset) / double(g));
        const TimeUs line = std::max<TimeUs>(0, m_gridOffset + TimeUs(std::llround(n * double(g))));
        const TimeUs d = std::llabs(line - t);
        if (d <= tolerance && (!best || d < std::llabs(*best - t)))
            best = line;
    }
    return best;
}

TimeUs TimelineWidget::gridStep() const
{
    switch (m_grid) {
    case Grid::Frames: return m_project->settings().fps.frameDuration();
    case Grid::HalfSeconds: return kUsPerSecond / 2;
    case Grid::Seconds: return kUsPerSecond;
    case Grid::Beats: return TimeUs(60.0 * kUsPerSecond / std::clamp(m_gridBpm, 20.0, 400.0));
    case Grid::Off: break;
    }
    return 0;
}

void TimelineWidget::setGrid(Grid grid)
{
    m_grid = grid;
    viewport()->update();
}

void TimelineWidget::setGridTempo(double bpm, TimeUs firstBeat)
{
    m_gridBpm = std::clamp(bpm, 20.0, 400.0);
    const TimeUs beat = TimeUs(60.0 * kUsPerSecond / m_gridBpm);
    m_gridOffset = ((firstBeat % beat) + beat) % beat; // the earliest beat at or after 0
    viewport()->update();
}

void TimelineWidget::paintGrid(QPainter& p, const QRect& band)
{
    const TimeUs g = gridStep();
    if (g <= 0 || double(g) * m_pxPerSec / kUsPerSecond < 5) // too dense to draw (snapping still works)
        return;
    const TimeUs from = xToTime(band.left()), to = xToTime(band.right());
    int64_t n = int64_t(std::floor(double(from - m_gridOffset) / double(g)));
    for (TimeUs t = m_gridOffset + n * g; t <= to; t += g, ++n) {
        if (t < 0)
            continue;
        const bool strong = m_grid == Grid::Beats ? n % 4 == 0 : m_grid == Grid::Frames ? false : (t % (5 * kUsPerSecond)) == 0;
        p.setPen(ink(strong ? 70 : 34));
        const double x = timeToX(t);
        p.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
    }
}

TimeUs TimelineWidget::snapped(TimeUs t, Qt::KeyboardModifiers mods) const
{
    if (m_snapping && !(mods & Qt::ShiftModifier))
        if (auto s = snapTo(t))
            return *s;
    return quantize(t);
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void TimelineWidget::mousePressEvent(QMouseEvent* e)
{
    setFocus();
    const QPoint p = e->position().toPoint();
    m_pressPos = p;
    m_dragMoved = false;
    m_drag = Drag::None;
    const bool ctrl = e->modifiers() & Qt::ControlModifier;

    if (e->button() == Qt::RightButton) {
        const Hit hit = hitTest(p);
        if (hit.clipId && !m_selection.count(hit.clipId))
            setSelection(ops::expandToGroups(m_project->timeline(), {hit.clipId}));
        if (hit.track >= 0)
            m_selectedTrack = hit.track;
        return;
    }
    if (e->button() != Qt::LeftButton)
        return;

    if (p.y() < kTopH && p.x() >= kHeaderWidth) {
        buildSnapPoints({});
        const TimeUs t = std::max<TimeUs>(0, snapped(xToTime(p.x()), e->modifiers()));
        if (p.y() < kRegionBarH) {
            if (hasRegion()) {
                const double xa = timeToX(m_regionStart), xb = timeToX(m_regionEnd);
                if (std::abs(p.x() - xa) <= 5) {
                    m_drag = Drag::RegionStart;
                    return;
                }
                if (std::abs(p.x() - xb) <= 5) {
                    m_drag = Drag::RegionEnd;
                    return;
                }
                if (p.x() > xa && p.x() < xb) {
                    m_drag = Drag::RegionMove;
                    m_regionPressStart = m_regionStart;
                    m_regionPressEnd = m_regionEnd;
                    m_pressTime = xToTime(p.x());
                    return;
                }
            }
            m_drag = Drag::RegionNew;
            m_pressTime = t;
            return;
        }
        if (p.y() < kRulerTop) {
            int part = 0;
            if (const int id = markerAt(p, &part)) {
                m_before = m_project->timeline();
                if (const Marker* m = m_before.findMarker(id)) {
                    m_dragMarker = id;
                    m_pressTime = xToTime(p.x());
                    m_markerPressTime = m->time;
                    m_markerPressLength = m->length;
                    m_drag = part == 1 ? Drag::MarkerStart : part == 2 ? Drag::MarkerEnd : Drag::MarkerMove;
                    buildSnapPoints({}, id);
                    return;
                }
            }
        }
        m_drag = Drag::Scrub;
        const TimeUs c = std::max<TimeUs>(0, quantize(xToTime(p.x())));
        setCursorTime(c, false);
        emit cursorMoved(c);
        emit scrubbed(c);
        return;
    }

    if (p.x() < kHeaderWidth) {
        headerPress(p);
        return;
    }

    if (const EnvHit eh = envelopeHit(p); eh.track >= 0) {
        m_selectedTrack = eh.track;
        m_before = m_project->timeline();
        m_envDrag = eh;
        m_envPressTime = xToTime(p.x());
        m_drag = eh.point >= 0 ? Drag::EnvelopePoint : Drag::EnvelopeSegment;
        buildSnapPoints({});
        viewport()->update();
        return;
    }

    const Hit hit = hitTest(p);
    if (hit.track >= 0)
        m_selectedTrack = hit.track;
    if (hit.clipId) {
        const std::set<int> group = ops::expandToGroups(m_project->timeline(), {hit.clipId});
        std::set<int> sel = m_selection;
        m_ctrlDeselect = false;
        if (ctrl && !(e->modifiers() & Qt::AltModifier)) { // (Ctrl+Alt is a slide, not a selection change)
            // Ctrl+click toggles it (taken out on release, if not dragged); Ctrl+drag copies.
            if (sel.count(hit.clipId))
                m_ctrlDeselect = true;
            else
                sel.insert(group.begin(), group.end());
        } else if (!sel.count(hit.clipId)) {
            sel = group;
        }
        setSelection(sel);
        if (!m_selection.count(hit.clipId))
            return;
        if (hit.zone == Zone::FxBadge) {
            emit clipEffectsRequested(hit.clipId);
            return;
        }
        if (hit.zone == Zone::PanCropBadge) {
            emit panCropRequested(hit.clipId);
            return;
        }
        if (hit.zone == Zone::TransitionRemove) {
            removeTransitionAt(hit.clipId, hit.transitionIn);
            return;
        }
        if (hit.zone == Zone::TransitionLabel) {
            int ti = -1;
            const Timeline& tl = m_project->timeline();
            if (const Clip* c = tl.findClip(hit.clipId, &ti)) {
                const Track* t = &tl.tracks[size_t(ti)];
                // Audio fades have no settings of their own: show the grouped video's transition.
                if (t->type == TrackType::Audio) {
                    const Clip* video = nullptr;
                    int vti = -1;
                    for (int id : ops::expandToGroups(tl, {c->id}))
                        if (const Clip* x = tl.findClip(id, &vti); x && isVisualTrack(tl.tracks[size_t(vti)].type)
                                                                   && (hit.transitionIn ? x->start == c->start : x->end() == c->end())) {
                            video = x;
                            break;
                        }
                    if (!video) {
                        emit message(tr("An audio fade: drag its handle at the top corner to change its length, or click \u00D7 to remove it."));
                        return;
                    }
                    c = video;
                    t = &tl.tracks[size_t(vti)];
                }
                const EndTransition e = endTransition(*t, size_t(c - t->clips.data()), hit.transitionIn);
                if (e.holder)
                    emit transitionRequested(e.holder, e.holderIn);
            }
            return;
        }
        m_before = m_project->timeline();
        m_dragClip = hit.clipId;
        m_pressTrack = hit.track;
        m_pressTime = xToTime(p.x());
        m_copyDrag = false;
        // Ctrl+Alt on an edge another event touches: roll the cut between them.
        if ((hit.zone == Zone::LeftEdge || hit.zone == Zone::RightEdge) && ctrl && (e->modifiers() & Qt::AltModifier)) {
            const Track& track = m_before.tracks[size_t(hit.track)];
            const Clip* c = m_before.findClip(hit.clipId);
            const TimeUs half = m_project->settings().fps.frameDuration() / 2;
            for (const Clip& o : track.clips) {
                if (o.id == c->id)
                    continue;
                if (hit.zone == Zone::RightEdge && std::llabs(o.start - c->end()) <= half) {
                    m_rollLeft = c->id;
                    m_rollRight = o.id;
                    m_drag = Drag::Roll;
                } else if (hit.zone == Zone::LeftEdge && std::llabs(o.end() - c->start) <= half) {
                    m_rollLeft = o.id;
                    m_rollRight = c->id;
                    m_drag = Drag::Roll;
                }
            }
            if (m_drag == Drag::Roll) {
                buildSnapPoints({m_rollLeft, m_rollRight});
                viewport()->update();
                return;
            }
        }
        switch (hit.zone) {
        case Zone::LeftEdge: m_drag = Drag::TrimLeft; break;
        case Zone::RightEdge: m_drag = Drag::TrimRight; break;
        case Zone::FadeIn: m_drag = Drag::FadeIn; break;
        case Zone::FadeOut: m_drag = Drag::FadeOut; break;
        case Zone::Gain: m_drag = Drag::Gain; break;
        default:
            // Alt: slip the footage inside the event; Ctrl+Alt: slide the event between its neighbours.
            if (e->modifiers() & Qt::AltModifier) {
                m_drag = ctrl ? Drag::Slide : Drag::Slip;
            } else {
                m_drag = Drag::Move;
                m_copyDrag = ctrl; // Ctrl+drag copies
            }
            break;
        }
        buildSnapPoints(m_selection);
        viewport()->update();
        return;
    }

    // Empty space: deselect and move the cursor; dragging draws a box that
    // selects the events it touches (Ctrl adds to the selection), Shift+drag
    // makes a time selection.
    if (!ctrl)
        setSelection({});
    const TimeUs t = std::max<TimeUs>(0, quantize(xToTime(p.x())));
    setCursorTime(t, false);
    emit cursorMoved(t);
    buildSnapPoints({});
    m_pressTime = t;
    if (e->modifiers() & Qt::ShiftModifier) {
        m_drag = Drag::TimeSelect;
    } else {
        m_drag = Drag::RubberBand;
        m_bandBase = m_selection;
        m_bandTime = xToTime(p.x());
        m_bandY = p.y() + m_scrollY;
        m_bandNow = p;
    }
    viewport()->update();
}

void TimelineWidget::headerPress(QPoint p)
{
    const int ti = trackAt(p.y());
    if (ti < 0)
        return;
    m_selectedTrack = ti;
    const Track& track = m_project->timeline().tracks[ti];
    const QRect hr = headerRect(ti);
    if (p.y() >= hr.bottom() - 3) {
        m_drag = Drag::TrackResize;
        m_pressTrack = ti;
        m_resizeStartHeight = trackHeight(track);
        return;
    }
    if (fxRect(hr).contains(p)) {
        emit trackEffectsRequested(track.id);
        return;
    }
    if (muteRect(hr).contains(p) || soloRect(hr).contains(p)) {
        Timeline tl = m_project->timeline();
        const bool mute = muteRect(hr).contains(p);
        if (mute)
            tl.tracks[ti].muted = !tl.tracks[ti].muted;
        else
            tl.tracks[ti].solo = !tl.tracks[ti].solo;
        m_project->applyEdit(tl, mute ? tr("Mute Track") : tr("Solo Track"));
        return;
    }
    const bool hasPan = track.type == TrackType::Audio && hr.height() >= 62;
    if (levelRect(hr).contains(p) || (hasPan && panRect(hr).contains(p))) {
        m_before = m_project->timeline();
        m_pressTrack = ti;
        m_drag = levelRect(hr).contains(p) ? Drag::TrackLevel : Drag::TrackPan;
        applyTrackLevel(p);
        return;
    }
    if (isVisualTrack(track.type) && hr.height() >= 62 && blendRect(hr).contains(p)) {
        QMenu menu(this);
        const QStringList modes = blendModeNames();
        for (int i = 0; i < int(modes.size()); ++i) {
            QAction* a = menu.addAction(modes[i], this, [this, ti, i] { setTrackBlend(ti, i); });
            a->setCheckable(true);
            a->setChecked(track.blend == i);
        }
        menu.exec(viewport()->mapToGlobal(blendRect(hr).bottomLeft()));
        return;
    }
    emit trackSelected(track.id);
    // Dragging the header moves the track.
    m_drag = Drag::TrackReorder;
    m_pressTrack = ti;
    m_reorderGap = -1;
    viewport()->update();
}

void TimelineWidget::applyTrackLevel(QPoint p)
{
    if (m_pressTrack < 0 || m_pressTrack >= int(m_project->timeline().tracks.size()))
        return;
    const QRect hr = headerRect(m_pressTrack);
    Timeline tl = m_project->timeline();
    Track& track = tl.tracks[m_pressTrack];
    if (m_drag == Drag::TrackLevel) {
        const QRect r = levelRect(hr);
        const double pos = std::clamp(double(p.x() - r.left()) / r.width(), 0.0, 1.0);
        track.level = isVisualTrack(track.type) ? float(pos) : volumeFromPos(pos);
    } else {
        const QRect r = panRect(hr);
        float pan = float(std::clamp(double(p.x() - r.left()) / r.width() * 2.0 - 1.0, -1.0, 1.0));
        if (std::abs(pan) < 0.04f)
            pan = 0; // detent at centre
        track.pan = pan;
    }
    m_project->setTimelineLive(tl);
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* e)
{
    const QPoint p = e->position().toPoint();
    if (m_drag == Drag::None) {
        updateHoverCursor(p);
        return;
    }
    if (!m_dragMoved && (p - m_pressPos).manhattanLength() < 3)
        return;
    m_dragMoved = true;
    const TimeUs t = xToTime(p.x());
    switch (m_drag) {
    case Drag::Scrub: {
        const TimeUs c = std::max<TimeUs>(0, quantize(t));
        if (c == m_cursor)
            break;
        setCursorTime(c, false);
        emit cursorMoved(c);
        emit scrubbed(c);
        break;
    }
    case Drag::RegionNew:
    case Drag::TimeSelect:
        setRegion(m_pressTime, std::max<TimeUs>(0, snapped(t, e->modifiers())));
        break;
    case Drag::RubberBand: {
        m_bandNow = p;
        const TimeUs t0 = std::min(m_bandTime, t), t1 = std::max(m_bandTime, t);
        const int y0 = std::min(m_bandY, p.y() + m_scrollY), y1 = std::max(m_bandY, p.y() + m_scrollY);
        std::set<int> hits;
        const auto& tracks = m_project->timeline().tracks;
        for (int i = 0; i < int(tracks.size()); ++i) {
            const int top = trackTop(i) + m_scrollY, bottom = top + trackHeight(tracks[size_t(i)]);
            if (bottom < y0 || top > y1)
                continue;
            for (const Clip& c : tracks[size_t(i)].clips)
                if (c.end() > t0 && c.start < t1)
                    hits.insert(c.id);
        }
        std::set<int> sel = m_bandBase;
        const std::set<int> grouped = ops::expandToGroups(m_project->timeline(), hits);
        sel.insert(grouped.begin(), grouped.end());
        setSelection(std::move(sel));
        viewport()->update();
        break;
    }
    case Drag::RegionStart:
    case Drag::RegionEnd: {
        const TimeUs v = std::max<TimeUs>(0, snapped(t, e->modifiers()));
        const TimeUs other = m_drag == Drag::RegionStart ? m_regionEnd : m_regionStart;
        if ((m_drag == Drag::RegionStart && v > other) || (m_drag == Drag::RegionEnd && v < other))
            m_drag = m_drag == Drag::RegionStart ? Drag::RegionEnd : Drag::RegionStart;
        setRegion(v, other);
        break;
    }
    case Drag::RegionMove: {
        const TimeUs len = m_regionPressEnd - m_regionPressStart;
        const TimeUs a = std::max<TimeUs>(0, quantize(m_regionPressStart + (t - m_pressTime)));
        setRegion(a, a + len);
        break;
    }
    case Drag::Move: dragMove(p, e->modifiers()); break;
    case Drag::Roll: dragRoll(p, e->modifiers()); break;
    case Drag::Slip: dragSlip(p); break;
    case Drag::Slide: dragSlide(p, e->modifiers()); break;
    case Drag::MarkerMove:
    case Drag::MarkerStart:
    case Drag::MarkerEnd: dragMarker(p, e->modifiers()); break;
    case Drag::TrackReorder: {
        const int gap = reorderGap(p.y());
        const int gapNow = gap == m_pressTrack || gap == m_pressTrack + 1 ? -1 : gap;
        if (gapNow != m_reorderGap) {
            m_reorderGap = gapNow;
            viewport()->update();
        }
        viewport()->setCursor(Qt::ClosedHandCursor);
        break;
    }
    case Drag::TrimLeft: dragTrim(p, true, e->modifiers()); break;
    case Drag::TrimRight: dragTrim(p, false, e->modifiers()); break;
    case Drag::FadeIn: dragFade(p, true); break;
    case Drag::FadeOut: dragFade(p, false); break;
    case Drag::Gain: dragGain(p); break;
    case Drag::TrackLevel:
    case Drag::TrackPan: applyTrackLevel(p); break;
    case Drag::EnvelopePoint:
    case Drag::EnvelopeSegment: dragEnvelope(p, e->modifiers()); break;
    case Drag::TrackResize:
        if (m_pressTrack >= 0 && m_pressTrack < int(m_project->timeline().tracks.size())) {
            const int id = m_project->timeline().tracks[m_pressTrack].id;
            m_trackHeights[id] = std::clamp(m_resizeStartHeight + p.y() - m_pressPos.y(), 30, 400);
            updateScrollBars();
            viewport()->update();
        }
        break;
    case Drag::None: break;
    }
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* e)
{
    const Drag drag = m_drag;
    m_drag = Drag::None;
    switch (drag) {
    case Drag::Move:
        if (!m_dragMoved) {
            // A click on an event also places the cursor there (handy for S).
            const TimeUs t = quantize(xToTime(e->position().x()));
            setCursorTime(t, false);
            emit cursorMoved(t);
            if (m_ctrlDeselect) { // Ctrl+click on a selected event: out of the selection
                std::set<int> sel = m_selection;
                for (int id : ops::expandToGroups(m_project->timeline(), {m_dragClip}))
                    sel.erase(id);
                setSelection(sel);
            }
            m_project->commitEdit(m_before, tr("Move Event"));
        } else if (m_copyDrag) {
            m_project->commitEdit(m_before, tr("Copy Events"));
            setSelection(m_copyIds);
        } else {
            m_project->commitEdit(m_before, tr("Move Event"));
        }
        m_copyDrag = false;
        break;
    case Drag::Roll: m_project->commitEdit(m_before, tr("Trim Adjacent Events")); break;
    case Drag::TrimLeft:
    case Drag::TrimRight: m_project->commitEdit(m_before, tr("Trim Event")); break;
    case Drag::Slip: m_project->commitEdit(m_before, tr("Slip Event")); break;
    case Drag::Slide: m_project->commitEdit(m_before, tr("Slide Event")); break;
    case Drag::FadeIn:
    case Drag::FadeOut: m_project->commitEdit(m_before, tr("Fade")); break;
    case Drag::Gain: m_project->commitEdit(m_before, tr("Event Level")); break;
    case Drag::TrackLevel: m_project->commitEdit(m_before, tr("Track Level")); break;
    case Drag::TrackPan: m_project->commitEdit(m_before, tr("Track Pan")); break;
    case Drag::EnvelopePoint:
    case Drag::EnvelopeSegment:
        m_project->commitEdit(m_before, tr("Edit Envelope"));
        QToolTip::hideText();
        break;
    case Drag::RegionNew:
        if (!m_dragMoved)
            setRegion(0, 0);
        break;
    case Drag::MarkerMove:
    case Drag::MarkerStart:
    case Drag::MarkerEnd:
        if (!m_dragMoved) {
            // A click on a marker puts the cursor on it.
            if (const Marker* m = m_before.findMarker(m_dragMarker)) {
                const TimeUs t = drag == Drag::MarkerEnd ? m->end() : m->time;
                setCursorTime(t, false);
                emit cursorMoved(t);
            }
        } else {
            m_project->commitEdit(m_before, drag == Drag::MarkerMove ? tr("Move Marker") : tr("Resize Region"));
        }
        break;
    case Drag::RubberBand: viewport()->update(); break;
    case Drag::TrackReorder: {
        const int gap = m_reorderGap;
        m_reorderGap = -1;
        if (m_dragMoved && gap >= 0)
            moveTrack(m_pressTrack, gap > m_pressTrack ? gap - 1 : gap);
        viewport()->update();
        break;
    }
    default: break;
    }
    updateHoverCursor(e->position().toPoint());
}

void TimelineWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    const QPoint p = e->position().toPoint();
    if (p.x() >= kHeaderWidth && p.y() >= kRegionBarH && p.y() < kRulerTop) {
        // Marker bar: a region selects its span, a marker gets renamed,
        // empty space gets a new marker.
        if (const int id = markerAt(p)) {
            if (const Marker* m = m_project->timeline().findMarker(id); m && m->isRegion())
                setRegion(m->time, m->end());
            else
                renameMarker(id);
        } else {
            buildSnapPoints({});
            addMarker(snapped(xToTime(p.x()), e->modifiers()));
        }
        return;
    }
    if (p.x() < kHeaderWidth) {
        const int ti = trackAt(p.y());
        if (ti < 0)
            return;
        const QRect hr = headerRect(ti);
        Timeline tl = m_project->timeline();
        if (levelRect(hr).contains(p)) {
            tl.tracks[ti].level = 1.0f; // reset to unity
            m_project->applyEdit(tl, tr("Track Level"));
        } else if (panRect(hr).contains(p) && tl.tracks[ti].type == TrackType::Audio) {
            tl.tracks[ti].pan = 0.0f;
            m_project->applyEdit(tl, tr("Track Pan"));
        } else if (blendRect(hr).contains(p) && isVisualTrack(tl.tracks[ti].type)) {
            // (the compositing mode menu opened on the first click)
        } else if (!muteRect(hr).contains(p) && !soloRect(hr).contains(p) && !fxRect(hr).contains(p)) {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Track"), tr("Track name:"), QLineEdit::Normal,
                                                       tl.tracks[ti].name, &ok);
            if (ok) {
                tl.tracks[ti].name = name;
                m_project->applyEdit(tl, tr("Rename Track"));
            }
        }
        return;
    }
    // Double-clicking an envelope line adds a point there.
    if (const EnvHit eh = envelopeHit(p); eh.track >= 0) {
        Timeline tl = m_project->timeline();
        Envelope* env = findEnvelope(tl, eh);
        if (eh.point < 0 && env) {
            const EnvBand band = envBand(tl, eh);
            const TimeUs t = std::clamp(quantize(xToTime(p.x())) - band.origin, TimeUs(0), band.maxT);
            ParamValue pv;
            pv.keys = env->points;
            pv.value = env->value;
            pv.addKey(t, env->at(t));
            env->points = pv.keys;
            if (eh.clipId)
                ops::syncGroupRamps(tl, eh.clipId);
            m_project->applyEdit(tl, tr("Add Envelope Point"));
        }
        return;
    }
    // Double-clicking generated media opens its settings; other events
    // select their time span as the loop region.
    const Hit hit = hitTest(p);
    if (hit.clipId)
        if (const Clip* c = m_project->timeline().findClip(hit.clipId)) {
            if(c->isSequence()) emit sequenceRequested(c->id);
            else if (c->isGenerated())
                emit generatorRequested(c->id);
            else
                setRegion(c->start, c->end());
        }
}

void TimelineWidget::dragSlip(QPoint p)
{
    // The footage moves with the mouse (dragging right shows earlier footage),
    // as far as every selected event's media allows.
    const TimeUs delta = quantize(xToTime(p.x()) - m_pressTime);
    TimeUs lo = std::numeric_limits<TimeUs>::min(), hi = std::numeric_limits<TimeUs>::max();
    struct Slipped {
        int id;
        double rate; // media per timeline time (negative: reversed)
    };
    std::vector<Slipped> clips;
    for (const Track& t : m_before.tracks)
        for (const Clip& c : t.clips) {
            if (!m_selection.count(c.id) || c.isGenerated())
                continue;
            const auto media = m_project->mediaItem(c.mediaId);
            if (!media || media->info.isStill || media->info.duration <= 0)
                continue;
            const double rate = c.reverse ? -c.speed : c.speed;
            // sourceIn - delta * rate must stay within 0 .. duration - span.
            const double room = double(media->info.duration - c.sourceSpan());
            if (room < 0)
                continue;
            const double a = (double(c.sourceIn) - room) / rate, b = double(c.sourceIn) / rate;
            lo = std::max(lo, TimeUs(std::ceil(std::min(a, b))));
            hi = std::min(hi, TimeUs(std::floor(std::max(a, b))));
            clips.push_back({c.id, rate});
        }
    if (clips.empty())
        return;
    const TimeUs d = lo > hi ? 0 : std::clamp(delta, lo, hi);
    Timeline tl = m_before;
    for (const Slipped& s : clips)
        if (Clip* c = tl.findClip(s.id))
            c->sourceIn = std::max<TimeUs>(0, c->sourceIn - TimeUs(std::llround(double(d) * s.rate)));
    m_project->setTimelineLive(tl);
    emit message(tr("Slip: the footage %1 by %2 s").arg(d >= 0 ? tr("moves later") : tr("moves earlier")).arg(std::abs(double(d)) / 1e6, 0, 'f', 2));
}

void TimelineWidget::dragSlide(QPoint p, Qt::KeyboardModifiers mods)
{
    // The clicked event (and what's grouped with it) moves; on each of its tracks
    // the event just before ends where it now starts, and the one just after
    // starts where it now ends (its footage trimmed to match).
    TimeUs delta = quantize(xToTime(p.x()) - m_pressTime);
    if (m_snapping && !(mods & Qt::ShiftModifier))
        if (const Clip* anchor = m_before.findClip(m_dragClip))
            if (const auto s = snapTo(anchor->start + delta))
                delta = *s - anchor->start;
    const std::set<int> moving = ops::expandToGroups(m_before, {m_dragClip});
    TimeUs lo = std::numeric_limits<TimeUs>::min(), hi = std::numeric_limits<TimeUs>::max();
    constexpr TimeUs kMinLength = 40000; // neighbours keep at least a frame or so
    for (const Track& t : m_before.tracks)
        for (size_t i = 0; i < t.clips.size(); ++i) {
            const Clip& c = t.clips[i];
            if (!moving.count(c.id))
                continue;
            const Clip* prev = i > 0 && !moving.count(t.clips[i - 1].id) ? &t.clips[i - 1] : nullptr;
            const Clip* next = i + 1 < t.clips.size() && !moving.count(t.clips[i + 1].id) ? &t.clips[i + 1] : nullptr;
            if (prev && prev->end() == c.start) { // it lengthens or shortens with the slide
                lo = std::max(lo, kMinLength - prev->length);
                if (!prev->isGenerated())
                    if (const auto m = m_project->mediaItem(prev->mediaId); m && !m->info.isStill)
                        hi = std::min(hi, ops::maxClipLength(m.get(), *prev) - prev->length);
            } else if (prev) {
                lo = std::max(lo, prev->end() - c.start); // not into it
            } else {
                lo = std::max(lo, -c.start);
            }
            if (next && next->start == c.end()) { // its start follows (and its footage)
                hi = std::min(hi, next->length - kMinLength);
                const auto range = next->isGenerated() ? std::pair<TimeUs, TimeUs>{-next->start, next->length}
                                                       : ops::trimRange(*next, m_project->mediaItem(next->mediaId).get(), true);
                lo = std::max(lo, range.first);
            } else if (next) {
                hi = std::min(hi, next->start - c.end());
            }
        }
    const TimeUs d = lo > hi ? 0 : std::clamp(delta, lo, hi);
    Timeline tl = m_before;
    for (Track& t : tl.tracks) {
        const std::vector<Clip> before = t.clips;
        for (size_t i = 0; i < before.size(); ++i) {
            if (!moving.count(before[i].id))
                continue;
            if (i > 0 && !moving.count(before[i - 1].id) && before[i - 1].end() == before[i].start)
                t.clips[i - 1].length += d;
            if (i + 1 < before.size() && !moving.count(before[i + 1].id) && before[i + 1].start == before[i].end())
                ops::trimClipStart(t.clips[i + 1], d);
            t.clips[i].start += d;
        }
    }
    m_project->setTimelineLive(tl);
    emit message(tr("Slide: %1 s").arg(double(d) / 1e6, 0, 'f', 2));
}

void TimelineWidget::dragMove(QPoint p, Qt::KeyboardModifiers mods)
{
    Timeline tl = m_before;
    const Clip* anchor = m_before.findClip(m_dragClip);
    if (!anchor)
        return;
    TimeUs minStart = std::numeric_limits<TimeUs>::max();
    for (const auto& t : m_before.tracks)
        for (const auto& c : t.clips)
            if (m_selection.count(c.id))
                minStart = std::min(minStart, c.start);

    TimeUs delta = quantize(xToTime(p.x()) - m_pressTime);
    if (m_snapping && !(mods & Qt::ShiftModifier)) {
        const auto s1 = snapTo(anchor->start + delta);
        const auto s2 = snapTo(anchor->end() + delta);
        const TimeUs d1 = s1 ? *s1 - (anchor->start + delta) : 0;
        const TimeUs d2 = s2 ? *s2 - (anchor->end() + delta) : 0;
        if (s1 && (!s2 || std::llabs(d1) <= std::llabs(d2)))
            delta += d1;
        else if (s2)
            delta += d2;
    }
    delta = std::max(delta, -minStart);

    // Vertical: move between tracks of the same kind.
    int trackDelta = 0;
    const int target = trackAt(p.y());
    if (target >= 0)
        trackDelta = target - m_pressTrack;
    if (trackDelta != 0) {
        for (size_t i = 0; i < m_before.tracks.size() && trackDelta != 0; ++i) {
            bool hasSel = false;
            for (const auto& c : m_before.tracks[i].clips)
                hasSel |= m_selection.count(c.id) > 0;
            if (!hasSel)
                continue;
            const int j = int(i) + trackDelta;
            if (j < 0 || j >= int(m_before.tracks.size()) || m_before.tracks[j].type != m_before.tracks[i].type)
                trackDelta = 0;
        }
    }

    std::vector<std::pair<int, Clip>> moving;
    for (size_t i = 0; i < tl.tracks.size(); ++i) {
        auto& clips = tl.tracks[i].clips;
        for (const auto& c : clips)
            if (m_selection.count(c.id))
                moving.emplace_back(int(i), c);
        if (!m_copyDrag) // (copies: the originals stay where they are)
            clips.erase(std::remove_if(clips.begin(), clips.end(), [&](const Clip& c) { return m_selection.count(c.id) > 0; }),
                        clips.end());
    }
    std::set<int> placed;
    std::map<int, int> newGroups;
    m_copyIds.clear();
    for (auto& [i, c] : moving) {
        if (m_copyDrag) { // a copy: new ids, its own group (video and audio together)
            c.id = tl.allocId();
            reassignEffectIds(c.effects, [&] { return tl.allocId(); }); // effects keep their own state
            if (c.groupId) {
                auto [it, fresh] = newGroups.try_emplace(c.groupId, 0);
                if (fresh)
                    it->second = tl.allocId();
                c.groupId = it->second;
            }
            m_copyIds.insert(c.id);
        }
        c.start += delta;
        placed.insert(c.id);
        tl.tracks[i + trackDelta].clips.push_back(c);
    }
    tl.sortClips();
    // Auto ripple: what comes after the moved events moves with them.
    if (!m_copyDrag && m_ripple != RippleMode::Off && delta != 0)
        ops::rippleShift(tl, minStart, delta, rippleTracks(m_before, m_selection), m_selection, m_ripple == RippleMode::All);
    if (!m_autoCrossfade) // no crossfades: what they land on is cut
        ops::overwriteOverlaps(tl, placed);
    m_project->setTimelineLive(tl);
}

void TimelineWidget::dragTrim(QPoint p, bool left, Qt::KeyboardModifiers mods)
{
    Timeline tl = m_before;
    const Clip* anchor = m_before.findClip(m_dragClip);
    if (!anchor)
        return;
    const TimeUs edge = left ? anchor->start : anchor->end();
    const TimeUs target = snapped(xToTime(p.x()), mods);
    const TimeUs frame = m_project->settings().fps.frameDuration();
    // Ctrl+drag an edge time-stretches (changes speed, keeps the footage).
    const bool stretchMode = mods & Qt::ControlModifier;

    // Selected clips sharing this edge (e.g. grouped video + audio) trim together.
    std::vector<Clip*> clips;
    std::vector<int> clipTracks; // track id of each
    for (auto& t : tl.tracks)
        for (auto& c : t.clips)
            if (m_selection.count(c.id) && (left ? c.start : c.end()) == edge) {
                clips.push_back(&c);
                clipTracks.push_back(t.id);
            }
    // Auto ripple: the event keeps its start, what follows moves with its end.
    const bool ripple = m_ripple != RippleMode::Off;

    auto unboundedClip = [&](const Clip* c) {
        auto media = m_project->mediaItem(c->mediaId);
        return (media && media->info.isStill) || c->isGenerated();
    };
    TimeUs lo = std::numeric_limits<TimeUs>::min() / 4, hi = std::numeric_limits<TimeUs>::max() / 4;
    for (Clip* c : clips) {
        auto media = m_project->mediaItem(c->mediaId);
        const bool unbounded = unboundedClip(c);
        if (left) {
            if (!ripple)
                lo = std::max(lo, -c->start);
            hi = std::min(hi, c->length - frame);
        } else {
            lo = std::max(lo, frame - c->length);
        }
        if (stretchMode && !unbounded) {
            // Same footage, new length: keep the speed within limits.
            const double len = double(c->length) * c->speed;
            const TimeUs minLen = std::max(frame, TimeUs(len / ops::kMaxSpeed));
            const TimeUs maxLen = TimeUs(len / ops::kMinSpeed);
            if (left) {
                lo = std::max(lo, c->length - maxLen);
                hi = std::min(hi, c->length - minLen);
            } else {
                lo = std::max(lo, minLen - c->length);
                hi = std::min(hi, maxLen - c->length);
            }
        } else if (!unbounded) {
            // Can't show footage before the start of the media; after its end only
            // a plain event goes, looping: the file again from its start.
            const auto [rlo, rhi] = ops::trimRange(*c, media.get(), left);
            const bool loops = !left && media && media->info.duration > 0 && c->speed == 1.0 && !c->reverse && !c->hasRamp();
            lo = std::max(lo, rlo);
            if (!loops)
                hi = std::min(hi, rhi);
        }
    }
    if (lo > hi)
        return;
    const TimeUs d = std::clamp(target - edge, lo, hi);
    struct Before {
        TimeUs start, end, length;
    };
    std::vector<Before> before;
    for (const Clip* c : clips)
        before.push_back({c->start, c->end(), c->length});
    for (Clip* c : clips) {
        if (stretchMode && !unboundedClip(c)) {
            const TimeUs oldLength = c->length;
            const double span = double(c->sourceSpan());
            if (left)
                c->start += d;
            c->length += left ? -d : d;
            if (c->hasRamp()) {
                // The ramp stretches with the event.
                const double factor = double(c->length) / double(std::max<TimeUs>(1, oldLength));
                ops::scaleRampTime(*c, factor);
                c->speed = std::clamp(c->speed / factor, ops::kMinSpeed, ops::kMaxSpeed);
            } else {
                c->speed = std::clamp(span / double(std::max<TimeUs>(1, c->length)), ops::kMinSpeed, ops::kMaxSpeed);
            }
        } else if (left) {
            ops::trimClipStart(*c, d);
        } else {
            ops::trimClipEnd(*c, d);
            // Past the end of its file: it loops.
            if (auto media = m_project->mediaItem(c->mediaId); media && !unboundedClip(c) && media->info.duration > 0
                                                                && c->sourceIn + c->sourceSpan() > media->info.duration)
                c->loopLength = media->info.duration;
        }
        ops::clampFades(*c);
    }
    if (ripple && !clips.empty()) {
        std::set<int> trimmed;
        std::vector<std::tuple<int, TimeUs, TimeUs>> shifts; // track id, from, by
        size_t anchorIndex = 0;
        for (size_t i = 0; i < clips.size(); ++i) {
            Clip* c = clips[i];
            if (left)
                c->start = before[i].start;
            trimmed.insert(c->id);
            shifts.emplace_back(clipTracks[i], before[i].end, c->length - before[i].length);
            if (c->id == m_dragClip)
                anchorIndex = i;
        }
        // (clips[] point into tl: done with them before shifting re-sorts the tracks)
        if (m_ripple == RippleMode::All) {
            const auto& [track, from, by] = shifts[anchorIndex];
            ops::rippleShift(tl, from, by, {}, trimmed, true);
        } else {
            for (const auto& [track, from, by] : shifts)
                ops::rippleShift(tl, from, by, {track}, trimmed, false);
        }
    }
    tl.sortClips();
    if (!m_autoCrossfade) { // no crossfades: what the trimmed edge runs into is cut
        std::set<int> trimmed;
        for (const auto& t : tl.tracks)
            for (const Clip& c : t.clips)
                if (m_selection.count(c.id))
                    trimmed.insert(c.id);
        ops::overwriteOverlaps(tl, trimmed);
    }
    m_project->setTimelineLive(tl);
}

void TimelineWidget::dragRoll(QPoint p, Qt::KeyboardModifiers mods)
{
    // The cut between two touching events moves: one gets longer, the other
    // shorter (their grouped events with them); nothing else moves.
    Timeline tl = m_before;
    const Clip* L = m_before.findClip(m_rollLeft);
    const Clip* R = m_before.findClip(m_rollRight);
    if (!L || !R)
        return;
    const TimeUs edge = L->end();
    const TimeUs frame = m_project->settings().fps.frameDuration();
    auto bounded = [&](const Clip& c) {
        auto media = m_project->mediaItem(c.mediaId);
        return !c.isGenerated() && media && !media->info.isStill;
    };
    std::vector<int> lefts, rights;
    for (int id : ops::expandToGroups(m_before, {L->id}))
        if (const Clip* c = m_before.findClip(id); c && std::llabs(c->end() - edge) <= frame / 2)
            lefts.push_back(id);
    for (int id : ops::expandToGroups(m_before, {R->id}))
        if (const Clip* c = m_before.findClip(id); c && std::llabs(c->start - R->start) <= frame / 2)
            rights.push_back(id);
    TimeUs lo = std::numeric_limits<TimeUs>::min() / 4, hi = std::numeric_limits<TimeUs>::max() / 4;
    for (int id : lefts) { // their ends
        const Clip* c = m_before.findClip(id);
        lo = std::max(lo, frame - c->length);
        if (bounded(*c) && !c->loopLength)
            hi = std::min(hi, ops::trimRange(*c, m_project->mediaItem(c->mediaId).get(), false).second);
    }
    for (int id : rights) { // their starts
        const Clip* c = m_before.findClip(id);
        hi = std::min(hi, c->length - frame);
        if (bounded(*c))
            lo = std::max(lo, ops::trimRange(*c, m_project->mediaItem(c->mediaId).get(), true).first);
    }
    if (lo > hi)
        return;
    const TimeUs d = std::clamp(snapped(xToTime(p.x()), mods) - edge, lo, hi);
    for (int id : lefts)
        if (Clip* c = tl.findClip(id)) {
            ops::trimClipEnd(*c, d);
            c->fadeOut = std::min(c->fadeOut, c->length - std::min(c->fadeIn, c->length));
        }
    for (int id : rights)
        if (Clip* c = tl.findClip(id)) {
            ops::trimClipStart(*c, d);
            ops::clampFades(*c);
        }
    tl.sortClips();
    m_project->setTimelineLive(tl);
}

void TimelineWidget::dragFade(QPoint p, bool fadeIn)
{
    Timeline tl = m_before;
    const Clip* anchor = m_before.findClip(m_dragClip);
    if (!anchor)
        return;
    const TimeUs t = quantize(xToTime(p.x()));
    const TimeUs edge = fadeIn ? anchor->start : anchor->end();
    const TimeUs wanted = std::max<TimeUs>(0, fadeIn ? t - edge : edge - t);
    for (auto& track : tl.tracks)
        for (auto& c : track.clips) {
            if (!m_selection.count(c.id) || (fadeIn ? c.start : c.end()) != edge)
                continue;
            if (fadeIn)
                c.fadeIn = std::clamp<TimeUs>(wanted, 0, std::max<TimeUs>(0, c.length - c.fadeOut));
            else
                c.fadeOut = std::clamp<TimeUs>(wanted, 0, std::max<TimeUs>(0, c.length - c.fadeIn));
        }
    m_project->setTimelineLive(tl);
}

void TimelineWidget::dragGain(QPoint p)
{
    if (m_pressTrack < 0 || m_pressTrack >= int(m_before.tracks.size()))
        return;
    const int top = trackTop(m_pressTrack);
    const int h = trackHeight(m_before.tracks[m_pressTrack]);
    const double bodyTop = top + 1 + kTitleH;
    const double bodyH = std::max(1, h - 2 - kTitleH);
    const float gain = float(std::clamp(1.0 - (p.y() - bodyTop) / bodyH, 0.0, 1.0));
    const TrackType type = m_before.tracks[m_pressTrack].type;
    Timeline tl = m_before;
    for (auto& track : tl.tracks)
        if (track.type == type)
            for (auto& c : track.clips)
                if (m_selection.count(c.id))
                    c.gain = gain;
    m_project->setTimelineLive(tl);
}

void TimelineWidget::dragMarker(QPoint p, Qt::KeyboardModifiers mods)
{
    Timeline tl = m_before;
    Marker* m = tl.findMarker(m_dragMarker);
    if (!m)
        return;
    const TimeUs t = xToTime(p.x());
    const TimeUs frame = m_project->settings().fps.frameDuration();
    const TimeUs end = m_markerPressTime + m_markerPressLength;
    if (m_drag == Drag::MarkerMove) {
        m->time = std::max<TimeUs>(0, snapped(m_markerPressTime + (t - m_pressTime), mods));
    } else if (m_drag == Drag::MarkerStart) {
        m->time = std::clamp(snapped(t, mods), TimeUs(0), end - frame);
        m->length = end - m->time;
    } else {
        m->length = std::max(frame, snapped(t, mods) - m->time);
    }
    const TimeUs shown = m_drag == Drag::MarkerEnd ? m->end() : m->time;
    tl.sortMarkers(); // m is not valid after this
    m_project->setTimelineLive(tl);
    QToolTip::showText(viewport()->mapToGlobal(p + QPoint(14, 10)), formatTimecode(shown, m_project->settings().fps), viewport());
}

void TimelineWidget::updateHoverCursor(QPoint p)
{
    Qt::CursorShape shape = Qt::ArrowCursor;
    QString tip;
    const EnvHit eh = (p.x() >= kHeaderWidth && p.y() >= kTopH) ? envelopeHit(p) : EnvHit{};
    if (eh.track >= 0) {
        shape = eh.point >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor;
        tip = eh.point >= 0 ? tr("Drag to move the point; right-click for curve or delete")
                            : tr("Double-click to add a point; drag to move this segment");
        if (eh.clipId)
            tip += tr("\nSpeed ramp: up is faster, 100% is the dotted line; right-click for presets");
    } else if (p.x() >= kHeaderWidth && p.y() >= kTopH) {
        const Hit hit = hitTest(p);
        switch (hit.zone) {
        case Zone::LeftEdge:
        case Zone::RightEdge: shape = Qt::SizeHorCursor; tip = tr("Drag to trim, Ctrl+drag to change speed"); break;
        case Zone::FadeIn:
        case Zone::FadeOut: shape = Qt::PointingHandCursor; tip = tr("Drag to fade"); break;
        case Zone::Gain: shape = Qt::SizeVerCursor; tip = tr("Drag to change event level"); break;
        case Zone::FxBadge: shape = Qt::PointingHandCursor; tip = tr("Video event FX"); break;
        case Zone::PanCropBadge: shape = Qt::PointingHandCursor; tip = tr("Event Pan/Crop"); break;
        case Zone::TransitionLabel: shape = Qt::PointingHandCursor; tip = tr("Transition: click for its properties (type, settings)"); break;
        case Zone::TransitionRemove: shape = Qt::PointingHandCursor; tip = tr("Remove this transition (hard cut)"); break;
        default: break;
        }
        if (tip.isEmpty() && hit.clipId && p.y() < trackTop(hit.track) + 1 + kTitleH)
            if (const Clip* c = m_project->timeline().findClip(hit.clipId); c && c->groupId)
                tip = tr("Grouped: moves, trims, fades and transitions apply to the whole group (e.g. a video file's picture "
                         "and sound). Right-click > Ungroup (U) to edit them separately.");
    } else if (p.x() < kHeaderWidth) {
        const int ti = trackAt(p.y());
        if (ti >= 0 && p.y() >= headerRect(ti).bottom() - 3) {
            shape = Qt::SizeVerCursor;
        } else if (ti >= 0) {
            const QRect hr = headerRect(ti);
            const bool video = isVisualTrack(m_project->timeline().tracks[size_t(ti)].type);
            const Track& ht = m_project->timeline().tracks[size_t(ti)];
            if (video && hr.height() >= 62 && blendRect(hr).contains(p))
                tip = tr("Compositing mode: how this track mixes with the tracks below");
            else if (!video && ht.duckUnder && QRect(hr.right() - 104, hr.top() + 4, 25, 18).contains(p)) {
                const int src = m_project->timeline().trackIndex(ht.duckUnder);
                tip = tr("Auto duck: turned down %1 dB while \"%2\" has sound (right-click the header to change)")
                          .arg(double(ht.duckAmount))
                          .arg(src >= 0 ? m_project->timeline().tracks[size_t(src)].name : QString());
            }
            else if (!muteRect(hr).contains(p) && !soloRect(hr).contains(p) && !fxRect(hr).contains(p) && !levelRect(hr).contains(p)
                     && !(!video && panRect(hr).contains(p)))
                tip = tr("Drag to move the track; double-click to rename");
        }
    } else if (p.y() >= kRegionBarH && p.y() < kRulerTop) {
        int part = 0;
        if (const int id = markerAt(p, &part)) {
            shape = part ? Qt::SizeHorCursor : Qt::PointingHandCursor;
            const Marker* m = m_project->timeline().findMarker(id);
            tip = m && m->isRegion() ? tr("Region: drag to move, drag an edge to resize, double-click to select its time, right-click for more")
                                     : tr("Marker: click to go there, drag to move, double-click to rename, right-click for more");
        } else {
            tip = tr("Marker bar: double-click (or press M) to add a marker, R makes a region from the time selection");
        }
    } else if (p.y() < kRegionBarH && hasRegion()) {
        if (std::abs(p.x() - timeToX(m_regionStart)) <= 5 || std::abs(p.x() - timeToX(m_regionEnd)) <= 5)
            shape = Qt::SizeHorCursor;
    }
    viewport()->setCursor(shape);
    viewport()->setToolTip(tip);
}

void TimelineWidget::wheelEvent(QWheelEvent* e)
{
    const QPoint delta = e->angleDelta();
    const double x = e->position().x();
    if (e->modifiers() & Qt::ControlModifier || std::abs(delta.x()) > std::abs(delta.y())) {
        const int d = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
        m_viewStart = std::max<TimeUs>(0, m_viewStart - TimeUs(d * visibleDuration() / 1200));
        updateScrollBars();
        viewport()->update();
    } else if (e->modifiers() & Qt::ShiftModifier) {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y() / 2);
    } else {
        // Plain wheel zooms around the mouse.
        zoomAround(std::max<double>(x, kHeaderWidth), std::pow(1.0015, delta.y()));
    }
    e->accept();
}

void TimelineWidget::contextMenuEvent(QContextMenuEvent* e)
{
    const QPoint p = e->pos();
    QMenu menu(this);
    if (p.x() < kHeaderWidth && p.y() >= kTopH) {
        const int ti = trackAt(p.y());
        menu.addAction(tr("Insert Video Track"), this, [this] { insertTrack(TrackType::Video); });
        menu.addAction(tr("Insert Audio Track"), this, [this] { insertTrack(TrackType::Audio); });
        if (ti >= 0) {
            m_selectedTrack = ti;
            const Track& trk = m_project->timeline().tracks[size_t(ti)];
            menu.addSeparator();
            auto envAction = [&](const QString& label, EnvelopeKind k) {
                QAction* a = menu.addAction(label, this, [this, k] { toggleEnvelope(k); });
                a->setCheckable(true);
                a->setChecked(trk.envelope(k)->active);
            };
            if (trk.type == TrackType::Audio) {
                envAction(tr("Volume Envelope\tV"), EnvelopeKind::Volume);
                envAction(tr("Pan Envelope\tP"), EnvelopeKind::Pan);
                QMenu* duck = menu.addMenu(tr("Auto Duck Under"));
                duck->setToolTipsVisible(true);
                const float amount = trk.duckAmount;
                QAction* off = duck->addAction(tr("Off"), this, [this, ti, amount] { setTrackDuck(ti, 0, amount); });
                off->setCheckable(true);
                off->setChecked(trk.duckUnder == 0);
                for (const Track& other : m_project->timeline().tracks)
                    if (other.type == TrackType::Audio && other.id != trk.id) {
                        QAction* a = duck->addAction(other.name.isEmpty() ? tr("Audio") : other.name, this,
                                                     [this, ti, id = other.id, amount] { setTrackDuck(ti, id, amount); });
                        a->setToolTip(tr("Turn this track down while that one has sound (e.g. music under a voice)"));
                        a->setCheckable(true);
                        a->setChecked(trk.duckUnder == other.id);
                    }
                duck->addSeparator();
                QMenu* by = duck->addMenu(tr("Duck By"));
                for (float db : {-6.0f, -9.0f, -12.0f, -18.0f, -24.0f, -36.0f}) {
                    QAction* a = by->addAction(tr("%1 dB").arg(double(db)), this, [this, ti, db, under = trk.duckUnder] { setTrackDuck(ti, under, db); });
                    a->setCheckable(true);
                    a->setChecked(std::abs(trk.duckAmount - db) < 0.01f);
                }
            } else {
                envAction(tr("Composite Level Envelope\tV"), EnvelopeKind::Composite);
            }
            menu.addSeparator();
        }
        if (ti >= 0) {
            const Track& tr0 = m_project->timeline().tracks[ti];
            const int trackId = tr0.id;
            menu.addAction(tr0.type == TrackType::Image ? tr("Image Track FX...")
                         : isVisualTrack(tr0.type) ? tr("Video Track FX...") : tr("Audio Track FX..."), this,
                           [this, trackId] { emit trackEffectsRequested(trackId); });
            if (isVisualTrack(tr0.type)) {
                QMenu* blend = menu.addMenu(tr("Compositing Mode"));
                const QStringList modes = blendModeNames();
                for (int i = 0; i < int(modes.size()); ++i) {
                    QAction* a = blend->addAction(modes[i], this, [this, ti, i] { setTrackBlend(ti, i); });
                    a->setCheckable(true);
                    a->setChecked(tr0.blend == i);
                }
            }
            menu.addSeparator();
            const int n = int(m_project->timeline().tracks.size());
            menu.addAction(tr("Move Track Up"), this, [this, ti] { moveTrack(ti, ti - 1); })->setEnabled(ti > 0);
            menu.addAction(tr("Move Track Down"), this, [this, ti] { moveTrack(ti, ti + 1); })->setEnabled(ti + 1 < n);
        }
        if (ti >= 0) {
            menu.addSeparator();
            menu.addAction(tr("Delete Track"), this, [this, ti] {
                Timeline tl = m_project->timeline();
                ops::removeTrack(tl, ti);
                m_project->applyEdit(tl, tr("Delete Track"));
            });
        }
    } else if (p.x() >= kHeaderWidth && p.y() >= kRegionBarH && p.y() < kRulerTop) {
        markerMenu(menu, p);
    } else if (const EnvHit eh = p.y() >= kTopH ? envelopeHit(p) : EnvHit{}; eh.track >= 0) {
        envelopeMenu(menu, eh);
    } else if (p.y() >= kTopH) {
        const Hit hit = hitTest(p);
        if (hit.clipId) {
            if (const Clip* oc = m_project->timeline().findClip(hit.clipId); oc && !oc->isGenerated()) {
                const auto media = m_project->mediaItem(oc->mediaId);
                if (media && !media->info.ok()) {
                    const int mid = oc->mediaId;
                    menu.addAction(tr("Relink Media..."), this, [this, mid] { emit relinkRequested(mid); });
                    menu.addSeparator();
                }
            }
            menu.addAction(tr("Split at Cursor\tS"), this, &TimelineWidget::splitAtCursor);
            menu.addSeparator();
            if (m_project->timeline().tracks[hit.track].type == TrackType::Audio) {
                const int cid = hit.clipId;
                menu.addAction(tr("Audio Event FX..."), this, [this, cid] { emit clipEffectsRequested(cid); });
                const int tid = m_project->timeline().tracks[hit.track].id;
                menu.addAction(tr("Audio Track FX (whole track)..."), this, [this, tid] { emit trackEffectsRequested(tid); });
                menu.addAction(tr("Normalize"), this, &TimelineWidget::normalizeSelection);
                menu.addAction(tr("Detect Beats (Markers)..."), this, [this] { emit beatsRequested(); });
                menu.addAction(tr("Remove Fades / Transitions (both ends)"), this, &TimelineWidget::removeTransitionsFromSelection);
                menu.addSeparator();
            }
            if (isVisualTrack(m_project->timeline().tracks[hit.track].type)) {
                const int cid = hit.clipId;
                if (const Clip* gc = m_project->timeline().findClip(cid); gc && gc->isGenerated())
                    menu.addAction(tr("Edit Generated Media..."), this, [this, cid] { emit generatorRequested(cid); });
                menu.addAction(tr("Video Event FX..."), this, [this, cid] { emit clipEffectsRequested(cid); });
                const int tid = m_project->timeline().tracks[hit.track].id;
                menu.addAction(tr("Video Track FX (whole track)..."), this, [this, tid] { emit trackEffectsRequested(tid); });
                menu.addAction(tr("Event Pan/Crop..."), this, [this, cid] { emit panCropRequested(cid); });
                if (const Clip* sc = m_project->timeline().findClip(cid); sc && !sc->isGenerated())
                    menu.addAction(sc->stabilize > 0 ? tr("Stabilize (on)...") : tr("Stabilize..."), this, [this, cid] { emit stabilizeRequested(cid); });
                menu.addAction(tr("Freeze Frame at Cursor (2 s)\tShift+F"), this, [this] { insertFreezeFrame(2 * kUsPerSecond); });
                const Track& vt = m_project->timeline().tracks[size_t(hit.track)];
                const Clip* vc = m_project->timeline().findClip(cid);
                const size_t vi = size_t(vc - vt.clips.data());
                bool anyTransition = false;
                for (bool in : {true, false}) {
                    const EndTransition e = endTransition(vt, vi, in);
                    anyTransition |= e.spec != nullptr;
                    const QString current = e.spec ? endTransitionName(e, in) : tr("none");
                    QMenu* sub = menu.addMenu((in ? tr("Transition In (%1)") : tr("Transition Out (%1)")).arg(current));
                    QAction* none = sub->addAction(tr("None (hard cut)"), this, [this, cid, in] { removeTransitionAt(cid, in); });
                    none->setCheckable(true);
                    none->setChecked(!e.spec);
                    sub->addSeparator();
                    const QString type = e.spec ? (e.spec->isPlainFade() ? QStringLiteral("dissolve") : e.spec->type) : QString();
                    for (const TransitionDef& t : transitions()) {
                        QAction* a = sub->addAction(t.name, this, [this, cid, in, id = t.id] {
                            Timeline tl = m_project->timeline();
                            const auto [holder, isIn] = setTransition(tl, cid, in, id);
                            m_project->applyEdit(tl, tr("Set Transition"));
                            if (holder)
                                emit transitionRequested(holder, isIn);
                        });
                        a->setCheckable(true);
                        a->setChecked(t.id == type);
                    }
                    sub->addSeparator();
                    const int holder = e.holder;
                    const bool holderIn = e.holderIn;
                    sub->addAction(tr("Properties..."), this, [this, holder, holderIn] { emit transitionRequested(holder, holderIn); })
                        ->setEnabled(e.spec != nullptr);
                }
                menu.addAction(tr("Remove Transitions (both ends)"), this, &TimelineWidget::removeTransitionsFromSelection)
                    ->setEnabled(anyTransition || m_selection.size() > 1);
                menu.addSeparator();
            }
            if (const Clip* fc = m_project->timeline().findClip(hit.clipId)) {
                for (bool in : {true, false}) {
                    QMenu* curves = menu.addMenu(in ? tr("Fade In Curve") : tr("Fade Out Curve"));
                    const int now = in ? fc->fadeInCurve : fc->fadeOutCurve;
                    for (int k = 0; k < kFadeCurves; ++k) {
                        QAction* a = curves->addAction(tr(qPrintable(fadeCurveName(k))), this, [this, in, k] { setFadeCurveOnSelection(in, k); });
                        a->setCheckable(true);
                        a->setChecked(now == k);
                    }
                    curves->setToolTip(tr("Constant Power keeps the loudness even through an audio crossfade"));
                }
                menu.addSeparator();
            }
            menu.addAction(tr("Cut\tCtrl+X"), this, &TimelineWidget::cutSelection);
            menu.addAction(tr("Copy\tCtrl+C"), this, &TimelineWidget::copySelection);
            menu.addAction(tr("Delete\tDel"), this, [this] { deleteSelection(false); });
            menu.addAction(tr("Ripple Delete\tCtrl+Del"), this, [this] { deleteSelection(true); });
            menu.addSeparator();
            {
                // Grouped events (a video file's picture and sound) move, trim,
                // fade and get transitions together; ungroup to edit them apart.
                const Timeline& gtl = m_project->timeline();
                const Clip* gc = gtl.findClip(hit.clipId);
                bool hasVideo = false, hasAudio = false;
                for (int id : ops::expandToGroups(gtl, {hit.clipId})) {
                    int gti = -1;
                    if (gtl.findClip(id, &gti))
                        (isVisualTrack(gtl.tracks[size_t(gti)].type) ? hasVideo : hasAudio) = true;
                }
                if (gc && gc->groupId)
                    menu.addAction(hasVideo && hasAudio ? tr("Ungroup Video and Audio\tU") : tr("Ungroup\tU"), this,
                                   &TimelineWidget::ungroupSelection);
                menu.addAction(tr("Group Selected Events\tG"), this, &TimelineWidget::groupSelection)->setEnabled(m_selection.size() > 1);
            }
            menu.addAction(tr("Mute / Unmute Event"), this, &TimelineWidget::toggleMuteSelection);
            {
                QMenu* speedMenu = menu.addMenu(tr("Playback Speed"));
                for (double sp : {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0})
                    speedMenu->addAction(QStringLiteral("%1%").arg(sp * 100), this, [this, sp] { setSpeedOnSelection(sp); });
                speedMenu->addSeparator();
                speedMenu->addAction(tr("Reverse"), this, &TimelineWidget::toggleReverseOnSelection);
                const int pid = hit.clipId;
                speedMenu->addAction(tr("Custom..."), this, [this, pid] { emit propertiesRequested(pid); });
            }
            if (const Clip* hc = m_project->timeline().findClip(hit.clipId); hc && !hc->isGenerated()) {
                QMenu* rampMenu = menu.addMenu(tr("Speed Ramp"));
                QAction* va = rampMenu->addAction(tr("Velocity Envelope\tShift+V"), this, &TimelineWidget::toggleVelocityEnvelope);
                va->setCheckable(true);
                va->setChecked(hc->hasRamp());
                rampMenu->addSeparator();
                for (auto preset : {ops::RampPreset::SpeedUp, ops::RampPreset::SlowDown, ops::RampPreset::SlowMiddle,
                                    ops::RampPreset::FastMiddle, ops::RampPreset::FreezeMiddle})
                    rampMenu->addAction(ops::rampPresetName(preset), this, [this, preset] { applyRampPresetToSelection(preset); });
            }
            menu.addAction(tr("Remove Fades"), this, [this] {
                Timeline tl = m_project->timeline();
                for (int id : m_selection)
                    if (Clip* c = tl.findClip(id))
                        c->fadeIn = c->fadeOut = 0;
                m_project->applyEdit(tl, tr("Remove Fades"));
            });
            menu.addAction(tr("Reset Event Level"), this, [this] {
                Timeline tl = m_project->timeline();
                for (int id : m_selection)
                    if (Clip* c = tl.findClip(id))
                        c->gain = 1.0f;
                m_project->applyEdit(tl, tr("Reset Event Level"));
            });
            menu.addSeparator();
            const int clipId = hit.clipId;
            menu.addAction(tr("Event Properties..."), this, [this, clipId] { emit propertiesRequested(clipId); });
            menu.addAction(tr("Media Properties..."), this, [this, clipId] {
                if (const Clip* c = m_project->timeline().findClip(clipId))
                    if (auto m = m_project->mediaItem(c->mediaId))
                        QMessageBox::information(this, tr("Media Properties"), m->info.summary());
            });
        } else {
            QAction* pasteAct = menu.addAction(tr("Paste\tCtrl+V"), this, &TimelineWidget::paste);
            pasteAct->setEnabled(!m_clipboard.empty());
            menu.addSeparator();
            const TimeUs at = quantize(xToTime(p.x()));
            menu.addAction(tr("Insert Marker Here"), this, [this, at] { addMarker(at); });
            menu.addAction(tr("Insert Video Track"), this, [this] { insertTrack(TrackType::Video); });
            menu.addAction(tr("Insert Audio Track"), this, [this] { insertTrack(TrackType::Audio); });
            menu.addAction(tr("Select All\tCtrl+A"), this, &TimelineWidget::selectAll);
        }
    } else {
        return;
    }
    menu.exec(e->globalPos());
}

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------

void TimelineWidget::dragEnterEvent(QDragEnterEvent* e)
{
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QString::fromLatin1(MediaBin::kMimeType)) || md->hasUrls()
        || md->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime))
        || md->hasFormat(QString::fromLatin1(EffectsBrowser::kTransitionMime))
        || md->hasFormat(QString::fromLatin1(EffectsBrowser::kGeneratorMime))
        || md->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime))) {
        buildSnapPoints({});
        e->acceptProposedAction();
    }
}

void TimelineWidget::dragMoveEvent(QDragMoveEvent* e)
{
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime)) || md->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime))
        || md->hasFormat(QString::fromLatin1(EffectsBrowser::kTransitionMime))) {
        // FX go onto events (or track headers) of the matching kind;
        // transitions only onto video events.
        const QPoint p = e->position().toPoint();
        const int ti = trackAt(p.y());
        TrackType wanted = TrackType::Video;
        if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime)))
            if (const auto meta = effectMeta(QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kEffectMime)))); meta && meta->audio)
                wanted = TrackType::Audio;
        if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime)))
            if (const auto preset = PresetLibrary::instance().find(QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kPresetMime))));
                preset && preset->audio)
                wanted = TrackType::Audio;
        const bool matches = ti >= 0 && sameTrackEffectKind(m_project->timeline().tracks[ti].type, wanted);
        const bool isEffect = md->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime)) || md->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime));
        const int clip = matches && p.x() >= kHeaderWidth ? hitTest(p).clipId : 0;
        // Effects: on an event = Event FX; on the header or empty lane = Track FX.
        const int fxClip = isEffect ? clip : 0;
        const int fxTrack = isEffect && matches && !clip ? ti : -1;
        if (fxClip != m_fxDropClip || fxTrack != m_fxDropTrack) {
            m_fxDropClip = fxClip;
            m_fxDropTrack = fxTrack;
            viewport()->update();
        }
        if (matches && (isEffect || clip))
            e->acceptProposedAction();
        else
            e->ignore();
        return;
    }
    // Media: show where it will land (start of the timeline / joined to the last clip).
    if (md->hasFormat(QString::fromLatin1(MediaBin::kMimeType)) || md->hasUrls()) {
        if (m_snapPoints.empty())
            buildSnapPoints({});
        const QPoint p = e->position().toPoint();
        m_dropX = int(timeToX(mediaDropTime(p.x(), trackAt(p.y()), e->modifiers())));
    } else {
        m_dropX = int(e->position().x());
    }
    viewport()->update();
    e->acceptProposedAction();
}

TimeUs TimelineWidget::mediaDropTime(int x, int track, Qt::KeyboardModifiers mods) const
{
    constexpr TimeUs kJoinDistance = 3 * kUsPerSecond;
    const TimeUs at = std::max<TimeUs>(0, xToTime(std::max(x, kHeaderWidth)));
    if (mods & Qt::ShiftModifier)
        return quantize(at);
    const Timeline& tl = m_project->timeline();
    TimeUs trackEnd = -1, timelineEnd = -1;
    for (size_t i = 0; i < tl.tracks.size(); ++i)
        for (const Clip& c : tl.tracks[i].clips) {
            timelineEnd = std::max(timelineEnd, c.end());
            if (int(i) == track)
                trackEnd = std::max(trackEnd, c.end());
        }
    if (timelineEnd < 0)
        return 0; // first clip: the start of the timeline
    const TimeUs join = trackEnd >= 0 ? trackEnd : timelineEnd;
    if (std::llabs(at - join) <= kJoinDistance)
        return join;
    return std::max<TimeUs>(0, snapped(at, mods));
}

int TimelineWidget::runDropTest(QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        if (!ok)
            ++failures;
    };
    if (m_project->media().empty()) {
        check(false, QStringLiteral("drop test: no media in the project"));
        return failures;
    }
    const int mediaId = m_project->media().begin()->first;
    Timeline empty = m_project->timeline();
    for (auto& t : empty.tracks)
        t.clips.clear();
    m_project->applyEdit(empty, QStringLiteral("test"));
    m_viewStart = 0;
    m_pxPerSec = 20;
    const int videoTrack = [&] {
        for (int i = 0; i < int(m_project->timeline().tracks.size()); ++i)
            if (isVisualTrack(m_project->timeline().tracks[size_t(i)].type))
                return i;
        return 0;
    }();
    const int y = trackTop(videoTrack) + 30;
    auto drop = [&](TimeUs at, Qt::KeyboardModifiers mods, bool fromFile = false) {
        const Timeline previous = m_project->timeline();
        QMimeData mime;
        if (fromFile)
            mime.setUrls({QUrl::fromLocalFile(m_project->mediaItem(mediaId)->path)});
        else
            mime.setData(QString::fromLatin1(MediaBin::kMimeType), QByteArray::number(mediaId));
        const QPoint pos(int(timeToX(at)), y);
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(viewport(), &enter);
        QDragMoveEvent move(pos, Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(viewport(), &move);
        QDropEvent ev(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, mods);
        QApplication::sendEvent(viewport(), &ev);
        const Timeline& placed = m_project->timeline();
        const bool hasAudio = m_project->mediaItem(mediaId)->info.hasAudio();
        bool oldTracksKept = true;
        for (const Track& old : previous.tracks) {
            const int index = placed.trackIndex(old.id);
            oldTracksKept &= index >= 0 && placed.tracks[size_t(index)] == old;
        }
        check(placed.tracks.size() == previous.tracks.size() + (hasAudio ? 2 : 1) && oldTracksKept,
              fromFile ? QStringLiteral("file drop creates fresh video/audio tracks and preserves existing tracks")
                       : QStringLiteral("Project Media drop creates fresh video/audio tracks and preserves existing tracks"));
        // The newest clip on the video track.
        const auto& clips = m_project->timeline().tracks[size_t(videoTrack)].clips;
        TimeUs start = -1;
        int best = -1;
        for (const Clip& c : clips)
            if (c.id > best) {
                best = c.id;
                start = c.start;
            }
        return start;
    };
    const auto secs = [](TimeUs t) { return QString::number(double(t) / kUsPerSecond, 'f', 2); };
    const TimeUs first = drop(5 * kUsPerSecond, Qt::NoModifier);
    check(first == 0, QStringLiteral("first clip dropped at 5 s lands at the start: %1 s").arg(secs(first)));
    const Timeline firstPlaced = m_project->timeline();
    const Clip& firstVideo = firstPlaced.tracks[size_t(videoTrack)].clips.front();
    if (m_project->mediaItem(mediaId)->info.hasAudio()) {
        const Track& newAudio = firstPlaced.tracks[size_t(videoTrack + 1)];
        check(newAudio.type == TrackType::Audio && newAudio.clips.size() == 1 && firstVideo.groupId != 0
                  && newAudio.clips.front().groupId == firstVideo.groupId && newAudio.clips.front().start == firstVideo.start
                  && newAudio.clips.front().length == firstVideo.length,
              QStringLiteral("the dropped video's new audio track stays grouped and aligned with its picture"));
        check(firstPlaced.tracks[size_t(videoTrack)].name == QStringLiteral("Clip1-Video")
                  && newAudio.name == QStringLiteral("Clip1-Audio"),
              QStringLiteral("the new video/audio tracks are adjacent and have the same clip number"));
    }
    m_project->undoStack()->undo();
    check(m_project->timeline() == empty, QStringLiteral("undo removes the dropped events and their new tracks together"));
    m_project->undoStack()->redo();
    check(m_project->timeline() == firstPlaced, QStringLiteral("redo restores the dropped events and their new tracks together"));
    const TimeUs end1 = m_project->timeline().duration();
    const TimeUs second = drop(end1 + 2 * kUsPerSecond, Qt::NoModifier);
    check(second == end1, QStringLiteral("dropped 2 s after the last clip joins its end (%1 s): %2 s").arg(secs(end1), secs(second)));
    const TimeUs end2 = m_project->timeline().duration();
    const TimeUs before = drop(end2 - 2 * kUsPerSecond, Qt::NoModifier);
    check(before == end2, QStringLiteral("dropped 2 s before the end joins too (%1 s): %2 s").arg(secs(end2), secs(before)));
    const TimeUs end3 = m_project->timeline().duration();
    const TimeUs far = drop(end3 + 20 * kUsPerSecond, Qt::NoModifier);
    check(std::llabs(far - (end3 + 20 * kUsPerSecond)) < kUsPerSecond / 5,
          QStringLiteral("dropped 20 s after the end stays where it was dropped: %1 s").arg(secs(far)));
    const TimeUs end4 = m_project->timeline().duration();
    const TimeUs free = drop(end4 + kUsPerSecond, Qt::ShiftModifier);
    check(std::llabs(free - (end4 + kUsPerSecond)) < kUsPerSecond / 5, QStringLiteral("Shift drops exactly where dropped: %1 s").arg(secs(free)));
    const TimeUs fileAt = m_project->timeline().duration() + 5 * kUsPerSecond;
    check(drop(fileAt, Qt::ShiftModifier, true) == fileAt, QStringLiteral("Explorer file drops use the new-track placement path"));
    const Timeline beforeBatch = m_project->timeline();
    insertMedia({mediaId, mediaId}, fileAt, videoTrack, true);
    const Timeline& afterBatch = m_project->timeline();
    const int tracksPerFile = m_project->mediaItem(mediaId)->info.hasAudio() ? 2 : 1;
    bool separateBatch = afterBatch.tracks.size() == beforeBatch.tracks.size() + 2 * tracksPerFile
                         && m_selection.size() == size_t(2 * tracksPerFile);
    for (const Track& track : afterBatch.tracks) {
        int selectedOnTrack = 0;
        for (const Clip& c : track.clips)
            selectedOnTrack += m_selection.count(c.id) ? 1 : 0;
        separateBatch &= selectedOnTrack <= 1;
    }
    if (tracksPerFile == 2) {
        for (size_t i : {size_t(0), size_t(2)}) {
            const Track& video = afterBatch.tracks[i];
            const Track& audio = afterBatch.tracks[i + 1];
            separateBatch &= isVisualTrack(video.type) && audio.type == TrackType::Audio
                             && video.name.endsWith(QStringLiteral("-Video"))
                             && audio.name == video.name.chopped(6) + QStringLiteral("-Audio");
        }
    }
    check(separateBatch, QStringLiteral("a batch of dropped videos gives each file its own tracks"));
    // Track imports must make the scrollbar appear without resizing the timeline.
    for (int i = 0; i < 5; ++i)
        QCoreApplication::processEvents();
    QScrollBar* scroll = verticalScrollBar();
    check(scroll->maximum() > 0 && scroll->isVisible(),
          QStringLiteral("adding tracks shows the vertical scrollbar without resizing the timeline"));
    scroll->setValue(scroll->maximum());
    const int lastTrack = int(afterBatch.tracks.size()) - 1;
    const int lastBottom = trackTop(lastTrack) + trackHeight(afterBatch.tracks[size_t(lastTrack)]);
    check(m_scrollY == scroll->maximum() && lastBottom <= viewport()->height() && lastBottom > kTopH,
          QStringLiteral("vertical scrolling reaches the last imported track without resizing"));
    scroll->setValue(0);
    m_project->undoStack()->undo();
    check(m_project->timeline() == beforeBatch, QStringLiteral("a whole batch's new events and tracks undo in one step"));

    // Still-image drops have their own orange visual track, including file drops.
    {
        QTemporaryDir images;
        QImage image(64, 48, QImage::Format_RGB32);
        image.fill(QColor(0xe0, 0x50, 0x20));
        const QString path = images.filePath(QStringLiteral("image.png"));
        image.save(path);
        const int imageId = m_project->addMedia(probeMedia(path));
        const Timeline beforeImage = m_project->timeline();
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        const QPoint pos(int(timeToX(fileAt)), y);
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(viewport(), &enter);
        QDropEvent event(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(viewport(), &event);
        const Timeline placed = m_project->timeline();
        const Track& track = placed.tracks.front();
        const bool isImage = track.type == TrackType::Image && track.name == QStringLiteral("Image 1")
                             && track.clips.size() == 1 && track.clips.front().mediaId == imageId;
        check(isImage && placed.tracks.size() == beforeImage.tracks.size() + 1,
              QStringLiteral("dropping an image file creates a unique Image track without an audio track"));
        verticalScrollBar()->setValue(0);
        const QImage painted = viewport()->grab().toImage();
        const qreal pixelRatio = painted.devicePixelRatio();
        const QColor stripe = painted.pixelColor(int(pixelRatio), int((trackTop(0) + 25) * pixelRatio));
        const QColor expectedStripe = theme::c().imageClip.lighter(130);
        check(stripe.rgb() == expectedStripe.rgb(),
              QStringLiteral("the Image track header is painted orange"));
        if (isImage) {
            setSelection({track.clips.front().id});
            check(addEffectToSelection(QStringLiteral("invert")) == track.clips.front().id,
                  QStringLiteral("visual effects can be applied to an Image track event"));
            m_project->undoStack()->undo();
            copySelection();
            paste();
            check(m_project->timeline().tracks.front().clips.size() == 2,
                  QStringLiteral("Image track events can be copied and pasted"));
            m_project->undoStack()->undo();
        }
        m_project->undoStack()->undo();
        check(m_project->timeline() == beforeImage, QStringLiteral("undo removes an imported image and its Image track together"));
    }

    // Effects: on an event -> Event FX; on an empty part of the lane -> Track FX.
    auto dropEffectAt = [&](QPoint pos) {
        QMimeData mime;
        mime.setData(QString::fromLatin1(EffectsBrowser::kEffectMime), QByteArray("invert"));
        QDragEnterEvent enter(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &enter);
        QDragMoveEvent move(pos, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &move);
        const bool showsTrack = m_fxDropTrack == videoTrack, showsClip = m_fxDropClip != 0;
        QDropEvent ev(QPointF(pos), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(viewport(), &ev);
        return std::pair{showsTrack, showsClip};
    };
    const Clip firstClip = m_project->timeline().tracks[size_t(videoTrack)].clips.front();
    const auto [gapTrack, gapClip] = dropEffectAt(QPoint(int(timeToX(m_project->timeline().duration() + 2 * kUsPerSecond)), y));
    const Track& vt = m_project->timeline().tracks[size_t(videoTrack)];
    check(gapTrack && !gapClip && vt.effects.size() == 1 && vt.clips.front().effects.empty(),
          QStringLiteral("an effect dropped on an empty part of the lane goes to Track FX (highlighted as such)"));
    const auto [onTrack, onClip] = dropEffectAt(QPoint(int(timeToX(firstClip.start + firstClip.length / 2)), y));
    const Track& vt2 = m_project->timeline().tracks[size_t(videoTrack)];
    check(!onTrack && onClip && vt2.effects.size() == 1 && vt2.clips.front().effects.size() == 1,
          QStringLiteral("an effect dropped on an event goes to that event only"));

    // A click on the track header (outside its buttons) selects the track's FX.
    int selected = 0;
    const auto conn = connect(this, &TimelineWidget::trackSelected, this, [&](int id) { selected = id; });
    const QPoint header(40, trackTop(videoTrack) + 12);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(header), mapToGlobal(QPointF(header)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(viewport(), &press);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(header), mapToGlobal(QPointF(header)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(viewport(), &release);
    disconnect(conn);
    check(selected == vt2.id, QStringLiteral("clicking the track header selects the track"));
    return failures;
}

void TimelineWidget::dragLeaveEvent(QDragLeaveEvent*)
{
    m_dropX.reset();
    m_fxDropClip = 0;
    m_fxDropTrack = -1;
    viewport()->update();
}

void TimelineWidget::dropEvent(QDropEvent* e)
{
    m_dropX.reset();
    m_fxDropClip = 0;
    m_fxDropTrack = -1;
    const QPoint p = e->position().toPoint();
    const TimeUs at = std::max<TimeUs>(0, snapped(xToTime(std::max(p.x(), kHeaderWidth)), e->modifiers()));
    const int track = trackAt(p.y());
    const QMimeData* md = e->mimeData();
    if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kGeneratorMime))) {
        const int id = insertGenerator(QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kGeneratorMime))), at, track);
        if (id)
            emit generatorRequested(id);
    } else if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kEffectMime))) {
        dropEffect(p, QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kEffectMime))));
    } else if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kPresetMime))) {
        dropPreset(p, QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kPresetMime))));
    } else if (md->hasFormat(QString::fromLatin1(EffectsBrowser::kTransitionMime))) {
        dropTransition(p, QString::fromUtf8(md->data(QString::fromLatin1(EffectsBrowser::kTransitionMime))));
    } else if (md->hasFormat(QString::fromLatin1(MediaBin::kMimeType))) {
        std::vector<int> ids;
        for (const QByteArray& s : md->data(QString::fromLatin1(MediaBin::kMimeType)).split(','))
            if (!s.isEmpty())
                ids.push_back(s.toInt());
        insertMedia(ids, mediaDropTime(p.x(), track, e->modifiers()), track, true);
    } else if (md->hasUrls()) {
        QStringList paths;
        for (const QUrl& url : md->urls())
            if (url.isLocalFile())
                paths << url.toLocalFile();
        if (!paths.isEmpty())
            emit filesDropped(paths, mediaDropTime(p.x(), track, e->modifiers()), track);
    }
    e->acceptProposedAction();
    viewport()->update();
}

void TimelineWidget::setSpeedOnSelection(double speed)
{
    Timeline tl = m_project->timeline();
    const FrameRate fps = m_project->settings().fps;
    bool changed = false;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips)
            if (m_selection.count(c.id) && !c.isGenerated()) {
                auto media = m_project->mediaItem(c.mediaId);
                ops::setClipSpeed(c, media.get(), speed, true, fps);
                changed = true;
            }
    if (changed)
        m_project->applyEdit(tl, tr("Playback Speed %1%").arg(speed * 100));
}

void TimelineWidget::toggleReverseOnSelection()
{
    Timeline tl = m_project->timeline();
    bool anyForward = false;
    for (int id : m_selection)
        if (const Clip* c = tl.findClip(id))
            anyForward |= !c->reverse;
    for (int id : m_selection)
        if (Clip* c = tl.findClip(id))
            c->reverse = anyForward;
    m_project->applyEdit(tl, anyForward ? tr("Reverse") : tr("Play Forward"));
}

void TimelineWidget::toggleEnvelope(EnvelopeKind kind)
{
    Timeline tl = m_project->timeline();
    if (m_selectedTrack < 0 || m_selectedTrack >= int(tl.tracks.size()))
        return;
    Track& t = tl.tracks[size_t(m_selectedTrack)];
    if (isVisualTrack(t.type)) {
        if (kind == EnvelopeKind::Pan)
            return;
        kind = EnvelopeKind::Composite;
    } else if (kind == EnvelopeKind::Composite) {
        kind = EnvelopeKind::Volume;
    }
    Envelope& env = *t.envelope(kind);
    env.active = !env.active; // points are kept while hidden
    m_project->applyEdit(tl, env.active ? tr("Show Envelope") : tr("Hide Envelope"));
}

double TimelineWidget::envValueToY(EnvelopeKind k, double v, int top, int h) const
{
    const double y0 = top + 6, y1 = top + h - 6;
    double pos;
    switch (k) {
    case EnvelopeKind::Volume: pos = posFromVolume(float(v)); break;
    case EnvelopeKind::Pan: return y0 + (std::clamp(v, -1.0, 1.0) + 1.0) / 2.0 * (y1 - y0); // up = left
    case EnvelopeKind::Velocity: pos = posFromVelocity(v); break;
    default: pos = std::clamp(v, 0.0, 1.0); break;
    }
    return y1 - pos * (y1 - y0);
}

double TimelineWidget::envYToValue(EnvelopeKind k, double y, int top, int h) const
{
    const double y0 = top + 6, y1 = top + h - 6;
    const double f = std::clamp((y - y0) / std::max(1.0, y1 - y0), 0.0, 1.0);
    switch (k) {
    case EnvelopeKind::Volume: return volumeFromPos(1.0 - f);
    case EnvelopeKind::Pan: {
        const double v = f * 2.0 - 1.0;
        return std::abs(v) < 0.03 ? 0.0 : v; // detent at centre
    }
    case EnvelopeKind::Velocity: return velocityFromPos(1.0 - f);
    default: return 1.0 - f;
    }
}

QString TimelineWidget::envValueText(EnvelopeKind k, double v) const
{
    switch (k) {
    case EnvelopeKind::Volume: return tr("Volume %1").arg(volumeText(float(v)));
    case EnvelopeKind::Pan: return tr("Pan %1").arg(panText(float(v)));
    case EnvelopeKind::Velocity: return tr("Velocity %1%").arg(int(std::lround(v * 100)));
    default: return tr("Level %1%").arg(int(std::lround(v * 100)));
    }
}

const Envelope* TimelineWidget::findEnvelope(const Timeline& tl, const EnvHit& h)
{
    if (h.track < 0 || h.track >= int(tl.tracks.size()))
        return nullptr;
    if (h.clipId) {
        const Clip* c = tl.findClip(h.clipId);
        return c ? &c->velocity : nullptr;
    }
    return tl.tracks[size_t(h.track)].envelope(h.kind);
}

Envelope* TimelineWidget::findEnvelope(Timeline& tl, const EnvHit& h)
{
    return const_cast<Envelope*>(findEnvelope(std::as_const(tl), h));
}

TimelineWidget::EnvBand TimelineWidget::envBand(const Timeline& tl, const EnvHit& h) const
{
    EnvBand b;
    if (h.track < 0 || h.track >= int(tl.tracks.size()))
        return b;
    b.top = trackTop(h.track);
    b.h = trackHeight(tl.tracks[size_t(h.track)]);
    b.maxT = std::numeric_limits<TimeUs>::max() / 4;
    if (h.clipId)
        if (const Clip* c = tl.findClip(h.clipId)) {
            // Velocity envelopes live in the event body, in event time.
            b.origin = c->start;
            b.maxT = c->length;
            b.top += 1 + kTitleH;
            b.h -= 2 + kTitleH;
        }
    return b;
}

TimelineWidget::EnvHit TimelineWidget::envelopeHit(QPoint p) const
{
    EnvHit none;
    const int ti = trackAt(p.y());
    if (ti < 0 || p.x() < kHeaderWidth)
        return none;
    const Timeline& tl = m_project->timeline();
    const Track& track = tl.tracks[size_t(ti)];
    std::vector<EnvHit> candidates;
    for (EnvelopeKind k : {EnvelopeKind::Volume, EnvelopeKind::Pan, EnvelopeKind::Composite})
        candidates.push_back(EnvHit{ti, k, -1, 0});
    // Velocity envelope of the (topmost) event under the mouse.
    for (auto it = track.clips.rbegin(); it != track.clips.rend(); ++it)
        if (it->velocity.active && p.x() >= timeToX(it->start) && p.x() <= timeToX(it->end())) {
            candidates.push_back(EnvHit{ti, EnvelopeKind::Velocity, -1, it->id});
            break;
        }
    // Points first (they sit on top), then lines.
    for (const EnvHit& c : candidates) {
        const Envelope* env = findEnvelope(tl, c);
        if (!env || !env->active)
            continue;
        const EnvBand band = envBand(tl, c);
        for (size_t i = 0; i < env->points.size(); ++i) {
            if (env->points[i].time < 0 || env->points[i].time > band.maxT)
                continue;
            const QPointF pt(timeToX(band.origin + env->points[i].time), envValueToY(c.kind, env->points[i].value, band.top, band.h));
            if (std::abs(pt.x() - p.x()) <= 5 && std::abs(pt.y() - p.y()) <= 5)
                return EnvHit{ti, c.kind, int(i), c.clipId};
        }
    }
    for (const EnvHit& c : candidates) {
        const Envelope* env = findEnvelope(tl, c);
        if (!env || !env->active)
            continue;
        const EnvBand band = envBand(tl, c);
        // Leave event edges to trimming.
        if (c.clipId && (p.x() - timeToX(band.origin) <= kEdgeGrab || timeToX(band.origin + band.maxT) - p.x() <= kEdgeGrab))
            continue;
        if (std::abs(envValueToY(c.kind, env->at(xToTime(p.x()) - band.origin), band.top, band.h) - p.y()) <= 4)
            return c;
    }
    return none;
}

void TimelineWidget::paintEnvelopes(QPainter& p, const Track& track, int top, int h)
{
    const int W = viewport()->width();
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.75);
    p.setFont(f);
    auto drawLine = [&](const Envelope& env, EnvelopeKind k, const QColor& color, TimeUs origin, int bandTop, int bandH,
                        double x0, double x1, TimeUs maxT, const QString& name) {
        // Sample the curve every 2 px (smooth/hold segments are drawn as they play).
        QPolygonF line;
        for (double x = x0; x <= x1 + 2; x += 2)
            line << QPointF(x, envValueToY(k, env.at(xToTime(x) - origin), bandTop, bandH));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(0, 0, 0, 140), 3));
        p.drawPolyline(line);
        p.setPen(QPen(color, 1.5));
        p.drawPolyline(line);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const Keyframe& pt : env.points) {
            const double x = timeToX(origin + pt.time);
            if (x < x0 - 4 || x > x1 + 4 || pt.time < 0 || pt.time > maxT)
                continue;
            const QRectF r(x - 3.5, envValueToY(k, pt.value, bandTop, bandH) - 3.5, 7, 7);
            p.fillRect(r, Qt::white);
            p.setPen(color);
            p.drawRect(r);
        }
        p.setPen(color);
        p.drawText(QPointF(x0 + 4, envValueToY(k, env.at(xToTime(x0 + 4) - origin), bandTop, bandH) - 4), name);
    };
    for (EnvelopeKind k : {EnvelopeKind::Composite, EnvelopeKind::Pan, EnvelopeKind::Volume}) {
        const Envelope& env = *track.envelope(k);
        if (!env.active)
            continue;
        const QColor color = k == EnvelopeKind::Volume ? QColor(0x5a, 0xa8, 0xff)
                             : k == EnvelopeKind::Pan  ? QColor(0xff, 0x6e, 0x6e)
                                                       : QColor(0xc8, 0x8c, 0xff);
        const QString name = k == EnvelopeKind::Volume ? tr("Vol") : (k == EnvelopeKind::Pan ? tr("Pan") : tr("Level"));
        drawLine(env, k, color, 0, top, h, kHeaderWidth, W, std::numeric_limits<TimeUs>::max() / 4, name);
    }
    // Speed ramps: each event's velocity envelope inside its body.
    const QColor velocityColor(0xff, 0xb8, 0x4d);
    for (const Clip& c : track.clips) {
        if (!c.velocity.active)
            continue;
        const double x0 = std::max(timeToX(c.start), double(kHeaderWidth)), x1 = std::min(timeToX(c.end()), double(W));
        if (x1 - x0 < 2)
            continue;
        const int bandTop = top + 1 + kTitleH, bandH = h - 2 - kTitleH;
        p.save();
        p.setClipRect(QRectF(x0, bandTop, x1 - x0, bandH), Qt::IntersectClip);
        const double y100 = envValueToY(EnvelopeKind::Velocity, 1.0, bandTop, bandH);
        p.setPen(QPen(QColor(0xff, 0xb8, 0x4d, 110), 1, Qt::DotLine));
        p.drawLine(QPointF(x0, y100), QPointF(x1, y100)); // 100% reference
        drawLine(c.velocity, EnvelopeKind::Velocity, velocityColor, c.start, bandTop, bandH, x0, x1, c.length, tr("Velocity"));
        p.restore();
    }
}

void TimelineWidget::dragEnvelope(QPoint p, Qt::KeyboardModifiers mods)
{
    Timeline tl = m_before;
    Envelope* envp = findEnvelope(tl, m_envDrag);
    const Envelope* origp = findEnvelope(m_before, m_envDrag);
    if (!envp || !origp)
        return;
    Envelope& env = *envp;
    const Envelope& orig = *origp;
    const EnvBand band = envBand(m_before, m_envDrag);
    const EnvelopeKind k = m_envDrag.kind;
    const TimeUs pressT = m_envPressTime - band.origin;
    double shown = 0;
    if (m_drag == Drag::EnvelopePoint && m_envDrag.point < int(env.points.size())) {
        const size_t i = size_t(m_envDrag.point);
        TimeUs t = std::clamp(snapped(xToTime(p.x()), mods) - band.origin, TimeUs(0), band.maxT);
        // Points keep their order.
        if (i > 0)
            t = std::max(t, env.points[i - 1].time + 1);
        if (i + 1 < env.points.size())
            t = std::min(t, env.points[i + 1].time - 1);
        env.points[i].time = t;
        env.points[i].value = envYToValue(k, p.y(), band.top, band.h);
        shown = env.points[i].value;
    } else {
        // Move the segment under the press (or the flat line) up/down.
        const double dy = p.y() - m_pressPos.y();
        auto moved = [&](double v) { return envYToValue(k, envValueToY(k, v, band.top, band.h) + dy, band.top, band.h); };
        if (orig.points.empty()) {
            env.value = moved(orig.value);
            shown = env.value;
        } else {
            size_t next = 0;
            while (next < orig.points.size() && orig.points[next].time <= pressT)
                ++next;
            if (next > 0)
                env.points[next - 1].value = moved(orig.points[next - 1].value);
            if (next < orig.points.size())
                env.points[next].value = moved(orig.points[next].value);
            shown = env.at(pressT);
        }
    }
    if (m_envDrag.clipId)
        ops::syncGroupRamps(tl, m_envDrag.clipId);
    m_project->setTimelineLive(tl);
    QToolTip::showText(viewport()->mapToGlobal(p + QPoint(14, 10)), envValueText(k, shown), viewport());
}

void TimelineWidget::envelopeMenu(QMenu& menu, const EnvHit& hit)
{
    const EnvelopeKind k = hit.kind;
    auto edit = [this, hit](const QString& label, std::function<void(Envelope&)> fn) {
        Timeline tl = m_project->timeline();
        if (Envelope* e = findEnvelope(tl, hit)) {
            fn(*e);
            if (hit.clipId)
                ops::syncGroupRamps(tl, hit.clipId);
            m_project->applyEdit(tl, label);
        }
    };
    const Envelope* current = findEnvelope(m_project->timeline(), hit);
    if (!current)
        return;
    if (hit.point >= 0 && hit.point < int(current->points.size())) {
        const int i = hit.point;
        menu.addAction(tr("Delete Point"), this, [edit, i] {
            edit(QObject::tr("Delete Envelope Point"), [i](Envelope& e) {
                if (i < int(e.points.size())) {
                    // Keep the level the line had there when it was the only point.
                    if (e.points.size() == 1)
                        e.value = e.points.front().value;
                    e.points.erase(e.points.begin() + i);
                }
            });
        });
        QMenu* curve = menu.addMenu(tr("Curve to Next Point"));
        const Keyframe::Interp interpNow = current->points[size_t(i)].interp;
        for (int n = 0; n < kKeyInterps; ++n) {
            const auto interp = Keyframe::Interp(n);
            if (interp == Keyframe::Interp::Bezier) // (its handles are drawn in the Keyframe Editor only)
                continue;
            QAction* a = curve->addAction(keyInterpName(interp), this, [edit, i, interp] {
                edit(QObject::tr("Envelope Curve"), [i, interp](Envelope& e) {
                    if (i < int(e.points.size()))
                        e.points[size_t(i)].interp = interp;
                });
            });
            a->setCheckable(true);
            a->setChecked(interp == interpNow);
        }
        menu.addSeparator();
    }
    if (hit.clipId) {
        menu.addAction(tr("Reset to 100%"), this, [edit] {
            edit(QObject::tr("Reset Speed Ramp"), [](Envelope& e) {
                e.points.clear();
                e.value = 1.0;
            });
        });
        QMenu* presets = menu.addMenu(tr("Speed Ramp Presets"));
        for (auto preset : {ops::RampPreset::SpeedUp, ops::RampPreset::SlowDown, ops::RampPreset::SlowMiddle,
                            ops::RampPreset::FastMiddle, ops::RampPreset::FreezeMiddle})
            presets->addAction(ops::rampPresetName(preset), this, [this, preset] { applyRampPresetToSelection(preset); });
        menu.addAction(tr("Remove Speed Ramp"), this, [edit] {
            edit(QObject::tr("Remove Speed Ramp"), [](Envelope& e) { e = Envelope{false, 1.0, {}}; });
        });
        return;
    }
    menu.addAction(tr("Reset Envelope (remove all points)"), this, [edit, k] {
        edit(QObject::tr("Reset Envelope"), [k](Envelope& e) {
            e.points.clear();
            e.value = k == EnvelopeKind::Pan ? 0.0 : 1.0;
        });
    });
    menu.addAction(tr("Hide Envelope"), this, [edit] { edit(QObject::tr("Hide Envelope"), [](Envelope& e) { e.active = false; }); });
}

void TimelineWidget::toggleVelocityEnvelope()
{
    Timeline tl = m_project->timeline();
    bool any = false, anyOn = false;
    for (int id : m_selection)
        if (const Clip* c = tl.findClip(id); c && !c->isGenerated()) {
            any = true;
            anyOn |= c->hasRamp();
        }
    if (!any)
        return;
    for (int id : m_selection)
        if (Clip* c = tl.findClip(id); c && !c->isGenerated())
            c->velocity = Envelope{!anyOn, 1.0, {}}; // a flat 100% line changes nothing
    m_project->applyEdit(tl, anyOn ? tr("Remove Speed Ramp") : tr("Velocity Envelope"));
}

void TimelineWidget::applyRampPresetToSelection(ops::RampPreset preset)
{
    Timeline tl = m_project->timeline();
    const FrameRate fps = m_project->settings().fps;
    std::vector<int> done;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips)
            if (m_selection.count(c.id) && !c.isGenerated()) {
                auto media = m_project->mediaItem(c.mediaId);
                ops::applyRampPreset(c, media.get(), preset, fps);
                done.push_back(c.id);
            }
    if (done.empty())
        return;
    // Video events lead: their grouped audio gets exactly the same ramp and length.
    for (int id : done) {
        int ti = -1;
        if (tl.findClip(id, &ti) && isVisualTrack(tl.tracks[size_t(ti)].type))
            ops::syncGroupRamps(tl, id);
    }
    tl.sortClips();
    m_project->applyEdit(tl, tr("Speed Ramp: %1").arg(ops::rampPresetName(preset)));
}

int TimelineWidget::insertGenerator(const QString& presetId, TimeUs at, int trackIndex)
{
    const GeneratorPreset* preset = findGeneratorPreset(presetId);
    if (!preset)
        return 0;
    Timeline tl = m_project->timeline();
    const int id = ops::insertGenerated(tl, *preset, at, trackIndex, m_project->settings().fps);
    if (const Clip* c = tl.findClip(id))
        rippleAfterInsert(tl, c->start, {id});
    if (!m_autoCrossfade)
        ops::overwriteOverlaps(tl, {id});
    m_project->applyEdit(tl, tr("Insert %1").arg(preset->name));
    setSelection({id});
    if (const Clip* c = m_project->timeline().findClip(id)) {
        int ti = -1;
        m_project->timeline().findClip(id, &ti);
        m_selectedTrack = ti;
        ensureTimeVisible(c->start, false);
    }
    return id;
}

int TimelineWidget::insertGeneratorAtCursor(const QString& presetId)
{
    const auto& tracks = m_project->timeline().tracks;
    return insertGenerator(presetId, m_cursor, m_selectedTrack < int(tracks.size()) ? m_selectedTrack : -1);
}

void TimelineWidget::paintGenerated(QPainter& p, const Clip& c, const QRectF& body)
{
    if (body.height() < 8)
        return;
    const ProjectSettings& s = m_project->settings();
    const double thumbH = body.height();
    const double thumbW = std::max(8.0, thumbH * s.width / std::max(1, s.height));
    const QSize size{int(thumbW), int(thumbH)};
    // Rendered once per settings change; shows the picture mid-way through the event.
    GeneratedThumb& thumb = m_generatedThumbs[c.id];
    if (thumb.image.isNull() || thumb.size != size || !(thumb.spec == c.generator) || thumb.length != c.length) {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(0x2a, 0x22, 0x3a));
        const QImage gen = renderGenerator(c.generator, size, thumbW / std::max(1, s.width), c.length / 2, c.length, s.fps);
        QPainter(&img).drawImage(0, 0, gen);
        thumb = {c.generator, c.length, size, img};
    }
    const int W = viewport()->width();
    const double vis0 = std::max(body.left(), double(kHeaderWidth)), vis1 = std::min(body.right(), double(W));
    for (int i = std::max(0, int((vis0 - body.left()) / thumbW)); body.left() + i * thumbW < vis1; ++i)
        p.drawImage(QPointF(body.left() + i * thumbW, body.top()), thumb.image);
}

int TimelineWidget::firstSelectedVideoClip() const
{
    for (const auto& t : m_project->timeline().tracks)
        if (isVisualTrack(t.type))
            for (const auto& c : t.clips)
                if (m_selection.count(c.id))
                    return c.id;
    return 0;
}

int TimelineWidget::firstSelectedClip() const
{
    for (const auto& t : m_project->timeline().tracks)
        for (const auto& c : t.clips)
            if (m_selection.count(c.id))
                return c.id;
    return 0;
}

int TimelineWidget::addEffectToSelection(const QString& effectType)
{
    const auto meta = effectMeta(effectType);
    if (!meta)
        return 0;
    const TrackType wanted = meta->audio ? TrackType::Audio : TrackType::Video;
    Timeline tl = m_project->timeline();
    int first = 0;
    for (auto& t : tl.tracks)
        if (sameTrackEffectKind(t.type, wanted))
            for (auto& c : t.clips)
                if (m_selection.count(c.id)) {
                    c.effects.push_back(makeEffectOfType(effectType, tl.allocId()));
                    if (!first)
                        first = c.id;
                }
    if (first)
        m_project->applyEdit(tl, tr("Add %1").arg(meta->name));
    return first;
}

void TimelineWidget::normalizeSelection()
{
    Timeline tl = m_project->timeline();
    bool changed = false;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    for (auto& t : tl.tracks) {
        if (t.type != TrackType::Audio)
            continue;
        for (auto& c : t.clips) {
            if (!m_selection.count(c.id))
                continue;
            auto media = m_project->mediaItem(c.mediaId);
            const float peak = media ? measureClipPeak(*media, c) : 0.0f;
            if (peak <= 1e-6f)
                continue;
            const double gainDb = 20.0 * std::log10(0.98855 / peak); // peak -> -0.1 dBFS
            // Reuse a Gain effect at the front of the chain, else add one there.
            if (c.effects.empty() || c.effects.front().type != QLatin1String("a_gain"))
                c.effects.insert(c.effects.begin(), makeEffectOfType(QStringLiteral("a_gain"), tl.allocId()));
            ParamValue& g = c.effects.front().params[QStringLiteral("gain")];
            g.keys.clear();
            g.value = std::clamp(gainDb, -60.0, 24.0);
            changed = true;
        }
    }
    QApplication::restoreOverrideCursor();
    if (changed)
        m_project->applyEdit(tl, tr("Normalize"));
}

int TimelineWidget::setTransitionOnSelection(const QString& transitionType)
{
    Timeline tl = m_project->timeline();
    int first = 0;
    std::vector<int> ids;
    for (const auto& t : tl.tracks)
        if (isVisualTrack(t.type))
            for (const auto& c : t.clips)
                if (m_selection.count(c.id))
                    ids.push_back(c.id);
    for (int id : ids) {
        const auto [holder, in] = setTransition(tl, id, true, transitionType);
        if (!first)
            first = holder;
    }
    if (first)
        m_project->applyEdit(tl, tr("Set Transition"));
    return first;
}

std::pair<int, bool> TimelineWidget::setTransition(Timeline& tl, int clipId, bool atStart, const QString& type)
{
    int ti = -1;
    Clip* c = tl.findClip(clipId, &ti);
    if (!c || !isVisualTrack(tl.tracks[ti].type))
        return {0, true};
    Track& track = tl.tracks[ti];
    size_t idx = 0;
    while (idx < track.clips.size() && track.clips[idx].id != clipId)
        ++idx;
    const TransitionDef* def = findTransition(type);
    const TransitionSpec spec = (!def || def->id == QLatin1String("dissolve")) ? TransitionSpec{} : makeTransition(*def);
    const TimeUs defaultFade = std::min(kUsPerSecond, quantize(c->length / 2));
    const bool overlapPrev = idx > 0 && track.clips[idx - 1].end() > c->start;
    const bool overlapNext = idx + 1 < track.clips.size() && track.clips[idx + 1].start < c->end();
    if (atStart) {
        c->transitionIn = spec;
        if (!overlapPrev && c->fadeIn == 0)
            c->fadeIn = std::clamp<TimeUs>(defaultFade, 0, std::max<TimeUs>(0, c->length - c->fadeOut));
        const int id = c->id;
        ops::syncGroupFades(tl, id); // the file's audio fades with it
        return {id, true};
    }
    if (overlapNext) {
        // The overlap at this event's end belongs to the next event's start.
        Clip& next = track.clips[idx + 1];
        next.transitionIn = spec;
        return {next.id, true};
    }
    c->transitionOut = spec;
    if (c->fadeOut == 0)
        c->fadeOut = std::clamp<TimeUs>(defaultFade, 0, std::max<TimeUs>(0, c->length - c->fadeIn));
    const int id = c->id;
    ops::syncGroupFades(tl, id);
    return {id, false};
}

void TimelineWidget::dropEffect(QPoint p, const QString& type)
{
    const auto meta = effectMeta(type);
    const int ti = trackAt(p.y());
    if (!meta || ti < 0)
        return;
    const TrackType wanted = meta->audio ? TrackType::Audio : TrackType::Video;
    if (!sameTrackEffectKind(m_project->timeline().tracks[ti].type, wanted))
        return;
    Timeline tl = m_project->timeline();
    // On the track header or an empty part of the lane: Track FX (the whole track).
    if (p.x() < kHeaderWidth || !hitTest(p).clipId) {
        tl.tracks[ti].effects.push_back(makeEffectOfType(type, tl.allocId()));
        m_project->applyEdit(tl, tr("Add Track %1").arg(meta->name));
        emit trackEffectsRequested(tl.tracks[ti].id);
        return;
    }
    const Hit hit = hitTest(p);
    if (!hit.clipId)
        return;
    // Dropping on a selected event applies to the whole selection.
    std::set<int> targets = m_selection.count(hit.clipId) ? m_selection : std::set<int>{hit.clipId};
    for (auto& t : tl.tracks)
        if (sameTrackEffectKind(t.type, wanted))
            for (auto& c : t.clips)
                if (targets.count(c.id))
                    c.effects.push_back(makeEffectOfType(type, tl.allocId()));
    m_project->applyEdit(tl, tr("Add %1").arg(meta->name));
    if (!m_selection.count(hit.clipId))
        setSelection(ops::expandToGroups(m_project->timeline(), {hit.clipId}));
    emit clipEffectsRequested(hit.clipId);
}

void TimelineWidget::dropPreset(QPoint p, const QString& presetId)
{
    const auto preset = PresetLibrary::instance().find(presetId);
    const int ti = trackAt(p.y());
    if (!preset || ti < 0)
        return;
    const TrackType wanted = preset->audio ? TrackType::Audio : TrackType::Video;
    if (!sameTrackEffectKind(m_project->timeline().tracks[size_t(ti)].type, wanted))
        return;
    Timeline tl = m_project->timeline();
    auto append = [&](EffectChain& chain) {
        const EffectChain add = instantiatePreset(*preset, [&] { return tl.allocId(); });
        chain.insert(chain.end(), add.begin(), add.end());
    };
    const Hit hit = hitTest(p);
    if (p.x() < kHeaderWidth || !hit.clipId) { // header or empty lane: Track FX
        append(tl.tracks[size_t(ti)].effects);
        m_project->applyEdit(tl, tr("Add Track Preset %1").arg(preset->name));
        emit trackEffectsRequested(tl.tracks[size_t(ti)].id);
        return;
    }
    const std::set<int> targets = m_selection.count(hit.clipId) ? m_selection : std::set<int>{hit.clipId};
    for (auto& t : tl.tracks)
        if (sameTrackEffectKind(t.type, wanted))
            for (auto& c : t.clips)
                if (targets.count(c.id))
                    append(c.effects);
    m_project->applyEdit(tl, tr("Add Preset %1").arg(preset->name));
    if (!m_selection.count(hit.clipId))
        setSelection(ops::expandToGroups(m_project->timeline(), {hit.clipId}));
    emit clipEffectsRequested(hit.clipId);
}

int TimelineWidget::addPresetToSelection(const QString& presetId)
{
    const auto preset = PresetLibrary::instance().find(presetId);
    if (!preset)
        return 0;
    const TrackType wanted = preset->audio ? TrackType::Audio : TrackType::Video;
    Timeline tl = m_project->timeline();
    int first = 0;
    for (auto& t : tl.tracks)
        if (sameTrackEffectKind(t.type, wanted))
            for (auto& c : t.clips)
                if (m_selection.count(c.id)) {
                    const EffectChain add = instantiatePreset(*preset, [&] { return tl.allocId(); });
                    c.effects.insert(c.effects.end(), add.begin(), add.end());
                    if (!first)
                        first = c.id;
                }
    if (first)
        m_project->applyEdit(tl, tr("Add Preset %1").arg(preset->name));
    return first;
}

void TimelineWidget::dropTransition(QPoint p, const QString& type)
{
    const Hit hit = hitTest(p);
    if (!hit.clipId)
        return;
    const Clip* c = m_project->timeline().findClip(hit.clipId);
    if (!c)
        return;
    const bool atStart = xToTime(p.x()) < c->start + c->length / 2;
    Timeline tl = m_project->timeline();
    const auto [holder, in] = setTransition(tl, hit.clipId, atStart, type);
    if (!holder)
        return;
    m_project->applyEdit(tl, tr("Set Transition"));
    emit transitionRequested(holder, in);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

void TimelineWidget::paintEvent(QPaintEvent*)
{
    QPainter p(viewport());
    const int W = viewport()->width();
    const int H = viewport()->height();
    p.fillRect(viewport()->rect(), theme::c().timelineBg);
    const auto& tracks = m_project->timeline().tracks;

    p.save();
    p.setClipRect(QRect(kHeaderWidth, kTopH, W - kHeaderWidth, H - kTopH));
    int y = kTopH - m_scrollY;
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const int h = trackHeight(tracks[ti]);
        if (y + h >= kTopH && y <= H) {
            const QRect band(kHeaderWidth, y, W - kHeaderWidth, h);
            p.fillRect(band, ti == m_selectedTrack ? theme::c().trackSelected : (ti % 2 ? theme::c().trackBgAlt : theme::c().trackBg));
            paintGrid(p, band);
            p.setPen(theme::c().deep);
            p.drawLine(band.left(), band.bottom(), band.right(), band.bottom());
            paintTrackClips(p, tracks[ti], y, h);
            paintEnvelopes(p, tracks[ti], y, h);
        }
        y += h;
    }

    if (hasRegion()) {
        const double x0 = timeToX(m_regionStart), x1 = timeToX(m_regionEnd);
        QColor c = theme::c().region;
        c.setAlpha(40);
        p.fillRect(QRectF(x0, kTopH, x1 - x0, H - kTopH), c);
    }
    for (const Marker& m : m_project->timeline().markers) {
        if (m.isRegion())
            continue;
        const double x = timeToX(m.time);
        if (x < kHeaderWidth || x > W)
            continue;
        QColor c = markerColor(m.color);
        c.setAlpha(120);
        p.setPen(QPen(c, 1, Qt::DotLine));
        p.drawLine(QPointF(x, kTopH), QPointF(x, H));
    }
    if (m_dropX) {
        p.setPen(QPen(theme::c().accent, 2));
        p.drawLine(*m_dropX, kTopH, *m_dropX, H);
    }
    if (m_drag == Drag::RubberBand && m_dragMoved) {
        const QRectF band = QRectF(QPointF(timeToX(m_bandTime), m_bandY - m_scrollY), QPointF(m_bandNow)).normalized();
        QColor fill = theme::c().accent;
        fill.setAlpha(40);
        p.fillRect(band, fill);
        p.setPen(QPen(theme::c().accent, 1, Qt::DashLine));
        p.drawRect(band);
    }
    // Where a dragged effect would go.
    if (m_fxDropTrack >= 0 && m_fxDropTrack < int(tracks.size())) {
        const QRectF band(kHeaderWidth, trackTop(m_fxDropTrack), W - kHeaderWidth, trackHeight(tracks[size_t(m_fxDropTrack)]));
        QColor wash = theme::c().good;
        wash.setAlpha(45);
        p.fillRect(band, wash);
        p.setPen(QPen(theme::c().good, 2));
        p.drawRect(band.adjusted(1, 1, -1, -1));
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(theme::c().bright);
        p.drawText(band.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Track FX: applies to everything on this track"));
    } else if (m_fxDropClip) {
        int ti = -1;
        if (const Clip* c = m_project->timeline().findClip(m_fxDropClip, &ti)) {
            const QRectF r(timeToX(c->start), trackTop(ti) + 1, timeToX(c->end()) - timeToX(c->start), trackHeight(tracks[size_t(ti)]) - 2);
            p.setPen(QPen(theme::c().good, 3));
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
    }
    const double cx = timeToX(m_cursor);
    p.setPen(QPen(theme::c().cursor, 1, Qt::DashLine));
    p.drawLine(QPointF(cx, kTopH), QPointF(cx, H));
    if (m_playhead) {
        const double px = timeToX(*m_playhead);
        p.setPen(QPen(theme::c().playhead, 1));
        p.drawLine(QPointF(px, kTopH), QPointF(px, H));
    }
    // An empty timeline says how to start.
    if (std::all_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.clips.empty(); })) {
        p.setPen(theme::c().faint);
        p.drawText(QRect(kHeaderWidth, kTopH, W - kHeaderWidth, H - kTopH).adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Drag video, music or pictures here, or press Ctrl+I to import them.\n"
                      "Ctrl+T adds a title; the Generators tab has backgrounds.\n"
                      "New here? Help > Tutorials, or Shift+F1 for help on any part of the window."));
    }
    p.restore();

    paintRuler(p);
    paintHeaders(p);
    // Where a dragged track header would go.
    if (m_drag == Drag::TrackReorder && m_reorderGap >= 0) {
        const int y = std::max(kTopH, trackTop(m_reorderGap));
        p.fillRect(QRect(0, y - 2, W, 4), theme::c().accent);
    }

    // Timecode display in the corner.
    const QRect corner(0, 0, kHeaderWidth, kTopH);
    p.fillRect(corner, theme::c().corner);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.6);
    f.setBold(true);
    p.setFont(f);
    p.setPen(theme::c().timecode);
    p.drawText(corner, Qt::AlignCenter, formatTimecode(m_playhead.value_or(m_cursor), m_project->settings().fps));
    p.setPen(theme::c().deep);
    p.drawLine(kHeaderWidth - 1, 0, kHeaderWidth - 1, H);
}

void TimelineWidget::paintTrackClips(QPainter& p, const Track& track, int top, int height)
{
    const int W = viewport()->width();
    const TimeUs t0 = xToTime(kHeaderWidth);
    const TimeUs t1 = xToTime(W);
    const double pxPerUs = m_pxPerSec / kUsPerSecond;
    const QColor base = trackColor(track.type);

    for (size_t ci = 0; ci < track.clips.size(); ++ci) {
        const Clip& c = track.clips[ci];
        if (c.end() < t0 || c.start > t1)
            continue;
        // Clamp to the visible area so huge zoom levels don't overflow.
        const double rx0 = timeToX(c.start), rx1 = timeToX(c.end());
        const double vx0 = std::max(rx0, double(kHeaderWidth) - 4), vx1 = std::min(rx1, double(W) + 4);
        const QRectF r(rx0, top + 1, rx1 - rx0, height - 2);
        const QRectF vr(vx0, r.top(), vx1 - vx0, r.height());
        const QRectF body(vx0, r.top() + kTitleH, vx1 - vx0, r.height() - kTitleH);
        const bool selected = m_selection.count(c.id) > 0;
        auto media = m_project->mediaItem(c.mediaId);
        const bool offline = !c.isGenerated() && (!media || !media->info.ok());

        p.fillRect(vr, base.darker(230));
        p.save();
        p.setClipRect(body.intersected(QRectF(kHeaderWidth, top, W - kHeaderWidth, height)), Qt::IntersectClip);
        if (offline) {
            p.fillRect(body, QColor(0x60, 0x20, 0x20));
            p.setPen(theme::c().clipText);
            p.drawText(body.adjusted(6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Media offline"));
        } else if (c.isGenerated()) {
            paintGenerated(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        } else if (isVisualTrack(track.type)) {
            paintThumbnails(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        } else {
            paintWaveform(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        }

        // Fades (effective, so automatic crossfades show too).
        const ClipEnvelope env = effectiveEnvelope(track, ci);
        QColor shade(0, 0, 0, 110);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1));
        // Fades drawn in their curve's shape (the level line), shaded above it.
        auto fadeLine = [&](double xEdge, double xInner, int curve) {
            QPolygonF line;
            constexpr int kSteps = 24;
            for (int k = 0; k <= kSteps; ++k) {
                const double f = double(k) / kSteps;
                line << QPointF(xEdge + (xInner - xEdge) * f, body.bottom() - body.height() * fadeShape(curve, f));
            }
            QPainterPath path;
            path.moveTo(xEdge, body.top());
            for (const QPointF& pt : line)
                path.lineTo(pt);
            path.closeSubpath();
            p.fillPath(path, shade);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.drawPolyline(line);
            p.setRenderHint(QPainter::Antialiasing, false);
        };
        if (env.fadeIn > 0)
            fadeLine(rx0, rx0 + env.fadeIn * pxPerUs, c.fadeInCurve);
        if (env.fadeOut > 0)
            fadeLine(rx1, rx1 - env.fadeOut * pxPerUs, c.fadeOutCurve);
        // Looping (past the end of its file): a notch where the file starts again.
        if (c.loops() && !c.reverse && !c.hasRamp() && c.speed > 0) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xff, 0xff, 0xff, 230));
            const double every = double(c.loopLength) / c.speed;
            for (double t = double(c.start) + double(c.loopLength - c.sourceIn % c.loopLength) / c.speed; t < double(c.end()); t += every) {
                const double x = timeToX(TimeUs(t));
                p.drawPolygon(QPolygonF{QPointF(x - 4, body.top()), QPointF(x + 4, body.top()), QPointF(x, body.top() + 6)});
                p.fillRect(QRectF(x - 0.5, body.top(), 1, body.height()), QColor(0xff, 0xff, 0xff, 70));
            }
            p.setBrush(Qt::NoBrush);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        // The transition's name inside its fade / overlap area, with a × to remove
        // it (audio: its fade, which follows the video's transition when grouped).
        {
            QFont tf = font();
            tf.setPointSizeF(tf.pointSizeF() * 0.8);
            p.setFont(tf);
            for (bool in : {true, false}) {
                const QRectF lr = transitionLabelRect(track, ci, top, height, in);
                if (lr.isEmpty())
                    continue;
                const EndTransition e = endTransition(track, ci, in);
                p.fillRect(lr, QColor(0, 0, 0, 160));
                QRectF textRect = lr;
                if (lr.width() >= kTransitionCloseMinW) {
                    const QRectF x = transitionCloseRect(lr, in);
                    p.fillRect(x.adjusted(1, 1, -1, -1), QColor(0x8a, 0x34, 0x34));
                    p.setPen(Qt::white);
                    p.drawText(x, Qt::AlignCenter, QStringLiteral("\u00D7"));
                    textRect = in ? lr.adjusted(0, 0, -15, 0) : lr.adjusted(15, 0, 0, 0);
                }
                p.setPen(e.spec && !e.spec->isPlainFade() ? QColor(0xff, 0xd0, 0x80) : QColor(0xd8, 0xd8, 0xdc));
                p.drawText(textRect, Qt::AlignCenter,
                           p.fontMetrics().elidedText(endTransitionName(e, in), Qt::ElideRight, int(std::max(0.0, textRect.width()))));
            }
        }
        // Event level line.
        if (c.gain < 0.999f) {
            const double gy = body.top() + (1.0 - c.gain) * body.height();
            p.fillRect(QRectF(body.left(), body.top(), body.width(), gy - body.top()), QColor(0, 0, 0, 90));
            p.setPen(QPen(QColor(0x60, 0xb0, 0xff), 1));
            p.drawLine(QPointF(body.left(), gy), QPointF(body.right(), gy));
        }
        if (c.muted)
            p.fillRect(body, QBrush(QColor(0, 0, 0, 140), Qt::BDiagPattern));
        p.restore();

        // Title strip.
        const QRectF title(vx0, r.top(), vx1 - vx0, kTitleH);
        p.fillRect(title, selected ? theme::c().clipSelected.darker(140) : base);
        p.setPen(theme::c().clipText);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.85);
        p.setFont(f);
        double textX = std::max(rx0, double(kHeaderWidth)) + 4;
        if (c.groupId && vx1 - textX > 40) {
            // Grouped (e.g. a video file's picture and sound): a chain link.
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0xe8, 0xe8, 0xec), 1.4));
            p.setBrush(Qt::NoBrush);
            const double cy = title.top() + kTitleH / 2.0;
            p.drawRoundedRect(QRectF(textX, cy - 2.5, 7, 5), 2.5, 2.5);
            p.drawRoundedRect(QRectF(textX + 4.5, cy - 2.5, 7, 5), 2.5, 2.5);
            p.restore();
            textX += 15;
        }
        const QRectF textRect(textX, title.top(), std::max(0.0, vx1 - textX - 4), kTitleH);
        QString label = c.isGenerated() ? generatorLabel(c.generator) : c.name;
        if (c.muted)
            label = tr("[muted] ") + label;
        if (c.speed != 1.0 || c.reverse || c.hasRamp())
            label += QStringLiteral("  [%1%2%%3]")
                         .arg(c.reverse ? QStringLiteral("-") : QString())
                         .arg(c.speed * 100, 0, 'f', 0)
                         .arg(c.hasRamp() ? tr(" ramp") : QString());
        const QRectF badge = fxBadgeRect(c, top);
        const QRectF nameRect = badge.isEmpty() ? textRect : textRect.adjusted(0, 0, -50, 0);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(label, Qt::ElideRight, int(std::max(0.0, nameRect.width()))));
        if (!badge.isEmpty() && isVisualTrack(track.type)) {
            const QRectF pc = badge.translated(-24, 0);
            const bool framed = !isDefaultPanCrop(c.panCrop);
            p.fillRect(pc, framed ? QColor(0x3c, 0x7c, 0xd0) : QColor(0, 0, 0, 70));
            p.setPen(QPen(framed ? Qt::white : QColor(0xc0, 0xc0, 0xc0), 1));
            p.drawRect(pc.adjusted(4, 3, -4, -3));
            p.drawRect(pc.adjusted(7, 5, -7, -5));
        }
        if (!badge.isEmpty()) {
            const bool hasFx = hasActiveEffectsList(c.effects);
            p.fillRect(badge, hasFx ? QColor(0x3c, 0xb0, 0x5a) : QColor(0, 0, 0, 70));
            p.setPen(hasFx ? Qt::white : QColor(0xc0, 0xc0, 0xc0));
            p.drawText(badge, Qt::AlignCenter, QStringLiteral("fx"));
        }

        // Fade handles.
        p.setBrush(Qt::white);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(rx0 + c.fadeIn * pxPerUs - 2, r.top(), 4, 4));
        p.drawRect(QRectF(rx1 - c.fadeOut * pxPerUs - 2, r.top(), 4, 4));
        p.setBrush(Qt::NoBrush);

        p.setPen(QPen(selected ? theme::c().clipSelected : base.lighter(140), selected ? 2 : 1));
        p.drawRect(QRectF(rx0 + 0.5, r.top() + 0.5, rx1 - rx0 - 1, r.height() - 1).intersected(vr.adjusted(-1, 0, 1, 0)));
    }
}

void TimelineWidget::paintThumbnails(QPainter& p, const Clip& c, const QRectF& body)
{
    auto media = m_project->mediaItem(c.mediaId);
    const VideoStreamInfo* vs = media ? media->info.videoStream(c.streamIndex) : nullptr;
    if (!vs || body.height() < 10)
        return;
    const QSize disp = vs->displaySize();
    const double thumbH = body.height();
    const double thumbW = std::max(8.0, thumbH * disp.width() / std::max(1, disp.height()));
    const int W = viewport()->width();
    const double vis0 = std::max(body.left(), double(kHeaderWidth));
    const double vis1 = std::min(body.right(), double(W));
    if (vis1 <= vis0)
        return;

    // Quantise source times so neighbouring repaints reuse cached thumbnails.
    static const TimeUs steps[] = {0, 100000, 250000, 500000, 1000000, 2000000, 5000000, 10000000,
                                   30000000, 60000000, 120000000, 300000000, 600000000};
    const TimeUs slotDur = TimeUs(thumbW * kUsPerSecond / m_pxPerSec);
    TimeUs step = m_project->settings().fps.frameDuration();
    for (TimeUs s : steps)
        if (s > 0 && s <= slotDur)
            step = s;

    const int first = std::max(0, int((vis0 - body.left()) / thumbW));
    const int last = int((vis1 - body.left()) / thumbW);
    for (int i = first; i <= last; ++i) {
        const double sx = body.left() + i * thumbW;
        TimeUs src = c.sourceTimeAt(xToTime(sx));
        src = (src / step) * step;
        const QImage img = m_cache->thumbnail(media, c.streamIndex, src);
        const QRectF target(sx, body.top(), thumbW, thumbH);
        if (img.isNull())
            p.fillRect(target.adjusted(1, 1, -1, -1), QColor(0x20, 0x20, 0x24));
        else
            p.drawImage(target, img);
    }
}

void TimelineWidget::paintWaveform(QPainter& p, const Clip& c, const QRectF& body)
{
    auto media = m_project->mediaItem(c.mediaId);
    if (!media)
        return;
    auto wave = m_cache->waveform(media, c.streamIndex);
    const int W = viewport()->width();
    const int vis0 = int(std::max(body.left(), double(kHeaderWidth)));
    const int vis1 = int(std::min(body.right(), double(W)));
    const double cy = body.center().y();
    const double half = body.height() / 2 - 1;
    p.setPen(QColor(0, 0, 0, 80));
    p.drawLine(QPointF(vis0, cy), QPointF(vis1, cy));
    if (!wave || wave->buckets() == 0)
        return;

    const double bucketsPerUs = double(wave->bucketsPerSecond) / kUsPerSecond;
    const int64_t nb = int64_t(wave->buckets());
    std::vector<QLineF> lines;
    lines.reserve(size_t(std::max(0, vis1 - vis0)));
    for (int x = vis0; x < vis1; ++x) {
        const double sa = double(c.sourceTimeAt(xToTime(x))), sb = double(c.sourceTimeAt(xToTime(x + 1)));
        int64_t b0 = int64_t(std::min(sa, sb) * bucketsPerUs);
        int64_t b1 = std::max(b0 + 1, int64_t(std::max(sa, sb) * bucketsPerUs));
        if (b0 >= nb || b1 <= 0)
            continue;
        b0 = std::max<int64_t>(0, b0);
        b1 = std::min(b1, nb);
        int lo = 0, hi = 0;
        for (int64_t b = b0; b < b1; ++b) {
            lo = std::min<int>(lo, wave->peaks[size_t(b) * 2]);
            hi = std::max<int>(hi, wave->peaks[size_t(b) * 2 + 1]);
        }
        lines.emplace_back(x + 0.5, cy - hi * half / 127.0, x + 0.5, cy - lo * half / 127.0 + 1);
    }
    p.setPen(theme::c().waveform);
    p.drawLines(lines.data(), int(lines.size()));
}

void TimelineWidget::paintRuler(QPainter& p)
{
    const int W = viewport()->width();
    const QRect bar(kHeaderWidth, 0, W - kHeaderWidth, kRegionBarH);
    const QRect ruler(kHeaderWidth, kRulerTop, W - kHeaderWidth, kRulerH);
    p.fillRect(bar, theme::c().markerBar);
    p.fillRect(ruler, theme::c().ruler);
    p.save();
    p.setClipRect(QRect(kHeaderWidth, 0, W - kHeaderWidth, kTopH));

    if (hasRegion()) {
        const double x0 = timeToX(m_regionStart), x1 = timeToX(m_regionEnd);
        p.fillRect(QRectF(x0, 2, x1 - x0, kRegionBarH - 4), theme::c().region);
        p.setBrush(QColor(0xe0, 0xe8, 0xff));
        p.setPen(Qt::NoPen);
        const QPointF a[] = {{x0, 0}, {x0 + 6, 0}, {x0, double(kRegionBarH)}};
        const QPointF b[] = {{x1, 0}, {x1 - 6, 0}, {x1, double(kRegionBarH)}};
        p.drawPolygon(a, 3);
        p.drawPolygon(b, 3);
        p.setBrush(Qt::NoBrush);
    }
    paintMarkers(p);

    // Major tick spacing: the smallest "nice" interval at least ~100 px apart.
    const FrameRate fps = m_project->settings().fps;
    const TimeUs fd = fps.frameDuration();
    const TimeUs candidates[] = {fd, 2 * fd, 5 * fd, 10 * fd, kUsPerSecond, 2 * kUsPerSecond, 5 * kUsPerSecond,
                                 10 * kUsPerSecond, 15 * kUsPerSecond, 30 * kUsPerSecond, 60 * kUsPerSecond,
                                 120 * kUsPerSecond, 300 * kUsPerSecond, 600 * kUsPerSecond, 900 * kUsPerSecond,
                                 1800 * kUsPerSecond, 3600 * kUsPerSecond, 7200 * kUsPerSecond};
    TimeUs major = candidates[std::size(candidates) - 1];
    for (TimeUs c : candidates)
        if (c * m_pxPerSec / kUsPerSecond >= 100) {
            major = c;
            break;
        }
    const TimeUs minor = major >= kUsPerSecond && major % (5 * kUsPerSecond) == 0 ? major / 5
                         : major <= 10 * fd                                      ? std::max<TimeUs>(fd, major / 5)
                                                                                 : major / 2;
    const bool frameTicks = major < kUsPerSecond;
    auto tickTime = [&](int64_t n, TimeUs step) {
        if (frameTicks)
            return fps.timeOf(fps.frameAt(n * step + step / 2));
        return n * step;
    };

    const TimeUs t0 = xToTime(kHeaderWidth), t1 = xToTime(W);
    p.setPen(theme::c().dim);
    for (int64_t n = std::max<int64_t>(0, t0 / minor); n * minor <= t1 + minor; ++n) {
        const double x = std::round(timeToX(tickTime(n, minor))) + 0.5;
        p.drawLine(QPointF(x, kTopH - 5), QPointF(x, kTopH));
    }
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    for (int64_t n = std::max<int64_t>(0, t0 / major); n * major <= t1 + major; ++n) {
        const TimeUs t = tickTime(n, major);
        const double x = std::round(timeToX(t)) + 0.5;
        p.setPen(theme::c().text);
        p.drawLine(QPointF(x, kRulerTop + 4), QPointF(x, kTopH));
        p.setPen(theme::c().text);
        p.drawText(QPointF(x + 4, kRulerTop + 14), formatTimecode(t, fps));
    }

    // Cursor marker on the ruler.
    const double cx = timeToX(m_cursor);
    p.setBrush(theme::c().cursor);
    p.setPen(Qt::NoPen);
    const QPointF tri[] = {{cx - 5, double(kRulerTop)}, {cx + 5, double(kRulerTop)}, {cx, double(kRulerTop + 7)}};
    p.drawPolygon(tri, 3);
    if (m_playhead) {
        const double px = timeToX(*m_playhead);
        p.setBrush(theme::c().playhead);
        const QPointF tri2[] = {{px - 5, double(kRulerTop)}, {px + 5, double(kRulerTop)}, {px, double(kRulerTop + 7)}};
        p.drawPolygon(tri2, 3);
    }
    p.restore();
}

void TimelineWidget::paintHeaders(QPainter& p)
{
    const int H = viewport()->height();
    const auto& tracks = m_project->timeline().tracks;
    p.save();
    p.setClipRect(QRect(0, kTopH, kHeaderWidth, H - kTopH));
    p.fillRect(QRect(0, kTopH, kHeaderWidth, H - kTopH), theme::c().timelineBg);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);

    int videoNo = 0, audioNo = 0, imageNo = 0;
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const Track& t = tracks[ti];
        const int number = t.type == TrackType::Image ? ++imageNo : t.type == TrackType::Video ? ++videoNo : ++audioNo;
        const QRect hr = headerRect(ti);
        if (hr.bottom() < kTopH || hr.top() > H)
            continue;
        const bool isVideo = isVisualTrack(t.type);
        const QColor color = trackColor(t.type);
        p.fillRect(hr, ti == m_selectedTrack ? theme::c().headerSelected : theme::c().headerBg);
        p.fillRect(QRect(hr.left(), hr.top(), 4, hr.height()), color.lighter(130));
        p.setPen(theme::c().deep);
        p.drawLine(hr.left(), hr.bottom(), hr.right(), hr.bottom());

        // Number badge + name.
        const QRect badge(hr.left() + 8, hr.top() + 4, 18, 18);
        p.fillRect(badge, color);
        p.setPen(theme::c().clipText);
        p.setFont(small);
        p.drawText(badge, Qt::AlignCenter, QString::number(number));
        p.setFont(font());
        p.setPen(theme::c().text);
        const QRect nameRect(hr.left() + 30, hr.top() + 4, hr.width() - (t.duckUnder ? 118 : 90), 18);
        const QString name = t.name.isEmpty() ? (t.type == TrackType::Image ? tr("Image") : isVideo ? tr("Video") : tr("Audio")) : t.name;
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(name, Qt::ElideRight, nameRect.width()));

        // Mute / solo.
        auto button = [&](const QRect& r, const QString& label, bool on, const QColor& onColor) {
            p.fillRect(r, on ? onColor : theme::c().raised);
            p.setPen(on ? theme::c().window : theme::c().dim);
            p.setFont(small);
            p.drawText(r, Qt::AlignCenter, label);
        };
        button(fxRect(hr), QStringLiteral("fx"), hasActiveEffectsList(t.effects), theme::c().good);
        button(muteRect(hr), QStringLiteral("M"), t.muted, theme::c().key);
        button(soloRect(hr), QStringLiteral("S"), t.solo, theme::c().warn);

        // Level fader.
        if (hr.height() >= 44) {
            const QRect lr = levelRect(hr);
            const double pos = isVideo ? double(t.level) : posFromVolume(t.level);
            p.fillRect(lr, theme::c().field);
            p.fillRect(QRect(lr.left(), lr.top(), int(lr.width() * pos), lr.height()),
                       color);
            p.setPen(theme::c().text);
            p.setFont(small);
            const QString text = isVideo ? tr("Level: %1%").arg(int(std::lround(t.level * 100)))
                                         : tr("Vol: %1").arg(volumeText(t.level));
            p.drawText(lr.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
        // Pan.
        if (!isVideo && hr.height() >= 62) {
            const QRect pr = panRect(hr);
            p.fillRect(pr, theme::c().field);
            const int cx = pr.left() + pr.width() / 2;
            const int px = pr.left() + int((t.pan + 1.0f) / 2.0f * pr.width());
            p.fillRect(QRect(std::min(cx, px), pr.top(), std::max(2, std::abs(px - cx)), pr.height()), theme::c().region);
            p.setPen(theme::c().text);
            p.setFont(small);
            p.drawText(pr.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Pan: %1").arg(panText(t.pan)));
        }
        // Audio meter (left / right) at the header's right edge.
        if (!isVideo && hr.height() >= 40) {
            const auto it = m_meters.find(t.id);
            const std::pair<float, float> lv = it == m_meters.end() ? std::pair<float, float>{0, 0} : it->second;
            const int top = hr.top() + 26, bottom = hr.bottom() - 3;
            for (int ch = 0; ch < 2; ++ch) {
                const QRect bar(hr.right() - 8 + ch * 3, top, 2, bottom - top);
                p.fillRect(bar, theme::c().field);
                const float peak = ch ? lv.second : lv.first;
                const int fill = int(bar.height() * levels::meterPos(peak));
                if (fill > 0)
                    p.fillRect(QRect(bar.left(), bar.bottom() - fill + 1, bar.width(), fill),
                               peak >= 0.98f ? theme::c().bad : peak >= 0.5f ? theme::c().warn : theme::c().good);
            }
        }
        if (!isVideo && t.duckUnder && m_project->timeline().trackIndex(t.duckUnder) >= 0) {
            // Auto ducking: a badge next to the fx button.
            const QRect badge(hr.right() - 104, hr.top() + 4, 25, 18);
            p.fillRect(badge, theme::c().key);
            p.setPen(theme::c().window);
            p.setFont(small);
            p.drawText(badge, Qt::AlignCenter, tr("duck"));
        }
        // Compositing mode.
        if (isVideo && hr.height() >= 62) {
            const QRect br = blendRect(hr);
            p.fillRect(br, t.blend ? theme::c().checked : theme::c().field);
            p.setPen(t.blend ? theme::c().bright : theme::c().dim);
            p.setFont(small);
            p.drawText(br.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Mode: %1").arg(blendModeNames().value(t.blend)));
            p.drawText(br.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("\u25BE"));
        }
    }
    p.restore();
}

// ---------------------------------------------------------------------------
// Developer check (MutantVideoProducer --editor-test)
// ---------------------------------------------------------------------------

int TimelineWidget::runEditTest(QStringList& log)
{
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        log << QStringLiteral("%1  %2").arg(ok ? QStringLiteral("PASS") : QStringLiteral("FAIL"), what);
        if (!ok)
            ++failures;
    };
    const auto secs = [](TimeUs t) { return QString::number(double(t) / kUsPerSecond, 'f', 2); };
    const auto near = [](TimeUs a, TimeUs b) { return std::llabs(a - b) <= 40000; }; // ~a frame
    if (m_project->media().empty()) {
        check(false, QStringLiteral("edit test: no media in the project"));
        return failures;
    }
    const int mediaId = m_project->media().begin()->first;
    const auto media = m_project->mediaItem(mediaId);
    const FrameRate fps = m_project->settings().fps;
    const bool snapWas = m_snapping;
    m_snapping = false;
    m_viewStart = 0;
    m_pxPerSec = 100;
    m_scrollY = 0;
    updateScrollBars();
    auto mouse = [&](QEvent::Type type, QPoint pos, Qt::MouseButton button = Qt::LeftButton) {
        QMouseEvent ev(type, QPointF(pos), mapToGlobal(QPointF(pos)), button,
                       type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(button), Qt::NoModifier);
        QApplication::sendEvent(viewport(), &ev);
    };
    auto drag = [&](QPoint from, QPoint to) {
        mouse(QEvent::MouseButtonPress, from);
        for (int i = 1; i <= 4; ++i)
            mouse(QEvent::MouseMove, from + (to - from) * i / 4, Qt::NoButton);
        mouse(QEvent::MouseButtonRelease, to);
    };
    const int markerY = kRegionBarH + kMarkerBarH / 2;

    // Two grouped video + audio events of 2 s, A at 0 and B at 3 s.
    Timeline base = defaultTimeline();
    base.nextId = std::max(base.nextId, m_project->timeline().nextId);
    ops::insertMedia(base, *media, 0, -1, fps);
    ops::insertMedia(base, *media, 3 * kUsPerSecond, -1, fps);
    for (auto& t : base.tracks)
        for (auto& c : t.clips)
            c.length = std::min<TimeUs>(c.length, 2 * kUsPerSecond);
    m_project->applyEdit(base, QStringLiteral("edit test"));
    const int baseUndo = m_project->undoStack()->index();
    const int videoTrack = 0, audioTrack = 1;
    auto clipAt = [&](int track, int i) -> const Clip& { return m_project->timeline().tracks[size_t(track)].clips[size_t(i)]; };
    const int idA = clipAt(videoTrack, 0).id;
    auto selectA = [&] { setSelection(ops::expandToGroups(m_project->timeline(), {clipAt(videoTrack, 0).id})); };
    auto reset = [&] {
        while (m_project->undoStack()->index() > baseUndo)
            m_project->undoStack()->undo();
        setSelection({});
    };
    const int clipY = trackTop(videoTrack) + 40;

    // ---- Markers and regions ----
    addMarker(1 * kUsPerSecond);
    addMarker(3 * kUsPerSecond, QStringLiteral("drop"));
    setRegion(2 * kUsPerSecond, 4 * kUsPerSecond);
    addRegionFromSelection();
    const auto& markers = m_project->timeline().markers;
    check(markers.size() == 3 && markerLabel(markers[0]) == QStringLiteral("1") && markerLabel(markers[2]) == QStringLiteral("2 drop")
              && markers[1].isRegion() && markerLabel(markers[1]) == QStringLiteral("R1"),
          QStringLiteral("M adds markers, R a region from the time selection; numbered 1, 2 drop, R1"));
    const int dropId = markers[2].id;
    drag(QPoint(int(timeToX(3 * kUsPerSecond)) + 2, markerY), QPoint(int(timeToX(5 * kUsPerSecond)) + 2, markerY));
    const Marker* moved = m_project->timeline().findMarker(dropId);
    check(moved && near(moved->time, 5 * kUsPerSecond), QStringLiteral("dragging a marker in the marker bar moves it: %1 s").arg(moved ? secs(moved->time) : QStringLiteral("?")));
    m_project->undoStack()->undo();
    check(m_project->timeline().findMarker(dropId)->time == 3 * kUsPerSecond, QStringLiteral("undo puts the marker back"));
    const int regionId = markers[1].id;
    drag(QPoint(int(timeToX(4 * kUsPerSecond)), markerY), QPoint(int(timeToX(6 * kUsPerSecond)), markerY));
    const Marker* region = m_project->timeline().findMarker(regionId);
    check(region && region->time == 2 * kUsPerSecond && near(region->end(), 6 * kUsPerSecond),
          QStringLiteral("dragging a region's end edge resizes it: %1 - %2 s").arg(region ? secs(region->time) : QString(), region ? secs(region->end()) : QString()));
    setCursorTime(0);
    mouse(QEvent::MouseButtonPress, QPoint(int(timeToX(1 * kUsPerSecond)) + 3, markerY));
    mouse(QEvent::MouseButtonRelease, QPoint(int(timeToX(1 * kUsPerSecond)) + 3, markerY));
    check(m_cursor == 1 * kUsPerSecond, QStringLiteral("clicking a marker moves the cursor to it"));
    jumpToMarker(1);
    const TimeUs j1 = m_cursor;
    jumpToMarker(1);
    const TimeUs j2 = m_cursor;
    jumpToMarker(-1);
    check(j1 == 2 * kUsPerSecond && j2 == 3 * kUsPerSecond && m_cursor == j1,
          QStringLiteral("] / [ step through markers and region edges (%1, %2, back to %3 s)").arg(secs(j1), secs(j2), secs(m_cursor)));
    setRegion(0, 0);
    setInPoint(kUsPerSecond);
    setOutPoint(2500000);
    check(hasRegion() && m_regionStart == kUsPerSecond && near(m_regionEnd, 2500000), QStringLiteral("I / O set the time selection: %1 - %2 s").arg(secs(m_regionStart), secs(m_regionEnd)));
    setRegion(0, 0);
    reset();

    // ---- Ripple ----
    // Trim A's end 0.5 s shorter: Off leaves B, Tracks pulls B (video and audio) along.
    auto trimEnd = [&] {
        selectA();
        const TimeUs end = clipAt(videoTrack, 0).end();
        drag(QPoint(int(timeToX(end)) - 1, clipY), QPoint(int(timeToX(end - kUsPerSecond / 2)) - 1, clipY));
    };
    m_ripple = RippleMode::Off;
    trimEnd();
    check(clipAt(videoTrack, 0).length == 1500000 && clipAt(videoTrack, 1).start == 3 * kUsPerSecond,
          QStringLiteral("ripple off: trimming A leaves B at 3 s (A %1 s)").arg(secs(clipAt(videoTrack, 0).length)));
    reset();
    m_ripple = RippleMode::Tracks;
    trimEnd();
    check(clipAt(videoTrack, 1).start == 2500000 && clipAt(audioTrack, 1).start == 2500000,
          QStringLiteral("ripple tracks: trimming A's end pulls B back on both tracks (video %1 s, audio %2 s)")
              .arg(secs(clipAt(videoTrack, 1).start), secs(clipAt(audioTrack, 1).start)));
    reset();
    // Trimming the start: A stays at 0, its footage starts later, B follows.
    selectA();
    const TimeUs inBefore = clipAt(videoTrack, 0).sourceIn;
    drag(QPoint(int(timeToX(0)) + 1, clipY), QPoint(int(timeToX(kUsPerSecond / 2)) + 1, clipY));
    check(clipAt(videoTrack, 0).start == 0 && clipAt(videoTrack, 0).length == 1500000 && clipAt(videoTrack, 0).sourceIn - inBefore == 500000
              && clipAt(videoTrack, 1).start == 2500000,
          QStringLiteral("ripple trim of the start: A stays at 0 s, starts 0.5 s later in its media, B moves to %1 s").arg(secs(clipAt(videoTrack, 1).start)));
    reset();
    // Moving A 1 s later moves B too.
    selectA();
    drag(QPoint(int(timeToX(kUsPerSecond)), clipY), QPoint(int(timeToX(2 * kUsPerSecond)), clipY));
    check(clipAt(videoTrack, 0).start == kUsPerSecond && clipAt(videoTrack, 1).start == 4 * kUsPerSecond && clipAt(audioTrack, 1).start == 4 * kUsPerSecond,
          QStringLiteral("ripple move: B follows A 1 s later (%1 s)").arg(secs(clipAt(videoTrack, 1).start)));
    reset();
    // Delete with ripple on: B closes up; All mode moves markers too.
    addMarker(4 * kUsPerSecond);
    selectA();
    deleteSelection(false);
    const bool tracksDelete = clipAt(videoTrack, 0).start == kUsPerSecond && m_project->timeline().markers.front().time == 4 * kUsPerSecond;
    m_project->undoStack()->undo();
    m_ripple = RippleMode::All;
    selectA();
    deleteSelection(false);
    check(tracksDelete && clipAt(videoTrack, 0).start == kUsPerSecond && clipAt(audioTrack, 0).start == kUsPerSecond
              && m_project->timeline().markers.front().time == 2 * kUsPerSecond,
          QStringLiteral("ripple delete: B moves to 1 s; markers stay with Tracks, move with All (%1 s)")
              .arg(secs(m_project->timeline().markers.front().time)));
    reset();
    // Paste with ripple: inserts, splitting A at the cursor.
    m_ripple = RippleMode::Tracks;
    setSelection(ops::expandToGroups(m_project->timeline(), {clipAt(videoTrack, 1).id}));
    copySelection();
    setCursorTime(kUsPerSecond);
    paste();
    const auto& vclips = m_project->timeline().tracks[size_t(videoTrack)].clips;
    QStringList layout;
    for (const Clip& c : vclips)
        layout << QStringLiteral("%1-%2").arg(secs(c.start), secs(c.end()));
    check(vclips.size() == 4 && vclips[0].end() == kUsPerSecond && vclips[1].start == kUsPerSecond && vclips[2].start == 3 * kUsPerSecond
              && vclips[3].start == 5 * kUsPerSecond && m_project->timeline().tracks[size_t(audioTrack)].clips.size() == 4,
          QStringLiteral("ripple paste inserts: A split at 1 s, the rest moves 2 s later (%1)").arg(layout.join(QStringLiteral(", "))));
    reset();
    m_ripple = RippleMode::Off;

    // ---- Removing transitions ----
    {
        // A transition at A's start (from black): its label's × removes it.
        Timeline tl = m_project->timeline();
        setTransition(tl, idA, true, QStringLiteral("iris"));
        m_project->applyEdit(tl, QStringLiteral("t"));
        const int h = trackHeight(m_project->timeline().tracks[size_t(videoTrack)]);
        const QRectF label = transitionLabelRect(m_project->timeline().tracks[size_t(videoTrack)], 0, trackTop(videoTrack), h, true);
        const QPoint x = transitionCloseRect(label, true).center().toPoint();
        const bool shown = !label.isEmpty() && hitTest(x).zone == Zone::TransitionRemove;
        mouse(QEvent::MouseButtonPress, x);
        mouse(QEvent::MouseButtonRelease, x);
        check(shown && clipAt(videoTrack, 0).fadeIn == 0 && clipAt(videoTrack, 0).transitionIn.type.isEmpty(),
              QStringLiteral("a transition's label on the timeline has a \u00D7 that removes it (hard cut)"));
        reset();
        // A crossfade (B overlapping A by 0.5 s): removing it cuts A where B starts, video and audio.
        Timeline ov = m_project->timeline();
        for (auto& t : ov.tracks)
            t.clips[1].start = 1500000;
        m_project->applyEdit(ov, QStringLiteral("t"));
        const QRectF xlabel = transitionLabelRect(m_project->timeline().tracks[size_t(videoTrack)], 1, trackTop(videoTrack), h, true);
        const QPoint xx = transitionCloseRect(xlabel, true).center().toPoint();
        mouse(QEvent::MouseButtonPress, xx);
        mouse(QEvent::MouseButtonRelease, xx);
        check(clipAt(videoTrack, 0).end() == 1500000 && clipAt(audioTrack, 0).end() == 1500000 && clipAt(videoTrack, 1).start == 1500000
                  && clipAt(videoTrack, 1).length == 2 * kUsPerSecond,
              QStringLiteral("removing a crossfade cuts the earlier event (video and audio) where the later one starts: %1 s")
                  .arg(secs(clipAt(videoTrack, 0).end())));
        m_project->undoStack()->undo();
        check(clipAt(videoTrack, 0).end() == 2 * kUsPerSecond, QStringLiteral("undo brings the crossfade back"));
        reset();
    }

    // ---- Grouped video and audio share transitions ----
    {
        Timeline tl = m_project->timeline();
        setTransition(tl, idA, true, QStringLiteral("iris"));
        setTransition(tl, idA, false, QStringLiteral("dissolve"));
        m_project->applyEdit(tl, QStringLiteral("t"));
        const bool same = clipAt(audioTrack, 0).fadeIn == clipAt(videoTrack, 0).fadeIn && clipAt(audioTrack, 0).fadeIn > 0
                          && clipAt(audioTrack, 0).fadeOut == clipAt(videoTrack, 0).fadeOut && clipAt(audioTrack, 0).fadeOut > 0;
        // The audio fade has a × too, and removing it removes the video's transition as well.
        const int ha = trackHeight(m_project->timeline().tracks[size_t(audioTrack)]);
        const QRectF label = transitionLabelRect(m_project->timeline().tracks[size_t(audioTrack)], 0, trackTop(audioTrack), ha, true);
        const QPoint x = transitionCloseRect(label, true).center().toPoint();
        const bool audioX = !label.isEmpty() && hitTest(x).zone == Zone::TransitionRemove;
        mouse(QEvent::MouseButtonPress, x);
        mouse(QEvent::MouseButtonRelease, x);
        check(same && audioX && clipAt(audioTrack, 0).fadeIn == 0 && clipAt(videoTrack, 0).fadeIn == 0
                  && clipAt(videoTrack, 0).transitionIn.type.isEmpty() && clipAt(audioTrack, 0).fadeOut > 0,
              QStringLiteral("a transition on a video file's picture fades its sound too; the audio fade's \u00D7 removes both"));
        reset();
        // Ungrouped: the transition is the picture's alone.
        selectA();
        ungroupSelection();
        Timeline tl2 = m_project->timeline();
        setTransition(tl2, idA, true, QStringLiteral("iris"));
        m_project->applyEdit(tl2, QStringLiteral("t"));
        check(clipAt(videoTrack, 0).groupId == 0 && clipAt(videoTrack, 0).fadeIn > 0 && clipAt(audioTrack, 0).fadeIn == 0,
              QStringLiteral("after Ungroup (U) the picture's transition leaves the sound alone"));
        reset();
    }

    // ---- Selecting, nudging, trimming to the cursor, freeze frames ----
    {
        // A box dragged from empty space (below the tracks) up over both events
        // selects them and their grouped audio.
        setSelection({});
        const QPoint emptyStart(int(timeToX(5500000)), trackTop(audioTrack) + trackHeight(m_project->timeline().tracks[size_t(audioTrack)]) + 10);
        drag(emptyStart, QPoint(int(timeToX(kUsPerSecond / 2)), trackTop(videoTrack) + 30));
        check(m_selection.size() == 4 && !hasRegion(),
              QStringLiteral("dragging a box from empty space selects the events it touches, with their grouped audio (%1 selected)").arg(m_selection.size()));
        setSelection({});
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(emptyStart), mapToGlobal(QPointF(emptyStart)), Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        QApplication::sendEvent(viewport(), &press);
        for (int i = 1; i <= 3; ++i) {
            const QPointF at(emptyStart.x() - 60 * i, emptyStart.y());
            QMouseEvent mv(QEvent::MouseMove, at, mapToGlobal(at), Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
            QApplication::sendEvent(viewport(), &mv);
        }
        QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(emptyStart.x() - 180, emptyStart.y()), mapToGlobal(QPointF(emptyStart.x() - 180, emptyStart.y())),
                        Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        QApplication::sendEvent(viewport(), &rel);
        check(hasRegion() && m_selection.empty(), QStringLiteral("Shift+drag in empty space makes a time selection instead"));
        setRegion(0, 0);

        selectA();
        nudgeSelection(3);
        const TimeUs frame3 = fps.timeOf(3);
        check(clipAt(videoTrack, 0).start == frame3 && clipAt(audioTrack, 0).start == frame3,
              QStringLiteral("Alt+Right nudges the selected events (and their audio) by frames"));
        reset();
        selectA();
        setCursorTime(kUsPerSecond / 2);
        trimSelectionToCursor(true);
        const bool trimStart = clipAt(videoTrack, 0).start == kUsPerSecond / 2 && clipAt(audioTrack, 0).start == kUsPerSecond / 2;
        setCursorTime(1500000);
        trimSelectionToCursor(false);
        check(trimStart && clipAt(videoTrack, 0).end() == 1500000 && clipAt(audioTrack, 0).end() == 1500000,
              QStringLiteral("Alt+[ / Alt+] trim the selected events' start / end to the cursor"));
        reset();
        setSelection({});
        setCursorTime(kUsPerSecond);
        const int freezeId = insertFreezeFrame(kUsPerSecond);
        const Clip* fz = m_project->timeline().findClip(freezeId);
        const auto& vclips2 = m_project->timeline().tracks[size_t(videoTrack)].clips;
        QStringList lay;
        for (const Clip& c : vclips2)
            lay << QStringLiteral("%1-%2").arg(secs(c.start), secs(c.end()));
        check(fz && fz->start == kUsPerSecond && fz->length == kUsPerSecond && fz->hasRamp() && fz->velocity.value == 0.0
                  && vclips2.size() == 4 && vclips2[2].start == 2 * kUsPerSecond && m_project->timeline().tracks[size_t(audioTrack)].clips[1].start == 2 * kUsPerSecond,
              QStringLiteral("Shift+F holds the frame at the cursor for 1 s and pushes the rest (and its audio) later (%1)").arg(lay.join(QStringLiteral(", "))));
        reset();
        selectA();
        setFadeCurveOnSelection(true, 4);
        check(clipAt(videoTrack, 0).fadeInCurve == 4 && clipAt(audioTrack, 0).fadeInCurve == 4,
              QStringLiteral("fade curves can be set on the selected events (Constant Power)"));
        reset();
    }

    // ---- Slip (Alt+drag) and slide (Ctrl+Alt+drag) ----
    {
        auto altDrag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers mods) {
            auto send = [&](QEvent::Type type, QPoint pos) {
                QMouseEvent ev(type, QPointF(pos), mapToGlobal(QPointF(pos)), Qt::LeftButton,
                               type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), mods);
                QApplication::sendEvent(viewport(), &ev);
            };
            send(QEvent::MouseButtonPress, from);
            for (int i = 1; i <= 4; ++i)
                send(QEvent::MouseMove, from + (to - from) * i / 4);
            send(QEvent::MouseButtonRelease, to);
        };
        // Three video events side by side (1 s each, from the middle of the file), no audio.
        Timeline t3 = defaultTimeline();
        t3.nextId = std::max(t3.nextId, m_project->timeline().nextId);
        for (int i = 0; i < 3; ++i) {
            Clip c;
            c.id = t3.allocId();
            c.mediaId = mediaId;
            c.streamIndex = media->info.video.front().index;
            c.start = TimeUs(i) * kUsPerSecond;
            c.length = kUsPerSecond;
            c.sourceIn = kUsPerSecond;
            t3.tracks[size_t(videoTrack)].clips.push_back(c);
        }
        m_project->applyEdit(t3, QStringLiteral("slip test"));
        const Clip a = clipAt(videoTrack, 0), b = clipAt(videoTrack, 1), c = clipAt(videoTrack, 2);
        setSelection({b.id});
        const int y = trackTop(videoTrack) + 40;
        // Slip B 0.5 s right: the same place and length, footage from 0.5 s earlier.
        altDrag(QPoint(int(timeToX(1500000)), y), QPoint(int(timeToX(2000000)), y), Qt::AltModifier);
        const Clip& slipped = clipAt(videoTrack, 1);
        const bool slip = slipped.start == b.start && slipped.length == b.length && near(slipped.sourceIn, b.sourceIn - 500000);
        // As far as the footage goes: 3 s further right stops at its start.
        altDrag(QPoint(int(timeToX(1500000)), y), QPoint(int(timeToX(4500000)), y), Qt::AltModifier);
        const bool slipStops = clipAt(videoTrack, 1).sourceIn == 0;
        check(slip && slipStops && m_project->undoStack()->undoText() == tr("Slip Event"),
              QStringLiteral("Alt+drag slips the footage inside an event (in point %1 -> %2 s), stopping at the file's start")
                  .arg(secs(b.sourceIn), secs(slipped.sourceIn)));
        m_project->undoStack()->undo();
        m_project->undoStack()->undo();
        // Slide B 0.4 s right: A ends later, C starts later with its footage trimmed to match.
        setSelection({b.id});
        altDrag(QPoint(int(timeToX(1500000)), y), QPoint(int(timeToX(1900000)), y), Qt::AltModifier | Qt::ControlModifier);
        const Clip &a2 = clipAt(videoTrack, 0), &b2 = clipAt(videoTrack, 1), &c2 = clipAt(videoTrack, 2);
        check(near(b2.start, b.start + 400000) && b2.sourceIn == b.sourceIn && a2.end() == b2.start && c2.start == b2.end()
                  && near(c2.length, c.length - 400000) && near(c2.sourceIn, c.sourceIn + 400000) && near(a2.length, a.length + 400000),
              QStringLiteral("Ctrl+Alt+drag slides an event between its neighbours (now %1 s; the one before ends there, the one after starts at its end)")
                  .arg(secs(b2.start)));
        reset();
    }

    // ---- Ctrl+drag copies, Ctrl+click toggles, roll the cut, loop past the end, grid, crossfades off ----
    {
        auto modMouse = [&](QEvent::Type type, QPoint pos, Qt::KeyboardModifiers mods) {
            QMouseEvent ev(type, QPointF(pos), mapToGlobal(QPointF(pos)), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), mods);
            QApplication::sendEvent(viewport(), &ev);
        };
        auto modDrag = [&](QPoint from, QPoint to, Qt::KeyboardModifiers mods) {
            modMouse(QEvent::MouseButtonPress, from, mods);
            if (from != to)
                for (int i = 1; i <= 4; ++i)
                    modMouse(QEvent::MouseMove, from + (to - from) * i / 4, mods);
            modMouse(QEvent::MouseButtonRelease, to, mods);
        };
        const int y = trackTop(videoTrack) + 40;
        auto at = [&](double seconds) { return QPoint(int(std::lround(timeToX(TimeUs(seconds * kUsPerSecond)))), y); };
        const TimeUs S = kUsPerSecond;
        const int groupA = clipAt(videoTrack, 0).groupId;
        // Ctrl+drag A (0-2 s) by 6 s: a copy at 6 s with its audio, A where it was.
        setSelection({});
        modDrag(at(1), at(7), Qt::ControlModifier);
        const auto& vt = m_project->timeline().tracks[size_t(videoTrack)].clips;
        const auto& at2 = m_project->timeline().tracks[size_t(audioTrack)].clips;
        const bool copied = vt.size() == 3 && at2.size() == 3 && vt[0].start == 0 && vt[2].start == 6 * S && at2[2].start == 6 * S
                            && vt[2].groupId && vt[2].groupId == at2[2].groupId && vt[2].groupId != groupA && m_selection.count(vt[2].id)
                            && m_project->undoStack()->undoText() == tr("Copy Events");
        reset();
        // Ctrl+click: adds an event to the selection, and takes a selected one out.
        selectA();
        modDrag(at(4), at(4), Qt::ControlModifier); // B in
        const bool added = m_selection.count(clipAt(videoTrack, 0).id) && m_selection.count(clipAt(videoTrack, 1).id);
        modDrag(at(1), at(1), Qt::ControlModifier); // A out
        const bool removed = !m_selection.count(clipAt(videoTrack, 0).id) && m_selection.count(clipAt(videoTrack, 1).id);
        check(copied && added && removed,
              QStringLiteral("Ctrl+drag copies an event with its audio (the copy at 6 s, the original where it was); Ctrl+click adds "
                             "an event to the selection and takes a selected one out"));
        reset();
        // Roll: A ends where B starts (3 s); Ctrl+Alt+drag the cut to 3.5 s: A longer, B shorter, from later in its file.
        {
            Timeline tl = m_project->timeline();
            for (auto& t : tl.tracks)
                for (auto& c : t.clips)
                    if (c.start == 0)
                        c.length = 3 * S;
            m_project->applyEdit(tl, QStringLiteral("touching"));
        }
        setSelection({});
        const QPoint cut(int(std::lround(timeToX(3 * S))) - 2, y);
        modDrag(cut, at(3.5), Qt::ControlModifier | Qt::AltModifier);
        const Clip& ra = clipAt(videoTrack, 0);
        const Clip& rb = clipAt(videoTrack, 1);
        const Clip& raAudio = clipAt(audioTrack, 0);
        const bool rolled = near(ra.end(), 3500000) && near(rb.start, 3500000) && rb.end() == 5 * S && near(rb.sourceIn, 500000)
                            && near(raAudio.end(), 3500000) && near(clipAt(audioTrack, 1).start, 3500000)
                            && m_project->undoStack()->undoText() == tr("Trim Adjacent Events");
        check(rolled, QStringLiteral("Ctrl+Alt+drag the cut between touching events rolls it (now %1 s): the first ends there, the "
                                     "second starts there from later in its file, their audio too, nothing else moves")
                          .arg(secs(rb.start)));
        reset();
        // Loop: drag B's end (5 s) out to 12 s, past the end of its 4 s file: it loops, with notches.
        setSelection({});
        drag(QPoint(int(std::lround(timeToX(5 * S))) - 2, y), at(12));
        const Clip& lb = clipAt(videoTrack, 1);
        const bool looped = near(lb.end(), 12 * S) && lb.loopLength == media->info.duration && lb.loops()
                            && lb.sourceTimeAt(lb.start + media->info.duration + S / 2) == S / 2;
        check(looped, QStringLiteral("dragging an event's end past the end of its file loops it (now %1 s long, the file every %2 s)")
                          .arg(secs(lb.length)).arg(secs(lb.loopLength)));
        reset();
        // Grid: seconds; with snapping, A dragged by 1.95 s lands on 2 s.
        m_snapping = true;
        setGrid(Grid::Seconds);
        setSelection({});
        drag(at(1), at(2.95));
        const TimeUs gridStart = clipAt(videoTrack, 0).start;
        setGrid(Grid::Off);
        m_snapping = false;
        reset();
        check(gridStart == 2 * S, QStringLiteral("with the seconds grid, an event dragged by 1.95 s snaps to the line at %1 s").arg(secs(gridStart)));
        // Automatic crossfades off: B dragged over A's end cuts A there (and its audio), no overlap.
        setAutoCrossfade(false);
        setSelection({});
        drag(at(4), at(2.5)); // B 3-5 s -> 1.5-3.5 s
        const Clip& xa = clipAt(videoTrack, 0);
        const Clip& xb = clipAt(videoTrack, 1);
        const bool cutNotFaded = near(xb.start, 1500000) && near(xa.end(), 1500000) && near(clipAt(audioTrack, 0).end(), 1500000)
                                 && effectiveEnvelope(m_project->timeline().tracks[size_t(videoTrack)], 1).fadeIn == 0;
        const TimeUs cutAt = xa.end();
        setAutoCrossfade(true);
        reset();
        check(cutNotFaded, QStringLiteral("with Automatic Crossfades off, an event dragged over another's end cuts it there (%1 s), "
                                          "its audio too, instead of crossfading").arg(secs(cutAt)));
    }

    // ---- Tracks ----
    const int videoId = m_project->timeline().tracks[size_t(videoTrack)].id;
    const int audioId = m_project->timeline().tracks[size_t(audioTrack)].id;
    const QPoint header(70, trackTop(videoTrack) + 12);
    drag(header, QPoint(70, trackTop(audioTrack) + trackHeight(m_project->timeline().tracks[size_t(audioTrack)]) * 3 / 4));
    check(m_project->timeline().tracks[0].id == audioId && m_project->timeline().tracks[1].id == videoId,
          QStringLiteral("dragging a track header below another track moves it there"));
    m_project->undoStack()->undo();
    check(m_project->timeline().tracks[0].id == videoId, QStringLiteral("undo puts the track back"));
    setTrackBlend(videoTrack, 3);
    check(m_project->timeline().tracks[size_t(videoTrack)].blend == 3, QStringLiteral("a video track's compositing mode can be set (Multiply)"));
    reset();

    m_snapping = snapWas;
    return failures;
}
