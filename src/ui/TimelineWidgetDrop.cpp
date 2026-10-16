// TimelineWidget: dropping media, effects, presets and transitions.
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
