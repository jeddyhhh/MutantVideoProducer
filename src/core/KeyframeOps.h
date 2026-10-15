#pragma once

#include "core/Project.h"

#include <QJsonObject>
#include <set>
#include <vector>

// The keyframed settings of an event, a track, the video output or the audio
// master, found by what they belong to (not by position, so reordering effects
// keeps them), and the edits the Keyframe Editor makes to their keyframes.
namespace keyops {

// Where a setting lives in the timeline.
struct Address {
    enum class Source { ClipFx, TrackFx, OutputFx, MasterFx, PanCrop, Generator, TransitionIn, TransitionOut };
    Source source = Source::ClipFx;
    int owner = 0;   // clip id (ClipFx, PanCrop, Generator, Transition*), track id (TrackFx)
    int effect = 0;  // effect id (the *Fx sources)
    int chain = -1;  // a rack's chain (its level and blend), else -1
    QString param;

    QString key() const; // unique and stable: "clipfx:12:40:-1:amount"
    bool operator==(const Address&) const = default;
};

// One setting that can have keyframes (a number or a colour).
struct Channel {
    Address address;
    ParamDef def;
    QString group;       // "Event FX · Gaussian Blur", "Pan/Crop", "Title", "Transition In · Iris"
    QString effectType;  // effect, generator or transition type ("" for Pan/Crop)
    int ordinal = 0;     // which effect of that type (the first is 0): pastes find their match by it
    bool eventLocal = false; // keyframe times count from the event's start
};

// What the editor shows.
enum class Scope { Clip, Track, Output, AudioMaster };
// Every animatable setting of an event (its FX with racks, Pan/Crop, generated media and
// transitions), a track's FX, the output FX or the master FX, in panel order.
std::vector<Channel> channels(const Timeline& tl, Scope scope, int id);

// The setting's value and keyframes. `create`: a setting not stored yet (Pan/Crop, a new effect
// parameter) is added with its default. Null when what it belongs to is gone.
ParamValue* find(Timeline& tl, const Channel& ch, bool create = false);
const ParamValue* find(const Timeline& tl, const Channel& ch);
// Converts between the channel's keyframe time and timeline time.
TimeUs toTimeline(const Timeline& tl, const Channel& ch, TimeUs key);
TimeUs fromTimeline(const Timeline& tl, const Channel& ch, TimeUs t);
// The keyframe times the channel may use: its event's span, or [0, a long way].
std::pair<TimeUs, TimeUs> keyRange(const Timeline& tl, const Channel& ch);

// ---- Edits on one setting's keyframes; keyframes are picked by time ----

using Times = std::set<TimeUs>;
// Moves the picked keyframes by `delta`; one landing on a keyframe that is not moving replaces it.
// Returns their new times.
Times moveKeys(ParamValue& p, const Times& picked, TimeUs delta);
// Spreads the picked keyframes out (factor > 1) or together around `pivot`; `snap` rounds a time
// (to a frame). Keyframes that end up together merge. Returns their new times.
Times scaleKeys(ParamValue& p, const Times& picked, TimeUs pivot, double factor, const std::function<TimeUs(TimeUs)>& snap);
// Plays the picked keyframes backwards in the span they cover (curves mirror with them).
Times reverseKeys(ParamValue& p, const Times& picked);
// Removes them; with none left, the setting keeps the value it had at `now`.
void deleteKeys(ParamValue& p, const Times& picked, TimeUs now, ParamType type);
// The curve from each picked keyframe to the next. Bezier starts from the curve each one had.
void setInterp(ParamValue& p, const Times& picked, Keyframe::Interp interp);
// Bezier curves through the picked keyframes with smooth handles: flat at the ends and at peaks
// and dips (no overshoot), sloped through the keyframes in between.
void autoTangents(ParamValue& p, const Times& picked);
// A keyframe at t with the value the curve has there; a Bezier curve keeps its shape.
void insertKey(ParamValue& p, TimeUs t, ParamType type);
// Sets the picked keyframes' values (clamped to the setting's range).
void setValue(ParamValue& p, const Times& picked, double value, const ParamDef& def);

// ---- Copy and paste ----

// The picked keyframes of some settings, times from the earliest of them all.
QJsonObject copyKeys(const Timeline& tl, const std::vector<std::pair<Channel, Times>>& picked);
// Puts copied keyframes into the matching settings of `into` (the same setting, else the same
// effect type and parameter, else, for one copied setting, `fallback`), starting at timeline
// time `at`. Returns how many keyframes were pasted.
int pasteKeys(Timeline& tl, const QJsonObject& copied, const std::vector<Channel>& into, const Channel* fallback, TimeUs at);

} // namespace keyops
