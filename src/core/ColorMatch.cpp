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

QImage eventPicture(const ProjectSnapshot& snap, int clipId, TimeUs t, int beforeEffect, int width)
{
    const Clip* found = snap.timeline.findClip(clipId);
    if (!found)
        return {};
    Clip c = *found;
    if (beforeEffect)
        for (size_t i = 0; i < c.effects.size(); ++i)
            if (c.effects[i].id == beforeEffect) {
                c.effects.resize(i);
                break;
            }
    c.fadeIn = c.fadeOut = 0;
    c.transitionIn = c.transitionOut = {};
    c.muted = false;
    Track track;
    track.id = 1;
    track.clips = {c};
    ProjectSnapshot alone = snap;
    alone.timeline.tracks = {track};
    alone.timeline.outputEffects.clear();
    const QSize size = sizeForWidth(width, snap.settings.width, snap.settings.height, std::max(width, 2) * 4);
    Compositor renderer(false, true);
    return renderer.render(alone, std::clamp(t, c.start, c.end() - 1), size);
}

void setNeutral(EffectInstance& wb, const QColor& picked)
{
    static const char* channel[3] = {"R", "G", "B"};
    const double in[3] = {picked.redF(), picked.greenF(), picked.blueF()};
    int out[3];
    for (int c = 0; c < 3; ++c) {
        auto value = [&](const char* id, double def) {
            const auto it = wb.params.find(QLatin1String(id) + QLatin1String(channel[c]));
            return it == wb.params.end() ? def : it->second.value;
        };
        const double black = value("black", 0), white = value("white", 1);
        out[c] = std::clamp(int(std::lround(255 * std::clamp((in[c] - black) / std::max(1e-3, white - black), 0.0, 1.0))), 1, 255);
    }
    wb.params[QStringLiteral("neutral")].value = double((out[0] << 16) | (out[1] << 8) | out[2]);
}

void autoColour(const QImage& picture, EffectInstance& wb)
{
    Histogram h;
    h.add(picture);
    if (h.pixels < 1)
        return;
    static const char* channel[3] = {"R", "G", "B"};
    double black[3], white[3];
    for (int c = 0; c < 3; ++c) {
        black[c] = std::clamp(level(h.bins[size_t(c)], h.pixels, 0.005), 0.0, 0.5);
        white[c] = std::clamp(level(h.bins[size_t(c)], h.pixels, 0.995), 0.5, 1.0);
        wb.params[QStringLiteral("black") + QLatin1String(channel[c])].value = black[c];
        wb.params[QStringLiteral("white") + QLatin1String(channel[c])].value = white[c];
    }
    // The neutral: the average of the pixels that are nearly grey once stretched (mid tones, little colour), else of
    // all mid tones.
    const QImage img = picture.convertToFormat(QImage::Format_ARGB32);
    double sum[2][3] = {}, count[2] = {};
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x]) < 128)
                continue;
            const double raw[3] = {qRed(row[x]) / 255.0, qGreen(row[x]) / 255.0, qBlue(row[x]) / 255.0};
            double v[3];
            for (int c = 0; c < 3; ++c)
                v[c] = std::clamp((raw[c] - black[c]) / std::max(1e-3, white[c] - black[c]), 0.0, 1.0);
            const double hi = std::max({v[0], v[1], v[2]}), lo = std::min({v[0], v[1], v[2]}), mid = (v[0] + v[1] + v[2]) / 3;
            if (mid < 0.15 || mid > 0.9)
                continue;
            const int set = hi - lo < 0.15 ? 0 : 1;
            for (int c = 0; c < 3; ++c)
                sum[set][c] += v[c];
            ++count[set];
        }
    }
    const int use = count[0] > double(img.width()) * img.height() * 0.02 ? 0 : 1;
    if (count[use] < 1)
        return;
    int rgb[3];
    for (int c = 0; c < 3; ++c)
        rgb[c] = std::clamp(int(std::lround(255 * sum[use][c] / count[use])), 1, 255);
    wb.params[QStringLiteral("neutral")].value = double((rgb[0] << 16) | (rgb[1] << 8) | rgb[2]);
}

int autoColourEvents(Timeline& tl, const ProjectSnapshot& snap, const std::set<int>& ids, TimeUs at, QString* error)
{
    const EffectDef* def = findEffect(QStringLiteral("white_balance"));
    int done = 0;
    for (const Track& t : snap.timeline.tracks)
        if (isVisualTrack(t.type))
            for (const Clip& c : t.clips) {
                if (!ids.count(c.id) || c.isGenerated() || !def)
                    continue;
                // Measured before its own White Balance (if it has one already, it is set again).
                const auto own = std::find_if(c.effects.begin(), c.effects.end(), [](const EffectInstance& e) { return e.type == QLatin1String("white_balance"); });
                const TimeUs when = at >= c.start && at < c.end() ? at : c.start + c.length / 2;
                const QImage picture = eventPicture(snap, c.id, when, own != c.effects.end() ? own->id : 0);
                Clip* dst = tl.findClip(c.id);
                if (picture.isNull() || !dst)
                    continue;
                auto it = std::find_if(dst->effects.begin(), dst->effects.end(), [](const EffectInstance& e) { return e.type == QLatin1String("white_balance"); });
                if (it == dst->effects.end()) {
                    dst->effects.push_back(makeEffect(*def, tl.allocId()));
                    it = dst->effects.end() - 1;
                }
                autoColour(picture, *it);
                ++done;
            }
    if (!done && error)
        *error = QStringLiteral("Select the video events to colour first.");
    return done;
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
    const QSize size = sizeForWidth(w, snap.settings.width, snap.settings.height, std::max(w, 2) * 4);
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
