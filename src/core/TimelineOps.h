#pragma once

#include "core/Project.h"

#include <set>
#include <vector>

// Pure editing operations on a Timeline value. The UI applies them to a copy
// and commits the result as one undo step.
namespace ops {

// Longest a clip using `media` may be when starting at `sourceIn` and
// playing at `speed`. Still images are unbounded.
TimeUs maxClipLength(const MediaItem* media, TimeUs sourceIn, double speed = 1.0);

// Speed limits for events.
constexpr double kMinSpeed = 0.05;
constexpr double kMaxSpeed = 20.0;

// Longest `c` may be with its current start in the media, speed and ramp.
TimeUs maxClipLength(const MediaItem* media, const Clip& c);

// Changes an event's speed. With keepSpan the same footage is kept and the
// length changes (a speed ramp stretches with it); otherwise the length stays
// (clamped to available media).
void setClipSpeed(Clip& c, const MediaItem* media, double speed, bool keepSpan, const FrameRate& fps);

// Trimming: moves the start (or end) edge by d (positive = later) and keeps
// the media under the rest of the event in place (speed, ramp and reverse
// aware). trimRange is the allowed range of d that stays inside the media.
void trimClipStart(Clip& c, TimeUs d);
// Keeps an event's fades inside it (the fade in first, the fade out in what is left).
void clampFades(Clip& c);
void trimClipEnd(Clip& c, TimeUs d);
std::pair<TimeUs, TimeUs> trimRange(const Clip& c, const MediaItem* media, bool startEdge);

// --- Speed ramps (velocity envelopes) ---
// Largest event-local time x in [lo, hi] whose media offset is <= `offset`
// (binary search; the offset grows while the ramp plays forwards).
TimeUs rampTimeForOffset(const Clip& c, double offset, TimeUs lo, TimeUs hi);
// Stretches the velocity envelope in time by `factor` (around the event start).
void scaleRampTime(Clip& c, double factor);
// Drops velocity points that can no longer affect the event (outside
// [0, length] beyond the nearest neighbour on each side).
void trimRampPoints(Clip& c);
// Ready-made ramps. They keep the event's footage: the length changes.
enum class RampPreset { SpeedUp, SlowDown, SlowMiddle, FastMiddle, FreezeMiddle };
QString rampPresetName(RampPreset p);
void applyRampPreset(Clip& c, const MediaItem* media, RampPreset preset, const FrameRate& fps);
// Copies `source`'s speed ramp to the other events in its group that cover
// the same time (so grouped video and audio stay in sync).
void syncGroupRamps(Timeline& tl, int sourceClipId);

// Places media at `at`: its video on a video track and its audio on an
// audio track (grouped together). `preferredTrack` is the track the user
// dropped onto, or -1. With newTracks, creates an adjacent video/audio pair on
// top named ClipN-Video / ClipN-Audio instead of reusing tracks. Returns new clip ids.
// Still images use Image tracks; a new Image track is made for each dropped image.
std::vector<int> insertMedia(Timeline& tl, const MediaItem& media, TimeUs at, int preferredTrack, const FrameRate& fps,
                             bool newTracks = false);

// Splits clips at time t. With no ids, splits every clip under t.
// Returns the ids of the new right-hand pieces.
std::vector<int> splitClips(Timeline& tl, const std::set<int>& ids, TimeUs t);

// Places a generated event (text, solid colour...) from a preset at `at` on
// `preferredTrack` if it's a video track, else the first video track
// (created if missing). Where that would put it wholly inside another event
// (a crossfade over all of it), it goes on the nearest video track above
// that is free there, or a new one, so it shows over the picture.
// Returns the new clip id.
int insertGenerated(Timeline& tl, const GeneratorPreset& preset, TimeUs at, int preferredTrack, const FrameRate& fps);

// Moves an event's keyframes (effects, generator and velocity envelope) by delta.
void shiftClipKeyframes(Clip& c, TimeUs delta);

// Paste Event Attributes: parts of event `from` given to the events `to` (each of the same kind: picture or sound;
// Pan/Crop, transitions and the picture fixes only between pictures). Effects replace the event's own (with new ids);
// a playback rate keeps each event's length (as much as its file allows). Returns how many events changed.
enum EventAttribute : unsigned {
    AttrEffects = 1, AttrPanCrop = 2, AttrFades = 4, AttrTransitions = 8, AttrLevel = 16, AttrSpeed = 32, AttrFixes = 64,
    AttrLabel = 128, AttrAll = 255
};
int pasteAttributes(Timeline& tl, const MediaMap& media, const Clip& from, bool fromPicture, const std::set<int>& to, unsigned what,
                    const FrameRate& fps);
// Replace Media: the events `ids` (and the events grouped with them that show the same file) show `mediaId` instead,
// keeping where they are, their trims (where the new file is long enough), effects and keyframes. Events of a kind
// the new file lacks (a picture from a sound file) are left. Returns how many changed; `why` says why none did.
int replaceMedia(Timeline& tl, const MediaMap& media, const std::set<int>& ids, int mediaId, const FrameRate& fps, QString* why = nullptr);

// Removes the transition at one end of an event so it starts / ends with a
// hard cut: its fade goes and its transition type resets. Where it overlaps
// the event before it (a crossfade), that event (and the ones grouped with
// it) is cut where this one starts. The end of an event overlapping the next
// one is that event's start. Returns false if there was nothing to remove.
bool removeTransition(Timeline& tl, int clipId, bool atStart);
// Whether an event has a transition (fade or overlap) at that end, and its name.
bool hasTransition(const Timeline& tl, int clipId, bool atStart);
// Gives the events grouped with an event (e.g. a video file's audio) the same
// fade at the ends they share with it, so transitions affect both.
void syncGroupFades(Timeline& tl, int clipId);

void removeClips(Timeline& tl, const std::set<int>& ids);
// An event wholly inside another on its track (a crossfade over all of it):
// the longer one is cut before and after it, so it plays in between (an
// overwrite). True if anything was cut.
bool overwriteContained(Timeline& tl, int clipId);
// Automatic crossfades off: the events `ids` (just moved, trimmed or placed)
// cut what they overlap on their tracks instead of crossfading with it: an
// event under one of them is shortened, split around it or removed.
void overwriteOverlaps(Timeline& tl, const std::set<int>& ids);
// Removes clips and closes the gaps they leave on their tracks.
void rippleRemoveClips(Timeline& tl, const std::set<int>& ids);
// Removes clips and closes the time they used on every track (and moves
// markers after them back too when `markers`).
void rippleRemoveClipsAll(Timeline& tl, const std::set<int>& ids, bool markers);

// --- Ripple editing ---
// Moves every event starting at or after `from` by `delta` (negative =
// earlier) on the tracks with the given ids (empty = all tracks), except
// `exclude`; markers and regions at or after `from` too when `markers`.
// With `markers`, story chapters move too (one the edit is inside of gets longer or shorter); `chapters` false
// leaves them to a caller that sets them itself.
void rippleShift(Timeline& tl, TimeUs from, TimeUs delta, const std::set<int>& trackIds, const std::set<int>& exclude,
                 bool markers, bool chapters = true);
// The story chapters (and their regions) of that ripple edit alone; the chapters `except` (ids) stay as they are.
void rippleChapters(Timeline& tl, TimeUs from, TimeUs delta, const std::set<int>& except = {});
// Opens a gap of `length` at `at` on those tracks: events crossing `at` are
// split first, then everything from `at` on moves later.
void rippleInsert(Timeline& tl, TimeUs at, TimeUs length, const std::set<int>& trackIds, const std::set<int>& exclude,
                  bool markers);

std::set<int> expandToGroups(const Timeline& tl, const std::set<int>& ids);
void groupClips(Timeline& tl, const std::set<int>& ids);
void ungroupClips(Timeline& tl, const std::set<int>& ids);

// Inserts a new track; index -1 appends. Returns the new track's index.
int addTrack(Timeline& tl, TrackType type, int index = -1);
// A recorded voice-over starting at `at`: on the first audio track named "Voice-over..." with room for it, otherwise on a
// new "Voice-over" track above the other audio tracks. Returns the new event's id (0: the media has no audio).
int placeVoiceOver(Timeline& tl, const MediaItem& media, TimeUs at, const FrameRate& fps);
void removeTrack(Timeline& tl, int index);
// Moves a track so that it ends up at index `to`.
void moveTrack(Timeline& tl, int from, int to);

} // namespace ops
