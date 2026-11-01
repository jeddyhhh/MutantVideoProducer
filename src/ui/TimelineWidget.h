#pragma once

#include "core/Project.h"
#include "core/TimelineOps.h"

#include <QAbstractScrollArea>
#include <QRegion>
#include <map>
#include <optional>
#include <set>
#include <vector>

class MediaCache;

// Track view: track headers on the left, a loop-region bar and
// time ruler on top, events (clips) laid out on tracks below.
//
// Mouse: drag events to move (across tracks too), drag edges to trim, drag
// the top corners for fades, drag the line under the title for gain, drag in
// empty space to make a time selection, wheel to zoom, Ctrl+wheel to scroll.
class TimelineWidget : public QAbstractScrollArea {
    Q_OBJECT
public:
    TimelineWidget(Project* project, MediaCache* cache, QWidget* parent = nullptr);

    TimeUs cursorTime() const { return m_cursor; }
    void setCursorTime(TimeUs t, bool ensureVisible = true);
    // Playback position marker; nullopt hides it.
    void setPlayhead(std::optional<TimeUs> t);
    std::optional<TimeUs> playhead() const { return m_playhead; }

    bool hasRegion() const { return m_regionEnd > m_regionStart; }
    TimeUs regionStart() const { return m_regionStart; }
    TimeUs regionEnd() const { return m_regionEnd; }
    void setRegion(TimeUs a, TimeUs b);

    const std::set<int>& selection() const { return m_selection; }
    void selectClips(const std::set<int>& ids) { setSelection(ids); }
    bool snapping() const { return m_snapping; }
    // The timecode in the top-left corner: its style (a click there moves to the next) and colour (right-click; invalid
    // is the theme's). Both are kept in the settings.
    enum ClockStyle { ClockStandard, ClockSegment, ClockDotMatrix, ClockMono, ClockTypewriter, ClockCondensed, ClockStyles };
    int clockStyle() const { return m_clockStyle; }
    void setClockStyle(int style);
    void setClockColor(const QColor& color);
    static QString clockStyleName(int style);
    void setSnapping(bool on) { m_snapping = on; }
    // Automatic crossfades: events overlapping on a track crossfade;
    // off, an event moved, trimmed or placed over another cuts it instead.
    void setAutoCrossfade(bool on) { m_autoCrossfade = on; }
    bool autoCrossfade() const { return m_autoCrossfade; }
    // Grid lines events snap to: off, frames, half seconds, seconds, beats.
    enum class Grid { Off, Frames, HalfSeconds, Seconds, Beats };
    void setGrid(Grid grid);
    // Faint lines down the tracks at the ruler's marks (labelled ones a little stronger), while the snap grid is off.
    void setRulerLines(bool on);
    // Pre-rendering's heavy stretches, under the ruler: green done, orange not yet (start, end, done).
    void setPreRenderSpans(std::vector<std::tuple<TimeUs, TimeUs, bool>> spans);
    // A colour label (0: none) on the selected events and their groups: one undo step.
    void setLabelOnSelection(int label);
    bool rulerLines() const { return m_rulerLines; }
    Grid grid() const { return m_grid; }
    // The beat grid's tempo and its first beat (Detect Beats sets them).
    void setGridTempo(double bpm, TimeUs firstBeat);
    double gridBpm() const { return m_gridBpm; }
    TimeUs gridOffset() const { return m_gridOffset; }

    // Auto ripple: edits push or pull what comes after them. Tracks = only
    // the tracks being edited; All = every track, markers and regions.
    enum class RippleMode { Off, Tracks, All };
    RippleMode rippleMode() const { return m_ripple; }
    void setRippleMode(RippleMode mode) { m_ripple = mode; }

    // Markers (M) and regions (R) in the marker bar above the ruler.
    int addMarker(TimeUs t, const QString& name = {});
    int addRegion(TimeUs a, TimeUs b, const QString& name = {});
    bool addRegionFromSelection(); // the loop region becomes a named region
    void renameMarker(int markerId);
    void removeMarker(int markerId);
    void jumpToMarker(int direction);
    // I / O: start and end of the loop region (time selection).
    void setInPoint(TimeUs t);
    void setOutPoint(TimeUs t);
    // Tracks.
    void moveTrack(int from, int to);
    void toggleTrackLock(int trackIndex);
    void setTrackBlend(int trackIndex, int mode);
    void setTrackMatte(int trackIndex, int mode);
    // Noise reduction (0 off .. 1 strong) on the selected video events.
    void setDenoise(double amount);
    // The Compositing Mode and Track Matte choices for a video track (the header's Mode box and its right-click menu).
    void addCompositingMenus(QMenu* menu, int trackIndex);
    // Transitions: removes the one at an event's start / end (a hard cut),
    // or at both ends of every selected video event.
    void removeTransitionAt(int clipId, bool atStart);
    // Fade shapes (FadeCurve) of the selected events' starts / ends.
    void setFadeCurveOnSelection(bool fadeIn, int curve);
    // Audio meters in the track headers (peaks by track id, linear).
    void setTrackMeters(const std::map<int, std::pair<float, float>>& peaks);
    // Auto ducking of an audio track under another (0: off).
    void setTrackDuck(int trackIndex, int underTrackId, float amountDb);
    // Keyboard editing: moves the selected events by whole frames; trims the
    // start / end of the selected events under the cursor to the cursor.
    void nudgeSelection(int frames);
    void trimSelectionToCursor(bool start);
    // Freeze frame: holds the picture at the cursor for `length` on the
    // selected video event (or the top one under the cursor), pushing the
    // rest of it (and its grouped audio) later. Returns the new event's id.
    // inPlace: the held frame covers the time after the cursor (the event carries on after it, nothing moves later).
    int insertFreezeFrame(TimeUs length, bool inPlace = false);
    void removeTransitionsFromSelection();
    int selectedTrack() const { return m_selectedTrack; }

    // Editing commands (shortcuts are bound in MainWindow).
    void splitAtCursor();
    void deleteSelection(bool ripple);
    void selectAll();
    void groupSelection();
    void ungroupSelection();
    void copySelection();
    void cutSelection();
    void paste();
    // Ctrl+D: a copy of the selected events (and their groups) right after them, starting where they end. Selected.
    void duplicateSelection();
    void toggleMuteSelection();
    void insertTrack(TrackType type);
    // Timeline drops give each video its own tracks; cursor placement can reuse tracks.
    void insertMedia(const std::vector<int>& mediaIds, TimeUs at, int trackIndex, bool newVideoTracks = false);
    void insertMediaAtCursor(int mediaId);
    // Adds an effect to the selected video events; returns the first event's id (0 if none).
    int addEffectToSelection(const QString& effectType);
    // Adds a preset's devices to the selected events of its kind; returns the first event's id (0 if none).
    int addPresetToSelection(const QString& presetId);
    // Sets a transition on the selected video events' starts; returns the first event's id.
    int setTransitionOnSelection(const QString& transitionType);
    // First selected event on a video track (0 if none).
    int firstSelectedVideoClip() const;
    int firstSelectedClip() const;
    // Audio: sets each selected audio event's level so its peak is -0.1 dBFS.
    void normalizeSelection();
    // Inserts generated media (text, solid colour...) from a preset; returns the clip id.
    int insertGenerator(const QString& presetId, TimeUs at, int trackIndex);
    int insertGeneratorAtCursor(const QString& presetId);
    // Playback speed of the selected events (keeps their footage, changes length).
    void setSpeedOnSelection(double speed);
    void toggleReverseOnSelection();
    // Shows/hides an envelope on the selected track (V = volume or composite
    // level for video tracks, P = pan).
    void toggleEnvelope(EnvelopeKind kind);
    // Speed ramps on the selected events (grouped video and audio stay in sync):
    // shows a flat 100% velocity envelope, or removes the ramp if there is one.
    void toggleVelocityEnvelope();
    void applyRampPresetToSelection(ops::RampPreset preset);

    // Developer check (MutantVideoProducer --devices-test): media drops with synthetic events.
    int runDropTest(QStringList& log);
    // Developer check (MutantVideoProducer --editor-test): markers, ripple edits, track
    // reordering and blend modes with synthetic mouse events.
    int runEditTest(QStringList& log);

    void zoomIn();
    void zoomOut();
    void zoomToFit();
    void stepFrames(int frames);
    void jumpToEdit(int direction);
    void goToStart();
    void goToEnd();

signals:
    void cursorMoved(TimeUs t);
    void cursorSet(TimeUs t); // the cursor put somewhere by the program (setCursorTime: a seek, a marker...), to follow it
    void beatsRequested(); // right-click > Detect Beats
    void stabilizeRequested(int clipId);
    void matchColourRequested();
    void autoColourRequested();
    void spectrogramRequested(int clipId);
    void selectionChanged();
    void filesDropped(const QStringList& paths, TimeUs at, int trackIndex);
    void clipEffectsRequested(int clipId);
    void trackEffectsRequested(int trackId);
    void trackSelected(int trackId); // a click on the track header (outside its controls)
    void transitionRequested(int clipId, bool in);
    void generatorRequested(int clipId);
    void sequenceRequested(int clipId);
    void panCropRequested(int clipId);
    void propertiesRequested(int clipId);
    void relinkRequested(int mediaId);
    // The cursor was dragged on the ruler or stepped by a frame (for audio scrubbing).
    void scrubbed(TimeUs t);
    void message(const QString& text); // for the status bar
    void angleKey(int angle);          // 1-9 pressed on the timeline (a multicam angle)

protected:
    void keyPressEvent(QKeyEvent*) override;
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void scrollContentsBy(int dx, int dy) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void dragEnterEvent(QDragEnterEvent*) override;
    void dragMoveEvent(QDragMoveEvent*) override;
    void dragLeaveEvent(QDragLeaveEvent*) override;
    void dropEvent(QDropEvent*) override;

private:
    enum class Zone { None, Body, LeftEdge, RightEdge, FadeIn, FadeOut, Gain, FxBadge, PanCropBadge, TransitionLabel, TransitionRemove };
    struct Hit {
        int clipId = 0;
        int track = -1;
        Zone zone = Zone::None;
        bool transitionIn = true; // for the transition zones: which end
    };
    enum class Drag {
        None, Move, TrimLeft, TrimRight, FadeIn, FadeOut, Gain, Scrub, TimeSelect,
        RegionNew, RegionStart, RegionEnd, RegionMove, TrackLevel, TrackPan, TrackResize,
        EnvelopePoint, EnvelopeSegment, MarkerMove, MarkerStart, MarkerEnd, TrackReorder, RubberBand,
        Slip, Slide, Roll, MoodMove, RulerZoom, Pan
    };
    // A character's expression key in an animation scene, shown on the event (and dragged along it).
    struct MoodMark {
        QString variable; // <character>_mood
        int index = 0;    // its key
        double time = 0;  // event-local seconds
        QString mood;
        QRectF rect;
    };
    std::vector<MoodMark> moodMarks(const Clip& clip, const QRectF& body) const;
    int m_moodClip = 0, m_moodIndex = 0;
    QString m_moodVariable;
    double m_moodPressTime = 0;
    // Ctrl+drag: copies of the selection are moved (the originals stay).
    bool m_copyDrag = false;
    bool m_splitEdit = false; // trimming or rolling this event alone, not its group (a J or L cut)
    std::set<int> m_copyIds;
    // Ctrl+click on an event that was selected: taken out of the selection if not dragged.
    bool m_ctrlDeselect = false;
    // Roll (Ctrl+Alt+drag the cut between two touching events): the events before and after it.
    int m_rollLeft = 0, m_rollRight = 0;
    void dragRoll(QPoint p, Qt::KeyboardModifiers mods);
    // Grid.
    TimeUs gridStep() const;
    void paintGrid(QPainter& p, const QRect& band);
    // The ruler's marks at this zoom: labelled ones every `major`, small ones every `minor`; frame-exact below a second.
    struct RulerSteps {
        TimeUs major, minor;
        bool frames;
    };
    RulerSteps rulerSteps() const;
    TimeUs rulerTick(int64_t n, TimeUs step, bool frames) const;
    bool m_rulerLines = true;
    std::vector<std::tuple<TimeUs, TimeUs, bool>> m_preRenderSpans;
    Grid m_grid = Grid::Off;
    double m_gridBpm = 120;
    TimeUs m_gridOffset = 0;
    bool m_autoCrossfade = true;

    // Geometry
    double timeToX(TimeUs t) const;
    TimeUs xToTime(double x) const;
    int trackHeight(const Track& t) const;
    int trackTop(int index) const;
    int trackAt(int y) const;
    int tracksHeight() const;
    QRect headerRect(int index) const;
    TimeUs visibleDuration() const;
    TimeUs quantize(TimeUs t) const;
    Hit hitTest(QPoint p) const;

    // Snapping
    void buildSnapPoints(const std::set<int>& exclude, int excludeMarker = 0);
    std::optional<TimeUs> snapTo(TimeUs t) const;
    TimeUs snapped(TimeUs t, Qt::KeyboardModifiers mods) const;
    // Where dropped media lands: the first clip goes to the start of the
    // timeline; later ones join the end of the last clip on the track (or
    // the timeline) when dropped within a few seconds of it. Shift: as dropped.
    TimeUs mediaDropTime(int x, int track, Qt::KeyboardModifiers mods) const;

    // Drags
    void dragMove(QPoint p, Qt::KeyboardModifiers mods);
    // Alt+drag: the footage inside the selected events moves (they stay put).
    void dragSlip(QPoint p);
    // Ctrl+Alt+drag: the event moves between its neighbours, which trim to fill the gap.
    void dragSlide(QPoint p, Qt::KeyboardModifiers mods);
    void dragTrim(QPoint p, bool left, Qt::KeyboardModifiers mods);
    void dragFade(QPoint p, bool fadeIn);
    void dragGain(QPoint p);
    void headerPress(QPoint p);
    void applyTrackLevel(QPoint p);
    void dropEffect(QPoint p, const QString& type);
    void dropPreset(QPoint p, const QString& presetId);
    void dropTransition(QPoint p, const QString& type);
    // Sets a transition at one end of a clip, creating a 1 s fade if needed.
    // Returns the clip that holds it (the next clip when ends overlap) and whether it is the "in" side.
    std::pair<int, bool> setTransition(Timeline& tl, int clipId, bool atStart, const QString& type);
    QRectF fxBadgeRect(const Clip& c, int trackTop) const;
    // The label naming the transition at one end of event ci of a video track
    // (empty if there is none or it is too narrow); its × removes it.
    QRectF transitionLabelRect(const Track& track, size_t ci, int top, int height, bool atStart) const;

    // Envelopes
    struct EnvHit {
        int track = -1;
        EnvelopeKind kind = EnvelopeKind::Volume;
        int point = -1;   // index of the point under the mouse, or -1 for the line
        int clipId = 0;   // set for an event's velocity envelope (speed ramp)
    };
    // Where an envelope is drawn: its time origin (event start for velocity),
    // vertical band and the time range its points may use (origin-relative).
    struct EnvBand {
        TimeUs origin = 0;
        int top = 0;
        int h = 0;
        TimeUs maxT = 0;
    };
    static const Envelope* findEnvelope(const Timeline& tl, const EnvHit& h);
    static Envelope* findEnvelope(Timeline& tl, const EnvHit& h);
    EnvBand envBand(const Timeline& tl, const EnvHit& h) const;
    EnvHit envelopeHit(QPoint p) const;
    double envValueToY(EnvelopeKind k, double v, int top, int h) const;
    double envYToValue(EnvelopeKind k, double y, int top, int h) const;
    QString envValueText(EnvelopeKind k, double v) const;
    void paintEnvelopes(QPainter& p, const Track& track, int top, int h);
    void dragEnvelope(QPoint p, Qt::KeyboardModifiers mods);
    void envelopeMenu(QMenu& menu, const EnvHit& hit);
    void updateHoverCursor(QPoint p);

    // Markers: the one under p in the marker bar (0 if none); part 0 = the
    // marker / region body, 1 = region start edge, 2 = region end edge.
    int markerAt(QPoint p, int* part = nullptr) const;
    QString markerLabel(const Marker& m) const;
    void paintMarkers(QPainter& p);
    void markerMenu(QMenu& menu, QPoint p);
    void dragMarker(QPoint p, Qt::KeyboardModifiers mods);

    // Ripple: the ids of the tracks an edit of `clips` moves ({} = all tracks).
    std::set<int> rippleTracks(const Timeline& tl, const std::set<int>& clips) const;
    // Makes room for events just placed at `at` (ids `created`) when rippling.
    void rippleAfterInsert(Timeline& tl, TimeUs at, const std::set<int>& created) const;
    int reorderGap(int y) const;

    // Painting
    void paintTrackClips(QPainter& p, const Track& track, int top, int height);
    void paintThumbnails(QPainter& p, const Clip& clip, const QRectF& body);
    void paintWaveform(QPainter& p, const Clip& clip, const QRectF& body);
    void paintGenerated(QPainter& p, const Clip& clip, const QRectF& body);
    void paintMoods(QPainter& p, const Clip& clip, const QRectF& body);
    void paintRuler(QPainter& p);
    void paintClock(QPainter& p, const QRectF& box, const QString& text);
    void paintHeaders(QPainter& p);

    void updateScrollBars();
    void ensureTimeVisible(TimeUs t, bool page);
    void zoomAround(double x, double factor);
    void pruneSelection();
    void setSelection(std::set<int> sel);

    friend class MainWindow; // editor regression checks exercise transient timeline state
    friend class McpServer;  // AI control sets transitions like the event menu
    std::shared_ptr<const int> m_session;
    Project* m_project;
    MediaCache* m_cache;

    TimeUs m_viewStart = 0;
    QRegion m_paintArea; // what this repaint covers (clips and headers outside it are not drawn)
    double m_pxPerSec = 60.0;
    int m_scrollY = 0;
    TimeUs m_cursor = 0;
    int m_clockStyle = ClockStandard;
    QColor m_clockColor;
    std::optional<TimeUs> m_playhead;
    TimeUs m_regionStart = 0;
    TimeUs m_regionEnd = 0;
    std::set<int> m_selection;
    int m_selectedTrack = 0;
    std::map<int, int> m_trackHeights; // by track id
    std::map<int, std::pair<float, float>> m_meters; // shown audio levels by track id (decaying)
    bool m_snapping = true;
    RippleMode m_ripple = RippleMode::Off;

    Drag m_drag = Drag::None;
    QPoint m_pressPos;
    TimeUs m_pressTime = 0;
    Timeline m_before;
    int m_dragClip = 0;
    int m_pressTrack = -1;
    int m_resizeStartHeight = 0;
    TimeUs m_regionPressStart = 0;
    TimeUs m_regionPressEnd = 0;
    bool m_dragMoved = false;
    int m_dragMarker = 0;
    TimeUs m_markerPressTime = 0;
    TimeUs m_markerPressLength = 0;
    int m_reorderGap = -1; // where a dragged track header would go (-1: nowhere new)
    // On the ruler: where the cursor was before the press (a drag up or down zooms instead, and puts it back), and the
    // last y of a zoom drag. Dragging empty space: the time held under the mouse.
    bool m_rulerPress = false;
    TimeUs m_scrubFrom = 0;
    int m_zoomY = 0;
    TimeUs m_panTime = 0;
    // Rubber-band selection: where it started (time, content y) and the mouse now.
    TimeUs m_bandTime = 0;
    int m_bandY = 0;
    QPoint m_bandNow;
    std::set<int> m_bandBase; // selection kept (Ctrl+drag adds to it)
    EnvHit m_envDrag;
    TimeUs m_envPressTime = 0;
    std::vector<TimeUs> m_snapPoints;

    std::optional<int> m_dropX;
    // Effect being dragged over the timeline: the event it would go on, or the
    // track (header or empty lane) whose Track FX it would join.
    int m_fxDropClip = 0;
    int m_fxDropTrack = -1;
    std::vector<std::pair<int, Clip>> m_clipboard; // track index, clip (start relative)
    struct GeneratedThumb {
        GeneratorSpec spec;
        TimeUs length = 0;
        QSize size;
        QImage image;
    };
    std::map<int, GeneratedThumb> m_generatedThumbs; // by clip id
};
