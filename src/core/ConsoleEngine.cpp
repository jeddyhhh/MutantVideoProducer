// CPU renderer of the virtual broken console (see ConsoleEngine.h). The
// per-pixel program mirrors GpuConsoleShaders.h statement by statement.

#include "core/ConsoleEngine.h"

#include "core/EffectHelpers.h"
#include "core/Parallel.h"

#include <QMutex>
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace vg {

QByteArray shaderDefines()
{
    QByteArray s;
    int i = 0;
#define VG_DEF(n) s += QByteArray("#define G_" #n " uG[") + QByteArray::number(i++) + "]\n";
    VG_GLOBALS(VG_DEF)
#undef VG_DEF
    s += "#define VG_GLOBAL_COUNT " + QByteArray::number(int(kGlobalCount)) + "\n";
    return s;
}

void initPlan(Plan& p, int gw, int gh, int ax, int ay, int cw, int ch, int tile, QSize layer)
{
    p.g.fill(0.0f);
    p[G_GW] = float(gw);
    p[G_GH] = float(gh);
    p[G_AX] = float(ax);
    p[G_AY] = float(ay);
    p[G_CW] = float(cw);
    p[G_CH] = float(ch);
    p[G_TILE] = float(tile);
    p[G_TX] = float((cw + tile - 1) / tile);
    p[G_TY] = float((ch + tile - 1) / tile);
    p[G_SRC_W] = float(std::max(1, layer.width()));
    p[G_SRC_H] = float(std::max(1, layer.height()));
    p[G_QBITS_R] = p[G_QBITS_G] = p[G_QBITS_B] = 8;
    p[G_DITHER_AMT] = 1;
    p[G_GAIN] = p[G_CONTRAST] = p[G_SAT] = 1;
    p[G_AND_R] = p[G_AND_G] = p[G_AND_B] = 255;
    p[G_SPARK_LEN] = 1;
    p[G_ART_CELL] = 8;
    p[G_GARB_BPP] = 2;
    p[G_VEC_GAIN] = 2;
    p[G_VEC_COLOR] = float(0xffffff);
    p.rowMap.resize(size_t(gh));
    p.rowFx.assign(size_t(gh), {1, -1, 0, 0});
    p.rowEx.assign(size_t(gh), {-1, 0, 0, 0});
    p.rowEx2.assign(size_t(gh), {0, 1, 0, 0});
    for (int y = 0; y < gh; ++y)
        p.rowMap[size_t(y)] = {0, 1, float(y - ay) + 0.5f, 0};
    p.cols.assign(size_t(gw), {1, -1, 0, 0});
    p.tiles.assign(size_t(p[G_TX] * p[G_TY]), {0, 0, 0, 0});
}

void finalize(Plan& p)
{
    const int tx = int(p[G_TX]), ty = int(p[G_TY]);
    const int w = std::max({p.gw(), p.gh(), 128, tx});
    const bool lut = p.lut && int(p[G_QMODE]) == QPalette;
    const int lutLanes = lut ? (8192 + w - 1) / w : 0;
    p[G_DATA_W] = float(w);
    p[G_LANE_LUT] = float(kFixedLanes);
    p[G_LANE_TILES] = float(kFixedLanes + lutLanes);
    p[G_PAL_N] = float(std::min<size_t>(p.palette.size(), 128));
    const int h = kFixedLanes + lutLanes + ty;
    p.dataW = w;
    p.dataH = h;
    p.data.assign(size_t(w) * size_t(h) * 4, 0.0f);
    auto put = [&](int x, int lane, const Px4& v) { std::copy(v.begin(), v.end(), &p.data[(size_t(lane) * size_t(w) + size_t(x)) * 4]); };
    for (int y = 0; y < p.gh(); ++y) {
        put(y, LaneRowMap, p.rowMap[size_t(y)]);
        put(y, LaneRowFx, p.rowFx[size_t(y)]);
        put(y, LaneRowEx, p.rowEx[size_t(y)]);
        put(y, LaneRowEx2, p.rowEx2[size_t(y)]);
    }
    for (int x = 0; x < p.gw(); ++x)
        put(x, LaneCols, p.cols[size_t(x)]);
    for (int i = 0; i < int(p[G_PAL_N]); ++i) {
        const uint32_t shown = size_t(i) < p.display.size() ? p.display[size_t(i)] : p.palette[size_t(i)];
        put(i, LanePalette, {float(p.palette[size_t(i)] & 0xffffff), float(shown & 0xffffff), 0, 0});
    }
    if (lut) {
        float* base = &p.data[size_t(kFixedLanes) * size_t(w) * 4];
        for (size_t i = 0; i < 32768; ++i)
            base[i] = float((*p.lut)[i]);
    }
    for (int y = 0; y < ty; ++y)
        for (int x = 0; x < tx; ++x)
            put(x, kFixedLanes + lutLanes + y, p.tiles[size_t(y * tx + x)]);
}

const std::vector<uint8_t>* paletteLut(const std::vector<uint32_t>& palette)
{
    static QMutex mutex;
    static std::map<std::vector<uint32_t>, std::unique_ptr<std::vector<uint8_t>>> cache;
    QMutexLocker lock(&mutex);
    auto& slot = cache[palette];
    if (!slot) {
        auto lut = std::make_unique<std::vector<uint8_t>>(32768);
        const size_t n = std::min<size_t>(palette.size(), 128);
        for (int i = 0; i < 32768; ++i) {
            const int r = ((i >> 10) & 31) * 8 + 4, g = ((i >> 5) & 31) * 8 + 4, b = (i & 31) * 8 + 4;
            int best = 0, bestD = std::numeric_limits<int>::max();
            for (size_t k = 0; k < n; ++k) {
                const int dr = r - int((palette[k] >> 16) & 0xff), dg = g - int((palette[k] >> 8) & 0xff), db = b - int(palette[k] & 0xff);
                const int d = 2 * dr * dr + 4 * dg * dg + 3 * db * db;
                if (d < bestD) {
                    bestD = d;
                    best = int(k);
                }
            }
            (*lut)[size_t(i)] = uint8_t(best);
        }
        slot = std::move(lut);
    }
    return slot.get();
}

namespace {

constexpr int kBayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
constexpr int kPs1Dither[16] = {-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2};

// Picture source: the layer (colours-only / glitch-only looks) or the console
// framebuffer (full look): CW x CH texels of (RGB packed, palette index, alpha, luma).
struct Src {
    QImage img;
    std::vector<float> grid;
    bool valid() const { return !img.isNull() || !grid.empty(); }
};

struct ConsoleState : EffectState {
    enum class Step { Reset, Next, Same };
    std::vector<Src> frames; // newest first
    QImage prevOut, out;
    Src held;
    int64_t heldKey = std::numeric_limits<int64_t>::min();
    int64_t last = std::numeric_limits<int64_t>::min();

    // Same rules as fx::FrameHistory::push.
    Step push(int64_t time, double fps, const Src& src, size_t keep)
    {
        const double frame = double(kUsPerSecond) / std::max(1.0, fps);
        const bool have = !frames.empty() && frames.front().img.size() == src.img.size() && frames.front().grid.size() == src.grid.size();
        Step s = Step::Reset;
        if (have && time == last)
            s = Step::Same;
        else if (have && std::abs(double(time - last) - frame) < frame * 0.5)
            s = Step::Next;
        if (s == Step::Reset) {
            frames.clear();
            prevOut = QImage();
            out = QImage();
            held = Src();
            heldKey = std::numeric_limits<int64_t>::min();
        }
        if (s == Step::Same) {
            frames.front() = src;
        } else {
            if (s == Step::Next)
                prevOut = out;
            frames.insert(frames.begin(), src);
            if (frames.size() > keep)
                frames.resize(keep);
        }
        last = time;
        return s;
    }
};

inline void unpack(float packed, float& r, float& g, float& b)
{
    const int p = int(packed);
    r = float((p >> 16) & 255);
    g = float((p >> 8) & 255);
    b = float(p & 255);
}

struct Ctx {
    const float* g = nullptr;
    const float* data = nullptr;
    int dw = 0;
    int W = 0, H = 0;
    const Src* cur = nullptr;
    const Src* alt = nullptr;
    const std::vector<float>* cells = nullptr;
    int cellsW = 0, cellsH = 0;
    const QImage* overlay = nullptr;
    std::unique_ptr<fx::Pixels> curPx, altPx;
    const float* D(int x, int lane) const { return data + (size_t(lane) * size_t(dw) + size_t(x)) * 4; }
    float G(Global i) const { return g[size_t(i)]; }
};

struct Smp {
    float r, g, b, a;
    int idx;
    float luma;
};

float ditherAt(const Ctx& c, int x, int y)
{
    const int mode = int(c.G(G_DITHER));
    if (mode == 1)
        return (float(kBayer4[(y & 3) * 4 + (x & 3)]) + 0.5f) / 16.0f - 0.5f;
    if (mode == 2)
        return float(kPs1Dither[(y & 3) * 4 + (x & 3)]) / 8.0f;
    if (mode == 3)
        return ((x + y) & 1) != 0 ? 0.25f : -0.25f;
    return 0.0f;
}

int quantBits(int v, int n, float d)
{
    if (n >= 8)
        return v;
    const int L = (1 << n) - 1;
    const float step = 255.0f / float(L);
    const int k = std::clamp(int(std::floor((float(v) + d * step) / step + 0.5f)), 0, L);
    return int(std::floor(float(k) * 255.0f / float(L) + 0.5f));
}

// Console colours: returns the colour and its palette index (-1 = direct colour).
Smp quantize(const Ctx& c, int r, int g, int b, int x, int y)
{
    const int mode = int(c.G(G_QMODE));
    const float d = ditherAt(c, x, y) * c.G(G_DITHER_AMT);
    if (mode == QBits)
        return {float(quantBits(r, int(c.G(G_QBITS_R)), d)), float(quantBits(g, int(c.G(G_QBITS_G)), d)),
                float(quantBits(b, int(c.G(G_QBITS_B)), d)), 0, -1, 0};
    const int n = int(c.G(G_PAL_N));
    if (mode == QPalette && n > 0) {
        const float off = d * 32.0f;
        auto q = [&](int v) { return std::clamp(int(std::floor(float(v) + off + 0.5f)), 0, 255) >> 3; };
        const int i = (q(r) << 10) | (q(g) << 5) | q(b);
        const int idx = std::min(int(c.data[size_t(c.G(G_LANE_LUT)) * size_t(c.dw) * 4 + size_t(i)]), n - 1);
        Smp s{0, 0, 0, 0, idx, 0};
        unpack(c.D(idx, LanePalette)[0], s.r, s.g, s.b);
        return s;
    }
    if (mode == QShades && n > 0) {
        const float l = 0.299f * float(r) + 0.587f * float(g) + 0.114f * float(b) + d * 256.0f / float(n);
        const int idx = std::clamp(int(std::floor(l * float(n) / 256.0f)), 0, n - 1);
        Smp s{0, 0, 0, 0, idx, 0};
        unpack(c.D(idx, LanePalette)[0], s.r, s.g, s.b);
        return s;
    }
    return {float(r), float(g), float(b), 0, -1, 0};
}

inline int unpremul(int v, int a) { return a > 0 ? std::min(255, (v * 255 + a / 2) / a) : 0; }
inline int lumaInt(int r, int g, int b) { return (299 * r + 587 * g + 114 * b + 500) / 1000; }

Smp sampleAt(const Ctx& c, bool alt, float sx, float sy, int px, int py)
{
    const int cw = int(c.G(G_CW)), ch = int(c.G(G_CH));
    const Src& s = alt ? *c.alt : *c.cur;
    if (int(c.G(G_LOOK)) == LookFull) {
        const int ix = std::clamp(int(std::floor(sx)), 0, cw - 1), iy = std::clamp(int(std::floor(sy)), 0, ch - 1);
        const float* t = &s.grid[(size_t(iy) * size_t(cw) + size_t(ix)) * 4];
        Smp r{0, 0, 0, t[2], int(t[1]), t[3]};
        unpack(t[0], r.r, r.g, r.b);
        return r;
    }
    const fx::Pixels& in = alt ? *c.altPx : *c.curPx;
    const uint32_t p = in.sample(sx * float(c.W) / float(cw), sy * float(c.H) / float(ch), fx::Edge::Clamp);
    const int a = fx::pa(p);
    const int r = unpremul(fx::pr(p), a), g = unpremul(fx::pg(p), a), b = unpremul(fx::pb(p), a);
    Smp q{float(r), float(g), float(b), 0, -1, 0};
    const int mode = int(c.G(G_QMODE));
    if (int(c.G(G_LOOK)) == LookColours && mode >= QBits && mode <= QShades)
        q = quantize(c, r, g, b, px, py);
    q.a = float(a);
    q.luma = float(lumaInt(r, g, b));
    return q;
}

inline float wrapf(float v, float n)
{
    v -= n * std::floor(v / n);
    return v >= n ? 0.0f : v;
}

void permute(int& r, int& g, int& b, int perm)
{
    const int R = r, G = g, B = b;
    switch (perm) {
    case 1: r = R, g = B, b = G; break;
    case 2: r = G, g = R, b = B; break;
    case 3: r = G, g = B, b = R; break;
    case 4: r = B, g = R, b = G; break;
    case 5: r = B, g = G, b = R; break;
    default: break;
    }
}

uint32_t shade(const Ctx& c, int x, int y)
{
    const int GW = int(c.G(G_GW)), GH = int(c.G(G_GH)), AX = int(c.G(G_AX)), AY = int(c.G(G_AY));
    const int CW = int(c.G(G_CW)), CH = int(c.G(G_CH));
    const float CWf = float(CW), CHf = float(CH);
    const int nx = (2 * x + 1) * GW, ny = (2 * y + 1) * GH;
    const int gx = std::min(nx / (2 * c.W), GW - 1), gy = std::min(ny / (2 * c.H), GH - 1);
    const float fu = float(nx - gx * 2 * c.W) / float(2 * c.W), fv = float(ny - gy * 2 * c.H) / float(2 * c.H);
    const float u = float(gx) + fu;
    const float* rowMap = c.D(gy, LaneRowMap);
    const float* rowFx = c.D(gy, LaneRowFx);
    const float* rowEx = c.D(gy, LaneRowEx);
    const float* rowEx2 = c.D(gy, LaneRowEx2);
    const float* col = c.D(gx, LaneCols);
    float r = 0, g = 0, b = 0, a = 255;
    const bool active = gx >= AX && gx < AX + CW && gy >= AY && gy < AY + CH;
    if (!active) {
        unpack(rowEx[0] >= 0.0f ? rowEx[0] : c.G(G_BORDER), r, g, b);
    } else {
        const float lu = u - float(AX);
        float sx = rowMap[0] + rowMap[1] * lu;
        float sy = rowMap[2] + rowMap[3] * lu + (fv - 0.5f);
        bool backdrop = false, synth = false;
        const int edge = int(c.G(G_EDGE));
        if (edge == 0) {
            sx = wrapf(sx, CWf);
            sy = wrapf(sy, CHf);
        } else if (edge == 1) {
            sx = std::clamp(sx, 0.0f, CWf - 0.001f);
            sy = std::clamp(sy, 0.0f, CHf - 0.001f);
        } else if (sx < 0.0f || sy < 0.0f || sx >= CWf || sy >= CHf) {
            backdrop = true;
        }
        const float M = c.G(G_MOSAIC);
        if (M > 1.0f) {
            sx = (std::floor(sx / M) + 0.5f) * M;
            sy = (std::floor(sy / M) + 0.5f) * M;
        }
        int shift = 0, perm = -1, txor = 0, ink = -1, paper = -1, flash = 0;
        if (!backdrop) {
            const float T = c.G(G_TILE);
            const int tx = std::clamp(int(std::floor(sx / T)), 0, int(c.G(G_TX)) - 1);
            const int ty = std::clamp(int(std::floor(sy / T)), 0, int(c.G(G_TY)) - 1);
            const float* tile = c.D(tx, int(c.G(G_LANE_TILES)) + ty);
            const int m = int(tile[2]), op = m & 7;
            float lx = sx - float(tx) * T, ly = sy - float(ty) * T;
            if (op == TileCopy) {
                if ((m & 8) != 0)
                    lx = T - lx;
                if ((m & 16) != 0)
                    ly = T - ly;
                sx = wrapf(tile[0] * T + lx, CWf);
                sy = wrapf(tile[1] * T + ly, CHf);
            } else if (op == TileGarbage) {
                const int ix = std::clamp(int(std::floor(lx)), 0, int(T) - 1), iy = std::clamp(int(std::floor(ly)), 0, int(T) - 1);
                const uint32_t ts = uint32_t(int(tile[3]));
                const uint32_t bits = fx::hash32(uint32_t(iy), ts + uint32_t(ix >> 3) * 7919u, 11u);
                const int bit = 7 - (ix & 7);
                int v = int((bits >> bit) & 1u);
                if (c.G(G_GARB_BPP) > 1.5f)
                    v |= int((bits >> (bit + 8)) & 1u) << 1;
                const int n = int(c.G(G_PAL_N));
                if (v == 0 && c.G(G_GARB_BPP) < 1.5f) {
                    unpack(c.G(G_BACKDROP), r, g, b);
                } else if (n > 0) {
                    const int i = ((int(tile[0]) + v) % n + n) % n;
                    unpack(c.D(i, LanePalette)[1], r, g, b);
                } else if (v == 0) {
                    unpack(c.G(G_BACKDROP), r, g, b);
                } else {
                    unpack(float(fx::hash32(uint32_t(v), ts, 13u) & 0xffffffu), r, g, b);
                }
                synth = true;
            } else if (op == TileSolid) {
                unpack(tile[3], r, g, b);
                synth = true;
            } else if (op == TileHide) {
                backdrop = true;
            } else if (op == TileShift) {
                shift = int(tile[0]);
                perm = int(tile[1]);
                txor = int(tile[3]);
            } else if (op == TileAttr) {
                ink = int(tile[0]);
                paper = int(tile[1]);
                flash = int(tile[3]);
            }
        }
        if (backdrop) {
            unpack(c.G(G_BACKDROP), r, g, b);
            synth = true;
        }
        if (!synth) {
            const bool alt = rowEx[1] >= 0.5f;
            Smp s = sampleAt(c, alt, sx, sy, x, y);
            const int sub = int(c.G(G_SUB_OP));
            if (sub > 0 && c.G(G_SUB_AMT) > 0.0f) {
                const Smp t = sampleAt(c, alt, wrapf(sx + c.G(G_SUB_DX), CWf), wrapf(sy + c.G(G_SUB_DY), CHf), x, y);
                const float k = c.G(G_SUB_AMT);
                if (sub == 1) {
                    s.r = std::min(255.0f, s.r + t.r * k);
                    s.g = std::min(255.0f, s.g + t.g * k);
                    s.b = std::min(255.0f, s.b + t.b * k);
                } else if (sub == 2) {
                    s.r = std::max(0.0f, s.r - t.r * k);
                    s.g = std::max(0.0f, s.g - t.g * k);
                    s.b = std::max(0.0f, s.b - t.b * k);
                } else if (sub == 3) {
                    s.r = s.r + (t.r - s.r) * k * 0.5f;
                    s.g = s.g + (t.g - s.g) * k * 0.5f;
                    s.b = s.b + (t.b - s.b) * k * 0.5f;
                } else if (sub == 4) {
                    if (((gx + gy) & 1) != 0) {
                        s.r = s.r + (t.r - s.r) * k;
                        s.g = s.g + (t.g - s.g) * k;
                        s.b = s.b + (t.b - s.b) * k;
                    }
                } else {
                    s.r = std::max(s.r, t.r * k);
                    s.g = std::max(s.g, t.g * k);
                    s.b = std::max(s.b, t.b * k);
                }
                s.idx = -1;
            }
            int idx = s.idx;
            const int n = int(c.G(G_PAL_N));
            if (c.G(G_CELLS) > 0.5f && idx >= 0 && c.cells) {
                const int cx = std::clamp(int(std::floor(sx / 8.0f)), 0, c.cellsW - 1);
                const int cy = std::clamp(int(std::floor(sy / 8.0f)), 0, c.cellsH - 1);
                const float* cell = &(*c.cells)[(size_t(cy) * size_t(c.cellsW) + size_t(cx)) * 4];
                // The pixel is ink or paper by the cell's own colours; corrupted
                // attributes then recolour that bitmap (FLASH swaps them).
                const int ci0 = int(cell[0]), cp0 = int(cell[1]);
                float pr, pg, pb, ir, ig, ib, qr, qg, qb;
                unpack(c.D(idx, LanePalette)[0], pr, pg, pb);
                unpack(c.D(ci0, LanePalette)[0], ir, ig, ib);
                unpack(c.D(cp0, LanePalette)[0], qr, qg, qb);
                const int di = int(2 * (pr - ir) * (pr - ir) + 4 * (pg - ig) * (pg - ig) + 3 * (pb - ib) * (pb - ib));
                const int dp = int(2 * (pr - qr) * (pr - qr) + 4 * (pg - qg) * (pg - qg) + 3 * (pb - qb) * (pb - qb));
                int ci = ink >= 0 ? ink : ci0, cp = paper >= 0 ? paper : cp0;
                if (flash != 0 && c.G(G_FLASH) > 0.5f)
                    std::swap(ci, cp);
                idx = di <= dp ? ci : cp;
            }
            if (idx >= 0 && n > 0) {
                const int i2 = ((idx + int(c.G(G_PAL_SHIFT)) + shift) % n + n) % n;
                unpack(c.D(i2, LanePalette)[1], s.r, s.g, s.b);
            }
            if (int(c.G(G_QMODE)) == QVector) {
                const float l0 = sampleAt(c, alt, sx - 1.0f, sy, x, y).luma, l1 = sampleAt(c, alt, sx + 1.0f, sy, x, y).luma;
                const float l2 = sampleAt(c, alt, sx, sy - 1.0f, x, y).luma, l3 = sampleAt(c, alt, sx, sy + 1.0f, x, y).luma;
                const float e = std::abs(l1 - l0) + std::abs(l3 - l2);
                const float k = std::clamp(e * c.G(G_VEC_GAIN) / 255.0f, 0.0f, 1.0f);
                unpack(c.G(G_VEC_COLOR), s.r, s.g, s.b);
                s.r *= k;
                s.g *= k;
                s.b *= k;
            }
            r = s.r;
            g = s.g;
            b = s.b;
            a = s.a;
        }
        // Tone, data bits and channel wiring.
        const float gain = rowFx[0] * col[0] * c.G(G_GAIN);
        r = (r - 128.0f) * c.G(G_CONTRAST) + 128.0f + c.G(G_BRIGHT);
        g = (g - 128.0f) * c.G(G_CONTRAST) + 128.0f + c.G(G_BRIGHT);
        b = (b - 128.0f) * c.G(G_CONTRAST) + 128.0f + c.G(G_BRIGHT);
        const float l = 0.299f * r + 0.587f * g + 0.114f * b;
        r = (l + (r - l) * c.G(G_SAT)) * gain;
        g = (l + (g - l) * c.G(G_SAT)) * gain;
        b = (l + (b - l) * c.G(G_SAT)) * gain;
        if (c.G(G_TINT_AMT) > 0.0f) {
            float tr, tg, tb;
            unpack(c.G(G_TINT), tr, tg, tb);
            r = r + (r * tr / 255.0f - r) * c.G(G_TINT_AMT);
            g = g + (g * tg / 255.0f - g) * c.G(G_TINT_AMT);
            b = b + (b * tb / 255.0f - b) * c.G(G_TINT_AMT);
        }
        int ir = std::clamp(int(std::floor(r + 0.5f)), 0, 255), ig = std::clamp(int(std::floor(g + 0.5f)), 0, 255),
            ib = std::clamp(int(std::floor(b + 0.5f)), 0, 255);
        const int rx = int(rowFx[2]) ^ txor;
        ir = (((ir & int(c.G(G_AND_R))) | int(c.G(G_OR_R))) ^ int(c.G(G_XOR_R)) ^ rx) & 255;
        ig = (((ig & int(c.G(G_AND_G))) | int(c.G(G_OR_G))) ^ int(c.G(G_XOR_G)) ^ rx) & 255;
        ib = (((ib & int(c.G(G_AND_B))) | int(c.G(G_OR_B))) ^ int(c.G(G_XOR_B)) ^ rx) & 255;
        permute(ir, ig, ib, perm >= 0 ? perm : int(c.G(G_PERM)));
        const int drop = int(c.G(G_DROP));
        if ((drop & 1) != 0)
            ir = 0;
        if ((drop & 2) != 0)
            ig = 0;
        if ((drop & 4) != 0)
            ib = 0;
        r = float(ir);
        g = float(ig);
        b = float(ib);
        if (rowEx2[0] > 0.5f) { // bitmap without colour attributes: ink on paper
            const float v = 0.299f * r + 0.587f * g + 0.114f * b >= 128.0f ? 215.0f : 0.0f;
            r = g = b = v;
        }
        // Overlays.
        if (rowEx[2] > 0.0f) {
            const float nv = fx::hash01(uint32_t(gx), uint32_t(gy), uint32_t(int(c.G(G_NOISE_SEED)))) * 255.0f;
            r = r + (nv - r) * rowEx[2];
            g = g + (nv - g) * rowEx[2];
            b = b + (nv - b) * rowEx[2];
        }
        if (rowFx[1] >= 0.0f) {
            float fr, fg, fb;
            unpack(rowFx[1], fr, fg, fb);
            const float k = rowEx[3];
            r = r + (fr - r) * k;
            g = g + (fg - g) * k;
            b = b + (fb - b) * k;
            a = a + (255.0f - a) * k;
        }
        if (col[1] >= 0.0f) {
            float fr, fg, fb;
            unpack(col[1], fr, fg, fb);
            const float k = col[2];
            r = r + (fr - r) * k;
            g = g + (fg - g) * k;
            b = b + (fb - b) * k;
            a = a + (255.0f - a) * k;
        }
        const int spark = int(c.G(G_SPARK_MODE));
        if (spark > 0) {
            const uint32_t seed = uint32_t(int(c.G(G_SPARK_SEED)));
            bool lit;
            if (spark == 3) {
                const int len = std::max(1, int(c.G(G_SPARK_LEN)));
                const int off = int(fx::hash32(uint32_t(gy), 77u, seed) % uint32_t(len));
                lit = fx::hash01(uint32_t((gx + off) / len), uint32_t(gy), seed) < c.G(G_SPARK_DENS) * rowEx2[1];
            } else {
                lit = fx::hash01(uint32_t(gx), uint32_t(gy), seed) < c.G(G_SPARK_DENS) * rowEx2[1];
            }
            if (lit) {
                if (spark == 2) {
                    unpack(float(fx::hash32(uint32_t(gx), uint32_t(gy), seed + 1u) & 0xffffffu), r, g, b);
                } else if (spark == 4) {
                    const int k = int(fx::hash32(uint32_t(gx), uint32_t(gy), seed + 2u) % 5u);
                    const float cols[5] = {0.0f, float(0xffffff), float(0xff0000), float(0x00ff00), float(0x0000ff)};
                    unpack(cols[k], r, g, b);
                } else {
                    unpack(c.G(G_SPARK_COLOR), r, g, b);
                }
                a = 255.0f;
            }
        }
        const int art = int(c.G(G_ART_MODE));
        if (art > 0) {
            const int C = std::max(2, int(c.G(G_ART_CELL)));
            const uint32_t seed = uint32_t(int(c.G(G_ART_SEED)));
            const float dens = c.G(G_ART_DENS);
            int pick = -1; // 0 = C1, 1 = C2, 2 = random bits
            if (art == 1) {
                if ((gx % C) < std::max(2, C / 2) && (gy % C) < std::max(1, C / 8) && fx::hash01(uint32_t(gx / C), uint32_t(gy / C), seed) < dens)
                    pick = int(fx::hash32(uint32_t(gx / C), uint32_t(gy / C), seed + 1u) & 1u);
            } else if (art == 2) {
                if (fx::hash01(uint32_t(gx / C), uint32_t(gy / C), seed) < dens)
                    pick = ((gx >> 1) + (gy >> 1)) & 1;
            } else if (art == 3) {
                if (fx::hash01(uint32_t(gx), 0u, seed) < dens && fx::hash01(uint32_t(gx), uint32_t(gy / C), seed + 2u) < 0.85f)
                    pick = int(fx::hash32(uint32_t(gx), 1u, seed) & 1u);
            } else {
                if (fx::hash01(uint32_t(gx / C), uint32_t(gy / C), seed) < dens)
                    pick = 2;
            }
            if (pick == 2) {
                const uint32_t h = fx::hash32(uint32_t(gx), uint32_t(gy), seed + 3u);
                r = float((h & 7u) * 36u);
                g = float(((h >> 3) & 7u) * 36u);
                b = float(((h >> 6) & 3u) * 85u);
                a = 255.0f;
            } else if (pick >= 0) {
                unpack(pick == 0 ? c.G(G_ART_C1) : c.G(G_ART_C2), r, g, b);
                a = 255.0f;
            }
        }
        if (lu < rowFx[3]) {
            r = g = b = 0.0f;
            a = 255.0f;
        }
    }
    if (c.G(G_BLANK) > 0.0f) {
        float br, bg, bb;
        unpack(c.G(G_BLANK_COLOR), br, bg, bb);
        const float k = c.G(G_BLANK);
        r = r + (br - r) * k;
        g = g + (bg - g) * k;
        b = b + (bb - b) * k;
        a = a + (255.0f - a) * k;
    }
    if (c.G(G_OVERLAY) > 0.5f && c.overlay) {
        const int ow = c.overlay->width(), oh = c.overlay->height();
        const int ox = std::clamp(int(std::floor((float(gx) + fu) * float(ow) / float(GW))), 0, ow - 1);
        const int oy = std::clamp(int(std::floor((float(gy) + fv) * float(oh) / float(GH))), 0, oh - 1);
        const uint32_t o = reinterpret_cast<const uint32_t*>(c.overlay->constScanLine(oy))[ox];
        const float oa = float(fx::pa(o));
        r = r * (255.0f - oa) / 255.0f + float(fx::pr(o));
        g = g * (255.0f - oa) / 255.0f + float(fx::pg(o));
        b = b * (255.0f - oa) / 255.0f + float(fx::pb(o));
        a = std::max(a, oa);
    }
    if (int(c.G(G_LOOK)) == LookFull && c.G(G_SCREEN_AMT) > 0.0f) {
        const float cwPx = float(c.W) / float(GW), chPx = float(c.H) / float(GH);
        const int type = int(c.G(G_SCREEN_TYPE));
        if (type == ScreenCrt) {
            const float fade = std::clamp((chPx - 1.5f) * 0.5f, 0.0f, 1.0f);
            const float dark = fv > 0.5f ? (fv - 0.5f) * 2.0f : 0.0f;
            const float k = 1.0f - c.G(G_SCREEN_AMT) * 0.6f * fade * dark;
            r *= k;
            g *= k;
            b *= k;
        } else if (type == ScreenLcd) {
            const float fade = std::clamp((std::min(cwPx, chPx) - 2.0f) * 0.5f, 0.0f, 1.0f);
            // First output pixel of a console pixel (integer test: no rounding at cell edges).
            const bool gapX = x == 0 || std::min((2 * x - 1) * GW / (2 * c.W), GW - 1) != gx;
            const bool gapY = y == 0 || std::min((2 * y - 1) * GH / (2 * c.H), GH - 1) != gy;
            if (gapX || gapY) {
                const float k = 1.0f - c.G(G_SCREEN_AMT) * 0.4f * fade;
                r *= k;
                g *= k;
                b *= k;
            }
        }
    }
    const int A = std::clamp(int(std::floor(a + 0.5f)), 0, 255);
    auto ch = [&](float v) { return std::min(A, std::clamp(int(std::floor(v * float(A) / 255.0f + 0.5f)), 0, 255)); };
    return (uint32_t(A) << 24) | (uint32_t(ch(r)) << 16) | (uint32_t(ch(g)) << 8) | uint32_t(ch(b));
}

// Area-average of the layer into the console framebuffer, then console colours.
void downscale(const Ctx& c, const QImage& img, std::vector<float>& grid)
{
    const int cw = int(c.G(G_CW)), ch = int(c.G(G_CH)), W = img.width(), H = img.height();
    grid.assign(size_t(cw) * size_t(ch) * 4, 0.0f);
    const uchar* bits = img.constBits();
    const qsizetype bpl = img.bytesPerLine();
    parallelFor(ch, [&](int y0, int y1) {
        for (int gy = y0; gy < y1; ++gy) {
            const int sy0 = std::min(H - 1, int(int64_t(gy) * H / ch));
            const int sy1 = std::min(H, std::max(sy0 + 1, int(int64_t(gy + 1) * H / ch)));
            for (int gx = 0; gx < cw; ++gx) {
                const int sx0 = std::min(W - 1, int(int64_t(gx) * W / cw));
                const int sx1 = std::min(W, std::max(sx0 + 1, int(int64_t(gx + 1) * W / cw)));
                int sr = 0, sg = 0, sb = 0, sa = 0;
                for (int y = sy0; y < sy1; ++y) {
                    const auto* row = reinterpret_cast<const uint32_t*>(bits + bpl * y);
                    for (int x = sx0; x < sx1; ++x) {
                        const uint32_t p = row[x];
                        sa += fx::pa(p);
                        sr += fx::pr(p);
                        sg += fx::pg(p);
                        sb += fx::pb(p);
                    }
                }
                const int n = (sy1 - sy0) * (sx1 - sx0);
                const int A = (sa + n / 2) / n;
                const int r = unpremul((sr + n / 2) / n, A), g = unpremul((sg + n / 2) / n, A), b = unpremul((sb + n / 2) / n, A);
                const Smp q = quantize(c, r, g, b, gx, gy);
                float* t = &grid[(size_t(gy) * size_t(cw) + size_t(gx)) * 4];
                t[0] = float((int(q.r) << 16) | (int(q.g) << 8) | int(q.b));
                t[1] = float(q.idx);
                t[2] = float(A);
                t[3] = float(lumaInt(r, g, b));
            }
        }
    });
}

// Attribute cells (8x8): the two most common palette colours of each cell.
void attributeCells(const Ctx& c, const std::vector<float>& grid, std::vector<float>& cells, int& cwOut, int& chOut)
{
    const int cw = int(c.G(G_CW)), ch = int(c.G(G_CH));
    const int n = std::clamp(int(c.G(G_PAL_N)), 1, 16);
    cwOut = (cw + 7) / 8;
    chOut = (ch + 7) / 8;
    cells.assign(size_t(cwOut) * size_t(chOut) * 4, 0.0f);
    for (int cy = 0; cy < chOut; ++cy)
        for (int cx = 0; cx < cwOut; ++cx) {
            int counts[16] = {};
            for (int y = cy * 8; y < std::min(ch, cy * 8 + 8); ++y)
                for (int x = cx * 8; x < std::min(cw, cx * 8 + 8); ++x) {
                    const int i = int(grid[(size_t(y) * size_t(cw) + size_t(x)) * 4 + 1]);
                    if (i >= 0 && i < n)
                        ++counts[i];
                }
            int first = 0;
            for (int i = 1; i < n; ++i)
                if (counts[i] > counts[first])
                    first = i;
            int second = -1;
            for (int i = 0; i < n; ++i)
                if (i != first && counts[i] > 0 && (second < 0 || counts[i] > counts[second]))
                    second = i;
            float* t = &cells[(size_t(cy) * size_t(cwOut) + size_t(cx)) * 4];
            t[0] = float(first);
            t[1] = float(second < 0 ? first : second);
        }
}

// Triangles of the plan's mesh (affine texture mapping, like early 3D hardware).
QImage rasterize(const Plan& p, const QImage& src)
{
    const int W = src.width(), H = src.height();
    QImage out(W, H, QImage::Format_ARGB32_Premultiplied);
    out.fill(0xff000000u | (uint32_t(int(p[G_BACKDROP])) & 0xffffffu));
    const fx::Pixels in(src);
    struct Tri {
        float x[3], y[3], u[3], v[3];
        float minX, maxX, minY, maxY, area;
    };
    std::vector<Tri> tris;
    tris.reserve(p.mesh.size() / 3);
    for (size_t i = 0; i + 2 < p.mesh.size(); i += 3) {
        Tri t;
        for (int k = 0; k < 3; ++k) {
            const MeshVertex& m = p.mesh[i + size_t(k)];
            t.x[k] = m.x * float(W);
            t.y[k] = m.y * float(H);
            t.u[k] = m.u * float(W);
            t.v[k] = m.v * float(H);
        }
        t.area = (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.y[1] - t.y[0]) * (t.x[2] - t.x[0]);
        if (std::abs(t.area) < 1e-6f)
            continue;
        t.minX = std::min({t.x[0], t.x[1], t.x[2]});
        t.maxX = std::max({t.x[0], t.x[1], t.x[2]});
        t.minY = std::min({t.y[0], t.y[1], t.y[2]});
        t.maxY = std::max({t.y[0], t.y[1], t.y[2]});
        tris.push_back(t);
    }
    uchar* bits = out.bits();
    const qsizetype bpl = out.bytesPerLine();
    parallelFor(H, [&](int y0, int y1) {
        for (const Tri& t : tris) {
            const int ys = std::max(y0, int(std::ceil(t.minY - 0.5f))), ye = std::min(y1 - 1, int(std::floor(t.maxY - 0.5f)));
            if (ys > ye)
                continue;
            const int xs = std::max(0, int(std::ceil(t.minX - 0.5f))), xe = std::min(W - 1, int(std::floor(t.maxX - 0.5f)));
            for (int y = ys; y <= ye; ++y) {
                auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
                const float py = float(y) + 0.5f;
                for (int x = xs; x <= xe; ++x) {
                    const float px = float(x) + 0.5f;
                    const float w0 = (t.x[2] - t.x[1]) * (py - t.y[1]) - (t.y[2] - t.y[1]) * (px - t.x[1]);
                    const float w1 = (t.x[0] - t.x[2]) * (py - t.y[2]) - (t.y[0] - t.y[2]) * (px - t.x[2]);
                    const float w2 = (t.x[1] - t.x[0]) * (py - t.y[0]) - (t.y[1] - t.y[0]) * (px - t.x[0]);
                    const bool inside = t.area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                    if (!inside)
                        continue;
                    const float b0 = w0 / t.area, b1 = w1 / t.area, b2 = w2 / t.area;
                    row[x] = in.sample(b0 * t.u[0] + b1 * t.u[1] + b2 * t.u[2], b0 * t.v[0] + b1 * t.v[1] + b2 * t.v[2], fx::Edge::Clamp);
                }
            }
        }
    });
    return out;
}

} // namespace

void render(const Plan& plan, QImage& layer, std::unique_ptr<EffectState>& state)
{
    if (layer.isNull() || plan.data.empty())
        return;
    if (layer.format() != QImage::Format_ARGB32_Premultiplied)
        layer = layer.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!plan.mesh.empty())
        layer = rasterize(plan, layer);
    Ctx c;
    c.g = plan.g.data();
    c.data = plan.data.data();
    c.dw = plan.dataW;
    c.W = layer.width();
    c.H = layer.height();

    Src cur;
    if (plan.lookFull())
        downscale(c, layer, cur.grid);
    else
        cur.img = layer;

    ConsoleState* st = nullptr;
    Src altSrc;
    if (plan.usesHistory()) {
        st = &fx::stateOf<ConsoleState>(state);
        st->push(plan.time, plan.fps, cur, 2);
        if (plan.freezeKey >= 0) {
            const bool same = st->held.img.size() == cur.img.size() && st->held.grid.size() == cur.grid.size() && st->held.valid();
            if (st->heldKey != plan.freezeKey || !same) {
                st->held = cur;
                st->heldKey = plan.freezeKey;
            }
            altSrc = st->held;
        } else if (plan.needPrev) {
            altSrc = st->frames.size() > 1 ? st->frames[1] : st->frames[0];
        }
    }
    if (!altSrc.valid())
        altSrc = cur;
    c.cur = &cur;
    c.alt = &altSrc;
    if (!plan.lookFull()) {
        c.curPx = std::make_unique<fx::Pixels>(cur.img);
        c.altPx = std::make_unique<fx::Pixels>(altSrc.img);
    }
    std::vector<float> cells;
    if (plan[G_CELLS] > 0.5f && plan.lookFull()) {
        attributeCells(c, cur.grid, cells, c.cellsW, c.cellsH);
        c.cells = &cells;
    }
    if (plan[G_OVERLAY] > 0.5f && !plan.overlay.isNull())
        c.overlay = &plan.overlay;

    QImage out(c.W, c.H, QImage::Format_ARGB32_Premultiplied);
    uchar* bits = out.bits();
    const qsizetype bpl = out.bytesPerLine();
    parallelFor(c.H, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
            for (int x = 0; x < c.W; ++x)
                row[x] = shade(c, x, y);
        }
    });

    if (plan.ghost > 0 && st) {
        if (st->prevOut.size() == out.size()) {
            const int k = std::clamp(int(plan.ghost * 256.0f + 0.5f), 0, 256);
            const QImage prev = st->prevOut;
            const uchar* pb = prev.constBits();
            const qsizetype pbpl = prev.bytesPerLine();
            parallelFor(c.H, [&](int y0, int y1) {
                for (int y = y0; y < y1; ++y) {
                    uchar* o = bits + bpl * y;
                    const uchar* q = pb + pbpl * y;
                    for (int i = 0; i < c.W * 4; ++i)
                        o[i] = uchar((int(o[i]) * (256 - k) + int(q[i]) * k) >> 8);
                }
            });
        }
        st->out = out;
    }
    layer = out;
}

} // namespace vg
