#pragma once

// The Composite Signal nodes on the GPU (GpuCompositor.cpp): one fragment program, a mode a step. A signal is a
// one-channel float texture (CompositeSignal.h's): texel (x, line) is sample x of that line, in IRE. Each mode does
// what the CPU version in CompositeSignal.cpp does, sample for sample (the same order of sums where it can).

namespace gpusig {

enum Mode {
    Encode = 1,   // uPic (the picture) -> signal
    View = 2,     // uA -> a grey picture (uI0.xy: its size)
    SepMark = 3,  // uA -> 1 where clearly below the slice, -1 clearly above (0: in between) (uP0: slice, hysteresis)
    SepScan = 14, // each 0 takes the mark uI0.x samples back: a doubling of "the last clear mark"
    SepOut = 15,  // the marks -> sync pulses (uP0.z: their level)
    Mix = 4,      // uA video, uB sync, uC low pass (uI0: has video, has sync, has low pass, strip; uP0: luma, chroma, burst, k)
    Delay = 5,    // uA through the per-line delay in uTable (whole samples, fraction)
    Noise = 6,    // uA + snow, hum, spikes (uP0: snow, hum, hum phase, spikes; uSeed)
    RcInit = 7,   // a * uA (uP0.x: a)
    RcStep = 8,   // uA[n] + uP0.x * uA[n - uI0.x] (round the frame): one doubling of the RC's sum
    RcHigh = 9,   // uA - uB
    Clipper = 10, // uP0: high, low, softness
    Split = 11,   // uI0: chroma, comb, lag
    Switch = 12,  // uA, uB (uI0: across, swap, cut line, cut x; uI1: has A, has B)
    TvDraw = 13,  // the TV's raster: rows in uTable, uA the signal, uTail the frame before's end, uPrev the raster before
};

inline constexpr const char* kFragSignal = R"(
uniform sampler2D uA, uB, uC, uPic, uTable, uTail, uPrev;
uniform int uMode;
uniform ivec4 uStd;     // line, lines, samples, pal
uniform ivec4 uTiming;  // syncEnd, burstStart, burstEnd, activeStart
uniform ivec4 uTiming2; // activeEnd, firstActive, activeLines, the frame's step in subcarrier quarters (samples & 3)
uniform ivec4 uFrame;   // frame & 3, (frame * lines) & 1
uniform ivec4 uI0, uI1;
uniform vec4 uP0, uP1;
uniform uint uSeed;

int L() { return uStd.x; }
int NL() { return uStd.y; }
int N() { return uStd.z; }
int wrapN(int n) { int c = N(); if (n < 0) n += c * ((-n + c - 1) / c); return n >= c ? n - c * (n / c) : n; }
float sigAt(sampler2D t, int n) { n = clamp(n, 0, N() - 1); return texelFetch(t, ivec2(n - (n / L()) * L(), n / L()), 0).r; }
float sigWrap(sampler2D t, int n) { n = wrapN(n); return texelFetch(t, ivec2(n - (n / L()) * L(), n / L()), 0).r; }
int phaseOf(int n) { return (uFrame.x * uTiming2.w + n + 4000000) & 3; }
float sinP(int k) { return k == 1 ? 1.0 : (k == 3 ? -1.0 : 0.0); }
float cosP(int k) { return k == 0 ? 1.0 : (k == 2 ? -1.0 : 0.0); }
float palSign(int n)
{
    int line = n >= 0 ? n / L() : -((-n + L() - 1) / L());
    return ((uFrame.y + line + 2000000) & 1) == 1 ? -1.0 : 1.0;
}
float hash01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
float notchAt(sampler2D t, int n) { return (0.5 * sigAt(t, n - 2) + sigAt(t, n - 1) + sigAt(t, n) + sigAt(t, n + 1) + 0.5 * sigAt(t, n + 2)) / 4.0; }

// ---- Encode ----
// Y, U, V of picture sample xi (0 .. width-1) on picture line a (premultiplied: over black).
vec3 yuvAt(int xi, int a, float black, float range, float sat, float sign)
{
    // (bilinear by hand, the CPU's way: four texels and float weights, not the GPU's 8-bit filter)
    int width = uTiming2.x - uTiming.w;
    ivec2 sz = textureSize(uPic, 0);
    float sx = clamp((float(xi) + 0.5) * float(sz.x) / float(width) - 0.5, 0.0, float(sz.x - 1));
    float sy = clamp((float(a) + 0.5) * float(sz.y) / float(uTiming2.z) - 0.5, 0.0, float(sz.y - 1));
    int x0 = int(sx), y0 = int(sy), x1 = min(x0 + 1, sz.x - 1), y1 = min(y0 + 1, sz.y - 1);
    float fx = sx - float(x0), fy = sy - float(y0);
    vec3 c = (1.0 - fx) * (1.0 - fy) * texelFetch(uPic, ivec2(x0, y0), 0).rgb + fx * (1.0 - fy) * texelFetch(uPic, ivec2(x1, y0), 0).rgb
             + (1.0 - fx) * fy * texelFetch(uPic, ivec2(x0, y1), 0).rgb + fx * fy * texelFetch(uPic, ivec2(x1, y1), 0).rgb;
    float y = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
    return vec3(black + range * y, range * 0.492 * (c.b - y) * sat, range * 0.877 * (c.r - y) * sat * sign);
}
float encodeAt(int x, int line)
{
    bool vsync = line >= 3 && line < 6;
    float v = (vsync ? x < L() - uTiming.x : x < uTiming.x) ? -40.0 : 0.0;
    if (line < 9)
        return v;
    int n = line * L() + x;
    float sign = uStd.w == 1 ? palSign(line * L()) : 1.0;
    if (x >= uTiming.y && x < uTiming.z) {
        int k = phaseOf(n);
        return uP1.x * sinP(k) + sign * uP1.y * cosP(k); // (the burst: uP1.xy its U and V)
    }
    int a = line - uTiming2.y;
    int width = uTiming2.x - uTiming.w;
    int xi = x - uTiming.w;
    if (a < 0 || a >= uTiming2.z || uI0.x == 0 || xi < 0 || xi >= width)
        return v;
    float black = uP0.x, range = uP0.y, sat = uP0.z;
    int box = uI0.y, before = box / 2, after = box - 1 - before;
    float Y = yuvAt(xi, a, black, range, sat, sign).x, U = 0.0, V = 0.0;
    for (int k = -before; k <= after; ++k) { // (the colour's low pass: the ends of the line repeated)
        vec3 c = yuvAt(clamp(xi + k, 0, width - 1), a, black, range, sat, sign);
        U += c.y;
        V += c.z;
    }
    int k = phaseOf(n);
    return Y + U / float(box) * sinP(k) + V / float(box) * cosP(k);
}

// ---- The TV's picture ----
// The sample n of the frame (< 0: from the frame before's end, in uTail).
float tvAt(int n)
{
    if (n >= 0)
        return texelFetch(uA, ivec2(min(n, N() - 1) - (min(n, N() - 1) / L()) * L(), min(n, N() - 1) / L()), 0).r;
    int t = n + uI0.z;
    return t >= 0 ? texelFetch(uTail, ivec2(t, 0), 0).r : 0.0;
}
float tvLuma(int n) { return uI0.x == 1 ? 0.5 * (tvAt(n) + tvAt(n - uI0.y)) : (0.5 * tvAt(n - 2) + tvAt(n - 1) + tvAt(n) + tvAt(n + 1) + 0.5 * tvAt(n + 2)) / 4.0; }
// U and V of picture sample xi of the line starting at `start` (before the colour's low pass).
vec2 tvUV(float start, int xi, float hue, float sign)
{
    int n = int(start) + uTiming.w + xi;
    float c = tvAt(n) - tvLuma(n);
    int k = phaseOf(n);
    float ch = cos(hue + uP0.x), sh = sin(hue + uP0.x);
    return vec2(2.0 * c * (sinP(k) * ch + cosP(k) * sh), 2.0 * c * (cosP(k) * ch - sinP(k) * sh) * sign);
}
vec2 tvUVBox(float start, int xi, float hue, float sign)
{
    int width = uTiming2.x - uTiming.w;
    vec2 s = vec2(0.0);
    for (int k = -3; k <= 2; ++k) // (a box of 6, the ends of the line repeated)
        s += tvUV(start, clamp(xi + k, 0, width - 1), hue, sign);
    return s / 6.0;
}
vec4 tvDraw(int xi, int row)
{
    vec4 t0 = texelFetch(uTable, ivec2(0, row), 0); // start, end, clamp, drawn
    if (t0.w < 0.5)
        return texelFetch(uPrev, ivec2(xi, row), 0); // (no line on this row this frame: as it was)
    vec4 t1 = texelFetch(uTable, ivec2(1, row), 0); // hue, colour gain, sign, a line before with colour
    vec4 t2 = texelFetch(uTable, ivec2(2, row), 0); // the line before's start, hue, sign
    int width = uTiming2.x - uTiming.w;
    int visible = clamp(int(t0.y) - (int(t0.x) + uTiming.w), 0, width);
    if (xi >= visible)
        return vec4(0.0, 0.0, 0.0, 1.0); // (the beam had gone back already)
    float Y = tvLuma(int(t0.x) + uTiming.w + xi);
    vec2 uv = tvUVBox(t0.x, xi, t1.x, t1.z);
    if (uStd.w == 1 && t1.w > 0.5) // PAL's delay line: averaged with the line before's
        uv = 0.5 * (uv + tvUVBox(t2.x, xi, t2.y, t2.z));
    float black = uStd.w == 1 ? 0.0 : 7.5, range = 100.0 - black;
    float yy = (Y - t0.z - black) / range * uP0.y + uP0.z;
    float u = uv.x * t1.y * uP0.y, w = uv.y * t1.y * uP0.y;
    float r = yy + w / 0.877, b = yy + u / 0.492;
    float g = (yy - 0.299 * r - 0.114 * b) / 0.587;
    return vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    int x = p.x, line = p.y, n = line * L() + x;
    float v = 0.0;
    if (uMode == 1) {
        v = encodeAt(x, line);
    } else if (uMode == 2) {
        int sl = min(NL() - 1, p.y * NL() / uI0.y), sx = min(L() - 1, p.x * L() / uI0.x);
        float g = clamp((texelFetch(uA, ivec2(sx, sl), 0).r + 40.0) / 140.0, 0.0, 1.0);
        fragColor = vec4(vec3(g), 1.0);
        return;
    } else if (uMode == 3) {
        // The comparator's state at n is the last clear mark at or before n (the frame's first sample marks it).
        float s = sigAt(uA, n);
        v = s < uP0.x ? 1.0 : (s >= uP0.x + uP0.y ? -1.0 : (n == 0 ? -1.0 : 0.0));
    } else if (uMode == 14) {
        v = sigAt(uA, n);
        if (v == 0.0 && n - uI0.x >= 0)
            v = sigAt(uA, n - uI0.x);
    } else if (uMode == 15) {
        v = sigAt(uA, n) > 0.0 ? uP0.z : 0.0;
    } else if (uMode == 4) {
        float k = uP0.w;
        float b[5];
        for (int j = 0; j < 5; ++j) {
            int i = clamp(n + j - 2, 0, N() - 1);
            float a = uI0.x == 1 ? sigAt(uA, i) : 0.0;
            b[j] = uI0.z == 1 ? a * (1.0 - k) + sigAt(uC, i) * k : a;
        }
        float y = (0.5 * b[0] + b[1] + b[2] + b[3] + 0.5 * b[4]) / 4.0;
        float c = b[2] - y;
        if (uI0.w == 1)
            y = max(y, 0.0);
        v = uP0.x * y + ((x >= uTiming.y && x < uTiming.z) ? uP0.z : uP0.y) * c + (uI0.y == 1 ? sigAt(uB, n) : 0.0);
    } else if (uMode == 5) {
        vec2 d = texelFetch(uTable, ivec2(line, 0), 0).xy; // (the line's start in the frame, whole, and the fraction)
        int a = wrapN(int(d.x) + x);
        v = sigWrap(uA, a) * (1.0 - d.y) + sigWrap(uA, a + 1) * d.y;
    } else if (uMode == 6) {
        float bar = uP0.y * sin(6.283185307179586 * (float(line) / float(NL()) + uP0.z));
        int from = L(), to = L();
        float level = 0.0;
        if (uP0.w > 0.0 && hash01(uint(line), 1u, uSeed) < uP0.w * 0.15) {
            from = int(hash01(uint(line), 2u, uSeed) * float(L()));
            to = from + 10 + int(hash01(uint(line), 3u, uSeed) * 180.0);
            level = hash01(uint(line), 4u, uSeed) < 0.5 ? -60.0 : 110.0;
        }
        v = sigAt(uA, n) + bar;
        if (uP0.x > 0.0)
            v += uP0.x * 2.0 * (hash01(uint(x), uint(line), uSeed) - 0.5);
        if (x >= from && x < to)
            v = level;
    } else if (uMode == 7) {
        v = uP0.x * sigAt(uA, n);
    } else if (uMode == 8) {
        v = sigAt(uA, n) + uP0.x * sigWrap(uA, n - uI0.x);
    } else if (uMode == 9) {
        v = sigAt(uA, n) - sigAt(uB, n);
    } else if (uMode == 10) {
        float hi = uP0.x, lo = uP0.y, soft = uP0.z;
        v = sigAt(uA, n);
        if (soft <= 0.001)
            v = clamp(v, lo, hi);
        else if (v > hi - soft)
            v = hi - soft * exp(-(v - (hi - soft)) / soft);
        else if (v < lo + soft)
            v = lo + soft * exp((v - (lo + soft)) / soft);
    } else if (uMode == 11) {
        float here = sigAt(uA, n);
        float y = uI0.y == 1 ? 0.5 * (here + sigWrap(uA, n - uI0.z)) : notchAt(uA, n);
        v = uI0.x == 1 ? here - y : y;
    } else if (uMode == 12) {
        bool second = (uI0.x == 1 ? x >= uI0.w : line >= uI0.z) != (uI0.y == 1);
        v = second ? (uI1.y == 1 ? sigAt(uB, n) : 0.0) : (uI1.x == 1 ? sigAt(uA, n) : 0.0);
    } else if (uMode == 13) {
        fragColor = tvDraw(p.x, p.y);
        return;
    }
    fragColor = vec4(v, 0.0, 0.0, 1.0);
}
)";

} // namespace gpusig
