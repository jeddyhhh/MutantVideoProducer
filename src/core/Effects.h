#pragma once

#include "core/Time.h"

#include <QColor>
#include <array>
#include <cstdint>
#include <QImage>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

// ---------------------------------------------------------------------------
// Parameters and keyframes
// ---------------------------------------------------------------------------

enum class ParamType { Number, Choice, Bool, Color, Text, Font };

struct ParamDef {
    QString id;
    QString name;
    ParamType type = ParamType::Number;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;          // Color: 0xRRGGBB, Choice: index, Bool: 0/1
    int decimals = 2;
    QString suffix;
    QStringList choices;
    QString defText;           // Text / Font params
    QString tooltip;
    bool multiline = false;    // Text params: multi-line editor
    bool logScale = false;     // Number params: slider is logarithmic (zoom, scale)
    QString fileFilter;        // Text params: a file path, with a Browse button using this filter
    bool hidden = false;       // kept with the effect, not shown as a control (e.g. a plugin's saved state)

    bool animatable() const { return type == ParamType::Number || type == ParamType::Color; }
};

struct Keyframe {
    enum class Interp { Linear, Hold, Smooth };
    TimeUs time = 0;           // event-local for event FX, timeline time otherwise
    double value = 0.0;
    Interp interp = Interp::Linear; // curve towards the next keyframe

    bool operator==(const Keyframe&) const = default;
};

// Live tab: a setting that moves by itself, on the beat, with the sound, or
// from a MIDI controller (see LiveModulation.h).
struct Modulation {
    enum class Source { Beat, Bar, Lfo, Level, Bass, Highs, Midi, Mids }; // (new ones last: saved as numbers)
    enum class Shape { Sine, Triangle, SawUp, SawDown, Square, Random };
    Source source = Source::Beat;
    Shape shape = Shape::Sine;   // LFO
    double amount = 0.5;         // -1..1: how far, as a share of the setting's range
    double beats = 1;            // LFO: beats per cycle (0.25..16); pulses: how long they last
    int channel = 0, cc = -1;    // MIDI: channel 0..15, controller 0..127
    bool operator==(const Modulation&) const = default;
};

struct ParamValue {
    double value = 0.0;        // used when there are no keyframes
    QString text;
    std::vector<Keyframe> keys; // sorted by time
    std::optional<Modulation> mod; // Live tab only

    double at(TimeUs t, ParamType type) const;
    // Sets the value at t: updates/creates a keyframe if animated, else the static value.
    void set(TimeUs t, double v);
    bool hasKeyAt(TimeUs t, TimeUs tolerance = 0) const;
    void removeKeyAt(TimeUs t, TimeUs tolerance = 0);
    void addKey(TimeUs t, double v);

    bool operator==(const ParamValue&) const = default;
};

using ParamMap = std::map<QString, ParamValue>;

// Parameter values resolved at one point in time, handed to effect code.
class Params {
public:
    double num(const QString& id) const;
    int choice(const QString& id) const { return int(num(id) + 0.5); }
    bool flag(const QString& id) const { return num(id) >= 0.5; }
    QColor color(const QString& id) const;
    QString text(const QString& id) const;

    std::map<QString, double> numbers;
    std::map<QString, QString> texts;

    bool operator==(const Params&) const = default;
};

Params resolveParams(const std::vector<ParamDef>& defs, const ParamMap& values, TimeUs t);
ParamMap defaultParams(const std::vector<ParamDef>& defs);

// ---------------------------------------------------------------------------
// Instances stored in the project
// ---------------------------------------------------------------------------

struct RackChain;

// One effect in a chain. A rack (type kRackType) holds parallel chains that
// each process the rack's input and are mixed back together; racks can be
// nested.
struct EffectInstance {
    int id = 0;                // unique within the timeline (runtime state key)
    QString type;              // EffectDef::id, or kRackType
    bool enabled = true;
    ParamMap params;
    std::vector<RackChain> chains; // racks only

    bool isRack() const;
    bool operator==(const EffectInstance&) const = default;
};

using EffectChain = std::vector<EffectInstance>;

// One parallel chain of a rack.
struct RackChain {
    QString name;
    bool enabled = true;
    bool solo = false;
    ParamMap params;           // rackChainParams(): level, blend mode (video)
    EffectChain effects;

    bool operator==(const RackChain&) const = default;
};

inline constexpr const char* kRackType = "rack";
inline bool EffectInstance::isRack() const { return type == QLatin1String(kRackType); }

// ---- Racks -------------------------------------------------------------------
const std::vector<ParamDef>& rackParams();                 // "mix" (dry/wet, %)
const std::vector<ParamDef>& rackChainParams(bool audio);  // "level" (dB / %), video "blend"
// Video chains are layered bottom-up with these modes (CPU and GPU agree).
enum class BlendMode { Normal, Add, Screen, Multiply, Overlay, Lighten, Darken, Difference };
QStringList blendModeNames();
EffectInstance makeRack(int id); // empty: add chains with makeRackChain
RackChain makeRackChain(bool audio, const QString& name, EffectChain effects = {});
// Chains of a rack that play: enabled, and soloed ones if any are soloed.
std::vector<const RackChain*> activeRackChains(const EffectInstance& rack);
// Gives every effect, including those inside racks, a fresh id.
void reassignEffectIds(EffectChain& chain, const std::function<int()>& allocId);
// Visits every effect, including those inside racks.
void forEachEffect(const EffectChain& chain, const std::function<void(const EffectInstance&)>& fn);

// Address of a chain inside a target's effect chain: each step enters the
// rack at index `first` and its chain `second`. No steps = the top level.
struct ChainPath {
    std::vector<std::pair<int, int>> steps;
    ChainPath child(int effect, int chain) const
    {
        ChainPath p = *this;
        p.steps.emplace_back(effect, chain);
        return p;
    }
    bool operator==(const ChainPath&) const = default;
};
EffectChain* resolveChain(EffectChain& root, const ChainPath& path);
const EffectChain* resolveChain(const EffectChain& root, const ChainPath& path);
// Moves (or copies, with fresh ids) effect `from` of chain `fromPath` to
// position `to` of chain `toPath` (`to` counts positions before the move).
// Refuses to move a rack into itself.
bool moveEffect(EffectChain& root, const ChainPath& fromPath, int from, const ChainPath& toPath, int to, bool copy,
                const std::function<int()>& allocId);
// Wraps effects `indices` of a chain into a new rack (one chain, same order)
// placed where the first of them was. Returns the rack's index, or -1.
int groupIntoRack(EffectChain& root, const ChainPath& path, std::vector<int> indices, int rackId, bool audio);
// Replaces a rack that has a single chain with that chain's effects.
bool ungroupRack(EffectChain& root, const ChainPath& path, int index);

struct TransitionSpec {
    QString type;              // empty = plain crossfade / fade
    ParamMap params;

    bool isPlainFade() const { return type.isEmpty() || type == QLatin1String("dissolve"); }
    bool operator==(const TransitionSpec&) const = default;
};

// Moves keyframe times (e.g. when an event's start is trimmed).
void shiftKeyframes(EffectChain& chain, TimeUs delta);
void shiftKeyframes(ParamMap& params, TimeUs delta);
QJsonObject paramsToJson(const ParamMap& params);
ParamMap paramsFromJson(const QJsonObject& o);

QJsonObject effectToJson(const EffectInstance& e);
EffectInstance effectFromJson(const QJsonObject& o);
QJsonObject transitionToJson(const TransitionSpec& t);
TransitionSpec transitionFromJson(const QJsonObject& o);

// ---------------------------------------------------------------------------
// Registry: effect and transition implementations
// ---------------------------------------------------------------------------

struct RenderContext {
    TimeUs time = 0;           // time used for keyframes (event-local or timeline)
    TimeUs timelineTime = 0;
    double scale = 1.0;        // output pixels per project pixel (preview < 1)
    double fps = 30.0;
    bool highQuality = false;
    QRectF contentRect;        // where the picture sits in the layer (letterboxing)
};

// Per-compositor runtime state for effects that need it (FFmpeg filter graphs).
struct EffectState {
    virtual ~EffectState() = default;
};

// Per-channel lookup tables (R, G, B) on unpremultiplied colour.
using ChannelLuts = std::array<std::array<uint8_t, 256>, 3>;
// 3x4 row-major colour matrix on unpremultiplied colour in 0..1.
using ColorMatrix = std::array<float, 12>;

struct EffectDef {
    QString id;
    QString name;
    QString category;
    QString description;
    std::vector<ParamDef> params;
    // Pure colour effects describe themselves as LUTs or a matrix, so the
    // CPU and GPU renderers produce identical results. Return false = no-op.
    std::function<bool(const Params&, ChannelLuts&)> luts;
    std::function<bool(const Params&, ColorMatrix&)> colorMatrix;
    // Processes `layer` (premultiplied ARGB32, canvas sized) in place.
    std::function<void(QImage& layer, const Params&, const RenderContext&, std::unique_ptr<EffectState>& state)> apply;
    // Has the shared Mix / "Apply to" mask parameters (fx::glitchCommonParams):
    // the renderers blend the result with the input through that mask.
    bool glitchPost = false;
};

struct TransitionDef {
    QString id;
    QString name;
    QString description;
    std::vector<ParamDef> params;
    // Writes the blend of `from` -> `to` at progress p (0..1) into `out`.
    // Either input may be null (transparent), e.g. a fade from nothing.
    std::function<void(QImage& out, const QImage* from, const QImage* to, double p, const Params&, const RenderContext&)> apply;
};

const std::vector<EffectDef>& videoEffects();
const EffectDef* findEffect(const QString& id);
const std::vector<TransitionDef>& transitions();
const TransitionDef* findTransition(const QString& id);

EffectInstance makeEffect(const EffectDef& def, int id);

// Name, parameters etc. of a video *or* audio effect type (audio ids start with "a_").
struct EffectMeta {
    QString name;
    QString category;
    QString description;
    const std::vector<ParamDef>* params = nullptr;
    bool audio = false;
};
std::optional<EffectMeta> effectMeta(const QString& type);
EffectInstance makeEffectOfType(const QString& type, int id);
TransitionSpec makeTransition(const TransitionDef& def);
