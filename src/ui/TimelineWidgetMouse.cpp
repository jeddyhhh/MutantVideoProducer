// TimelineWidget: mouse, drag, wheel and hover handling.
#include "ui/TimelineWidget.h"
#include "ui/TimelineLayout.h"
#include "core/AudioFinishing.h"
#include "core/AudioMixer.h"
#include "core/PanCrop.h"
#include "core/Presets.h"
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
#include <QKeyEvent>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QApplication>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonObject>
#include <QToolTip>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QScrollBar>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

using namespace timeline_layout;

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

    if (p.y() < kTopH && p.x() < kHeaderWidth) { // (the timecode: the next style)
        setClockStyle(m_clockStyle + 1);
        return;
    }
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
        m_rulerPress = overRuler(p);
        m_scrubFrom = m_cursor;
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
    if (hit.clipId) { // a character's expression key on a scene: dragged along it
        const Track& track = m_project->timeline().tracks[size_t(hit.track)];
        const Clip* c = m_project->timeline().findClip(hit.clipId);
        const int top = trackTop(hit.track), h = trackHeight(track);
        const QRectF body(timeToX(c->start), top + 1 + kTitleH, timeToX(c->end()) - timeToX(c->start), h - 2 - kTitleH);
        for (const MoodMark& m : moodMarks(*c, body))
            if (m.rect.adjusted(-2, -2, 2, 2).contains(p)) {
                m_before = m_project->timeline();
                m_moodClip = c->id;
                m_moodVariable = m.variable;
                m_moodIndex = m.index;
                m_moodPressTime = m.time;
                m_pressTime = xToTime(p.x());
                m_drag = Drag::MoodMove;
                return;
            }
    }
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
                const EndTransition end = endTransition(*t, size_t(c - t->clips.data()), hit.transitionIn);
                if (end.holder)
                    emit transitionRequested(end.holder, end.holderIn);
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

    // Empty space: deselect and move the cursor; dragging moves the timeline
    // left and right, Ctrl+drag draws a box that adds the events it touches to
    // the selection, Shift+drag makes a time selection.
    if (!ctrl)
        setSelection({});
    const TimeUs t = std::max<TimeUs>(0, quantize(xToTime(p.x())));
    setCursorTime(t, false);
    emit cursorMoved(t);
    buildSnapPoints({});
    m_pressTime = t;
    if (e->modifiers() & Qt::ShiftModifier) {
        m_drag = Drag::TimeSelect;
    } else if (!ctrl) {
        m_drag = Drag::Pan;
        m_panTime = xToTime(p.x());
        viewport()->setCursor(Qt::ClosedHandCursor);
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
    if (!m_dragMoved && m_drag == Drag::Scrub && m_rulerPress
        && std::abs(p.y() - m_pressPos.y()) > std::abs(p.x() - m_pressPos.x())) {
        // Up or down on the ruler zooms, around where it was grabbed (and the press doesn't move the cursor).
        m_drag = Drag::RulerZoom;
        m_zoomY = m_pressPos.y();
        if (m_cursor != m_scrubFrom) {
            setCursorTime(m_scrubFrom, false);
            emit cursorMoved(m_scrubFrom);
        }
    }
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
    case Drag::RulerZoom: // up zooms in, as the wheel does over the ruler
        zoomAround(m_pressPos.x(), std::pow(1.01, m_zoomY - p.y()));
        m_zoomY = p.y();
        break;
    case Drag::Pan: // the time grabbed stays under the mouse
        m_viewStart = std::max<TimeUs>(0, m_viewStart + m_panTime - t);
        updateScrollBars();
        viewport()->update();
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
    case Drag::MoodMove: {
        Timeline tl = m_before;
        Clip* c = tl.findClip(m_moodClip);
        if (!c)
            break;
        QJsonObject data = c->generator.data;
        QJsonObject variables = data.value(QStringLiteral("variables")).toObject();
        QJsonObject spec = variables.value(m_moodVariable).toObject();
        QJsonArray keys = spec.value(QStringLiteral("keyframes")).toArray();
        if (m_moodIndex >= keys.size())
            break;
        const double length = double(c->length) / kUsPerSecond;
        double when = std::clamp(m_moodPressTime + double(xToTime(p.x()) - m_pressTime) / kUsPerSecond, 0.0, length);
        when = double(quantize(TimeUs(when * kUsPerSecond))) / kUsPerSecond;
        QJsonArray k = keys[m_moodIndex].toArray();
        k[0] = when;
        keys.removeAt(m_moodIndex);
        int at = 0; // (in time order, and never on another key's time)
        while (at < keys.size() && keys[at].toArray()[0].toDouble() < when)
            ++at;
        if (at < keys.size() && std::abs(keys[at].toArray()[0].toDouble() - when) < 1e-6)
            k[0] = when + 0.001;
        keys.insert(at, k);
        spec[QStringLiteral("keyframes")] = keys;
        variables[m_moodVariable] = spec;
        data[QStringLiteral("variables")] = variables;
        c->generator.data = data;
        m_project->setTimelineLive(tl);
        QToolTip::showText(viewport()->mapToGlobal(p + QPoint(14, 10)),
                           tr("%1: %2 at %3").arg(m_moodVariable.chopped(5), k[1].toString(), formatTimecode(c->start + TimeUs(when * kUsPerSecond), m_project->settings().fps)),
                           viewport());
        break;
    }
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
    case Drag::MoodMove:
        if (m_dragMoved)
            m_project->commitEdit(m_before, tr("Move Expression"));
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
    case Drag::Pan: updateHoverCursor(e->position().toPoint()); break;
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
    if (p.y() < kTopH && p.x() < kHeaderWidth && e->button() == Qt::LeftButton) { // (quick clicks on the timecode)
        setClockStyle(m_clockStyle + 1);
        return;
    }
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
    } else if (p.x() < kHeaderWidth && p.y() < kTopH) {
        shape = Qt::PointingHandCursor;
        tip = tr("Timecode (%1): click for the next font, right-click for its colour").arg(clockStyleName(m_clockStyle));
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
    viewport()->setCursor(overRuler(p) ? magnifierCursor() : QCursor(shape));
    viewport()->setToolTip(tip);
}

void TimelineWidget::keyPressEvent(QKeyEvent* e)
{
    if (e->key() >= Qt::Key_1 && e->key() <= Qt::Key_9 && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier))) {
        emit angleKey(e->key() - Qt::Key_0);
        e->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(e);
}

void TimelineWidget::wheelEvent(QWheelEvent* e)
{
    const QPoint delta = e->angleDelta();
    const double x = e->position().x();
    if (e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier) || std::abs(delta.x()) > std::abs(delta.y())) {
        const int d = std::abs(delta.x()) > std::abs(delta.y()) ? delta.x() : delta.y();
        m_viewStart = std::max<TimeUs>(0, m_viewStart - TimeUs(d * visibleDuration() / 1200));
        updateScrollBars();
        viewport()->update();
    } else if (overRuler(e->position())) {
        zoomAround(x, std::pow(1.0015, delta.y()));
    } else {
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y() / 2);
    }
    e->accept();
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
