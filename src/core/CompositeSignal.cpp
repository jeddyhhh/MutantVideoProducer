#include "core/CompositeSignal.h"

#include "core/EffectHelpers.h"
#include "core/NodeGraph.h"
#include "core/Parallel.h"

#include <QPainter>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <numbers>

namespace composite {

const Standard& ntsc()
{
    static const Standard s{"NTSC", false, 315e6 / 22.0, 910, 525, 67, 76, 112, 150, 902, 22, 480};
    return s;
}

const Standard& pal()
{
    // 4 x 4.43361875 MHz; 64 us a line; sync 4.7 us, burst from 5.6 us (10 cycles), picture from 10.5 us for 52 us.
    static const Standard s{"PAL", true, 17734475.0, 1135, 625, 83, 99, 139, 186, 1108, 23, 576};
    return s;
}

namespace {

using Inputs = std::vector<const QImage*>;
using fx::choice;
using fx::num;
using std::numbers::pi;

constexpr int kVsyncFirst = 3, kVsyncEnd = 6; // the lines of broad pulses
constexpr int kFirstBurstLine = 9;
constexpr float kBurst = 20.0f;               // the burst's amplitude (IRE)

// The subcarrier at a sample: four samples a cycle, so sin and cos are 0 or +-1. A frame is 2 (NTSC) or 3 (PAL)
// samples past a whole number of cycles: the phase runs on from frame to frame (dot crawl).
constexpr float kSin[4] = {0, 1, 0, -1};
constexpr float kCos[4] = {1, 0, -1, 0};
inline int phaseOf(const Standard& S, int64_t frame, int64_t n) // (two's complement: right for n < 0 too)
{
    return int((frame * (S.samples() & 3) + n) & 3);
}
// PAL's V switch: +1 or -1, line by line (and on from frame to frame: a frame is an odd number of lines).
inline int palSign(const Standard& S, int64_t frame, int64_t n)
{
    const int64_t line = n >= 0 ? n / S.line : -((-n + S.line - 1) / S.line);
    return ((frame * S.lines + line) & 1) ? -1 : 1;
}

inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

// Each value the mean of `len` around it (the ends repeated).
void boxFilter(std::vector<float>& v, int len, std::vector<double>& sums)
{
    const int n = int(v.size());
    if (len <= 1 || n == 0)
        return;
    const int before = len / 2, after = len - 1 - before;
    sums.assign(size_t(n) + 1, 0.0);
    for (int i = 0; i < n; ++i)
        sums[size_t(i) + 1] = sums[size_t(i)] + v[size_t(i)];
    const double first = v.front(), last = v.back();
    for (int i = 0; i < n; ++i) {
        const int lo = i - before, hi = i + after; // inclusive
        const int a = std::max(lo, 0), b = std::min(hi, n - 1);
        double s = sums[size_t(b) + 1] - sums[size_t(a)];
        s += first * std::max(0, -lo) + last * std::max(0, hi - (n - 1));
        v[size_t(i)] = float(s / len);
    }
}

int64_t frameOf(const nodes::NodeContext& c) { return frameIndex(c.render.timelineTime, c.render.fps); }

Signal blank(const Standard& S, int64_t frame) { return Signal{&S, std::vector<float>(size_t(S.samples()), 0.0f), frame}; }

// A signal like `in` (its standard and frame), its samples to be filled.
Signal like(const Signal& in)
{
    Signal out;
    out.std = in.std;
    out.frame = in.frame;
    out.v.resize(in.v.size());
    return out;
}

ParamDef ohms(const char* id, const char* name, double min, double max, double def)
{
    ParamDef p = num(id, name, min, max, def, 0, " Ω");
    p.logScale = true;
    return p;
}

} // namespace

int64_t frameIndex(int64_t timelineTime, double fps) { return std::llround(double(timelineTime) * fps / 1e6); }

int chromaBox(const Standard& S, double chromaMHz) { return std::clamp(int(std::lround(S.rate / (2e6 * std::max(0.1, chromaMHz)))), 1, 64); }

void delayTable(const Standard& S, const Params& p, double seconds, std::vector<float>& table)
{
    const double perUs = S.rate / 1e6;
    const double base = p.num(QStringLiteral("lines")) * S.line + p.num(QStringLiteral("delay")) * perUs
                        + p.num(QStringLiteral("drift")) * seconds * S.line;
    const double wobble = p.num(QStringLiteral("wobble")) * perUs, rate = p.num(QStringLiteral("wobbleHz"));
    const int64_t count = S.samples();
    table.resize(size_t(2 * S.lines));
    for (int line = 0; line < S.lines; ++line) {
        const double at = seconds + double(line) * S.line / S.rate; // (when the line comes)
        const double d = base + wobble * std::sin(2 * std::numbers::pi * rate * at);
        // (the same delay along the line: one whole-sample offset and one fraction for all of it)
        const double start = double(int64_t(line) * S.line) - d, whole = std::floor(start);
        table[size_t(2 * line)] = float((int64_t(whole) % count + count) % count);
        table[size_t(2 * line + 1)] = float(start - whole);
    }
}

double rcTau(const Params& p, bool high)
{
    static const double kLow[] = {47e-12, 220e-12, 1e-9, 4.7e-9, 22e-9}, kHigh[] = {1e-9, 10e-9, 100e-9, 1e-6, 10e-6, 100e-6};
    const int c = p.choice(QStringLiteral("capacitor"));
    return p.num(QStringLiteral("resistance")) * (high ? kHigh[std::clamp(c, 0, 5)] : kLow[std::clamp(c, 0, 4)]);
}

uint32_t noiseSeed(const Params& p, int64_t frame)
{
    return uint32_t(p.num(QStringLiteral("seed"))) * 2654435761u + uint32_t(frame) * 40503u;
}

const Standard& generatorStandard(const Params& p) { return p.choice(QStringLiteral("standard")) == 1 ? pal() : ntsc(); }

QImage generatorPicture(const Params& p)
{
    QImage picture(64, 48, QImage::Format_ARGB32_Premultiplied);
    picture.fill(Qt::black);
    const int kind = p.choice(QStringLiteral("picture"));
    if (kind == 1) {
        QPainter painter(&picture);
        const QColor bars[] = {QColor(191, 191, 191), QColor(191, 191, 0), QColor(0, 191, 191), QColor(0, 191, 0),
                               QColor(191, 0, 191), QColor(191, 0, 0), QColor(0, 0, 191)};
        for (int i = 0; i < 7; ++i)
            painter.fillRect(QRect(i * 64 / 7, 0, 64 / 7 + 1, 48), bars[i]);
    } else if (kind == 2) {
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                picture.setPixel(x, y, qRgb(x * 4, x * 4, x * 4));
    }
    return picture;
}

double generatorSlip(const Params& p, const Standard& S, double seconds)
{
    return p.num(QStringLiteral("lineDrift")) * seconds * S.rate / 1e6 + p.num(QStringLiteral("frameDrift")) * seconds * S.line;
}

// ---- Encoder ------------------------------------------------------------------------------------------------------

Signal encode(const QImage& picture, int64_t frame, const EncodeOptions& o)
{
    const Standard& S = *o.std;
    Signal s = blank(S, frame);
    const QImage pic = picture.isNull() ? QImage()
                       : picture.format() == QImage::Format_ARGB32_Premultiplied ? picture
                                                                                  : picture.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    const float black = o.setup && !S.pal ? 7.5f : 0.0f, range = 100.0f - black, sat = float(o.saturation);
    const int box = chromaBox(S, o.chromaMHz);
    const int width = S.activeWidth();
    // Where each sample of a line falls in the picture (the active line spans its width).
    std::vector<int> x0(static_cast<size_t>(width)), x1(static_cast<size_t>(width));
    std::vector<float> fxs(static_cast<size_t>(width));
    const int w = pic.width(), h = pic.height();
    for (int x = 0; x < width && w > 0; ++x) {
        const float sx = std::clamp((float(x) + 0.5f) * float(w) / float(width) - 0.5f, 0.0f, float(w - 1));
        x0[size_t(x)] = int(sx);
        x1[size_t(x)] = std::min(x0[size_t(x)] + 1, w - 1);
        fxs[size_t(x)] = sx - float(x0[size_t(x)]);
    }
    const float burstU = S.pal ? -kBurst / float(std::numbers::sqrt2) : -kBurst, burstV = S.pal ? kBurst / float(std::numbers::sqrt2) : 0.0f;
    parallelFor(S.lines, [&](int l0, int l1) {
        std::vector<float> Y(static_cast<size_t>(width)), U(static_cast<size_t>(width)), V(static_cast<size_t>(width));
        std::vector<double> sums;
        for (int line = l0; line < l1; ++line) {
            float* row = s.v.data() + size_t(line) * S.line;
            const bool vsync = line >= kVsyncFirst && line < kVsyncEnd; // broad pulses, a serration at each line's end
            for (int x = 0; x < S.line; ++x)
                row[x] = (vsync ? x < S.line - S.syncEnd : x < S.syncEnd) ? -40.0f : 0.0f;
            if (line < kFirstBurstLine)
                continue;
            const int64_t base = int64_t(line) * S.line;
            const float sign = S.pal ? float(palSign(S, frame, base)) : 1.0f;
            for (int x = S.burstStart; x < S.burstEnd; ++x) { // NTSC on the -U axis (180 degrees), PAL swinging 135/225
                const int k = phaseOf(S, frame, base + x);
                row[x] = burstU * kSin[k] + sign * burstV * kCos[k];
            }
            const int a = line - S.firstActive;
            if (a < 0 || a >= S.activeLines || pic.isNull())
                continue;
            const float sy = std::clamp((float(a) + 0.5f) * float(h) / float(S.activeLines) - 0.5f, 0.0f, float(h - 1));
            const int y0 = int(sy), y1 = std::min(y0 + 1, h - 1);
            const float fy = sy - float(y0);
            const auto* r0 = reinterpret_cast<const uint32_t*>(pic.constScanLine(y0));
            const auto* r1 = reinterpret_cast<const uint32_t*>(pic.constScanLine(y1));
            for (int x = 0; x < width; ++x) {
                const uint32_t p[4] = {r0[x0[size_t(x)]], r0[x1[size_t(x)]], r1[x0[size_t(x)]], r1[x1[size_t(x)]]};
                const float fx = fxs[size_t(x)];
                const float wt[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                float c[3] = {0, 0, 0};
                for (int k = 0; k < 4; ++k) {
                    c[0] += wt[k] * float((p[k] >> 16) & 0xff);
                    c[1] += wt[k] * float((p[k] >> 8) & 0xff);
                    c[2] += wt[k] * float(p[k] & 0xff);
                }
                const float r = c[0] / 255.0f, g = c[1] / 255.0f, b = c[2] / 255.0f; // (premultiplied: over black)
                const float y = 0.299f * r + 0.587f * g + 0.114f * b;
                Y[size_t(x)] = black + range * y;
                U[size_t(x)] = range * 0.492f * (b - y) * sat;
                V[size_t(x)] = range * 0.877f * (r - y) * sat * sign;
            }
            boxFilter(U, box, sums); // (colour is narrow)
            boxFilter(V, box, sums);
            for (int x = 0; x < width; ++x) {
                const int k = phaseOf(S, frame, base + S.activeStart + x);
                row[S.activeStart + x] = Y[size_t(x)] + U[size_t(x)] * kSin[k] + V[size_t(x)] * kCos[k];
            }
        }
    });
    return s;
}

QImage view(const Signal& s, QSize size)
{
    const Standard& S = *s.std;
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    if (size.isEmpty())
        return out;
    const int w = size.width(), h = size.height();
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(out.scanLine(y));
            const float* line = s.v.data() + size_t(std::min(S.lines - 1, y * S.lines / h)) * S.line;
            for (int x = 0; x < w; ++x) {
                const int g = int(clamp01((line[std::min(S.line - 1, x * S.line / w)] + 40.0f) / 140.0f) * 255.0f + 0.5f);
                row[x] = 0xff000000u | uint32_t(g) << 16 | uint32_t(g) << 8 | uint32_t(g);
            }
        }
    });
    return out;
}

namespace {

// ---- The TV ---------------------------------------------------------------------------------------------------

// The TV's beam and its circuits' memory: it runs on from frame to frame (the stream of samples is one long line).
struct Beam {
    int64_t lineStart = 0; // where the line being drawn began (a sample of this frame; < 0: of the one before)
    int lineY = 0;         // its line, counted from the last vertical retrace
    int y = 0;
    int lowRun = 0;        // samples below the sync slice in a row
    float vInt = 0;        // the vertical sync integrator: an RC on the sliced sync (how much of the time it is low)
    float clamp = 0;       // the back porch's level (DC restore), taken on lines a sync pulse started
    float tip = -40;       // the sync tips' level (the lowest the signal goes, creeping back up between them)
    float smooth = 0;      // the signal through the sync separator's input filter (colour and ringing taken off)
    double porch = 0;
    int porchN = 0;
    double bSin = 0, bCos = 0; // the burst, against the TV's own subcarrier
    float hue = 0;         // the colour oscillator's phase error (locked to the burst)
    float acc = 1;         // colour gain from the burst's level
    bool colour = false;   // the colour killer: off without a burst
};

struct TvSettings {
    int hFree = 0;          // the line oscillator's own period (samples)
    int vFree = 0;          // the frame oscillator's (lines)
    int capture = 0;        // a sync pulse resets the line after this many samples
    int vCapture = 0;       // lines before a vertical sync is taken
    float slice = 10;       // sync: at least this far below the back porch
    float contrast = 1, brightness = 0, colour = 1, tint = 0;
    bool comb = false;
};

struct Tv : EffectState {
    const Standard* std = nullptr;
    Beam now, atStart;
    std::vector<float> tail, tailAtStart; // the previous frame's last samples (three lines)
    int64_t lastFrame = std::numeric_limits<int64_t>::min();
};

// The CPU's TV: the beam (tvScan's) and the picture it draws on.
struct CpuTv : EffectState {
    std::unique_ptr<EffectState> beam;
    QImage raster, rasterAtStart; // activeWidth x activeLines
};

constexpr int kTrigger = 8;         // samples below the slice that make a sync pulse
constexpr float kVThreshold = 0.6f; // the integrator's level that starts a vertical retrace (the 2nd broad pulse)
constexpr int kVLine = 4;           // the line a vertical retrace is in

inline float wrapAngle(float a)
{
    while (a > float(pi))
        a -= float(2 * pi);
    while (a < -float(pi))
        a += float(2 * pi);
    return a;
}

void endLine(const Standard& S, Beam& b, int64_t next, bool synced, float minDepth, std::vector<TvLine>& lines)
{
    // The clamp from a line a sync pulse started (one the oscillator ended has its "porch" anywhere), and not from a
    // vertical sync line's (its porch is inside a broad pulse, down at the tips).
    if (synced && b.porchN >= 8)
        if (const float porch = float(b.porch / b.porchN); porch > b.tip + minDepth)
            b.clamp = porch;
    const double amplitude = 2.0 * std::hypot(b.bSin, b.bCos) / (S.burstEnd - S.burstStart);
    // PAL: the burst swings to 135 or 225 degrees, telling the line's V switch (its cos part's sign).
    const int sign = S.pal && b.bCos < 0 ? -1 : 1;
    if (amplitude > 5.0) {
        const double expected = S.pal ? pi - sign * pi / 4 : pi;
        const float e = wrapAngle(float(std::atan2(b.bCos, b.bSin) - expected));
        b.hue = wrapAngle(b.hue + 0.3f * wrapAngle(e - b.hue));
        b.acc += 0.25f * (float(std::min(8.0, double(kBurst) / amplitude)) - b.acc);
        b.colour = true;
    } else {
        b.colour = false;
    }
    lines.push_back({b.lineStart, next, b.lineY, b.clamp, b.hue, b.acc, b.colour, sign});
    b.lineStart = next;
    b.lineY = ++b.y;
    b.porch = b.bSin = b.bCos = 0;
    b.porchN = 0;
}

// The beam through a frame's samples: where each line starts and ends (sync, or the oscillators running free).
void scan(Beam& b, const Signal& s, const TvSettings& set, std::vector<TvLine>& lines)
{
    const Standard& S = *s.std;
    const float* v = s.v.data();
    const float vRate = 1.0f / float(S.line); // (the vertical integrator's time constant: a line)
    const int64_t n1 = S.samples();
    for (int64_t n = 0; n < n1; ++n) {
        const float x = v[n];
        b.smooth += 0.2f * (x - b.smooth); // (an RC of about 0.3 us: sync is sliced from this)
        const float sx = b.smooth;
        b.tip = sx < b.tip ? sx : b.tip + 0.002f; // (back up ~2 IRE a line)
        // Sync is sliced halfway between its tips and the back porch (a stronger or weaker signal too), and at
        // least Sync sensitivity below the porch: sync weaker than that isn't seen.
        const bool low = sx < b.clamp - std::max(set.slice, 0.5f * (b.clamp - b.tip));
        b.lowRun = low ? b.lowRun + 1 : 0;
        b.vInt += ((low ? 1.0f : 0.0f) - b.vInt) * vRate;
        const int64_t hp = n - b.lineStart;
        if (hp >= S.burstStart && hp < S.burstEnd) {
            const int k = phaseOf(S, s.frame, n);
            b.bSin += x * kSin[k];
            b.bCos += x * kCos[k];
        } else if (hp >= S.burstEnd + 4 && hp < S.activeStart - 4) {
            b.porch += x;
            ++b.porchN;
        }
        if (b.lowRun == kTrigger && hp + 1 > set.capture)
            endLine(S, b, n + 1 - kTrigger, true, set.slice, lines);
        else if (hp + 1 >= set.hFree + kTrigger) // (free: a sync pulse it waited for comes kTrigger late)
            endLine(S, b, n + 1, false, set.slice, lines);
        if (b.vInt > kVThreshold && b.y > set.vCapture) {
            b.y = kVLine;
            b.vInt = 0;
        } else if (b.y >= set.vFree) {
            b.y = 0;
        }
    }
}

// The lines the beam drew, decoded into the raster (each row from the last line drawn on it).
void draw(QImage& raster, const Signal& s, const TvFrame& f)
{
    const Standard& S = *s.std;
    const int width = S.activeWidth();
    const std::vector<TvLine>& lines = f.lines;
    std::vector<int> rowLine(size_t(S.activeLines), -1);
    for (size_t i = 0; i < lines.size(); ++i)
        if (const int row = lines[i].y - S.firstActive; row >= 0 && row < S.activeLines)
            rowLine[size_t(row)] = int(i);
    const float* v = s.v.data();
    const int64_t n1 = S.samples();
    const std::vector<float>& tail = f.tail;
    auto at = [&](int64_t n) -> float {
        if (n >= 0)
            return v[std::min<int64_t>(n, n1 - 1)];
        const int64_t t = n + int64_t(tail.size());
        return t >= 0 ? tail[size_t(t)] : 0.0f;
    };
    const int combLag = S.pal ? 2 * S.line : S.line; // (PAL's subcarrier comes back to its phase every second line)
    // A line's luma and its colour, demodulated (before the colour's low pass).
    auto demod = [&](const TvLine& l, std::vector<float>& Y, std::vector<float>& U, std::vector<float>& V) {
        const float ch = std::cos(l.hue + f.tint), sh = std::sin(l.hue + f.tint);
        for (int x = 0; x < width; ++x) {
            const int64_t n = l.start + S.activeStart + x;
            const float here = at(n);
            const float y = f.comb ? 0.5f * (here + at(n - combLag))
                                     : (0.5f * at(n - 2) + at(n - 1) + here + at(n + 1) + 0.5f * at(n + 2)) / 4.0f;
            const float c = here - y;
            const int k = phaseOf(S, s.frame, n);
            Y[size_t(x)] = y;
            U[size_t(x)] = 2.0f * c * (kSin[k] * ch + kCos[k] * sh);
            V[size_t(x)] = 2.0f * c * (kCos[k] * ch - kSin[k] * sh) * float(l.sign);
        }
    };
    raster.bits();
    parallelFor(S.activeLines, [&](int r0, int r1) {
        std::vector<float> Y(static_cast<size_t>(width)), U(static_cast<size_t>(width)), V(static_cast<size_t>(width)), pY(static_cast<size_t>(width)), pU(static_cast<size_t>(width)), pV(static_cast<size_t>(width));
        std::vector<double> sums;
        for (int row = r0; row < r1; ++row) {
            const int li = rowLine[size_t(row)];
            if (li < 0)
                continue;
            const TvLine& l = lines[size_t(li)];
            const int visible = int(std::clamp<int64_t>(l.end - (l.start + S.activeStart), 0, width));
            demod(l, Y, U, V);
            if (S.pal && li > 0 && lines[size_t(li - 1)].colour) { // PAL's delay line: the colour averaged with the line
                demod(lines[size_t(li - 1)], pY, pU, pV);          // before's, so a phase error only fades it
                for (int x = 0; x < width; ++x) {
                    U[size_t(x)] = 0.5f * (U[size_t(x)] + pU[size_t(x)]);
                    V[size_t(x)] = 0.5f * (V[size_t(x)] + pV[size_t(x)]);
                }
            }
            boxFilter(U, 6, sums);
            boxFilter(V, 6, sums);
            auto* out = reinterpret_cast<uint32_t*>(raster.scanLine(row));
            const float gain = l.colour ? l.acc * f.colour / 92.5f : 0.0f;
            const float black = S.pal ? 0.0f : 7.5f, range = 100.0f - black;
            for (int x = 0; x < width; ++x) {
                if (x >= visible) {
                    out[x] = 0xff000000u; // (the beam had gone back already)
                    continue;
                }
                const float yy = (Y[size_t(x)] - l.clamp - black) / range * f.contrast + f.brightness;
                const float u = U[size_t(x)] * gain * f.contrast, w = V[size_t(x)] * gain * f.contrast;
                const float r = yy + w / 0.877f, b = yy + u / 0.492f;
                const float g = (yy - 0.299f * r - 0.114f * b) / 0.587f;
                out[x] = 0xff000000u | uint32_t(clamp01(r) * 255.0f + 0.5f) << 16 | uint32_t(clamp01(g) * 255.0f + 0.5f) << 8
                         | uint32_t(clamp01(b) * 255.0f + 0.5f);
            }
        }
    });
}

} // namespace

TvFrame tvScan(const Signal& s, const Params& p, std::unique_ptr<EffectState>& state)
{
    const Standard& S = *s.std;
    auto* tv = dynamic_cast<Tv*>(state.get());
    if (!tv) {
        state = std::make_unique<Tv>();
        tv = static_cast<Tv*>(state.get());
    }
    TvSettings set;
    set.hFree = int(std::lround(S.line * (1.0 + 0.03 * p.num(QStringLiteral("hHold")))));
    set.vFree = int(std::lround(S.lines + 30 * p.num(QStringLiteral("vHold"))));
    set.capture = int(S.line * (1.0 - std::clamp(p.num(QStringLiteral("hLock")), 0.01, 0.6)));
    set.vCapture = S.lines * 4 / 7;
    set.slice = float(p.num(QStringLiteral("slice")));
    TvFrame f;
    f.contrast = float(p.num(QStringLiteral("contrast")));
    f.brightness = float(p.num(QStringLiteral("brightness")));
    f.colour = float(p.num(QStringLiteral("colour")));
    f.tint = float(p.num(QStringLiteral("tint")) * pi / 180.0);
    f.comb = p.choice(QStringLiteral("separation")) == 1;
    f.start = TvFrame::Start::Continue;
    if (tv->std == &S && s.frame == tv->lastFrame) { // the same frame again (a still preview): drawn as it was
        tv->now = tv->atStart;
        tv->tail = tv->tailAtStart;
        f.start = TvFrame::Start::Repeat;
    } else if (tv->std != &S || s.frame != tv->lastFrame + 1) { // a jump (or another standard): a TV as it is at a
        tv->std = &S;                                            // frame's start when locked (the frame before's last
        tv->now = Beam{};                                        // line ending, the vertical sync to come), so it
        tv->now.y = tv->now.lineY = S.lines;                     // starts steady wherever playing starts
        tv->tail.assign(size_t(3 * S.line), 0.0f);
        f.start = TvFrame::Start::Fresh;
    }
    tv->atStart = tv->now;
    tv->tailAtStart = tv->tail;
    f.lines.reserve(size_t(S.lines) + 80);
    scan(tv->now, s, set, f.lines);
    f.tail = tv->tail;
    tv->now.lineStart -= S.samples(); // (now counted in the next frame's samples)
    tv->tail.assign(s.v.end() - 3 * S.line, s.v.end());
    tv->lastFrame = s.frame;
    return f;
}

namespace {

QImage decode(const Signal& s, const Params& p, QSize size, std::unique_ptr<EffectState>& state)
{
    const Standard& S = *s.std;
    auto* cpu = dynamic_cast<CpuTv*>(state.get());
    if (!cpu) {
        state = std::make_unique<CpuTv>();
        cpu = static_cast<CpuTv*>(state.get());
    }
    const TvFrame f = tvScan(s, p, cpu->beam);
    if (f.start == TvFrame::Start::Repeat) {
        cpu->raster = cpu->rasterAtStart;
    } else if (f.start == TvFrame::Start::Fresh || cpu->raster.size() != QSize(S.activeWidth(), S.activeLines)) {
        cpu->raster = QImage(S.activeWidth(), S.activeLines, QImage::Format_RGB32);
        cpu->raster.fill(Qt::black);
    }
    cpu->rasterAtStart = cpu->raster.copy();
    draw(cpu->raster, s, f);
    // The raster over the picture (the 4:3 screen fills it).
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    const QImage& r = cpu->raster;
    const int w = size.width(), h = size.height(), rw = r.width(), rh = r.height();
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const float sy = std::clamp((float(y) + 0.5f) * float(rh) / float(h) - 0.5f, 0.0f, float(rh - 1));
            const int a = int(sy), b = std::min(a + 1, rh - 1);
            const float fy = sy - float(a);
            const auto* ra = reinterpret_cast<const uint32_t*>(r.constScanLine(a));
            const auto* rb = reinterpret_cast<const uint32_t*>(r.constScanLine(b));
            auto* row = reinterpret_cast<uint32_t*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float sx = std::clamp((float(x) + 0.5f) * float(rw) / float(w) - 0.5f, 0.0f, float(rw - 1));
                const int c = int(sx), d = std::min(c + 1, rw - 1);
                const float fx = sx - float(c);
                uint32_t px = 0xff000000u;
                for (int sh = 0; sh < 24; sh += 8) {
                    const float top = float((ra[c] >> sh) & 0xff) * (1 - fx) + float((ra[d] >> sh) & 0xff) * fx;
                    const float bottom = float((rb[c] >> sh) & 0xff) * (1 - fx) + float((rb[d] >> sh) & 0xff) * fx;
                    px |= uint32_t(top * (1 - fy) + bottom * fy + 0.5f) << sh;
                }
                row[x] = px;
            }
        }
    });
    return out;
}

// ---- Circuits ---------------------------------------------------------------------------------------------------

// The luma of sample n (a notch at the subcarrier: the mean over a cycle, centred), from a frame's samples.
inline float notchAt(const float* v, int64_t n, int64_t count)
{
    auto at = [&](int64_t i) { return v[std::clamp<int64_t>(i, 0, count - 1)]; };
    if (n >= 2 && n + 2 < count)
        return (0.5f * v[n - 2] + v[n - 1] + v[n] + v[n + 1] + 0.5f * v[n + 2]) / 4.0f;
    return (0.5f * at(n - 2) + at(n - 1) + at(n) + at(n + 1) + 0.5f * at(n + 2)) / 4.0f;
}

// The sync separator: a comparator (with a little hysteresis) that gives clean sync pulses where the signal dips
// below its slice level; Correction sets how much of them goes on to the mixer.
Signal separate(const Signal& in, const Params& p)
{
    Signal out = like(in);
    const float slice = float(p.num(QStringLiteral("slice"))), hyst = float(p.num(QStringLiteral("hysteresis")));
    const float level = -40.0f * float(p.num(QStringLiteral("correction")));
    bool low = in.v.front() < slice;
    for (size_t n = 0; n < out.v.size(); ++n) {
        const float x = in.v[n];
        low = low ? x < slice + hyst : x < slice;
        out.v[n] = low ? level : 0.0f;
    }
    return out;
}

// The sync mixer: the video (crossfaded with the low-pass input) split into luma and chroma, each at its own level,
// its own sync taken off (clipped at blanking) where new sync comes in, and that sync added. (Inputs of another
// standard than the video's aren't used.)
Signal mix(const Signal* video, const Signal* sync, const Signal* lowPass, const Params& p, int64_t frame)
{
    const Signal* first = video ? video : sync ? sync : lowPass;
    const Standard& S = first ? *first->std : ntsc();
    if (sync && sync->std != &S)
        sync = nullptr;
    if (lowPass && lowPass->std != &S)
        lowPass = nullptr;
    Signal out = blank(S, first ? first->frame : frame);
    const int64_t count = S.samples();
    const float luma = float(p.num(QStringLiteral("luma"))), chroma = float(p.num(QStringLiteral("chroma")));
    const float burst = float(p.num(QStringLiteral("burst"))), k = lowPass ? float(p.num(QStringLiteral("lowPass"))) : 0.0f;
    const bool strip = sync && p.flag(QStringLiteral("strip"));
    // The video crossfaded with the low-pass input, first (each sample's neighbours are read below).
    std::vector<float> buf(size_t(count), 0.0f);
    parallelFor(S.lines, [&](int l0, int l1) {
        for (size_t n = size_t(l0) * S.line; n < size_t(l1) * S.line; ++n) {
            const float a = video ? video->v[n] : 0.0f;
            buf[n] = lowPass ? a * (1 - k) + lowPass->v[n] * k : a;
        }
    });
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int line = l0; line < l1; ++line)
            for (int x = 0; x < S.line; ++x) {
                const int64_t n = int64_t(line) * S.line + x;
                const float here = buf[size_t(n)];
                float y = notchAt(buf.data(), n, count);
                const float c = here - y;
                if (strip)
                    y = std::max(y, 0.0f);
                out.v[size_t(n)] = luma * y + (x >= S.burstStart && x < S.burstEnd ? burst : chroma) * c + (sync ? sync->v[size_t(n)] : 0.0f);
            }
    });
    return out;
}

// The signal later by `delay(line)` samples on each line (a fraction too, between samples), round the frame.
Signal delayed(const Signal& in, const std::function<double(int)>& delay)
{
    const Standard& S = *in.std;
    const int64_t count = S.samples();
    Signal out = like(in);
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int line = l0; line < l1; ++line) {
            // (the same delay along the line: one whole-sample offset and one fraction for all of it)
            const double start = double(int64_t(line) * S.line) - delay(line), whole = std::floor(start);
            const float t = float(start - whole);
            int64_t a = (int64_t(whole) % count + count) % count;
            float* o = out.v.data() + size_t(line) * S.line;
            for (int x = 0; x < S.line; ++x) {
                const int64_t b = a + 1 < count ? a + 1 : 0;
                o[x] = in.v[size_t(a)] * (1 - t) + in.v[size_t(b)] * t;
                a = b;
            }
        }
    });
    return out;
}

// A delay line: the signal later by a number of lines and microseconds (in between samples too: a fraction of a
// sample turns the colour's phase), drifting by Drift lines a second and wobbling along the frame. Delayed sync
// slips the picture against it (a genlock gone wrong), a delayed copy mixed back is a ghost (multipath).
Signal delayLine(const Signal& in, const Params& p, double seconds)
{
    const Standard& S = *in.std;
    const int64_t count = S.samples();
    std::vector<float> table;
    delayTable(S, p, seconds, table);
    Signal out = like(in);
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int line = l0; line < l1; ++line) {
            int64_t a = int64_t(table[size_t(2 * line)]);
            const float t = table[size_t(2 * line + 1)];
            float* o = out.v.data() + size_t(line) * S.line;
            for (int x = 0; x < S.line; ++x) {
                const int64_t b = a + 1 < count ? a + 1 : 0;
                o[x] = in.v[size_t(a)] * (1 - t) + in.v[size_t(b)] * t;
                a = b;
            }
        }
    });
    return out;
}

// Interference on the way: snow (noise in every sample), a mains hum bar rolling slowly up the picture, and
// spikes (bursts that blank a stretch of a line, or knock its sync out).
Signal rfNoise(const Signal& in, const Params& p, double seconds)
{
    const Standard& S = *in.std;
    Signal out = like(in);
    const float snow = float(p.num(QStringLiteral("snow"))), hum = float(p.num(QStringLiteral("hum")));
    const double humPhase = p.num(QStringLiteral("humSpeed")) * seconds;
    const double spikes = p.num(QStringLiteral("spikes"));
    const uint32_t seed = noiseSeed(p, in.frame);
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int line = l0; line < l1; ++line) {
            const float bar = hum * float(std::sin(2 * pi * (double(line) / S.lines + humPhase)));
            int spikeFrom = S.line, spikeTo = S.line;
            float spikeLevel = 0;
            if (spikes > 0 && fx::hash01(uint32_t(line), 1, seed) < spikes * 0.15) {
                spikeFrom = int(fx::hash01(uint32_t(line), 2, seed) * float(S.line));
                spikeTo = spikeFrom + 10 + int(fx::hash01(uint32_t(line), 3, seed) * 180);
                spikeLevel = fx::hash01(uint32_t(line), 4, seed) < 0.5f ? -60.0f : 110.0f;
            }
            const float* src = in.v.data() + size_t(line) * S.line;
            float* dst = out.v.data() + size_t(line) * S.line;
            for (int x = 0; x < S.line; ++x) {
                float v = src[x] + bar;
                if (snow > 0)
                    v += snow * 2.0f * (fx::hash01(uint32_t(x), uint32_t(line), seed) - 0.5f);
                if (x >= spikeFrom && x < spikeTo)
                    v = spikeLevel;
                dst[x] = v;
            }
        }
    });
    return out;
}

// The signal through an RC low pass with time constant tau (seconds); high: what's left (the RC the other way
// round: a capacitor in series, a resistor to ground).
Signal rc(const Signal& in, double tau, bool high)
{
    const Standard& S = *in.std;
    const int64_t count = S.samples();
    const float a = float(1.0 / (1.0 + tau * S.rate)); // (backward Euler)
    Signal out = like(in);
    // In pieces on several threads (one, for a long time constant), each run in from a little before it; the first
    // from the frame's end, as it would come after the frame before.
    const int64_t preroll = std::min<int64_t>(count, int64_t(8 * tau * S.rate) + S.line);
    const int pieces = preroll * 8 < count ? 16 : 1;
    const int64_t piece = (count + pieces - 1) / pieces;
    parallelFor(pieces, [&](int p0, int p1) {
        for (int pc = p0; pc < p1; ++pc) {
            const int64_t from = int64_t(pc) * piece, to = std::min<int64_t>(count, from + piece);
            float y = in.v[size_t(((from - preroll) % count + count) % count)];
            for (int64_t i = from - preroll; i < to; ++i) {
                const int64_t n = (i % count + count) % count;
                y += a * (in.v[size_t(n)] - y);
                if (i >= from)
                    out.v[size_t(n)] = high ? in.v[size_t(n)] - y : y;
            }
        }
    }, 1);
    return out;
}

// A small linear system, factored once for a frame.
struct Lu {
    static constexpr int N = 4;
    std::array<double, N * N> a{};
    std::array<int, N> perm{};
    void factor()
    {
        for (int i = 0; i < N; ++i)
            perm[size_t(i)] = i;
        for (int c = 0; c < N; ++c) {
            int best = c;
            for (int r = c + 1; r < N; ++r)
                if (std::abs(a[size_t(r * N + c)]) > std::abs(a[size_t(best * N + c)]))
                    best = r;
            if (best != c) {
                for (int k = 0; k < N; ++k)
                    std::swap(a[size_t(c * N + k)], a[size_t(best * N + k)]);
                std::swap(perm[size_t(c)], perm[size_t(best)]);
            }
            const double d = a[size_t(c * N + c)] != 0 ? a[size_t(c * N + c)] : 1e-30;
            for (int r = c + 1; r < N; ++r) {
                const double f = a[size_t(r * N + c)] / d;
                a[size_t(r * N + c)] = f;
                for (int k = c + 1; k < N; ++k)
                    a[size_t(r * N + k)] -= f * a[size_t(c * N + k)];
            }
        }
    }
    void solve(const double* b, double* x) const
    {
        double y[N];
        for (int i = 0; i < N; ++i) {
            double s = b[perm[size_t(i)]];
            for (int k = 0; k < i; ++k)
                s -= a[size_t(i * N + k)] * y[k];
            y[i] = s;
        }
        for (int i = N - 1; i >= 0; --i) {
            double s = y[i];
            for (int k = i + 1; k < N; ++k)
                s -= a[size_t(i * N + k)] * x[k];
            const double d = a[size_t(i * N + i)];
            x[i] = s / (d != 0 ? d : 1e-30);
        }
    }
};

// The enhancer: a one-transistor amplifier solved as a circuit, sample by sample. The input (loaded to ground,
// through a buffer) goes in through Input Z to the base; the emitter goes to ground through Gain, bypassed by the
// Gain Channel's capacitor (so fine detail gets more gain: sharpening, ringing); the collector's load gives the
// output (through a buffer), clipping at the supply; collector-to-base feedback (a resistor in series with a
// capacitor or a resistor, and a second resistor) takes gain back, the capacitor path only for fine detail
// (smearing). Values: 75 ohm source, 22 pF base, 270 ohm collector load with 5 pF, gm 40 mS (1 mA), 100 pF and 1k
// in the switched feedback path.
Signal enhance(const Signal& in, const Params& p)
{
    const Standard& S = *in.std;
    const int64_t count = S.samples();
    enum { B, E, C, F }; // (the input, an ideal source behind Input Z, is a current into the base)
    constexpr double kGm = 0.04, kRc = 270, kCb = 22e-12, kCc = 5e-12, kCf = 100e-12, kRfix = 1000;
    constexpr double kHigh = 300, kLow = -260; // where the collector clips (IRE about its resting point)
    const double dt = 1.0 / S.rate;
    const double load = p.num(QStringLiteral("load")), rin = std::max(1.0, p.num(QStringLiteral("inputZ")));
    const double rfa = std::max(1.0, p.num(QStringLiteral("feedbackA"))), rfb = std::max(1.0, p.num(QStringLiteral("feedbackB")));
    const bool capPath = p.choice(QStringLiteral("feedbackPath")) == 0;
    const double rg = std::max(1.0, p.num(QStringLiteral("gain")));
    static const double kChannel[] = {0, 100e-12, 330e-12, 1e-9};
    const double cg = kChannel[std::clamp(p.choice(QStringLiteral("gainChannel")), 0, 3)];
    const double sign = p.choice(QStringLiteral("output")) == 0 ? -1.0 : 1.0; // (Normal: a second stage turns it back)
    const double divider = load / (load + 75.0) / rin;
    const double gb = kCb / dt, gcc = kCc / dt, gf = kCf / dt, gg = cg / dt;

    // The circuit is linear but for the clipping: its matrix inverted once (as it is, and with the collector held
    // at each rail), each sample is then a multiply.
    std::array<double, 16> inverse[3];
    for (int m = 0; m < 3; ++m) {
        Lu lu;
        auto& A = lu.a;
        auto stamp = [&](int i, int j, double g) {
            if (i >= 0)
                A[size_t(i * 4 + i)] += g;
            if (j >= 0)
                A[size_t(j * 4 + j)] += g;
            if (i >= 0 && j >= 0) {
                A[size_t(i * 4 + j)] -= g;
                A[size_t(j * 4 + i)] -= g;
            }
        };
        stamp(B, -1, 1.0 / rin);
        stamp(B, -1, gb);
        stamp(E, -1, 1.0 / rg);
        if (cg > 0)
            stamp(E, -1, gg);
        stamp(C, -1, 1.0 / kRc);
        stamp(C, -1, gcc);
        stamp(C, F, 1.0 / rfa);
        stamp(F, B, capPath ? gf : 1.0 / kRfix);
        stamp(C, B, 1.0 / rfb);
        // The transistor: gm (vB - vE) from collector to emitter.
        A[size_t(C * 4 + B)] += kGm;
        A[size_t(C * 4 + E)] -= kGm;
        A[size_t(E * 4 + B)] -= kGm;
        A[size_t(E * 4 + E)] += kGm;
        if (m > 0) { // clipped: the collector held at the rail
            for (int k = 0; k < 4; ++k)
                A[size_t(C * 4 + k)] = 0;
            A[size_t(C * 4 + C)] = 1;
        }
        lu.factor();
        for (int col = 0; col < 4; ++col) {
            double e[4] = {0, 0, 0, 0}, x[4];
            e[col] = 1;
            lu.solve(e, x);
            for (int row = 0; row < 4; ++row)
                inverse[m][size_t(row * 4 + col)] = x[row];
        }
    }
    auto apply = [](const std::array<double, 16>& M, const double* b, double* x) {
        for (int r = 0; r < 4; ++r)
            x[r] = M[size_t(r * 4)] * b[0] + M[size_t(r * 4 + 1)] * b[1] + M[size_t(r * 4 + 2)] * b[2] + M[size_t(r * 4 + 3)] * b[3];
    };

    Signal out = like(in);
    // In pieces on several threads, each run in from a little before it (its capacitors charged as they would be).
    const double tau = std::max({capPath ? kCf * (rfa + kRc + rin) : 0.0, cg * rg, kCb * rin, kCc * kRc});
    // (Capped at 8 lines: the long time constants come with big feedback resistors, which take back little.)
    const int64_t preroll = std::clamp<int64_t>(int64_t(6 * tau * S.rate), S.line, 8 * S.line);
    constexpr int kPieces = 16;
    const int64_t piece = (count + kPieces - 1) / kPieces;
    parallelFor(kPieces, [&](int p0, int p1) {
        for (int pc = p0; pc < p1; ++pc) {
            const int64_t from = int64_t(pc) * piece, to = std::min<int64_t>(count, from + piece);
            double vb = 0, ve = 0, vc = 0, vf = 0;
            for (int64_t i = from - preroll; i < to; ++i) {
                const int64_t n = (i % count + count) % count;
                double b[4] = {double(in.v[size_t(n)]) * divider + gb * vb, gg * ve, gcc * vc, 0}, x[4];
                if (capPath) {
                    const double d = vf - vb;
                    b[F] += gf * d;
                    b[B] -= gf * d;
                }
                apply(inverse[0], b, x);
                if (x[C] > kHigh || x[C] < kLow) {
                    const bool high = x[C] > kHigh;
                    b[C] = high ? kHigh : kLow;
                    apply(inverse[high ? 1 : 2], b, x);
                }
                vb = x[B];
                ve = x[E];
                vc = x[C];
                vf = x[F];
                if (i >= from)
                    out.v[size_t(n)] = float(sign * vc);
            }
        }
    }, 1);
    return out;
}

// A clipper (diodes to two levels): the signal held between Low and High, with a soft knee Softness wide.
Signal clip(const Signal& in, const Params& p)
{
    Signal out = like(in);
    const float hi = float(p.num(QStringLiteral("high"))), lo = std::min(hi, float(p.num(QStringLiteral("low"))));
    const float soft = std::min(float(p.num(QStringLiteral("softness"))), 0.5f * (hi - lo));
    parallelFor(in.std->lines, [&](int l0, int l1) {
        for (size_t n = size_t(l0) * in.std->line; n < size_t(l1) * in.std->line; ++n) {
            float x = in.v[n];
            if (soft <= 0.001f) {
                x = std::clamp(x, lo, hi);
            } else if (x > hi - soft) { // (an exponential knee: smooth at its start, flat at the level)
                x = hi - soft * std::exp(-(x - (hi - soft)) / soft);
            } else if (x < lo + soft) {
                x = lo + soft * std::exp((x - (lo + soft)) / soft);
            }
            out.v[n] = x;
        }
    });
    return out;
}

// Luma or chroma alone: a notch at the subcarrier (or a comb, one line back for NTSC, two for PAL) takes the colour
// off for the luma (sync and all); what it took is the chroma (burst and all).
Signal split(const Signal& in, const Params& p)
{
    const Standard& S = *in.std;
    const int64_t count = S.samples();
    const bool chroma = p.choice(QStringLiteral("output")) == 1, comb = p.choice(QStringLiteral("method")) == 1;
    const int lag = S.pal ? 2 * S.line : S.line;
    Signal out = like(in);
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int64_t n = int64_t(l0) * S.line; n < int64_t(l1) * S.line; ++n) {
            const float here = in.v[size_t(n)];
            const float y = comb ? 0.5f * (here + in.v[size_t(((n - lag) % count + count) % count)]) : notchAt(in.v.data(), n, count);
            out.v[size_t(n)] = chroma ? here - y : y;
        }
    });
    return out;
}

// A switcher cutting from A to B at a line (top/bottom) or a point along each line (left/right): a wipe made in the
// signal, sync and all (B's sync after the cut: if it isn't A's, the TV jumps).
Signal switcher(const Signal* a, const Signal* b, const Params& p, int64_t frame)
{
    if (b && a && b->std != a->std)
        b = nullptr;
    const Signal* first = a ? a : b;
    const Standard& S = first ? *first->std : ntsc();
    Signal out = blank(S, first ? first->frame : frame);
    const bool across = p.choice(QStringLiteral("direction")) == 1, swap = p.flag(QStringLiteral("swap"));
    const double at = std::clamp(p.num(QStringLiteral("position")), 0.0, 1.0);
    const int cutLine = int(std::lround(at * S.lines)), cutX = S.activeStart + int(std::lround(at * S.activeWidth()));
    parallelFor(S.lines, [&](int l0, int l1) {
        for (int line = l0; line < l1; ++line)
            for (int x = 0; x < S.line; ++x) {
                const bool second = (across ? x >= cutX : line >= cutLine) != swap;
                const Signal* from = second ? b : a;
                const size_t n = size_t(line) * S.line + x;
                out.v[n] = from ? from->v[n] : 0.0f;
            }
    });
    return out;
}

// A sync generator's signal: black (with its burst), colour bars or a grey ramp, on its own clock: Line drift and
// Frame drift slip it against the standard's (a second camera that isn't genlocked).
Signal generator(const Params& p, int64_t frame, double seconds)
{
    const Standard& S = generatorStandard(p);
    EncodeOptions o;
    o.std = &S;
    const Signal own = encode(generatorPicture(p), frame, o);
    const double slip = generatorSlip(p, S, seconds);
    return slip == 0 ? own : delayed(own, [&](int) { return slip; });
}

// ---- Nodes ------------------------------------------------------------------------------------------------------

// The signal wired into input i: from the node that made it, or a picture turned into one.
std::shared_ptr<const Signal> input(const Inputs& in, size_t i, const nodes::NodeContext& c)
{
    if (c.bus && i < c.inputs.size() && c.inputs[i])
        if (const auto it = c.bus->find(c.inputs[i]); it != c.bus->end() && it->second)
            return it->second;
    if (i < in.size() && in[i])
        return std::make_shared<Signal>(encode(*in[i], frameOf(c)));
    return nullptr;
}

QImage publish(Signal&& s, const nodes::NodeContext& c)
{
    auto shared = std::make_shared<const Signal>(std::move(s));
    if (c.bus)
        (*c.bus)[c.node] = shared;
    return c.pictureWanted ? view(*shared, c.size) : QImage(); // (only signal nodes after it: no picture needed)
}

// A node taking one signal through f (without one: a blank signal).
std::function<QImage(const Inputs&, const Params&, const ParamMap&, const nodes::NodeContext&, std::unique_ptr<EffectState>&)>
through(std::function<Signal(const Signal&, const Params&, const nodes::NodeContext&)> f)
{
    return [f](const Inputs& in, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>&) {
        const auto s = input(in, 0, c);
        return s ? publish(f(*s, p, c), c) : publish(blank(ntsc(), frameOf(c)), c);
    };
}

} // namespace

void addNodes(std::vector<nodes::NodeDef>& list)
{
    const QString category = QStringLiteral("Composite Signal");
    const QString signalIn = QStringLiteral("Signal");
    const QStringList standards{QStringLiteral("NTSC"), QStringLiteral("PAL")};
    nodes::NodeDef d;

    d = {};
    d.id = QStringLiteral("composite_encode");
    d.name = QStringLiteral("Composite Encode");
    d.category = category;
    d.description = QStringLiteral(
        "Turns the picture into an NTSC or PAL composite video signal, as a camera or console sends it: sync pulses, colour "
        "burst, the colour on its subcarrier. Wire it through the circuit nodes and into Composite Decode (a TV) to see it "
        "again. (The circuit nodes encode a picture wired straight in themselves, as NTSC.)");
    d.inputs = {QStringLiteral("Image")};
    d.params = {choice("standard", "Standard", standards), num("saturation", "Saturation", 0, 2, 1),
                num("chroma", "Colour bandwidth", 0.3, 3, 1.3, 1, " MHz"), fx::flag("setup", "7.5 IRE black (NTSC-M)", true)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>&) {
        EncodeOptions o;
        o.std = p.choice(QStringLiteral("standard")) == 1 ? &pal() : &ntsc();
        o.saturation = p.num(QStringLiteral("saturation"));
        o.chromaMHz = p.num(QStringLiteral("chroma"));
        o.setup = p.flag(QStringLiteral("setup"));
        return publish(encode(in[0] ? *in[0] : QImage(), frameOf(c), o), c);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_decode");
    d.name = QStringLiteral("Composite Decode");
    d.category = category;
    d.description = QStringLiteral(
        "A TV (NTSC or PAL, as the signal is): locks to the signal's sync and colour burst and shows its picture. When the "
        "sync is damaged or missing the picture rolls and tears (its oscillators running free, as Horizontal and Vertical "
        "Hold set them); without a burst it is black and white, and a shifted burst shifts the colours (on PAL, only fades "
        "them: its delay line averages each line's colour with the one before's).");
    d.inputs = {signalIn};
    d.params = {num("hHold", "Horizontal hold", -1, 1, 0.1), num("vHold", "Vertical hold", -1, 1, 0.2),
                num("hLock", "Horizontal lock range", 0.01, 0.6, 0.15), num("slice", "Sync sensitivity", 2, 40, 10, 0, " IRE"),
                num("contrast", "Contrast", 0, 3, 1), num("brightness", "Brightness", -1, 1, 0), num("colour", "Colour", 0, 3, 1),
                num("tint", "Tint", -180, 180, 0, 0, "°"), choice("separation", "Luma/chroma separation", {QStringLiteral("Notch"), QStringLiteral("Comb")})};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>& state) {
        const auto s = input(in, 0, c);
        if (!s) {
            QImage black(c.size, QImage::Format_ARGB32_Premultiplied);
            black.fill(Qt::black);
            return black;
        }
        return decode(*s, p, c.size, state);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_sync_generator");
    d.name = QStringLiteral("Sync Generator");
    d.category = category;
    d.description = QStringLiteral(
        "A signal of its own: black (with sync and burst, \"black burst\"), colour bars or a grey ramp, on its own clock. "
        "Line drift and Frame drift slip it against the picture's: take its sync into a Sync Mixer and the TV follows a "
        "clock that isn't the picture's, as with a second camera that isn't genlocked.");
    d.params = {choice("standard", "Standard", standards),
                choice("picture", "Picture", {QStringLiteral("Black"), QStringLiteral("Colour bars"), QStringLiteral("Grey ramp")}),
                num("lineDrift", "Line drift", -20, 20, 0, 3, " µs/s"), num("frameDrift", "Frame drift", -60, 60, 0, 2, " lines/s")};
    d.eval = [](const Inputs&, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>&) {
        return publish(generator(p, frameOf(c), double(c.render.timelineTime) / 1e6), c);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_sync_separator");
    d.name = QStringLiteral("Sync Separator");
    d.category = category;
    d.description = QStringLiteral(
        "Takes the sync pulses out of a signal (where it dips below the slice level) as clean new pulses. Wire them into a "
        "Sync Mixer to put one signal's sync on another's picture; Correction sets how strong they are.");
    d.inputs = {QStringLiteral("Sync In")};
    d.params = {num("slice", "Slice level", -40, 40, -20, 0, " IRE"), num("hysteresis", "Hysteresis", 0, 30, 4, 0, " IRE"),
                num("correction", "Correction", 0, 2, 1)};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return separate(s, p); });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_sync_mixer");
    d.name = QStringLiteral("Sync Mixer");
    d.category = category;
    d.description = QStringLiteral(
        "Mixes a signal's luma and chroma at their own levels, crossfaded with the Low Pass input, and puts sync on it from "
        "the Sync input (taking its own off first). Sync from another signal than the picture: the TV follows that one.");
    d.inputs = {QStringLiteral("Video"), QStringLiteral("Sync"), QStringLiteral("Low Pass")};
    d.params = {num("luma", "Luma", 0, 2, 1), num("chroma", "Chroma", 0, 3, 1), num("burst", "Burst", 0, 3, 1),
                num("lowPass", "Low Pass", 0, 1, 0.5), fx::flag("strip", "Take the video's own sync off", true)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>&) {
        const auto video = input(in, 0, c), sync = input(in, 1, c), low = input(in, 2, c);
        return publish(mix(video.get(), sync.get(), low.get(), p, frameOf(c)), c);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_switcher");
    d.name = QStringLiteral("Signal Switcher");
    d.category = category;
    d.description = QStringLiteral(
        "Cuts from signal A to signal B at a line (top and bottom) or at a point along every line (left and right): a wipe "
        "made in the signal, sync and all. Keyframe Position to move it; where B's sync isn't A's the TV jumps at the cut.");
    d.inputs = {QStringLiteral("A"), QStringLiteral("B")};
    d.params = {choice("direction", "Cut", {QStringLiteral("Top / bottom (at a line)"), QStringLiteral("Left / right (along each line)")}),
                num("position", "Position", 0, 1, 0.5, 3), fx::flag("swap", "B first", false)};
    d.eval = [](const Inputs& in, const Params& p, const ParamMap&, const nodes::NodeContext& c, std::unique_ptr<EffectState>&) {
        const auto a = input(in, 0, c), b = input(in, 1, c);
        return publish(switcher(a.get(), b.get(), p, frameOf(c)), c);
    };
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_split");
    d.name = QStringLiteral("Luma/Chroma Split");
    d.category = category;
    d.description = QStringLiteral(
        "The signal's luma alone (its sync with it) or its chroma alone (its burst with it), taken apart with a notch at "
        "the subcarrier or a comb filter. Treat them apart (delay the colour, smear the luma) and add them back in a Sync "
        "Mixer (Low Pass at 0.5, levels at 2).");
    d.inputs = {signalIn};
    d.params = {choice("output", "Output", {QStringLiteral("Luma"), QStringLiteral("Chroma")}),
                choice("method", "Separation", {QStringLiteral("Notch"), QStringLiteral("Comb")})};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return split(s, p); });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_low_pass");
    d.name = QStringLiteral("RC Low Pass");
    d.category = category;
    d.description = QStringLiteral(
        "A resistor into a capacitor to ground: smears the signal along each line; big values smear the colour away, "
        "lose the sync's edges and drag streaks down the picture.");
    d.inputs = {signalIn};
    d.params = {ohms("resistance", "Resistance", 10, 100000, 470),
                choice("capacitor", "Capacitor", {QStringLiteral("47 pF"), QStringLiteral("220 pF"), QStringLiteral("1 nF"), QStringLiteral("4.7 nF"), QStringLiteral("22 nF")}, 1)};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return rc(s, rcTau(p, false), false); });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_high_pass");
    d.name = QStringLiteral("RC High Pass");
    d.category = category;
    d.description = QStringLiteral(
        "A capacitor in series, a resistor to ground (AC coupling, as between two pieces of kit): the signal's steady level "
        "taken off. A small one tilts every line and leaves streaks after bright things; the TV's clamp puts each line's "
        "black back, but brightness wanders and the sync can slip under it.");
    d.inputs = {signalIn};
    d.params = {ohms("resistance", "Resistance", 10, 1000000, 1000),
                choice("capacitor", "Capacitor", {QStringLiteral("1 nF"), QStringLiteral("10 nF"), QStringLiteral("100 nF"), QStringLiteral("1 µF"), QStringLiteral("10 µF"), QStringLiteral("100 µF")}, 3)};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return rc(s, rcTau(p, true), true); });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_clipper");
    d.name = QStringLiteral("Clipper");
    d.category = category;
    d.description = QStringLiteral(
        "Diodes holding the signal between two levels: High crushes the highlights flat (and the colour on them), Low "
        "clips the sync tips (too high and the TV loses them). Softness rounds the knee.");
    d.inputs = {signalIn};
    d.params = {num("high", "High", -40, 140, 100, 1, " IRE"), num("low", "Low", -60, 100, -40, 1, " IRE"),
                num("softness", "Softness", 0, 30, 0, 1, " IRE")};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return clip(s, p); });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_delay_line");
    d.name = QStringLiteral("Delay Line");
    d.category = category;
    d.description = QStringLiteral(
        "The signal later, by lines and microseconds (a fraction of a sample turns its colour), drifting and wobbling. "
        "Delay the sync a Sync Mixer puts back and the picture slips against it; mix a delayed copy back in for ghosts.");
    d.inputs = {signalIn};
    d.params = {num("delay", "Delay", 0, 63.5, 2, 3, " µs"), num("lines", "Lines", 0, 312, 0, 0),
                num("drift", "Drift", -60, 60, 0, 2, " lines/s"), num("wobble", "Wobble", 0, 2, 0, 3, " µs"),
                num("wobbleHz", "Wobble rate", 0, 30, 1, 2, " Hz")};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext& c) {
        return delayLine(s, p, double(c.render.timelineTime) / 1e6);
    });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_rf_noise");
    d.name = QStringLiteral("RF Noise");
    d.category = category;
    d.description = QStringLiteral(
        "Interference on the signal: snow, a mains hum bar rolling up the picture, and spikes that blank a stretch of "
        "a line or knock its sync out (enough of them and the TV loses its hold).");
    d.inputs = {signalIn};
    d.params = {num("snow", "Snow", 0, 80, 12, 1, " IRE"), num("hum", "Hum", 0, 40, 0, 1, " IRE"),
                num("humSpeed", "Hum speed", -2, 2, 0.06, 2, " bars/s"), num("spikes", "Spikes", 0, 1, 0, 2),
                num("seed", "Seed", 0, 1000, 1, 0)};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext& c) {
        return rfNoise(s, p, double(c.render.timelineTime) / 1e6);
    });
    list.push_back(d);

    d = {};
    d.id = QStringLiteral("composite_enhancer");
    d.name = QStringLiteral("Enhancer");
    d.category = category;
    d.description = QStringLiteral(
        "A one-transistor video amplifier, solved as a circuit: Gain (its emitter resistor) sets the level, the Gain Channel "
        "capacitor gives fine detail more (sharpening, ringing, colour boost), Input Z and the Feedback resistors take it "
        "back or smear it (through the capacitor), and pushed hard it clips at its supply.");
    d.inputs = {signalIn};
    d.params = {ohms("load", "Input load", 10, 10000, 10000), ohms("inputZ", "Input Z", 10, 100000, 470),
                ohms("feedbackA", "Feedback", 100, 1000000, 1000000),
                choice("feedbackPath", "Feedback path", {QStringLiteral("Capacitor"), QStringLiteral("Resistor")}),
                ohms("feedbackB", "Feedback (direct)", 100, 1000000, 1000000), ohms("gain", "Gain", 1, 1000, 220),
                choice("gainChannel", "Gain channel", {QStringLiteral("Off"), QStringLiteral("Narrow (100 pF)"), QStringLiteral("Medium (330 pF)"), QStringLiteral("Wide (1 nF)")}, 1),
                choice("output", "Output", {QStringLiteral("Normal"), QStringLiteral("Inverted")})};
    d.eval = through([](const Signal& s, const Params& p, const nodes::NodeContext&) { return enhance(s, p); });
    list.push_back(d);
}

} // namespace composite
