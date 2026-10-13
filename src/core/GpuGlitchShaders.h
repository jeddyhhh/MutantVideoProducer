#pragma once

// GLSL for the glitch / analog / psychedelic effects that need more than a
// warp: one "uber" fragment shader whose uMode picks the effect (or pass of
// a multi-pass effect). Formulas mirror GlitchEffects.cpp line by line, so
// the GPU and CPU renderers agree; pixel reads use texelFetch with the same
// edge rules and 8-bit bilinear weights as fx::Pixels.

namespace gpufx {

enum Mode {
    Strobe = 0, PixelStretch, BlockGlitch, Slice, TvStatic, VhsTracking, Ghosting, Halftone, Emboss, GradientMap,
    Databend, OldFilm, Interlace, TimeSplit, SlitScan, Trails, Feedback, MoshLuma, MoshMotion, MoshWarp,
    VhsEncode, BoxBlur, VhsCompose, NtscEncode, NtscDemod, NtscCompose, DitherAverage, DitherMap, DitherExpand,
    AsciiAverage, AsciiCompose, NeonEdges, NeonCombine, JpegToYcc, JpegHalfChroma, JpegForward, JpegInverse, JpegCompose,
    PixelSortInit, PixelSortScan, PixelSortKeys, PixelSortStep, PixelSortOutput,
    DatabendLoad, DatabendEchoStep, DatabendEchoMix, DitherSums, DitherResolve, GlitchPost, EffectMask, SegmentComposite,
};
static_assert(JpegCompose == 37 && PixelSortOutput == 42 && DitherResolve == 47 && GlitchPost == 48 && EffectMask == 49 && SegmentComposite == 50, "modes must match the shader");

inline const char* kFragFx = R"(
uniform sampler2D uTex;    // unit 0: the layer (RGBA8) or the pass input
uniform sampler2D uTex1;   // unit 1
uniform sampler2D uTex2;   // unit 2
uniform sampler2D uData;   // unit 3: float data (per-row plans, tables)
uniform vec2 uSize;        // size of uTex in pixels
uniform int uMode;
uniform vec4 uA;
uniform vec4 uB;
uniform vec4 uC;
uniform ivec4 uI;
uniform ivec4 uJ;
uniform int uSeed;
uniform vec4 uArr[64];
uniform int uQ0[64];
uniform int uQ1[64];
const float PI = 3.14159265358979;

// ---- helpers (same as EffectHelpers.h) ----
uint useed(int extra) { return uint(uSeed) + uint(extra); }
float hash01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
int floordiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
int edgeIdx(int i, int n, int e) {
    if (e == 1) return i - n * floordiv(i, n);
    if (e == 2) { int m = i - 2 * n * floordiv(i, 2 * n); return m < n ? m : 2 * n - 1 - m; }
    return clamp(i, 0, n - 1);
}
vec4 B(vec4 c) { return floor(c * 255.0 + 0.5); }   // texel -> bytes
vec4 fetchB(sampler2D t, ivec2 p, ivec2 size, int e) {
    if (e == 3 && (p.x < 0 || p.y < 0 || p.x >= size.x || p.y >= size.y)) return vec4(0.0);
    return B(texelFetch(t, ivec2(edgeIdx(p.x, size.x, e), edgeIdx(p.y, size.y, e)), 0));
}
vec4 at(ivec2 p) { return fetchB(uTex, p, ivec2(uSize), 0); }
// Bilinear sample in bytes with 8-bit weights (fx::Pixels::sample).
vec4 sampleB(sampler2D t, vec2 pix, ivec2 size, int e) {
    pix -= 0.5;
    vec2 f0 = floor(pix);
    ivec2 i0 = ivec2(f0);
    float wx = floor((pix.x - f0.x) * 256.0 + 0.5), wy = floor((pix.y - f0.y) * 256.0 + 0.5);
    vec4 p00 = fetchB(t, i0, size, e), p10 = fetchB(t, i0 + ivec2(1, 0), size, e);
    vec4 p01 = fetchB(t, i0 + ivec2(0, 1), size, e), p11 = fetchB(t, i0 + ivec2(1, 1), size, e);
    vec4 a = floor((p00 * (256.0 - wx) + p10 * wx) / 256.0);
    vec4 b = floor((p01 * (256.0 - wx) + p11 * wx) / 256.0);
    return floor((a * (256.0 - wy) + b * wy) / 256.0);
}
vec4 smp(vec2 pix, int e) { return sampleB(uTex, pix, ivec2(uSize), e); }
// fx::packf / fx::pack: rounded, clamped, colour <= alpha; returns a texel.
vec4 packf(float a, vec3 c) {
    float A = clamp(floor(a + 0.5), 0.0, 255.0);
    vec3 C = min(clamp(floor(c + 0.5), 0.0, 255.0), vec3(A));
    return vec4(C, A) / 255.0;
}
vec4 packv(vec4 b) { return packf(b.a, b.rgb); }
float lumaB(vec4 b) { return (0.299 * b.r + 0.587 * b.g + 0.114 * b.b) / 255.0; }
vec3 toYiq(vec3 c) { return vec3(0.299 * c.r + 0.587 * c.g + 0.114 * c.b, 0.596 * c.r - 0.274 * c.g - 0.322 * c.b, 0.211 * c.r - 0.523 * c.g + 0.312 * c.b); }
// ---- dither (core/Dither.cpp): palette uArr (uI.y colours) or uI.z levels per channel; uJ.z perceptual ----
uint ditherHash(uint x, uint y, uint seed) {
    uint h = x * 0x8da6b343u ^ y * 0xd8163841u ^ seed * 0xcb1ab31fu;
    h ^= h >> 15u; h *= 0x2c1b3c6du; h ^= h >> 12u; h *= 0x297a2d39u; h ^= h >> 15u;
    return h;
}
vec3 ditherColor(int i) {
    int l = uI.z;
    if (l == 0) return uArr[i].rgb;
    ivec3 q = ivec3(i / (l * l), (i / l) % l, i % l);
    return vec3((q * 255 * 2 + (l - 1)) / (2 * (l - 1)));
}
vec3 oklab(vec3 c) {
    c = clamp(c, 0.0, 255.0) / 255.0;
    vec3 lin = vec3(c.r <= 0.04045 ? c.r / 12.92 : pow((c.r + 0.055) / 1.055, 2.4), c.g <= 0.04045 ? c.g / 12.92 : pow((c.g + 0.055) / 1.055, 2.4),
                    c.b <= 0.04045 ? c.b / 12.92 : pow((c.b + 0.055) / 1.055, 2.4));
    float l = pow(0.4122214708 * lin.r + 0.5363325363 * lin.g + 0.0514459929 * lin.b, 1.0 / 3.0);
    float m = pow(0.2119034982 * lin.r + 0.6806995451 * lin.g + 0.1073969566 * lin.b, 1.0 / 3.0);
    float s = pow(0.0883024619 * lin.r + 0.2817188376 * lin.g + 0.6299787005 * lin.b, 1.0 / 3.0);
    return vec3(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
                0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s);
}
int ditherNearest(vec3 v) {
    int l = uI.z;
    if (l > 0) {
        ivec3 q = clamp(ivec3(floor(v * float(l - 1) / 255.0 + 0.5)), ivec3(0), ivec3(l - 1));
        return (q.r * l + q.g) * l + q.b;
    }
    int best = 0;
    float bestD = 1e30;
    if (uJ.z == 1) {
        vec3 p = oklab(v);
        for (int i = 0; i < uI.y; ++i) {
            vec3 d = p - oklab(uArr[i].rgb);
            float dd = dot(d, d);
            if (dd < bestD) { bestD = dd; best = i; }
        }
        return best;
    }
    for (int i = 0; i < uI.y; ++i) {
        vec3 d = v - uArr[i].rgb;
        float dd = d.r * d.r * 0.3 + d.g * d.g * 0.59 + d.b * d.b * 0.11;
        if (dd < bestD) { bestD = dd; best = i; }
    }
    return best;
}
vec3 fromYiq(vec3 c) { return vec3(c.x + 0.956 * c.y + 0.621 * c.z, c.x - 0.272 * c.y - 0.647 * c.z, c.x - 1.106 * c.y + 1.703 * c.z); }
vec4 rowPlan(int y) { return texelFetch(uData, ivec2(y, 0), 0); }
vec4 combineChannels(vec4 r, vec4 g, vec4 b) { float a = max(max(r.a, g.a), b.a); return packf(a, vec3(r.r, g.g, b.b)); }
vec4 glitchColor(vec4 p, int mode) {
    ivec4 q = ivec4(p);
    if (mode == 0) return packf(p.a, p.bgr);
    if (mode == 1) return packf(p.a, vec3(p.a) - p.rgb);
    if (mode == 2) return packf(p.a, vec3(p.r, 0.0, 0.0));
    if (mode == 3) return packf(p.a, vec3(0.0, p.g, p.b));
    if (mode == 4) return packf(p.a, vec3(q.rgb & ivec3(0xc0)));
    return packf(p.a, vec3((q.r ^ 0x5a) & 0xff, (q.g ^ 0xa5) & 0xff, q.b));
}
float dotScreen(vec2 p, float ang, int kind, float cell) {
    float c = cos(ang), s = sin(ang);
    float u = p.x * c + p.y * s, v = -p.x * s + p.y * c;
    float cu = (floor(u / cell) + 0.5) * cell, cv = (floor(v / cell) + 0.5) * cell;
    vec4 src = smp(vec2(cu * c - cv * s, cu * s + cv * c), 0);
    float al = max(1.0, src.a);
    float val;
    if (kind == 0) val = 1.0 - lumaB(src) * 255.0 / al;
    else if (kind <= 4) {
        vec3 n = src.rgb / al;
        float k = 1.0 - max(max(n.r, n.g), n.b);
        if (kind == 4) val = k;
        else { float vv = kind == 1 ? n.r : (kind == 2 ? n.g : n.b); val = k >= 0.999 ? 0.0 : (1.0 - vv - k) / (1.0 - k); }
    } else val = (kind == 5 ? src.r : (kind == 6 ? src.g : src.b)) / al;
    float radius = cell * 0.70710678 * sqrt(clamp(val, 0.0, 1.0));
    return clamp(radius - length(vec2(u - cu, v - cv)) + 0.5, 0.0, 1.0);
}
vec4 hsvColor(float h, float s, float v) {
    h = fract(h) * 6.0;
    int i = int(h);
    float f = h - float(i), p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    vec3 c = i == 0 ? vec3(v, t, p) : i == 1 ? vec3(q, v, p) : i == 2 ? vec3(p, v, t) : i == 3 ? vec3(p, q, v) : i == 4 ? vec3(t, p, v) : vec3(v, p, q);
    return vec4(floor(c * 255.0 + 0.5), 255.0);
}
float dctCos(int k, int i, int n) {
    return (k == 0 ? sqrt(1.0 / float(n)) : sqrt(2.0 / float(n))) * cos(float((2 * i + 1) * k) * PI / float(2 * n));
}
float roundAway(float x) { return sign(x) * floor(abs(x) + 0.5); }
)"
 R"(void main() {
    ivec2 ip = ivec2(floor(vPix));
    ivec2 size = ivec2(uSize);
    int x = ip.x, y = ip.y;
    vec4 c0 = at(ip);

    if (uMode == 0) {                       // strobe: uI.x mode, uI.y opacity (0..256); uA.rgb flash colour
        vec4 f;
        if (uI.x == 1) f = B(packf(c0.a, vec3(c0.a) - c0.rgb));
        else if (uI.x == 2) f = B(packf(c0.a, vec3(0.0)));
        else if (uI.x == 3) f = vec4(0.0);
        else if (uI.x == 6) { float v = lumaB(c0) * 255.0 >= c0.a * 0.5 ? c0.a : 0.0; f = B(packf(c0.a, vec3(v))); }
        else f = B(packf(255.0, uA.rgb));
        if (uI.y < 256) f = floor((c0 * float(256 - uI.y) + f * float(uI.y)) / 256.0);
        fragColor = f / 255.0;
    } else if (uMode == 1) {                // pixel stretch: uI.x dir; uA.x position, uA.y jitter, uA.z length
        bool horizontal = uI.x <= 1;
        int n = horizontal ? size.x : size.y;
        int line = horizontal ? y : x, along = horizontal ? x : y;
        float off = uA.y > 0.0 ? (hash01(uint(line), 21u, useed(0)) - 0.5) * 2.0 * uA.y : 0.0;
        int a0 = clamp(int(uA.x * float(n) + off), 0, n - 1);
        bool fwd = uI.x == 0 || uI.x == 2;
        bool take = fwd ? (along > a0 && along <= a0 + int(uA.z * float(n - 1 - a0))) : (along < a0 && along >= a0 - int(uA.z * float(a0)));
        fragColor = packv(take ? at(horizontal ? ivec2(a0, y) : ivec2(x, a0)) : c0);
    } else if (uMode == 2) {                // block glitch: uI.x bh, uI.y bw, uI.z colour mode; uA amount, shift, colourP, vertical shift; uB.x static
        int by = y / uI.x;
        int stagger = int(hash01(uint(by), 3u, useed(0)) * float(uI.y));
        int bx = (x + stagger) / uI.y;
        if (hash01(uint(bx), uint(by), useed(0)) >= uA.x) { fragColor = packv(c0); return; }
        if (uB.x > 0.0 && hash01(uint(bx), uint(by), useed(5)) < uB.x) {
            uint nn = hash32(uint(x), uint(y), useed(6));
            fragColor = packf(255.0, vec3(float(nn & 255u), float((nn >> 8u) & 255u), float((nn >> 16u) & 255u)));
            return;
        }
        int dx = int((hash01(uint(bx), uint(by), useed(1)) - 0.5) * 2.0 * uA.y);
        int dy = int((hash01(uint(bx), uint(by), useed(2)) - 0.5) * 2.0 * uA.w * float(uI.x));
        vec4 p = fetchB(uTex, ip + ivec2(dx, dy), size, 1);
        if (hash01(uint(bx), uint(by), useed(3)) < uA.z)
            fragColor = glitchColor(p, uI.z == 0 ? int(hash32(uint(bx), uint(by), useed(4)) % 6u) : uI.z - 1);
        else fragColor = packv(p);
    } else if (uMode == 3) {                // slice: plan per row (or column): shift, torn, colour mode + 1; uI.x vertical, uI.y edge; uA.x rgb split
        bool vert = uI.x == 1;
        vec4 r = rowPlan(vert ? x : y);
        if (r.y == 0.0) { fragColor = packv(c0); return; }
        vec2 p = vec2(float(x) + 0.5 - (vert ? 0.0 : r.x), float(y) + 0.5 - (vert ? r.x : 0.0));
        vec2 sd = vert ? vec2(0.0, uA.x) : vec2(uA.x, 0.0);
        vec4 v = uA.x != 0.0 ? B(combineChannels(smp(p - sd, uI.y), smp(p, uI.y), smp(p + sd, uI.y))) : smp(p, uI.y);
        fragColor = r.z > 0.0 ? glitchColor(v, int(r.z) - 1) : packv(v);
    } else if (uMode == 4) {                // tv static: rows (bar gain, lost); uA.x amount; uI.x colour, uI.y grain
        vec4 r = rowPlan(y);
        bool lost = r.y > 0.0;
        float mixK = lost ? 1.0 : uA.x;
        uint h = hash32(uint(x / uI.y), uint(y / uI.y), useed(62));
        float n = float(h & 255u);
        float nr = uI.x == 1 ? float((h >> 8u) & 255u) : n, ng = uI.x == 1 ? float((h >> 16u) & 255u) : n;
        vec3 c = vec3(c0.r + (nr - c0.r) * mixK, c0.g + (ng - c0.g) * mixK, c0.b + (n - c0.b) * mixK);
        float a = lost ? 255.0 : c0.a + (255.0 - c0.a) * mixK;
        fragColor = packf(a, c * r.x);
    } else if (uMode == 5) {                // vhs tracking: rows (weight, offset); uA.x noise; uI.x noise cell
        vec4 r = rowPlan(y);
        if (r.x <= 0.0) { fragColor = packv(c0); return; }
        vec4 p = smp(vec2(float(x) + 0.5 - r.y, float(y) + 0.5), 0);
        if (hash01(uint(x / uI.x), uint(y), useed(52)) < uA.x * r.x * 0.6) {
            float v = 150.0 + float(int(hash01(uint(x), uint(y), useed(53)) * 105.0));
            fragColor = packf(255.0, vec3(v));
        } else {
            float lift = float(int(30.0 * r.x));
            fragColor = packf(p.a, p.rgb + lift);
        }
    } else if (uMode == 6) {                // ghosting: uA.x offset, uA.y strength, uI.x echoes
        vec4 acc = c0;
        float total = 1.0;
        for (int k = 1; k <= uI.x; ++k) {
            float wk = uA.y * pow(0.6, float(k - 1));
            acc += smp(vec2(float(x) + 0.5 - uA.x * float(k), float(y) + 0.5), 0) * wk;
            total += wk;
        }
        fragColor = packf(max(c0.a, acc.a / total), acc.rgb / total);
    } else if (uMode == 7) {                // halftone: uA.x cell, uA.y angle, uI.x mode, uB ink, uC paper
        vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
        float a = c0.a, cell = uA.x, ang = uA.y;
        if (uI.x == 0) {
            float k = dotScreen(p, ang, 0, cell);
            fragColor = packf(a, (uC.rgb + (uB.rgb - uC.rgb) * k) * a / 255.0);
        } else if (uI.x == 1) {
            float cC = dotScreen(p, ang + 15.0 * PI / 180.0, 1, cell), cM = dotScreen(p, ang + 75.0 * PI / 180.0, 2, cell);
            float cY = dotScreen(p, ang, 3, cell), cK = dotScreen(p, ang + 45.0 * PI / 180.0, 4, cell);
            vec3 rgb = 255.0 * vec3((1.0 - cC) * (1.0 - cK), (1.0 - cM) * (1.0 - cK), (1.0 - cY) * (1.0 - cK));
            fragColor = packf(a, rgb * a / 255.0);
        } else {
            float r = dotScreen(p, ang, 5, cell), g = dotScreen(p, ang + 30.0 * PI / 180.0, 6, cell), b = dotScreen(p, ang + 60.0 * PI / 180.0, 7, cell);
            fragColor = packf(a, vec3(r, g, b) * a);
        }
    } else if (uMode == 8) {                // emboss: uA dx, dy, strength, mix; uI.x keep colour
        vec2 p = vec2(float(x) + 0.5, float(y) + 0.5);
        float e = (lumaB(smp(p - uA.xy, 0)) - lumaB(smp(p + uA.xy, 0))) * uA.z;
        vec3 c = uI.x == 1 ? c0.rgb + e * 255.0 : vec3((0.5 + e) * c0.a);
        fragColor = packf(c0.a, c0.rgb + (c - c0.rgb) * uA.w);
    } else if (uMode == 9) {                // gradient map: data = 256 colours (bytes); uA.x mix
        if (c0.a <= 0.0) { fragColor = packv(c0); return; }
        int l = min(255, int(lumaB(c0) * 255.0 * 255.0 / c0.a));
        vec3 g = texelFetch(uData, ivec2(l, 0), 0).rgb * (c0.a / 255.0);
        fragColor = packf(c0.a, c0.rgb + (g - c0.rgb) * uA.x);
    } else if (uMode == 10) {               // databend (non-echo): uI.x mode, uI.y chunk, uI.z bits, uI.w mix; uA.x amount;
                                            // uJ.x byte shift, uJ.y stride error
        int n = size.x * size.y * 3;
        vec3 outc;
        for (int ch = 0; ch < 3; ++ch) {
            int i = (y * size.x + x) * 3 + ch;
            int j = i;
            int cidx = i / uI.y;
            if (uI.x == 1 && (cidx & 1) == 0) {
                int s0 = cidx * uI.y, e0 = min(n, s0 + uI.y);
                j = s0 + e0 - 1 - i;
            }
            if (uI.x == 4) j = (i + uJ.x) % n;
            if (uI.x == 5) { int jj = y * (size.x * 3 + uJ.y) + x * 3 + ch; j = jj - n * floordiv(jj, n); }
            if (uI.x == 6 && cidx > 0 && hash01(uint(cidx), 9u, useed(0)) <= uA.x) j = i - uI.y;
            vec4 src = at(ivec2((j / 3) % size.x, (j / 3) / size.x));
            int v = int(j % 3 == 0 ? src.r : (j % 3 == 1 ? src.g : src.b));
            if (uI.x == 2) v = ((v << uI.z) | (v >> (8 - uI.z))) & 255;
            if (uI.x == 3 && hash01(uint(cidx), 7u, useed(0)) <= uA.x) v ^= int(hash32(uint(cidx), 8u, useed(0)) & 255u);
            float orig = ch == 0 ? c0.r : (ch == 1 ? c0.g : c0.b);
            outc[ch] = floor((orig * float(256 - uI.w) + float(v) * float(uI.w)) / 256.0);
        }
        fragColor = packf(c0.a, outc);)"
 R"(    } else if (uMode == 11) {               // old film: uA dx, dy, flicker, sepia; uB grain, vignette, scratch width, scratches; uI.x specks
        vec4 p = smp(vec2(float(x) + 0.5 - uA.x, float(y) + 0.5 - uA.y), 0);
        vec3 c = p.rgb;
        float l = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        c += (vec3(l * 1.07 + 12.0, l * 0.95 + 4.0, l * 0.75 - 6.0) - c) * uA.w;
        float k = uA.z;
        if (uB.y > 0.0) {
            float u = (float(x) + 0.5) / uSize.x - 0.5, v = (float(y) + 0.5) / uSize.y - 0.5;
            k *= 1.0 - uB.y * min(1.0, (u * u + v * v) * 2.2);
        }
        float n = uB.x > 0.0 ? (hash01(uint(x), uint(y), useed(84)) - 0.5) * uB.x * 80.0 : 0.0;
        vec4 o = B(packf(p.a, c * k + n));
        for (int i = 0; i < int(uB.w); ++i) {
            int sx = int(texelFetch(uData, ivec2(i, y), 0).r);
            if (x >= sx && x < sx + int(uB.z))
                o = uArr[48 + i].x > 0.0 ? B(packf(o.a, floor(o.rgb / 3.0))) : B(packf(o.a, o.rgb + vec3(110.0, 110.0, 100.0)));
        }
        for (int i = 0; i < uI.x; ++i) {
            vec4 sp = uArr[i];
            float cov = clamp(sp.z + 0.5 - length(vec2(float(x) + 0.5, float(y) + 0.5) - sp.xy), 0.0, 1.0);
            if (cov <= 0.0) continue;
            float target = sp.w > 0.0 ? 15.0 : 235.0;
            o = B(packf(o.a, o.rgb + (vec3(target) - o.rgb) * cov));
        }
        fragColor = o / 255.0;
    } else if (uMode == 12) {               // interlace: uTex1 previous frame; uI.x mode; uA.x shift
        if ((y & 1) == 0) { fragColor = packv(c0); return; }
        if (uI.x == 0) fragColor = packv(fetchB(uTex1, ip, size, 0));
        else if (uI.x == 1) fragColor = packv(smp(vec2(float(x) + 0.5 - uA.x, float(y) + 0.5), 0));
        else fragColor = packv(at(ip - ivec2(0, 1)));
    } else if (uMode == 13) {               // RGB time split: uTex1 green frame, uTex2 blue frame
        fragColor = combineChannels(c0, fetchB(uTex1, ip, size, 0), fetchB(uTex2, ip, size, 0));
    } else if (uMode == 14) {               // slit scan pass: uI.x direction, uI.y this frame, uI.z span, uI.w frames kept
        float f;
        vec2 uv = (vec2(ip) + 0.5) / uSize;
        if (uI.x == 0) f = uv.y; else if (uI.x == 1) f = 1.0 - uv.y; else if (uI.x == 2) f = uv.x; else if (uI.x == 3) f = 1.0 - uv.x;
        else f = min(1.0, length(uv - 0.5) * 2.0);
        int ago = min(uI.w - 1, int(f * float(uI.z - 1) + 0.5));
        if (ago != uI.y) discard;
        fragColor = packv(c0);
    } else if (uMode == 15) {               // trails: uTex1 previous output; uA.x decay; uI.x mode
        vec4 o = fetchB(uTex1, ip, size, 0);
        vec4 od = o * uA.x;
        if (uI.x == 0) fragColor = packf(max(c0.a, od.a), max(c0.rgb, od.rgb));
        else if (uI.x == 1) fragColor = packf(c0.a * (1.0 - uA.x) + od.a, c0.rgb * (1.0 - uA.x) + od.rgb);
        else if (uI.x == 2) fragColor = packf(max(c0.a, od.a), c0.rgb + od.rgb * 0.5);
        else { float k = 1.0 - uA.x; fragColor = packf(c0.a, min(c0.rgb, o.rgb + (vec3(255.0) - o.rgb) * k)); }
    } else if (uMode == 16) {               // feedback: uTex1 previous output; uA zoom, cos, sin, decay; uB offset, hue cos/sin; uC.x input mix; uI.x lighten
        vec2 c = uSize * 0.5;
        vec2 u = (vec2(float(x) + 0.5, float(y) + 0.5) - c - uB.xy) / uA.x;
        vec4 f = sampleB(uTex1, c + vec2(u.x * uA.y - u.y * uA.z, u.x * uA.z + u.y * uA.y), size, 3);
        vec3 q = toYiq(f.rgb);
        vec3 rgb = fromYiq(vec3(q.x, q.y * uB.z - q.z * uB.w, q.y * uB.w + q.z * uB.z)) * uA.w;
        float fa = f.a * uA.w;
        if (uI.x == 1) fragColor = packf(max(c0.a, fa), max(c0.rgb, rgb));
        else fragColor = packf(fa + (c0.a - fa) * uC.x, rgb + (c0.rgb - rgb) * uC.x);
    } else if (uMode == 17) {               // datamosh luma: quarter-res; uI.x factor; uSize = full size
        int f = uI.x, sum = 0;
        for (int j = 0; j < f; ++j)
            for (int i = 0; i < f; ++i) {
                ivec4 q = ivec4(at(ivec2(min(x * f + i, size.x - 1), min(y * f + j, size.y - 1))));
                sum += (q.r * 77 + q.g * 150 + q.b * 29) >> 8;
            }
        fragColor = vec4(float(sum / (f * f)) / 255.0, 0.0, 0.0, 1.0);
    } else if (uMode == 18) {               // datamosh motion: uTex cur luma, uTex1 prev luma; uI bs, range, lw, lh
        int bs = uI.x, R = uI.y, lw = uI.z, lh = uI.w;
        int x0 = x * bs, y0 = y * bs;
        int best = 0x7fffffff, bdx = 0, bdy = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (int dy = (pass == 0 ? 0 : -R); dy <= (pass == 0 ? 0 : R); ++dy)
                for (int dx = (pass == 0 ? 0 : -R); dx <= (pass == 0 ? 0 : R); ++dx) {
                    int total = 0;
                    for (int yy = y0; yy < min(lh, y0 + bs) && total < best; ++yy) {
                        int sy = clamp(yy + dy, 0, lh - 1);
                        for (int xx = x0; xx < min(lw, x0 + bs); ++xx) {
                            int sx = clamp(xx + dx, 0, lw - 1);
                            total += abs(int(texelFetch(uTex, ivec2(xx, yy), 0).r * 255.0 + 0.5) - int(texelFetch(uTex1, ivec2(sx, sy), 0).r * 255.0 + 0.5));
                        }
                    }
                    if (total < best) { best = total; bdx = dx; bdy = dy; }
                }
            if (pass == 0 && best <= bs * bs * 2) break;
        }
        fragColor = vec4(float(bdx * uJ.x), float(bdy * uJ.x), 0.0, 1.0);
    } else if (uMode == 19) {               // datamosh warp: uTex current, uTex1 previous output, uTex2 vectors; uI blockPx, bx, by, a256; uA.x motion
        int by = min(uI.z - 1, y / uI.x), bx = min(uI.y - 1, x / uI.x);
        vec2 d = texelFetch(uTex2, ivec2(bx, by), 0).rg;
        int mx = int(roundAway(d.x * uA.x)), my = int(roundAway(d.y * uA.x));
        vec4 moved = fetchB(uTex1, ivec2(clamp(x + mx, 0, size.x - 1), clamp(y + my, 0, size.y - 1)), size, 0);
        float a = float(uI.w);
        fragColor = floor((moved * a + c0 * (256.0 - a)) / 256.0) / 255.0;)"
 R"(    } else if (uMode == 20) {               // VHS encode: rows (offset...) -> float (Y, I, Q, A)
        vec4 p = smp(vec2(float(x) + 0.5 - rowPlan(y).x, float(y) + 0.5), 0);
        fragColor = vec4(toYiq(p.rgb), p.a);
    } else if (uMode == 21) {               // horizontal box blur of a float texture: radius per channel in uI.xyz
        vec4 v = texelFetch(uTex, ip, 0);
        for (int ch = 0; ch < 3; ++ch) {
            int r = uI[ch];
            if (r <= 0) continue;
            int a = max(0, x - r), b = min(size.x - 1, x + r);
            float s = 0.0;
            for (int k = a; k <= b; ++k) s += texelFetch(uTex, ivec2(k, y), 0)[ch];
            v[ch] = s / float(b - a + 1);
        }
        fragColor = v;
    } else if (uMode == 22) {               // VHS compose: uTex filtered (Y,I,Q,A); rows; uA chroma shift, sat, noise, tracking
        vec4 r = rowPlan(y);
        vec4 c = texelFetch(uTex, ip, 0);
        float yy = c.r;
        float xs = clamp(float(x) - uA.x, 0.0, float(size.x - 1));
        int i0 = min(int(xs), max(0, size.x - 2));
        float f = xs - float(i0);
        vec2 iq0 = texelFetch(uTex, ivec2(i0, y), 0).gb, iq1 = texelFetch(uTex, ivec2(min(i0 + 1, size.x - 1), y), 0).gb;
        vec2 iq = (size.x == 1 ? iq0 : iq0 + (iq1 - iq0) * f) * uA.y;
        if (uA.z > 0.0) yy += (hash01(uint(x), uint(y), uint(uSeed) * 3u + 1u) - 0.5) * uA.z * 90.0;
        if (r.y > 0.0) yy += (hash01(uint(x / 2), uint(y), useed(9)) - 0.5) * uA.w * 120.0;
        int ds = int(r.z), dl = int(r.w);
        if (ds >= 0 && x >= ds && x < ds + dl) yy += 140.0 * (1.0 - abs(float(x - ds) / float(max(1, dl)) * 2.0 - 1.0));
        fragColor = packf(c.a, fromYiq(vec3(yy, iq)));
    } else if (uMode == 23) {               // NTSC encode: uA.x period; uSeed frame -> float (signal, -, -, alpha)
        float ph = 2.0 * PI * float(x) / uA.x + (((y + uSeed) & 1) == 1 ? PI : 0.0);
        vec3 q = toYiq(c0.rgb);
        float sig = q.x + q.y * cos(ph) + q.z * sin(ph);
        fragColor = vec4(sig, sig, 0.0, c0.a);
    } else if (uMode == 24) {               // NTSC demod: uTex (signal, Y); uI.x bleed radius -> (Y, I, Q, A)
        vec4 s0 = texelFetch(uTex, ip, 0);
        int a = max(0, x - uI.x), b = min(size.x - 1, x + uI.x);
        vec2 iq = vec2(0.0);
        float lineShift = (((y + uSeed) & 1) == 1) ? PI : 0.0;
        for (int k = a; k <= b; ++k) {
            vec2 sy = texelFetch(uTex, ivec2(k, y), 0).rg;
            float ph = 2.0 * PI * float(k) / uA.x + lineShift;
            float cc = sy.x - sy.y;
            iq += 2.0 * cc * vec2(cos(ph), sin(ph));
        }
        fragColor = vec4(s0.g, iq / float(b - a + 1), s0.a);
    } else if (uMode == 25) {               // NTSC compose: uTex (Y,I,Q,A), uTex1 original; uA.y ringing, uA.z strength; uI.x ringing radius
        vec4 c = texelFetch(uTex, ip, 0);
        float yy = c.r;
        if (uA.y > 0.0) {
            int a = max(0, x - uI.x), b = min(size.x - 1, x + uI.x);
            float s = 0.0;
            for (int k = a; k <= b; ++k) s += texelFetch(uTex, ivec2(k, y), 0).r;
            yy += (yy - s / float(b - a + 1)) * uA.y * 1.5;
        }
        vec3 rgb = fromYiq(vec3(yy, c.gb));
        vec4 o = fetchB(uTex1, ip, size, 0);
        fragColor = packf(o.a, o.rgb + (rgb - o.rgb) * uA.z);
    } else if (uMode == 26) {               // dither: cell averages (small target); uI.x cell; uSize = full size
        vec4 s = vec4(0.0);
        int n = 0;
        for (int j = y * uI.x; j < min(size.y, (y + 1) * uI.x); ++j)
            for (int i = x * uI.x; i < min(size.x, (x + 1) * uI.x); ++i) { s += at(ivec2(i, j)); ++n; }
        float un = s.a > 0.0 ? 255.0 / s.a : 0.0;
        fragColor = vec4(s.rgb * un, floor(s.a / float(max(1, n)) + 0.5));
    } else if (uMode == 27) {               // dither: palette pick (small target); uI.x kind (0 none, 1 map, 2 white noise, 3 gradient
                                            // noise, 4 pattern), uI.w map size (map in uData); uJ.xy noise offset, uJ.w plan; uA.x spread, uA.y carry
        vec4 c = texelFetch(uTex, ip, 0);
        int kind = uI.x;
        float t = 0.5;
        if (kind == 1 || kind == 4) t = texelFetch(uData, ivec2((x + uJ.x) % uI.w, (y + uJ.y) % uI.w), 0).r;
        else if (kind == 2) t = float(ditherHash(uint(x + uJ.x), uint(y + uJ.y), 0u) >> 8u) / 16777216.0;
        else if (kind == 3) {
            float f = 0.06711056 * float(x + uJ.x) + 0.00583715 * float(y + uJ.y);
            float g = 52.9829189 * (f - floor(f));
            t = g - floor(g);
        }
        int pick;
        if (kind == 4) {                    // Knoll's pattern: a plan of colours averaging to this one, lightest last
            int picks[64];
            vec3 e = vec3(0.0);
            for (int i = 0; i < uJ.w; ++i) {
                int k = ditherNearest(clamp(c.rgb + e * uA.y, 0.0, 255.0));
                picks[i] = k;
                e += c.rgb - ditherColor(k);
            }
            for (int i = 1; i < uJ.w; ++i) {  // stable insertion sort by luma
                int k = picks[i];
                vec3 kc = ditherColor(k);
                int kl = 299 * int(kc.r) + 587 * int(kc.g) + 114 * int(kc.b);
                int j = i - 1;
                while (j >= 0) {
                    vec3 jc = ditherColor(picks[j]);
                    if (299 * int(jc.r) + 587 * int(jc.g) + 114 * int(jc.b) <= kl) break;
                    picks[j + 1] = picks[j];
                    --j;
                }
                picks[j + 1] = k;
            }
            pick = picks[min(uJ.w - 1, int(t * float(uJ.w)))];
        } else {
            pick = ditherNearest(c.rgb + (t - 0.5) * uA.x);
        }
        fragColor = vec4(ditherColor(pick), c.a);
    } else if (uMode == 28) {               // dither: expand (full res); uTex1 small picks; uI.x cell
        vec4 c = texelFetch(uTex1, ivec2(x / uI.x, y / uI.x), 0);
        float a = c.a;
        fragColor = packf(a, floor(c.rgb * a / 255.0));
    } else if (uMode == 29) {               // ascii: cell averages (small); uI.x cell; uSize full
        vec4 s = vec4(0.0);
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i)
                s += at(ivec2(min(size.x - 1, x * uI.x + (i * 2 + 1) * uI.x / 6), min(size.y - 1, y * uI.x + (j * 2 + 1) * uI.x / 6)));
        fragColor = s / 9.0;
    } else if (uMode == 30) {               // ascii compose: uTex1 averages, uTex2 glyph atlas; uI cell, glyphs, colour mode; uA.x background
        int cell = uI.x;
        vec4 s = texelFetch(uTex1, ivec2(x / cell, y / cell), 0);
        float l = (0.299 * s.r + 0.587 * s.g + 0.114 * s.b) / 255.0;
        int g = clamp(int(l * float(uI.y)), 0, uI.y - 1);
        float k = floor(texelFetch(uTex2, ivec2(g * cell + x % cell, y % cell), 0).r * 255.0 + 0.5) / 255.0;
        vec3 f;
        if (uI.z == 1) f = vec3(40.0, 255.0, 90.0);
        else if (uI.z == 2) f = vec3(255.0, 176.0, 40.0);
        else if (uI.z == 3) f = vec3(255.0);
        else { float mx = max(max(max(s.r, s.g), s.b), 1.0); f = s.rgb * 255.0 / mx; }
        fragColor = packf(255.0, c0.rgb * uA.x * (1.0 - k) + f * k);)"
 R"(    } else if (uMode == 31) {               // neon edges: uA.x strength, uA.y hue time; uI.x colour mode; uB.rgb colour
        float L[9];
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) L[(j + 1) * 3 + i + 1] = lumaB(at(ip + ivec2(i, j)));
        float gx = L[2] + 2.0 * L[5] + L[8] - L[0] - 2.0 * L[3] - L[6];
        float gy = L[6] + 2.0 * L[7] + L[8] - L[0] - 2.0 * L[1] - L[2];
        float e = min(1.0, sqrt(gx * gx + gy * gy) * uA.x);
        vec4 c;
        if (uI.x == 1) c = hsvColor(atan(gy, gx) / (2.0 * PI) + uA.y, 1.0, 1.0);
        else if (uI.x == 2) c = vec4(uB.rgb, 255.0);
        else {
            int mx = int(max(max(max(c0.r, c0.g), c0.b), 1.0));
            c = vec4(float(int(c0.r) * 255 / mx), float(int(c0.g) * 255 / mx), float(int(c0.b) * 255 / mx), 255.0);
        }
        fragColor = packf(255.0 * e, c.rgb * e);
    } else if (uMode == 32) {               // neon combine: uTex1 edges, uTex2 glow; uA.x background, uI.x has glow
        vec4 e = fetchB(uTex1, ip, size, 0);
        vec4 g = uI.x == 1 ? fetchB(uTex2, ip, size, 0) : vec4(0.0);
        float k = uI.x == 1 ? 1.5 : 0.0;
        fragColor = packf(max(c0.a, e.a), c0.rgb * uA.x + e.rgb + g.rgb * k);
    } else if (uMode == 33) {               // jpeg: straight-colour YCbCr (float)
        float un = c0.a > 0.0 ? 255.0 / c0.a : 0.0;
        vec3 c = c0.rgb * un;
        fragColor = vec4(0.299 * c.r + 0.587 * c.g + 0.114 * c.b, 128.0 - 0.168736 * c.r - 0.331264 * c.g + 0.5 * c.b,
                         128.0 + 0.5 * c.r - 0.418688 * c.g - 0.081312 * c.b, c0.a);
    } else if (uMode == 34) {               // jpeg: half-resolution chroma; uTex full YCbCr (float), uSize full size
        vec2 s = vec2(0.0);
        for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) s += texelFetch(uTex, ivec2(min(size.x - 1, x * 2 + i), min(size.y - 1, y * 2 + j)), 0).gb;
        fragColor = vec4(0.0, s * 0.25, 0.0);
    } else if (uMode == 35) {               // jpeg forward DCT + quantise + corruption; uTex plane (float), uSize plane size;
                                            // uI.x n, uI.y channel mask (1 r, 2 g, 4 b), uI.z first channel uses table 0, uI.w corrupt what;
                                            // uA.x qscale (luma), uA.y corruption, uA.z qscale (colour)
        int n = uI.x;
        int bx = x / n, by = y / n, k = y % n, u = x % n;
        vec3 s = vec3(0.0);
        for (int yy = 0; yy < n; ++yy)
            for (int xx = 0; xx < n; ++xx) {
                vec3 p = texelFetch(uTex, ivec2(min(size.x - 1, bx * n + xx), min(size.y - 1, by * n + yy)), 0).rgb - 128.0;
                s += dctCos(k, yy, n) * dctCos(u, xx, n) * p;
            }
        int ti = (k * 8 / n) * 8 + u * 8 / n;
        vec4 o = vec4(0.0);
        for (int ch = 0; ch < 3; ++ch) {
            if ((uI.y & (1 << ch)) == 0) continue;
            bool lumaTable = ch == 0 && uI.z == 1;
            float q = max(1.0, floor((float(lumaTable ? uQ0[ti] : uQ1[ti]) * (lumaTable ? uA.x : uA.z) + 50.0) / 100.0));
            float v = roundAway(s[ch] / q) * q;
            uint seed = uint(uSeed) + uint(ch == 1 ? 100 : (ch == 2 ? 200 : 0));
            if (uA.y > 0.0 && hash01(uint(bx), uint(by), seed) < uA.y) {
                int count = 1 + int(hash32(uint(bx), uint(by), seed + 1u) % 4u);
                for (int cc = 0; cc < count; ++cc) {
                    uint hh = hash32(uint(bx), uint(by), seed + 2u + uint(cc));
                    int target = uI.w == 1 ? 0 : (uI.w == 2 ? 1 + int(hh % uint(n * n - 1)) : int(hh % uint(n * n)));
                    if (target == k * n + u) v += (float(int(hh >> 8u) % 512) - 256.0) * 2.0;
                }
            }
            o[ch] = v;
        }
        fragColor = o;
    } else if (uMode == 36) {               // jpeg inverse DCT; uTex coefficients, uI.x n, uSize plane size
        int n = uI.x;
        int bx = x / n, by = y / n, yy = y % n, xx = x % n;
        vec3 s = vec3(0.0);
        for (int k = 0; k < n; ++k)
            for (int u = 0; u < n; ++u)
                s += dctCos(k, yy, n) * dctCos(u, xx, n) * texelFetch(uTex, ivec2(bx * n + u, by * n + k), 0).rgb;
        fragColor = vec4(s + 128.0, 0.0);
    } else if (uMode == 37) {               // jpeg compose: uTex1 full plane (Y + maybe CbCr), uTex2 half chroma; uI.x subsample
        float Y = texelFetch(uTex1, ip, 0).r;
        vec2 cc = uI.x == 1 ? texelFetch(uTex2, ivec2(x / 2, y / 2), 0).gb : texelFetch(uTex1, ip, 0).gb;
        float cb = cc.x - 128.0, cr = cc.y - 128.0;
        float k = c0.a / 255.0;
        fragColor = packf(c0.a, vec3(Y + 1.402 * cr, Y - 0.344136 * cb - 0.714136 * cr, Y + 1.772 * cb) * k);
)"
 R"(    } else if (uMode == 38) {               // pixel sort, init. Work texture: x = position along the line, y = line.
                                            // uI.x vertical, uI.y line length, uI.z sort by, uI.w threshold on; uA.xy thresholds;
                                            // uJ.x invert, uJ.y reverse; uB.x random breaks (seed: uSeed)
                                            // out: r = last break (own index, -1 inside a run), g = key + 256 if in range
        int i = x, line = y;
        if (i >= uI.y) { fragColor = vec4(float(i), 0.0, 0.0, 0.0); return; }
        vec4 p = at(uI.x == 1 ? ivec2(line, i) : ivec2(i, line));
        float L = lumaB(p);
        float tv = L;
        if (uI.w == 1) {
            float mx = max(max(p.r, p.g), p.b), mn = min(min(p.r, p.g), p.b), d = mx - mn;
            tv = 0.0;
            if (d > 0.0) {
                tv = mx == p.r ? (p.g - p.b) / d : (mx == p.g ? 2.0 + (p.b - p.r) / d : 4.0 + (p.r - p.g) / d);
                tv /= 6.0;
                if (tv < 0.0) tv += 1.0;
            }
        } else if (uI.w == 2) {
            ivec3 q = ivec3(p.rgb);
            int mx = max(max(q.r, q.g), q.b), mn = min(min(q.r, q.g), q.b);
            tv = float(mx > 0 ? (mx - mn) * 255 / mx : 0) / 255.0;
        }
        bool inR = ((tv >= uA.x && tv <= uA.y) != (uJ.x == 1)) && p.a > 0.0;
        bool brk = uB.x > 0.0 && hash01(uint(i), uint(line), useed(0)) < uB.x;
        int key;
        if (uI.z == 1) {
            float mx = max(max(p.r, p.g), p.b), mn = min(min(p.r, p.g), p.b), d = mx - mn;
            float hh = 0.0;
            if (d > 0.0) {
                hh = mx == p.r ? (p.g - p.b) / d : (mx == p.g ? 2.0 + (p.b - p.r) / d : 4.0 + (p.r - p.g) / d);
                hh /= 6.0;
                if (hh < 0.0) hh += 1.0;
            }
            key = int(hh * 255.0);
        } else if (uI.z == 2) {
            ivec3 q = ivec3(p.rgb);
            int mx = max(max(q.r, q.g), q.b), mn = min(min(q.r, q.g), q.b);
            key = mx > 0 ? (mx - mn) * 255 / mx : 0;
        } else if (uI.z == 3) key = int(p.r);
        else if (uI.z == 4) key = int(p.g);
        else if (uI.z == 5) key = int(p.b);
        else key = int(L * 255.0);
        if (uJ.y == 1) key = 255 - key; // descending, ties keep their order (stable)
        // A random break inside a run starts a new run here (the last break is the pixel before).
        fragColor = vec4(inR ? (brk ? float(i - 1) : -1.0) : float(i), float(key + (inR ? 256 : 0)), 0.0, 0.0);
    } else if (uMode == 39) {               // pixel sort: max-scan of the last break; uI.x step
        vec4 v = texelFetch(uTex, ip, 0);
        if (x - uI.x >= 0) v.r = max(v.r, texelFetch(uTex, ivec2(x - uI.x, y), 0).r);
        fragColor = v;
    } else if (uMode == 40) {               // pixel sort: sort keys = segment start * 256 + key, original index; uI.x max run, uI.y length
        vec4 v = texelFetch(uTex, ip, 0);
        int key = int(v.g) & 255;
        bool inR = int(v.g) >= 256;
        float comp;
        if (x >= uI.y) comp = 8388608.0;                       // padding sorts last
        else if (inR) {
            int lastBreak = int(v.r);
            int pos = x - lastBreak - 1;
            int segStart = lastBreak + 1 + (uI.x > 0 ? (pos / uI.x) * uI.x : 0);
            comp = float(segStart * 256 + key);
        } else comp = float(x * 256 + key);                     // a pixel outside every run stays put
        fragColor = vec4(comp, float(x), 0.0, 0.0);
    } else if (uMode == 41) {               // pixel sort: one bitonic compare-exchange step; uI.x = j, uI.y = k
        vec2 a = texelFetch(uTex, ip, 0).rg;
        int partner = x ^ uI.x;
        vec2 b = texelFetch(uTex, ivec2(partner, y), 0).rg;
        bool aLess = a.x < b.x || (a.x == b.x && a.y < b.y);
        bool ascending = (x & uI.y) == 0;
        bool keepA = ((x < partner) == ascending) ? aLess : !aLess;
        fragColor = vec4(keepA ? a : b, 0.0, 0.0);
    } else if (uMode == 42) {               // pixel sort: output; uTex1 sorted work texture; uI.x vertical
        int along = uI.x == 1 ? y : x, line = uI.x == 1 ? x : y;
        int idx = int(texelFetch(uTex1, ivec2(along, line), 0).g + 0.5);
        fragColor = packv(at(uI.x == 1 ? ivec2(line, idx) : ivec2(idx, line)));
)"
 R"(    } else if (uMode == 43) {               // databend: colour bytes as floats (r, g, b)
        fragColor = vec4(c0.rgb, 0.0);
    } else if (uMode == 44) {               // databend echo: one doubling step; uI.x shift (bytes), uA.x feedback^(2^step)
        vec4 v = texelFetch(uTex, ip, 0);
        for (int ch = 0; ch < 3; ++ch) {
            int j = (y * size.x + x) * 3 + ch - uI.x;
            if (j < 0) continue;
            int q = j / 3;
            v[ch] += uA.x * texelFetch(uTex, ivec2(q % size.x, q / size.x), 0)[j - q * 3];
        }
        fragColor = v;
    } else if (uMode == 48) {               // glitch Mix / Apply to (fx::postWeight): uTex effect output, uTex1 input;
                                            // uA mix, threshold, softness (box: pixels), mode; uB box x0, y0, x1, y1 (pixels)
        vec4 before = fetchB(uTex1, ip, size, 0);
        vec2 pc = vec2(ip) + 0.5;
        int mode = int(uA.w);
        float k = 1.0;
        if (mode == 1 || mode == 2) {
            float e = min(min(pc.x - uB.x, uB.z - pc.x), min(pc.y - uB.y, uB.w - pc.y));
            k = uA.z > 0.0 ? clamp(e / uA.z + 0.5, 0.0, 1.0) : (e >= 0.0 ? 1.0 : 0.0);
            if (mode == 2) k = 1.0 - k;
        } else if (mode >= 3) {
            float L = lumaB(before);
            k = uA.z > 0.0 ? clamp((L - uA.y) / uA.z + 0.5, 0.0, 1.0) : (L >= uA.y ? 1.0 : 0.0);
            if (mode == 4) k = 1.0 - k;
        }
        float t = k * uA.x;
        if (t >= 1.0) fragColor = c0 / 255.0;
        else if (t <= 0.0) fragColor = before / 255.0;
        else { float wt = floor(t * 256.0); fragColor = floor((before * (256.0 - wt) + c0 * wt) / 256.0) / 255.0; }
    } else if (uMode == 49) {               // effect mask (maskWeight): uTex effect output, uTex1 input;
                                            // uA centre, half size (pixels); uB cos, sin, feather (pixels), shape; uC.x invert
        vec4 before = fetchB(uTex1, ip, size, 0);
        vec2 d = vec2(ip) + 0.5 - uA.xy;
        float qx = d.x * uB.x + d.y * uB.y, qy = -d.x * uB.y + d.y * uB.x;
        float e, k;
        if (int(uB.w) == 3) {               // freehand: uTex2 holds the coverage (alpha)
            k = texelFetch(uTex2, ip, 0).a;
        } else {
            if (int(uB.w) == 1) {
                e = min(uA.z - abs(qx), uA.w - abs(qy));
            } else {
                float ax = qx / max(uA.z, 1e-3), ay = qy / max(uA.w, 1e-3);
                e = (1.0 - sqrt(ax * ax + ay * ay)) * min(uA.z, uA.w);
            }
            k = uB.z > 0.0 ? clamp(e / uB.z + 0.5, 0.0, 1.0) : (e >= 0.0 ? 1.0 : 0.0);
        }
        if (uC.x > 0.5) k = 1.0 - k;
        if (k >= 1.0) fragColor = c0 / 255.0;
        else if (k <= 0.0) fragColor = before / 255.0;
        else { float wt = floor(k * 256.0); fragColor = floor((before * (256.0 - wt) + c0 * wt) / 256.0) / 255.0; }
    } else if (uMode == 50) {               // remove background (Segmentation.cpp): uTex1 the person mask (small: smoothed up),
                                            // uTex2 the blurred layer; uA threshold, softness / 2 (< 0: a matte, as it is), mode,
                                            // invert; uB.rgb colour
        float m0 = texture(uTex1, vUv).r;
        float a = uA.y < 0.0 ? m0 : ss(uA.x - uA.y, uA.x + uA.y, m0);
        if (uA.w > 0.5) a = 1.0 - a;
        int m = int(uA.z + 0.5);
        if (m == 3) {
            fragColor = vec4(vec3(a), 1.0);
        } else {
            vec4 under = m == 0 ? vec4(0.0) : (m == 1 ? texelFetch(uTex2, ip, 0) : vec4(uB.rgb, 1.0));
            fragColor = mix(under, c0 / 255.0, a);
        }
    } else if (uMode == 45) {               // databend echo: output; uTex1 accumulated; uI.w mix (0..256)
        vec3 acc = texelFetch(uTex1, ip, 0).rgb;
        vec3 o = floor(min(vec3(255.0), acc));
        fragColor = packf(c0.a, floor((c0.rgb * float(256 - uI.w) + o * float(uI.w)) / 256.0));
    } else if (uMode == 46) {               // dither: per-cell totals (small target); uI.x cell; uSize full size
        vec4 s = vec4(0.0);
        for (int j = y * uI.x; j < min(size.y, (y + 1) * uI.x); ++j)
            for (int i = x * uI.x; i < min(size.x, (x + 1) * uI.x); ++i) s += at(ivec2(i, j));
        fragColor = s;
    } else if (uMode == 47) {               // dither: palette colour + alpha from the diffusion result; uTex1 totals; uI.x cell, uI.z levels; uSize full
        int idx = int(texelFetch(uTex, ip, 0).a + 0.5);
        float sumA = texelFetch(uTex1, ip, 0).a;
        int n = (min(size.x, (x + 1) * uI.x) - x * uI.x) * (min(size.y, (y + 1) * uI.x) - y * uI.x);
        fragColor = vec4(ditherColor(idx), floor(sumA / float(max(1, n)) + 0.5));
    } else {
        fragColor = texelFetch(uTex, ip, 0);
    }
}
)";

// Error diffusion, one diagonal wave (x + uSlope * y = uT) at a time: every
// pixel of a wave depends only on earlier waves (each kernel tap is `lag`
// waves back, at most 8). Waves rotate through up to nine textures (wave t - k
// is bound as uPrevk) and also go to a second output that collects the whole
// picture. Drawn as one point per row.
inline const char* kVertDiffuse = R"(#version 330 core
uniform int uT;
uniform int uSlope;
uniform ivec2 uDims;
flat out ivec2 vP;
void main() {
    int y = gl_VertexID, x = uT - uSlope * y;
    vP = ivec2(x, y);
    bool ok = x >= 0 && x < uDims.x && y < uDims.y;
    vec2 pos = (vec2(x, y) + 0.5) / vec2(uDims) * 2.0 - 1.0;
    gl_Position = ok ? vec4(pos, 0.0, 1.0) : vec4(2.0, 2.0, 0.0, 1.0);
}
)";

inline const char* kFragDiffuse = R"(#version 330 core
flat in ivec2 vP;
layout(location = 0) out vec4 fragColor;  // this wave's slot
layout(location = 1) out vec4 fragAll;    // the whole picture
uniform sampler2D uSums;   // per-cell totals (r, g, b, a)
uniform sampler2D uPrev1;  // waves t-1 .. t-8: error (1/16 units, strength applied) and palette index
uniform sampler2D uPrev2;
uniform sampler2D uPrev3;
uniform sampler2D uPrev4;
uniform sampler2D uPrev5;
uniform sampler2D uPrev6;
uniform sampler2D uPrev7;
uniform sampler2D uPrev8;
uniform ivec4 uTaps[16];   // dx, dy, weight, lag (waves back)
uniform int uTapCount;
uniform int uDivisor;
uniform int uStrength;     // x / 256
uniform ivec2 uDims;
uniform int uCount;        // palette colours (uLevels == 0)
uniform int uLevels;       // > 0: levels per channel
uniform ivec3 uPal[64];    // palette in 1/16 units
int floorDiv(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }
ivec3 floorDiv3(ivec3 a, int b) { return ivec3(floorDiv(a.x, b), floorDiv(a.y, b), floorDiv(a.z, b)); }
bool inside(ivec2 p) { return p.x >= 0 && p.y >= 0 && p.x < uDims.x && p.y < uDims.y; }
ivec3 errAt(int lag, ivec2 p) {
    vec4 e = lag == 1 ? texelFetch(uPrev1, p, 0) : lag == 2 ? texelFetch(uPrev2, p, 0) : lag == 3 ? texelFetch(uPrev3, p, 0)
           : lag == 4 ? texelFetch(uPrev4, p, 0) : lag == 5 ? texelFetch(uPrev5, p, 0) : lag == 6 ? texelFetch(uPrev6, p, 0)
           : lag == 7 ? texelFetch(uPrev7, p, 0) : texelFetch(uPrev8, p, 0);
    return ivec3(e.rgb);
}
ivec3 levelColor16(int i) {
    int l = uLevels;
    ivec3 q = ivec3(i / (l * l), (i / l) % l, i % l);
    return (q * 255 * 2 + (l - 1)) / (2 * (l - 1)) * 16;
}
void main() {
    ivec4 s = ivec4(texelFetch(uSums, vP, 0) + 0.5);
    ivec3 v = s.a > 0 ? (s.rgb * 4080 * 2 + s.a) / (2 * s.a) : ivec3(0);
    for (int i = 0; i < uTapCount; ++i) {
        ivec2 from = vP - uTaps[i].xy;
        if (inside(from)) v += floorDiv3(errAt(uTaps[i].w, from) * uTaps[i].z, uDivisor);
    }
    int best = 0;
    ivec3 chosen;
    if (uLevels > 0) {
        int l = uLevels;
        ivec3 q = (clamp(v, ivec3(0), ivec3(4080)) * (l - 1) * 2 + 4080) / 8160;
        best = (q.r * l + q.g) * l + q.b;
        chosen = levelColor16(best);
    } else {
        int bestD = 0x7fffffff;
        for (int i = 0; i < uCount; ++i) {
            ivec3 d = v - uPal[i];
            int dd = d.r * d.r * 3 + d.g * d.g * 6 + d.b * d.b;
            if (dd < bestD) { bestD = dd; best = i; }
        }
        chosen = uPal[best];
    }
    fragColor = vec4(vec3(floorDiv3((v - chosen) * uStrength, 256)), float(best));
    fragAll = fragColor;
}
)";

} // namespace gpufx
