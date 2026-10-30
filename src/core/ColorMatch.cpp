#include "core/ColorMatch.h"
#include "core/Compositor.h"

#include <algorithm>

namespace colormatch {
namespace {
// The fraction of the pixels below level v (0..1).
double rank(const std::array<double, 256>& bins, double total, double v)
{
    const double x = std::clamp(v, 0.0, 1.0) * 256.0;
    const int i = std::min(255, int(x));
    double run = 0;
    for (int j = 0; j < i; ++j)
        run += bins[size_t(j)];
    return (run + bins[size_t(i)] * (x - i)) / total;
}

// The level (0..1) below which that fraction of the pixels are.
double level(const std::array<double, 256>& bins, double total, double fraction)
{
    const double want = std::clamp(fraction, 0.0, 1.0) * total;
    double run = 0;
    for (int i = 0; i < 256; ++i) {
        if (bins[size_t(i)] > 0 && run + bins[size_t(i)] >= want)
            return (i + (want - run) / bins[size_t(i)]) / 256.0;
        run += bins[size_t(i)];
    }
    return 1.0;
}

bool isMatch(const EffectInstance& e) { return e.type == QLatin1String("color_match"); }
} // namespace

void Histogram::add(const QImage& img)
{
    const QImage argb = img.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < argb.height(); ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(row[x]) < 250) // (what isn't drawn: letterboxing, around a cut-out)
                continue;
            bins[0][size_t(qRed(row[x]))] += 1;
            bins[1][size_t(qGreen(row[x]))] += 1;
            bins[2][size_t(qBlue(row[x]))] += 1;
            pixels += 1;
        }
    }
}

Curves curves(const Histogram& source, const Histogram& reference)
{
    Curves out{};
    for (size_t c = 0; c < 3; ++c) {
        double last = 0;
        for (int k = 0; k < kPoints; ++k) {
            const double x = double(k) / (kPoints - 1);
            const double y = source.pixels > 0 && reference.pixels > 0
                                 ? level(reference.bins[c], reference.pixels, rank(source.bins[c], source.pixels, x))
                                 : x;
            last = std::max(last, std::clamp(y, 0.0, 1.0)); // (never turning a level darker than a lower one)
            out[c][size_t(k)] = last;
        }
    }
    return out;
}

int match(Timeline& tl, const ProjectSnapshot& snap, const std::set<int>& ids, TimeUs reference, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return 0;
    };
    std::vector<int> targets;
    for (const Track& t : snap.timeline.tracks)
        if (isVisualTrack(t.type))
            for (const Clip& c : t.clips)
                if (ids.count(c.id) && !c.isGenerated()) {
                    if (reference >= c.start && reference < c.end())
                        return fail(QStringLiteral("Put the cursor on the shot to match, outside the events being matched."));
                    targets.push_back(c.id);
                }
    if (targets.empty())
        return fail(QStringLiteral("Select the video events to match first."));
    const int w = 320;
    const QSize size(w, std::max(1, int(double(w) * snap.settings.height / std::max(1, snap.settings.width))));
    Histogram ref;
    {
        Compositor renderer;
        ref.add(renderer.render(snap, reference, size));
    }
    if (ref.pixels < 100)
        return fail(QStringLiteral("There is no picture at the cursor to match."));
    const EffectDef* def = findEffect(QStringLiteral("color_match"));
    static const char* channels[] = {"red", "green", "blue"};
    int matched = 0;
    for (int id : targets) {
        // The event alone, as it looks (its own FX, without the match it had), at five times across it.
        Clip c = *snap.timeline.findClip(id);
        std::erase_if(c.effects, isMatch);
        c.fadeIn = c.fadeOut = 0;
        c.transitionIn = c.transitionOut = {};
        c.gain = 1.0f;
        c.muted = false;
        Track track;
        track.id = 1;
        track.clips = {c};
        ProjectSnapshot alone = snap;
        alone.timeline.tracks = {track};
        alone.timeline.outputEffects.clear();
        Compositor renderer(false, true);
        Histogram h;
        for (int k = 0; k < 5; ++k)
            h.add(renderer.render(alone, c.start + TimeUs(double(c.length) * (k + 0.5) / 5), size));
        if (h.pixels < 100)
            continue;
        const Curves cv = curves(h, ref);
        Clip* dst = tl.findClip(id);
        if (!dst || !def)
            continue;
        std::erase_if(dst->effects, isMatch);
        EffectInstance fx = makeEffect(*def, tl.allocId());
        for (size_t ch = 0; ch < 3; ++ch)
            for (int k = 0; k < kPoints; ++k)
                fx.params[QStringLiteral("%1%2").arg(QLatin1String(channels[ch])).arg(k)].value = cv[ch][size_t(k)];
        dst->effects.push_back(fx);
        ++matched;
    }
    if (!matched)
        return fail(QStringLiteral("The selected events have no picture to match."));
    return matched;
}

} // namespace colormatch
