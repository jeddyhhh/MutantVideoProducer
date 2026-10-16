#pragma once

#include "core/Effects.h"
#include "core/Generators.h"
#include "core/MediaInfo.h"
#include "core/Time.h"

#include <QJsonArray>
#include <QObject>
#include <QString>
#include <QUndoStack>
#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <vector>

enum class TrackType { Video, Audio, Image };

inline bool isVisualTrack(TrackType type) { return type == TrackType::Video || type == TrackType::Image; }
inline bool sameTrackEffectKind(TrackType a, TrackType b) { return isVisualTrack(a) == isVisualTrack(b); }
inline QString trackTypeName(TrackType type)
{
    return type == TrackType::Image ? QStringLiteral("image")
         : type == TrackType::Audio ? QStringLiteral("audio") : QStringLiteral("video");
}

// Automation line ("envelope"): points in timeline time for track
// envelopes, event-local time for an event's velocity envelope.
// Inactive envelopes are hidden and have no effect.
struct Envelope {
    bool active = false;
    double value = 0.0;            // level when there are no points
    std::vector<Keyframe> points;  // sorted by time; Keyframe::interp = curve to the next point

    double at(TimeUs t) const;
    // Area under the line from 0 to t (us x value); negative for t < 0.
    double integral(double t) const;
    // Largest integral(x) for x in [0, t] (>= 0).
    double maxIntegral(double t) const;
    bool operator==(const Envelope&) const = default;
};

enum class EnvelopeKind { Volume, Pan, Composite, Velocity };

// Speed ramp (velocity envelope) values: -100% (backwards) .. 1000%.
constexpr double kMinVelocity = -1.0;
constexpr double kMaxVelocity = 10.0;

// Shape of a fade (or crossfade / transition progress), 0..1 -> 0..1.
enum class FadeCurve { Linear, Smooth, Fast, Slow, ConstantPower };
inline constexpr int kFadeCurves = 5;
double fadeShape(int curve, double x);
QString fadeCurveName(int curve);

struct Timeline;
// Immutable nested content: timeline copies and undo states share it safely.
struct NestedTimeline {
    std::shared_ptr<const Timeline> value;
    bool operator==(const NestedTimeline& other) const;
};

// A media event placed on a track (an "event").
struct Clip {
    int id = 0;
    int mediaId = 0;
    int streamIndex = -1;    // container stream used by this clip
    TimeUs start = 0;        // position on the timeline
    TimeUs length = 0;
    TimeUs sourceIn = 0;     // offset into the media
    TimeUs fadeIn = 0;
    TimeUs fadeOut = 0;
    int fadeInCurve = 0;     // FadeCurve of the fade in (also the crossfade into this event)
    int fadeOutCurve = 0;    // FadeCurve of the fade out (also the crossfade out of it)
    float gain = 1.0f;       // video: opacity, audio: linear volume
    int groupId = 0;         // 0 = not grouped
    bool muted = false;
    QString name;
    EffectChain effects;          // event FX, video or audio (keyframe times are event-local)
    TransitionSpec transitionIn;  // used over the fade-in / overlap with the previous event
    TransitionSpec transitionOut; // used over the fade-out when nothing overlaps
    GeneratorSpec generator;      // generated media (text, solid colour...); mediaId is 0
    ParamMap panCrop;             // Event Pan/Crop (see PanCrop.h); empty = default fit
    double speed = 1.0;           // playback rate (0.05 .. 20)
    bool reverse = false;         // play the source span backwards
    bool preservePitch = true;    // audio: time-stretch instead of tape-style pitch change
    bool frameBlend = false;      // video: blend neighbouring frames (smooth slow motion)
    double stabilize = 0;         // video: shake smoothed over this many seconds (0 = off; see Stabilizer.h)
    bool stabilizeZoom = true;    // zoom in to hide the edges the correction moves into view
    TimeUs loopLength = 0;        // > 0: the file repeats every loopLength (the event runs on past its end)
    Envelope velocity{false, 1.0, {}}; // speed ramp: rate multiplier over event-local time
                                       // (kMinVelocity .. kMaxVelocity), on top of `speed`

    NestedTimeline sequence;
    QJsonObject narration; // original script, alternate takes and caption ownership
    bool isSequence() const { return bool(sequence.value); }

    bool isGenerated() const { return !generator.isEmpty(); }
    bool hasRamp() const { return velocity.active; }
    // Playback rate at event-local time (negative plays backwards).
    double rateAt(TimeUs local) const { return speed * (velocity.active ? velocity.at(local) : 1.0); }
    // Media advanced from the event start to event-local time `local` (us,
    // before reverse; may be negative with a backwards ramp or local < 0).
    double rampOffset(double local) const { return speed * (velocity.active ? velocity.integral(local) : local); }
    // Length of media this event uses.
    TimeUs sourceSpan() const;
    // Media time shown at timeline time t (speed, ramp and direction applied;
    // a looping event's time wrapped back into the file).
    TimeUs sourceTimeAt(TimeUs t) const;
    // Looping: whether the event uses more than one pass through its file.
    bool loops() const { return loopLength > 0 && sourceIn + sourceSpan() > loopLength; }

    TimeUs end() const { return start + length; }
    bool operator==(const Clip&) const = default;
};

struct Track {
    int id = 0;
    TrackType type = TrackType::Video;
    QString name;
    bool muted = false;
    bool solo = false;
    bool locked = false;     // its events can't be selected or edited in the timeline, and ripples pass it by
    float level = 1.0f;      // video: composite opacity, audio: linear volume (0..2)
    float pan = 0.0f;        // audio only, -1..1
    std::vector<Clip> clips; // sorted by start
    EffectChain effects;     // track FX, video or audio (timeline-time keyframes)
    Envelope volume{false, 1.0, {}};    // audio: linear gain multiplier (0 .. 2 = +6 dB)
    Envelope panEnvelope{false, 0.0, {}}; // audio: -1 (left) .. 1 (right), added to the pan slider
    Envelope composite{false, 1.0, {}}; // video: opacity multiplier 0 .. 1
    int blend = 0;           // video: compositing mode over the tracks below (BlendMode)
    // Audio: auto ducking. While the track with id duckUnder (e.g. a voice)
    // has sound, this one (e.g. music) is turned down by duckAmount dB.
    int duckUnder = 0;
    float duckAmount = -12.0f;

    Envelope* envelope(EnvelopeKind k) { return k == EnvelopeKind::Volume ? &volume : (k == EnvelopeKind::Pan ? &panEnvelope : &composite); }
    const Envelope* envelope(EnvelopeKind k) const { return const_cast<Track*>(this)->envelope(k); }
    // Effective values at timeline time t (track control x envelope).
    double gainAt(TimeUs t) const { return double(level) * (volume.active ? std::max(0.0, volume.at(t)) : 1.0); }
    double panAt(TimeUs t) const { return std::clamp(double(panEnvelope.active ? panEnvelope.at(t) : 0.0) + double(pan), -1.0, 1.0); }
    double compositeAt(TimeUs t) const { return double(level) * (composite.active ? std::clamp(composite.at(t), 0.0, 1.0) : 1.0); }

    bool operator==(const Track&) const = default;
};

// A Live tab pad: an effect chain switched on and off while performing,
// applied after the Live FX chain.
struct LivePad {
    QString name;
    EffectChain effects;
    bool locked = false; // no edits (effects, name, clearing) until unlocked
    bool momentary = false; // on only while held (mouse, key or MIDI), on top of the active pad
    bool isEmpty() const { return name.isEmpty() && effects.empty() && !locked && !momentary; }
    bool operator==(const LivePad&) const = default;
};
inline constexpr int kLivePads = 16; // a 4x4 grid
using LivePads = std::array<LivePad, kLivePads>;
// Pads as JSON (project files and saved pad sets).
QJsonArray livePadsToJson(const LivePads& pads);
LivePads livePadsFromJson(const QJsonArray& a);

// How the Live tab's second source (the layer) is laid over the main one.
struct LiveLayerSettings {
    enum class Mode { PictureInPicture, Full };
    Mode mode = Mode::PictureInPicture;
    int corner = 3;       // picture in picture: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right, 4 centre
    double size = 0.3;    // picture in picture: its width, as a share of the picture's
    double opacity = 1.0; // 0..1
    int blend = 0;        // BlendMode
    bool operator==(const LiveLayerSettings&) const = default;
};

// Something drawn on top of the Live output: text, a picture (a logo), the
// tempo, the time, or the audio input as spectrum bars, a waveform or a ring.
struct LiveOverlay {
    enum class Kind { Text, Image, Bpm, Clock, Spectrum, Waveform, Circle };
    Kind kind = Kind::Text;
    bool enabled = true;
    QString text;          // Text
    QString image;         // Image: file path
    int anchor = 2;        // 0..8: top-left, top, top-right, left, centre, right, bottom-left, bottom, bottom-right
    double size = 8;       // % of the picture height (text height, or the picture height)
    double width = 60;     // spectrum / waveform: % of the picture width
    int color = 0xffffff;  // 0xRRGGBB (text)
    double opacity = 1;    // 0..1
    bool pulse = false;    // grows a little on each beat
    bool operator==(const LiveOverlay&) const = default;
};
QJsonArray liveOverlaysToJson(const std::vector<LiveOverlay>& overlays);
std::vector<LiveOverlay> liveOverlaysFromJson(const QJsonArray& a);

// A Live tab scene: the whole look at one moment (Live FX, the active pad, the
// layer and its FX, the overlays), recalled with one press (F1-F8, MIDI).
struct LiveScene {
    QString name;
    EffectChain liveEffects;
    int pad = -1;
    QString layerSource; // the layer's source by name (empty: no layer)
    LiveLayerSettings layer;
    EffectChain layerEffects;
    std::vector<LiveOverlay> overlays;
    bool used = false;
    bool operator==(const LiveScene&) const = default;
};
inline constexpr int kLiveScenes = 8;
using LiveScenes = std::array<LiveScene, kLiveScenes>;
QJsonArray liveScenesToJson(const LiveScenes& scenes);
LiveScenes liveScenesFromJson(const QJsonArray& a);

// A Live tab setlist entry: what a song needs, recalled with one press
// (Page Down / Page Up, MIDI): its pad set, its tempo (locked, or listened
// for afresh) and a scene.
struct LiveSong {
    QString name;
    QString padSet;    // a saved pad set to load (empty: keep the pads)
    double bpm = 0;    // 0: find the tempo by listening
    bool lockBpm = true; // lock the tempo at bpm (or start from it and keep listening)
    int scene = -1;    // a scene to recall (0..7), -1: none
    QString notes;     // a reminder shown while it plays
    bool operator==(const LiveSong&) const = default;
};
QJsonArray liveSetlistToJson(const std::vector<LiveSong>& songs);
std::vector<LiveSong> liveSetlistFromJson(const QJsonArray& a);

// A timeline marker (M) or region (R): a named point or span, e.g. a beat
// to cut on or a section to render.
struct Marker {
    int id = 0;
    TimeUs time = 0;
    TimeUs length = 0; // > 0: a region
    QString name;      // empty: shown by its number
    int color = 0;     // index into the marker colours
    bool isRegion() const { return length > 0; }
    TimeUs end() const { return time + length; }
    bool operator==(const Marker&) const = default;
};
inline constexpr int kMarkerColors = 8;

struct Timeline {
    QJsonArray chapters;
    QJsonObject narrationSettings;
    std::vector<Track> tracks;
    std::vector<Marker> markers; // sorted by time
    EffectChain outputEffects; // video output FX, applied to the final picture
    EffectChain audioMasterEffects; // audio master bus FX
    EffectChain liveEffects; // Live tab FX, applied to the captured picture
    LivePads livePads; // Live tab pads, applied after liveEffects
    EffectChain liveLayerEffects; // Live tab: FX on the second source (the layer) before it is laid over
    std::vector<LiveOverlay> liveOverlays; // Live tab: drawn over the output
    LiveScenes liveScenes; // Live tab: saved looks
    std::vector<LiveSong> liveSetlist; // Live tab: the songs of a set, in order
    int nextId = 1;

    int allocId() { return nextId++; }
    TimeUs duration() const;
    TimeUs soundEnd() const; // the end of the last event on an audio track (0: none)
    Clip* findClip(int clipId, int* trackIndex = nullptr);
    const Clip* findClip(int clipId, int* trackIndex = nullptr) const;
    Marker* findMarker(int markerId);
    const Marker* findMarker(int markerId) const;
    int trackIndex(int trackId) const; // -1 if missing
    void sortClips();
    void sortMarkers();

    bool operator==(const Timeline&) const = default;
};

struct ProjectSettings {
    int width = 1920;
    int height = 1080;
    FrameRate fps{30, 1};
    int sampleRate = 48000;

    bool operator==(const ProjectSettings&) const = default;
};

struct MediaItem {
    int id = 0;
    QString path;
    MediaInfo info;
    int revision = 0; // bumped when relinked: decoders of the old file are not reused
    // Organising in Project Media (saved with the project): a folder ("Footage/Day 1"),
    // a rating (0..5 stars) and the part to use when placed (markOut 0: to the end).
    QString folder;
    QString origin; // "youtube": downloaded in Mutant Video Producer; independent of folder organisation
    int rating = 0;
    TimeUs markIn = 0, markOut = 0;
    QString displayName() const;
};

using MediaMap = std::map<int, std::shared_ptr<const MediaItem>>;

// Immutable copy of everything a render/mix thread needs.
struct ProjectSnapshot {
    // Runtime identity survives edits, but changes on load/new (never serialized).
    std::shared_ptr<const int> session;
    ProjectSettings settings;
    MediaMap media;
    Timeline timeline;
};

// Fade envelope helpers shared by the compositor, mixer and timeline view.
// Overlapping clips on the same track crossfade automatically.
struct ClipEnvelope {
    TimeUs fadeIn = 0;
    TimeUs fadeOut = 0;
};
ClipEnvelope effectiveEnvelope(const Track& track, size_t clipIndex);
double envelopeGain(const Clip& clip, const ClipEnvelope& env, TimeUs t);

struct PackedTimeline; // the undo history's form of a timeline (Project.cpp)

class Project : public QObject {
    Q_OBJECT
public:
    explicit Project(QObject* parent = nullptr);

    const ProjectSettings& settings() const { return m_settings; }
    void setSettings(const ProjectSettings& s);

    // An embedded editor shares immutable media references; new imports keep their own ids.
    void shareMediaPool(const MediaMap& media);
    const MediaMap& media() const { return m_media; }
    std::shared_ptr<const MediaItem> mediaItem(int id) const;
    // Adds probed media to the pool; returns its id.
    int addMedia(const MediaInfo& info);
    bool removeMedia(int id);   // fails if used on the timeline
    // Project Media's folder, rating and in / out marks of a file (see MediaItem).
    void setMediaDetails(int id, const QString& folder, int rating, TimeUs markIn, TimeUs markOut, const QString& origin = {});
    bool isMediaUsed(int id) const;
    // Media whose file can't be opened (moved, renamed, deleted).
    std::vector<int> offlineMedia() const;
    // Points media at another file; its events keep their places. Events
    // using a stream the new file lacks switch to its first stream of that
    // kind (one undo step). Fails if the file can't be opened.
    bool relinkMedia(int id, const QString& path, QString* error = nullptr);

    const Timeline& timeline() const { return m_timeline; }
    // Live update without an undo entry (used while dragging).
    void setTimelineLive(const Timeline& t);
    // Records an undo step from `before` to the current timeline.
    void commitEdit(const Timeline& before, const QString& description);
    // Convenience: apply `after` as a single undoable edit.
    void applyEdit(const Timeline& after, const QString& description);

    QUndoStack* undoStack() { return &m_undo; }

    std::shared_ptr<const ProjectSnapshot> snapshot() const;

    const QString& filePath() const { return m_filePath; }
    bool isModified() const { return !m_undo.isClean() || m_dirtyExtra; }
    void clear(const ProjectSettings& settings = {}); // an empty, unmodified project of that size and rate
    bool save(const QString& path, QString* error);
    QJsonObject collectionDocument() const;
    bool load(const QString& path, QString* error, QStringList* missingMedia = nullptr);
    // Autosave: writes a copy that remembers the project's own file, without
    // changing the file name or the saved state.
    bool saveBackup(const QString& path, QString* error) const;
    // Opens an autosaved copy: the project gets its original file name back
    // (if it had one) and counts as modified.
    bool recover(const QString& backupPath, QString* error, QStringList* missingMedia = nullptr);

    // Called by the undo command.
    void restoreTimeline(const Timeline& t);
    void restorePacked(const std::shared_ptr<const PackedTimeline>& t);

signals:
    void changed();        // timeline or settings changed (repaint, re-render)
    void mediaChanged();   // media pool changed
    void mediaRelinked(int id);
    void settingsChanged();

private:
    void touch();
    QByteArray toJson(const QString& baseDir, const QString& backupOf, bool backup) const;
    bool loadFile(const QString& path, QString* error, QStringList* missingMedia, QString* backupOf);

    std::shared_ptr<const int> m_session = std::make_shared<const int>(0);
    ProjectSettings m_settings;
    MediaMap m_media;
    int m_nextMediaId = 1;
    Timeline m_timeline;
    QUndoStack m_undo;
    QString m_filePath;
    bool m_dirtyExtra = false;
    bool m_portablePaths = false;
    mutable std::shared_ptr<const ProjectSnapshot> m_snapshot;
    std::shared_ptr<const PackedTimeline> m_packed; // the last state put in the undo history (to share with)
};

// Creates a default empty timeline: one video track and one audio track.
Timeline defaultTimeline();

// What the rest of the program counts on in a timeline (events with a
// length, in order, ids unique and below nextId...): empty when sound,
// else what is wrong. For tests and checks.
QString timelineProblem(const Timeline& tl);

// Nested sequence serialization (same track/clip schema as a project).
QJsonObject nestedTimelineToJson(const Timeline& timeline);
Timeline nestedTimelineFromJson(const QJsonObject& object);

// Reuse an immutable child snapshot between frames/audio blocks.
struct SequenceSnapshotCache {
    std::shared_ptr<const Timeline> content;
    ProjectSnapshot snapshot;
    const ProjectSnapshot& get(const ProjectSnapshot& parent,const NestedTimeline& sequence) {
        if(content!=sequence.value) { content=sequence.value; snapshot.timeline=*content; }
        snapshot.session = parent.session;
        if(snapshot.settings!=parent.settings)snapshot.settings=parent.settings;
        if(snapshot.media!=parent.media)snapshot.media=parent.media;
        return snapshot;
    }
};
