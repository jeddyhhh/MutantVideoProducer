// Keyframe curves, their file format and the Keyframe Editor's edits (core/KeyframeOps.h).
#include "core/Effects.h"
#include "core/Generators.h"
#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include <QCoreApplication>
#include <QJsonArray>
#include <QTextStream>
#include <cmath>

using Interp = Keyframe::Interp;

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    int failures = 0;
    auto check = [&](bool ok, const QString& what) { QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << Qt::endl; failures += !ok; };
    auto key = [](TimeUs t, double v, Interp i = Interp::Linear) { Keyframe k; k.time = t; k.value = v; k.interp = i; return k; };
    const TimeUs S = kUsPerSecond;

    // ---- Curves ----
    bool ends = true, names = true;
    for (int i = 0; i < kKeyInterps; ++i) {
        Keyframe a = key(0, 2, Interp(i)), b = key(S, 7);
        if (Interp(i) == Interp::Bezier)
            a.y1 = 3, a.y2 = -4;
        ends &= std::abs(keyframeValue(a, b, 0) - 2) < 1e-9 && std::abs(keyframeValue(a, b, 1) - 7) < 1e-9;
        names &= keyInterpFromId(keyInterpId(Interp(i))) == Interp(i) && !keyInterpName(Interp(i)).isEmpty();
    }
    check(ends, "every curve starts on its keyframe's value and ends on the next one's");
    check(names, "curve ids round-trip (the graphics engine's names)");
    bool monotonic = true;
    for (Interp i : {Interp::Linear, Interp::Smooth, Interp::EaseIn, Interp::EaseOut, Interp::EaseInOut}) {
        double last = -1;
        for (int s = 0; s <= 100; ++s) {
            const double v = keyframeValue(key(0, 0, i), key(S, 1), s / 100.0);
            monotonic &= v >= last - 1e-12;
            last = v;
        }
    }
    check(monotonic, "Linear, Smooth and the eases never turn back");
    const double inMid = keyframeValue(key(0, 0, Interp::EaseIn), key(S, 1), 0.5), outMid = keyframeValue(key(0, 0, Interp::EaseOut), key(S, 1), 0.5);
    check(inMid < 0.2 && outMid > 0.8, QStringLiteral("Ease In starts slowly (%1 halfway), Ease Out arrives slowly (%2)").arg(inMid).arg(outMid));
    double elasticPeak = 0;
    for (int s = 0; s <= 200; ++s)
        elasticPeak = std::max(elasticPeak, keyframeValue(key(0, 0, Interp::Elastic), key(S, 1), s / 200.0));
    check(elasticPeak > 1.05, QStringLiteral("Elastic overshoots (to %1) and settles").arg(elasticPeak));
    bool fits = true;
    for (Interp i : {Interp::Linear, Interp::Smooth, Interp::EaseIn, Interp::EaseOut}) {
        Keyframe a = key(0, -3, i), b = key(S, 5), bez = a;
        fitBezierHandles(bez, b);
        for (int s = 0; s <= 50; ++s)
            fits &= bez.interp == Interp::Bezier && std::abs(keyframeValue(a, b, s / 50.0) - keyframeValue(bez, b, s / 50.0)) < 1e-6;
    }
    check(fits, "Bezier handles fitted to Linear, Smooth, Ease In and Ease Out draw the same curve");
    {
        Keyframe a = key(0, 5, Interp::Bezier), b = key(S, 5);
        a.y1 = 4, a.y2 = 4;
        check(keyframeValue(a, b, 0.5) > 7, "a Bezier curve between equal values can still bulge (handles in setting units)");
    }
    {
        ParamValue c;
        c.keys = {key(0, double(0x000000), Interp::Elastic), key(S, double(0xffffff))};
        bool inRange = true;
        for (int s = 0; s <= 100; ++s) {
            const auto v = uint32_t(c.at(s * S / 100, ParamType::Color));
            inRange &= v <= 0xffffffu;
        }
        check(inRange, "an overshooting colour stays a colour (each channel clamped)");
    }

    // ---- File format ----
    {
        ParamMap m;
        Keyframe b = key(2 * S, 9, Interp::Bezier);
        b.x1 = 0.2, b.y1 = 3, b.x2 = 0.9, b.y2 = -1;
        m["x"].keys = {key(0, 1, Interp::EaseOut), b, key(3 * S, 4, Interp::Elastic)};
        const QJsonObject j = paramsToJson(m);
        const ParamMap back = paramsFromJson(j);
        check(back == m, "keyframes with every kind of curve and Bezier handles survive saving and loading");
        bool triples = true;
        for (const auto& k : j["x"].toObject()["keys"].toArray())
            triples &= k.toArray().size() == 3;
        check(triples && j["x"].toObject()["bez"].toArray().size() == 1,
              "keyframes stay [time, value, curve] (older builds still load them; handles sit beside them)");
        QJsonObject future = j;
        QJsonObject x = future["x"].toObject();
        QJsonArray list;
        list.append(QJsonArray{QStringLiteral("0"), 1.0, 42});
        x["keys"] = list;
        future["x"] = x;
        const ParamMap newer = paramsFromJson(future);
        check(newer.at("x").keys.size() == 1 && newer.at("x").keys.front().interp == Interp::Smooth, "a curve from a newer build loads as Smooth");
        Timeline tl = defaultTimeline();
        const int id = ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("solid_white")), 0, -1, FrameRate{});
        Envelope& e = tl.findClip(id)->velocity;
        e.active = true;
        e.points = {key(0, 1, Interp::Bezier), key(S, 2)};
        e.points[0].y1 = 0.5;
        const Timeline loaded = nestedTimelineFromJson(nestedTimelineToJson(tl));
        check(loaded.findClip(id) && loaded.findClip(id)->velocity == e, "envelope points keep their Bezier handles");
    }

    // ---- Speed ramps (envelope integrals) ----
    {
        Envelope e;
        e.active = true;
        e.points = {key(0, 1, Interp::EaseIn), key(S, 2)};
        check(std::abs(e.integral(double(S)) - 1.25 * S) < 2, QStringLiteral("an Ease In speed ramp covers the right span (%1 us of 1250000)").arg(e.integral(double(S)), 0, 'f', 1));
        Envelope b = e;
        b.points[0].interp = Interp::Bezier;
        b.points[0].y1 = 1.0, b.points[0].y2 = 0;
        double sum = 0;
        for (int i = 0; i < 100000; ++i)
            sum += b.at(TimeUs((i + 0.5) * S / 100000)) * double(S) / 100000;
        check(std::abs(b.integral(double(S)) - sum) < 5, "a Bezier speed ramp's span matches the curve");
        Envelope back;
        back.active = true;
        back.points = {key(0, 1, Interp::Elastic), key(S, -1)};
        check(back.maxIntegral(double(S)) >= back.integral(double(S)) && back.maxIntegral(double(S)) > 0,
              "the furthest point of a ramp that turns backwards is found on any curve");
    }

    // ---- Edits ----
    {
        ParamValue p;
        p.keys = {key(0, 0), key(S, 1), key(2 * S, 2), key(3 * S, 3)};
        auto moved = keyops::moveKeys(p, {0, S}, 2 * S);
        check(p.keys.size() == 2 && moved == keyops::Times{2 * S, 3 * S} && p.keys[0].value == 0 && p.keys[1].value == 1,
              "moved keyframes replace the ones they land on");
        ParamValue q;
        q.keys = {key(S, 0), key(2 * S, 1), key(3 * S, 2)};
        auto scaled = keyops::scaleKeys(q, {S, 2 * S, 3 * S}, S, 2.0, [](TimeUs t) { return t; });
        check(scaled == keyops::Times{S, 3 * S, 5 * S} && q.keys.back().value == 2, "spreading keyframes out doubles their spacing");
        ParamValue r;
        r.keys = {key(0, 0, Interp::EaseIn), key(S, 4, Interp::Bezier), key(3 * S, 1)};
        r.keys[1].x1 = 0.1, r.keys[1].y1 = 2, r.keys[1].x2 = 0.7, r.keys[1].y2 = -1;
        const ParamValue before = r;
        keyops::reverseKeys(r, {0, S, 3 * S});
        bool mirrored = true;
        for (int s = 0; s <= 60; ++s) {
            const TimeUs t = s * 3 * S / 60;
            mirrored &= std::abs(r.at(3 * S - t, ParamType::Number) - before.at(t, ParamType::Number)) < 1e-4;
        }
        check(mirrored, "reversing keyframes plays the same curve backwards (eases and handles mirrored)");
        ParamValue peak;
        peak.keys = {key(0, 0), key(S, 10), key(2 * S, 0), key(3 * S, 5), key(4 * S, 8)};
        keyops::autoTangents(peak, {0, S, 2 * S, 3 * S, 4 * S});
        double top = 0, last = -1;
        bool rising = true;
        for (int s = 0; s <= 400; ++s) {
            const double v = peak.at(s * 4 * S / 400, ParamType::Number);
            if (s <= 100) top = std::max(top, v);
            if (s >= 200) rising &= v >= last - 1e-9, last = v;
        }
        check(top <= 10 + 1e-9 && rising, QStringLiteral("smooth handles don't overshoot a peak (%1) and keep a climb climbing").arg(top));
        ParamValue split;
        split.keys = {key(0, 0, Interp::Bezier), key(S, 6)};
        split.keys[0].x1 = 0.6, split.keys[0].y1 = 5, split.keys[0].x2 = 0.2, split.keys[0].y2 = 3;
        const ParamValue unsplit = split;
        keyops::insertKey(split, 3 * S / 10, ParamType::Number);
        bool same = split.keys.size() == 3;
        for (int s = 0; s <= 100; ++s)
            same &= std::abs(split.at(s * S / 100, ParamType::Number) - unsplit.at(s * S / 100, ParamType::Number)) < 1e-3;
        check(same, "a keyframe added on a Bezier curve keeps the curve's shape");
        ParamValue last1;
        last1.keys = {key(0, 0), key(S, 10)};
        keyops::deleteKeys(last1, {0, S}, S / 2, ParamType::Number);
        check(last1.keys.empty() && std::abs(last1.value - 5) < 1e-9, "deleting every keyframe keeps the value at the cursor");
        ParamValue ease;
        ease.keys = {key(0, 0), key(S, 1), key(2 * S, 0)};
        keyops::setInterp(ease, {0}, Interp::Bezier);
        check(ease.keys[0].interp == Interp::Bezier && std::abs(ease.at(S / 4, ParamType::Number) - 0.25) < 1e-6,
              "switching to Bezier starts from the curve the keyframe had");
    }

    // ---- Settings of an event, found by what they belong to ----
    {
        Timeline tl = defaultTimeline();
        const FrameRate fps;
        const int a = ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("solid_white")), S, -1, fps);
        const int b = ops::insertGenerated(tl, *findGeneratorPreset(QStringLiteral("solid_white")), 10 * S, -1, fps);
        for (int id : {a, b}) {
            Clip* c = tl.findClip(id);
            c->effects.push_back(makeEffectOfType(QStringLiteral("gaussian_blur"), tl.allocId()));
            EffectInstance rack = makeRack(tl.allocId());
            rack.chains.push_back(makeRackChain(false, QStringLiteral("Chain 1"), {makeEffectOfType(QStringLiteral("vignette"), tl.allocId())}));
            c->effects.push_back(rack);
        }
        const auto chans = keyops::channels(tl, keyops::Scope::Clip, a);
        auto count = [&](keyops::Address::Source s) { return std::count_if(chans.begin(), chans.end(), [&](const keyops::Channel& c) { return c.address.source == s; }); };
        check(count(keyops::Address::Source::PanCrop) > 0 && count(keyops::Address::Source::Generator) > 0 && count(keyops::Address::Source::ClipFx) > 3,
              QStringLiteral("an event lists Pan/Crop, its generated media and its FX with racks (%1 settings)").arg(chans.size()));
        const auto blur = std::find_if(chans.begin(), chans.end(), [](const keyops::Channel& c) { return c.effectType == "gaussian_blur"; });
        const auto pan = std::find_if(chans.begin(), chans.end(), [](const keyops::Channel& c) { return c.address.source == keyops::Address::Source::PanCrop; });
        check(keyops::find(tl, *pan) == nullptr && keyops::find(tl, *pan, true) != nullptr, "a Pan/Crop setting not stored yet is added when it gets keyframes");
        ParamValue* bp = keyops::find(tl, *blur, true);
        bp->keys = {key(S, 1), key(2 * S, 3)};
        check(keyops::toTimeline(tl, *blur, S) == 2 * S && keyops::fromTimeline(tl, *blur, 2 * S) == S, "event FX keyframes count from the event's start");
        tl.findClip(a)->effects.insert(tl.findClip(a)->effects.begin(), makeEffectOfType(QStringLiteral("hsl"), tl.allocId()));
        check(keyops::find(tl, *blur) == bp || (keyops::find(tl, *blur) && keyops::find(tl, *blur)->keys.size() == 2),
              "a setting is still found after its effect moves in the chain");
        const QJsonObject copied = keyops::copyKeys(tl, {{*blur, {S, 2 * S}}});
        const auto other = keyops::channels(tl, keyops::Scope::Clip, b);
        const int pasted = keyops::pasteKeys(tl, copied, other, nullptr, 12 * S);
        const auto otherBlur = std::find_if(other.begin(), other.end(), [&](const keyops::Channel& c) { return c.effectType == "gaussian_blur" && c.address.param == blur->address.param; });
        const ParamValue* ob = keyops::find(tl, *otherBlur);
        check(pasted == 2 && ob && ob->keys.size() == 2 && ob->keys[0].time == 2 * S && ob->keys[1].value == 3,
              "keyframes pasted into another event land on the same effect's setting at the cursor");
    }

    QTextStream(stdout) << (failures ? "FAIL" : "DONE") << "  keyframes: " << failures << " failure(s)" << Qt::endl;
    return failures;
}
