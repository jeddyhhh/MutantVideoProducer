#include "ui/TimelineWidget.h"
#include "ui/TimelineLayout.h"

#include "core/AudioFinishing.h"
#include "core/AudioMixer.h"
#include "core/PanCrop.h"
#include "core/Presets.h"
#include "core/Profiles.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsBrowser.h"
#include "ui/AudioLevels.h"
#include "ui/MediaBin.h"
#include "ui/Theme.h"

#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QPixmap>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QApplication>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QScrollBar>
#include <QSettings>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

using namespace timeline_layout;


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
    const QSettings settings;
    m_clockStyle = std::clamp(settings.value(Profiles::instance().settingsKey(QStringLiteral("timeline/clockStyle")), 0).toInt(), 0, ClockStyles - 1);
    m_clockColor = QColor(settings.value(Profiles::instance().settingsKey(QStringLiteral("timeline/clockColor"))).toString());

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
    if (track.locked)
        return hit; // its events don't respond to the mouse
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
    const std::optional<TimeUs> was = m_playhead;
    const TimeUs viewStart = m_viewStart;
    m_playhead = t;
    if (t)
        ensureTimeVisible(*t, true);
    if (was && t && m_viewStart == viewStart) {
        // Playing, only the playhead moves: repaint its old and new places and the timecode, not every clip.
        auto strip = [this](TimeUs at) { return QRect(int(timeToX(at)) - 7, 0, 14, viewport()->height()); };
        viewport()->update(strip(*was));
        viewport()->update(strip(*t));
        viewport()->update(QRect(0, 0, kHeaderWidth, kTopH));
        return;
    }
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
    for (const Track& t : m_project->timeline().tracks) // events of locked tracks can't be picked
        if (t.locked)
            for (const Clip& c : t.clips)
                sel.erase(c.id);
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
        if (ti >= int(tl.tracks.size()) || isVisualTrack(tl.tracks[ti].type) != isVisualTrack(wanted) || tl.tracks[ti].locked)
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

void TimelineWidget::toggleTrackLock(int trackIndex)
{
    Timeline tl = m_project->timeline();
    if (trackIndex < 0 || trackIndex >= int(tl.tracks.size()))
        return;
    Track& t = tl.tracks[size_t(trackIndex)];
    t.locked = !t.locked;
    m_project->applyEdit(tl, t.locked ? tr("Lock Track") : tr("Unlock Track"));
    setSelection(m_selection); // drops the events that are now locked
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

int TimelineWidget::insertFreezeFrame(TimeUs length, bool inPlace)
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
        emit message(tr("Put the cursor over a video event from a file to freeze its picture there (generated pictures can't be frozen)."));
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
    if (!inPlace)
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
    if (inPlace) // (the event is cut around the hold, its sound plays on)
        ops::overwriteOverlaps(tl, {freeze.id});
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

// ---------------------------------------------------------------------------
// Drag & drop
// ---------------------------------------------------------------------------

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
    if (track.locked)
        return none;
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

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Developer check (MutantVideoProducer --editor-test)
// ---------------------------------------------------------------------------
