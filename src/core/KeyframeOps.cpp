#include "core/KeyframeOps.h"

#include "core/Generators.h"
#include "core/PanCrop.h"

#include <QJsonArray>
#include <QPointF>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace keyops {

namespace {

constexpr const char* kSourceNames[] = {"clipfx", "trackfx", "outputfx", "masterfx", "pancrop", "generator", "transin", "transout"};

bool isFx(Address::Source s)
{
    return s == Address::Source::ClipFx || s == Address::Source::TrackFx || s == Address::Source::OutputFx || s == Address::Source::MasterFx;
}

EffectInstance* findEffect(EffectChain& chain, int id)
{
    for (auto& e : chain) {
        if (e.id == id)
            return &e;
        for (auto& c : e.chains)
            if (EffectInstance* f = findEffect(c.effects, id))
                return f;
    }
    return nullptr;
}

EffectChain* fxRoot(Timeline& tl, const Address& a)
{
    switch (a.source) {
    case Address::Source::ClipFx:
        if (Clip* c = tl.findClip(a.owner))
            return &c->effects;
        return nullptr;
    case Address::Source::TrackFx:
        for (auto& t : tl.tracks)
            if (t.id == a.owner)
                return &t.effects;
        return nullptr;
    case Address::Source::OutputFx: return &tl.outputEffects;
    case Address::Source::MasterFx: return &tl.audioMasterEffects;
    default: return nullptr;
    }
}

ParamMap* paramMap(Timeline& tl, const Address& a)
{
    if (isFx(a.source)) {
        EffectChain* root = fxRoot(tl, a);
        EffectInstance* e = root ? findEffect(*root, a.effect) : nullptr;
        if (!e)
            return nullptr;
        if (a.chain < 0)
            return &e->params;
        return a.chain < int(e->chains.size()) ? &e->chains[size_t(a.chain)].params : nullptr;
    }
    Clip* c = tl.findClip(a.owner);
    if (!c)
        return nullptr;
    switch (a.source) {
    case Address::Source::PanCrop: return &c->panCrop;
    case Address::Source::Generator: return c->isGenerated() ? &c->generator.params : nullptr;
    case Address::Source::TransitionIn: return &c->transitionIn.params;
    case Address::Source::TransitionOut: return &c->transitionOut.params;
    default: return nullptr;
    }
}

// The animatable settings of `defs`, as channels of one group.
void addDefs(std::vector<Channel>& out, const std::vector<ParamDef>& defs, Address base, const QString& group,
             const QString& type, int ordinal, bool eventLocal)
{
    for (const ParamDef& d : defs) {
        if (!d.animatable() || d.hidden)
            continue;
        Channel ch;
        ch.address = base;
        ch.address.param = d.id;
        ch.def = d;
        ch.group = group;
        ch.effectType = type;
        ch.ordinal = ordinal;
        ch.eventLocal = eventLocal;
        out.push_back(ch);
    }
}

void addChain(std::vector<Channel>& out, const EffectChain& chain, Address base, const QString& prefix, bool audio,
              bool eventLocal, std::map<QString, int>& ordinals)
{
    for (const EffectInstance& e : chain) {
        base.effect = e.id;
        base.chain = -1;
        const int ordinal = ordinals[e.type]++;
        if (e.isRack()) {
            const QString rack = prefix + QObject::tr("Rack");
            addDefs(out, rackParams(), base, rack, e.type, ordinal, eventLocal);
            for (size_t c = 0; c < e.chains.size(); ++c) {
                const QString name = rack + QStringLiteral(" › ") + e.chains[c].name;
                Address chainBase = base;
                chainBase.chain = int(c);
                addDefs(out, rackChainParams(audio), chainBase, name, e.type, ordinal, eventLocal);
                addChain(out, e.chains[c].effects, base, name + QStringLiteral(" › "), audio, eventLocal, ordinals);
            }
            continue;
        }
        const auto meta = effectMeta(e.type);
        if (meta && meta->params)
            addDefs(out, *meta->params, base, prefix + meta->name, e.type, ordinal, eventLocal);
    }
}

std::vector<Keyframe>::iterator keyAt(std::vector<Keyframe>& keys, TimeUs t)
{
    return std::find_if(keys.begin(), keys.end(), [&](const Keyframe& k) { return k.time == t; });
}

void sortKeys(std::vector<Keyframe>& keys)
{
    std::stable_sort(keys.begin(), keys.end(), [](const Keyframe& a, const Keyframe& b) { return a.time < b.time; });
}

// Keyframes `moved` take their times over from the others.
Times mergeMoved(ParamValue& p, std::vector<Keyframe> moved, std::vector<Keyframe> rest)
{
    Times out;
    std::vector<Keyframe> unique;
    sortKeys(moved);
    for (const Keyframe& k : moved) {
        if (!unique.empty() && unique.back().time == k.time)
            unique.back() = k;
        else
            unique.push_back(k);
        out.insert(k.time);
    }
    rest.erase(std::remove_if(rest.begin(), rest.end(), [&](const Keyframe& k) { return out.count(k.time); }), rest.end());
    unique.insert(unique.end(), rest.begin(), rest.end());
    sortKeys(unique);
    p.keys = std::move(unique);
    return out;
}

double cubic(double t, double p0, double p1, double p2, double p3)
{
    const double s = 1.0 - t;
    return s * s * s * p0 + 3.0 * s * s * t * p1 + 3.0 * s * t * t * p2 + t * t * t * p3;
}

} // namespace

QString Address::key() const
{
    return QStringLiteral("%1:%2:%3:%4:%5").arg(QString::fromLatin1(kSourceNames[int(source)])).arg(owner).arg(effect).arg(chain).arg(param);
}

std::vector<Channel> channels(const Timeline& tl, Scope scope, int id)
{
    std::vector<Channel> out;
    std::map<QString, int> ordinals;
    Address base;
    switch (scope) {
    case Scope::Clip: {
        int ti = -1;
        const Clip* c = tl.findClip(id, &ti);
        if (!c)
            return out;
        const bool visual = isVisualTrack(tl.tracks[size_t(ti)].type);
        base.owner = id;
        auto transition = [&](const TransitionSpec& t, Address::Source source, const QString& label) {
            if (const TransitionDef* def = t.type.isEmpty() ? nullptr : findTransition(t.type)) {
                Address a = base;
                a.source = source;
                addDefs(out, def->params, a, label + QStringLiteral(" · ") + def->name, t.type, 0, false);
            }
        };
        transition(c->transitionIn, Address::Source::TransitionIn, QObject::tr("Transition In"));
        if (const GeneratorDef* def = c->isGenerated() ? findGenerator(c->generator.type) : nullptr) {
            Address a = base;
            a.source = Address::Source::Generator;
            addDefs(out, def->params, a, def->name, c->generator.type, 0, true);
        }
        if (visual) {
            Address a = base;
            a.source = Address::Source::PanCrop;
            addDefs(out, panCropParams(), a, QObject::tr("Pan/Crop"), QString(), 0, true);
        }
        base.source = Address::Source::ClipFx;
        addChain(out, c->effects, base, QObject::tr("Event FX") + QStringLiteral(" · "), !visual, true, ordinals);
        transition(c->transitionOut, Address::Source::TransitionOut, QObject::tr("Transition Out"));
        break;
    }
    case Scope::Track:
        for (const auto& t : tl.tracks)
            if (t.id == id) {
                base.source = Address::Source::TrackFx;
                base.owner = id;
                addChain(out, t.effects, base, QString(), !isVisualTrack(t.type), false, ordinals);
            }
        break;
    case Scope::Output:
        base.source = Address::Source::OutputFx;
        addChain(out, tl.outputEffects, base, QString(), false, false, ordinals);
        break;
    case Scope::AudioMaster:
        base.source = Address::Source::MasterFx;
        addChain(out, tl.audioMasterEffects, base, QString(), true, false, ordinals);
        break;
    }
    return out;
}

ParamValue* find(Timeline& tl, const Channel& ch, bool create)
{
    ParamMap* map = paramMap(tl, ch.address);
    if (!map)
        return nullptr;
    auto it = map->find(ch.address.param);
    if (it != map->end())
        return &it->second;
    if (!create)
        return nullptr;
    ParamValue v;
    v.value = ch.def.def;
    v.text = ch.def.defText;
    return &((*map)[ch.address.param] = v);
}

const ParamValue* find(const Timeline& tl, const Channel& ch)
{
    return find(const_cast<Timeline&>(tl), ch, false);
}

TimeUs toTimeline(const Timeline& tl, const Channel& ch, TimeUs key)
{
    if (ch.eventLocal)
        if (const Clip* c = tl.findClip(ch.address.owner))
            return c->start + key;
    return key;
}

TimeUs fromTimeline(const Timeline& tl, const Channel& ch, TimeUs t)
{
    if (ch.eventLocal)
        if (const Clip* c = tl.findClip(ch.address.owner))
            return t - c->start;
    return t;
}

std::pair<TimeUs, TimeUs> keyRange(const Timeline& tl, const Channel& ch)
{
    if (ch.eventLocal)
        if (const Clip* c = tl.findClip(ch.address.owner))
            return {0, c->length};
    return {0, std::numeric_limits<TimeUs>::max() / 4};
}

Times moveKeys(ParamValue& p, const Times& picked, TimeUs delta)
{
    std::vector<Keyframe> moved, rest;
    for (const Keyframe& k : p.keys)
        (picked.count(k.time) ? moved : rest).push_back(k);
    for (Keyframe& k : moved)
        k.time += delta;
    return mergeMoved(p, std::move(moved), std::move(rest));
}

Times scaleKeys(ParamValue& p, const Times& picked, TimeUs pivot, double factor, const std::function<TimeUs(TimeUs)>& snap)
{
    std::vector<Keyframe> moved, rest;
    for (const Keyframe& k : p.keys)
        (picked.count(k.time) ? moved : rest).push_back(k);
    for (Keyframe& k : moved)
        k.time = snap(pivot + TimeUs(std::llround(double(k.time - pivot) * factor)));
    return mergeMoved(p, std::move(moved), std::move(rest));
}

Times reverseKeys(ParamValue& p, const Times& picked)
{
    std::vector<size_t> idx;
    for (size_t i = 0; i < p.keys.size(); ++i)
        if (picked.count(p.keys[i].time))
            idx.push_back(i);
    if (idx.size() < 2)
        return picked;
    const TimeUs lo = p.keys[idx.front()].time, hi = p.keys[idx.back()].time;
    std::vector<Keyframe> old;
    for (size_t i : idx)
        old.push_back(p.keys[i]);
    const size_t n = old.size();
    // New key j is old key n-1-j; the curve between new j and j+1 is the old one between
    // n-2-j and n-1-j, run backwards (eases swap, Bezier handles swap ends).
    for (size_t j = 0; j < n; ++j) {
        Keyframe k = old[n - 1 - j];
        k.time = lo + hi - k.time;
        if (j + 1 < n) {
            const Keyframe& seg = old[n - 2 - j];
            k.interp = seg.interp == Keyframe::Interp::EaseIn ? Keyframe::Interp::EaseOut
                       : seg.interp == Keyframe::Interp::EaseOut ? Keyframe::Interp::EaseIn
                                                                 : seg.interp;
            k.x1 = 1.0 - seg.x2, k.y1 = seg.y2, k.x2 = 1.0 - seg.x1, k.y2 = seg.y1;
        } else {
            k.interp = old.back().interp; // towards whatever follows the span, as before
            k.x1 = old.back().x1, k.y1 = old.back().y1, k.x2 = old.back().x2, k.y2 = old.back().y2;
        }
        p.keys[idx[j]] = k;
    }
    sortKeys(p.keys);
    Times out;
    for (TimeUs t : picked)
        out.insert(t >= lo && t <= hi ? lo + hi - t : t);
    return out;
}

void deleteKeys(ParamValue& p, const Times& picked, TimeUs now, ParamType type)
{
    const double before = p.at(now, type);
    p.keys.erase(std::remove_if(p.keys.begin(), p.keys.end(), [&](const Keyframe& k) { return picked.count(k.time); }), p.keys.end());
    if (p.keys.empty())
        p.value = before;
}

void setInterp(ParamValue& p, const Times& picked, Keyframe::Interp interp)
{
    for (size_t i = 0; i < p.keys.size(); ++i) {
        Keyframe& k = p.keys[i];
        if (!picked.count(k.time))
            continue;
        if (interp == Keyframe::Interp::Bezier && i + 1 < p.keys.size())
            fitBezierHandles(k, p.keys[i + 1]);
        else
            k.interp = interp;
    }
}

void autoTangents(ParamValue& p, const Times& picked)
{
    const size_t n = p.keys.size();
    if (n < 2)
        return;
    auto slope = [&](size_t i) -> double {
        if (i == 0 || i + 1 >= n)
            return 0.0;
        const Keyframe &a = p.keys[i - 1], &k = p.keys[i], &b = p.keys[i + 1];
        const double l = (k.value - a.value) / double(std::max<TimeUs>(1, k.time - a.time));
        const double r = (b.value - k.value) / double(std::max<TimeUs>(1, b.time - k.time));
        if (l * r <= 0)
            return 0.0; // a peak, a dip or flat: no overshoot
        const double s = (b.value - a.value) / double(std::max<TimeUs>(1, b.time - a.time));
        const double limit = 3.0 * std::min(std::abs(l), std::abs(r)); // keeps the curve monotonic
        return std::clamp(s, -limit, limit);
    };
    std::vector<double> slopes(n);
    for (size_t i = 0; i < n; ++i)
        slopes[i] = slope(i);
    for (size_t i = 0; i + 1 < n; ++i) {
        if (!picked.count(p.keys[i].time) && !picked.count(p.keys[i + 1].time))
            continue;
        Keyframe& a = p.keys[i];
        const double dt = double(p.keys[i + 1].time - a.time);
        a.interp = Keyframe::Interp::Bezier;
        a.x1 = 1.0 / 3, a.y1 = slopes[i] * dt / 3.0;
        a.x2 = 2.0 / 3, a.y2 = -slopes[i + 1] * dt / 3.0;
    }
}

void insertKey(ParamValue& p, TimeUs t, ParamType type)
{
    if (keyAt(p.keys, t) != p.keys.end())
        return;
    if (p.keys.empty()) {
        p.addKey(t, p.value);
        return;
    }
    const double v = p.at(t, type);
    auto next = std::upper_bound(p.keys.begin(), p.keys.end(), t, [](TimeUs x, const Keyframe& k) { return x < k.time; });
    Keyframe k;
    k.time = t;
    k.value = v;
    if (next != p.keys.begin()) {
        Keyframe& a = *std::prev(next);
        k.interp = a.interp;
        k.x1 = a.x1, k.y1 = a.y1, k.x2 = a.x2, k.y2 = a.y2;
        if (next != p.keys.end() && a.interp == Keyframe::Interp::Bezier) {
            // Split the cubic where it passes t (de Casteljau), so the curve keeps its shape.
            const Keyframe& b = *next;
            const double f = double(t - a.time) / double(b.time - a.time);
            double lo = 0, hi = 1;
            for (int i = 0; i < 40; ++i) {
                const double mid = (lo + hi) / 2;
                (cubic(mid, 0.0, a.x1, a.x2, 1.0) < f ? lo : hi) = mid;
            }
            const double u = (lo + hi) / 2;
            auto lerp = [&](QPointF p0, QPointF p1) { return p0 + (p1 - p0) * u; };
            const QPointF p0(0, a.value), p1(a.x1, a.value + a.y1), p2(a.x2, b.value + a.y2), p3(1, b.value);
            const QPointF q1 = lerp(p0, p1), m = lerp(p1, p2), r2 = lerp(p2, p3);
            const QPointF q2 = lerp(q1, m), r1 = lerp(m, r2), s = lerp(q2, r1);
            const double sx = std::clamp(s.x(), 1e-9, 1.0 - 1e-9);
            k.value = s.y();
            k.x1 = std::clamp((r1.x() - sx) / (1 - sx), 0.0, 1.0), k.y1 = r1.y() - s.y();
            k.x2 = std::clamp((r2.x() - sx) / (1 - sx), 0.0, 1.0), k.y2 = r2.y() - b.value;
            a.x1 = std::clamp(q1.x() / sx, 0.0, 1.0), a.y1 = q1.y() - a.value;
            a.x2 = std::clamp(q2.x() / sx, 0.0, 1.0), a.y2 = q2.y() - s.y();
        }
    }
    p.keys.insert(next, k);
}

void setValue(ParamValue& p, const Times& picked, double value, const ParamDef& def)
{
    if (def.type == ParamType::Number)
        value = std::clamp(value, def.min, def.max);
    for (Keyframe& k : p.keys)
        if (picked.count(k.time))
            k.value = value;
}

QJsonObject copyKeys(const Timeline& tl, const std::vector<std::pair<Channel, Times>>& picked)
{
    TimeUs base = std::numeric_limits<TimeUs>::max();
    for (const auto& [ch, times] : picked)
        if (!times.empty())
            base = std::min(base, toTimeline(tl, ch, *times.begin()));
    QJsonArray list;
    for (const auto& [ch, times] : picked) {
        const ParamValue* p = find(tl, ch);
        if (!p || times.empty())
            continue;
        std::vector<Keyframe> keys;
        for (Keyframe k : p->keys)
            if (times.count(k.time)) {
                k.time = toTimeline(tl, ch, k.time) - base;
                keys.push_back(k);
            }
        QJsonObject o{{QStringLiteral("address"), ch.address.key()},
                      {QStringLiteral("source"), int(ch.address.source)},
                      {QStringLiteral("type"), ch.effectType},
                      {QStringLiteral("ordinal"), ch.ordinal},
                      {QStringLiteral("param"), ch.address.param}};
        keysToJson(keys, o, QStringLiteral("keys"));
        list.append(o);
    }
    return QJsonObject{{QStringLiteral("format"), QStringLiteral("mvp-keyframes")}, {QStringLiteral("channels"), list}};
}

int pasteKeys(Timeline& tl, const QJsonObject& copied, const std::vector<Channel>& into, const Channel* fallback, TimeUs at)
{
    const QJsonArray list = copied[QStringLiteral("channels")].toArray();
    int pasted = 0;
    for (const auto& item : list) {
        const QJsonObject o = item.toObject();
        const QString param = o[QStringLiteral("param")].toString(), type = o[QStringLiteral("type")].toString();
        const auto source = Address::Source(std::clamp(o[QStringLiteral("source")].toInt(), 0, 7));
        const Channel* target = nullptr;
        for (const Channel& ch : into)
            if (ch.address.key() == o[QStringLiteral("address")].toString())
                target = &ch;
        for (const Channel& ch : into) // the same effect type and setting (in another event, track...)
            if (!target && ch.address.param == param && ch.effectType == type && ch.ordinal == o[QStringLiteral("ordinal")].toInt()
                && (type.isEmpty() ? ch.address.source == source : isFx(ch.address.source) == isFx(source)))
                target = &ch;
        if (!target && list.size() == 1)
            target = fallback;
        ParamValue* p = target ? find(tl, *target, true) : nullptr;
        if (!p)
            continue;
        const auto [lo, hi] = keyRange(tl, *target);
        for (Keyframe k : keysFromJson(o, QStringLiteral("keys"))) {
            k.time = std::clamp(fromTimeline(tl, *target, at + k.time), lo, hi);
            if (target->def.type == ParamType::Number)
                k.value = std::clamp(k.value, target->def.min, target->def.max);
            if (auto it = keyAt(p->keys, k.time); it != p->keys.end())
                *it = k;
            else
                p->keys.insert(std::upper_bound(p->keys.begin(), p->keys.end(), k.time,
                                                [](TimeUs x, const Keyframe& kk) { return x < kk.time; }), k);
            ++pasted;
        }
    }
    return pasted;
}

} // namespace keyops
