#include "core/TransitionKernels.h"

#include "core/EffectHelpers.h"
#include "core/ImageOps.h"

#include <algorithm>
#include <cmath>

namespace tfx {
namespace {

constexpr float kPi = 3.14159265358979f;

// ---- Small GLSL-like vector maths, so the C++ kernel reads like the shader ----

struct V2 {
    float x = 0, y = 0;
};
inline V2 operator+(V2 a, V2 b) { return {a.x + b.x, a.y + b.y}; }
inline V2 operator-(V2 a, V2 b) { return {a.x - b.x, a.y - b.y}; }
inline V2 operator*(V2 a, float s) { return {a.x * s, a.y * s}; }
inline V2 operator/(V2 a, float s) { return {a.x / s, a.y / s}; }
inline float dot(V2 a, V2 b) { return a.x * b.x + a.y * b.y; }
inline float length(V2 a) { return std::sqrt(dot(a, a)); }

struct V3 {
    float x = 0, y = 0, z = 0;
};
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct V4 {
    float r = 0, g = 0, b = 0, a = 0;
};
inline V4 operator+(V4 x, V4 y) { return {x.r + y.r, x.g + y.g, x.b + y.b, x.a + y.a}; }
inline V4 operator*(V4 x, float s) { return {x.r * s, x.g * s, x.b * s, x.a * s}; }
inline V4 mix(V4 x, V4 y, float t) { return x * (1.0f - t) + y * t; }
inline V4 shadeRgb(V4 c, float s) { return {c.r * s, c.g * s, c.b * s, c.a}; }

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float sgn(float v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }
inline float glslMod(float x, float y) { return x - y * std::floor(x / y); }
inline float ss(float e0, float e1, float x) { return imageops::smoothstep(e0, e1, x); }
inline float ease(float t)
{
    t = clampf(t, 0, 1);
    return t * t * (3.0f - 2.0f * t);
}
using imageops::wipeMask;

using fx::hash01;

float valueNoise(V2 q)
{
    const float ix = std::floor(q.x), iy = std::floor(q.y);
    const float fx = q.x - ix, fy = q.y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    const uint32_t x = uint32_t(ix), y = uint32_t(iy);
    const float a = hash01(x, y, 9u), b = hash01(x + 1u, y, 9u), c = hash01(x, y + 1u, 9u), d = hash01(x + 1u, y + 1u, 9u);
    const float top = a + (b - a) * ux, bottom = c + (d - c) * ux;
    return top + (bottom - top) * uy;
}

float fbm(V2 q)
{
    float s = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < 4; ++i) {
        s += amp * valueNoise(q);
        norm += amp;
        q = q * 2.0f + V2{17.3f, 9.1f};
        amp *= 0.5f;
    }
    return s / norm;
}

// Signed distances of unit shapes (y up), after Inigo Quilez.
float sdStar5(V2 p, float r, float rf)
{
    const V2 k1{0.809016994375f, -0.587785252292f};
    const V2 k2{-k1.x, k1.y};
    p.x = std::abs(p.x);
    p = p - k1 * (2.0f * std::max(dot(k1, p), 0.0f));
    p = p - k2 * (2.0f * std::max(dot(k2, p), 0.0f));
    p.x = std::abs(p.x);
    p.y -= r;
    const V2 ba = V2{-k1.y, k1.x} * rf - V2{0, 1};
    const float h = clampf(dot(p, ba) / dot(ba, ba), 0.0f, r);
    return length(p - ba * h) * sgn(p.y * ba.x - p.x * ba.y);
}

float sdHeart(V2 p)
{
    p.x = std::abs(p.x);
    if (p.y + p.x > 1.0f) {
        const V2 d = p - V2{0.25f, 0.75f};
        return std::sqrt(dot(d, d)) - std::sqrt(2.0f) / 4.0f;
    }
    const V2 d1 = p - V2{0, 1};
    const float m = 0.5f * std::max(p.x + p.y, 0.0f);
    const V2 d2 = p - V2{m, m};
    return std::sqrt(std::min(dot(d1, d1), dot(d2, d2))) * sgn(p.x - p.y);
}

float sdHexagon(V2 p, float r)
{
    const float kx = -0.866025404f, ky = 0.5f, kz = 0.577350269f;
    p = {std::abs(p.x), std::abs(p.y)};
    const float t = 2.0f * std::min(kx * p.x + ky * p.y, 0.0f);
    p = p - V2{kx * t, ky * t};
    p = p - V2{clampf(p.x, -kz * r, kz * r), r};
    return length(p) * sgn(p.y);
}

float sdTriangle(V2 p, float r)
{
    const float k = std::sqrt(3.0f);
    p.x = std::abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0f)
        p = V2{p.x - k * p.y, -k * p.x - p.y} / 2.0f;
    p.x -= clampf(p.x, -2.0f * r, 0.0f);
    return -length(p) * sgn(p.y);
}

float sdCross(V2 p, V2 b)
{
    p = {std::abs(p.x), std::abs(p.y)};
    if (p.y > p.x)
        p = {p.y, p.x};
    const V2 q = p - b;
    const float k = std::max(q.y, q.x);
    const V2 w = k > 0.0f ? q : V2{b.y - p.x, -k};
    return sgn(k) * length(V2{std::max(w.x, 0.0f), std::max(w.y, 0.0f)});
}

float sdShape(int shape, V2 p)
{
    switch (shape) {
    case 0: return sdStar5(p, 1.0f, 0.5f);
    case 1: return sdHeart(p + V2{0, 0.5f});
    case 2: return sdHexagon(p, 1.0f);
    case 3: return sdTriangle(p, 1.0f);
    default: return sdCross(p, V2{1.0f, 0.33f});
    }
}
// Radius of the largest circle inside each unit shape (a little under), for growing it past the frame.
constexpr float kShapeInRadius[] = {0.45f, 0.33f, 0.95f, 0.55f, 0.31f};

// Bilinear sample at pixel position p (pixel centres at +0.5), transparent outside.
struct Sampler {
    const QImage* img = nullptr;
    int w = 0, h = 0;

    V4 operator()(V2 p) const
    {
        if (!img || p.x < 0 || p.y < 0 || p.x > float(w) || p.y > float(h))
            return {};
        const float fx = p.x - 0.5f, fy = p.y - 0.5f;
        const float flx = std::floor(fx), fly = std::floor(fy);
        const float tx = fx - flx, ty = fy - fly;
        const int x0 = std::clamp(int(flx), 0, w - 1), x1 = std::clamp(int(flx) + 1, 0, w - 1);
        const int y0 = std::clamp(int(fly), 0, h - 1), y1 = std::clamp(int(fly) + 1, 0, h - 1);
        const auto* r0 = reinterpret_cast<const uint32_t*>(img->constScanLine(y0));
        const auto* r1 = reinterpret_cast<const uint32_t*>(img->constScanLine(y1));
        auto unpack = [](uint32_t c) {
            return V4{float((c >> 16) & 0xff), float((c >> 8) & 0xff), float(c & 0xff), float(c >> 24)} * (1.0f / 255.0f);
        };
        const V4 top = mix(unpack(r0[x0]), unpack(r0[x1]), tx);
        const V4 bottom = mix(unpack(r1[x0]), unpack(r1[x1]), tx);
        return mix(top, bottom, ty);
    }
};

// Distortions fold positions past the edge back into the frame (mirrored)
// rather than showing nothing there.
inline float mirror1(float x, float L)
{
    const float m = glslMod(x, 2.0f * L);
    return m <= L ? m : 2.0f * L - m;
}
inline V2 mirrorPos(V2 q, float w, float h) { return {mirror1(q.x, w), mirror1(q.y, h)}; }

// One face of the card / cube: where the ray to screen point l meets it.
struct FaceHit {
    bool hit = false;
    float lambda = 0;
    float s = 0, t = 0;
    bool front = false;
};

FaceHit hitFace(V2 l, float D, V3 P, V3 U, V3 N, float halfW, float halfH, bool twoSided)
{
    FaceHit f;
    const V3 dir{l.x, l.y, D}, cam{0, 0, -D};
    const float dn = dot(dir, N);
    if (std::abs(dn) < 1e-6f || (!twoSided && dn >= 0))
        return f;
    f.lambda = dot(P - cam, N) / dn;
    const V3 q = cam + dir * f.lambda;
    f.s = dot(q - P, U);
    f.t = q.y - P.y;
    f.front = dn < 0;
    f.hit = f.lambda > 0 && std::abs(f.s) <= halfW && std::abs(f.t) <= halfH;
    return f;
}

V4 shade(const Uniforms& u, V2 px, const Sampler& A, const Sampler& B)
{
    const float p = u.p;
    switch (u.mode) {
    case 0: { // Glitch: bands torn sideways, RGB split, blocks flicker over
        const float I = u.a[0], bandH = u.h / u.a[1];
        const uint32_t st = uint32_t(u.a[2]);
        const uint32_t by = uint32_t(std::floor(px.y / bandH));
        V2 s = px;
        if (hash01(by, st, 1u) < I * 0.7f)
            s.x += (hash01(by, st, 2u) - 0.5f) * u.w * 0.3f * I;
        const uint32_t bx = uint32_t(std::floor(px.x / (bandH * 2.0f)));
        const Sampler& src = hash01(bx, by, st + 7u) < p ? B : A;
        const float sp = I * u.w * 0.015f;
        const V4 cr = src(V2{s.x + sp, s.y}), cg = src(s), cb = src(V2{s.x - sp, s.y});
        V4 o{cr.r, cg.g, cb.b, std::max(std::max(cr.a, cg.a), cb.a)};
        if (hash01(by, st, 3u) < I * 0.3f)
            o = shadeRgb(o, 0.6f);
        return o;
    }
    case 1: { // Ripple: rings spread out from the centre
        const V2 c{u.a[0], u.a[1]};
        const V2 d = px - c;
        const float r = length(d);
        const float off = u.a[3] * std::sin(2.0f * kPi * (r / u.a[2] - p * 3.0f));
        const V2 s = mirrorPos(r > 0.001f ? c + d * ((r + off) / r) : px, u.w, u.h);
        const float m = wipeMask(r / u.b[0], p, u.b[1]);
        return mix(m < 1 ? A(s) : V4{}, m > 0 ? B(s) : V4{}, m);
    }
    case 2: { // Swirl
        const V2 c{u.a[0], u.a[1]};
        const V2 d = px - c;
        const float r = length(d);
        V2 s = px;
        if (r < u.a[2]) {
            const float f = 1.0f - r / u.a[2], an = u.a[3] * f * f;
            const float cs = std::cos(an), sn = std::sin(an);
            s = mirrorPos(c + V2{d.x * cs - d.y * sn, d.x * sn + d.y * cs}, u.w, u.h);
        }
        const float t = u.b[0];
        return mix(t < 1 ? A(s) : V4{}, t > 0 ? B(s) : V4{}, t);
    }
    case 3:   // Card Flip
    case 4: { // Cube Spin
        const float W = u.a[0], H = u.a[1], D = u.a[2], ang = u.a[3];
        const bool hor = u.b[0] > 0.5f;
        const float sg = u.b[1], push = u.b[2];
        V2 l = hor ? V2{px.x - u.w / 2, px.y - u.h / 2} : V2{px.y - u.h / 2, px.x - u.w / 2};
        l.x *= sg;
        const float cs = std::cos(ang), sn = std::sin(ang);
        FaceHit best;
        bool onB = false;
        float light = 1;
        if (u.mode == 3) {
            const V3 N{-sn, 0, -cs};
            best = hitFace(l, D, V3{0, 0, push}, V3{cs, 0, -sn}, N, W / 2, H / 2, true);
            onB = !best.front;
            if (onB)
                best.s = -best.s;
            light = 0.55f + 0.45f * std::abs(N.z);
        } else {
            const V3 O{0, 0, W / 2 + push};
            const V3 na{-sn, 0, -cs}, nb{cs, 0, -sn};
            const FaceHit fa = hitFace(l, D, O + na * (W / 2), V3{cs, 0, -sn}, na, W / 2, H / 2, false);
            const FaceHit fb = hitFace(l, D, O + nb * (W / 2), V3{sn, 0, cs}, nb, W / 2, H / 2, false);
            if (fa.hit && (!fb.hit || fa.lambda <= fb.lambda)) {
                best = fa;
                light = 0.55f + 0.45f * cs;
            } else if (fb.hit) {
                best = fb;
                onB = true;
                light = 0.55f + 0.45f * sn;
            }
        }
        if (!best.hit)
            return {};
        const V2 img = hor ? V2{best.s * sg + u.w / 2, best.t + u.h / 2} : V2{best.t + u.w / 2, best.s * sg + u.h / 2};
        return shadeRgb(onB ? B(img) : A(img), light);
    }
    case 5: { // Film Burn
        const float soft = u.a[1];
        const float s = ss(0.2f, 0.8f, fbm(px / u.a[0]));
        const float m = wipeMask(s, p, soft);
        const float ch = clampf((p * (1.0f + soft * 3.0f) - s) / (soft * 3.0f), 0, 1);
        const V4 a = m < 1 ? shadeRgb(A(px), 1.0f - 0.85f * ch) : V4{};
        V4 o = mix(a, m > 0 ? B(px) : V4{}, m);
        const float dist = (s - (p * (1.0f + soft) - soft * 0.5f)) / (soft * 1.5f + 0.02f);
        const float g = std::exp(-dist * dist) * clampf(std::min(p, 1.0f - p) * 12.0f, 0, 1), k = g * g;
        o.r += (u.color[0] + (u.color2[0] - u.color[0]) * k) * g * 1.2f;
        o.g += (u.color[1] + (u.color2[1] - u.color[1]) * k) * g * 1.2f;
        o.b += (u.color[2] + (u.color2[2] - u.color[2]) * k) * g * 1.2f;
        o.a = std::max(o.a, g);
        return o;
    }
    case 6: { // Luma Fade: the brightest parts go first
        const V4 a = A(px);
        float l = a.r * 0.299f + a.g * 0.587f + a.b * 0.114f;
        if (u.a[0] > 0.5f)
            l = 1.0f - l;
        const float m = wipeMask(l, p, u.a[1]);
        return mix(a, m > 0 ? B(px) : V4{}, m);
    }
    case 7: { // Shape Reveal
        const float k = u.a[2];
        if (k < 1e-3f)
            return A(px);
        const V2 c{u.a[0], u.a[1]};
        V2 q = px - c;
        q = V2{u.b[1] * q.x + u.b[2] * q.y, -(-u.b[2] * q.x + u.b[1] * q.y)};
        const float d = k * sdShape(int(u.b[0] + 0.5f), q / k);
        const float m = clampf(0.5f - d / u.a[3], 0, 1);
        V4 o = mix(m < 1 ? A(px) : V4{}, m > 0 ? B(px) : V4{}, m);
        if (u.b[3] > 0) {
            const float ring = clampf(std::min(d + 0.5f, u.b[3] - d + 0.5f), 0, 1);
            o = mix(o, V4{u.color[0], u.color[1], u.color[2], 1}, ring);
        }
        return o;
    }
    case 8: { // Mosaic
        const float block = u.a[0], t = u.a[1];
        V2 s{(std::floor(px.x / block) + 0.5f) * block, (std::floor(px.y / block) + 0.5f) * block};
        s = {clampf(s.x, 0.5f, u.w - 0.5f), clampf(s.y, 0.5f, u.h - 0.5f)};
        return mix(t < 1 ? A(s) : V4{}, t > 0 ? B(s) : V4{}, t);
    }
    case 9: { // Zoom Blur
        const V2 c{u.a[0], u.a[1]};
        const float bl = u.a[2], t = u.a[3];
        V4 acc;
        for (int i = 0; i < 12; ++i) {
            const float f = 1.0f - bl * 0.25f * float(i) / 11.0f;
            const V2 q = (px - c) * f;
            acc = acc + mix(t < 1 ? A(c + q / u.b[0]) : V4{}, t > 0 ? B(c + q / u.b[1]) : V4{}, t);
        }
        return acc * (1.0f / 12.0f);
    }
    case 10: { // Tiles
        const float cols = u.a[0], rows = u.a[1];
        const V2 cell{u.w / cols, u.h / rows};
        const V2 g{std::min(std::floor(px.x / cell.x), cols - 1), std::min(std::floor(px.y / cell.y), rows - 1)};
        const V2 uv{(px.x - (g.x + 0.5f) * cell.x) / (cell.x * 0.5f), (px.y - (g.y + 0.5f) * cell.y) / (cell.y * 0.5f)};
        const int order = int(u.a[2] + 0.5f);
        float d;
        if (order == 0)
            d = ((cols > 1 ? g.x / (cols - 1) : 0.0f) + (rows > 1 ? g.y / (rows - 1) : 0.0f)) / 2.0f;
        else if (order == 1)
            d = hash01(uint32_t(g.x), uint32_t(g.y), 5u);
        else if (order == 2)
            d = std::min(length(V2{(g.x + 0.5f) / cols - 0.5f, (g.y + 0.5f) / rows - 0.5f}) / 0.70711f, 1.0f);
        else
            d = cols > 1 ? g.x / (cols - 1) : 0.0f;
        const float e = ease((p - d * 0.6f) / 0.4f);
        const float m = wipeMask(std::max(std::abs(uv.x), std::abs(uv.y)), e, u.a[3]);
        return mix(m < 1 ? A(px) : V4{}, m > 0 ? B(px) : V4{}, m);
    }
    case 11: { // Light Leak
        const float t = u.b[0];
        V4 o = mix(t < 1 ? A(px) : V4{}, t > 0 ? B(px) : V4{}, t);
        const V2 c{u.a[0], u.a[1]};
        const float R = u.a[2], f = u.a[3];
        const V2 d1 = (px - c) / R;
        const V2 d2 = (px - c - V2{R * 0.8f, R * 0.35f}) / (R * 0.7f);
        const float i1 = f * std::exp(-dot(d1, d1)), i2 = f * std::exp(-dot(d2, d2)) * 0.8f;
        const float lr = clampf(u.color[0] * i1 + u.color2[0] * i2, 0, 1);
        const float lg = clampf(u.color[1] * i1 + u.color2[1] * i2, 0, 1);
        const float lb = clampf(u.color[2] * i1 + u.color2[2] * i2, 0, 1);
        const float ex = 1.0f + 0.5f * f;
        o = V4{o.r * ex, o.g * ex, o.b * ex, o.a};
        o.a = std::max(o.a, std::min(1.0f, std::max(i1, i2)));
        return V4{o.r + lr * (1.0f - o.r), o.g + lg * (1.0f - o.g), o.b + lb * (1.0f - o.b), o.a};
    }
    case 12: { // Flash
        const float f = u.a[0];
        const V4 base = u.a[1] > 0.5f ? B(px) : A(px);
        const float a = base.a + (1.0f - base.a) * f;
        const float k = 1.0f + 2.0f * f;
        return V4{base.r * k + u.color[0] * f, base.g * k + u.color[1] * f, base.b * k + u.color[2] * f, a};
    }
    case 13: { // Wave Wipe
        const bool vert = u.a[0] > 0.5f;
        float along = vert ? px.y / u.h : px.x / u.w;
        if (u.a[1] < 0)
            along = 1.0f - along;
        const float perp = vert ? px.x / u.w : px.y / u.h;
        const float amp = u.a[3];
        float s = along + amp * std::sin(2.0f * kPi * (perp * u.a[2] + p * 1.5f));
        s = (s + amp) / (1.0f + 2.0f * amp);
        const float m = wipeMask(s, p, u.b[0]);
        return mix(m < 1 ? A(px) : V4{}, m > 0 ? B(px) : V4{}, m);
    }
    case 14: { // Slices
        const float n = u.a[0];
        const bool vert = u.a[1] > 0.5f;
        const float along = vert ? px.y : px.x, perp = vert ? px.x : px.y;
        const float L = vert ? u.h : u.w, Lp = vert ? u.w : u.h;
        const float k = std::min(std::floor(perp / (Lp / n)), n - 1);
        const float d = n > 1 ? k / (n - 1) * 0.4f : 0.0f;
        const float e = ease((p - d) / 0.6f);
        const float off = (1.0f - e) * L * (glslMod(k, 2.0f) < 0.5f ? -1.0f : 1.0f);
        const float sa = along - off;
        if (sa >= 0 && sa <= L)
            return B(vert ? V2{px.x, sa} : V2{sa, px.y});
        return A(px);
    }
    default: { // Kaleidoscope
        const V2 c{u.a[0], u.a[1]};
        const V2 d = px - c;
        const float r = length(d);
        const float seg = 2.0f * kPi / u.a[2];
        float fa = glslMod(std::atan2(d.y, d.x), seg);
        if (fa > seg * 0.5f)
            fa = seg - fa;
        fa += u.b[1];
        const float k = u.a[3];
        const V2 folded = c + V2{std::cos(fa), std::sin(fa)} * (r * (1.0f - 0.3f * k));
        const V2 s = mirrorPos(px + (folded - px) * k, u.w, u.h);
        const float t = u.b[0];
        return mix(t < 1 ? A(s) : V4{}, t > 0 ? B(s) : V4{}, t);
    }
    }
}

// ---- Definitions -----------------------------------------------------------------------

using fx::choice;
using fx::color;
using fx::flag;
using fx::num;

const QStringList kDirections = {QStringLiteral("From left"), QStringLiteral("From right"), QStringLiteral("From top"),
                                 QStringLiteral("From bottom")};

std::vector<ParamDef> center() { return {num("centerX", "Center X", 0, 1, 0.5), num("centerY", "Center Y", 0, 1, 0.5)}; }

void setColor(float* v, const QColor& c)
{
    v[0] = float(c.redF());
    v[1] = float(c.greenF());
    v[2] = float(c.blueF());
    v[3] = 1;
}

void set4(float* v, float a, float b = 0, float c = 0, float d = 0)
{
    v[0] = a;
    v[1] = b;
    v[2] = c;
    v[3] = d;
}

} // namespace

void addTransitions(std::vector<TransitionDef>& list)
{
    struct Entry {
        const char* id;
        const char* name;
        const char* description;
        std::vector<ParamDef> params;
    };
    auto withCenter = [](std::vector<ParamDef> params) {
        auto c = center();
        params.insert(params.begin(), c.begin(), c.end());
        return params;
    };
    const Entry entries[] = {
        {"glitch", "Glitch", "Digital breakup: bands tear sideways, colours split and blocks flicker over to the next picture.",
         {num("intensity", "Intensity", 0, 1, 0.7), num("bands", "Bands", 4, 60, 18, 0)}},
        {"ripple", "Ripple", "Rings spread out from a point like a drop in water, carrying the next picture with them.",
         withCenter({num("waves", "Waves", 1, 40, 10, 0), num("strength", "Strength", 0, 200, 30, 0, " px")})},
        {"swirl", "Swirl", "The picture twists into a whirlpool and untwists as the next one.",
         withCenter({num("twist", "Twist", -1440, 1440, 540, 0, "°"), num("radius", "Radius", 0.1, 2, 1)})},
        {"card_flip", "Card Flip", "The picture flips over in 3D like a card, with the next one on its back.",
         {choice("direction", "Direction", kDirections, 1)}},
        {"cube", "Cube Spin", "Both pictures are faces of a cube that turns to show the next one.",
         {choice("direction", "Direction", kDirections, 1)}},
        {"burn", "Film Burn", "The picture burns away in a glowing, ragged edge.",
         {color("color", "Flame color", 0xff6a00), color("color2", "Hot core", 0xffe9a0), num("edge", "Edge width", 0.01, 0.4, 0.08),
          num("grain", "Grain size", 0.1, 4, 1)}},
        {"luma_fade", "Luma Fade", "The brightest parts of the picture change first (or the darkest).",
         {flag("invert", "Darkest first", false), num("softness", "Softness", 0, 1, 0.25)}},
        {"shape_reveal", "Shape Reveal", "A growing star, heart, hexagon, triangle or cross reveals the next picture.",
         {choice("shape", "Shape",
                 {QStringLiteral("Star"), QStringLiteral("Heart"), QStringLiteral("Hexagon"), QStringLiteral("Triangle"), QStringLiteral("Cross")}),
          num("centerX", "Center X", 0, 1, 0.5), num("centerY", "Center Y", 0, 1, 0.5), num("angle", "Rotation", -180, 180, 0, 0, "°"),
          num("softness", "Softness", 0, 0.2, 0.005, 3), num("border", "Border width", 0, 60, 8, 0, " px"), color("color", "Border color", 0xffffff)}},
        {"mosaic", "Mosaic", "The picture breaks into ever bigger blocks, then sharpens again as the next one.",
         {num("size", "Largest block", 4, 300, 80, 0, " px")}},
        {"zoom_blur", "Zoom Blur", "Rushes into the picture with a radial blur and out of the next one.",
         withCenter({num("strength", "Strength", 0.05, 1, 0.6)})},
        {"tiles", "Tiles", "The next picture appears tile by tile.",
         {num("count", "Columns", 2, 48, 10, 0),
          choice("order", "Order", {QStringLiteral("Diagonal"), QStringLiteral("Random"), QStringLiteral("From center"), QStringLiteral("Left to right")}),
          num("softness", "Softness", 0, 0.5, 0.05)}},
        {"light_leak", "Light Leak", "Warm light flares across the lens while the pictures change underneath.",
         {color("color", "Color 1", 0xff8a3d), color("color2", "Color 2", 0xff3d8a), num("intensity", "Intensity", 0, 1.5, 1)}},
        {"flash", "Flash", "A burst of light hides the cut (white by default).", {color("color", "Color", 0xffffff)}},
        {"wave_wipe", "Wave Wipe", "A wipe with a wavy, moving edge.",
         {choice("direction", "Direction", kDirections), num("waves", "Waves", 0.5, 20, 3, 1), num("amplitude", "Amplitude", 0, 0.25, 0.05, 3),
          num("softness", "Softness", 0, 0.5, 0.04)}},
        {"slices", "Slices", "Strips of the next picture slide in from alternate sides.",
         {num("count", "Strips", 2, 48, 10, 0), choice("orientation", "Orientation", {QStringLiteral("Horizontal strips"), QStringLiteral("Vertical strips")})}},
        {"kaleidoscope", "Kaleidoscope", "The picture folds into a turning kaleidoscope and unfolds as the next one.",
         withCenter({num("segments", "Segments", 3, 16, 6, 0)})},
    };
    for (const Entry& e : entries) {
        TransitionDef d;
        d.id = QString::fromLatin1(e.id);
        d.name = QString::fromLatin1(e.name);
        d.description = QString::fromLatin1(e.description);
        d.params = e.params;
        const QString id = d.id;
        d.apply = [id](QImage& out, const QImage* a, const QImage* b, double p, const Params& prm, const RenderContext& ctx) {
            Uniforms u;
            setup(id, prm, p, out.size(), ctx.scale, u);
            render(out, a, b, u);
        };
        list.push_back(std::move(d));
    }
}

bool setup(const QString& id, const Params& prm, double pd, QSize size, double scale, Uniforms& u)
{
    u = {};
    const float p = float(std::clamp(pd, 0.0, 1.0));
    const float w = float(std::max(1, size.width())), h = float(std::max(1, size.height()));
    const float diag = std::hypot(w, h);
    const float bell = std::max(0.0f, std::sin(kPi * p)); // (sin(pi) in float is a hair below 0)
    const float sc = float(scale);
    u.p = p;
    u.w = w;
    u.h = h;
    auto n = [&](const char* key) { return float(prm.num(QString::fromLatin1(key))); };
    auto ch = [&](const char* key) { return prm.choice(QString::fromLatin1(key)); };
    const float cx = prm.numbers.count(QStringLiteral("centerX")) ? n("centerX") * w : w / 2;
    const float cy = prm.numbers.count(QStringLiteral("centerY")) ? n("centerY") * h : h / 2;
    const float maxR = std::hypot(std::max(cx, w - cx), std::max(cy, h - cy));
    // Direction choices: axis (0 = left/right, 1 = up/down) and sense.
    auto direction = [&](bool& horizontal, float& sign) {
        const int dir = ch("direction");
        horizontal = dir < 2;
        sign = (dir % 2 == 1) ? 1.0f : -1.0f;
    };

    if (id == QLatin1String("glitch")) {
        u.mode = 0;
        set4(u.a, n("intensity") * bell, std::max(1.0f, std::round(n("bands"))), std::floor(p * 12.0f));
    } else if (id == QLatin1String("ripple")) {
        u.mode = 1;
        set4(u.a, cx, cy, diag / std::max(1.0f, n("waves")), n("strength") * sc * bell);
        set4(u.b, std::max(1.0f, maxR), 0.3f);
    } else if (id == QLatin1String("swirl")) {
        u.mode = 2;
        set4(u.a, cx, cy, std::max(1.0f, n("radius") * diag / 2), n("twist") * kPi / 180.0f * bell);
        set4(u.b, ss(0.35f, 0.65f, p));
    } else if (id == QLatin1String("card_flip") || id == QLatin1String("cube")) {
        const bool cube = id == QLatin1String("cube");
        u.mode = cube ? 4 : 3;
        bool hor;
        float sign;
        direction(hor, sign);
        const float W = hor ? w : h, H = hor ? h : w;
        const float ang = cube ? ease(p) * kPi / 2 : ease(p) * kPi;
        const float push = cube ? W * 0.6f * bell : W * 0.5f * std::sin(ang);
        set4(u.a, W, H, 2.0f * W, ang);
        set4(u.b, hor ? 1.0f : 0.0f, sign, push);
    } else if (id == QLatin1String("burn")) {
        u.mode = 5;
        set4(u.a, std::max(4.0f, h * 0.18f * n("grain")), std::max(0.01f, n("edge")));
        setColor(u.color, prm.color(QStringLiteral("color")));
        setColor(u.color2, prm.color(QStringLiteral("color2")));
    } else if (id == QLatin1String("luma_fade")) {
        u.mode = 6;
        set4(u.a, prm.flag(QStringLiteral("invert")) ? 1.0f : 0.0f, n("softness"));
    } else if (id == QLatin1String("shape_reveal")) {
        u.mode = 7;
        const int shape = std::clamp(ch("shape"), 0, 4);
        const float soft = std::max(1.0f, n("softness") * h);
        const float kMax = (maxR + soft) / kShapeInRadius[shape];
        const float ang = n("angle") * kPi / 180.0f;
        set4(u.a, cx, cy, kMax * p, soft);
        set4(u.b, float(shape), std::cos(ang), std::sin(ang), (p > 0 && p < 1) ? n("border") * sc : 0.0f);
        setColor(u.color, prm.color(QStringLiteral("color")));
    } else if (id == QLatin1String("mosaic")) {
        u.mode = 8;
        const float k = ease(1.0f - std::abs(2.0f * p - 1.0f));
        set4(u.a, std::max(1.0f, std::round(1.0f + (std::max(1.0f, n("size") * sc) - 1.0f) * k)), ss(0.4f, 0.6f, p));
    } else if (id == QLatin1String("zoom_blur")) {
        u.mode = 9;
        const float s = n("strength"), e = ease(p);
        set4(u.a, cx, cy, s * bell, ss(0.4f, 0.6f, p));
        set4(u.b, 1.0f + s * e * 0.6f, 1.0f + s * (1.0f - e) * 0.6f);
    } else if (id == QLatin1String("tiles")) {
        u.mode = 10;
        const float cols = std::max(1.0f, std::round(n("count")));
        set4(u.a, cols, std::max(1.0f, std::round(cols * h / w)), float(ch("order")), n("softness"));
    } else if (id == QLatin1String("light_leak")) {
        u.mode = 11;
        set4(u.a, w * (-0.2f + 1.4f * p), h * (0.3f + 0.15f * bell), 0.45f * std::max(w, h), n("intensity") * std::pow(bell, 1.5f));
        set4(u.b, ss(0.35f, 0.65f, p));
        setColor(u.color, prm.color(QStringLiteral("color")));
        setColor(u.color2, prm.color(QStringLiteral("color2")));
    } else if (id == QLatin1String("flash")) {
        u.mode = 12;
        const float f = 1.0f - std::abs(2.0f * p - 1.0f);
        set4(u.a, f * f, p < 0.5f ? 0.0f : 1.0f);
        setColor(u.color, prm.color(QStringLiteral("color")));
    } else if (id == QLatin1String("wave_wipe")) {
        u.mode = 13;
        bool hor;
        float sign;
        direction(hor, sign);
        set4(u.a, hor ? 0.0f : 1.0f, sign > 0 ? -1.0f : 1.0f, n("waves"), n("amplitude"));
        set4(u.b, n("softness"));
    } else if (id == QLatin1String("slices")) {
        u.mode = 14;
        set4(u.a, std::max(1.0f, std::round(n("count"))), float(ch("orientation")));
    } else if (id == QLatin1String("kaleidoscope")) {
        u.mode = 15;
        set4(u.a, cx, cy, std::max(3.0f, std::round(n("segments"))), bell);
        set4(u.b, ss(0.4f, 0.6f, p), p * kPi * 0.5f);
    } else {
        return false;
    }
    return true;
}

void render(QImage& out, const QImage* from, const QImage* to, const Uniforms& u)
{
    const int w = out.width(), h = out.height();
    const Sampler A{from && !from->isNull() ? from : nullptr, from ? from->width() : 0, from ? from->height() : 0};
    const Sampler B{to && !to->isNull() ? to : nullptr, to ? to->width() : 0, to ? to->height() : 0};
    uchar* bits = out.bits();
    const qsizetype bpl = out.bytesPerLine();
    parallelFor(h, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(bits + bpl * y);
            for (int x = 0; x < w; ++x) {
                const V4 c = shade(u, V2{float(x) + 0.5f, float(y) + 0.5f}, A, B);
                const float a = clampf(c.a, 0, 1);
                auto chan = [a](float v) { return uint32_t(std::lround(clampf(v, 0, a) * 255.0f)); };
                row[x] = (uint32_t(std::lround(a * 255.0f)) << 24) | (chan(c.r) << 16) | (chan(c.g) << 8) | chan(c.b);
            }
        }
    });
}

// The same kernels in GLSL (keep in step with shade() above).
const char* const kGlsl = R"(
uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform int uMode;
uniform float uP;
uniform vec2 uSize;
uniform vec4 uA;
uniform vec4 uB;
uniform vec4 uColor;
uniform vec4 uColor2;
const float PI = 3.14159265358979;

float h01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
vec4 sA(vec2 q) {
    if (q.x < 0.0 || q.y < 0.0 || q.x > uSize.x || q.y > uSize.y) return vec4(0.0);
    return texture(uFrom, q / uSize);
}
vec4 sB(vec2 q) {
    if (q.x < 0.0 || q.y < 0.0 || q.x > uSize.x || q.y > uSize.y) return vec4(0.0);
    return texture(uTo, q / uSize);
}
vec2 mirrorPos(vec2 q) {
    vec2 m = mod(q, 2.0 * uSize);
    return vec2(m.x <= uSize.x ? m.x : 2.0 * uSize.x - m.x, m.y <= uSize.y ? m.y : 2.0 * uSize.y - m.y);
}
vec4 sAB(bool useB, vec2 q) { return useB ? sB(q) : sA(q); }
vec4 blend2(vec2 qa, vec2 qb, float m) {
    vec4 a = m < 1.0 ? sA(qa) : vec4(0.0);
    vec4 b = m > 0.0 ? sB(qb) : vec4(0.0);
    return mix(a, b, m);
}
float valueNoise(vec2 q) {
    vec2 i = floor(q);
    vec2 f = q - i;
    vec2 u = f * f * (3.0 - 2.0 * f);
    uint x = uint(i.x), y = uint(i.y);
    float a = h01(x, y, 9u), b = h01(x + 1u, y, 9u), c = h01(x, y + 1u, 9u), d = h01(x + 1u, y + 1u, 9u);
    float top = a + (b - a) * u.x, bottom = c + (d - c) * u.x;
    return top + (bottom - top) * u.y;
}
float fbm(vec2 q) {
    float s = 0.0, amp = 0.5, norm = 0.0;
    for (int i = 0; i < 4; ++i) { s += amp * valueNoise(q); norm += amp; q = q * 2.0 + vec2(17.3, 9.1); amp *= 0.5; }
    return s / norm;
}
float sdStar5(vec2 p, float r, float rf) {
    const vec2 k1 = vec2(0.809016994375, -0.587785252292);
    const vec2 k2 = vec2(-k1.x, k1.y);
    p.x = abs(p.x);
    p -= 2.0 * max(dot(k1, p), 0.0) * k1;
    p -= 2.0 * max(dot(k2, p), 0.0) * k2;
    p.x = abs(p.x);
    p.y -= r;
    vec2 ba = rf * vec2(-k1.y, k1.x) - vec2(0.0, 1.0);
    float h = clamp(dot(p, ba) / dot(ba, ba), 0.0, r);
    return length(p - ba * h) * sign(p.y * ba.x - p.x * ba.y);
}
float sdHeart(vec2 p) {
    p.x = abs(p.x);
    if (p.y + p.x > 1.0) return length(p - vec2(0.25, 0.75)) - sqrt(2.0) / 4.0;
    vec2 d1 = p - vec2(0.0, 1.0);
    vec2 d2 = p - vec2(0.5 * max(p.x + p.y, 0.0));
    return sqrt(min(dot(d1, d1), dot(d2, d2))) * sign(p.x - p.y);
}
float sdHexagon(vec2 p, float r) {
    const vec3 k = vec3(-0.866025404, 0.5, 0.577350269);
    p = abs(p);
    p -= 2.0 * min(dot(k.xy, p), 0.0) * k.xy;
    p -= vec2(clamp(p.x, -k.z * r, k.z * r), r);
    return length(p) * sign(p.y);
}
float sdTriangle(vec2 p, float r) {
    const float k = sqrt(3.0);
    p.x = abs(p.x) - r;
    p.y = p.y + r / k;
    if (p.x + k * p.y > 0.0) p = vec2(p.x - k * p.y, -k * p.x - p.y) / 2.0;
    p.x -= clamp(p.x, -2.0 * r, 0.0);
    return -length(p) * sign(p.y);
}
float sdCross(vec2 p, vec2 b) {
    p = abs(p);
    if (p.y > p.x) p = p.yx;
    vec2 q = p - b;
    float k = max(q.y, q.x);
    vec2 w = k > 0.0 ? q : vec2(b.y - p.x, -k);
    return sign(k) * length(max(w, 0.0));
}
float sdShape(int shape, vec2 p) {
    if (shape == 0) return sdStar5(p, 1.0, 0.5);
    if (shape == 1) return sdHeart(p + vec2(0.0, 0.5));
    if (shape == 2) return sdHexagon(p, 1.0);
    if (shape == 3) return sdTriangle(p, 1.0);
    return sdCross(p, vec2(1.0, 0.33));
}
float easeT(float t) { t = clamp(t, 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
// Card / cube face: x = hit, y = lambda, zw = face coordinates; front in `front`.
vec4 hitFace(vec2 l, float D, vec3 P, vec3 U, vec3 N, float hw, float hh, bool twoSided, out bool front) {
    vec3 dir = vec3(l, D), cam = vec3(0.0, 0.0, -D);
    float dn = dot(dir, N);
    front = dn < 0.0;
    if (abs(dn) < 1e-6 || (!twoSided && dn >= 0.0)) return vec4(0.0);
    float lam = dot(P - cam, N) / dn;
    vec3 q = cam + dir * lam;
    float s = dot(q - P, U), t = q.y - P.y;
    bool hit = lam > 0.0 && abs(s) <= hw && abs(t) <= hh;
    return vec4(hit ? 1.0 : 0.0, lam, s, t);
}

vec4 shadeFx(vec2 px) {
    float p = uP;
    if (uMode == 0) {
        float I = uA.x, bandH = uSize.y / uA.y;
        uint st = uint(uA.z);
        uint by = uint(floor(px.y / bandH));
        vec2 s = px;
        if (h01(by, st, 1u) < I * 0.7) s.x += (h01(by, st, 2u) - 0.5) * uSize.x * 0.3 * I;
        uint bx = uint(floor(px.x / (bandH * 2.0)));
        bool useB = h01(bx, by, st + 7u) < p;
        float sp = I * uSize.x * 0.015;
        vec4 cr = sAB(useB, vec2(s.x + sp, s.y)), cg = sAB(useB, s), cb = sAB(useB, vec2(s.x - sp, s.y));
        vec4 o = vec4(cr.r, cg.g, cb.b, max(max(cr.a, cg.a), cb.a));
        if (h01(by, st, 3u) < I * 0.3) o.rgb *= 0.6;
        return o;
    }
    if (uMode == 1) {
        vec2 c = uA.xy;
        vec2 d = px - c;
        float r = length(d);
        float off = uA.w * sin(2.0 * PI * (r / uA.z - p * 3.0));
        vec2 s = mirrorPos(r > 0.001 ? c + d * ((r + off) / r) : px);
        return blend2(s, s, wipeMask(r / uB.x, p, uB.y));
    }
    if (uMode == 2) {
        vec2 c = uA.xy;
        vec2 d = px - c;
        float r = length(d);
        vec2 s = px;
        if (r < uA.z) {
            float f = 1.0 - r / uA.z, an = uA.w * f * f;
            float cs = cos(an), sn = sin(an);
            s = mirrorPos(c + vec2(d.x * cs - d.y * sn, d.x * sn + d.y * cs));
        }
        return blend2(s, s, uB.x);
    }
    if (uMode == 3 || uMode == 4) {
        float W = uA.x, H = uA.y, D = uA.z, ang = uA.w;
        bool hor = uB.x > 0.5;
        float sg = uB.y, push = uB.z;
        vec2 l = hor ? vec2(px.x - uSize.x / 2.0, px.y - uSize.y / 2.0) : vec2(px.y - uSize.y / 2.0, px.x - uSize.x / 2.0);
        l.x *= sg;
        float cs = cos(ang), sn = sin(ang);
        vec4 best = vec4(0.0);
        bool onB = false;
        float light = 1.0;
        bool front;
        if (uMode == 3) {
            vec3 N = vec3(-sn, 0.0, -cs);
            best = hitFace(l, D, vec3(0.0, 0.0, push), vec3(cs, 0.0, -sn), N, W / 2.0, H / 2.0, true, front);
            onB = !front;
            if (onB) best.z = -best.z;
            light = 0.55 + 0.45 * abs(N.z);
        } else {
            vec3 O = vec3(0.0, 0.0, W / 2.0 + push);
            vec3 na = vec3(-sn, 0.0, -cs), nb = vec3(cs, 0.0, -sn);
            vec4 fa = hitFace(l, D, O + na * (W / 2.0), vec3(cs, 0.0, -sn), na, W / 2.0, H / 2.0, false, front);
            vec4 fb = hitFace(l, D, O + nb * (W / 2.0), vec3(sn, 0.0, cs), nb, W / 2.0, H / 2.0, false, front);
            if (fa.x > 0.5 && (fb.x < 0.5 || fa.y <= fb.y)) { best = fa; light = 0.55 + 0.45 * cs; }
            else if (fb.x > 0.5) { best = fb; onB = true; light = 0.55 + 0.45 * sn; }
        }
        if (best.x < 0.5) return vec4(0.0);
        vec2 img = hor ? vec2(best.z * sg + uSize.x / 2.0, best.w + uSize.y / 2.0) : vec2(best.w + uSize.x / 2.0, best.z * sg + uSize.y / 2.0);
        vec4 c = sAB(onB, img);
        return vec4(c.rgb * light, c.a);
    }
    if (uMode == 5) {
        float soft = uA.y;
        float s = ss(0.2, 0.8, fbm(px / uA.x));
        float m = wipeMask(s, p, soft);
        float ch = clamp((p * (1.0 + soft * 3.0) - s) / (soft * 3.0), 0.0, 1.0);
        vec4 a = m < 1.0 ? sA(px) : vec4(0.0);
        a.rgb *= 1.0 - 0.85 * ch;
        vec4 o = mix(a, m > 0.0 ? sB(px) : vec4(0.0), m);
        float dist = (s - (p * (1.0 + soft) - soft * 0.5)) / (soft * 1.5 + 0.02);
        float g = exp(-dist * dist) * clamp(min(p, 1.0 - p) * 12.0, 0.0, 1.0), k = g * g;
        o.rgb += (uColor.rgb + (uColor2.rgb - uColor.rgb) * k) * g * 1.2;
        o.a = max(o.a, g);
        return o;
    }
    if (uMode == 6) {
        vec4 a = sA(px);
        float l = a.r * 0.299 + a.g * 0.587 + a.b * 0.114;
        if (uA.x > 0.5) l = 1.0 - l;
        float m = wipeMask(l, p, uA.y);
        return mix(a, m > 0.0 ? sB(px) : vec4(0.0), m);
    }
    if (uMode == 7) {
        float k = uA.z;
        if (k < 1e-3) return sA(px);
        vec2 q = px - uA.xy;
        q = vec2(uB.y * q.x + uB.z * q.y, -(-uB.z * q.x + uB.y * q.y));
        float d = k * sdShape(int(uB.x + 0.5), q / k);
        float m = clamp(0.5 - d / uA.w, 0.0, 1.0);
        vec4 o = blend2(px, px, m);
        if (uB.w > 0.0) {
            float ring = clamp(min(d + 0.5, uB.w - d + 0.5), 0.0, 1.0);
            o = mix(o, vec4(uColor.rgb, 1.0), ring);
        }
        return o;
    }
    if (uMode == 8) {
        float block = uA.x;
        vec2 s = (floor(px / block) + 0.5) * block;
        s = clamp(s, vec2(0.5), uSize - 0.5);
        return blend2(s, s, uA.y);
    }
    if (uMode == 9) {
        vec2 c = uA.xy;
        float bl = uA.z, t = uA.w;
        vec4 acc = vec4(0.0);
        for (int i = 0; i < 12; ++i) {
            float f = 1.0 - bl * 0.25 * float(i) / 11.0;
            vec2 q = (px - c) * f;
            acc += blend2(c + q / uB.x, c + q / uB.y, t);
        }
        return acc * (1.0 / 12.0);
    }
    if (uMode == 10) {
        float cols = uA.x, rows = uA.y;
        vec2 cell = uSize / vec2(cols, rows);
        vec2 g = min(floor(px / cell), vec2(cols - 1.0, rows - 1.0));
        vec2 uv = (px - (g + 0.5) * cell) / (cell * 0.5);
        int order = int(uA.z + 0.5);
        float d;
        if (order == 0) d = ((cols > 1.0 ? g.x / (cols - 1.0) : 0.0) + (rows > 1.0 ? g.y / (rows - 1.0) : 0.0)) / 2.0;
        else if (order == 1) d = h01(uint(g.x), uint(g.y), 5u);
        else if (order == 2) d = min(length((g + 0.5) / vec2(cols, rows) - 0.5) / 0.70711, 1.0);
        else d = cols > 1.0 ? g.x / (cols - 1.0) : 0.0;
        float e = easeT((p - d * 0.6) / 0.4);
        return blend2(px, px, wipeMask(max(abs(uv.x), abs(uv.y)), e, uA.w));
    }
    if (uMode == 11) {
        vec4 o = blend2(px, px, uB.x);
        vec2 c = uA.xy;
        float R = uA.z, f = uA.w;
        vec2 d1 = (px - c) / R;
        vec2 d2 = (px - c - vec2(R * 0.8, R * 0.35)) / (R * 0.7);
        float i1 = f * exp(-dot(d1, d1)), i2 = f * exp(-dot(d2, d2)) * 0.8;
        vec3 lt = clamp(uColor.rgb * i1 + uColor2.rgb * i2, 0.0, 1.0);
        o.rgb *= 1.0 + 0.5 * f;
        o.a = max(o.a, min(1.0, max(i1, i2)));
        return vec4(o.rgb + lt * (1.0 - o.rgb), o.a);
    }
    if (uMode == 12) {
        float f = uA.x;
        vec4 base = uA.y > 0.5 ? sB(px) : sA(px);
        return vec4(base.rgb * (1.0 + 2.0 * f) + uColor.rgb * f, base.a + (1.0 - base.a) * f);
    }
    if (uMode == 13) {
        bool vert = uA.x > 0.5;
        float along = vert ? px.y / uSize.y : px.x / uSize.x;
        if (uA.y < 0.0) along = 1.0 - along;
        float perp = vert ? px.x / uSize.x : px.y / uSize.y;
        float amp = uA.w;
        float s = along + amp * sin(2.0 * PI * (perp * uA.z + p * 1.5));
        s = (s + amp) / (1.0 + 2.0 * amp);
        return blend2(px, px, wipeMask(s, p, uB.x));
    }
    if (uMode == 14) {
        float n = uA.x;
        bool vert = uA.y > 0.5;
        float along = vert ? px.y : px.x, perp = vert ? px.x : px.y;
        float L = vert ? uSize.y : uSize.x, Lp = vert ? uSize.x : uSize.y;
        float k = min(floor(perp / (Lp / n)), n - 1.0);
        float d = n > 1.0 ? k / (n - 1.0) * 0.4 : 0.0;
        float e = easeT((p - d) / 0.6);
        float off = (1.0 - e) * L * (mod(k, 2.0) < 0.5 ? -1.0 : 1.0);
        float sa = along - off;
        if (sa >= 0.0 && sa <= L) return sB(vert ? vec2(px.x, sa) : vec2(sa, px.y));
        return sA(px);
    }
    vec2 c = uA.xy;
    vec2 d = px - c;
    float r = length(d);
    float seg = 2.0 * PI / uA.z;
    float fa = mod(atan(d.y, d.x), seg);
    if (fa > seg * 0.5) fa = seg - fa;
    fa += uB.y;
    float k = uA.w;
    vec2 folded = c + vec2(cos(fa), sin(fa)) * (r * (1.0 - 0.3 * k));
    vec2 s = mirrorPos(px + (folded - px) * k);
    return blend2(s, s, uB.x);
}

void main() {
    vec4 c = shadeFx(vPix);
    float a = clamp(c.a, 0.0, 1.0);
    fragColor = vec4(clamp(c.rgb, vec3(0.0), vec3(a)), a);
}
)";

} // namespace tfx
