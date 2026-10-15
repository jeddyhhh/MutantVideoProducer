#include "core/Effects.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <numbers>

// ---------------------------------------------------------------------------
// Keyframes
// ---------------------------------------------------------------------------

namespace {

double lerpColor(double a, double b, double f)
{
    const auto ia = uint32_t(a), ib = uint32_t(b);
    uint32_t out = 0;
    for (int shift : {16, 8, 0}) {
        const double ca = (ia >> shift) & 0xff, cb = (ib >> shift) & 0xff;
        out |= uint32_t(std::clamp<long>(std::lround(ca + (cb - ca) * f), 0, 255)) << shift; // (Elastic overshoots)
    }
    return double(out);
}

double cubic(double t, double p0, double p1, double p2, double p3)
{
    const double s = 1.0 - t;
    return s * s * s * p0 + 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 + t * t * t * p3;
}

double bounce(double x)
{
    const double a = 7.5625, b = 2.75;
    if (x < 1 / b)
        return a * x * x;
    if (x < 2 / b)
        return x -= 1.5 / b, a * x * x + 0.75;
    if (x < 2.5 / b)
        return x -= 2.25 / b, a * x * x + 0.9375;
    return x -= 2.625 / b, a * x * x + 0.984375;
}

} // namespace

QString keyInterpName(Keyframe::Interp interp)
{
    static const char* names[] = {"Linear", "Hold", "Smooth", "Ease In", "Ease Out", "Ease In/Out", "Bezier", "Bounce", "Elastic"};
    return QString::fromLatin1(names[std::clamp(int(interp), 0, kKeyInterps - 1)]);
}

QString keyInterpId(Keyframe::Interp interp)
{
    static const char* ids[] = {"linear", "hold", "smooth", "ease_in", "ease_out", "ease_in_out", "bezier", "bounce", "elastic"};
    return QString::fromLatin1(ids[std::clamp(int(interp), 0, kKeyInterps - 1)]);
}

std::optional<Keyframe::Interp> keyInterpFromId(const QString& id)
{
    for (int i = 0; i < kKeyInterps; ++i)
        if (keyInterpId(Keyframe::Interp(i)) == id)
            return Keyframe::Interp(i);
    return std::nullopt;
}

double keyframeProgress(const Keyframe& a, const Keyframe& b, double f)
{
    f = std::clamp(f, 0.0, 1.0);
    switch (a.interp) {
    case Keyframe::Interp::Linear: return f;
    case Keyframe::Interp::Hold: return f < 1.0 ? 0.0 : 1.0;
    case Keyframe::Interp::Smooth: return f * f * (3.0 - 2.0 * f);
    case Keyframe::Interp::EaseIn: return f * f * f;
    case Keyframe::Interp::EaseOut: return 1.0 - std::pow(1.0 - f, 3.0);
    case Keyframe::Interp::EaseInOut: return f < 0.5 ? 4.0 * f * f * f : 1.0 - std::pow(-2.0 * f + 2.0, 3.0) / 2.0;
    case Keyframe::Interp::Bounce: return bounce(f);
    case Keyframe::Interp::Elastic:
        return f <= 0.0 || f >= 1.0 ? f : std::pow(2.0, -10.0 * f) * std::sin((f * 10.0 - 0.75) * 2.0 * std::numbers::pi / 3.0) + 1.0;
    case Keyframe::Interp::Bezier:
        // Packed colours have no "above": the handles' timing only would mislead, so a smooth curve.
        if (b.value == a.value)
            return f * f * (3.0 - 2.0 * f);
        return (keyframeValue(a, b, f) - a.value) / (b.value - a.value);
    }
    return f;
}

double keyframeValue(const Keyframe& a, const Keyframe& b, double f)
{
    f = std::clamp(f, 0.0, 1.0);
    if (a.interp != Keyframe::Interp::Bezier)
        return a.value + (b.value - a.value) * keyframeProgress(a, b, f);
    // The curve's time runs 0, x1, x2, 1 (x1, x2 within 0..1: one value per time); find its t at f.
    const double x1 = std::clamp(a.x1, 0.0, 1.0), x2 = std::clamp(a.x2, 0.0, 1.0);
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i) {
        const double mid = (lo + hi) / 2;
        (cubic(mid, 0.0, x1, x2, 1.0) < f ? lo : hi) = mid;
    }
    return cubic((lo + hi) / 2, a.value, a.value + a.y1, b.value + a.y2, b.value);
}

void fitBezierHandles(Keyframe& a, const Keyframe& b)
{
    const double dv = b.value - a.value;
    // Smooth, Ease In and Ease Out are cubics with evenly spaced time handles: exact.
    switch (a.interp) {
    case Keyframe::Interp::Linear: a.x1 = 1.0 / 3, a.y1 = dv / 3, a.x2 = 2.0 / 3, a.y2 = -dv / 3; break;
    case Keyframe::Interp::EaseIn: a.x1 = 1.0 / 3, a.y1 = 0, a.x2 = 2.0 / 3, a.y2 = -dv; break;
    case Keyframe::Interp::EaseOut: a.x1 = 1.0 / 3, a.y1 = dv, a.x2 = 2.0 / 3, a.y2 = 0; break;
    case Keyframe::Interp::EaseInOut: a.x1 = 0.65, a.y1 = 0, a.x2 = 0.35, a.y2 = 0; break;
    case Keyframe::Interp::Hold: a.x1 = 1, a.y1 = 0, a.x2 = 1, a.y2 = -dv; break;
    case Keyframe::Interp::Bezier: return;
    default: a.x1 = 1.0 / 3, a.y1 = 0, a.x2 = 2.0 / 3, a.y2 = 0; break; // Smooth (Bounce, Elastic: near enough)
    }
    a.interp = Keyframe::Interp::Bezier;
}

void keysToJson(const std::vector<Keyframe>& keys, QJsonObject& o, const QString& keysName)
{
    QJsonArray list, bez;
    for (size_t i = 0; i < keys.size(); ++i) {
        const Keyframe& k = keys[i];
        list.append(QJsonArray{QString::number(k.time), k.value, int(k.interp)});
        if (k.interp == Keyframe::Interp::Bezier)
            bez.append(QJsonArray{int(i), k.x1, k.y1, k.x2, k.y2});
    }
    o[keysName] = list;
    if (!bez.isEmpty())
        o[QStringLiteral("bez")] = bez;
}

std::vector<Keyframe> keysFromJson(const QJsonObject& o, const QString& keysName)
{
    std::map<int, QJsonArray> handles; // by position in the list
    for (const auto& h : o[QStringLiteral("bez")].toArray())
        if (const QJsonArray a = h.toArray(); a.size() == 5 && a[0].isDouble())
            handles[a[0].toInt()] = a;
    std::vector<Keyframe> keys;
    const QJsonArray list = o[keysName].toArray();
    for (int i = 0; i < list.size(); ++i) {
        const QJsonArray a = list[i].toArray();
        if (a.size() < 2 || a.size() > 3 || !a[1].isDouble() || !std::isfinite(a[1].toDouble()))
            continue;
        bool ok = false;
        const double time = a[0].isString() ? double(a[0].toString().toLongLong(&ok)) : a[0].toDouble();
        if (a[0].isDouble()) ok = true;
        if (!ok || !std::isfinite(time) || std::abs(time) > 1'000'000'000'000.0 || std::trunc(time) != time)
            continue;
        Keyframe k;
        k.time = TimeUs(time);
        k.value = a[1].toDouble();
        const int curve = a.size() == 3 ? a[2].toInt() : 0;
        k.interp = Keyframe::Interp(curve < 0 ? 0 : curve < kKeyInterps ? curve : int(Keyframe::Interp::Smooth)); // (a newer curve)
        if (k.interp == Keyframe::Interp::Bezier) {
            auto h = handles.find(i);
            double v[4] = {k.x1, k.y1, k.x2, k.y2};
            if (h != handles.end())
                for (int j = 0; j < 4; ++j)
                    if (const double x = h->second[j + 1].toDouble(v[j]); std::isfinite(x) && std::abs(x) < 1e12)
                        v[j] = x;
            k.x1 = std::clamp(v[0], 0.0, 1.0), k.y1 = v[1], k.x2 = std::clamp(v[2], 0.0, 1.0), k.y2 = v[3];
        }
        keys.push_back(k);
    }
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
    // The last entry at a timestamp wins, matching an interactive key edit.
    std::vector<Keyframe> unique;
    for (const auto& key : keys) {
        if (!unique.empty() && unique.back().time == key.time) unique.back() = key;
        else unique.push_back(key);
    }
    return unique;
}

double ParamValue::at(TimeUs t, ParamType type) const
{
    if (keys.empty())
        return value;
    if (t <= keys.front().time)
        return keys.front().value;
    if (t >= keys.back().time)
        return keys.back().value;
    auto next = std::upper_bound(keys.begin(), keys.end(), t, [](TimeUs v, const Keyframe& k) { return v < k.time; });
    const Keyframe& a = *std::prev(next);
    const Keyframe& b = *next;
    const double f = double(t - a.time) / double(std::max<TimeUs>(1, b.time - a.time));
    if (type == ParamType::Color)
        return lerpColor(a.value, b.value, keyframeProgress(a, b, f));
    return keyframeValue(a, b, f);
}

bool ParamValue::hasKeyAt(TimeUs t, TimeUs tolerance) const
{
    return std::any_of(keys.begin(), keys.end(), [&](const Keyframe& k) { return std::llabs(k.time - t) <= tolerance; });
}

void ParamValue::removeKeyAt(TimeUs t, TimeUs tolerance)
{
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return std::llabs(k.time - t) <= tolerance; }),
               keys.end());
}

void ParamValue::addKey(TimeUs t, double v)
{
    for (auto& k : keys)
        if (k.time == t) {
            k.value = v;
            return;
        }
    Keyframe k;
    k.time = t;
    k.value = v;
    keys.insert(std::upper_bound(keys.begin(), keys.end(), t, [](TimeUs v2, const Keyframe& kk) { return v2 < kk.time; }), k);
}

void ParamValue::set(TimeUs t, double v)
{
    if (keys.empty())
        value = v;
    else
        addKey(t, v);
}

double Params::num(const QString& id) const
{
    auto it = numbers.find(id);
    return it == numbers.end() ? 0.0 : it->second;
}

QColor Params::color(const QString& id) const
{
    return QColor::fromRgb(QRgb(uint32_t(num(id)) | 0xff000000u));
}

QString Params::text(const QString& id) const
{
    auto it = texts.find(id);
    return it == texts.end() ? QString() : it->second;
}

Params resolveParams(const std::vector<ParamDef>& defs, const ParamMap& values, TimeUs t)
{
    Params p;
    for (const ParamDef& d : defs) {
        auto it = values.find(d.id);
        if (d.type == ParamType::Text || d.type == ParamType::Font) {
            p.texts[d.id] = it == values.end() ? d.defText : it->second.text;
            continue;
        }
        double v = it == values.end() ? d.def : it->second.at(t, d.type);
        if (d.type == ParamType::Number)
            v = std::clamp(v, d.min, d.max);
        p.numbers[d.id] = v;
    }
    return p;
}

ParamMap defaultParams(const std::vector<ParamDef>& defs)
{
    ParamMap m;
    for (const ParamDef& d : defs) {
        ParamValue v;
        v.value = d.def;
        v.text = d.defText;
        m[d.id] = v;
    }
    return m;
}

void shiftKeyframes(ParamMap& params, TimeUs delta)
{
    for (auto& [id, p] : params)
        for (auto& k : p.keys)
            k.time += delta;
}

void shiftKeyframes(EffectChain& chain, TimeUs delta)
{
    for (auto& e : chain) {
        shiftKeyframes(e.params, delta);
        for (auto& c : e.chains) {
            shiftKeyframes(c.params, delta);
            shiftKeyframes(c.effects, delta);
        }
    }
}

// ---------------------------------------------------------------------------
// Racks
// ---------------------------------------------------------------------------

const std::vector<ParamDef>& rackParams()
{
    static const std::vector<ParamDef> defs = [] {
        ParamDef mix;
        mix.id = QStringLiteral("mix");
        mix.name = QStringLiteral("Dry/Wet");
        mix.min = 0;
        mix.max = 100;
        mix.def = 100;
        mix.decimals = 0;
        mix.suffix = QStringLiteral(" %");
        return std::vector<ParamDef>{mix};
    }();
    return defs;
}

QStringList blendModeNames()
{
    return {QStringLiteral("Normal"), QStringLiteral("Add"), QStringLiteral("Screen"), QStringLiteral("Multiply"),
            QStringLiteral("Overlay"), QStringLiteral("Lighten"), QStringLiteral("Darken"), QStringLiteral("Difference")};
}

const std::vector<ParamDef>& rackChainParams(bool audio)
{
    static const std::vector<ParamDef> audioDefs = [] {
        ParamDef level;
        level.id = QStringLiteral("level");
        level.name = QStringLiteral("Volume");
        level.min = -60;
        level.max = 12;
        level.def = 0;
        level.decimals = 1;
        level.suffix = QStringLiteral(" dB");
        return std::vector<ParamDef>{level};
    }();
    static const std::vector<ParamDef> videoDefs = [] {
        ParamDef level;
        level.id = QStringLiteral("level");
        level.name = QStringLiteral("Opacity");
        level.min = 0;
        level.max = 100;
        level.def = 100;
        level.decimals = 0;
        level.suffix = QStringLiteral(" %");
        ParamDef blend;
        blend.id = QStringLiteral("blend");
        blend.name = QStringLiteral("Blend");
        blend.type = ParamType::Choice;
        blend.choices = blendModeNames();
        blend.max = double(blend.choices.size() - 1);
        return std::vector<ParamDef>{level, blend};
    }();
    return audio ? audioDefs : videoDefs;
}

RackChain makeRackChain(bool audio, const QString& name, EffectChain effects)
{
    RackChain c;
    c.name = name;
    c.params = defaultParams(rackChainParams(audio));
    c.effects = std::move(effects);
    return c;
}

EffectInstance makeRack(int id)
{
    EffectInstance e;
    e.id = id;
    e.type = QString::fromLatin1(kRackType);
    e.params = defaultParams(rackParams());
    return e;
}

std::vector<const RackChain*> activeRackChains(const EffectInstance& rack)
{
    const bool anySolo = std::any_of(rack.chains.begin(), rack.chains.end(), [](const RackChain& c) { return c.enabled && c.solo; });
    std::vector<const RackChain*> out;
    for (const RackChain& c : rack.chains)
        if (c.enabled && (!anySolo || c.solo))
            out.push_back(&c);
    return out;
}

void reassignEffectIds(EffectChain& chain, const std::function<int()>& allocId)
{
    for (auto& e : chain) {
        e.id = allocId();
        for (auto& c : e.chains)
            reassignEffectIds(c.effects, allocId);
    }
}

void forEachEffect(const EffectChain& chain, const std::function<void(const EffectInstance&)>& fn)
{
    for (const auto& e : chain) {
        fn(e);
        for (const auto& c : e.chains)
            forEachEffect(c.effects, fn);
    }
}

EffectChain* resolveChain(EffectChain& root, const ChainPath& path)
{
    EffectChain* c = &root;
    for (const auto& [effect, chain] : path.steps) {
        if (effect < 0 || effect >= int(c->size()) || !(*c)[size_t(effect)].isRack())
            return nullptr;
        auto& chains = (*c)[size_t(effect)].chains;
        if (chain < 0 || chain >= int(chains.size()))
            return nullptr;
        c = &chains[size_t(chain)].effects;
    }
    return c;
}

const EffectChain* resolveChain(const EffectChain& root, const ChainPath& path)
{
    return resolveChain(const_cast<EffectChain&>(root), path);
}

bool moveEffect(EffectChain& root, const ChainPath& fromPath, int from, const ChainPath& toPath, int to, bool copy,
                const std::function<int()>& allocId)
{
    EffectChain* src = resolveChain(root, fromPath);
    if (!src || from < 0 || from >= int(src->size()) || !resolveChain(root, toPath))
        return false;
    // The destination is inside the moved rack?
    const size_t n = fromPath.steps.size();
    const bool inside = toPath.steps.size() > n && std::equal(fromPath.steps.begin(), fromPath.steps.end(), toPath.steps.begin())
                        && toPath.steps[n].first == from;
    if (inside)
        return false;
    EffectInstance item = (*src)[size_t(from)];
    ChainPath dest = toPath;
    if (copy) {
        EffectChain one{item};
        reassignEffectIds(one, allocId);
        item = std::move(one.front());
    } else {
        if (toPath == fromPath && to > from)
            --to; // positions after the gap shift left
        if (dest.steps.size() > n && std::equal(fromPath.steps.begin(), fromPath.steps.end(), dest.steps.begin())
            && dest.steps[n].first > from)
            --dest.steps[n].first;
        src->erase(src->begin() + from);
    }
    EffectChain* dst = resolveChain(root, dest);
    if (!dst)
        return false;
    to = std::clamp(to, 0, int(dst->size()));
    dst->insert(dst->begin() + to, std::move(item));
    return true;
}

int groupIntoRack(EffectChain& root, const ChainPath& path, std::vector<int> indices, int rackId, bool audio)
{
    EffectChain* c = resolveChain(root, path);
    if (!c)
        return -1;
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    if (indices.empty() || indices.front() < 0 || indices.back() >= int(c->size()))
        return -1;
    EffectChain grouped;
    for (int i : indices)
        grouped.push_back((*c)[size_t(i)]);
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
        c->erase(c->begin() + *it);
    EffectInstance rack = makeRack(rackId);
    rack.chains.push_back(makeRackChain(audio, QStringLiteral("Chain 1"), std::move(grouped)));
    const int at = indices.front();
    c->insert(c->begin() + at, std::move(rack));
    return at;
}

bool ungroupRack(EffectChain& root, const ChainPath& path, int index)
{
    EffectChain* c = resolveChain(root, path);
    if (!c || index < 0 || index >= int(c->size()) || !(*c)[size_t(index)].isRack() || (*c)[size_t(index)].chains.size() > 1)
        return false;
    EffectChain inner = (*c)[size_t(index)].chains.empty() ? EffectChain{} : (*c)[size_t(index)].chains.front().effects;
    c->erase(c->begin() + index);
    c->insert(c->begin() + index, inner.begin(), inner.end());
    return true;
}

EffectInstance makeEffect(const EffectDef& def, int id)
{
    EffectInstance e;
    e.id = id;
    e.type = def.id;
    e.params = defaultParams(def.params);
    return e;
}

TransitionSpec makeTransition(const TransitionDef& def)
{
    TransitionSpec t;
    t.type = def.id;
    t.params = defaultParams(def.params);
    return t;
}

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------

QJsonObject paramsToJson(const ParamMap& params)
{
    QJsonObject o;
    for (const auto& [id, v] : params) {
        QJsonObject p;
        p[QStringLiteral("v")] = v.value;
        if (!v.text.isEmpty())
            p[QStringLiteral("text")] = v.text;
        if (!v.keys.empty())
            keysToJson(v.keys, p, QStringLiteral("keys"));
        if (v.mod) {
            const Modulation& m = *v.mod;
            QJsonObject mo;
            mo[QStringLiteral("source")] = int(m.source);
            mo[QStringLiteral("shape")] = int(m.shape);
            mo[QStringLiteral("amount")] = m.amount;
            mo[QStringLiteral("beats")] = m.beats;
            if (m.source == Modulation::Source::Midi) {
                mo[QStringLiteral("channel")] = m.channel;
                mo[QStringLiteral("cc")] = m.cc;
            }
            p[QStringLiteral("mod")] = mo;
        }
        o[id] = p;
    }
    return o;
}

ParamMap paramsFromJson(const QJsonObject& o)
{
    ParamMap m;
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QJsonObject p = it.value().toObject();
        ParamValue v;
        v.value = p[QStringLiteral("v")].toDouble();
        v.text = p[QStringLiteral("text")].toString();
        v.keys = keysFromJson(p, QStringLiteral("keys"));
        if (p.contains(QStringLiteral("mod"))) {
            const QJsonObject mo = p[QStringLiteral("mod")].toObject();
            Modulation mod;
            mod.source = Modulation::Source(std::clamp(mo[QStringLiteral("source")].toInt(), 0, int(Modulation::Source::Mids)));
            mod.shape = Modulation::Shape(std::clamp(mo[QStringLiteral("shape")].toInt(), 0, int(Modulation::Shape::Random)));
            mod.amount = std::clamp(mo[QStringLiteral("amount")].toDouble(0.5), -1.0, 1.0);
            mod.beats = std::clamp(mo[QStringLiteral("beats")].toDouble(1), 0.125, 64.0);
            mod.channel = std::clamp(mo[QStringLiteral("channel")].toInt(), 0, 15);
            mod.cc = std::clamp(mo[QStringLiteral("cc")].toInt(-1), -1, 127);
            v.mod = mod;
        }
        m[it.key()] = v;
    }
    return m;
}

QJsonObject effectToJson(const EffectInstance& e)
{
    QJsonObject o;
    o[QStringLiteral("id")] = e.id;
    o[QStringLiteral("type")] = e.type;
    o[QStringLiteral("enabled")] = e.enabled;
    o[QStringLiteral("params")] = paramsToJson(e.params);
    if (e.isRack()) {
        QJsonArray chains;
        for (const RackChain& c : e.chains) {
            QJsonObject co;
            co[QStringLiteral("name")] = c.name;
            co[QStringLiteral("enabled")] = c.enabled;
            co[QStringLiteral("solo")] = c.solo;
            co[QStringLiteral("params")] = paramsToJson(c.params);
            QJsonArray effects;
            for (const EffectInstance& sub : c.effects)
                effects.append(effectToJson(sub));
            co[QStringLiteral("effects")] = effects;
            chains.append(co);
        }
        o[QStringLiteral("chains")] = chains;
    }
    return o;
}

EffectInstance effectFromJson(const QJsonObject& o)
{
    EffectInstance e;
    e.id = o[QStringLiteral("id")].toInt();
    e.type = o[QStringLiteral("type")].toString();
    e.enabled = o[QStringLiteral("enabled")].toBool(true);
    e.params = paramsFromJson(o[QStringLiteral("params")].toObject());
    for (const auto& v : o[QStringLiteral("chains")].toArray()) {
        if (!e.isRack()) // only racks have (and save) chains
            break;
        const QJsonObject co = v.toObject();
        RackChain c;
        c.name = co[QStringLiteral("name")].toString();
        c.enabled = co[QStringLiteral("enabled")].toBool(true);
        c.solo = co[QStringLiteral("solo")].toBool(false);
        c.params = paramsFromJson(co[QStringLiteral("params")].toObject());
        for (const auto& ev : co[QStringLiteral("effects")].toArray())
            c.effects.push_back(effectFromJson(ev.toObject()));
        e.chains.push_back(std::move(c));
    }
    return e;
}

QJsonObject transitionToJson(const TransitionSpec& t)
{
    QJsonObject o;
    o[QStringLiteral("type")] = t.type;
    o[QStringLiteral("params")] = paramsToJson(t.params);
    return o;
}

TransitionSpec transitionFromJson(const QJsonObject& o)
{
    TransitionSpec t;
    t.type = o[QStringLiteral("type")].toString();
    if (!t.type.isEmpty()) // settings without a type are never saved
        t.params = paramsFromJson(o[QStringLiteral("params")].toObject());
    return t;
}
