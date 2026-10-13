#include "core/Effects.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>

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
        out |= uint32_t(std::lround(ca + (cb - ca) * f)) << shift;
    }
    return double(out);
}

} // namespace

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
    double f = double(t - a.time) / double(std::max<TimeUs>(1, b.time - a.time));
    switch (a.interp) {
    case Keyframe::Interp::Hold: f = 0.0; break;
    case Keyframe::Interp::Smooth: f = f * f * (3.0 - 2.0 * f); break;
    case Keyframe::Interp::Linear: break;
    }
    if (type == ParamType::Color)
        return lerpColor(a.value, b.value, f);
    return a.value + (b.value - a.value) * f;
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
        if (!v.keys.empty()) {
            QJsonArray keys;
            for (const auto& k : v.keys) {
                QJsonArray kv;
                kv.append(QString::number(k.time));
                kv.append(k.value);
                kv.append(int(k.interp));
                keys.append(kv);
            }
            p[QStringLiteral("keys")] = keys;
        }
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
        for (const auto& kv : p[QStringLiteral("keys")].toArray()) {
            const QJsonArray a = kv.toArray();
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
            k.interp = Keyframe::Interp(std::clamp(a.size() == 3 ? a[2].toInt() : 0, 0, 2));
            v.keys.push_back(k);
        }
        std::stable_sort(v.keys.begin(), v.keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
        // The last entry at a timestamp wins, matching an interactive key edit.
        std::vector<Keyframe> unique;
        for (const auto& key : v.keys) {
            if (!unique.empty() && unique.back().time == key.time) unique.back() = key;
            else unique.push_back(key);
        }
        v.keys = std::move(unique);
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
