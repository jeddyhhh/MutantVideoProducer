#include "core/RandomChain.h"

#include "core/Compositor.h"

#include <QPainter>
#include <algorithm>
#include <cmath>
#include <random>

// The pools below leave out the members an entry doesn't use.
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif

namespace {

enum class Role { Distort, Motion, Glitch, Color, Texture };

// A parameter's range at the Low tier and at the Insane tier; tiers between
// interpolate. Choices: any of `choices` (empty = any index); Bools: chance.
struct Tweak {
    const char* id;
    double lowA, lowB, insaneA, insaneB;
    bool flipSign = false; // either sign
};
struct Pick {
    const char* id;
    std::vector<int> choices;
};
struct Candidate {
    const char* effect;
    Role role;
    int minTier; // 0 Low .. 3 Insane
    std::vector<Tweak> tweaks;
    std::vector<Pick> picks;           // choice params to randomise
    std::vector<const char*> flags;    // bool params to randomise (50%)
    bool hasMix = false;               // the shared glitch Mix / Apply to
};

const std::vector<Candidate>& candidates()
{
    static const std::vector<Candidate> list = {
        // ---- Colour ----
        {"hsl", Role::Color, 0, {{"hue", -25, 25, -180, 180}, {"saturation", 0.1, 0.4, -0.6, 1.0}}},
        {"curves", Role::Color, 0,
         {{"darks", -0.08, 0.0, -0.3, 0.1}, {"lights", 0.0, 0.08, -0.1, 0.3}, {"shadows", 0.0, 0.08, -0.2, 0.3},
          {"blueShadows", -0.08, 0.08, -0.4, 0.4}, {"redHighlights", -0.06, 0.06, -0.4, 0.4}, {"greenMidtones", -0.04, 0.04, -0.3, 0.3}}},
        {"color_wheels", Role::Color, 0,
         {{"liftHue", 0, 360, 0, 360}, {"liftAmount", 0.15, 0.35, 0.5, 1.0}, {"gainHue", 0, 360, 0, 360},
          {"gainAmount", 0.1, 0.3, 0.5, 1.0}, {"gamma", -0.08, 0.08, -0.4, 0.4}}},
        {"tint", Role::Color, 0, {{"amount", 0.15, 0.3, 0.4, 0.8}}},
        {"psychedelic", Role::Color, 1, {{"speed", 30, 120, -720, 720}, {"lumaSpread", 180, 360, 360, 1080}, {"saturation", 1.2, 1.6, 1.5, 3.0}}},
        {"gradient_map", Role::Color, 1, {{"mix", 0.3, 0.6, 0.8, 1.0}, {"cycle", -0.3, 0.3, -2, 2}}, {{"palette", {}}}},
        {"posterize", Role::Color, 1, {{"levels", 10, 14, 2, 5}}},
        {"solarize", Role::Color, 2, {{"threshold", 0.45, 0.7, 0.3, 0.8}}},
        {"channel_swap", Role::Color, 2, {}, {{"order", {1, 2, 3, 4, 5}}}, {"invertR", "invertB"}, true},
        // ---- Texture ----
        {"noise", Role::Texture, 0, {{"amount", 0.05, 0.12, 0.3, 0.6}}, {}, {"monochrome"}},
        {"vignette", Role::Texture, 0, {{"amount", 0.3, 0.5, 0.6, 1.0}, {"size", 0.7, 0.9, 0.4, 0.8}}},
        {"glow", Role::Texture, 0, {{"threshold", 0.6, 0.75, 0.3, 0.5}, {"intensity", 0.5, 1.0, 1.5, 3.0}, {"radius", 10, 30, 30, 80}}},
        {"vhs", Role::Texture, 0,
         {{"noise", 0.05, 0.15, 0.3, 0.7}, {"tracking", 0.2, 0.5, 0.6, 1.0}, {"dropouts", 0.0, 0.2, 0.4, 1.0},
          {"bleed", 4, 10, 15, 40}, {"chromaShift", 2, 5, 8, 20, true}}},
        {"crt", Role::Texture, 0, {{"scanlines", 0.3, 0.5, 0.6, 1.0}, {"curvature", 0.05, 0.12, 0.2, 0.4}, {"lineCount", 270, 480, 90, 200}},
         {{"mask", {}}}},
        {"old_film", Role::Texture, 0,
         {{"scratches", 0.2, 0.5, 0.6, 1.0}, {"dust", 0.2, 0.5, 0.6, 1.0}, {"flicker", 0.1, 0.3, 0.5, 0.9}, {"sepia", 0.2, 0.6, 0.0, 1.0}}},
        {"ghosting", Role::Texture, 0, {{"offset", 10, 30, 60, 200, true}, {"strength", 0.2, 0.35, 0.5, 0.8}, {"echoes", 1, 2, 3, 5}}},
        {"halftone", Role::Texture, 1, {{"cellSize", 6, 10, 12, 32}, {"angle", -45, 45, -90, 90}}, {{"mode", {}}}},
        {"dither", Role::Texture, 1, {{"pixelSize", 2, 3, 4, 10}}, {{"palette", {}}, {"method", {}}}},
        {"tv_static", Role::Texture, 1, {{"amount", 0.1, 0.2, 0.4, 0.8}, {"bars", 0.1, 0.3, 0.4, 0.9}}, {}, {"colorNoise"}},
        {"interlace", Role::Texture, 1, {{"shift", 4, 10, 20, 40, true}}, {{"mode", {}}}},
        {"ascii", Role::Texture, 2, {{"cellSize", 8, 12, 10, 24}}, {{"charset", {}}, {"colorMode", {1, 2, 3}}}},
        {"neon_edges", Role::Texture, 2, {{"strength", 2, 4, 4, 10}, {"background", 0.2, 0.5, 0.0, 0.3}, {"glow", 4, 8, 8, 30}}, {{"colorMode", {}}}},
        {"pixelate", Role::Texture, 2, {{"size", 6, 10, 16, 48}}},
        // ---- Distort ----
        {"shake", Role::Distort, 0, {{"amount", 4, 10, 30, 120}, {"rotation", 0.5, 1, 3, 10}, {"speed", 3, 6, 8, 20}}},
        {"wave_warp", Role::Distort, 1, {{"amplitude", 8, 20, 60, 200, true}, {"wavelength", 200, 400, 60, 300}, {"speed", 0.3, 1, -4, 4}},
         {{"direction", {}}}},
        {"twirl", Role::Distort, 1, {{"angle", 60, 120, 360, 1440, true}, {"radius", 0.6, 0.9, 1.0, 2.0}}},
        {"ripple", Role::Distort, 1, {{"amplitude", 5, 12, 20, 80, true}, {"wavelength", 60, 120, 20, 200}, {"speed", 1, 2, -6, 6}}},
        {"lens_distortion", Role::Distort, 1, {{"amount", 0.2, 0.4, 0.8, 1.6}}},
        {"mirror_symmetry", Role::Distort, 1, {}, {{"mode", {}}}},
        {"radial_blur", Role::Distort, 1, {{"amount", 0.05, 0.1, 0.2, 0.45}}, {{"mode", {}}}},
        {"signal_jitter", Role::Distort, 1, {{"amplitude", 4, 10, 30, 120}, {"roll", 0, 0, -2, 2}}},
        {"vhs_tracking", Role::Distort, 1, {{"distortion", 30, 60, 100, 250}, {"position", 0.3, 0.9, 0.1, 0.9}}},
        {"melt", Role::Distort, 1, {{"amount", 20, 60, 100, 300, true}, {"frequency", 5, 10, 10, 30}, {"speed", 0.3, 1, -3, 3}}, {{"direction", {}}}},
        {"kaleidoscope", Role::Distort, 2, {{"segments", 4, 8, 6, 24}, {"spin", -30, 30, -180, 180}, {"zoom", 0.8, 1.2, 0.5, 2.0}}},
        {"tile", Role::Distort, 2, {{"tilesX", 2, 3, 3, 8}, {"tilesY", 2, 3, 3, 8}, {"scrollX", -0.2, 0.2, -2, 2}}, {}, {"mirror"}},
        {"polar", Role::Distort, 3, {}, {{"mode", {}}}},
        // ---- Motion (frame history) ----
        {"trails", Role::Motion, 2, {{"decay", 0.6, 0.8, 0.85, 0.97}}, {{"mode", {}}}},
        {"video_feedback", Role::Motion, 2,
         {{"zoom", 1.01, 1.03, 1.03, 1.1}, {"rotation", -2, 2, -8, 8}, {"hueShift", -10, 10, -40, 40}, {"decay", 0.7, 0.85, 0.85, 0.95}}},
        {"time_split", Role::Motion, 1, {{"delay", 2, 4, 6, 20}}, {{"order", {}}}, {}, true},
        {"slit_scan", Role::Motion, 3, {{"frames", 10, 20, 20, 60}}, {{"direction", {}}}},
        // ---- Glitch ----
        {"chromatic_aberration", Role::Glitch, 0, {{"amount", 3, 6, 15, 45, true}, {"wobble", 0, 0.3, 0.5, 1.0}}, {{"mode", {}}}, {}, true},
        {"rgb_split", Role::Glitch, 0,
         {{"redX", -6, -2, -60, -20}, {"blueX", 2, 6, 20, 60}, {"redY", -2, 2, -30, 30}, {"jitter", 0, 0, 10, 80}, {"rate", 6, 12, 10, 30}},
         {{"edge", {}}}, {}, true},
        {"pixel_sort", Role::Glitch, 1, {{"lower", 0.45, 0.6, 0.1, 0.4}, {"upper", 0.85, 1.0, 0.8, 1.0}, {"breaks", 0, 0.02, 0, 0.1}},
         {{"direction", {}}, {"sortBy", {}}}, {"animated", "reverse"}, true},
        {"block_glitch", Role::Glitch, 1,
         {{"amount", 0.1, 0.2, 0.4, 0.9}, {"maxShift", 60, 120, 200, 800}, {"colorGlitch", 0.2, 0.4, 0.5, 1.0}, {"noise", 0, 0.1, 0.2, 0.6}},
         {{"colorMode", {}}}, {}, true},
        {"slice_glitch", Role::Glitch, 1, {{"amount", 0.2, 0.35, 0.5, 0.9}, {"maxShift", 40, 100, 150, 600}, {"colorGlitch", 0, 0.2, 0.3, 1.0}},
         {{"direction", {}}}, {}, true},
        {"jpeg_glitch", Role::Glitch, 1, {{"quality", 20, 30, 1, 8}, {"corruption", 0, 0, 0.05, 0.3}}, {{"blockScale", {}}}, {}, true},
        {"datamosh", Role::Glitch, 2, {{"amount", 0.9, 0.95, 0.95, 0.99}, {"motionScale", 1, 2, 1.5, 4}, {"autoClean", 4, 6, 2, 4}},
         {{"mode", {}}}, {}, true},
        {"databend", Role::Glitch, 2, {{"mix", 0.3, 0.5, 0.6, 1.0}, {"feedback", 0.3, 0.5, 0.5, 0.9}}, {{"mode", {}}}, {}, true},
        {"bitcrush", Role::Glitch, 2, {{"bits", 5, 6, 2, 4}, {"gain", 1.1, 1.4, 1.5, 4.0}}, {}, {"wrap"}, true},
        {"pixel_stretch", Role::Glitch, 2,
         {{"position", 0.2, 0.8, 0.1, 0.9}, {"length", 0.3, 0.5, 0.6, 1.0}, {"sweep", 0, 0.2, 0.3, 1.0}, {"jitter", 0, 50, 100, 400}},
         {{"direction", {}}}, {}, true},
        {"stutter", Role::Glitch, 2, {{"fps", 10, 15, 3, 8}, {"freeze", 0, 0.1, 0.2, 0.5}}, {}, {}, true},
        {"strobe", Role::Glitch, 3, {{"rate", 2, 4, 4, 12}, {"duty", 0.1, 0.2, 0.1, 0.3}, {"chance", 0.3, 0.6, 0.6, 1.0}, {"opacity", 0.3, 0.5, 0.5, 0.9}},
         {{"mode", {0, 1, 2, 3, 5}}}, {}, true},
    };
    return list;
}

// How many effects of each role a tier uses: {distort, motion, glitch, color, texture} as (min, max).
struct Recipe {
    std::pair<int, int> distort, motion, glitch, color, texture;
    int minTotal, maxTotal;
};
Recipe recipe(RandomTier tier)
{
    switch (tier) {
    case RandomTier::Low: return {{0, 1}, {0, 0}, {0, 1}, {1, 1}, {0, 1}, 2, 2};
    case RandomTier::Medium: return {{0, 1}, {0, 1}, {1, 1}, {1, 1}, {0, 1}, 3, 3};
    case RandomTier::High: return {{1, 1}, {0, 1}, {1, 2}, {1, 1}, {0, 1}, 4, 5};
    case RandomTier::Insane: return {{1, 2}, {1, 1}, {2, 3}, {1, 2}, {1, 1}, 6, 8};
    }
    return {{0, 1}, {0, 0}, {0, 1}, {1, 1}, {0, 1}, 2, 2};
}

// How much the test picture must (and may) change, and the detail that must be left.
struct Bounds {
    double minChange, maxChange, minDetail;
};
Bounds bounds(RandomTier tier)
{
    switch (tier) {
    case RandomTier::Low: return {3, 40, 20};
    case RandomTier::Medium: return {10, 70, 18};
    case RandomTier::High: return {18, 110, 14};
    case RandomTier::Insane: return {28, 160, 10};
    }
    return {3, 40, 20};
}

// A colourful test picture that moves a little from frame to frame.
QImage testPicture(int frame)
{
    QImage img(192, 108, QImage::Format_ARGB32_Premultiplied);
    QPainter p(&img);
    QLinearGradient g(0, 0, 192, 108);
    g.setColorAt(0, QColor(30, 60, 140));
    g.setColorAt(0.5, QColor(200, 120, 60));
    g.setColorAt(1, QColor(20, 140, 90));
    p.fillRect(img.rect(), g);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    const QColor colors[] = {QColor(240, 220, 60), QColor(230, 40, 90), QColor(60, 220, 240), QColor(250, 250, 250), QColor(10, 10, 20)};
    for (int i = 0; i < 5; ++i) {
        p.setBrush(colors[i]);
        const double x = 20 + i * 34 + 6 * std::sin((frame + i * 3) * 0.6);
        const double y = 30 + 20 * std::cos(i * 1.3) + 4 * std::cos(frame * 0.5 + i);
        p.drawEllipse(QPointF(x, y), 10 + i * 2, 10 + i * 2);
    }
    for (int s = 0; s < 6; ++s)
        p.fillRect(QRectF(0, 78 + s * 5, 192, 2), QColor(255, 255, 255, 60 + s * 25));
    return img;
}

// change: mean difference from the input; detail: deviation of the result's
// brightness; clipped: share of it that is blown out to white or crushed to black.
void measure(const QImage& in, const QImage& out, double& change, double& detail, double& clipped)
{
    double diff = 0, sum = 0, sum2 = 0;
    int n = 0, clip = 0;
    for (int y = 0; y < in.height(); y += 2) {
        const auto* a = reinterpret_cast<const QRgb*>(in.constScanLine(y));
        const auto* b = reinterpret_cast<const QRgb*>(out.constScanLine(y));
        for (int x = 0; x < in.width(); x += 2) {
            diff += (std::abs(qRed(a[x]) - qRed(b[x])) + std::abs(qGreen(a[x]) - qGreen(b[x])) + std::abs(qBlue(a[x]) - qBlue(b[x]))) / 3.0;
            const double l = 0.3 * qRed(b[x]) + 0.59 * qGreen(b[x]) + 0.11 * qBlue(b[x]);
            sum += l;
            sum2 += l * l;
            clip += l > 240 || l < 12;
            ++n;
        }
    }
    change = diff / n;
    const double mean = sum / n;
    detail = std::sqrt(std::max(0.0, sum2 / n - mean * mean));
    clipped = double(clip) / n;
}

const char* kNames[4][2][12] = {
    {{"Soft", "Warm", "Faded", "Dreamy", "Gentle", "Hazy", "Golden", "Velvet", "Quiet", "Pastel", "Silver", "Late"},
     {"Glow", "Film", "Tape", "Haze", "Dusk", "Memory", "Hour", "Light", "Morning", "Polaroid", "Summer", "Bloom"}},
    {{"Neon", "Wobbly", "Retro", "Electric", "Liquid", "Fuzzy", "Analog", "Prism", "Chrome", "Cassette", "Laser", "Midnight"},
     {"Wave", "Signal", "Arcade", "Dream", "Static", "Pulse", "Rewind", "Bloom", "Drive", "Channel", "Grid", "Mirage"}},
    {{"Broken", "Melting", "Feral", "Acid", "Shattered", "Hyper", "Toxic", "Fractured", "Corrupt", "Molten", "Haywire", "Burning"},
     {"Circuit", "Mirror", "Stream", "Vortex", "Data", "Spiral", "Signal", "Lattice", "Engine", "Prism", "Machine", "Orbit"}},
    {{"TOTAL", "MAXIMUM", "Unhinged", "Cosmic", "Nuclear", "Absolute", "Infinite", "Rabid", "Feral", "Terminal", "Supreme", "Interdimensional"},
     {"Meltdown", "Overload", "Collapse", "Rupture", "Frenzy", "Singularity", "Chaos", "Mayhem", "Implosion", "Breakdown", "Apocalypse", "Hysteria"}},
};

// The slowest effects to render (each sorts or re-encodes the whole picture):
// one per chain at most, so even Insane chains play live.
bool heavy(const char* effect)
{
    for (const char* h : {"pixel_sort", "jpeg_glitch", "datamosh"})
        if (QLatin1String(effect) == QLatin1String(h))
            return true;
    return false;
}

RandomChain build(RandomTier tier, std::mt19937& rng, const std::function<int()>& allocId)
{
    const int t = int(tier);
    const double mixT = t / 3.0;
    std::uniform_real_distribution<double> uni(0.0, 1.0);
    auto between = [&](double a, double b) { return a + (b - a) * uni(rng); };
    auto range = [&](int lo, int hi) { return lo + int(rng() % unsigned(hi - lo + 1)); };
    const Recipe r = recipe(tier);
    const Role roles[] = {Role::Distort, Role::Motion, Role::Glitch, Role::Color, Role::Texture};
    const std::pair<int, int> counts[] = {r.distort, r.motion, r.glitch, r.color, r.texture};
    // How many of each role, within the tier's total.
    int want[5];
    int total = 0;
    for (int i = 0; i < 5; ++i)
        total += want[i] = range(counts[i].first, counts[i].second);
    while (total < r.minTotal) {
        const int i = range(0, 4);
        if (want[i] < counts[i].second) {
            ++want[i];
            ++total;
        }
    }
    while (total > r.maxTotal) {
        const int i = range(0, 4);
        if (want[i] > counts[i].first) {
            --want[i];
            --total;
        }
    }
    RandomChain out;
    bool haveHeavy = false;
    for (int ri = 0; ri < 5; ++ri) {
        std::vector<const Candidate*> pool;
        for (const Candidate& c : candidates())
            if (c.role == roles[ri] && c.minTier <= t && findEffect(QString::fromLatin1(c.effect)))
                pool.push_back(&c);
        std::shuffle(pool.begin(), pool.end(), rng);
        int added = 0;
        for (size_t k = 0; k < pool.size() && added < want[ri]; ++k) {
            const Candidate& c = *pool[k];
            if (heavy(c.effect) && haveHeavy)
                continue;
            haveHeavy = haveHeavy || heavy(c.effect);
            ++added;
            const EffectDef* def = findEffect(QString::fromLatin1(c.effect));
            EffectInstance e = makeEffect(*def, allocId());
            auto set = [&](const char* id, double v) {
                const QString key = QString::fromLatin1(id);
                for (const ParamDef& pd : def->params)
                    if (pd.id == key)
                        e.params[key].value = std::clamp(pd.decimals == 0 ? std::round(v) : v, pd.min, pd.max);
            };
            for (const Tweak& tw : c.tweaks) {
                const double a = tw.lowA + (tw.insaneA - tw.lowA) * mixT, b = tw.lowB + (tw.insaneB - tw.lowB) * mixT;
                double v = between(a, b);
                if (tw.flipSign && uni(rng) < 0.5)
                    v = -v;
                set(tw.id, v);
            }
            for (const Pick& pk : c.picks) {
                int count = 0;
                for (const ParamDef& pd : def->params)
                    if (pd.id == QLatin1String(pk.id))
                        count = int(pd.choices.size());
                if (count > 0)
                    set(pk.id, pk.choices.empty() ? range(0, count - 1) : pk.choices[size_t(range(0, int(pk.choices.size()) - 1))]);
            }
            for (const char* f : c.flags)
                set(f, uni(rng) < 0.5 ? 1 : 0);
            if (QLatin1String(c.effect) == QLatin1String("tint")) {
                const QColor col = QColor::fromHsv(range(0, 359), 200, 255);
                e.params[QStringLiteral("color")].value = double(col.rgb() & 0xffffff);
            }
            if (c.hasMix) {
                set("fxMix", between(0.45 + 0.4 * mixT, 0.7 + 0.3 * mixT));
                if (t >= 2 && uni(rng) < 0.2) // now and then only the bright or the dark parts
                    set("fxMask", uni(rng) < 0.5 ? 3 : 4);
            }
            for (const ParamDef& pd : def->params) // new patterns, not the default ones
                if (pd.id == QLatin1String("seed"))
                    e.params[pd.id].value = range(1, 999);
            out.effects.push_back(std::move(e));
        }
    }
    const int ni = t;
    out.name = QStringLiteral("%1 %2").arg(QString::fromLatin1(kNames[ni][0][range(0, 11)]), QString::fromLatin1(kNames[ni][1][range(0, 11)]));
    return out;
}

} // namespace

QString randomTierName(RandomTier tier)
{
    static const char* names[] = {"Low", "Medium", "High", "Insane"};
    return QString::fromLatin1(names[std::clamp(int(tier), 0, 3)]);
}

RandomChain makeRandomChain(RandomTier tier, uint32_t seed, const std::function<int()>& allocId)
{
    std::mt19937 rng(seed);
    const Bounds b = bounds(tier);
    RandomChain best;
    double bestScore = -1e9;
    int localId = 1;
    for (int attempt = 0; attempt < 10; ++attempt) {
        RandomChain c = build(tier, rng, [&] { return localId++; });
        // Render a few frames of the moving test picture (time-based effects need some).
        Compositor comp(false);
        double change = 0, detail = 0, clipped = 0;
        for (int f = 0; f < 4; ++f) {
            const QImage in = testPicture(f);
            const QImage out = comp.renderLive(in, c.effects, TimeUs(f) * 33333, 30.0);
            double ch, de, cl;
            measure(in, out.convertToFormat(QImage::Format_ARGB32_Premultiplied), ch, de, cl);
            change += ch / 4;
            detail += de / 4;
            clipped += cl / 4;
        }
        c.change = change;
        c.detail = detail;
        // In bounds: done. Otherwise keep the one closest to them.
        // (The test picture itself has ~8% pure white and black.)
        const double score = -std::max(0.0, b.minChange - change) - std::max(0.0, change - b.maxChange) - 2 * std::max(0.0, b.minDetail - detail)
            - 100 * std::max(0.0, clipped - 0.35);
        if (score > bestScore) {
            bestScore = score;
            best = std::move(c);
        }
        if (score >= 0)
            break;
    }
    reassignEffectIds(best.effects, allocId);
    return best;
}

QStringList randomChainUnknownParams()
{
    QStringList out;
    for (const Candidate& c : candidates()) {
        const EffectDef* def = findEffect(QString::fromLatin1(c.effect));
        if (!def) {
            out << QString::fromLatin1(c.effect);
            continue;
        }
        auto check = [&](const char* id, bool wantChoice) {
            for (const ParamDef& pd : def->params)
                if (pd.id == QLatin1String(id)) {
                    if (wantChoice && pd.choices.isEmpty())
                        out << QStringLiteral("%1.%2 (no choices)").arg(QLatin1String(c.effect), QLatin1String(id));
                    return;
                }
            out << QStringLiteral("%1.%2").arg(QLatin1String(c.effect), QLatin1String(id));
        };
        for (const Tweak& t : c.tweaks)
            check(t.id, false);
        for (const Pick& p : c.picks)
            check(p.id, true);
        for (const char* f : c.flags)
            check(f, false);
        if (c.hasMix)
            check("fxMix", false);
    }
    return out;
}
