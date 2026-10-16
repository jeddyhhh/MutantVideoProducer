// TimelineWidget: right-click menus.
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
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

using namespace timeline_layout;

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
                menu.addAction(tr("Track Listing for YouTube..."), this, [this, ti] {
                    const QString listing = finishing::trackListing(*m_project, ti);
                    if (listing.isEmpty()) {
                        emit message(tr("This track has no events to list."));
                        return;
                    }
                    bool ok = false;
                    const QString text = QInputDialog::getMultiLineText(
                        this, tr("Track Listing"), tr("Each event on this track with its start time. Edit the names, then OK copies the list."),
                        listing, &ok);
                    if (ok) {
                        QApplication::clipboard()->setText(text);
                        emit message(tr("Track listing copied for the video description."));
                    }
                });
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
