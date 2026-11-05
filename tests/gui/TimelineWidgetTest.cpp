// TimelineWidget's scripted GUI test, kept out of the editor's own source.
#include "ui/TimelineWidget.h"
#include "ui/TimelineLayout.h"
#include "core/AudioFinishing.h"
#include "core/AudioMixer.h"
#include "core/Generators.h"
#include "core/PanCrop.h"
#include "core/Presets.h"
#include "core/TimelineOps.h"
#include "core/Story.h"
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
    {
        // The timecode: a click moves to the next style; each style draws across the corner, in the chosen colour.
        const int style = m_clockStyle;
        const QColor colour = m_clockColor;
        mouse(QEvent::MouseButtonPress, QPoint(30, 20));
        mouse(QEvent::MouseButtonRelease, QPoint(30, 20));
        const bool next = m_clockStyle == (style + 1) % ClockStyles;
        setClockColor(QColor(0x39ff14));
        QStringList drawn;
        bool all = true;
        for (int s = 0; s < ClockStyles; ++s) {
            setClockStyle(s);
            const QImage img = viewport()->grab(QRect(0, 0, kHeaderWidth, kTopH)).toImage();
            int green = 0, left = img.width(), right = 0;
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const QColor c = img.pixelColor(x, y);
                    if (c.green() > 180 && c.red() < 120 && c.blue() < 90) {
                        ++green;
                        left = std::min(left, x);
                        right = std::max(right, x);
                    }
                }
            const double wide = img.width() ? double(right - left) / img.width() : 0;
            all = all && green > 300 && wide > 0.8;
            drawn << QStringLiteral("%1 %2 px %3%").arg(clockStyleName(s)).arg(green).arg(int(wide * 100));
        }
        setClockStyle(style);
        setClockColor(colour);
        check(next && all, QStringLiteral("a click on the timecode changes its style (%1); each fills the corner in neon green (%2)")
                               .arg(next)
                               .arg(drawn.join(QStringLiteral(", "))));
    }
    {
        auto wheel = [&](QPoint pos) {
            QWheelEvent ev(QPointF(pos), mapToGlobal(QPointF(pos)), {}, QPoint(0, 120), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QApplication::sendEvent(viewport(), &ev);
        };
        wheel(QPoint(kHeaderWidth + 100, kRulerTop + kRulerH / 2));
        const double zoomed = m_pxPerSec;
        wheel(QPoint(kHeaderWidth + 100, kTopH + 20));
        check(zoomed > 100 && m_pxPerSec == zoomed, QStringLiteral("wheel zooms over the time ruler only, elsewhere scrolls the tracks"));
        m_viewStart = 0;
        m_pxPerSec = 100;
        updateScrollBars();
    }
    {
        // Up / down on the ruler zooms around the grabbed time (the cursor stays); sideways still scrubs. Dragging empty
        // track space moves the timeline, the grabbed time staying under the mouse.
        const Timeline was = m_project->timeline();
        m_project->setTimelineLive(defaultTimeline());
        m_viewStart = 10 * kUsPerSecond;
        updateScrollBars();
        setCursorTime(2 * kUsPerSecond, false);
        const int rulerY = kRulerTop + kRulerH / 2, x = kHeaderWidth + 200;
        const TimeUs grabbed = xToTime(x);
        drag(QPoint(x, rulerY), QPoint(x, rulerY - 60));
        const double zoomedIn = m_pxPerSec;
        const bool held = std::llabs(xToTime(x) - grabbed) < 20000 && m_cursor == 2 * kUsPerSecond;
        drag(QPoint(x, rulerY), QPoint(x, rulerY + 120));
        const double zoomedOut = m_pxPerSec;
        drag(QPoint(x, rulerY), QPoint(x + 100, rulerY));
        const bool scrubbed = near(m_cursor, xToTime(x + 100));
        m_pxPerSec = 100;
        m_viewStart = 10 * kUsPerSecond;
        updateScrollBars();
        const int emptyY = kTopH + 20;
        const TimeUs under = xToTime(x);
        drag(QPoint(x, emptyY), QPoint(x - 150, emptyY));
        const bool panned = std::llabs(xToTime(x - 150) - under) < 20000 && m_viewStart == 10 * kUsPerSecond + TimeUs(1.5 * kUsPerSecond);
        check(zoomedIn > 100 * 1.5 && held && zoomedOut < zoomedIn / 2 && scrubbed && panned,
              QStringLiteral("dragging up / down on the ruler zooms (%1 -> %2 -> %3 px/s, cursor kept), sideways scrubs (%4); dragging empty "
                             "space moves the timeline (%5)")
                  .arg(100)
                  .arg(zoomedIn, 0, 'f', 0)
                  .arg(zoomedOut, 0, 'f', 0)
                  .arg(scrubbed)
                  .arg(panned));
        m_project->setTimelineLive(was);
        m_viewStart = 0;
        m_pxPerSec = 100;
        setCursorTime(0, false);
        updateScrollBars();
    }

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

    // ---- Track lock ----
    {
        selectA();
        toggleTrackLock(videoTrack);
        const bool dropped = !m_selection.count(idA);
        mouse(QEvent::MouseButtonPress, QPoint(int(timeToX(kUsPerSecond)), clipY));
        mouse(QEvent::MouseButtonRelease, QPoint(int(timeToX(kUsPerSecond)), clipY));
        const bool unclickable = !m_selection.count(idA);
        drag(QPoint(int(timeToX(kUsPerSecond)), clipY), QPoint(int(timeToX(2 * kUsPerSecond)), clipY));
        check(m_project->timeline().tracks[videoTrack].locked && dropped && unclickable && clipAt(videoTrack, 0).start == 0,
              QStringLiteral("a locked track's events drop out of the selection and can't be clicked or dragged"));
        reset();
    }

    // ---- Track colours: captions / text and graphics stand out ----
    {
        Track captions, graphics, mixed; // video tracks
        Clip text, overlay;
        text.generator.type = QStringLiteral("text");
        overlay.generator.type = QStringLiteral("graphics_overlay");
        captions.clips = {text, text};
        graphics.clips = {overlay};
        mixed.clips = {text, overlay};
        check(text.isGenerated() && trackColor(captions) == theme::c().captionClip && trackColor(graphics) == theme::c().graphicsClip
                  && trackColor(mixed) == theme::c().videoClip && clipColor(TrackType::Video, overlay) == theme::c().graphicsClip,
              QStringLiteral("captions and graphics tracks (and their events) have their own colours; a mixed track keeps the video colour"));
    }

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
    {
        // A compound sequence (no file of its own) pastes back with its picture as well as its sound.
        Timeline tl = m_project->timeline();
        const std::vector<int> made = story::compound(tl, ops::expandToGroups(tl, {clipAt(videoTrack, 0).id}), QStringLiteral("Seq"));
        m_project->applyEdit(tl, QStringLiteral("compound"));
        setSelection({made.begin(), made.end()});
        copySelection();
        setCursorTime(m_project->timeline().duration());
        paste();
        int pictures = 0, sounds = 0;
        for (const Track& t : m_project->timeline().tracks)
            for (const Clip& c : t.clips)
                if (c.isSequence())
                    ++(isVisualTrack(t.type) ? pictures : sounds);
        check(made.size() == 2 && pictures == 2 && sounds == 2,
              QStringLiteral("a copied compound sequence pastes its picture and its sound (%1 pictures, %2 sounds)").arg(pictures).arg(sounds));
        reset();
    }

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
        // A box Ctrl+dragged from empty space (below the tracks) up over both events
        // selects them and their grouped audio (a plain drag moves the timeline).
        setSelection({});
        const QPoint emptyStart(int(timeToX(5500000)), trackTop(audioTrack) + trackHeight(m_project->timeline().tracks[size_t(audioTrack)]) + 10);
        const QPoint boxEnd(int(timeToX(kUsPerSecond / 2)), trackTop(videoTrack) + 30);
        const TimeUs viewWas = m_viewStart;
        for (int i = 0; i <= 5; ++i) {
            const QPoint at = emptyStart + (boxEnd - emptyStart) * std::min(i, 4) / 4;
            const QEvent::Type type = i == 0 ? QEvent::MouseButtonPress : i == 5 ? QEvent::MouseButtonRelease : QEvent::MouseMove;
            QMouseEvent ev(type, QPointF(at), mapToGlobal(QPointF(at)), Qt::LeftButton,
                           type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::MouseButtons(Qt::LeftButton), Qt::ControlModifier);
            QApplication::sendEvent(viewport(), &ev);
        }
        check(m_selection.size() == 4 && !hasRegion() && m_viewStart == viewWas,
              QStringLiteral("Ctrl+dragging a box from empty space selects the events it touches, with their grouped audio (%1 selected)").arg(m_selection.size()));
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
        setCursorTime(kUsPerSecond / 2);
        const Timeline beforeHold = m_project->timeline();
        const int holdId = insertFreezeFrame(kUsPerSecond, true);
        const auto& held = m_project->timeline().tracks[size_t(videoTrack)].clips;
        check(holdId && held.size() == 4 && held[1].id == holdId && held[1].start == kUsPerSecond / 2 && held[2].start == 3 * kUsPerSecond / 2
                  && held[2].end() == 2 * kUsPerSecond && held[3].start == 3 * kUsPerSecond
                  && m_project->timeline().tracks[size_t(audioTrack)].clips == beforeHold.tracks[size_t(audioTrack)].clips,
              QStringLiteral("a freeze in place holds over the following second; nothing moves and the sound plays on"));
        reset();
        {
            // A character's expression keys show on the scene's event and drag along it.
            Timeline tl = m_project->timeline();
            Clip scene;
            scene.id = tl.allocId();
            scene.start = 6 * kUsPerSecond;
            scene.length = 4 * kUsPerSecond;
            scene.generator = makeGenerator(*findGeneratorPreset("graphics_blank"));
            scene.generator.data = QJsonObject{{"version", 2}, {"layers", QJsonArray{}},
                                               {"variables", QJsonObject{{"gus_mood", QJsonObject{{"keyframes", QJsonArray{QJsonArray{0, "neutral", "hold"},
                                                                                                                        QJsonArray{2, "angry", "hold"}}}}}}}};
            tl.tracks[size_t(videoTrack)].clips.push_back(scene);
            tl.sortClips();
            m_project->applyEdit(tl, QStringLiteral("scene"));
            m_viewStart = 5 * kUsPerSecond; // (the scene in view)
            updateScrollBars();
            const int y = trackTop(videoTrack) + trackHeight(m_project->timeline().tracks[size_t(videoTrack)]) - 1 - 7;
            const int x = int(timeToX(8 * kUsPerSecond));
            mouse(QEvent::MouseButtonPress, QPoint(x, y));
            mouse(QEvent::MouseMove, QPoint(x + 50, y), Qt::NoButton);
            mouse(QEvent::MouseMove, QPoint(x + 100, y), Qt::NoButton);
            mouse(QEvent::MouseButtonRelease, QPoint(x + 100, y));
            const Clip* moved = m_project->timeline().findClip(scene.id);
            const QJsonArray keys = moved->generator.data["variables"].toObject()["gus_mood"].toObject()["keyframes"].toArray();
            check(moved->start == scene.start && keys.size() == 2 && std::abs(keys[1].toArray()[0].toDouble() - 3) < 0.05 && keys[1].toArray()[1] == "angry",
                  QStringLiteral("an expression key on a scene's event drags along it (angry now at %1 s)").arg(keys[1].toArray()[0].toDouble()));
            reset();
            m_viewStart = 0;
            updateScrollBars();
        }
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
        {
            // J and L cuts: Alt+drag A's picture end (3 s) to 2.5 s: the picture alone (its sound runs on to 3 s, an L cut).
            // Then Ctrl+Alt+Shift+drag the sound's cut to 3.5 s: the sound alone rolls (B's sound starts later, a J cut the
            // other way), the pictures' cut stays at 2.5 s.
            Timeline tl = m_project->timeline();
            for (auto& t : tl.tracks)
                for (auto& c : t.clips)
                    if (c.start == 0)
                        c.length = 3 * S;
            m_project->applyEdit(tl, QStringLiteral("touching"));
            setSelection({});
            modDrag(QPoint(int(std::lround(timeToX(3 * S))) - 2, y), at(2.5), Qt::AltModifier);
            const bool pictureAlone = near(clipAt(videoTrack, 0).end(), 2500000) && clipAt(audioTrack, 0).end() == 3 * S
                                      && clipAt(videoTrack, 0).groupId && clipAt(videoTrack, 0).groupId == clipAt(audioTrack, 0).groupId
                                      && m_project->undoStack()->undoText() == tr("Split Edit");
            setSelection({});
            const int ay = trackTop(audioTrack) + 20;
            modDrag(QPoint(int(std::lround(timeToX(3 * S))) - 2, ay), QPoint(at(3.5).x(), ay),
                    Qt::ControlModifier | Qt::AltModifier | Qt::ShiftModifier);
            const bool soundAlone = near(clipAt(audioTrack, 0).end(), 3500000) && near(clipAt(audioTrack, 1).start, 3500000)
                                    && near(clipAt(videoTrack, 0).end(), 2500000) && clipAt(videoTrack, 1).start == 3 * S
                                    && m_project->undoStack()->undoText() == tr("Split Edit");
            check(pictureAlone && soundAlone,
                  QStringLiteral("J and L cuts: Alt+drag trims a grouped event's picture alone (its sound to %1 s, the picture to %2 s), "
                                 "Ctrl+Alt+Shift+drag rolls the sound's cut alone (to %3 s), still grouped")
                      .arg(secs(clipAt(audioTrack, 0).end())).arg(secs(clipAt(videoTrack, 0).end())).arg(secs(clipAt(audioTrack, 1).start)));
            reset();
        }
        {
            // Ctrl+D: A (and its sound) copied to start where A ends, the copies selected and grouped together; again: the next.
            reset();
            selectA();
            const Clip a = clipAt(videoTrack, 0);
            const size_t before = m_project->timeline().tracks[size_t(videoTrack)].clips.size();
            duplicateSelection();
            const auto& vt = m_project->timeline().tracks[size_t(videoTrack)].clips;
            const auto& at = m_project->timeline().tracks[size_t(audioTrack)].clips;
            auto startingAt = [](const std::vector<Clip>& clips, TimeUs t) {
                return std::find_if(clips.begin(), clips.end(), [&](const Clip& c) { return c.start == t; });
            };
            const auto v = startingAt(vt, a.end()), au = startingAt(at, a.end());
            const bool once = vt.size() == before + 1 && v != vt.end() && au != at.end() && v->length == a.length && v->sourceIn == a.sourceIn
                              && v->groupId && v->groupId == au->groupId && v->groupId != a.groupId && m_selection.count(v->id) && m_selection.count(au->id)
                              && m_project->undoStack()->undoText() == tr("Duplicate");
            duplicateSelection();
            const auto& vt2 = m_project->timeline().tracks[size_t(videoTrack)].clips;
            const bool twice = startingAt(vt2, a.end() + a.length) != vt2.end();
            check(once && twice, QStringLiteral("Ctrl+D duplicates an event (and its sound) to start where it ends, selected; again: the next one"));
            reset();
        }
        {
            // With the snap grid off, faint lines run down the tracks at the ruler's marks, and only there.
            const Grid gridBefore = m_grid;
            setGrid(Grid::Off);
            setRulerLines(false);
            const QImage off = viewport()->grab().toImage();
            setRulerLines(true);
            const QImage on = viewport()->grab().toImage();
            if (!qEnvironmentVariableIsEmpty("MVP_TIMELINE_GRAB")) // (to look at)
                on.save(qEnvironmentVariable("MVP_TIMELINE_GRAB"));
            const RulerSteps steps = rulerSteps();
            std::vector<double> marks;
            for (int64_t n = 0; rulerTick(n, steps.minor, steps.frames) <= xToTime(viewport()->width()) + steps.minor; ++n)
                marks.push_back(timeToX(rulerTick(n, steps.minor, steps.frames)));
            int changed = 0, stray = 0;
            const double k = double(on.width()) / std::max(1, viewport()->width()); // (device pixels)
            for (int y = int(kTopH * k); y < std::min(on.height(), off.height()); y += 3)
                for (int x = int(kHeaderWidth * k); x < std::min(on.width(), off.width()); ++x)
                    if (on.pixel(x, y) != off.pixel(x, y)) {
                        ++changed;
                        const bool onMark = std::any_of(marks.begin(), marks.end(), [&](double m) { return std::abs(m * k - x) <= 1.5 * k; });
                        stray += onMark ? 0 : 1;
                    }
            setGrid(gridBefore);
            check(changed > 20 && stray == 0, QStringLiteral("faint lines on the tracks at the ruler's marks (%1 pixels, %2 elsewhere)").arg(changed).arg(stray));
        }
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
