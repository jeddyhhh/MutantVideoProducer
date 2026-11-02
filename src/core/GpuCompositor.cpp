#include "core/GpuCompositor.h"
#include "core/OpticalFlow.h"
#include "core/Dither.h"
#include "core/CubeLut.h"

#include "core/EffectHelpers.h"
#include "core/GpuGlitchShaders.h"
#include "core/ConsoleEngine.h"
#include "core/GpuConsoleShaders.h"

#include "core/Compositor.h"
#include "core/FrameConverter.h"
#include "core/Hardware.h"
#include "core/MotionGraphics.h"
#include "core/NodeGraph.h"
#include "core/RetroShaders.h"
#include <QFile>
#include <QMatrix4x4>
#include "core/PanCrop.h"
#include "core/EffectMask.h"
#include "core/Proxy.h"
#include "core/Segmentation.h"
#include "core/Stabilizer.h"
#include "core/TransitionKernels.h"
#include "core/VideoDecoder.h"

#include <QGenericMatrix>
#include <QVector2D>
#include <QVector3D>
#include <QVector4D>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLPaintDevice>
#include <QPainter>
#include <QSurfaceFormat>
#include <algorithm>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <memory>
#include <vector>

namespace {

constexpr uint64_t kEvictAfterFrames = 90;
constexpr size_t kMaxDecoders = 24;
constexpr size_t kMaxPooledTargets = 24;

// ---------------------------------------------------------------------------
// Shaders. Coordinates: pixel (0,0) is the first row in memory (image top);
// we never display GL output directly, so no vertical flip is needed.
// ---------------------------------------------------------------------------

const char* kVertex = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform mat3 uXform;      // quad pixels -> target pixels
uniform vec2 uQuadSize;
uniform vec2 uTargetSize;
uniform vec4 uSrcRect;    // texture coordinate origin + extent
out vec2 vUv;
out vec2 vPix;
void main() {
    vec3 p = uXform * vec3(aPos * uQuadSize, 1.0); // (p.z: 1, but for a perspective transform: Corner Pin)
    vPix = p.xy / p.z;
    vUv = uSrcRect.xy + aPos * uSrcRect.zw;
    gl_Position = vec4(p.xy / uTargetSize * 2.0 - p.z, 0.0, p.z); // (divided by p.z: the picture in perspective)
}
)";

const char* kFragHeader = R"(#version 330 core
in vec2 vUv;
in vec2 vPix;
out vec4 fragColor;
float ss(float e0, float e1, float x) {
    if (e1 <= e0) return x < e0 ? 0.0 : 1.0;
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
float wipeMask(float s, float p, float soft) {
    if (soft <= 0.0001) return s < p ? 1.0 : 0.0;
    return clamp((p * (1.0 + soft) - s) / soft, 0.0, 1.0);
}
uint hash32(uint x, uint y, uint seed) {
    uint h = x * 374761393u + y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    return h ^ (h >> 16u);
}
)";

const char* kFragTex = R"(
uniform sampler2D uTex;
uniform float uOpacity;
void main() { fragColor = texture(uTex, vUv) * uOpacity; }
)";

const char* kFragEncodeYuv = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uLuma; // Kr, Kb
uniform bool uFullRange;
void main() {
    vec2 p = floor(vPix);
    float value;
    if (p.y < uSize.y) {
        vec3 rgb = texelFetch(uTex, ivec2(p), 0).rgb;
        value = dot(rgb, vec3(uLuma.x, 1.0-uLuma.x-uLuma.y, uLuma.y));
        if (!uFullRange) value = (16.0 + 219.0*value)/255.0;
    } else {
        bool blue = p.x < uSize.x*0.5;
        p.x = mod(p.x, uSize.x*0.5);
        // Chroma averages the corresponding 2x2 RGB block on the GPU.
        vec3 rgb = texture(uTex, (vec2(2.0*p.x+1.0, 2.0*(p.y-uSize.y)+1.0))/uSize).rgb;
        float y = dot(rgb, vec3(uLuma.x, 1.0-uLuma.x-uLuma.y, uLuma.y));
        value = blue ? (rgb.b-y)/(2.0*(1.0-uLuma.y)) : (rgb.r-y)/(2.0*(1.0-uLuma.x));
        value = (128.0 + (uFullRange ? 255.0 : 224.0)*value)/255.0;
    }
    fragColor = vec4(clamp(value, 0.0, 1.0), 0.0, 0.0, 1.0);
}
)";

const char* kFragSolid = R"(
uniform vec4 uColor;
void main() { fragColor = uColor; }
)";

const char* kFragYuv = R"(
uniform sampler2D uY;
uniform sampler2D uU;
uniform sampler2D uV;
uniform sampler2D uA;
uniform int uLayout;      // 0 planar, 1 semi-planar (NV12-style)
uniform bool uSwapUV;
uniform bool uHasAlpha;
uniform float uNorm;
uniform vec3 uOffset;
uniform vec3 uScale;
uniform mat3 uMatrix;
void main() {
    float y = texture(uY, vUv).r * uNorm;
    vec2 c = uLayout == 1 ? texture(uU, vUv).rg * uNorm : vec2(texture(uU, vUv).r, texture(uV, vUv).r) * uNorm;
    if (uSwapUV) c = c.yx;
    vec3 rgb = clamp(uMatrix * ((vec3(y, c) - uOffset) * uScale), 0.0, 1.0);
    float a = uHasAlpha ? clamp(texture(uA, vUv).r * uNorm, 0.0, 1.0) : 1.0;
    fragColor = vec4(rgb * a, a);
}
)";

// 3D LUT (.cube): trilinear lookup through a linear-filtered 3D texture.
const char* kFragLut3D = R"(
uniform sampler2D uTex;
uniform sampler3D uLut3;
uniform float uLutSize;
uniform float uStrength;
uniform vec3 uDomMin;
uniform vec3 uDomMax;
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = c; return; }
    vec3 u = clamp(c.rgb / c.a, 0.0, 1.0);
    vec3 n = clamp((u - uDomMin) / max(uDomMax - uDomMin, vec3(1e-6)), 0.0, 1.0);
    vec3 o = texture(uLut3, (n * (uLutSize - 1.0) + 0.5) / uLutSize).rgb;
    fragColor = vec4(clamp(mix(u, o, uStrength), 0.0, 1.0) * c.a, c.a);
}
)";

const char* kFragLut = R"(
uniform sampler2D uTex;
uniform sampler2D uLut;
float lut(float v, int ch) {
    vec4 t = texture(uLut, vec2((floor(v * 255.0 + 0.5) + 0.5) / 256.0, 0.5));
    return ch == 0 ? t.r : (ch == 1 ? t.g : t.b);
}
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = c; return; }
    vec3 u = clamp(c.rgb / c.a, 0.0, 1.0);
    fragColor = vec4(vec3(lut(u.r, 0), lut(u.g, 1), lut(u.b, 2)) * c.a, c.a);
}
)";

const char* kFragMatrix = R"(
uniform sampler2D uTex;
uniform mat3 uM;
uniform vec3 uO;
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = c; return; }
    vec3 u = clamp(c.rgb / c.a, 0.0, 1.0);
    fragColor = vec4(clamp(uM * u + uO, 0.0, 1.0) * c.a, c.a);
}
)";

const char* kFragBlur = R"(
uniform sampler2D uTex;
uniform vec2 uStep;
uniform float uSigma;
uniform int uRadius;
void main() {
    vec4 acc = texture(uTex, vUv);
    float ws = 1.0;
    for (int i = 1; i <= uRadius; ++i) {
        float w = exp(-float(i * i) / (2.0 * uSigma * uSigma));
        acc += (texture(uTex, vUv + uStep * float(i)) + texture(uTex, vUv - uStep * float(i))) * w;
        ws += 2.0 * w;
    }
    fragColor = acc / ws;
}
)";

// Where each uBlock x uBlock block has anything drawn: the first and last such columns and rows in it (r, g: first;
// b, a: last), r = 255 where nothing is (a scene's drawn bounds, exact, read back small).
// Optical flow (OpticalFlow.h): the first frame moved forward along the motion, the second back, mixed.
const char* kFragFlowMix = R"(
uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform sampler2D uFlow; // the motion from uFrom to uTo, in texture coordinates
uniform float uW;
float mismatch(vec2 v) {
    vec3 d = abs(texture(uFrom, vUv - uW * v).rgb - texture(uTo, vUv + (1.0 - uW) * v).rgb);
    return (d.r + d.g + d.b) * 255.0;
}
void main() {
    // As the CPU (OpticalFlow.cpp): the blended motion, or a neighbouring cell's when it takes the frames to the same colour.
    vec2 v = texture(uFlow, vUv).xy;
    ivec2 size = textureSize(uFlow, 0);
    ivec2 near = ivec2(clamp(vUv * vec2(size) - 0.5, vec2(0.0), vec2(size - 1)) + 0.5); // (the nearest cell, and two around it each way)
    float best = mismatch(v);
    for (int k = 0; k < 25; ++k) {
        if (best <= 24.0) break;
        vec2 m = texelFetch(uFlow, clamp(near + ivec2(k % 5 - 2, k / 5 - 2), ivec2(0), size - 1), 0).xy;
        float d = mismatch(m);
        if (d + 12.0 < best) { best = d; v = m; }
    }
    fragColor = mix(texture(uFrom, vUv - uW * v), texture(uTo, vUv + (1.0 - uW) * v), uW);
}
)";

// Noise reduction (as the CPU's flow::denoise): the 3 x 3 around each pixel in the frame and in the frames either side
// (followed along the motion), weighted by how alike they are.
const char* kFragDenoise = R"(
uniform sampler2D uCur;
uniform sampler2D uPrev;
uniform sampler2D uNext;
uniform sampler2D uFlowPrev; // the motion from uCur to uPrev / uNext, in texture coordinates
uniform sampler2D uFlowNext;
uniform int uHasPrev;
uniform int uHasNext;
uniform float uSim;
vec4 centre;
vec4 sum;
float total;
void taps(sampler2D tex, vec2 at, vec2 px) {
    for (int k = 0; k < 9; ++k) {
        ivec2 o = ivec2(k % 3 - 1, k / 3 - 1);
        vec4 q = texture(tex, at + vec2(o) * px);
        vec3 d = q.rgb - centre.rgb;
        float near = (o.x == 0 && o.y == 0) ? 1.0 : ((o.x == 0 || o.y == 0) ? 0.6 : 0.37);
        float w = near * exp(-dot(d, d) / 3.0 / (2.0 * uSim * uSim));
        sum += w * q;
        total += w;
    }
}
void main() {
    vec2 px = 1.0 / vec2(textureSize(uCur, 0));
    centre = texture(uCur, vUv);
    sum = vec4(0.0);
    total = 0.0;
    taps(uCur, vUv, px);
    if (uHasPrev != 0) taps(uPrev, vUv + texture(uFlowPrev, vUv).xy, px);
    if (uHasNext != 0) taps(uNext, vUv + texture(uFlowNext, vUv).xy, px);
    fragColor = sum / total;
}
)";

const char* kFragMaxAlpha = R"(
uniform sampler2D uTex;
uniform int uBlock;
void main() {
    ivec2 size = textureSize(uTex, 0), base = ivec2(gl_FragCoord.xy) * uBlock;
    ivec2 lo = ivec2(255), hi = ivec2(0);
    for (int y = 0; y < uBlock; ++y)
        for (int x = 0; x < uBlock; ++x) {
            ivec2 at = base + ivec2(x, y);
            if (at.x < size.x && at.y < size.y && texelFetch(uTex, at, 0).a > 0.0) {
                lo = min(lo, ivec2(x, y));
                hi = max(hi, ivec2(x, y));
            }
        }
    fragColor = vec4(vec2(lo), vec2(hi)) / 255.0;
}
)";

const char* kFragSharpen = R"(
uniform sampler2D uTex;
uniform sampler2D uBlur;
uniform float uAmount;
void main() {
    vec4 c = texture(uTex, vUv);
    vec4 b = texture(uBlur, vUv);
    fragColor = vec4(clamp(c.rgb + uAmount * (c.rgb - b.rgb), vec3(0.0), vec3(c.a)), c.a);
}
)";

const char* kFragBright = R"(
uniform sampler2D uTex;
uniform float uThreshold;
void main() {
    vec4 c = texture(uTex, vUv);
    float luma = dot(c.rgb, vec3(0.2126, 0.7152, 0.0722));
    fragColor = c * ss(uThreshold, min(1.0, uThreshold + 0.15), luma);
}
)";

const char* kFragScreen = R"(
uniform sampler2D uTex;
uniform sampler2D uGlow;
uniform float uIntensity;
void main() {
    vec4 a = texture(uTex, vUv);
    vec4 b = min(texture(uGlow, vUv) * uIntensity, vec4(1.0));
    fragColor = a + b - a * b;
}
)";

// A track matte (TrackMatte uMode, as imageops::applyMatte): the layer kept where the matte is drawn or bright.
const char* kFragMatte = R"(
uniform sampler2D uLayer;
uniform sampler2D uMatte;
uniform int uMode;
void main() {
    vec4 m = texture(uMatte, vUv);
    float k = uMode <= 2 ? m.a : dot(m.rgb, vec3(0.2126, 0.7152, 0.0722));
    if (uMode == 2 || uMode == 4) k = 1.0 - k;
    fragColor = texture(uLayer, vUv) * k;
}
)";

// The Effect Builder's own nodes (core/NodeGraph.cpp, same formulas): uTex, uB, uC, uD are its inputs (uHas: which
// are wired), uP its settings.
const char* kFragNode = R"(
uniform sampler2D uTex;
uniform sampler2D uB;
uniform sampler2D uC;
uniform sampler2D uD;
uniform int uMode;
uniform ivec4 uHas;
uniform vec4 uP;
uniform vec2 uSize;
float luma(vec4 p) { return dot(p.rgb, vec3(54.0, 183.0, 19.0) / 256.0); }
float op(int o, float a, float b) {
    if (o == 0) return a + b;
    if (o == 1) return a - b;
    if (o == 2) return a * b;
    if (o == 3) return b > 0.004 ? a / b : 1.0;
    if (o == 4) return min(a, b);
    if (o == 5) return max(a, b);
    if (o == 6) return abs(a - b);
    return 0.5 * (a + b);
}
void main() {
    vec4 a = texture(uTex, vUv);
    vec4 b = texture(uB, vUv);
    if (uMode == 0) { // mix
        fragColor = mix(a, b, uP.x);
    } else if (uMode == 1) { // math (alpha: what either has; the colour kept within it)
        int o = int(uP.x);
        vec3 c = clamp(vec3(op(o, a.r, b.r), op(o, a.g, b.g), op(o, a.b, b.b)) * uP.y, 0.0, 1.0);
        float al = max(a.a, b.a);
        fragColor = vec4(min(c, vec3(al)), al);
    } else if (uMode == 2) { // set alpha
        vec3 c = a.a > 0.0 ? a.rgb / a.a : vec3(0.0);
        float k = uHas.y != 0 ? luma(b) : 1.0;
        if (uP.x > 0.5) k = 1.0 - k;
        fragColor = vec4(c * k, k);
    } else if (uMode == 3) { // luma key
        float l = a.a > 0.0 ? luma(a) / a.a : 0.0;
        float k = clamp((l - uP.x) / (uP.y - uP.x), 0.0, 1.0);
        k = k * k * (3.0 - 2.0 * k);
        if (uP.z > 0.5) k = 1.0 - k;
        fragColor = a * k;
    } else if (uMode == 4) { // difference key
        vec3 d3 = abs(a.rgb - b.rgb);
        float d = max(d3.r, max(d3.g, d3.b));
        float k = uHas.y != 0 ? clamp((d - uP.x) / uP.y, 0.0, 1.0) : 1.0;
        fragColor = a * k;
    } else if (uMode == 5) { // channel
        int ch = int(uP.x);
        if (ch == 3) { fragColor = vec4(vec3(a.a), 1.0); return; }
        float v = ch == 0 ? a.r : ch == 1 ? a.g : ch == 2 ? a.b : luma(a);
        fragColor = vec4(vec3(v), a.a);
    } else if (uMode == 6) { // combine channels
        float al = uHas.w != 0 ? luma(texture(uD, vUv)) : 1.0;
        vec3 c = vec3(uHas.x != 0 ? luma(a) : 0.0, uHas.y != 0 ? luma(b) : 0.0, uHas.z != 0 ? luma(texture(uC, vUv)) : 0.0);
        fragColor = vec4(c * al, al);
    } else if (uMode == 7) { // threshold
        float l = a.a > 0.0 ? luma(a) / a.a : 0.0;
        float k = clamp((l - uP.x) / uP.y + 0.5, 0.0, 1.0);
        fragColor = vec4(vec3(k * a.a), a.a);
    } else { // displace (uP: pixels across and up / down, red-green)
        if (uHas.y == 0) { fragColor = a; return; }
        float mx = uP.z > 0.5 ? b.r : luma(b);
        float my = uP.z > 0.5 ? b.g : mx;
        fragColor = texture(uTex, vUv + vec2((mx - 0.5) * 2.0 * uP.x, (my - 0.5) * 2.0 * uP.y) / uSize);
    }
}
)";

// The motion between two frames (as flow::estimate, OpticalFlow.cpp), worked out on the GPU: uMode 0 a grey copy (as
// QImage's Grayscale8), 1 half its size, 2 block matches for each cell of 8 pixels around the coarser level's guess,
// 3 the 3 x 3 median of the vectors, 4 the vectors as fractions of the picture. Pixels by texelFetch, in float targets.
const char* kFragFlow = R"(
uniform sampler2D uA;    // the grey picture (r), or the vectors (rg)
uniform sampler2D uB;    // match: the other grey picture
uniform sampler2D uPrev; // match: the coarser level's vectors
uniform int uMode;
uniform ivec4 uI;        // match: search radius, whether there is a coarser level, its size
uniform ivec2 uOut;      // the size drawn
uniform vec2 uScale;     // fractions: 1 / the picture's size
float at(sampler2D t, int x, int y) {
    ivec2 s = textureSize(t, 0);
    return texelFetch(t, ivec2(clamp(x, 0, s.x - 1), clamp(y, 0, s.y - 1)), 0).r;
}
float roundAway(float v) { return v < 0.0 ? -floor(-v + 0.5) : floor(v + 0.5); }
float median9(float n[9]) {
    for (int i = 0; i < 5; ++i)
        for (int j = i + 1; j < 9; ++j)
            if (n[j] < n[i]) { float t = n[i]; n[i] = n[j]; n[j] = t; }
    return n[4];
}
void main() {
    ivec2 p = ivec2(gl_FragCoord.xy);
    if (uMode == 0) {
        vec3 q = floor(texelFetch(uA, p, 0).rgb * 255.0 + 0.5);
        fragColor = vec4(floor((q.r * 11.0 + q.g * 16.0 + q.b * 5.0) / 32.0), 0.0, 0.0, 1.0);
    } else if (uMode == 1) {
        int x = 2 * p.x, y = 2 * p.y;
        fragColor = vec4(0.25 * (at(uA, x, y) + at(uA, x + 1, y) + at(uA, x, y + 1) + at(uA, x + 1, y + 1)), 0.0, 0.0, 1.0);
    } else if (uMode == 2) {
        vec2 guess = vec2(0.0);
        if (uI.y != 0) {
            ivec2 from = ivec2(min(p.x * uI.z / uOut.x, uI.z - 1), min(p.y * uI.w / uOut.y, uI.w - 1));
            guess = texelFetch(uPrev, from, 0).rg * 2.0;
        }
        int gx = int(roundAway(guess.x)), gy = int(roundAway(guess.y));
        int x0 = p.x * 8, y0 = p.y * 8, r = uI.x;
        float best = 1e30;
        int bx = gx, by = gy;
        for (int dy = gy - r; dy <= gy + r; ++dy)
            for (int dx = gx - r; dx <= gx + r; ++dx) {
                float sad = 0.0;
                for (int y = 0; y < 8 && sad < best; ++y)
                    for (int x = 0; x < 8; ++x)
                        sad += abs(at(uA, x0 + x, y0 + y) - at(uB, x0 + x + dx, y0 + y + dy));
                sad += 0.5 * float(abs(dx - gx) + abs(dy - gy)); // (a little against wandering from the guess)
                if (sad < best) { best = sad; bx = dx; by = dy; }
            }
        fragColor = vec4(float(bx), float(by), 0.0, 1.0);
    } else if (uMode == 3) {
        ivec2 s = textureSize(uA, 0);
        float nx[9], ny[9];
        int k = 0;
        for (int j = -1; j <= 1; ++j)
            for (int i = -1; i <= 1; ++i) {
                vec2 v = texelFetch(uA, ivec2(clamp(p.x + i, 0, s.x - 1), clamp(p.y + j, 0, s.y - 1)), 0).rg;
                nx[k] = v.x; ny[k] = v.y; ++k;
            }
        fragColor = vec4(median9(nx), median9(ny), 0.0, 1.0);
    } else {
        fragColor = vec4(texelFetch(uA, p, 0).rg * uScale, 0.0, 1.0);
    }
}
)";

// Generators the GPU works out itself (GeneratorsAnimated.cpp, the same formulas, at a quarter of the size as there):
// uMode 0 Plasma (uP: time, scale, aspect, the number of colour stops in uC), 1 Aurora (uP: time, aspect; uC: the light,
// top, sky and horizon colours).
const char* kFragGen = R"(
uniform int uMode;
uniform vec4 uP;
uniform vec4 uC[6];
uniform vec2 uSize;
float hash01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
float valueNoise1(float x, uint seed) {
    float i = floor(x), f = x - i;
    float u = f * f * (3.0 - 2.0 * f);
    uint xi = uint(int(i));
    float a = hash01(xi, 0u, seed), b = hash01(xi + 1u, 0u, seed);
    return a + (b - a) * u;
}
void main() {
    vec2 at = gl_FragCoord.xy / uSize; // ((x + 0.5) / w, (y + 0.5) / h)
    if (uMode == 0) {
        float T = uP.x, sc = uP.y, aspect = uP.z;
        float X = at.x * aspect * 3.0 / sc, Y = at.y * 3.0 / sc;
        float v = sin(X + T) + sin(Y * 1.3 - T * 0.7) + sin((X + Y) * 0.7 + T * 0.5)
                + sin(length(vec2(X - 1.5 * aspect / sc + sin(T * 0.3), Y - 1.5 / sc + cos(T * 0.4))) * 2.0 - T * 1.3);
        float f = fract(v / 8.0 + 0.5 + T * 0.03);
        int n = int(uP.w);
        float g = (f < 0.5 ? f * 2.0 : 2.0 - f * 2.0) * float(n - 1);
        int i = clamp(int(g), 0, n - 2);
        vec3 c = mix(uC[i].rgb, uC[i + 1].rgb, clamp(g - float(i), 0.0, 1.0));
        fragColor = vec4(floor(c * 255.0 + 0.5) / 255.0, 1.0);
    } else {
        float T = uP.x, X = at.x * uP.y, Y = at.y;
        vec3 col = mix(uC[2].rgb, uC[3].rgb, Y);
        for (int k = 0; k < 3; ++k) {
            float yc = 0.32 + 0.1 * float(k) + 0.3 * (valueNoise1(X * 1.4 + float(k) * 7.1 + T * 0.12, 30u + uint(k)) - 0.5);
            yc += 0.04 * sin(X * 6.0 + T * 0.8 + float(k));
            float d = Y - yc;
            float shape = d < 0.0 ? exp(d / 0.16) : exp(-d * d / 0.0009);
            float rays = 0.45 + 0.55 * valueNoise1(X * 45.0 + float(k) * 13.0 + T * 0.5, 40u + uint(k));
            col += mix(uC[0].rgb, uC[1].rgb, clamp(-d * 3.5, 0.0, 1.0)) * (shape * rays * (0.85 - 0.2 * float(k)));
        }
        fragColor = vec4(floor(clamp(col, 0.0, 1.0) * 255.0) / 255.0, 1.0);
    }
}
)";

const char* kFragRackBlend = R"(
uniform sampler2D uBase;
uniform sampler2D uLayer;
uniform float uOpacity;
uniform int uMode;
float blendChannel(float b, float s) {
    if (uMode == 1) return min(1.0, b + s);
    if (uMode == 2) return b + s - b * s;
    if (uMode == 3) return b * s;
    if (uMode == 4) return b <= 0.5 ? 2.0 * b * s : 1.0 - 2.0 * (1.0 - b) * (1.0 - s);
    if (uMode == 5) return max(b, s);
    if (uMode == 6) return min(b, s);
    if (uMode == 7) return abs(b - s);
    return s;
}
void main() {
    vec4 B = texture(uBase, vUv);
    vec4 L = texture(uLayer, vUv);
    if (L.a <= 0.0) { fragColor = B; return; }
    vec3 cb = B.a > 0.0 ? B.rgb / B.a : vec3(0.0);
    vec3 cs = L.rgb / L.a;
    vec3 mixed = (1.0 - B.a) * cs + B.a * vec3(blendChannel(cb.r, cs.r), blendChannel(cb.g, cs.g), blendChannel(cb.b, cs.b));
    float a = L.a * uOpacity;
    fragColor = vec4(mixed * a + B.rgb * (1.0 - a), a + B.a * (1.0 - a));
}
)";


// ---- Glitch / psychedelic effects (same formulas as GlitchEffects.cpp) ----
const char* kFragWarp = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform int uMode;
uniform int uEdge;      // 0 clamp, 1 wrap, 2 mirror, 3 transparent
uniform vec4 uA;
uniform vec4 uB;
uniform int uFrame;
const float PI = 3.14159265358979;
vec4 fetchAt(vec2 pix) {
    vec2 uv = pix / uSize;
    if (uEdge == 1) uv = fract(uv);
    else if (uEdge == 2) { vec2 m = mod(uv, 2.0); uv = mix(m, 2.0 - m, step(1.0, m)); }
    return texture(uTex, uv);
}
float hash01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
void main() {
    vec2 p = vPix;
    vec2 s = p;
    bool visible = true;
    if (uMode == 0) {            // wave: dir, amp, len, phase
        if (uA.x != 1.0) s.x += uA.y * sin(2.0 * PI * (p.y / uA.z - uA.w));
        if (uA.x != 0.0) s.y += uA.y * sin(2.0 * PI * (p.x / uA.z - uA.w));
    } else if (uMode == 1) {     // twirl: angle, radius, cx, cy
        vec2 d = p - uA.zw;
        float r = length(d);
        if (r < uA.y) {
            float k = 1.0 - r / uA.y, a = uA.x * k * k;
            s = uA.zw + vec2(d.x * cos(a) - d.y * sin(a), d.x * sin(a) + d.y * cos(a));
        }
    } else if (uMode == 2) {     // ripple: amp, len, phase, decay; cx, cy, diag
        vec2 d = p - uB.xy;
        float r = length(d);
        float off = uA.x * sin(2.0 * PI * (r / uA.y - uA.z)) * exp(-uA.w * 4.0 * r / uB.z);
        float k = r > 0.001 ? (r + off) / r : 1.0;
        s = uB.xy + d * k;
    } else if (uMode == 3) {     // lens: k, zoom, norm
        vec2 c = uSize * 0.5;
        vec2 d = (p - c) / uA.z;
        float f = (1.0 + uA.x * dot(d, d)) / uA.y;
        s = c + d * f * uA.z;
        visible = s.x >= 0.0 && s.y >= 0.0 && s.x <= uSize.x && s.y <= uSize.y;
    } else if (uMode == 4) {     // kaleidoscope: segments, rot, zoom; cx, cy
        vec2 d = p - uB.xy;
        float r = length(d) / uA.z;
        float seg = 2.0 * PI / uA.x;
        float a = atan(d.y, d.x) - uA.y;
        a -= floor(a / seg) * seg;
        if (a > seg * 0.5) a = seg - a;
        a += uA.y;
        s = uB.xy + r * vec2(cos(a), sin(a));
    } else if (uMode == 5) {     // mirror symmetry (integer pixel logic)
        ivec2 ip = ivec2(floor(p));
        ivec2 sz = ivec2(uSize);
        bool left = ip.x < sz.x / 2, top = ip.y < sz.y / 2;
        int m = int(uA.x);
        if ((m == 0 && !left) || (m == 1 && left) || (m == 4 && !left)) s.x = uSize.x - p.x;
        if ((m == 2 && !top) || (m == 3 && top) || (m == 4 && !top)) s.y = uSize.y - p.y;
    } else if (uMode == 6) {     // polar: toPolar, rmax
        vec2 c = uSize * 0.5;
        if (uA.x > 0.5) {
            vec2 d = (p - c) / uA.y;
            float a = atan(d.x, -d.y) / (2.0 * PI) + 0.5;
            float r = length(d);
            s = vec2(a * uSize.x, r * uSize.y);
            visible = r <= 1.0;
        } else {
            float a = (p.x / uSize.x - 0.5) * 2.0 * PI, r = p.y / uSize.y * uA.y;
            s = c + vec2(r * sin(a), -r * cos(a));
        }
    } else if (uMode == 7) {     // tile: tx, ty, scrollX, scrollY
        s = vec2((p.x / uSize.x * uA.x + uA.z) * uSize.x, (p.y / uSize.y * uA.y + uA.w) * uSize.y);
    } else if (uMode == 8) {     // shake: dx, dy, cos, sin; zoom
        vec2 c = uSize * 0.5;
        vec2 u = (p - c - uA.xy) / uB.x;
        s = c + vec2(u.x * uA.z - u.y * uA.w, u.x * uA.w + u.y * uA.z);
    } else if (uMode == 9) {     // melt: amount, freq, phase, up
        vec4 src = texelFetch(uTex, ivec2(floor(p)), 0);
        float l = (0.299 * floor(src.r * 255.0 + 0.5) + 0.587 * floor(src.g * 255.0 + 0.5) + 0.114 * floor(src.b * 255.0 + 0.5)) / 255.0;
        float wave = 0.5 + 0.5 * sin(2.0 * PI * (p.x / uSize.x * uA.y + uA.z));
        s.y = p.y + (uA.w > 0.5 ? 1.0 : -1.0) * uA.x * l * wave;
    } else if (uMode == 10) {    // sync wobble: amp, freq, phase, noise; roll, bar
        float yy = p.y - uB.x;
        yy -= floor(yy / uSize.y) * uSize.y;
        visible = !(uB.y > 0.0 && yy > uSize.y - uB.y);
        s.y = yy;
        s.x = p.x - uA.x * sin(2.0 * PI * (yy / uSize.y * uA.y + uA.z)) - uA.w * (hash01(uint(int(yy)), uint(uFrame), 71u) - 0.5) * 2.0;
    }
    if (!visible || (uEdge == 3 && (s.x < 0.0 || s.y < 0.0 || s.x > uSize.x || s.y > uSize.y))) {
        fragColor = vec4(0.0);
        return;
    }
    fragColor = fetchAt(s);
}
)";

const char* kFragChroma = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform int uRadial;
uniform float uK;
uniform vec2 uCenter;  // radial: centre, falloff, green shift, half diagonal
uniform float uFalloff;
uniform float uGs;
uniform float uRMax;
uniform int uEdge;     // 0 clamp, 1 wrap, 2 mirror, 3 transparent
uniform vec2 uR;   // offsets (pixels): sample at p - offset
uniform vec2 uG;
uniform vec2 uB;
vec4 at(vec2 pix) {
    if (uEdge == 1) pix = mod(pix, uSize);
    else if (uEdge == 2) { vec2 m = mod(pix, 2.0 * uSize); pix = mix(m, 2.0 * uSize - m, step(uSize, m)); }
    else if (uEdge == 3 && (pix.x < 0.0 || pix.y < 0.0 || pix.x > uSize.x || pix.y > uSize.y)) return vec4(0.0);
    return texture(uTex, pix / uSize);
}
void main() {
    vec2 p = vPix;
    vec4 r, g, b;
    if (uRadial == 1) {
        vec2 c = uCenter, d = p - c;
        float f = uFalloff == 1.0 ? 1.0 : pow(max(length(d) / uRMax, 1e-4), uFalloff - 1.0);
        r = at(c + d * (1.0 + uK * f));
        g = uGs == 0.0 ? at(p) : at(c + d * (1.0 + uGs * uK * f));
        b = at(c + d * (1.0 - uK * f));
    } else {
        r = at(p - uR);
        g = at(p - uG);
        b = at(p - uB);
    }
    float a = max(max(r.a, g.a), b.a);
    fragColor = vec4(min(vec3(r.r, g.g, b.b), vec3(a)), a);
}
)";

const char* kFragRadialPass = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec2 uCenter;
uniform int uSpin;
uniform vec4 uP;   // spin: cos, sin; zoom: zoomIn, zoomOut
vec4 at(vec2 pix) { return texture(uTex, pix / uSize); }
void main() {
    vec2 d = vPix - uCenter;
    vec4 a, b;
    if (uSpin == 1) {
        a = at(uCenter + vec2(d.x * uP.x - d.y * uP.y, d.x * uP.y + d.y * uP.x));
        b = at(uCenter + vec2(d.x * uP.x + d.y * uP.y, -d.x * uP.y + d.y * uP.x));
    } else {
        a = at(uCenter + d * uP.x);
        b = at(uCenter + d * uP.y);
    }
    fragColor = (a + b) * 0.5;
}
)";

const char* kFragCrt = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec4 uA;    // scanlines, lineCount, curvature, vignette
uniform vec4 uB;    // brightness, mask type, mask k, pitch
const float PI = 3.14159265358979;
void main() {
    vec2 uv = vPix / uSize * 2.0 - 1.0;
    float r2 = dot(uv, uv);
    uv *= 1.0 + uA.z * r2;
    if (abs(uv.x) > 1.0 || abs(uv.y) > 1.0) { fragColor = vec4(0.0, 0.0, 0.0, 1.0); return; }
    vec4 c = texture(uTex, (uv + 1.0) * 0.5);
    float phase = (uv.y + 1.0) * 0.5 * uA.y;
    float k = (1.0 - uA.x * (0.5 - 0.5 * cos(2.0 * PI * phase))) * uB.x;
    k *= 1.0 - uA.w * (uv.x * uv.x * uv.y * uv.y * 3.0 + r2 * 0.15);
    vec3 m = vec3(1.0);
    int mask = int(uB.y);
    if (mask > 0) {
        int pitch = int(uB.w);
        ivec2 ip = ivec2(floor(vPix));
        int col = (ip.x / pitch) % 3;
        if (mask == 2 && ((ip.y / pitch) & 1) == 1) col = (col + 1) % 3;
        if (mask == 3 && ((ip.y / (pitch * 2)) % 2) == 1 && ((ip.x / (pitch * 3)) % 2) == 1) col = (col + 1) % 3;
        m = vec3(col == 0 ? 1.0 : uB.z, col == 1 ? 1.0 : uB.z, col == 2 ? 1.0 : uB.z);
    }
    fragColor = vec4(min(c.rgb * k * m, vec3(c.a)), c.a);
}
)";

const char* kFragPsy = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform vec4 uA;   // base angle (deg), by luma (deg), by position (deg), saturation
void main() {
    vec4 c = texture(uTex, vUv);
    vec3 rgb = c.rgb * 255.0;
    float y = 0.299 * rgb.r + 0.587 * rgb.g + 0.114 * rgb.b;
    float i = 0.596 * rgb.r - 0.274 * rgb.g - 0.322 * rgb.b;
    float q = 0.211 * rgb.r - 0.523 * rgb.g + 0.312 * rgb.b;
    float x = floor(vPix.x);
    float a = (uA.x + uA.y * y / 255.0 + uA.z * x / uSize.x) * 3.14159265358979 / 180.0;
    float cs = cos(a) * uA.w, sn = sin(a) * uA.w;
    float i2 = i * cs - q * sn, q2 = i * sn + q * cs;
    vec3 o = vec3(y + 0.956 * i2 + 0.621 * q2, y - 0.272 * i2 - 0.647 * q2, y - 1.106 * i2 + 1.703 * q2) / 255.0;
    fragColor = vec4(clamp(o, vec3(0.0), vec3(c.a)), c.a);
}
)";


const char* kFragVignette = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform float uAmount;
uniform float uSizeP;
uniform float uSoft;
uniform float uRound;
void main() {
    vec4 c = texture(uTex, vUv);
    float xs = (1.0 - uRound) * (2.0 / uSize.x) + uRound * (2.0 / uSize.y);
    float dx = (vPix.x - uSize.x / 2.0) * xs;
    float dy = (vPix.y - uSize.y / 2.0) * (2.0 / uSize.y);
    float v = ss(uSizeP * (1.0 - uSoft), uSizeP + 1e-4, sqrt(dx * dx + dy * dy) / 1.41421);
    fragColor = uAmount > 0.0 ? vec4(c.rgb * (1.0 - uAmount * v), c.a)
                              : vec4(c.rgb + (vec3(c.a) - c.rgb) * (-uAmount) * v, c.a);
}
)";

const char* kFragKey = R"(
uniform sampler2D uTex;
uniform vec3 uKey;
uniform float uTol;
uniform float uSoft;
uniform float uSpill;
uniform bool uMask;
vec2 cbcr(vec3 c) { return vec2(-0.1146 * c.r - 0.3854 * c.g + 0.5 * c.b, 0.5 * c.r - 0.4542 * c.g - 0.0458 * c.b); }
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = c; return; }
    vec3 u = c.rgb / c.a;
    float dist = length(cbcr(u) - cbcr(uKey)) / 0.7;
    float alpha = ss(uTol, uTol + uSoft + 1e-4, dist);
    float s = uSpill * (1.0 - ss(uTol, uTol + uSoft + 0.35, dist));
    if (s > 0.0) { float l = dot(u, vec3(0.2126, 0.7152, 0.0722)); u += (vec3(l) - u) * s; }
    float a = c.a * alpha;
    fragColor = uMask ? vec4(vec3(a), 1.0) : vec4(clamp(u, 0.0, 1.0) * a, a);
}
)";

const char* kFragNoise = R"(
uniform sampler2D uTex;
uniform float uStrength;  // 0..128
uniform bool uMono;
uniform uint uSeed;
void main() {
    vec4 c = texture(uTex, vUv);
    if (c.a <= 0.0) { fragColor = c; return; }
    uint h = hash32(uint(vPix.x), uint(vPix.y), uSeed);
    vec3 r;
    for (int i = 0; i < 3; ++i) {
        uint bits = uMono ? h : (h >> uint(i * 8)) * 2654435761u;
        float n = float(int((bits >> 8u) & 255u) - 128);
        r[i] = clamp(c[i] + n * uStrength / 128.0 / 255.0 * c.a, 0.0, c.a);
    }
    fragColor = vec4(r, c.a);
}
)";

const char* kFragPixelate = R"(
uniform sampler2D uTex;
uniform vec2 uSize;
uniform float uBlock;
void main() {
    vec2 b0 = floor(vPix / uBlock) * uBlock;
    vec2 b1 = min(b0 + uBlock, uSize);
    vec4 acc = vec4(0.0);
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            acc += texture(uTex, (b0 + (b1 - b0) * (vec2(i, j) + 0.5) / 4.0) / uSize);
    fragColor = acc / 16.0;
}
)";

const char* kFragTransition = R"(
uniform sampler2D uFrom;
uniform sampler2D uTo;
uniform int uMode;
uniform float uP;
uniform vec4 uColor;
uniform vec4 uA;
uniform vec4 uB;
uniform vec2 uSize;
void main() {
    vec4 a = texture(uFrom, vUv);
    vec4 b = texture(uTo, vUv);
    vec2 px = vPix;
    float m = 0.0;
    if (uMode == 0) { fragColor = mix(a, b, uP); return; }
    if (uMode == 1) { fragColor = uP < 0.5 ? mix(a, uColor, uP * 2.0) : mix(uColor, b, (uP - 0.5) * 2.0); return; }
    if (uMode == 2) {
        float proj = (px.x - uSize.x / 2.0) * uA.x + (px.y - uSize.y / 2.0) * uA.y;
        m = wipeMask(0.5 + proj / (2.0 * uA.z), uP, uA.w);
    } else if (uMode == 3) {
        vec2 d = abs(px - uA.xy);
        float s = uA.z < 0.5 ? length(d) / uB.z : (uA.z < 1.5 ? (d.x + d.y) / uB.z : max(d.x / uB.x, d.y / uB.y));
        m = wipeMask(s, uP, uA.w);
    } else if (uMode == 4) {
        float ang = atan(px.x - uSize.x / 2.0, -(px.y - uSize.y / 2.0)) / 6.28318531;
        if (uA.z > 0.5) ang = -ang;
        float s = ang - uA.x;
        s -= floor(s);
        m = wipeMask(s, uP, uA.y);
    } else if (uMode == 5) {
        float s = uA.x > 0.5 ? abs(px.y / uSize.y - 0.5) * 2.0 : abs(px.x / uSize.x - 0.5) * 2.0;
        m = wipeMask(s, uP, uA.y);
    } else if (uMode == 6) {
        float s = uA.y > 0.5 ? px.x / uSize.x * uA.x : px.y / uSize.y * uA.x;
        s -= floor(s);
        m = wipeMask(s, uP, uA.z);
    } else if (uMode == 7) {
        uint h = uint(floor(px.x / uA.x)) * 374761393u + uint(floor(px.y / uA.x)) * 668265263u;
        h = (h ^ (h >> 13u)) * 1274126177u;
        h ^= h >> 16u;
        m = float(h & 0xffffffu) / 16777216.0 < uP ? 1.0 : 0.0;
    }
    fragColor = mix(a, b, m);
}
)";

QMatrix3x3 toMat3(const QTransform& t)
{
    const float v[9] = {float(t.m11()), float(t.m21()), float(t.dx()),  float(t.m12()), float(t.m22()),
                        float(t.dy()),  float(t.m13()), float(t.m23()), float(t.m33())};
    return QMatrix3x3(v);
}

QMatrix3x3 rowsToMat3(const float* rows)
{
    return QMatrix3x3(rows);
}

float ease(double p)
{
    const float t = float(std::clamp(p, 0.0, 1.0));
    return t * t * (3.0f - 2.0f * t);
}

QPointF directionOffset(int dir, double amount, QSize size)
{
    switch (dir) {
    case 0: return {-amount * size.width(), 0};
    case 1: return {amount * size.width(), 0};
    case 2: return {0, -amount * size.height()};
    default: return {0, amount * size.height()};
    }
}

QSurfaceFormat glFormat()
{
    QSurfaceFormat f;
    f.setRenderableType(QSurfaceFormat::OpenGL);
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setDepthBufferSize(0);
    f.setStencilBufferSize(0);
    return f;
}

} // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
    GLuint stencil = 0;
    int w = 0;
    int h = 0;
    bool mipmapped = false;
    GLenum fmt = GL_RGBA8; // GL_RGBA32F for intermediate float passes
    QSize size() const { return {w, h}; }
    explicit operator bool() const { return fbo != 0; }
};

struct GpuCompositor::Impl : protected QOpenGLExtraFunctions {
    std::map<int, SequenceSnapshotCache> sequenceSnapshots;
    // RAII handle for pooled render targets.
    class Layer {
    public:
        Layer() = default;
        Layer(Impl* owner, Target t) : m_owner(owner), m_t(t) {}
        ~Layer() { reset(); }
        Layer(Layer&& o) noexcept : m_owner(o.m_owner), m_t(o.m_t) { o.m_t = {}; }
        Layer& operator=(Layer&& o) noexcept
        {
            if (this != &o) {
                reset();
                m_owner = o.m_owner;
                m_t = o.m_t;
                o.m_t = {};
            }
            return *this;
        }
        Layer(const Layer&) = delete;
        Layer& operator=(const Layer&) = delete;
        void reset()
        {
            if (m_owner && m_t)
                m_owner->releaseTarget(m_t);
            m_t = {};
        }
        Target& operator*() { return m_t; }
        Target* operator->() { return &m_t; }
        const Target* get() const { return m_t ? &m_t : nullptr; }
        explicit operator bool() const { return bool(m_t); }

    private:
        Impl* m_owner = nullptr;
        Target m_t;
    };

    struct Entry {
        int mediaId = 0;
        int revision = 0; // MediaItem::revision (relinked media gets new decoders)
        int streamIndex = -1;
        QString path; // the file decoded (the media's, or its proxy)
        std::unique_ptr<VideoDecoder> decoder;
        FrameConverter fallback; // unusual pixel formats are converted on the CPU
        bool failed = false;
        int lastClipId = 0;
        uint64_t lastUsed = 0;
        GLuint planes[4] = {0, 0, 0, 0};
        int planeW[4] = {0, 0, 0, 0};
        int planeH[4] = {0, 0, 0, 0};
        GLenum planeFmt[4] = {0, 0, 0, 0};
        Target rgba;
        TimeUs rgbaPts = -1;
        Target nextRgba;
        TimeUs nextRgbaPts = -1;
        // Optical flow between the two (worked out on the CPU from small copies, kept as a texture).
        GLuint flowTex = 0;
        QSize flowSize;
        TimeUs flowPts = -1;
        // Noise reduction: the frame before, the motion to each neighbour, and the cleaned-up frame.
        Target prevRgba;
        TimeUs prevRgbaPts = -1;
        GLuint prevFlowTex = 0, nextFlowTex = 0;
        QSize prevFlowSize, nextFlowSize;
        Target denoised;
        TimeUs denoisedPts = -1;
        double denoisedStrength = 0;
    };
    struct GeneratorCache {
        QString type;
        Params params;
        QJsonObject data;
        Target target;
        uint64_t lastUsed = 0;
        bool valid = false;
    };
    struct StateEntry {
        std::unique_ptr<EffectState> state;
        QString type; // the effect the state belongs to (an id reused by another kind starts afresh)
        uint64_t lastUsed = 0;
    };
    struct SequenceResources {
        std::vector<std::unique_ptr<Entry>> entries;
        std::map<int, GeneratorCache> generators;
        std::map<int, StateEntry> effects;
        std::map<int, SequenceSnapshotCache> snapshots;
        std::map<int, std::unique_ptr<SequenceResources>> children;
    };
    std::map<int, std::unique_ptr<SequenceResources>> sequenceResources;
    int sequenceDepth = 0;

    QOffscreenSurface* surface = nullptr;
    std::unique_ptr<QOpenGLContext> context;
    bool highQuality = false;
    bool useProxies = false;
    QString rendererName;
    GLuint vao = 0, vbo = 0, lutTex = 0, emptyTex = 0, uploadTex = 0;
    QSize uploadSize;
    std::unique_ptr<QOpenGLShaderProgram> progTex, progSolid, progYuv, progLut, progMatrix, progBlur, progSharpen,
        progBright, progScreen, progVignette, progKey, progNoise, progPixelate, progTransition, progTransitionFx, progRackBlend, progWarp,
        progChroma, progRadialPass, progCrt, progPsy, progFx, progDiffuse, progConsole, progVgMesh, progLut3D, progEncodeYuv, progMaxAlpha, progFlowMix, progMatte, progDenoise, progNode, progFlow, progGen;
    // .cube LUTs as 3D textures, by the loaded table (kept while it is in the file cache).
    std::map<const CubeLut*, std::pair<std::shared_ptr<const CubeLut>, GLuint>> lut3dTextures;
    GLuint meshVao = 0, meshVbo = 0;
    std::vector<Target> pool;
    std::vector<std::unique_ptr<Entry>> entries;
    std::map<int, GeneratorCache> generators;
    std::shared_ptr<const int> session;
    std::map<int, StateEntry> effectStates;
    uint64_t tick = 0;

    void swapCaches(SequenceResources& cache) {
        entries.swap(cache.entries);
        generators.swap(cache.generators);
        effectStates.swap(cache.effects);
        sequenceSnapshots.swap(cache.snapshots);
        sequenceResources.swap(cache.children);
    }
    void deleteSequenceResources(SequenceResources& cache) {
        cache.effects.clear();
        for (auto& e : cache.entries) deleteEntryGl(*e);
        for (auto& [id, g] : cache.generators) deleteTarget(g.target);
        for (auto& [id, child] : cache.children) deleteSequenceResources(*child);
    }

    ~Impl()
    {
        if (!context || !context->makeCurrent(surface))
            return;
        for (auto& [id, cache] : sequenceResources) deleteSequenceResources(*cache);
        effectStates.clear(); // GPU effect states own textures
        builderState.reset();
        for (auto& e : entries)
            deleteEntryGl(*e);
        for (auto& [id, g] : generators)
            deleteTarget(g.target);
        for (Target& t : pool)
            deleteTarget(t);
        GLuint textures[] = {lutTex, emptyTex, uploadTex};
        glDeleteTextures(3, textures);
        for (auto& [ptr, entry] : lut3dTextures)
            glDeleteTextures(1, &entry.second);
        lut3dTextures.clear();
        progLut3D.reset();
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        progTex.reset();
        progSolid.reset();
        progYuv.reset();
        progLut.reset();
        progMatrix.reset();
        progBlur.reset();
        progSharpen.reset();
        progBright.reset();
        progScreen.reset();
        progVignette.reset();
        progKey.reset();
        progNoise.reset();
        progPixelate.reset();
        progTransition.reset();
        progTransitionFx.reset();
        progRackBlend.reset();
        progMatte.reset();
        progDenoise.reset();
        progNode.reset();
        progFlow.reset();
        progGen.reset();
        progWarp.reset();
        progChroma.reset();
        progRadialPass.reset();
        progCrt.reset();
        progPsy.reset();
        progFx.reset();
        progDiffuse.reset();
        progConsole.reset();
        progVgMesh.reset();
        if (meshVbo)
            glDeleteBuffers(1, &meshVbo);
        if (meshVao)
            glDeleteVertexArrays(1, &meshVao);
        if (dataTex)
            glDeleteTextures(1, &dataTex);
        // Destroyed while current: Qt's OpenGL paint engine (one per thread, used for
        // titles) frees its vertex array only if this context is current when it goes.
        // Off the GUI thread it otherwise keeps a dangling handle, and the next
        // renderer on this thread crashes in QPainter::begin.
        context.reset();
    }

    // ---- Setup ----------------------------------------------------------------

    bool init(QString* error)
    {
        context = std::make_unique<QOpenGLContext>();
        context->setFormat(glFormat());
        if (!context->create() || !context->makeCurrent(surface)) {
            if (error)
                *error = QStringLiteral("could not create an OpenGL 3.3 context");
            return false;
        }
        const QSurfaceFormat f = context->format();
        if (f.majorVersion() < 3 || (f.majorVersion() == 3 && f.minorVersion() < 3)) {
            if (error)
                *error = QStringLiteral("OpenGL %1.%2 is too old (3.3 needed)").arg(f.majorVersion()).arg(f.minorVersion());
            return false;
        }
        initializeOpenGLFunctions();
        rendererName = QString::fromLatin1(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

        struct {
            std::unique_ptr<QOpenGLShaderProgram>* prog;
            const char* frag;
        } programs[] = {{&progTex, kFragTex},         {&progSolid, kFragSolid},     {&progYuv, kFragYuv},
                        {&progEncodeYuv, kFragEncodeYuv},
                        {&progLut, kFragLut},         {&progMatrix, kFragMatrix},   {&progBlur, kFragBlur},
                        {&progSharpen, kFragSharpen}, {&progBright, kFragBright},   {&progScreen, kFragScreen},
                        {&progVignette, kFragVignette}, {&progKey, kFragKey},      {&progNoise, kFragNoise},
                        {&progPixelate, kFragPixelate}, {&progTransition, kFragTransition}, {&progTransitionFx, tfx::kGlsl},
                        {&progRackBlend, kFragRackBlend}, {&progWarp, kFragWarp}, {&progChroma, kFragChroma},
                        {&progRadialPass, kFragRadialPass}, {&progCrt, kFragCrt}, {&progPsy, kFragPsy},
                        {&progFx, gpufx::kFragFx}, {&progLut3D, kFragLut3D}, {&progMaxAlpha, kFragMaxAlpha}, {&progFlowMix, kFragFlowMix},
                        {&progMatte, kFragMatte}, {&progDenoise, kFragDenoise}, {&progNode, kFragNode}, {&progFlow, kFragFlow}, {&progGen, kFragGen}};
        for (auto& p : programs) {
            auto prog = std::make_unique<QOpenGLShaderProgram>();
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex)
                || !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, QByteArray(kFragHeader) + p.frag) || !prog->link()) {
                if (error)
                    *error = QStringLiteral("shader compile failed: %1").arg(prog->log());
                return false;
            }
            *p.prog = std::move(prog);
        }

        {
            auto prog = std::make_unique<QOpenGLShaderProgram>();
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, gpufx::kVertDiffuse)
                || !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, gpufx::kFragDiffuse) || !prog->link()) {
                if (error)
                    *error = QStringLiteral("shader compile failed: %1").arg(prog->log());
                return false;
            }
            progDiffuse = std::move(prog);
        }
        {
            auto prog = std::make_unique<QOpenGLShaderProgram>();
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex)
                || !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, QByteArray(kFragHeader) + vg::shaderDefines() + gpuvg::kFragConsole)
                || !prog->link()) {
                if (error)
                    *error = QStringLiteral("shader compile failed: %1").arg(prog->log());
                return false;
            }
            progConsole = std::move(prog);
        }
        {
            auto prog = std::make_unique<QOpenGLShaderProgram>();
            if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, gpuvg::kVertMesh)
                || !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, gpuvg::kFragMesh) || !prog->link()) {
                if (error)
                    *error = QStringLiteral("shader compile failed: %1").arg(prog->log());
                return false;
            }
            progVgMesh = std::move(prog);
        }

        const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

        lutTex = makeTexture(256, 1, GL_RGBA8, GL_NEAREST);
        emptyTex = makeTexture(1, 1, GL_RGBA8, GL_NEAREST);
        const uint32_t zero = 0;
        glBindTexture(GL_TEXTURE_2D, emptyTex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, &zero);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        return glGetError() == GL_NO_ERROR;
    }

    GLuint makeTexture(int w, int h, GLenum internal, GLenum filter)
    {
        GLuint t = 0;
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GLint(filter));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GLint(filter == GL_NEAREST ? GL_NEAREST : GL_LINEAR));
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        const bool flt = internal == GL_RGBA32F || internal == GL_RG32F;
        const GLenum format = internal == GL_RG32F ? GL_RG : flt ? GL_RGBA
                                  : (internal == GL_R8 || internal == GL_R16 ? GL_RED : (internal == GL_RG8 || internal == GL_RG16 ? GL_RG : GL_BGRA));
        const GLenum type = flt ? GL_FLOAT : (internal == GL_R16 || internal == GL_RG16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE);
        glTexImage2D(GL_TEXTURE_2D, 0, GLint(internal), w, h, 0, format, type, nullptr);
        return t;
    }

    // ---- Render targets -----------------------------------------------------------

    Target createTarget(QSize s, GLenum fmt = GL_RGBA8)
    {
        Target t;
        t.w = std::max(1, s.width());
        t.h = std::max(1, s.height());
        t.fmt = fmt;
        t.tex = makeTexture(t.w, t.h, fmt, fmt == GL_RGBA32F || fmt == GL_RG32F ? GL_NEAREST : GL_LINEAR);
        glGenFramebuffers(1, &t.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.tex, 0);
        return t;
    }

    void deleteTarget(Target& t)
    {
        if (t.stencil)
            glDeleteRenderbuffers(1, &t.stencil);
        if (t.fbo)
            glDeleteFramebuffers(1, &t.fbo);
        if (t.tex)
            glDeleteTextures(1, &t.tex);
        t = {};
    }

    Layer acquire(QSize s, bool clearIt = true, GLenum fmt = GL_RGBA8)
    {
        Target t;
        s = s.expandedTo(QSize(1, 1));
        for (size_t i = 0; i < pool.size(); ++i)
            if (pool[i].w == s.width() && pool[i].h == s.height() && pool[i].fmt == fmt) {
                t = pool[i];
                pool.erase(pool.begin() + i);
                break;
            }
        if (!t)
            t = createTarget(s, fmt);
        if (clearIt)
            clear(t, 0, 0, 0, 0);
        return Layer(this, t);
    }

    void releaseTarget(Target t)
    {
        if (t.mipmapped) {
            glBindTexture(GL_TEXTURE_2D, t.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            t.mipmapped = false;
        }
        pool.push_back(t);
        while (pool.size() > kMaxPooledTargets) {
            deleteTarget(pool.front());
            pool.erase(pool.begin());
        }
    }

    void clear(const Target& t, float r, float g, float b, float a)
    {
        glDisable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, t.fbo);
        glViewport(0, 0, t.w, t.h);
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // ---- Drawing primitives ------------------------------------------------------

    void bindTexture(int unit, GLuint tex)
    {
        glActiveTexture(GL_TEXTURE0 + GLenum(unit));
        glBindTexture(GL_TEXTURE_2D, tex);
    }

    // Draws a quad of `quadSize` pixels mapped by `xform` into `dst`.
    void draw(QOpenGLShaderProgram& prog, const Target& dst, const QTransform& xform, QSizeF quadSize,
              QRectF uv = QRectF(0, 0, 1, 1), bool blend = true)
    {
        prog.setUniformValue("uXform", toMat3(xform));
        prog.setUniformValue("uQuadSize", QVector2D(float(quadSize.width()), float(quadSize.height())));
        prog.setUniformValue("uTargetSize", QVector2D(float(dst.w), float(dst.h)));
        prog.setUniformValue("uSrcRect", QVector4D(float(uv.x()), float(uv.y()), float(uv.width()), float(uv.height())));
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.w, dst.h);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glBlendEquation(GL_FUNC_ADD);
        if (blend) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied "over"
        } else {
            glDisable(GL_BLEND);
        }
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    void fullscreen(QOpenGLShaderProgram& prog, const Target& dst, bool blend = false)
    {
        draw(prog, dst, QTransform(), QSizeF(dst.w, dst.h), QRectF(0, 0, 1, 1), blend);
    }

    // Draws texture `tex` (texW x texH) through `xform` with opacity.
    void drawTexture(const Target& dst, GLuint tex, QSizeF texSize, const QTransform& xform, double opacity,
                     QRectF uv = QRectF(0, 0, 1, 1), bool blend = true)
    {
        if (opacity <= 0.002)
            return;
        progTex->bind();
        bindTexture(0, tex);
        progTex->setUniformValue("uTex", 0);
        progTex->setUniformValue("uOpacity", float(std::min(opacity, 1.0)));
        draw(*progTex, dst, xform, texSize, uv, blend);
    }

    void drawLayer(const Target& dst, const Target& src, double opacity)
    {
        drawTexture(dst, src.tex, QSizeF(src.w, src.h), QTransform(), opacity);
    }

    void solid(const Target& dst, const QTransform& xform, QSizeF size, QColor color, double opacity)
    {
        progSolid->bind();
        const float a = float(color.alphaF() * std::clamp(opacity, 0.0, 1.0));
        progSolid->setUniformValue("uColor", QVector4D(float(color.redF()) * a, float(color.greenF()) * a, float(color.blueF()) * a, a));
        draw(*progSolid, dst, xform, size);
    }

    void ensureMipmaps(Target& t)
    {
        if (t.mipmapped)
            return;
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        t.mipmapped = true;
    }

    QImage download(const Target& t)
    {
        QImage img(t.w, t.h, QImage::Format_ARGB32_Premultiplied);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, t.fbo);
        glReadPixels(0, 0, t.w, t.h, GL_BGRA, GL_UNSIGNED_BYTE, img.bits());
        return img;
    }

    void upload(Target& t, const QImage& img)
    {
        const QImage src = img.format() == QImage::Format_ARGB32_Premultiplied ? img : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (src.size() != t.size())
            return;
        glBindTexture(GL_TEXTURE_2D, t.tex);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, int(src.bytesPerLine() / 4));
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, t.w, t.h, GL_BGRA, GL_UNSIGNED_BYTE, src.constBits());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        t.mipmapped = false;
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    // ---- Decoded frames -> RGBA textures ----------------------------------------------

    void deleteEntryGl(Entry& e)
    {
        for (GLuint& p : e.planes)
            if (p) {
                glDeleteTextures(1, &p);
                p = 0;
            }
        deleteTarget(e.rgba);
        deleteTarget(e.nextRgba);
        deleteTarget(e.prevRgba);
        deleteTarget(e.denoised);
        for (GLuint* tex : {&e.prevFlowTex, &e.nextFlowTex})
            if (*tex) {
                glDeleteTextures(1, tex);
                *tex = 0;
            }
        if (e.flowTex) {
            glDeleteTextures(1, &e.flowTex);
            e.flowTex = 0;
        }
    }

    void uploadPlane(Entry& e, int i, const uint8_t* data, int linesize, int w, int h, GLenum internal)
    {
        const GLenum format = internal == GL_R8 || internal == GL_R16 ? GL_RED : GL_RG;
        const GLenum type = internal == GL_R16 || internal == GL_RG16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE;
        const int bytesPerPixel = (format == GL_RG ? 2 : 1) * (type == GL_UNSIGNED_SHORT ? 2 : 1);
        if (!e.planes[i] || e.planeW[i] != w || e.planeH[i] != h || e.planeFmt[i] != internal) {
            if (e.planes[i])
                glDeleteTextures(1, &e.planes[i]);
            e.planes[i] = makeTexture(w, h, internal, GL_LINEAR);
            e.planeW[i] = w;
            e.planeH[i] = h;
            e.planeFmt[i] = internal;
        }
        glBindTexture(GL_TEXTURE_2D, e.planes[i]);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize / bytesPerPixel);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, format, type, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }

    // Converts a decoded frame into an RGBA target at its coded size.
    // A small copy of a picture, read back (what the motion is worked out from: flow::analysisSize).
    QImage smallCopy(const Target& from)
    {
        const QSize small = flow::analysisSize(from.size());
        Layer l = acquire(small, false);
        drawTexture(*l, from.tex, QSizeF(from.w, from.h), QTransform::fromScale(double(small.width()) / from.w, double(small.height()) / from.h),
                    1.0, QRectF(0, 0, 1, 1), false);
        return download(*l);
    }

    // The motion as an RG texture (made, or remade at a new size, as needed); false when there is none.
    bool uploadFlow(const flow::Field& f, GLuint& tex, QSize& size)
    {
        if (f.empty())
            return false;
        std::vector<float> rg(size_t(f.w) * size_t(f.h) * 2);
        for (size_t i = 0; i < f.dx.size(); ++i) {
            rg[i * 2] = f.dx[i];
            rg[i * 2 + 1] = f.dy[i];
        }
        if (!tex || size != QSize(f.w, f.h)) {
            if (tex)
                glDeleteTextures(1, &tex);
            tex = makeTexture(f.w, f.h, GL_RG32F, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            size = QSize(f.w, f.h);
        }
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f.w, f.h, GL_RG, GL_FLOAT, rg.data());
        return true;
    }

    // The motion from picture a to b (as flow::estimate, on the GPU), into the RG texture `tex` (made, or remade at a
    // new size, as needed). MVP_CPU_FLOW=1: worked out on the CPU from small copies read back, as before.
    bool motion(const Target& a, const Target& b, GLuint& tex, QSize& size)
    {
        if (a.size() != b.size() || !a)
            return false;
        if (qEnvironmentVariableIsSet("MVP_CPU_FLOW"))
            return uploadFlow(flow::estimate(smallCopy(a), smallCopy(b)), tex, size);
        constexpr int kLevels = 3, kBlock = 8;
        const QSize small = flow::analysisSize(a.size());
        auto pass = [&](const Target& dst, int mode, const Target* t0, const Target* t1, const Target* t2) {
            progFlow->bind();
            bindTexture(0, t0 ? t0->tex : emptyTex);
            bindTexture(1, t1 ? t1->tex : emptyTex);
            bindTexture(2, t2 ? t2->tex : emptyTex);
            progFlow->setUniformValue("uA", 0);
            progFlow->setUniformValue("uB", 1);
            progFlow->setUniformValue("uPrev", 2);
            progFlow->setUniformValue("uMode", mode);
            glUniform2i(progFlow->uniformLocation("uOut"), dst.w, dst.h);
            fullscreen(*progFlow, dst);
        };
        // Grey copies at the analysis size, and their pyramids.
        std::vector<Layer> pa, pb;
        for (const Target* from : {&a, &b}) {
            Layer copy = acquire(small, false);
            drawTexture(*copy, from->tex, QSizeF(from->w, from->h),
                        QTransform::fromScale(double(small.width()) / from->w, double(small.height()) / from->h), 1.0, QRectF(0, 0, 1, 1), false);
            std::vector<Layer>& py = from == &a ? pa : pb;
            py.push_back(acquire(small, false, GL_RGBA32F));
            pass(*py.back(), 0, &*copy, nullptr, nullptr);
            for (int l = 1; l < kLevels; ++l) {
                const Target& up = *py.back();
                Layer down = acquire(QSize(std::max(1, up.w / 2), std::max(1, up.h / 2)), false, GL_RGBA32F);
                pass(*down, 1, &up, nullptr, nullptr);
                py.push_back(std::move(down));
            }
        }
        // Coarse to fine: the smallest level searches widest, each finer one refines the guess doubled up from the last.
        Layer vectors;
        for (int l = kLevels - 1; l >= 0; --l) {
            const QSize cells(std::max(1, (pa[size_t(l)]->w + kBlock - 1) / kBlock), std::max(1, (pa[size_t(l)]->h + kBlock - 1) / kBlock));
            Layer matched = acquire(cells, false, GL_RGBA32F);
            progFlow->bind();
            setI(*progFlow, "uI", l == kLevels - 1 ? 6 : 2, vectors ? 1 : 0, vectors ? vectors->w : 0, vectors ? vectors->h : 0);
            pass(*matched, 2, &*pa[size_t(l)], &*pb[size_t(l)], vectors ? &*vectors : nullptr);
            Layer smoothed = acquire(cells, false, GL_RGBA32F);
            pass(*smoothed, 3, &*matched, nullptr, nullptr);
            vectors = std::move(smoothed);
        }
        // As fractions of the picture, into the texture the shaders read.
        if (!tex || size != vectors->size()) {
            if (tex)
                glDeleteTextures(1, &tex);
            tex = makeTexture(vectors->w, vectors->h, GL_RG32F, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            size = vectors->size();
        }
        Target out;
        out.tex = tex;
        out.w = vectors->w;
        out.h = vectors->h;
        out.fmt = GL_RG32F;
        glGenFramebuffers(1, &out.fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, out.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        progFlow->bind();
        progFlow->setUniformValue("uScale", QVector2D(1.0f / float(small.width()), 1.0f / float(small.height())));
        pass(out, 4, &*vectors, nullptr, nullptr);
        glDeleteFramebuffers(1, &out.fbo);
        return true;
    }

    // The decoded frame with its noise reduced (flow::denoise), kept until the frame or the strength changes.
    Target* denoised(Entry& e, double strength)
    {
        const TimeUs p0 = e.decoder->currentPts();
        if (e.denoised && e.denoisedPts == p0 && e.denoisedStrength == strength && e.denoised.size() == e.rgba.size())
            return &e.denoised;
        TimeUs pp = -1;
        const AVFrame* prev = e.decoder->previousFrame(&pp);
        const AVFrame* next = e.decoder->nextFrame();
        const bool hasPrev = prev && convertFrame(e, prev, e.prevRgba, e.prevRgbaPts, pp) && e.prevRgba.size() == e.rgba.size();
        const bool hasNext = next && convertFrame(e, next, e.nextRgba, e.nextRgbaPts, e.decoder->nextPts()) && e.nextRgba.size() == e.rgba.size();
        const bool prevFlow = hasPrev && motion(e.rgba, e.prevRgba, e.prevFlowTex, e.prevFlowSize);
        const bool nextFlow = hasNext && motion(e.rgba, e.nextRgba, e.nextFlowTex, e.nextFlowSize);
        if (!e.denoised || e.denoised.size() != e.rgba.size()) {
            deleteTarget(e.denoised);
            e.denoised = createTarget(e.rgba.size());
        }
        progDenoise->bind();
        bindTexture(0, e.rgba.tex);
        bindTexture(1, prevFlow ? e.prevRgba.tex : e.rgba.tex);
        bindTexture(2, nextFlow ? e.nextRgba.tex : e.rgba.tex);
        bindTexture(3, prevFlow ? e.prevFlowTex : e.rgba.tex);
        bindTexture(4, nextFlow ? e.nextFlowTex : e.rgba.tex);
        progDenoise->setUniformValue("uCur", 0);
        progDenoise->setUniformValue("uPrev", 1);
        progDenoise->setUniformValue("uNext", 2);
        progDenoise->setUniformValue("uFlowPrev", 3);
        progDenoise->setUniformValue("uFlowNext", 4);
        progDenoise->setUniformValue("uHasPrev", prevFlow ? 1 : 0);
        progDenoise->setUniformValue("uHasNext", nextFlow ? 1 : 0);
        progDenoise->setUniformValue("uSim", flow::denoiseSimilarity(strength));
        fullscreen(*progDenoise, e.denoised);
        e.denoisedPts = p0;
        e.denoisedStrength = strength;
        return &e.denoised;
    }

    bool convertFrame(Entry& e, const AVFrame* f, Target& out, TimeUs& outPts, TimeUs pts)
    {
        if (out && outPts == pts && out.w == f->width && out.h == f->height)
            return true;
        if (!out || out.w != f->width || out.h != f->height) {
            deleteTarget(out);
            out = createTarget(QSize(f->width, f->height));
        }
        if (out.mipmapped) {
            glBindTexture(GL_TEXTURE_2D, out.tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            out.mipmapped = false;
        }
        outPts = pts;

        const auto fmt = AVPixelFormat(f->format);
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(fmt);
        const int planes = av_pix_fmt_count_planes(fmt);
        bool yuv = desc && desc->nb_components >= 3
                   && !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_BE
                                       | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BITSTREAM))
                   && desc->comp[0].depth <= 16 && (planes == 2 || planes == 3 || planes == 4);
        const int depth = desc ? desc->comp[0].depth : 8;
        const int bytes = depth > 8 ? 2 : 1;
        bool semi = false, swapUV = false;
        if (yuv) {
            if (planes == 2) {
                semi = desc->comp[1].plane == 1 && desc->comp[2].plane == 1 && desc->comp[1].step == 2 * bytes;
                swapUV = desc->comp[1].offset > desc->comp[2].offset;
                yuv = semi && desc->comp[0].step == bytes;
            } else {
                yuv = desc->comp[0].step == bytes && desc->comp[1].plane == 1 && desc->comp[2].plane == 2
                      && desc->comp[1].step == bytes;
            }
        }

        if (!yuv) {
            // RGB, paletted, packed or exotic formats: convert on the CPU.
            const QImage img = e.fallback.convert(f, QSize(f->width, f->height), highQuality);
            if (img.isNull())
                return false;
            upload(out, img);
            return true;
        }

        const int cw = AV_CEIL_RSHIFT(f->width, desc->log2_chroma_w);
        const int ch = AV_CEIL_RSHIFT(f->height, desc->log2_chroma_h);
        const GLenum one = bytes == 2 ? GL_R16 : GL_R8;
        const GLenum two = bytes == 2 ? GL_RG16 : GL_RG8;
        uploadPlane(e, 0, f->data[0], f->linesize[0], f->width, f->height, one);
        if (semi) {
            uploadPlane(e, 1, f->data[1], f->linesize[1], cw, ch, two);
        } else {
            uploadPlane(e, 1, f->data[1], f->linesize[1], cw, ch, one);
            uploadPlane(e, 2, f->data[2], f->linesize[2], cw, ch, one);
        }
        const bool hasAlpha = planes == 4 && desc->nb_components == 4;
        if (hasAlpha)
            uploadPlane(e, 3, f->data[3], f->linesize[3], f->width, f->height, one);

        // Colour matrix and range, matching the CPU converter's choices.
        int cs = f->colorspace;
        if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RGB || cs == AVCOL_SPC_RESERVED)
            cs = f->height >= 720 ? AVCOL_SPC_BT709 : AVCOL_SPC_BT470BG;
        double kr = 0.299, kb = 0.114;
        if (cs == AVCOL_SPC_BT709)
            kr = 0.2126, kb = 0.0722;
        else if (cs == AVCOL_SPC_BT2020_NCL || cs == AVCOL_SPC_BT2020_CL)
            kr = 0.2627, kb = 0.0593;
        else if (cs == AVCOL_SPC_SMPTE240M)
            kr = 0.212, kb = 0.087;
        else if (cs == AVCOL_SPC_FCC)
            kr = 0.30, kb = 0.11;
        const double kg = 1.0 - kr - kb;
        const bool full = f->color_range == AVCOL_RANGE_JPEG || fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P
                          || fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P || fmt == AV_PIX_FMT_YUVJ411P;
        const double maxv = double((1 << depth) - 1);
        const int up = depth - 8;
        const float yOff = full ? 0.0f : float((16 << up) / maxv);
        const float cOff = float((full ? (1 << (depth - 1)) : (128 << up)) / maxv);
        const float yScale = full ? 1.0f : float(maxv / (219 << up));
        const float cScale = full ? 1.0f : float(maxv / (224 << up));
        const double storedMax = bytes == 2 ? 65535.0 : 255.0;
        const float norm = float(storedMax / (double(1 << desc->comp[0].shift) * maxv));
        const float m[9] = {1.0f, 0.0f, float(2 * (1 - kr)),
                            1.0f, float(-2 * kb * (1 - kb) / kg), float(-2 * kr * (1 - kr) / kg),
                            1.0f, float(2 * (1 - kb)), 0.0f};

        progYuv->bind();
        bindTexture(0, e.planes[0]);
        bindTexture(1, e.planes[1]);
        bindTexture(2, semi ? e.planes[1] : e.planes[2]);
        bindTexture(3, hasAlpha ? e.planes[3] : e.planes[0]);
        progYuv->setUniformValue("uY", 0);
        progYuv->setUniformValue("uU", 1);
        progYuv->setUniformValue("uV", 2);
        progYuv->setUniformValue("uA", 3);
        progYuv->setUniformValue("uLayout", semi ? 1 : 0);
        progYuv->setUniformValue("uSwapUV", GLint(swapUV));
        progYuv->setUniformValue("uHasAlpha", GLint(hasAlpha));
        progYuv->setUniformValue("uNorm", norm);
        progYuv->setUniformValue("uOffset", QVector3D(yOff, cOff, cOff));
        progYuv->setUniformValue("uScale", QVector3D(yScale, cScale, cScale));
        progYuv->setUniformValue("uMatrix", rowsToMat3(m));
        fullscreen(*progYuv, out);
        return true;
    }

    Entry* acquireDecoder(const MediaItem& media, int streamIndex, int clipId, TimeUs sourceTime)
    {
        const QString path = useProxies ? proxy::playbackPath(media.path, streamIndex) : media.path;
        Entry* best = nullptr;
        TimeUs bestCost = 0;
        for (auto& e : entries) {
            if (e->mediaId != media.id || e->revision != media.revision || e->streamIndex != streamIndex || e->lastUsed == tick
                || e->path != path)
                continue;
            if (e->failed)
                return nullptr;
            if (e->lastClipId == clipId) {
                best = e.get();
                break;
            }
            const TimeUs pos = e->decoder->lastRequest();
            const TimeUs cost = sourceTime >= pos ? sourceTime - pos : (pos - sourceTime) * 4;
            if (!best || cost < bestCost) {
                best = e.get();
                bestCost = cost;
            }
        }
        if (!best) {
            auto e = std::make_unique<Entry>();
            e->mediaId = media.id;
            e->revision = media.revision;
            e->streamIndex = streamIndex;
            e->path = path;
            e->decoder = std::make_unique<VideoDecoder>();
            // A proxy holds the video alone, as stream 0.
            e->failed = !e->decoder->open(path, path == media.path ? streamIndex : 0);
            best = e.get();
            entries.push_back(std::move(e));
            if (best->failed)
                return nullptr;
        }
        best->lastClipId = clipId;
        best->lastUsed = tick;
        return best;
    }

    // ---- Events ------------------------------------------------------------------------

    QRectF drawClip(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, const Target& dst, double opacity)
    {
        if (opacity <= 0.002)
            return {};
        if (clip.isSequence()) {
            if (sequenceDepth >= 16) return {};
            const auto& child=sequenceSnapshots[clip.id].get(snap,clip.sequence);
            auto& resources = sequenceResources[clip.id];
            if (!resources) resources = std::make_unique<SequenceResources>();
            Layer layer;
            {
                auto* cache = resources.get();
                swapCaches(*cache);
                ++sequenceDepth;
                struct Restore {
                    Impl* owner; SequenceResources* cache;
                    ~Restore() { owner->swapCaches(*cache); --owner->sequenceDepth; }
                } restore{this, cache};
                layer = compose(child, clip.sourceTimeAt(t), QSize(dst.w, dst.h), true);
                evict();
            }
            const QTransform xf = panCropTransform(resolveParams(panCropParams(), clip.panCrop, t - clip.start), QSizeF(dst.w,dst.h), QSizeF(dst.w,dst.h));
            drawTexture(dst, layer->tex, QSizeF(dst.w,dst.h), xf, opacity);
            return QRectF(0,0,dst.w,dst.h);
        }
        if (clip.isGenerated())
            return drawGenerated(snap, clip, t, dst, opacity);
        auto mit = snap.media.find(clip.mediaId);
        if (mit == snap.media.end())
            return {};
        const MediaItem& media = *mit->second;
        const VideoStreamInfo* vs = media.info.videoStream(clip.streamIndex);
        if (!vs)
            return {};
        const TimeUs srcT = media.info.isStill ? 0 : clip.sourceTimeAt(t);
        Entry* e = acquireDecoder(media, clip.streamIndex, clip.id, srcT);
        if (!e)
            return {};
        const bool denoise = clip.denoise > 0 && !media.info.isStill;
        if (denoise)
            e->decoder->setKeepPrevious(true);
        const AVFrame* frame = e->decoder->frameAt(srcT);
        if (!frame || !convertFrame(*e, frame, e->rgba, e->rgbaPts, e->decoder->currentPts()))
            return {};

        // Same geometry as the CPU compositor: texture -> upright picture ->
        // display (rotation metadata) -> canvas (pan/crop).
        const QSizeF disp(vs->displaySize());
        const Params pc = resolveParams(panCropParams(), clip.panCrop, t - clip.start);
        QTransform view = panCropTransform(pc, disp, QSizeF(dst.w, dst.h));
        if (clip.stabilize > 0 && !media.info.isStill) // as the CPU compositor
            view = stabilize::correction(media.path, *vs, srcT, clip.stabilize, clip.stabilizeZoom, clip.sourceIn, clip.sourceIn + clip.sourceSpan()) * view;
        const bool sideways = vs->rotation == 90 || vs->rotation == 270;
        const QSizeF upright = sideways ? QSizeF(disp.height(), disp.width()) : disp;
        QTransform m;
        m.scale(upright.width() / e->rgba.w, upright.height() / e->rgba.h);
        QTransform r;
        r.translate(disp.width() / 2, disp.height() / 2);
        r.rotate(vs->rotation);
        r.translate(-upright.width() / 2, -upright.height() / 2);
        const QTransform full = m * r * view;

        Target* picture = denoise ? denoised(*e, clip.denoise) : &e->rgba;
        Layer blended;
        if (clip.opticalFlow && !media.info.isStill)
            if (const AVFrame* next = e->decoder->nextFrame()) {
                const TimeUs p0 = e->decoder->currentPts(), p1 = e->decoder->nextPts();
                const double w = p1 > p0 ? double(srcT - p0) / double(p1 - p0) : 0.0;
                if (w > 0.02 && w < 0.98 && convertFrame(*e, next, e->nextRgba, e->nextRgbaPts, p1)
                    && e->nextRgba.size() == e->rgba.size()) {
                    if (e->flowPts != p0 || !e->flowTex) {
                        // The motion between the two (the same for every frame in between).
                        if (motion(e->rgba, e->nextRgba, e->flowTex, e->flowSize))
                            e->flowPts = p0;
                    }
                    if (e->flowTex && e->flowPts == p0) {
                        blended = acquire(e->rgba.size(), false);
                        progFlowMix->bind();
                        bindTexture(0, e->rgba.tex);
                        bindTexture(1, e->nextRgba.tex);
                        bindTexture(2, e->flowTex);
                        progFlowMix->setUniformValue("uFrom", 0);
                        progFlowMix->setUniformValue("uTo", 1);
                        progFlowMix->setUniformValue("uFlow", 2);
                        progFlowMix->setUniformValue("uW", float(w));
                        fullscreen(*progFlowMix, *blended);
                        picture = &*blended;
                    }
                }
            }
        if (clip.frameBlend && !clip.opticalFlow && !media.info.isStill)
            if (const AVFrame* next = e->decoder->nextFrame()) {
                const TimeUs p0 = e->decoder->currentPts(), p1 = e->decoder->nextPts();
                const double w = p1 > p0 ? double(srcT - p0) / double(p1 - p0) : 0.0;
                if (w > 0.02 && w < 0.98 && convertFrame(*e, next, e->nextRgba, e->nextRgbaPts, p1)
                    && e->nextRgba.size() == e->rgba.size()) {
                    blended = acquire(e->rgba.size(), false);
                    transitionShader(*blended, &e->rgba, &e->nextRgba, 0, w, {}, {}, {});
                    picture = &*blended;
                }
            }
        // Minifying a lot: mipmaps avoid shimmering/aliasing.
        const double scale = std::sqrt(std::abs(full.determinant()));
        if (scale < 0.7)
            ensureMipmaps(*picture);
        drawTexture(dst, picture->tex, QSizeF(picture->w, picture->h), full, opacity);
        return full.mapRect(QRectF(0, 0, picture->w, picture->h)).intersected(QRectF(0, 0, dst.w, dst.h));
    }

    // Paints a generator with its GPU paint path into `target` (`size` logical pixels, painted `paintScale` times as
    // big). False: it couldn't (then it draws on the CPU).
    bool paintGenerator(const GeneratorDef& def, Target& target, QSize size, int paintScale, const Params& params, const GeneratorContext& ctx)
    {
        bool painted = false;
        if (def.gpuShader) { // worked out at a quarter of the size, as its render() does, then smoothed up
            const QSize small(std::max(8, (size.width() + 3) / 4), std::max(8, (size.height() + 3) / 4));
            Layer sky = acquire(small, false);
            progGen->bind();
            const double T = double(ctx.time) / double(kUsPerSecond) * params.num(QStringLiteral("speed"));
            const double aspect = double(size.width()) / std::max(1, size.height());
            QVector4D colours[6];
            if (def.id == QLatin1String("plasma")) {
                const std::vector<QColor> stops = plasmaPalette(params.choice(QStringLiteral("palette")));
                for (size_t i = 0; i < stops.size() && i < 6; ++i)
                    colours[i] = QVector4D(float(stops[i].redF()), float(stops[i].greenF()), float(stops[i].blueF()), 1);
                progGen->setUniformValue("uMode", 0);
                progGen->setUniformValue("uP", QVector4D(float(T), float(std::max(0.05, params.num(QStringLiteral("scale")))), float(aspect),
                                                         float(std::min<size_t>(stops.size(), 6))));
            } else {
                int i = 0;
                for (const char* id : {"color1", "color2", "sky", "horizon"}) {
                    const QColor c = params.color(QLatin1String(id));
                    colours[i++] = QVector4D(float(c.redF()), float(c.greenF()), float(c.blueF()), 1);
                }
                progGen->setUniformValue("uMode", 1);
                progGen->setUniformValue("uP", QVector4D(float(T), float(aspect), 0, 0));
            }
            progGen->setUniformValueArray("uC", colours, 6);
            progGen->setUniformValue("uSize", QVector2D(float(small.width()), float(small.height())));
            fullscreen(*progGen, *sky);
            drawTexture(target, sky->tex, QSizeF(target.size()), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
            if (!def.paintOver)
                return true;
        }
        ensureStencil(target);
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            if (!def.gpuShader)
                clear(target, 0, 0, 0, 0);
            glStencilMask(0xffffffffu);
            glClearStencil(0);
            glClear(GL_STENCIL_BUFFER_BIT);
            // (Mutant Video Producer's texture origin is at GL row zero, the logical top. The device binds its
            // framebuffer again after a scene paints pictures of its own, which stay on the GPU meanwhile.)
            motion::FramebufferPaintDevice device(target.size(), target.fbo);
            ScenePictures pictures(*this);
            motion::setGpuPictures(&pictures);
            QPainter painter(&device);
            painter.scale(paintScale,paintScale);
            if (painter.isActive())
                painted = def.gpuShader ? def.paintOver(painter, size, params, ctx) : def.paint(painter, size, params, ctx);
            painter.end();
            motion::setGpuPictures(nullptr);
        }
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_DEPTH_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glBlendEquation(GL_FUNC_ADD);
        return painted;
    }

    QRectF drawGenerated(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, const Target& dst, double opacity)
    {
        const GeneratorDef* def = findGenerator(clip.generator.type);
        if (!def)
            return {};
        GeneratorContext ctx;
        ctx.time = clip.generatedTime(t);
        ctx.length = clip.generatedLength();
        ctx.timelineTime = t;
        ctx.scale = double(dst.w) / std::max(1, snap.settings.width);
        ctx.fps = snap.settings.fps;
        ctx.highQuality = highQuality;
        ctx.data = &clip.generator.data;
        const Params params = resolveParams(def->params, clip.generator.params, ctx.time);
        const bool animating = def->isAnimating && def->isAnimating(params, ctx);
        const int paintScale = def->paint && !qEnvironmentVariableIsSet("MVP_CPU_GENERATORS") ? std::clamp(def->paintScale,1,2) : 1;
        const QSize paintSize(dst.w*paintScale,dst.h*paintScale);
        GeneratorCache& cache = generators[clip.id];
        cache.lastUsed = tick;
        if (!cache.target || cache.target.size() != paintSize) {
            deleteTarget(cache.target);
            cache.target = createTarget(paintSize);
            cache.valid = false;
        }
        if (animating || !cache.valid || cache.type != def->id || !(cache.params == params) || cache.data != clip.generator.data) {
            const bool painted = def->paint && !qEnvironmentVariableIsSet("MVP_CPU_GENERATORS")
                                 && paintGenerator(*def, cache.target, QSize(dst.w, dst.h), paintScale, params, ctx);
            if (!painted) {
                QImage img(paintSize, QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                GeneratorContext fallbackContext=ctx; fallbackContext.scale*=paintScale;
                def->render(img, params, fallbackContext);
                upload(cache.target, img);
            }
            cache.type = def->id;
            cache.params = params;
            cache.data = clip.generator.data;
            cache.valid = !animating;
        }
        QTransform xf;
        if (!isDefaultPanCrop(clip.panCrop))
            xf = panCropTransform(resolveParams(panCropParams(), clip.panCrop, ctx.time), QSizeF(dst.w, dst.h), QSizeF(dst.w, dst.h));
        drawTexture(dst, cache.target.tex, QSizeF(dst.w, dst.h), xf, opacity);
        return QRectF(0, 0, dst.w, dst.h);
    }

    Layer clipLayer(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QSize size, const RenderContext& base)
    {
        Layer layer = acquire(size);
        const QRectF rect = drawClip(snap, clip, t, *layer, 1.0);
        if (rect.isEmpty())
            return {};
        if (hasActiveEffects(clip.effects)) {
            RenderContext ctx = base;
            ctx.time = t - clip.start;
            ctx.contentRect = rect;
            applyChain(clip.effects, layer, ctx);
        }
        if (clip.gain < 0.999f) {
            Layer faded = acquire(size);
            drawLayer(*faded, *layer, std::clamp(double(clip.gain), 0.0, 1.0));
            layer = std::move(faded);
        }
        return layer;
    }

    // ---- Effects -----------------------------------------------------------------------

    void applyChain(const EffectChain& chain, Layer& layer, const RenderContext& ctx)
    {
        for (const EffectInstance& e : chain) {
            if (!e.enabled)
                continue;
            // A mask limits the effect (or rack) to part of the picture (as Compositor::applyChain).
            const EffectMask mask = effectMask(e.params, ctx.time, layer->size());
            Layer unmasked;
            if (mask.active()) {
                unmasked = acquire(layer->size(), false);
                drawLayerReplace(*unmasked, *layer);
            }
            auto finishMask = [&] {
                if (!mask.active() || layer->size() != unmasked->size())
                    return;
                Layer out = acquire(layer->size(), false);
                Layer coverage; // freehand: drawn on the CPU (EffectMask.cpp), read by the shader
                if (mask.shape == 3) {
                    coverage = acquire(layer->size(), false);
                    upload(*coverage, mask.alpha);
                }
                fxPass(*out, gpufx::EffectMask, *layer, [&](QOpenGLShaderProgram& g) {
                    g.setUniformValue("uA", QVector4D(mask.cx, mask.cy, mask.hw, mask.hh));
                    g.setUniformValue("uB", QVector4D(mask.cs, mask.sn, mask.feather, float(mask.shape)));
                    g.setUniformValue("uC", QVector4D(mask.invert ? 1.0f : 0.0f, 0, 0, 0));
                }, &*unmasked, coverage ? &*coverage : nullptr);
                layer = std::move(out);
            };
            if (e.isRack()) {
                applyRack(e, layer, ctx);
                finishMask();
                continue;
            }
            const EffectDef* def = findEffect(e.type);
            if (!def || !def->apply)
                continue;
            StateEntry& st = effectStates[e.id];
            st.lastUsed = tick;
            if (st.type != e.type) {
                st.state.reset();
                st.type = e.type;
            }
            const Params params = resolveParams(def->params, e.params, ctx.time);
            const fx::PostMask post = def->glitchPost ? fx::postMask(params, layer->size()) : fx::PostMask{};
            Layer before;
            if (post.active()) {
                before = acquire(layer->size(), false);
                drawLayerReplace(*before, *layer);
            }
            applyEffect(*def, params, layer, ctx, st.state);
            if (post.active() && layer->size() == before->size()) {
                Layer out = acquire(layer->size(), false);
                fxPass(*out, gpufx::GlitchPost, *layer, [&](QOpenGLShaderProgram& g) {
                    g.setUniformValue("uA", QVector4D(post.mix, post.threshold, post.feather, float(post.mode)));
                    g.setUniformValue("uB", QVector4D(post.x0, post.y0, post.x1, post.y1));
                }, &*before);
                layer = std::move(out);
            }
            finishMask();
        }
    }

    // Same as Compositor::applyRack: chains layered over the dry picture.
    void applyRack(const EffectInstance& rack, Layer& layer, const RenderContext& ctx)
    {
        const auto chains = activeRackChains(rack);
        if (chains.empty())
            return;
        const QSize size = layer->size();
        Layer acc = acquire(size, false);
        drawLayerReplace(*acc, *layer);
        for (const RackChain* c : chains) {
            Layer branch = acquire(size, false);
            drawLayerReplace(*branch, *layer);
            applyChain(c->effects, branch, ctx);
            if (branch->size() != size)
                continue;
            const Params cp = resolveParams(rackChainParams(false), c->params, ctx.time);
            Layer out = acquire(size, false);
            progRackBlend->bind();
            bindTexture(0, acc->tex);
            bindTexture(1, branch->tex);
            progRackBlend->setUniformValue("uBase", 0);
            progRackBlend->setUniformValue("uLayer", 1);
            progRackBlend->setUniformValue("uOpacity", float(std::clamp(cp.num(QStringLiteral("level")) / 100.0, 0.0, 1.0)));
            progRackBlend->setUniformValue("uMode", cp.choice(QStringLiteral("blend")));
            fullscreen(*progRackBlend, *out);
            acc = std::move(out);
        }
        const double mix = resolveParams(rackParams(), rack.params, ctx.time).num(QStringLiteral("mix")) / 100.0;
        if (mix >= 0.999) {
            layer = std::move(acc);
            return;
        }
        Layer out = acquire(size, false);
        transitionShader(*out, &*layer, &*acc, 0, std::max(0.0, mix), {}, {}, {});
        layer = std::move(out);
    }

    // Runs a fullscreen shader from `layer` into a fresh layer and swaps.
    void filterPass(Layer& layer, QOpenGLShaderProgram& prog, const std::function<void()>& uniforms)
    {
        Layer out = acquire(layer->size(), false);
        prog.bind();
        bindTexture(0, layer->tex);
        prog.setUniformValue("uTex", 0);
        if (uniforms)
            uniforms();
        fullscreen(prog, *out);
        layer = std::move(out);
    }

    void blur(Layer& layer, double sigmaX, double sigmaY)
    {
        if (sigmaX < 0.3 && sigmaY < 0.3)
            return;
        // Large radii: blur a downscaled copy (each halving is a 2x2 box).
        int level = 0;
        while (std::max(sigmaX, sigmaY) / double(1 << level) > 4.0 && level < 6)
            ++level;
        Layer work;
        const Target* src = &*layer;
        for (int i = 0; i < level; ++i) {
            Layer half = acquire(QSize((src->w + 1) / 2, (src->h + 1) / 2), false);
            drawTexture(*half, src->tex, QSizeF(half->w, half->h), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
            work = std::move(half);
            src = &*work;
        }
        const double sx = sigmaX / double(1 << level), sy = sigmaY / double(1 << level);
        auto pass = [&](double sigma, bool horizontal) {
            Layer out = acquire(src->size(), false);
            progBlur->bind();
            bindTexture(0, src->tex);
            progBlur->setUniformValue("uTex", 0);
            progBlur->setUniformValue("uStep", horizontal ? QVector2D(1.0f / src->w, 0) : QVector2D(0, 1.0f / src->h));
            progBlur->setUniformValue("uSigma", float(sigma));
            progBlur->setUniformValue("uRadius", int(std::ceil(sigma * 3.0)));
            fullscreen(*progBlur, *out);
            work = std::move(out);
            src = &*work;
        };
        if (sx >= 0.3)
            pass(sx, true);
        if (sy >= 0.3)
            pass(sy, false);
        if (level > 0) {
            Layer up = acquire(layer->size(), false);
            drawTexture(*up, src->tex, QSizeF(up->w, up->h), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
            layer = std::move(up);
        } else if (work) {
            layer = std::move(work);
        }
    }

    // Qt's OpenGL paint engine uses stencil for path clipping: a target QPainter paints into needs one.
    void ensureStencil(Target& target)
    {
        if (target.stencil)
            return;
        glBindFramebuffer(GL_FRAMEBUFFER, target.fbo);
        glGenRenderbuffers(1, &target.stencil);
        glBindRenderbuffer(GL_RENDERBUFFER, target.stencil);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, target.w, target.h);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, target.stencil);
    }

    // The part of a target with anything in it (as motion's drawnBounds on the CPU), from each block's own extent.
    QRect drawnBounds(const Target& t, int block)
    {
        Layer small = acquire(QSize((t.w + block - 1) / block, (t.h + block - 1) / block), false);
        progMaxAlpha->bind();
        bindTexture(0, t.tex);
        progMaxAlpha->setUniformValue("uTex", 0);
        progMaxAlpha->setUniformValue("uBlock", block);
        fullscreen(*progMaxAlpha, *small);
        const QImage alpha = download(*small);
        int top = t.h, bottom = -1, left = t.w, right = -1;
        for (int y = 0; y < alpha.height(); ++y) {
            const auto* row = reinterpret_cast<const QRgb*>(alpha.constScanLine(y));
            for (int x = 0; x < alpha.width(); ++x)
                if (qRed(row[x]) < block) {
                    left = std::min(left, x * block + qRed(row[x]));
                    top = std::min(top, y * block + qGreen(row[x]));
                    right = std::max(right, x * block + qBlue(row[x]));
                    bottom = std::max(bottom, y * block + qAlpha(row[x]));
                }
        }
        return bottom < 0 ? QRect() : QRect(QPoint(left, top), QPoint(right, bottom));
    }

    // A scene's own pictures while it paints through OpenGL (motion::GpuPictures): motion blur moments summed, soft
    // layers blurred and given their effects, groups seen through as one, all in this compositor's targets.
    struct ScenePictures : motion::GpuPictures {
        Impl& d;
        std::map<int, Layer> held;
        int next = 0;
        explicit ScenePictures(Impl& d) : d(d) {}
        Picture keep(Layer layer)
        {
            Picture picture;
            picture.fbo = layer->fbo;
            picture.tex = layer->tex;
            picture.size = layer->size();
            picture.handle = next++;
            held.emplace(picture.handle, std::move(layer));
            return picture;
        }
        void refresh(Picture& picture)
        {
            const Target& t = *held.at(picture.handle);
            picture.fbo = t.fbo;
            picture.tex = t.tex;
            picture.size = t.size();
        }
        Picture acquire(QSize size, bool precise) override
        {
            Layer layer = d.acquire(size, true, precise ? GL_RGBA32F : GL_RGBA8);
            d.ensureStencil(*layer);
            return keep(std::move(layer));
        }
        void release(const Picture& picture) override { held.erase(picture.handle); }
        QRect drawnBounds(const Picture& picture, int block) override { return d.drawnBounds(*held.at(picture.handle), block); }
        Picture crop(const Picture& picture, const QRect& region) override
        {
            const Target& src = *held.at(picture.handle);
            Layer out = d.acquire(region.size(), false);
            d.drawTexture(*out, src.tex, QSizeF(src.w, src.h), QTransform::fromTranslate(-region.x(), -region.y()), 1.0, QRectF(0, 0, 1, 1), false);
            return keep(std::move(out));
        }
        void blur(Picture& picture, double radius) override
        {
            if (radius < .5)
                return;
            // The CPU's soft blur (shrunk, three box blurs each way, grown back) as one Gaussian of the same spread.
            const double shrink = std::max(1., radius / 3);
            const int r = std::max(1, int(std::lround(radius / shrink / 1.7)));
            const double sigma = shrink * std::sqrt(double(r) * (r + 1));
            d.blur(held.at(picture.handle), sigma, sigma);
            refresh(picture);
        }
        void effect(Picture& picture, const EffectDef& def, const Params& params, const RenderContext& context) override
        {
            std::unique_ptr<EffectState> state; // (each frame fresh, as on the CPU)
            d.applyEffect(def, params, held.at(picture.handle), context, state);
            refresh(picture);
        }
    };

    // Remove Background (as its CPU version in Segmentation.cpp): the picture is
    // shrunk to the model's size here and read back (small), the model runs on
    // the CPU, and the mask comes back up to blend the picture over the
    // transparent, blurred or coloured background in a shader.
    void removeBackground(Layer& layer, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        const SegmentModel which = SegmentModel(std::clamp(p.choice(QStringLiteral("model")), 0, 1));
        const QSize size = layer->size(), in = segmentationInputSize(which, size);
        Layer small = acquire(size, false);
        drawLayerReplace(*small, *layer);
        for (QSize s = size; s.width() > 2 * in.width() || s.height() > 2 * in.height();) { // halving: a box filter, like smooth scaling
            s = QSize(std::max(in.width(), s.width() / 2), std::max(in.height(), s.height() / 2));
            Layer half = acquire(s, false);
            drawTexture(*half, small->tex, QSizeF(s), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
            small = std::move(half);
        }
        Layer exact = acquire(in, false);
        drawTexture(*exact, small->tex, QSizeF(in), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
        QImage mask = personMaskSmall(download(*exact), which, state);
        if (mask.isNull())
            return; // no model: the picture as it is
        if (p.flag(QStringLiteral("steady")))
            steadyMask(mask, state);
        Layer maskTex = acquire(in, false);
        upload(*maskTex, mask.convertToFormat(QImage::Format_ARGB32_Premultiplied));
        const int mode = p.choice(QStringLiteral("mode"));
        Layer blurred;
        if (mode == 1) {
            blurred = acquire(size, false);
            drawLayerReplace(*blurred, *layer);
            const double sigma = std::max(0.5, p.num(QStringLiteral("blur")) * ctx.scale / 2);
            blur(blurred, sigma, sigma);
        }
        Layer out = acquire(size, false);
        const QColor c = p.color(QStringLiteral("color"));
        fxPass(*out, gpufx::SegmentComposite, *layer, [&](QOpenGLShaderProgram& g) {
            g.setUniformValue("uA", QVector4D(float(p.num(QStringLiteral("threshold"))),
                                              which == SegmentModel::Quality ? -1.0f : float(p.num(QStringLiteral("softness"))) / 2,
                                              float(mode), p.flag(QStringLiteral("invert")) ? 1.0f : 0.0f));
            g.setUniformValue("uB", QVector4D(float(c.redF()), float(c.greenF()), float(c.blueF()), 1.0f));
        }, &*maskTex, blurred ? &*blurred : nullptr);
        layer = std::move(out);
    }

    // ---- User effects: Effect Builder graphs (core/NodeGraph.h), node by node on the GPU ----

    struct NodeGraphState : EffectState {
        Impl* owner = nullptr;
        QString text;
        nodes::Graph graph;
        std::map<int, StateEntry> states;         // each node's own (effect nodes, nodes drawn on the CPU)
        std::map<int, std::deque<Target>> delays; // Delay nodes: the frames they saw, newest last
        std::map<int, Target> painted;            // source nodes painted on the GPU
        ~NodeGraphState() override
        {
            if (owner)
                owner->freeNodeGraphState(*this);
        }
    };

    void freeNodeGraphState(NodeGraphState& s)
    {
        s.states.clear(); // (effect states free their own textures)
        if (QOpenGLContext::currentContext() != context.get())
            return; // context already gone: the driver frees everything
        for (auto& [id, frames] : s.delays)
            for (Target& t : frames)
                deleteTarget(t);
        for (auto& [id, t] : s.painted)
            deleteTarget(t);
    }

    // The node's picture drawn by its CPU version (the inputs read back, the result sent up again).
    Layer cpuNode(const nodes::NodeDef& def, const nodes::Node& n, const std::vector<const Target*>& in, const Params& p,
                  const nodes::NodeContext& nc, std::unique_ptr<EffectState>& state)
    {
        ++cpuFallbacks;
        std::vector<QImage> images(in.size());
        std::vector<const QImage*> ptrs(in.size(), nullptr);
        for (size_t i = 0; i < in.size(); ++i)
            if (in[i]) {
                images[i] = download(*in[i]);
                ptrs[i] = &images[i];
            }
        QImage img = def.eval(ptrs, p, n.params, nc, state);
        if (!img.isNull() && img.size() != nc.size)
            img = img.scaled(nc.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        Layer out = acquire(nc.size, img.isNull());
        if (!img.isNull())
            upload(*out, img);
        return out;
    }

    std::unique_ptr<EffectState> builderState; // renderGraph's (the Effect Builder's preview)

    QImage renderGraph(const nodes::Graph& graph, const QImage& source, const RenderContext& ctx, std::map<int, QImage>* thumbs, QSize thumb)
    {
        if (source.isNull() || !context->makeCurrent(surface))
            return source;
        ++tick;
        auto* s = dynamic_cast<NodeGraphState*>(builderState.get());
        if (!s) {
            auto fresh = std::make_unique<NodeGraphState>();
            fresh->owner = this;
            s = fresh.get();
            builderState = std::move(fresh);
        }
        s->graph = graph;
        Layer layer = acquire(source.size(), false);
        upload(*layer, source);
        evalGraph(*s, layer, ctx, thumbs, thumb);
        QImage img = download(*layer);
        layer.reset();
        evict();
        return img;
    }

    // ---- Screen shaders: libretro GLSL presets (RetroShaders.h), pass after pass as RetroArch runs them ----

    struct RetroState : EffectState {
        Impl* owner = nullptr;
        std::deque<Target> history; // the pictures fed in, newest first (PrevTexture, Prev1Texture...)
        ~RetroState() override
        {
            if (owner && QOpenGLContext::currentContext() == owner->context.get())
                for (Target& t : history)
                    owner->deleteTarget(t);
        }
    };
    struct RetroPrograms {
        std::vector<std::unique_ptr<QOpenGLShaderProgram>> passes;
        bool failed = false;
    };
    std::map<QString, RetroPrograms> retroPrograms; // by preset file
    std::map<QString, std::pair<GLuint, QSize>> retroLuts; // by image file
    GLuint retroVao = 0, retroVbo = 0;

    RetroPrograms& retroProgramsFor(const retro::Preset& pre)
    {
        RetroPrograms& rp = retroPrograms[pre.path];
        if (!rp.passes.empty() || rp.failed)
            return rp;
        for (const retro::Pass& pass : pre.passes) {
            QFile f(pass.path);
            const QString src = f.open(QIODevice::ReadOnly | QIODevice::Text) ? QString::fromUtf8(f.readAll()) : QString();
            auto prog = std::make_unique<QOpenGLShaderProgram>();
            prog->bindAttributeLocation("VertexCoord", 0);
            prog->bindAttributeLocation("TexCoord", 1);
            prog->bindAttributeLocation("COLOR", 2);
            if (src.isEmpty() || !prog->addShaderFromSourceCode(QOpenGLShader::Vertex, retro::passSource(src, true))
                || !prog->addShaderFromSourceCode(QOpenGLShader::Fragment, retro::passSource(src, false)) || !prog->link()) {
                qWarning("screen shader %s: %s", qPrintable(pass.path), qPrintable(prog->log().left(600)));
                rp.passes.clear();
                rp.failed = true;
                return rp;
            }
            rp.passes.push_back(std::move(prog));
        }
        return rp;
    }

    GLuint retroLut(const retro::Lut& lut, QSize* size)
    {
        auto it = retroLuts.find(lut.path);
        if (it == retroLuts.end()) {
            QImage img = QImage(lut.path).convertToFormat(QImage::Format_RGBA8888);
            GLuint tex = 0;
            if (!img.isNull()) {
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_2D, tex);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img.width(), img.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, img.constBits());
                if (lut.mipmap)
                    glGenerateMipmap(GL_TEXTURE_2D);
            }
            it = retroLuts.emplace(lut.path, std::pair{tex, img.size()}).first;
        }
        if (it->second.first) { // (its sampling, each time: two presets may want it differently)
            glBindTexture(GL_TEXTURE_2D, it->second.first);
            retroSampling(lut.linear, lut.wrap, lut.mipmap);
        }
        *size = it->second.second;
        return it->second.first;
    }

    // The bound texture's filtering and edges as a pass asks for them.
    void retroSampling(bool linear, retro::Wrap wrap, bool mipmap)
    {
        constexpr GLenum kClampToBorder = 0x812D;
        const GLint w = wrap == retro::Wrap::Edge     ? GL_CLAMP_TO_EDGE
                        : wrap == retro::Wrap::Repeat ? GL_REPEAT
                        : wrap == retro::Wrap::Mirror ? GL_MIRRORED_REPEAT
                                                      : GLint(kClampToBorder);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, w);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, w);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmap ? (linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST) : (linear ? GL_LINEAR : GL_NEAREST));
    }
    void retroSamplingDefault(GLuint tex)
    {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    }

    // True when it was a screen shader (its picture made, or left as it was when the preset can't run).
    bool retroEffect(const QString& id, const Params& p, Layer& layer, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        if (!id.startsWith(QLatin1String("retro.")))
            return false;
        const retro::Preset* pre = retro::preset(id);
        if (!pre)
            return true;
        RetroPrograms& rp = retroProgramsFor(*pre);
        if (rp.failed)
            return true;
        if (!retroVao) { // VertexCoord, TexCoord, COLOR: a quad over 0..1
            const float v[] = {0, 0, 0, 1, 0, 0, 0, 0, 1, 1, 1, 1, //
                               1, 0, 0, 1, 1, 0, 0, 0, 1, 1, 1, 1, //
                               0, 1, 0, 1, 0, 1, 0, 0, 1, 1, 1, 1, //
                               1, 1, 0, 1, 1, 1, 0, 0, 1, 1, 1, 1};
            glGenVertexArrays(1, &retroVao);
            glGenBuffers(1, &retroVbo);
            glBindVertexArray(retroVao);
            glBindBuffer(GL_ARRAY_BUFFER, retroVbo);
            glBufferData(GL_ARRAY_BUFFER, sizeof v, v, GL_STATIC_DRAW);
            for (GLuint a = 0; a < 3; ++a) {
                glEnableVertexAttribArray(a);
                glVertexAttribPointer(a, 4, GL_FLOAT, GL_FALSE, 12 * sizeof(float), reinterpret_cast<const void*>(uintptr_t(a * 4 * sizeof(float))));
            }
            glBindVertexArray(0);
        }
        if (!state || !dynamic_cast<RetroState*>(state.get())) {
            auto s = std::make_unique<RetroState>();
            s->owner = this;
            state = std::move(s);
        }
        RetroState& st = *static_cast<RetroState*>(state.get());
        const QSize out = layer->size();
        // The picture fed in: flattened onto black, at an old console's lines (or as it is).
        static const int kLines[] = {0, 1080, 720, 480, 360, 288, 240, 224, 160, 144};
        const int lines = kLines[std::clamp(p.choice(QString::fromLatin1(retro::kSourceLines)), 0, int(std::size(kLines)) - 1)];
        const QSize in = lines && lines < out.height() ? QSize(std::max(1, int(std::lround(double(out.width()) * lines / out.height()))), lines) : out;
        Layer orig = acquire(in, false);
        clear(*orig, 0, 0, 0, 1);
        drawTexture(*orig, layer->tex, QSizeF(out), QTransform::fromScale(double(in.width()) / out.width(), double(in.height()) / out.height()), 1.0);
        const int64_t frame = int64_t(std::llround(double(ctx.time) * std::max(1.0, ctx.fps) / kUsPerSecond));
        std::vector<Layer> outputs;
        for (size_t i = 0; i < pre->passes.size(); ++i) {
            const retro::Pass& ps = pre->passes[i];
            const Target& src = outputs.empty() ? *orig : *outputs.back();
            const bool last = i + 1 == pre->passes.size();
            auto axis = [&](retro::Scale type, double scale, int source, int viewport) {
                if (last && !ps.scaleSet)
                    return viewport;
                const double v = type == retro::Scale::Absolute ? scale : (type == retro::Scale::Viewport ? viewport : source) * scale;
                return std::clamp(int(std::lround(v)), 1, 8192);
            };
            const QSize size(axis(ps.scaleX, ps.sx, src.w, out.width()), axis(ps.scaleY, ps.sy, src.h, out.height()));
            const GLenum fmt = ps.floatBuffer ? GL_RGBA16F : (ps.srgbBuffer ? GL_SRGB8_ALPHA8 : GL_RGBA8);
            Layer dst = acquire(size, true, fmt);
            QOpenGLShaderProgram& prog = *rp.passes[i];
            prog.bind();
            int unit = 0;
            auto sampler = [&](const QByteArray& name, GLuint tex, QSize texSize, bool linear, retro::Wrap wrap, bool mipmap) {
                const int loc = prog.uniformLocation(name);
                const int sizeLoc = prog.uniformLocation(QByteArray(name).replace("Texture", "TextureSize"));
                const int inLoc = prog.uniformLocation(QByteArray(name).replace("Texture", "InputSize"));
                if (loc < 0 && sizeLoc < 0 && inLoc < 0)
                    return;
                if (loc >= 0) {
                    bindTexture(unit, tex);
                    retroSampling(linear, wrap, mipmap);
                    prog.setUniformValue(loc, unit++);
                }
                if (sizeLoc >= 0)
                    prog.setUniformValue(sizeLoc, QVector2D(float(texSize.width()), float(texSize.height())));
                if (inLoc >= 0)
                    prog.setUniformValue(inLoc, QVector2D(float(texSize.width()), float(texSize.height())));
            };
            // The pass's input (as it asks to sample it), the original, the passes before, the frames before, the LUTs.
            if (ps.mipmapInput) {
                glBindTexture(GL_TEXTURE_2D, src.tex);
                glGenerateMipmap(GL_TEXTURE_2D);
            }
            bindTexture(unit, src.tex);
            retroSampling(ps.linear, ps.wrap, ps.mipmapInput);
            prog.setUniformValue("Texture", unit++);
            prog.setUniformValue("TextureSize", QVector2D(float(src.w), float(src.h)));
            prog.setUniformValue("InputSize", QVector2D(float(src.w), float(src.h)));
            prog.setUniformValue("OutputSize", QVector2D(float(size.width()), float(size.height())));
            prog.setUniformValue("FrameCount", int(ps.frameCountMod > 0 ? frame % ps.frameCountMod : frame));
            prog.setUniformValue("FrameDirection", 1);
            QMatrix4x4 mvp; // (0..1 onto the whole target)
            mvp.ortho(0, 1, 0, 1, -1, 1);
            prog.setUniformValue("MVPMatrix", mvp);
            sampler("OrigTexture", orig->tex, in, ps.linear, ps.wrap, false);
            for (size_t j = 0; j < outputs.size(); ++j) {
                const Target& o = *outputs[j];
                sampler("Pass" + QByteArray::number(qulonglong(j + 1)) + "Texture", o.tex, o.size(), ps.linear, ps.wrap, false);
                sampler("PassPrev" + QByteArray::number(qulonglong(i - j)) + "Texture", o.tex, o.size(), ps.linear, ps.wrap, false);
                if (!pre->passes[j].alias.isEmpty())
                    sampler(pre->passes[j].alias.toUtf8() + "Texture", o.tex, o.size(), ps.linear, ps.wrap, false);
            }
            sampler("PassPrev" + QByteArray::number(qulonglong(i + 1)) + "Texture", orig->tex, in, ps.linear, ps.wrap, false);
            for (int h = 0; h < 7; ++h) {
                const QByteArray name = h == 0 ? QByteArray("PrevTexture") : "Prev" + QByteArray::number(h) + "Texture";
                const bool have = size_t(h) < st.history.size() && st.history[size_t(h)].size() == in;
                sampler(name, have ? st.history[size_t(h)].tex : orig->tex, in, ps.linear, ps.wrap, false);
            }
            for (const retro::Lut& lut : pre->luts) {
                const int loc = prog.uniformLocation(lut.name.toUtf8());
                if (loc < 0)
                    continue;
                glActiveTexture(GL_TEXTURE0 + GLenum(unit)); // (its own unit: retroLut binds the texture where it is)
                QSize lutSize;
                if (retroLut(lut, &lutSize))
                    prog.setUniformValue(loc, unit++);
            }
            for (const retro::Parameter& par : pre->parameters)
                if (const int loc = prog.uniformLocation(par.id.toUtf8()); loc >= 0)
                    prog.setUniformValue(loc, float(p.num(par.id)));
            glBindFramebuffer(GL_FRAMEBUFFER, dst->fbo);
            glViewport(0, 0, size.width(), size.height());
            glDisable(GL_BLEND);
            glDisable(GL_SCISSOR_TEST);
            constexpr GLenum kFramebufferSrgb = 0x8DB9;
            if (ps.srgbBuffer)
                glEnable(kFramebufferSrgb);
            glBindVertexArray(retroVao);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            glBindVertexArray(0);
            if (ps.srgbBuffer)
                glDisable(kFramebufferSrgb);
            // (the textures back as the rest of the compositor samples them)
            retroSamplingDefault(src.tex);
            retroSamplingDefault(orig->tex);
            for (Layer& o : outputs)
                retroSamplingDefault(o->tex);
            for (Target& t : st.history)
                retroSamplingDefault(t.tex);
            glActiveTexture(GL_TEXTURE0);
            outputs.push_back(std::move(dst));
        }
        // The last pass's picture, at the layer's size, opaque.
        Layer result = acquire(out, false);
        drawTexture(*result, outputs.back()->tex, QSizeF(outputs.back()->size()),
                    QTransform::fromScale(double(out.width()) / outputs.back()->w, double(out.height()) / outputs.back()->h), 1.0, QRectF(0, 0, 1, 1), false);
        layer = std::move(result);
        // This picture fed in, kept for the next frames.
        if (pre->history > 0) {
            Target keep;
            copyInto(keep, *orig);
            st.history.push_front(keep);
            while (int(st.history.size()) > pre->history) {
                deleteTarget(st.history.back());
                st.history.pop_back();
            }
        }
        return true;
    }

    void nodeGraphEffect(const Params& p, Layer& layer, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        auto* s = dynamic_cast<NodeGraphState*>(state.get());
        if (!s) {
            auto fresh = std::make_unique<NodeGraphState>();
            fresh->owner = this;
            s = fresh.get();
            state = std::move(fresh);
        }
        nodes::graphFor(p, s->text, s->graph);
        evalGraph(*s, layer, ctx, nullptr, {});
    }

    // The graph in `s` on `layer` (its result replaces it); each node's picture, at most `thumb` big, into `thumbs`.
    void evalGraph(NodeGraphState& state, Layer& layer, const RenderContext& ctx, std::map<int, QImage>* thumbs, QSize thumb)
    {
        NodeGraphState* s = &state;
        const nodes::Graph& g = s->graph;
        const nodes::Node* output = nullptr;
        for (const nodes::Node& n : g.nodes)
            if (n.type == QLatin1String("output"))
                output = &n;
        if (!output)
            return; // (the source, unchanged)
        for (auto it = s->states.begin(); it != s->states.end();) // (nodes deleted since)
            it = g.find(it->first) ? std::next(it) : s->states.erase(it);
        const QSize size = layer->size();
        const nodes::NodeContext nc{size, ctx};
        std::map<int, Layer> done;
        std::set<int> visiting;
        std::function<const Target*(int)> eval = [&](int id) -> const Target* {
            if (auto it = done.find(id); it != done.end())
                return it->second.get();
            const nodes::Node* n = g.find(id);
            if (!n || !visiting.insert(id).second)
                return (done[id] = acquire(size)).get();
            const nodes::NodeDef* def = nodes::findNode(n->type);
            std::vector<const Target*> in(def ? size_t(def->inputs.size()) : 0, nullptr);
            for (int i = 0; i < int(in.size()); ++i)
                if (const nodes::Link* l = g.input(id, i))
                    in[size_t(i)] = eval(l->from);
            const QString& type = n->type;
            Layer out;
            if (!def) {
                out = acquire(size);
            } else if (type == QLatin1String("source")) {
                out = acquire(size, false);
                drawLayerReplace(*out, *layer);
            } else if (!def->output) { // the Output node: what is wired in, or the source
                out = acquire(size, false);
                drawLayerReplace(*out, in.empty() || !in[0] ? *layer : *in[0]);
            } else {
                const Params pr = resolveParams(def->params, n->params, ctx.time);
                StateEntry& st = s->states[id];
                if (st.type != type) {
                    st.state.reset();
                    st.type = type;
                }
                out = gpuNode(*def, *n, in, pr, nc, *s, st.state);
            }
            visiting.erase(id);
            if (thumbs && out) { // (drawn small on the GPU, so only that is read back)
                const QSize ts = out->size().scaled(thumb, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
                Layer small = acquire(ts);
                ensureMipmaps(*out);
                drawTexture(*small, out->tex, QSizeF(ts), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
                (*thumbs)[id] = download(*small);
            }
            return (done[id] = std::move(out)).get();
        };
        const Target* result = eval(output->id);
        Layer final = acquire(size, false);
        drawLayerReplace(*final, *result);
        layer = std::move(final);
    }

    Layer gpuNode(const nodes::NodeDef& def, const nodes::Node& n, const std::vector<const Target*>& in, const Params& p,
                  const nodes::NodeContext& nc, NodeGraphState& s, std::unique_ptr<EffectState>& state)
    {
        const QSize size = nc.size;
        const QString& type = n.type;
        auto has = [&](size_t i) { return i < in.size() && in[i]; };
        auto tex = [&](size_t i) { return has(i) ? in[i]->tex : emptyTex; };
        auto copyIn = [&](size_t i) {
            Layer l = acquire(size, !has(i));
            if (has(i))
                drawLayerReplace(*l, *in[i]);
            return l;
        };
        if (type.startsWith(QLatin1String("fx."))) { // a video effect: as on an event (Compositor::applyChain)
            const EffectDef* fx = findEffect(type.mid(3));
            Layer l = copyIn(0);
            if (!fx || !fx->apply)
                return l;
            RenderContext c = nc.render;
            c.contentRect = QRectF(0, 0, size.width(), size.height());
            const fx::PostMask post = fx->glitchPost ? fx::postMask(p, size) : fx::PostMask{};
            Layer before;
            if (post.active())
                before = copyIn(0);
            applyEffect(*fx, p, l, c, state);
            if (post.active() && l->size() == before->size()) {
                Layer o = acquire(size, false);
                fxPass(*o, gpufx::GlitchPost, *l, [&](QOpenGLShaderProgram& g) {
                    g.setUniformValue("uA", QVector4D(post.mix, post.threshold, post.feather, float(post.mode)));
                    g.setUniformValue("uB", QVector4D(post.x0, post.y0, post.x1, post.y1));
                }, &*before);
                l = std::move(o);
            }
            if (l->size() != size) {
                Layer o = acquire(size);
                drawTexture(*o, l->tex, QSizeF(size), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
                l = std::move(o);
            }
            return l;
        }
        if (type.startsWith(QLatin1String("gen."))) { // a source: painted on the GPU where it can be
            const GeneratorPreset* preset = findGeneratorPreset(type.mid(4));
            const GeneratorDef* gen = preset ? findGenerator(preset->generator) : nullptr;
            if (gen && gen->paint && !qEnvironmentVariableIsSet("MVP_CPU_GENERATORS")) {
                static const QJsonObject kNoData;
                GeneratorContext gc;
                gc.time = nc.render.time;
                gc.length = 3600 * kUsPerSecond; // (as the CPU node: a title's out-animation never comes)
                gc.timelineTime = nc.render.timelineTime;
                gc.scale = nc.render.scale;
                gc.fps = FrameRate{std::max(1, int(std::lround(nc.render.fps))), 1};
                gc.highQuality = highQuality;
                gc.data = &kNoData;
                const Params gp = resolveParams(gen->params, n.params, gc.time);
                const int scale = std::clamp(gen->paintScale, 1, 2);
                Target& t = s.painted[n.id];
                if (!t || t.size() != size * scale) {
                    deleteTarget(t);
                    t = createTarget(size * scale);
                }
                if (paintGenerator(*gen, t, size, scale, gp, gc)) {
                    Layer o = acquire(size);
                    drawTexture(*o, t.tex, QSizeF(size), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
                    return o;
                }
            }
            return cpuNode(def, n, in, p, nc, state);
        }
        if (type == QLatin1String("blend")) {
            Layer base = copyIn(0);
            if (!has(1))
                return base;
            Layer o = acquire(size, false);
            progRackBlend->bind();
            bindTexture(0, base->tex);
            bindTexture(1, in[1]->tex);
            progRackBlend->setUniformValue("uBase", 0);
            progRackBlend->setUniformValue("uLayer", 1);
            progRackBlend->setUniformValue("uOpacity", float(std::clamp(p.num(QStringLiteral("opacity")), 0.0, 1.0)));
            progRackBlend->setUniformValue("uMode", p.choice(QStringLiteral("mode")));
            fullscreen(*progRackBlend, *o);
            return o;
        }
        if (type == QLatin1String("mask")) {
            Layer o = acquire(size, false);
            progMatte->bind();
            bindTexture(0, tex(0));
            bindTexture(1, tex(1));
            progMatte->setUniformValue("uLayer", 0);
            progMatte->setUniformValue("uMatte", 1);
            progMatte->setUniformValue("uMode", p.choice(QStringLiteral("mode")) + 1);
            fullscreen(*progMatte, *o);
            return o;
        }
        if (type == QLatin1String("place")) {
            Layer o = acquire(size);
            if (!has(0))
                return o;
            const double scale = p.num(QStringLiteral("scale"));
            QTransform t;
            t.translate(size.width() * (0.5 + p.num(QStringLiteral("x")) / 2), size.height() * (0.5 + p.num(QStringLiteral("y")) / 2));
            t.rotate(p.num(QStringLiteral("rotation")));
            t.scale(scale, scale);
            drawTexture(*o, in[0]->tex, QSizeF(size), QTransform::fromTranslate(-size.width() / 2.0, -size.height() / 2.0) * t, 1.0);
            return o;
        }
        if (type == QLatin1String("delay")) {
            std::deque<Target>& frames = s.delays[n.id];
            const size_t keep = size_t(std::clamp(int(p.num(QStringLiteral("frames"))), 1, 120)) + 1;
            Target slot;
            if (frames.size() >= keep) { // (the oldest's texture is reused for the newest)
                slot = frames.front();
                frames.pop_front();
            }
            while (frames.size() > keep - 1) {
                deleteTarget(frames.front());
                frames.pop_front();
            }
            if (slot.size() != size) {
                deleteTarget(slot);
                slot = createTarget(size);
            }
            if (has(0))
                drawLayerReplace(slot, *in[0]);
            else
                clear(slot, 0, 0, 0, 0);
            frames.push_back(slot);
            Layer o = acquire(size, false);
            drawLayerReplace(*o, frames.front());
            return o;
        }
        static const std::map<QString, int> kModes = {
            {QStringLiteral("mix"), 0},      {QStringLiteral("math"), 1},           {QStringLiteral("set_alpha"), 2},
            {QStringLiteral("luma_key"), 3}, {QStringLiteral("difference_key"), 4}, {QStringLiteral("channel"), 5},
            {QStringLiteral("combine"), 6},  {QStringLiteral("threshold"), 7},      {QStringLiteral("displace"), 8}};
        const auto mode = kModes.find(type);
        if (mode == kModes.end())
            return cpuNode(def, n, in, p, nc, state);
        QVector4D u;
        switch (mode->second) {
        case 0: u = QVector4D(float(p.num(QStringLiteral("amount"))), 0, 0, 0); break;
        case 1: u = QVector4D(float(p.choice(QStringLiteral("op"))), float(p.num(QStringLiteral("gain"))), 0, 0); break;
        case 2: u = QVector4D(p.flag(QStringLiteral("invert")) ? 1.f : 0.f, 0, 0, 0); break;
        case 3: {
            const float lo = float(p.num(QStringLiteral("low")));
            u = QVector4D(lo, std::max(lo + 0.001f, float(p.num(QStringLiteral("high")))), p.flag(QStringLiteral("invert")) ? 1.f : 0.f, 0);
            break;
        }
        case 4: u = QVector4D(float(p.num(QStringLiteral("threshold"))), std::max(0.001f, float(p.num(QStringLiteral("softness")))), 0, 0); break;
        case 5: u = QVector4D(float(p.choice(QStringLiteral("channel"))), 0, 0, 0); break;
        case 7: u = QVector4D(float(p.num(QStringLiteral("level"))), std::max(0.001f, float(p.num(QStringLiteral("softness")))), 0, 0); break;
        case 8:
            u = QVector4D(float(p.num(QStringLiteral("amountX")) * nc.render.scale), float(p.num(QStringLiteral("amountY")) * nc.render.scale),
                          p.choice(QStringLiteral("use")) == 1 ? 1.f : 0.f, 0);
            break;
        default: break;
        }
        Layer o = acquire(size, false);
        progNode->bind();
        for (int i = 0; i < 4; ++i)
            bindTexture(i, tex(size_t(i)));
        progNode->setUniformValue("uTex", 0);
        progNode->setUniformValue("uB", 1);
        progNode->setUniformValue("uC", 2);
        progNode->setUniformValue("uD", 3);
        progNode->setUniformValue("uMode", mode->second);
        glUniform4i(progNode->uniformLocation("uHas"), has(0), has(1), has(2), has(3));
        progNode->setUniformValue("uP", u);
        progNode->setUniformValue("uSize", QVector2D(float(size.width()), float(size.height())));
        fullscreen(*progNode, *o);
        return o;
    }

    void applyEffect(const EffectDef& def, const Params& p, Layer& layer, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        const QSize size = layer->size();
        if (def.luts) {
            ChannelLuts l;
            if (!def.luts(p, l))
                return;
            uint8_t rgba[256 * 4];
            for (int i = 0; i < 256; ++i) {
                // BGRA upload: byte order B, G, R, A.
                rgba[i * 4 + 0] = l[2][size_t(i)];
                rgba[i * 4 + 1] = l[1][size_t(i)];
                rgba[i * 4 + 2] = l[0][size_t(i)];
                rgba[i * 4 + 3] = 255;
            }
            glBindTexture(GL_TEXTURE_2D, lutTex);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 1, GL_BGRA, GL_UNSIGNED_BYTE, rgba);
            filterPass(layer, *progLut, [&] {
                bindTexture(1, lutTex);
                progLut->setUniformValue("uLut", 1);
            });
            return;
        }
        if (def.colorMatrix) {
            ColorMatrix m;
            if (!def.colorMatrix(p, m))
                return;
            const float rows[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
            filterPass(layer, *progMatrix, [&] {
                progMatrix->setUniformValue("uM", rowsToMat3(rows));
                progMatrix->setUniformValue("uO", QVector3D(m[3], m[7], m[11]));
            });
            return;
        }
        const QString& id = def.id;
        if (id.startsWith(QLatin1String("user."))) {
            nodeGraphEffect(p, layer, ctx, state);
        } else if (id == QLatin1String("gaussian_blur")) {
            blur(layer, p.num(QStringLiteral("horizontal")) * ctx.scale, p.num(QStringLiteral("vertical")) * ctx.scale);
        } else if (id == QLatin1String("sharpen")) {
            const float amount = float(p.num(QStringLiteral("amount")));
            if (amount <= 0)
                return;
            Layer blurred = acquire(size, false);
            drawLayerReplace(*blurred, *layer);
            const double r = p.num(QStringLiteral("radius")) * ctx.scale;
            blur(blurred, r, r);
            filterPass(layer, *progSharpen, [&] {
                bindTexture(1, blurred->tex);
                progSharpen->setUniformValue("uBlur", 1);
                progSharpen->setUniformValue("uAmount", amount);
            });
        } else if (id == QLatin1String("glow")) {
            const float intensity = float(p.num(QStringLiteral("intensity")));
            if (intensity <= 0)
                return;
            Layer bright = acquire(size, false);
            progBright->bind();
            bindTexture(0, layer->tex);
            progBright->setUniformValue("uTex", 0);
            progBright->setUniformValue("uThreshold", float(p.num(QStringLiteral("threshold"))));
            fullscreen(*progBright, *bright);
            const double r = p.num(QStringLiteral("radius")) * ctx.scale;
            blur(bright, r, r);
            filterPass(layer, *progScreen, [&] {
                bindTexture(1, bright->tex);
                progScreen->setUniformValue("uGlow", 1);
                progScreen->setUniformValue("uIntensity", intensity);
            });
        } else if (id == QLatin1String("vignette")) {
            if (p.num(QStringLiteral("amount")) == 0)
                return;
            filterPass(layer, *progVignette, [&] {
                progVignette->setUniformValue("uSize", QVector2D(float(size.width()), float(size.height())));
                progVignette->setUniformValue("uAmount", float(p.num(QStringLiteral("amount"))));
                progVignette->setUniformValue("uSizeP", float(p.num(QStringLiteral("size"))));
                progVignette->setUniformValue("uSoft", float(p.num(QStringLiteral("softness"))));
                progVignette->setUniformValue("uRound", float(p.num(QStringLiteral("roundness"))));
            });
        } else if (id == QLatin1String("chroma_key")) {
            const QColor key = p.color(QStringLiteral("key"));
            filterPass(layer, *progKey, [&] {
                progKey->setUniformValue("uKey", QVector3D(float(key.redF()), float(key.greenF()), float(key.blueF())));
                progKey->setUniformValue("uTol", float(p.num(QStringLiteral("tolerance"))));
                progKey->setUniformValue("uSoft", float(p.num(QStringLiteral("softness"))));
                progKey->setUniformValue("uSpill", float(p.num(QStringLiteral("spill"))));
                progKey->setUniformValue("uMask", GLint(p.flag(QStringLiteral("showMask"))));
            });
        } else if (id == QLatin1String("noise")) {
            const double amount = p.num(QStringLiteral("amount"));
            if (amount <= 0)
                return;
            const uint32_t seed = p.flag(QStringLiteral("animated"))
                                      ? uint32_t(std::llround(ctx.timelineTime * ctx.fps / kUsPerSecond))
                                      : 7u;
            filterPass(layer, *progNoise, [&] {
                progNoise->setUniformValue("uStrength", float(int(amount * 128)));
                progNoise->setUniformValue("uMono", GLint(p.flag(QStringLiteral("monochrome"))));
                // Qt's GLuint overload is glUniform1i (for samplers); a uint uniform needs glUniform1ui.
                glUniform1ui(progNoise->uniformLocation("uSeed"), seed);
            });
        } else if (id == QLatin1String("pixelate")) {
            const int block = std::max(1, int(std::lround(p.num(QStringLiteral("size")) * ctx.scale)));
            if (block <= 1)
                return;
            filterPass(layer, *progPixelate, [&] {
                progPixelate->setUniformValue("uSize", QVector2D(float(size.width()), float(size.height())));
                progPixelate->setUniformValue("uBlock", float(block));
            });
        } else if (id == QLatin1String("lut_cube")) {
            const float strength = float(p.num(QStringLiteral("strength")));
            const auto lut = loadCubeLut(p.text(QStringLiteral("file")));
            if (!lut || lut->size < 2 || strength <= 0)
                return;
            auto it = lut3dTextures.find(lut.get());
            if (it == lut3dTextures.end()) {
                if (lut3dTextures.size() > 8) { // keep a few: LUTs are a few MB each
                    glDeleteTextures(1, &lut3dTextures.begin()->second.second);
                    lut3dTextures.erase(lut3dTextures.begin());
                }
                GLuint tex = 0;
                glGenTextures(1, &tex);
                glBindTexture(GL_TEXTURE_3D, tex);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                for (GLenum wrap : {GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T, GL_TEXTURE_WRAP_R})
                    glTexParameteri(GL_TEXTURE_3D, wrap, GL_CLAMP_TO_EDGE);
                glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
                glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB32F, lut->size, lut->size, lut->size, 0, GL_RGB, GL_FLOAT, lut->rgb.data());
                glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
                it = lut3dTextures.emplace(lut.get(), std::pair{lut, tex}).first;
            }
            const GLuint tex = it->second.second;
            filterPass(layer, *progLut3D, [&] {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_3D, tex);
                progLut3D->setUniformValue("uLut3", 1);
                progLut3D->setUniformValue("uLutSize", float(lut->size));
                progLut3D->setUniformValue("uStrength", std::min(strength, 1.0f));
                progLut3D->setUniformValue("uDomMin", QVector3D(lut->domainMin[0], lut->domainMin[1], lut->domainMin[2]));
                progLut3D->setUniformValue("uDomMax", QVector3D(lut->domainMax[0], lut->domainMax[1], lut->domainMax[2]));
            });
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_3D, 0);
            glActiveTexture(GL_TEXTURE0);
        } else if (id == QLatin1String("remove_bg")) {
            removeBackground(layer, p, ctx, state);
        } else if (id == QLatin1String("transform")) {
            transformEffect(layer, p, ctx);
        } else if (id == QLatin1String("corner_pin")) {
            // As the CPU's: the picture's box onto the four corners, in perspective; the frame's corners: as it is.
            const std::pair<const char*, double> start[] = {{"tlX", 0}, {"tlY", 0}, {"trX", 1}, {"trY", 0}, {"brX", 1}, {"brY", 1}, {"blX", 0}, {"blY", 1}};
            if (std::all_of(std::begin(start), std::end(start), [&](const auto& s) { return p.num(QLatin1String(s.first)) == s.second; }))
                return;
            const QRectF src = ctx.contentRect.isEmpty() ? QRectF(QPointF(), QSizeF(size)) : ctx.contentRect;
            auto at = [&](const char* x, const char* y) { return QPointF(p.num(QLatin1String(x)) * size.width(), p.num(QLatin1String(y)) * size.height()); };
            QTransform t;
            if (!QTransform::quadToQuad(QPolygonF{QPointF(0, 0), QPointF(src.width(), 0), QPointF(src.width(), src.height()), QPointF(0, src.height())},
                                        QPolygonF{at("tlX", "tlY"), at("trX", "trY"), at("brX", "brY"), at("blX", "blY")}, t))
                return;
            Layer out = acquire(size);
            drawTexture(*out, layer->tex, src.size(), t, 1.0,
                        QRectF(src.x() / size.width(), src.y() / size.height(), src.width() / size.width(), src.height() / size.height()), false);
            layer = std::move(out);
        } else if (id == QLatin1String("flip")) {
            const bool h = p.flag(QStringLiteral("horizontal")), v = p.flag(QStringLiteral("vertical"));
            if (!h && !v)
                return;
            Layer out = acquire(size, false);
            drawTexture(*out, layer->tex, QSizeF(size), QTransform(), 1.0, QRectF(h ? 1 : 0, v ? 1 : 0, h ? -1 : 1, v ? -1 : 1), false);
            layer = std::move(out);
        } else if (retroEffect(id, p, layer, ctx, state) || glitchEffect(id, p, layer, ctx) || fxEffect(id, p, layer, ctx, state)
                   || consoleEffect(id, p, layer, ctx, state)) {
            // done on the GPU
        } else {
            // No GPU version (e.g. FFmpeg Filter): run the CPU effect on a copy.
            ++cpuFallbacks;
            QImage img = download(*layer);
            def.apply(img, p, ctx, state);
            if (img.size() != size)
                img = img.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            upload(*layer, img);
        }
    }

    // ---- Effects shader (GpuGlitchShaders.h): multi-pass and time-based effects ----

    // GPU-side state for time-based effects: recent input frames and the
    // previous output as textures (freed with the state).
    struct FxState : EffectState {
        Impl* owner = nullptr;
        std::vector<Target> frames; // newest first
        Target prevOut, out, vectors, atlas, held;
        int64_t heldKey = std::numeric_limits<int64_t>::min(); // broken-console freezes
        TimeUs last = std::numeric_limits<TimeUs>::min();
        int64_t tick = std::numeric_limits<int64_t>::min();
        int64_t frozenUntil = std::numeric_limits<int64_t>::min(); // stutter freezes
        int64_t cleanTick = std::numeric_limits<int64_t>::min();   // datamosh automatic clean frames
        bool vectorsValid = false;
        int blocksX = 0, blocksY = 0, blockPx = 0;
        int atlasCell = 0, atlasCharset = -1, atlasGlyphs = 0;
        ~FxState() override
        {
            if (owner)
                owner->freeFxState(*this);
        }
    };
    enum class Step { Reset, Next, Same };

    GLuint dataTex = 0;
    QSize dataSize;
    int cpuFallbacks = 0;

    void freeFxState(FxState& s)
    {
        if (QOpenGLContext::currentContext() != context.get())
            return; // context already gone: the driver frees everything
        for (Target& t : s.frames)
            deleteTarget(t);
        for (Target* t : {&s.prevOut, &s.out, &s.vectors, &s.atlas, &s.held})
            deleteTarget(*t);
    }

    FxState& fxStateFor(std::unique_ptr<EffectState>& st)
    {
        if (!st || !dynamic_cast<FxState*>(st.get())) {
            auto s = std::make_unique<FxState>();
            s->owner = this;
            st = std::move(s);
        }
        return *static_cast<FxState*>(st.get());
    }

    void copyInto(Target& dst, const Target& src)
    {
        if (!dst || dst.size() != src.size() || dst.fmt != src.fmt) {
            deleteTarget(dst);
            dst = createTarget(src.size(), src.fmt);
        }
        drawLayerReplace(dst, src);
    }

    // Same rules as fx::FrameHistory::push.
    Step pushHistory(FxState& st, const RenderContext& ctx, const Target& layer, size_t keep)
    {
        const double frame = double(kUsPerSecond) / std::max(1.0, ctx.fps);
        const bool sameSize = !st.frames.empty() && st.frames.front().size() == layer.size();
        Step s = Step::Reset;
        if (sameSize && ctx.timelineTime == st.last)
            s = Step::Same;
        else if (sameSize && std::abs(double(ctx.timelineTime - st.last) - frame) < frame * 0.5)
            s = Step::Next;
        keep = std::max<size_t>(1, keep);
        if (s == Step::Reset) {
            for (Target& t : st.frames)
                deleteTarget(t);
            st.frames.clear();
            deleteTarget(st.prevOut);
            deleteTarget(st.out);
            deleteTarget(st.held);
            st.heldKey = std::numeric_limits<int64_t>::min();
            st.vectorsValid = false;
        }
        if (s == Step::Same) {
            copyInto(st.frames.front(), layer);
        } else {
            if (s == Step::Next)
                std::swap(st.prevOut, st.out);
            Target slot;
            if (st.frames.size() >= keep) {
                slot = st.frames.back();
                st.frames.pop_back();
            }
            while (st.frames.size() > keep - 1) {
                deleteTarget(st.frames.back());
                st.frames.pop_back();
            }
            copyInto(slot, layer);
            st.frames.insert(st.frames.begin(), slot);
        }
        st.last = ctx.timelineTime;
        return s;
    }

    const Target& ago(const FxState& st, size_t n) const { return st.frames[std::min(n, st.frames.size() - 1)]; }

    // RGBA float data on texture unit 3 (per-row plans, tables).
    void setData(const float* rgba, int w, int h)
    {
        w = std::max(1, w);
        h = std::max(1, h);
        if (!dataTex) {
            glGenTextures(1, &dataTex);
            glBindTexture(GL_TEXTURE_2D, dataTex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        glBindTexture(GL_TEXTURE_2D, dataTex);
        if (dataSize != QSize(w, h)) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, rgba);
            dataSize = QSize(w, h);
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_FLOAT, rgba);
        }
    }
    void setRows(const std::vector<fx::RowPlan>& rows)
    {
        static_assert(sizeof(fx::RowPlan) == 4 * sizeof(float));
        const float zero[4] = {0, 0, 0, 0};
        setData(rows.empty() ? zero : &rows.front().a, int(rows.size()), 1);
    }

    using Uniforms = std::function<void(QOpenGLShaderProgram&)>;
    // One pass of the effects shader: `src` on unit 0 (uSize = its size
    // unless given), optional textures on units 1-2, the data texture on 3.
    void fxPass(const Target& dst, int mode, const Target& src, const Uniforms& uniforms, const Target* t1 = nullptr,
                const Target* t2 = nullptr, QSize srcSize = {})
    {
        QOpenGLShaderProgram& prog = *progFx;
        prog.bind();
        bindTexture(0, src.tex);
        bindTexture(1, t1 && t1->tex ? t1->tex : emptyTex);
        bindTexture(2, t2 && t2->tex ? t2->tex : emptyTex);
        bindTexture(3, dataTex ? dataTex : emptyTex);
        prog.setUniformValue("uTex", 0);
        prog.setUniformValue("uTex1", 1);
        prog.setUniformValue("uTex2", 2);
        prog.setUniformValue("uData", 3);
        const QSize sz = srcSize.isValid() ? srcSize : src.size();
        prog.setUniformValue("uSize", QVector2D(float(sz.width()), float(sz.height())));
        prog.setUniformValue("uMode", mode);
        if (uniforms)
            uniforms(prog);
        fullscreen(prog, dst);
    }
    void setI(QOpenGLShaderProgram& prog, const char* name, int a, int b = 0, int c = 0, int d = 0)
    {
        glUniform4i(prog.uniformLocation(name), a, b, c, d);
    }
    void setSeed(QOpenGLShaderProgram& prog, uint32_t seed) { prog.setUniformValue("uSeed", GLint(int32_t(seed))); }

    // GPU versions of the remaining glitch / analog / stylize effects.
    // Returns false when the effect (or its current mode) has no GPU path.
    bool fxEffect(const QString& id, const Params& p, Layer& layer, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        using namespace gpufx;
        const QSize size = layer->size();
        const int w = size.width(), h = size.height();
        const float sc = float(ctx.scale);
        const double t = double(ctx.timelineTime) / double(kUsPerSecond);
        const uint32_t frame = fx::frameIndex(ctx);
        constexpr float kPi = 3.14159265358979f;
        auto S = [](const char* s) { return QString::fromLatin1(s); };
        auto single = [&](int mode, const Uniforms& u, const Target* t1 = nullptr, const Target* t2 = nullptr) {
            Layer out = acquire(size, false);
            fxPass(*out, mode, *layer, u, t1, t2);
            layer = std::move(out);
        };
        auto bytes = [](const QColor& c) { return QVector4D(float(c.red()), float(c.green()), float(c.blue()), 255.0f); };

        if (id == QLatin1String("strobe")) {
            const fx::StrobeNow now = fx::strobeNow(p, ctx);
            const float opacity = float(p.num(S("opacity")));
            if (!now.on || opacity <= 0)
                return true;
            single(Strobe, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", p.choice(S("mode")), opacity >= 1 ? 256 : int(uint32_t(opacity * 256.0f)));
                g.setUniformValue("uA", QVector4D(float((now.rgb >> 16) & 255u), float((now.rgb >> 8) & 255u), float(now.rgb & 255u), 255.0f));
            });
        } else if (id == QLatin1String("pixel_stretch")) {
            single(PixelStretch, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", p.choice(S("direction")));
                g.setUniformValue("uA", QVector4D(fx::stretchPosition(p, ctx), float(p.num(S("jitter")) * ctx.scale), float(p.num(S("length"))), 0));
                setSeed(g, fx::stretchSeed(p, ctx));
            });
        } else if (id == QLatin1String("block_glitch")) {
            const float amount = float(p.num(S("amount")));
            if (amount <= 0)
                return true;
            const int bh = std::max(2, int(p.num(S("blockSize")) * ctx.scale));
            const int bw = std::max(2, int(float(bh) * float(p.num(S("aspect")))));
            single(BlockGlitch, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", bh, bw, p.choice(S("colorMode")));
                g.setUniformValue("uA", QVector4D(amount, float(p.num(S("maxShift")) * ctx.scale), float(p.num(S("colorGlitch"))),
                                                  float(p.num(S("verticalShift")))));
                g.setUniformValue("uB", QVector4D(float(p.num(S("noise"))), 0, 0, 0));
                setSeed(g, fx::glitchRateSeed(p, ctx));
            });
        } else if (id == QLatin1String("slice_glitch")) {
            const bool vertical = p.choice(S("direction")) == 1;
            static const int kEdgeCodes[3] = {1, 0, 3}; // wrap, clamp, black (fetchB codes)
            setRows(fx::sliceRows(p, ctx, vertical ? w : h));
            single(Slice, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", vertical ? 1 : 0, kEdgeCodes[std::clamp(p.choice(S("edge")), 0, 2)]);
                g.setUniformValue("uA", QVector4D(float(p.num(S("rgbSplit")) * ctx.scale), 0, 0, 0));
            });
        } else if (id == QLatin1String("tv_static")) {
            setRows(fx::tvStaticRows(p, ctx, h));
            single(TvStatic, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(p.num(S("amount"))), 0, 0, 0));
                setI(g, "uI", p.flag(S("colorNoise")) ? 1 : 0, std::max(1, int(std::lround(ctx.scale * 1.5))));
                setSeed(g, p.flag(S("animated")) ? frame : 0u);
            });
        } else if (id == QLatin1String("vhs_tracking")) {
            if (p.num(S("height")) <= 0)
                return true;
            setRows(fx::vhsTrackingRows(p, ctx, h));
            single(VhsTracking, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(p.num(S("noise"))), 0, 0, 0));
                setI(g, "uI", std::max(1, int(2 * sc)));
                setSeed(g, frame);
            });
        } else if (id == QLatin1String("ghosting")) {
            const float off = float(p.num(S("offset")) * ctx.scale), strength = float(p.num(S("strength")));
            if (strength <= 0 || off == 0)
                return true;
            single(Ghosting, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(off, strength, 0, 0));
                setI(g, "uI", std::max(1, int(p.num(S("echoes")))));
            });
        } else if (id == QLatin1String("halftone")) {
            single(Halftone, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(std::max(2.0, p.num(S("cellSize")) * ctx.scale)), float(p.num(S("angle"))) * kPi / 180.0f, 0, 0));
                setI(g, "uI", p.choice(S("mode")));
                g.setUniformValue("uB", bytes(p.color(S("ink"))));
                g.setUniformValue("uC", bytes(p.color(S("paper"))));
            });
        } else if (id == QLatin1String("emboss")) {
            const float a = float(p.num(S("angle"))) * kPi / 180.0f, dist = float(std::max(1.0, ctx.scale * 1.5));
            single(Emboss, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(std::cos(a) * dist, std::sin(a) * dist, float(p.num(S("strength"))), float(p.num(S("mix")))));
                setI(g, "uI", p.flag(S("keepColor")) ? 1 : 0);
            });
        } else if (id == QLatin1String("gradient_map")) {
            const auto lut = fx::gradientMapLut(p, ctx);
            std::vector<float> data(256 * 4);
            for (size_t i = 0; i < 256; ++i) {
                data[i * 4] = float((lut[i] >> 16) & 0xff);
                data[i * 4 + 1] = float((lut[i] >> 8) & 0xff);
                data[i * 4 + 2] = float(lut[i] & 0xff);
            }
            setData(data.data(), 256, 1);
            single(GradientMap, [&](QOpenGLShaderProgram& g) { g.setUniformValue("uA", QVector4D(float(p.num(S("mix"))), 0, 0, 0)); });
        } else if (id == QLatin1String("databend")) {
            const int mode = p.choice(S("mode"));
            if (p.num(S("mix")) <= 0)
                return true;
            if (mode == 0) {
                // Echo: acc[i] = in[i] + fb * acc[i - d] over the byte stream, as
                // a doubling scan: step k adds fb^(2^k) * acc[i - 2^k d].
                const int64_t n = int64_t(w) * h * 3;
                const int64_t delay = std::max<int64_t>(1, int64_t(p.num(S("delay")) * ctx.scale * ctx.scale));
                const float fb = float(p.num(S("feedback")));
                Layer a = acquire(size, false, GL_RGBA32F), b = acquire(size, false, GL_RGBA32F);
                fxPass(*a, DatabendLoad, *layer, {});
                float coeff = fb;
                for (int64_t shift = delay; shift < n && coeff > 1e-12f; shift *= 2) {
                    fxPass(*b, DatabendEchoStep, *a, [&](QOpenGLShaderProgram& g) {
                        setI(g, "uI", int(shift));
                        g.setUniformValue("uA", QVector4D(coeff, 0, 0, 0));
                    });
                    std::swap(a, b);
                    coeff *= coeff;
                }
                single(DatabendEchoMix, [&](QOpenGLShaderProgram& g) { setI(g, "uI", 0, 0, 0, int(float(p.num(S("mix"))) * 256.0f)); }, &*a);
                return true;
            }
            const int64_t bytes = int64_t(w) * h * 3;
            const int64_t delay = int64_t(std::max<size_t>(1, size_t(p.num(S("delay")) * ctx.scale * ctx.scale)));
            single(Databend, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", mode, int(std::max<size_t>(3, size_t(p.num(S("chunk")) * ctx.scale * ctx.scale))),
                     std::clamp(int(p.num(S("bits"))), 1, 7), int(float(p.num(S("mix"))) * 256.0f));
                setI(g, "uJ", int(delay % bytes), int(p.num(S("stride"))));
                g.setUniformValue("uA", QVector4D(float(p.num(S("feedback"))), 0, 0, 0));
                setSeed(g, p.flag(S("animated")) ? frame : 1u);
            });
        } else if (id == QLatin1String("old_film")) {
            const fx::FilmPlan plan = fx::oldFilmPlan(p, ctx, w, h);
            const int ns = int(plan.scratchDark.size());
            std::vector<float> data(size_t(std::max(1, ns)) * size_t(h) * 4, 0.0f);
            for (int y = 0; y < h; ++y)
                for (int i = 0; i < ns; ++i)
                    data[(size_t(y) * size_t(ns) + size_t(i)) * 4] = plan.scratchX[size_t(y) * size_t(ns) + size_t(i)];
            setData(data.data(), std::max(1, ns), h);
            QVector4D arr[64];
            for (size_t i = 0; i < plan.specks.size() && i < 48; ++i)
                arr[i] = QVector4D(plan.specks[i][0], plan.specks[i][1], plan.specks[i][2], plan.specks[i][3]);
            for (int i = 0; i < ns && i < 16; ++i)
                arr[48 + i] = QVector4D(float(plan.scratchDark[size_t(i)]), 0, 0, 0);
            single(OldFilm, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(plan.dx, plan.dy, plan.flicker, float(p.num(S("sepia")))));
                g.setUniformValue("uB", QVector4D(float(p.num(S("grain"))), float(p.num(S("vignette"))), float(plan.scratchWidth), float(ns)));
                setI(g, "uI", int(std::min<size_t>(48, plan.specks.size())));
                g.setUniformValueArray("uArr", arr, 64);
                setSeed(g, frame);
            });
        } else if (id == QLatin1String("interlace")) {
            FxState& st = fxStateFor(state);
            pushHistory(st, ctx, *layer, 2);
            single(Interlace, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", p.choice(S("mode")));
                g.setUniformValue("uA", QVector4D(float(p.num(S("shift")) * ctx.scale), 0, 0, 0));
            }, &ago(st, 1));
        } else if (id == QLatin1String("time_split")) {
            FxState& st = fxStateFor(state);
            const std::array<int, 3> delays = fx::timeSplitDelays(p);
            pushHistory(st, ctx, *layer, size_t(std::max({delays[0], delays[1], delays[2]})) + 1);
            Layer out = acquire(size, false);
            fxPass(*out, TimeSplit, ago(st, size_t(delays[0])), {}, &ago(st, size_t(delays[1])), &ago(st, size_t(delays[2])));
            layer = std::move(out);
        } else if (id == QLatin1String("slit_scan")) {
            FxState& st = fxStateFor(state);
            const int n = std::max(2, int(p.num(S("frames"))));
            pushHistory(st, ctx, *layer, size_t(n));
            Layer out = acquire(size, false);
            for (int k = 0; k < int(st.frames.size()); ++k)
                fxPass(*out, SlitScan, st.frames[size_t(k)], [&](QOpenGLShaderProgram& g) {
                    setI(g, "uI", p.choice(S("direction")), k, n, int(st.frames.size()));
                });
            layer = std::move(out);
        } else if (id == QLatin1String("trails") || id == QLatin1String("video_feedback")) {
            FxState& st = fxStateFor(state);
            pushHistory(st, ctx, *layer, 1);
            if (st.prevOut && st.prevOut.size() == size) {
                if (id == QLatin1String("trails")) {
                    single(Trails, [&](QOpenGLShaderProgram& g) {
                        g.setUniformValue("uA", QVector4D(float(p.num(S("decay"))), 0, 0, 0));
                        setI(g, "uI", p.choice(S("mode")));
                    }, &st.prevOut);
                } else {
                    const float rot = float(p.num(S("rotation"))) * kPi / 180.0f, hue = float(p.num(S("hueShift"))) * kPi / 180.0f;
                    single(Feedback, [&](QOpenGLShaderProgram& g) {
                        g.setUniformValue("uA", QVector4D(float(p.num(S("zoom"))), std::cos(-rot), std::sin(-rot), float(p.num(S("decay")))));
                        g.setUniformValue("uB", QVector4D(float(p.num(S("offsetX")) * ctx.scale), float(p.num(S("offsetY")) * ctx.scale), std::cos(hue),
                                                          std::sin(hue)));
                        g.setUniformValue("uC", QVector4D(float(p.num(S("input"))), 0, 0, 0));
                        setI(g, "uI", p.choice(S("mode")) == 1 ? 1 : 0);
                    }, &st.prevOut);
                }
            }
            copyInto(st.out, *layer);
        } else if (id == QLatin1String("stutter")) {
            FxState& st = fxStateFor(state);
            const double fps = std::max(0.1, p.num(S("fps")));
            const int64_t frameTick = int64_t(std::floor(t * fps + 1e-6));
            bool fresh = false;
            if (!st.out || st.out.size() != size) {
                st.frozenUntil = std::numeric_limits<int64_t>::min();
                fresh = true;
            } else if (frameTick != st.tick) {
                fresh = fx::stutterShowsNew(p, frameTick, st.tick, st.frozenUntil);
            }
            st.tick = frameTick;
            if (fresh) {
                copyInto(st.out, *layer);
            } else {
                Layer out = acquire(size, false);
                drawLayerReplace(*out, st.out);
                layer = std::move(out);
            }
        } else if (id == QLatin1String("datamosh")) {
            FxState& st = fxStateFor(state);
            const Step step = pushHistory(st, ctx, *layer, 2);
            const int64_t cleanTick = fx::datamoshCleanTick(p, ctx);
            const bool clean = p.flag(S("refresh")) || cleanTick != st.cleanTick;
            st.cleanTick = cleanTick;
            if (step == Step::Reset || clean || !st.prevOut || st.frames.size() < 2) {
                copyInto(st.out, *layer);
                st.vectorsValid = false;
                return true;
            }
            constexpr int f = 4;
            const int bs = std::max(2, int(std::lround(p.num(S("blockSize")) * ctx.scale / f)));
            const int range = std::max(1, int(std::lround(p.num(S("search")) * ctx.scale / f)));
            const bool bloom = p.choice(S("mode")) == 1;
            const int lw = std::max(1, w / f), lh = std::max(1, h / f);
            if (!bloom || !st.vectorsValid || st.blockPx != bs * f) {
                Layer cur = acquire(QSize(lw, lh), false), prv = acquire(QSize(lw, lh), false);
                fxPass(*cur, MoshLuma, *layer, [&](QOpenGLShaderProgram& g) { setI(g, "uI", f); });
                fxPass(*prv, MoshLuma, st.frames[1], [&](QOpenGLShaderProgram& g) { setI(g, "uI", f); });
                st.blocksX = (lw + bs - 1) / bs;
                st.blocksY = (lh + bs - 1) / bs;
                st.blockPx = bs * f;
                if (!st.vectors || st.vectors.size() != QSize(st.blocksX, st.blocksY)) {
                    deleteTarget(st.vectors);
                    st.vectors = createTarget(QSize(st.blocksX, st.blocksY), GL_RGBA32F);
                }
                fxPass(st.vectors, MoshMotion, *cur, [&](QOpenGLShaderProgram& g) {
                    setI(g, "uI", bs, range, lw, lh);
                    setI(g, "uJ", f);
                }, &*prv);
                st.vectorsValid = true;
            }
            single(MoshWarp, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", st.blockPx, st.blocksX, st.blocksY, int(float(p.num(S("amount"))) * 256.0f));
                g.setUniformValue("uA", QVector4D(float(p.num(S("motionScale"))), 0, 0, 0));
            }, &st.prevOut, &st.vectors);
            copyInto(st.out, *layer);
        } else if (id == QLatin1String("vhs")) {
            setRows(fx::vhsRows(p, ctx, w, h));
            const int blur = int(std::lround(p.num(S("blur")) * sc)), bleed = int(std::lround(p.num(S("bleed")) * sc));
            Layer enc = acquire(size, false, GL_RGBA32F), b1 = acquire(size, false, GL_RGBA32F), b2 = acquire(size, false, GL_RGBA32F);
            fxPass(*enc, VhsEncode, *layer, {});
            fxPass(*b1, BoxBlur, *enc, [&](QOpenGLShaderProgram& g) { setI(g, "uI", blur, bleed, bleed); });
            fxPass(*b2, BoxBlur, *b1, [&](QOpenGLShaderProgram& g) { setI(g, "uI", 0, bleed, bleed); });
            Layer out = acquire(size, false);
            fxPass(*out, VhsCompose, *b2, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(p.num(S("chromaShift"))) * sc, float(p.num(S("saturation"))), float(p.num(S("noise"))),
                                                  float(p.num(S("tracking")))));
                setSeed(g, p.flag(S("animated")) ? frame : 0u);
            });
            layer = std::move(out);
        } else if (id == QLatin1String("ntsc")) {
            const float strength = float(p.num(S("strength")));
            if (strength <= 0)
                return true;
            const float period = std::max(2.0f, float(w) / 190.0f);
            const int perR = std::max(1, int(period * 0.5f));
            const int bleed = std::max(perR, int(std::lround(p.num(S("bleed")) * ctx.scale)));
            const uint32_t nf = p.flag(S("dotCrawl")) ? frame : 0u;
            Layer enc = acquire(size, false, GL_RGBA32F), yb = acquire(size, false, GL_RGBA32F), dem = acquire(size, false, GL_RGBA32F);
            fxPass(*enc, NtscEncode, *layer, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(period, 0, 0, 0));
                setSeed(g, nf);
            });
            fxPass(*yb, BoxBlur, *enc, [&](QOpenGLShaderProgram& g) { setI(g, "uI", 0, perR, 0); });
            fxPass(*dem, NtscDemod, *yb, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(period, 0, 0, 0));
                setI(g, "uI", bleed);
                setSeed(g, nf);
            });
            Layer out = acquire(size, false);
            fxPass(*out, NtscCompose, *dem, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(0, float(p.num(S("ringing"))), strength, 0));
                setI(g, "uI", perR * 3);
            }, &*layer);
            layer = std::move(out);
        } else if (id == QLatin1String("dither")) {
            // core/Dither.cpp, the same picture: per-cell colour, then the palette pick (or error
            // diffusion, wave by wave, in integers like the CPU), then back to full size.
            const auto& palettes = dither::palettes();
            const auto& methods = dither::methods();
            const dither::Palette& palette = palettes[size_t(std::clamp(p.choice(S("palette")), 0, int(palettes.size()) - 1))];
            const dither::Method& method = methods[size_t(std::clamp(p.choice(S("method")), 0, int(methods.size()) - 1))];
            if (method.kind == dither::Kind::Riemersma)
                return false; // one path through the whole picture: the CPU does it
            const int cell = std::max(1, int(std::lround(p.num(S("pixelSize")) * ctx.scale)));
            const QSize small((w + cell - 1) / cell, (h + cell - 1) / cell);
            Layer avg = acquire(small, false, GL_RGBA32F);
            fxPass(*avg, DitherAverage, *layer, [&](QOpenGLShaderProgram& g) { setI(g, "uI", cell); });
            std::vector<uint32_t> pal = palette.colors;
            if (palette.adaptive) { // from the same cell colours as the CPU
                std::vector<float> cells(size_t(small.width()) * size_t(small.height()) * 4);
                glBindFramebuffer(GL_FRAMEBUFFER, avg->fbo);
                glReadPixels(0, 0, small.width(), small.height(), GL_RGBA, GL_FLOAT, cells.data());
                std::vector<float> rgb(cells.size() / 4 * 3);
                std::vector<uint8_t> alpha(cells.size() / 4);
                for (size_t k = 0; k < alpha.size(); ++k) {
                    rgb[k * 3] = cells[k * 4];
                    rgb[k * 3 + 1] = cells[k * 4 + 1];
                    rgb[k * 3 + 2] = cells[k * 4 + 2];
                    alpha[k] = uint8_t(cells[k * 4 + 3]);
                }
                pal = dither::adaptivePalette(rgb, alpha, palette.adaptive);
            }
            const int levels = palette.levels;
            const int count = levels ? levels * levels * levels : int(pal.size());
            QVector4D arr[64];
            for (size_t i = 0; i < pal.size() && i < 64; ++i)
                arr[i] = QVector4D(float((pal[i] >> 16) & 0xff), float((pal[i] >> 8) & 0xff), float(pal[i] & 0xff), 0);
            const double strength = p.num(S("strength"));
            int ox = 0, oy = 0;
            if (p.flag(S("animate")))
                dither::noiseOffset(frame, ox, oy);
            if (method.kind == dither::Kind::Diffusion) {
                const int slope = dither::waveSlope(method);
                int maxLag = 1;
                for (const dither::Tap& t : method.taps)
                    maxLag = std::max(maxLag, dither::tapLag(t, slope));
                const int ringSize = maxLag + 1; // (at most 9)
                Layer sums = acquire(small, false, GL_RGBA32F), all = acquire(small, false, GL_RGBA32F), pick = acquire(small, false, GL_RGBA32F);
                std::vector<Layer> ring;
                for (int i = 0; i < ringSize; ++i)
                    ring.push_back(acquire(small, false, GL_RGBA32F));
                fxPass(*sums, DitherSums, *layer, [&](QOpenGLShaderProgram& g) { setI(g, "uI", cell); });
                GLint pal16[64 * 3] = {};
                for (size_t i = 0; i < pal.size() && i < 64; ++i) {
                    pal16[i * 3] = GLint((pal[i] >> 16) & 0xff) * 16;
                    pal16[i * 3 + 1] = GLint((pal[i] >> 8) & 0xff) * 16;
                    pal16[i * 3 + 2] = GLint(pal[i] & 0xff) * 16;
                }
                GLint taps[16 * 4] = {};
                for (size_t i = 0; i < method.taps.size() && i < 16; ++i) {
                    const dither::Tap& t = method.taps[i];
                    taps[i * 4] = t.dx;
                    taps[i * 4 + 1] = t.dy;
                    taps[i * 4 + 2] = t.weight;
                    taps[i * 4 + 3] = dither::tapLag(t, slope);
                }
                QOpenGLShaderProgram& diffuse = *progDiffuse;
                diffuse.bind();
                bindTexture(0, sums->tex);
                diffuse.setUniformValue("uSums", 0);
                for (int k = 1; k <= 8; ++k)
                    diffuse.setUniformValue(QStringLiteral("uPrev%1").arg(k).toLatin1().constData(), k);
                glUniform4iv(diffuse.uniformLocation("uTaps"), 16, taps);
                diffuse.setUniformValue("uTapCount", int(std::min<size_t>(16, method.taps.size())));
                diffuse.setUniformValue("uDivisor", method.divisor);
                diffuse.setUniformValue("uStrength", int(std::lround(strength * 256.0)));
                diffuse.setUniformValue("uSlope", slope);
                glUniform2i(diffuse.uniformLocation("uDims"), small.width(), small.height());
                diffuse.setUniformValue("uCount", int(std::min<size_t>(64, pal.size())));
                diffuse.setUniformValue("uLevels", levels);
                glUniform3iv(diffuse.uniformLocation("uPal"), 64, pal16);
                glDisable(GL_BLEND);
                glBindVertexArray(vao);
                // One framebuffer per slot: colour 0 = the slot, colour 1 = the whole picture.
                std::vector<GLuint> fbos(static_cast<size_t>(ringSize));
                glGenFramebuffers(ringSize, fbos.data());
                const GLenum buffers[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
                for (int i = 0; i < ringSize; ++i) {
                    glBindFramebuffer(GL_FRAMEBUFFER, fbos[size_t(i)]);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ring[size_t(i)]->tex, 0);
                    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, all->tex, 0);
                    glDrawBuffers(2, buffers);
                }
                glViewport(0, 0, small.width(), small.height());
                const GLint uT = diffuse.uniformLocation("uT");
                const int waves = (small.width() - 1) + slope * (small.height() - 1) + 1;
                for (int t = 0; t < waves; ++t) {
                    // Wave t - k on unit k (lags beyond the ring are never read: any slot but this wave's will do).
                    for (int k = 1; k <= 8; ++k)
                        bindTexture(k, ring[size_t(((t - std::min(k, ringSize - 1)) % ringSize + ringSize) % ringSize)]->tex);
                    glUniform1i(uT, t);
                    glBindFramebuffer(GL_FRAMEBUFFER, fbos[size_t(t % ringSize)]);
                    glDrawArrays(GL_POINTS, 0, small.height());
                }
                glDeleteFramebuffers(ringSize, fbos.data());
                fxPass(*pick, DitherResolve, *all, [&](QOpenGLShaderProgram& g) {
                    setI(g, "uI", cell, count, levels);
                    g.setUniformValueArray("uArr", arr, 64);
                }, &*sums, nullptr, size);
                single(DitherExpand, [&](QOpenGLShaderProgram& g) { setI(g, "uI", cell); }, &*pick);
                return true;
            }
            // Ordered, noise and pattern methods: one pick per cell.
            const float spread = float(strength) * (levels ? 255.0f / float(levels - 1) : 255.0f / std::max(1.5f, std::cbrt(float(count))));
            int kind = 0, mapN = 1;
            if (method.kind == dither::Kind::Map || method.kind == dither::Kind::Pattern) {
                const dither::ThresholdMap& map = dither::thresholdMap(method.map);
                std::vector<float> data(size_t(map.n) * size_t(map.n) * 4, 0.0f);
                for (size_t i = 0; i < map.values.size(); ++i)
                    data[i * 4] = map.values[i];
                setData(data.data(), map.n, map.n);
                mapN = map.n;
                kind = method.kind == dither::Kind::Map ? 1 : 4;
                if (method.kind == dither::Kind::Pattern || method.map != 11)
                    ox = oy = 0; // only the noises move
            } else if (method.kind == dither::Kind::WhiteNoise) {
                kind = 2;
            } else if (method.kind == dither::Kind::GradientNoise) {
                kind = 3;
            }
            const bool perceptual = p.choice(S("match")) == 1 && !levels;
            Layer picked = acquire(small, false, GL_RGBA32F);
            fxPass(*picked, DitherMap, *avg, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", kind, int(std::min<size_t>(64, pal.size())), levels, mapN);
                setI(g, "uJ", ox, oy, perceptual ? 1 : 0, mapN * mapN);
                g.setUniformValue("uA", QVector4D(spread, float(std::clamp(strength, 0.0, 2.0) / 2.0 + 0.5), 0, 0));
                g.setUniformValueArray("uArr", arr, 64);
            });
            single(DitherExpand, [&](QOpenGLShaderProgram& g) { setI(g, "uI", cell); }, &*picked);
        } else if (id == QLatin1String("ascii")) {
            FxState& st = fxStateFor(state);
            const int cell = std::max(3, int(std::lround(p.num(S("cellSize")) * ctx.scale)));
            const int charset = std::clamp(p.choice(S("charset")), 0, 3);
            if (!st.atlas.tex || st.atlasCell != cell || st.atlasCharset != charset) {
                const std::vector<QImage> glyphs = fx::asciiGlyphs(cell, charset);
                deleteTarget(st.atlas);
                st.atlas.w = cell * int(glyphs.size());
                st.atlas.h = cell;
                st.atlas.tex = makeTexture(st.atlas.w, st.atlas.h, GL_R8, GL_NEAREST);
                std::vector<uint8_t> pixels(size_t(st.atlas.w) * size_t(cell));
                for (size_t gi = 0; gi < glyphs.size(); ++gi)
                    for (int y = 0; y < cell; ++y)
                        std::copy_n(glyphs[gi].constScanLine(y), cell, pixels.begin() + std::ptrdiff_t(size_t(y) * size_t(st.atlas.w) + gi * size_t(cell)));
                glBindTexture(GL_TEXTURE_2D, st.atlas.tex);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, st.atlas.w, cell, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
                st.atlasCell = cell;
                st.atlasCharset = charset;
                st.atlasGlyphs = int(glyphs.size());
            }
            Layer avg = acquire(QSize((w + cell - 1) / cell, (h + cell - 1) / cell), false, GL_RGBA32F);
            fxPass(*avg, AsciiAverage, *layer, [&](QOpenGLShaderProgram& g) { setI(g, "uI", cell); });
            single(AsciiCompose, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", cell, st.atlasGlyphs, p.choice(S("colorMode")));
                g.setUniformValue("uA", QVector4D(float(p.num(S("background"))), 0, 0, 0));
            }, &*avg, &st.atlas);
        } else if (id == QLatin1String("neon_edges")) {
            Layer edges = acquire(size, false);
            fxPass(*edges, NeonEdges, *layer, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(p.num(S("strength"))), float(t * 0.1), 0, 0));
                setI(g, "uI", p.choice(S("colorMode")));
                g.setUniformValue("uB", bytes(p.color(S("color"))));
            });
            const double glowR = p.num(S("glow")) * ctx.scale;
            Layer glow;
            if (glowR >= 0.5) {
                glow = acquire(size, false);
                drawLayerReplace(*glow, *edges);
                blur(glow, glowR, glowR);
            }
            single(NeonCombine, [&](QOpenGLShaderProgram& g) {
                g.setUniformValue("uA", QVector4D(float(p.num(S("background"))), 0, 0, 0));
                setI(g, "uI", glow ? 1 : 0);
            }, &*edges, glow ? &*glow : nullptr);
        } else if (id == QLatin1String("pixel_sort")) {
            // Each line is sorted as a whole with a bitonic network on keys
            // (segment start, key, original position): runs sort in place,
            // pixels outside runs stay put, and ties keep their order.
            const bool vertical = p.choice(S("direction")) == 1;
            const int len = vertical ? h : w, lines = vertical ? w : h;
            int n = 1;
            while (n < len)
                n <<= 1;
            const QSize work(n, lines);
            Layer a = acquire(work, false, GL_RG32F), b = acquire(work, false, GL_RG32F);
            fxPass(*a, PixelSortInit, *layer, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", vertical ? 1 : 0, len, p.choice(S("sortBy")), p.choice(S("thresholdBy")));
                setI(g, "uJ", p.flag(S("invert")) ? 1 : 0, p.flag(S("reverse")) ? 1 : 0);
                g.setUniformValue("uA", QVector4D(float(p.num(S("lower"))), float(p.num(S("upper"))), 0, 0));
                g.setUniformValue("uB", QVector4D(float(p.num(S("breaks"))), 0, 0, 0));
                setSeed(g, fx::pixelSortBreakSeed(p, ctx));
            });
            for (int step = 1; step < n; step <<= 1) {
                fxPass(*b, PixelSortScan, *a, [&](QOpenGLShaderProgram& g) { setI(g, "uI", step); });
                std::swap(a, b);
            }
            fxPass(*b, PixelSortKeys, *a, [&](QOpenGLShaderProgram& g) {
                setI(g, "uI", std::max(0, int(p.num(S("maxLength")) * ctx.scale)), len);
            });
            std::swap(a, b);
            for (int k = 2; k <= n; k <<= 1)
                for (int j = k >> 1; j > 0; j >>= 1) {
                    fxPass(*b, PixelSortStep, *a, [&](QOpenGLShaderProgram& g) { setI(g, "uI", j, k); });
                    std::swap(a, b);
                }
            single(PixelSortOutput, [&](QOpenGLShaderProgram& g) { setI(g, "uI", vertical ? 1 : 0); }, &*a);
        } else if (id == QLatin1String("jpeg_glitch")) {
            const int quality = std::clamp(int(p.num(S("quality"))), 1, 100);
            const float qscale = quality < 50 ? 5000.0f / quality : 200.0f - 2.0f * quality;
            const int cq = int(p.num(S("chromaQuality"))) > 0 ? std::clamp(int(p.num(S("chromaQuality"))), 1, 100) : quality;
            const float qscaleC = cq < 50 ? 5000.0f / cq : 200.0f - 2.0f * cq;
            const int target = p.choice(S("corruptWhat"));
            const int n = fx::jpegBlockSize(p, ctx);
            const float corruption = float(p.num(S("corruption")));
            const uint32_t seed = p.flag(S("animated")) ? frame * 31u + 5u : 5u;
            const bool sub = p.flag(S("subsample"));
            auto padded = [n](QSize s) { return QSize((s.width() + n - 1) / n * n, (s.height() + n - 1) / n * n); };
            auto dctUniforms = [&](int mask, int lumaFirst) {
                return [&, mask, lumaFirst](QOpenGLShaderProgram& g) {
                    setI(g, "uI", n, mask, lumaFirst, target);
                    g.setUniformValue("uA", QVector4D(qscale, corruption, qscaleC, 0));
                    g.setUniformValueArray("uQ0", fx::jpegQuantTable(false), 64);
                    g.setUniformValueArray("uQ1", fx::jpegQuantTable(true), 64);
                    setSeed(g, seed);
                };
            };
            Layer ycc = acquire(size, false, GL_RGBA32F);
            fxPass(*ycc, JpegToYcc, *layer, {});
            Layer coef = acquire(padded(size), false, GL_RGBA32F), full = acquire(size, false, GL_RGBA32F);
            fxPass(*coef, JpegForward, *ycc, dctUniforms(sub ? 1 : 7, 1));
            fxPass(*full, JpegInverse, *coef, [&](QOpenGLShaderProgram& g) { setI(g, "uI", n); }, nullptr, nullptr, size);
            Layer half;
            if (sub) {
                const QSize hs((w + 1) / 2, (h + 1) / 2);
                Layer hp = acquire(hs, false, GL_RGBA32F), hc = acquire(padded(hs), false, GL_RGBA32F);
                half = acquire(hs, false, GL_RGBA32F);
                fxPass(*hp, JpegHalfChroma, *ycc, {});
                fxPass(*hc, JpegForward, *hp, dctUniforms(6, 0));
                fxPass(*half, JpegInverse, *hc, [&](QOpenGLShaderProgram& g) { setI(g, "uI", n); }, nullptr, nullptr, hs);
            }
            single(JpegCompose, [&](QOpenGLShaderProgram& g) { setI(g, "uI", sub ? 1 : 0); }, &*full, half ? &*half : nullptr);
        } else {
            return false;
        }
        return true;
    }

    // Shader versions of the stateless glitch / psychedelic effects (the
    // formulas mirror GlitchEffects.cpp). Returns false if `id` has none.
    // ---- Broken video game systems (ConsoleEngine.h, GpuConsoleShaders.h) ----

    void consolePass(const Target& dst, int pass, const Target& src, const Target* alt, const Target* cells, const Target* over,
                     const vg::Plan& plan, int ghost = 0)
    {
        QOpenGLShaderProgram& prog = *progConsole;
        prog.bind();
        bindTexture(0, src.tex);
        bindTexture(1, alt && alt->tex ? alt->tex : src.tex);
        bindTexture(2, cells && cells->tex ? cells->tex : emptyTex);
        bindTexture(3, dataTex ? dataTex : emptyTex);
        bindTexture(4, over && over->tex ? over->tex : emptyTex);
        prog.setUniformValue("uTex", 0);
        prog.setUniformValue("uTex1", 1);
        prog.setUniformValue("uCells", 2);
        prog.setUniformValue("uData", 3);
        prog.setUniformValue("uOver", 4);
        prog.setUniformValue("uPass", pass);
        prog.setUniformValue("uGhost", ghost);
        glUniform1fv(prog.uniformLocation("uG"), GLsizei(vg::kGlobalCount), plan.g.data());
        fullscreen(prog, dst);
    }

    void drawConsoleMesh(const Target& dst, const Target& src, const std::vector<vg::MeshVertex>& mesh)
    {
        if (!meshVao) {
            glGenVertexArrays(1, &meshVao);
            glBindVertexArray(meshVao);
            glGenBuffers(1, &meshVbo);
            glBindBuffer(GL_ARRAY_BUFFER, meshVbo);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, GLsizei(sizeof(vg::MeshVertex)), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, GLsizei(sizeof(vg::MeshVertex)), reinterpret_cast<const void*>(2 * sizeof(float)));
        }
        glBindVertexArray(meshVao);
        glBindBuffer(GL_ARRAY_BUFFER, meshVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(vg::MeshVertex)), mesh.data(), GL_STREAM_DRAW);
        progVgMesh->bind();
        bindTexture(0, src.tex);
        progVgMesh->setUniformValue("uTex", 0);
        progVgMesh->setUniformValue("uSize", QVector2D(float(src.w), float(src.h)));
        glBindFramebuffer(GL_FRAMEBUFFER, dst.fbo);
        glViewport(0, 0, dst.w, dst.h);
        glDisable(GL_BLEND);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(mesh.size()));
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
    }

    bool consoleEffect(const QString& id, const Params& p, Layer& layer, const RenderContext& ctx, std::unique_ptr<EffectState>& state)
    {
        if (!vg::isConsoleEffect(id))
            return false;
        const QSize size = layer->size();
        const vg::Plan plan = vg::buildPlan(id, p, ctx, size);
        if (plan.data.empty())
            return true;
        if (!plan.mesh.empty()) {
            Layer out = acquire(size, false);
            const uint32_t bd = uint32_t(int(plan[vg::G_BACKDROP])) & 0xffffffu;
            clear(*out, float((bd >> 16) & 255) / 255.0f, float((bd >> 8) & 255) / 255.0f, float(bd & 255) / 255.0f, 1.0f);
            drawConsoleMesh(*out, *layer, plan.mesh);
            layer = std::move(out);
        }
        setData(plan.data.data(), plan.dataW, plan.dataH);
        Layer frame;
        const Target* cur = layer.get();
        if (plan.lookFull()) {
            frame = acquire(QSize(plan.cw(), plan.ch()), false, GL_RGBA32F);
            consolePass(*frame, 0, *layer, nullptr, nullptr, nullptr, plan);
            cur = frame.get();
        }
        FxState* st = nullptr;
        const Target* alt = cur;
        if (plan.usesHistory()) {
            st = &fxStateFor(state);
            pushHistory(*st, ctx, *cur, 2);
            if (plan.freezeKey >= 0) {
                if (st->heldKey != plan.freezeKey || !st->held || st->held.size() != cur->size() || st->held.fmt != cur->fmt) {
                    copyInto(st->held, *cur);
                    st->heldKey = plan.freezeKey;
                }
                alt = &st->held;
            } else if (plan.needPrev) {
                alt = &ago(*st, 1);
            }
        }
        Layer cells, over;
        if (plan[vg::G_CELLS] > 0.5f && plan.lookFull()) {
            cells = acquire(QSize((plan.cw() + 7) / 8, (plan.ch() + 7) / 8), false, GL_RGBA32F);
            consolePass(*cells, 1, *cur, nullptr, nullptr, nullptr, plan);
        }
        if (plan[vg::G_OVERLAY] > 0.5f && !plan.overlay.isNull()) {
            over = acquire(plan.overlay.size(), false);
            upload(*over, plan.overlay);
        }
        Layer out = acquire(size, false);
        consolePass(*out, 2, *cur, alt, cells.get(), over.get(), plan);
        if (plan.ghost > 0 && st) {
            if (st->prevOut && st->prevOut.size() == size && st->prevOut.fmt == GL_RGBA8) {
                Layer blended = acquire(size, false);
                consolePass(*blended, 3, *out, &st->prevOut, nullptr, nullptr, plan, std::clamp(int(plan.ghost * 256.0f + 0.5f), 0, 256));
                out = std::move(blended);
            }
            copyInto(st->out, *out);
        }
        layer = std::move(out);
        return true;
    }


    bool glitchEffect(const QString& id, const Params& p, Layer& layer, const RenderContext& ctx)
    {
        const QSize size = layer->size();
        const float w = float(size.width()), h = float(size.height());
        const float sc = float(ctx.scale);
        const double t = double(ctx.timelineTime) / double(kUsPerSecond);
        constexpr float kPi = 3.14159265358979f;
        auto S = [](const char* s) { return QString::fromLatin1(s); };
        auto warp = [&](int mode, int edge, QVector4D a, QVector4D b = {}) {
            filterPass(layer, *progWarp, [&] {
                progWarp->setUniformValue("uSize", QVector2D(w, h));
                progWarp->setUniformValue("uMode", mode);
                progWarp->setUniformValue("uEdge", edge);
                progWarp->setUniformValue("uA", a);
                progWarp->setUniformValue("uB", b);
                progWarp->setUniformValue("uFrame", GLint(std::llround(double(ctx.timelineTime) * ctx.fps / double(kUsPerSecond))));
            });
        };
        if (id == QLatin1String("wave_warp")) {
            const float amp = float(p.num(S("amplitude")) * ctx.scale);
            if (amp == 0)
                return true;
            warp(0, 2, QVector4D(float(p.choice(S("direction"))), amp, float(std::max(1.0, p.num(S("wavelength")) * ctx.scale)),
                                 float(p.num(S("speed")) * t)));
        } else if (id == QLatin1String("twirl")) {
            const float angle = float(p.num(S("angle"))) * kPi / 180.0f, radius = float(p.num(S("radius"))) * h * 0.5f;
            if (angle == 0 || radius <= 0)
                return true;
            warp(1, 0, QVector4D(angle, radius, float(p.num(S("centerX"))) * w, float(p.num(S("centerY"))) * h));
        } else if (id == QLatin1String("ripple")) {
            warp(2, 2, QVector4D(float(p.num(S("amplitude")) * ctx.scale), float(std::max(1.0, p.num(S("wavelength")) * ctx.scale)),
                                 float(p.num(S("speed")) * t), float(p.num(S("decay")))),
                 QVector4D(float(p.num(S("centerX"))) * w, float(p.num(S("centerY"))) * h, 0.5f * std::hypot(w, h), 0));
        } else if (id == QLatin1String("lens_distortion")) {
            warp(3, 3, QVector4D(float(p.num(S("amount"))), float(p.num(S("zoom"))), 0.5f * std::hypot(w, h), 0));
        } else if (id == QLatin1String("kaleidoscope")) {
            const float rot = float((p.num(S("rotation")) + p.num(S("spin")) * t) * kPi / 180.0);
            warp(4, 2, QVector4D(float(std::max(2, int(p.num(S("segments"))))), rot, float(p.num(S("zoom"))), 0),
                 QVector4D(float(p.num(S("centerX"))) * w, float(p.num(S("centerY"))) * h, 0, 0));
        } else if (id == QLatin1String("mirror_symmetry")) {
            warp(5, 0, QVector4D(float(p.choice(S("mode"))), 0, 0, 0));
        } else if (id == QLatin1String("polar")) {
            warp(6, 1, QVector4D(p.choice(S("mode")) == 0 ? 1.0f : 0.0f, std::min(w, h) * 0.5f, 0, 0));
        } else if (id == QLatin1String("tile")) {
            warp(7, p.flag(S("mirror")) ? 2 : 1,
                 QVector4D(float(std::max(1.0, p.num(S("tilesX")))), float(std::max(1.0, p.num(S("tilesY")))), float(p.num(S("scrollX")) * t),
                           float(p.num(S("scrollY")) * t)));
        } else if (id == QLatin1String("shake")) {
            const double tt = t * p.num(S("speed"));
            const uint32_t seed = uint32_t(p.num(S("seed"))) * 7u + 3u;
            auto noise1 = [](double x, uint32_t sd) {
                auto hash01 = [](uint32_t a, uint32_t b, uint32_t c) {
                    uint32_t hh = a * 374761393u + b * 668265263u + c * 2246822519u;
                    hh = (hh ^ (hh >> 13)) * 1274126177u;
                    return float((hh ^ (hh >> 16)) & 0xffffffu) / float(0x1000000);
                };
                const double i = std::floor(x);
                const float f = float(x - i), u = f * f * (3.0f - 2.0f * f);
                const float a = hash01(uint32_t(int64_t(i)), 0, sd) * 2 - 1, b = hash01(uint32_t(int64_t(i) + 1), 0, sd) * 2 - 1;
                return a + (b - a) * u;
            };
            const float amt = float(p.num(S("amount")) * ctx.scale);
            const float rot = float(p.num(S("rotation"))) * kPi / 180.0f * noise1(tt, seed + 2);
            warp(8, 2, QVector4D(amt * noise1(tt, seed), amt * noise1(tt, seed + 1), std::cos(-rot), std::sin(-rot)),
                 QVector4D(float(p.num(S("zoom"))), 0, 0, 0));
        } else if (id == QLatin1String("melt")) {
            const float amount = float(p.num(S("amount")) * ctx.scale);
            if (amount == 0)
                return true;
            warp(9, 0, QVector4D(amount, float(p.num(S("frequency"))), float(p.num(S("speed")) * t), p.choice(S("direction")) == 1 ? 1.0f : 0.0f));
        } else if (id == QLatin1String("signal_jitter")) {
            const double rollRate = p.num(S("roll"));
            const double rollF = t * rollRate - std::floor(t * rollRate);
            warp(10, 1,
                 QVector4D(float(p.num(S("amplitude"))) * sc, float(p.num(S("frequency"))), float(t * p.num(S("speed"))), float(p.num(S("noise"))) * sc),
                 QVector4D(float(rollF) * h, rollRate != 0 ? float(std::max(2, int(h) / 25)) : 0.0f, 0, 0));
        } else if (id == QLatin1String("chromatic_aberration") || id == QLatin1String("rgb_split")) {
            QVector2D r, g, b, center(w * 0.5f, h * 0.5f);
            int radial = 0, edge = 0;
            float k = 0, falloff = 1, gs = 0;
            const float rmax = 0.5f * std::hypot(w, h);
            if (id == QLatin1String("rgb_split")) {
                const std::array<float, 6> o = fx::rgbSplitOffsets(p, ctx);
                r = QVector2D(o[0], o[1]);
                g = QVector2D(o[2], o[3]);
                b = QVector2D(o[4], o[5]);
                edge = std::clamp(p.choice(S("edge")), 0, 3);
            } else {
                const float amount = fx::chromaAmount(p, ctx);
                if (amount == 0)
                    return true;
                radial = p.choice(S("mode")) == 0 ? 1 : 0;
                k = amount / rmax;
                center = QVector2D(float(p.num(S("centerX"))) * w, float(p.num(S("centerY"))) * h);
                falloff = float(p.num(S("falloff")));
                gs = float(p.num(S("greenShift")));
                const float ang = float(p.num(S("angle"))) * kPi / 180.0f;
                r = -QVector2D(std::cos(ang), std::sin(ang)) * amount; // red sampled at p + offset
                b = -r;
                g = r * gs;
            }
            filterPass(layer, *progChroma, [&] {
                progChroma->setUniformValue("uSize", QVector2D(w, h));
                progChroma->setUniformValue("uRadial", radial);
                progChroma->setUniformValue("uK", k);
                progChroma->setUniformValue("uCenter", center);
                progChroma->setUniformValue("uFalloff", falloff);
                progChroma->setUniformValue("uGs", gs);
                progChroma->setUniformValue("uRMax", rmax);
                progChroma->setUniformValue("uEdge", edge);
                progChroma->setUniformValue("uR", r);
                progChroma->setUniformValue("uG", g);
                progChroma->setUniformValue("uB", b);
            });
        } else if (id == QLatin1String("radial_blur")) {
            const bool spin = p.choice(S("mode")) == 1;
            const float amount = float(p.num(S("amount")));
            if (amount <= 0)
                return true;
            const QVector2D center(float(p.num(S("centerX"))) * w, float(p.num(S("centerY"))) * h);
            const float radius = std::hypot(w, h) * 0.5f;
            const float spread = spin ? amount * kPi / 180.0f * radius : amount * radius;
            const int passes = std::clamp(int(std::ceil(std::log2(std::max(2.0f, spread / 1.5f)))), 1, 8);
            for (int k = 0; k < passes; ++k) {
                const float frac = 1.0f / float(2 << k);
                QVector4D pp;
                if (spin) {
                    const float a = amount * kPi / 180.0f * frac * 0.5f;
                    pp = QVector4D(std::cos(a), std::sin(a), 0, 0);
                } else {
                    const float zin = std::pow(1.0f + amount, frac * 0.5f);
                    pp = QVector4D(zin, 1.0f / zin, 0, 0);
                }
                filterPass(layer, *progRadialPass, [&] {
                    progRadialPass->setUniformValue("uSize", QVector2D(w, h));
                    progRadialPass->setUniformValue("uCenter", center);
                    progRadialPass->setUniformValue("uSpin", spin ? 1 : 0);
                    progRadialPass->setUniformValue("uP", pp);
                });
            }
        } else if (id == QLatin1String("crt")) {
            filterPass(layer, *progCrt, [&] {
                progCrt->setUniformValue("uSize", QVector2D(w, h));
                progCrt->setUniformValue("uA", QVector4D(float(p.num(S("scanlines"))), float(p.num(S("lineCount"))), float(p.num(S("curvature"))),
                                                         float(p.num(S("vignette")))));
                progCrt->setUniformValue("uB", QVector4D(float(p.num(S("brightness"))), float(p.choice(S("mask"))),
                                                         1.0f - float(p.num(S("maskStrength"))), float(std::max(1, int(std::lround(3 * ctx.scale))))));
            });
        } else if (id == QLatin1String("psychedelic")) {
            filterPass(layer, *progPsy, [&] {
                progPsy->setUniformValue("uSize", QVector2D(w, h));
                progPsy->setUniformValue("uA", QVector4D(float(p.num(S("speed")) * t), float(p.num(S("lumaSpread"))), float(p.num(S("positionSpread"))),
                                                         float(p.num(S("saturation")))));
            });
        } else {
            return false;
        }
        return true;
    }

    void drawLayerReplace(const Target& dst, const Target& src)
    {
        drawTexture(dst, src.tex, QSizeF(src.w, src.h), QTransform(), 1.0, QRectF(0, 0, 1, 1), false);
    }

    void transformEffect(Layer& layer, const Params& p, const RenderContext& ctx)
    {
        const double scale = p.num(QStringLiteral("scale"));
        const double x = p.num(QStringLiteral("x")), y = p.num(QStringLiteral("y"));
        const double rot = p.num(QStringLiteral("rotation"));
        const double cl = p.num(QStringLiteral("cropLeft")), cr = p.num(QStringLiteral("cropRight"));
        const double ct = p.num(QStringLiteral("cropTop")), cb = p.num(QStringLiteral("cropBottom"));
        const double opacity = p.num(QStringLiteral("opacity"));
        const double border = p.num(QStringLiteral("border")) * ctx.scale;
        if (scale == 1.0 && x == 0 && y == 0 && rot == 0 && cl + cr + ct + cb == 0 && opacity >= 1.0 && border <= 0)
            return;
        const double W = layer->w, H = layer->h;
        const QRectF content = ctx.contentRect.isEmpty() ? QRectF(0, 0, W, H) : ctx.contentRect;
        const QRectF crop(content.left() + cl * content.width(), content.top() + ct * content.height(),
                          std::max(1.0, content.width() * (1.0 - cl - cr)), std::max(1.0, content.height() * (1.0 - ct - cb)));
        QTransform t;
        t.translate(W / 2 + x * W, H / 2 + y * H);
        t.rotate(rot);
        t.scale(scale, scale);
        t.translate(-W / 2, -H / 2);
        Layer out = acquire(layer->size());
        if (scale < 0.7)
            ensureMipmaps(*layer);
        drawTexture(*out, layer->tex, crop.size(), QTransform::fromTranslate(crop.x(), crop.y()) * t, opacity,
                    QRectF(crop.x() / W, crop.y() / H, crop.width() / W, crop.height() / H));
        if (border > 0) {
            const double pw = border / std::max(0.01, scale);
            const QColor c = p.color(QStringLiteral("borderColor"));
            const QRectF o = crop.adjusted(-pw, -pw, pw, pw);
            const QRectF parts[] = {QRectF(o.left(), o.top(), o.width(), pw), QRectF(o.left(), crop.bottom(), o.width(), pw),
                                    QRectF(o.left(), crop.top(), pw, crop.height()), QRectF(crop.right(), crop.top(), pw, crop.height())};
            for (const QRectF& part : parts)
                solid(*out, QTransform::fromTranslate(part.x(), part.y()) * t, part.size(), c, opacity);
        }
        layer = std::move(out);
    }

    // ---- Transitions ---------------------------------------------------------------------

    void transitionShader(const Target& dst, const Target* from, const Target* to, int mode, double p, QVector4D a,
                          QVector4D b, QColor color)
    {
        progTransition->bind();
        bindTexture(0, from ? from->tex : emptyTex);
        bindTexture(1, to ? to->tex : emptyTex);
        progTransition->setUniformValue("uFrom", 0);
        progTransition->setUniformValue("uTo", 1);
        progTransition->setUniformValue("uMode", mode);
        progTransition->setUniformValue("uP", float(std::clamp(p, 0.0, 1.0)));
        progTransition->setUniformValue("uA", a);
        progTransition->setUniformValue("uB", b);
        progTransition->setUniformValue("uSize", QVector2D(float(dst.w), float(dst.h)));
        progTransition->setUniformValue("uColor", QVector4D(float(color.redF()), float(color.greenF()), float(color.blueF()), 1.0f));
        fullscreen(*progTransition, dst);
    }

    // The per-pixel transitions (TransitionKernels.cpp), same uniforms as the CPU kernel.
    void kernelTransition(const Target& dst, const Target* from, const Target* to, const tfx::Uniforms& u)
    {
        progTransitionFx->bind();
        bindTexture(0, from ? from->tex : emptyTex);
        bindTexture(1, to ? to->tex : emptyTex);
        progTransitionFx->setUniformValue("uFrom", 0);
        progTransitionFx->setUniformValue("uTo", 1);
        progTransitionFx->setUniformValue("uMode", u.mode);
        progTransitionFx->setUniformValue("uP", u.p);
        progTransitionFx->setUniformValue("uSize", QVector2D(u.w, u.h));
        progTransitionFx->setUniformValue("uA", QVector4D(u.a[0], u.a[1], u.a[2], u.a[3]));
        progTransitionFx->setUniformValue("uB", QVector4D(u.b[0], u.b[1], u.b[2], u.b[3]));
        progTransitionFx->setUniformValue("uColor", QVector4D(u.color[0], u.color[1], u.color[2], u.color[3]));
        progTransitionFx->setUniformValue("uColor2", QVector4D(u.color2[0], u.color2[1], u.color2[2], u.color2[3]));
        fullscreen(*progTransitionFx, dst);
    }

    void applyTransition(const TransitionSpec& spec, const Target& dst, const Target* from, const Target* to, double p,
                         double opacity, const RenderContext& ctx)
    {
        const TransitionDef* def = findTransition(spec.type);
        if (!def)
            def = findTransition(QStringLiteral("dissolve"));
        const Params prm = resolveParams(def->params, spec.params, ctx.timelineTime);
        p = std::clamp(p, 0.0, 1.0);
        const QSize size(dst.w, dst.h);
        Layer out = acquire(size);
        const QString& id = def->id;
        const float w = float(size.width()), h = float(size.height());
        tfx::Uniforms kernel;
        if (tfx::setup(id, prm, p, size, ctx.scale, kernel)) {
            kernelTransition(*out, from, to, kernel);
        } else if (id == QLatin1String("slide") || id == QLatin1String("push")) {
            const int dir = prm.choice(QStringLiteral("direction"));
            const double e = ease(p);
            // Slide: the current picture stays put; push: it moves out.
            const QPointF fromOff = id == QLatin1String("push") ? -directionOffset(dir, e, size) : QPointF();
            if (from)
                drawTexture(*out, from->tex, QSizeF(size), QTransform::fromTranslate(fromOff.x(), fromOff.y()), 1.0);
            if (to) {
                const QPointF off = directionOffset(dir, 1.0 - e, size);
                drawTexture(*out, to->tex, QSizeF(size), QTransform::fromTranslate(off.x(), off.y()), 1.0);
            }
        } else if (id == QLatin1String("cross_zoom")) {
            const double amount = prm.num(QStringLiteral("amount"));
            const double e = ease(p);
            auto zoomed = [&](const Target* img, double s, double o) {
                if (!img || o <= 0)
                    return;
                QTransform t;
                t.translate(w / 2.0, h / 2.0);
                t.scale(s, s);
                t.translate(-w / 2.0, -h / 2.0);
                drawTexture(*out, img->tex, QSizeF(size), t, o);
            };
            zoomed(from, 1.0 + e * amount, 1.0);
            zoomed(to, 1.0 + (1.0 - e) * amount, e);
        } else if (id == QLatin1String("dip")) {
            transitionShader(*out, from, to, 1, p, {}, {}, prm.color(QStringLiteral("color")));
        } else if (id == QLatin1String("wipe")) {
            const float ang = float(prm.num(QStringLiteral("angle")) * 3.14159265 / 180.0);
            const float c = std::cos(ang), s = std::sin(ang);
            const float extent = (std::abs(c) * w + std::abs(s) * h) / 2.0f;
            transitionShader(*out, from, to, 2, p, QVector4D(c, s, extent, float(prm.num(QStringLiteral("softness")))), {}, {});
        } else if (id == QLatin1String("iris")) {
            const int shape = prm.choice(QStringLiteral("shape"));
            const float cx = float(prm.num(QStringLiteral("centerX"))) * w, cy = float(prm.num(QStringLiteral("centerY"))) * h;
            const float mx = std::max(cx, w - cx), my = std::max(cy, h - cy);
            const float maxD = shape == 0 ? std::hypot(mx, my) : (shape == 1 ? mx + my : 1.0f);
            transitionShader(*out, from, to, 3, p, QVector4D(cx, cy, float(shape), float(prm.num(QStringLiteral("softness")))),
                             QVector4D(mx, my, maxD, 0), {});
        } else if (id == QLatin1String("clock")) {
            transitionShader(*out, from, to, 4, p,
                             QVector4D(float(prm.num(QStringLiteral("start")) / 360.0), float(prm.num(QStringLiteral("softness"))),
                                       prm.choice(QStringLiteral("rotation")) == 1 ? 1.0f : 0.0f, 0),
                             {}, {});
        } else if (id == QLatin1String("barn_door")) {
            transitionShader(*out, from, to, 5, p,
                             QVector4D(prm.choice(QStringLiteral("orientation")) == 1 ? 1.0f : 0.0f,
                                       float(prm.num(QStringLiteral("softness"))), 0, 0),
                             {}, {});
        } else if (id == QLatin1String("blinds")) {
            transitionShader(*out, from, to, 6, p,
                             QVector4D(float(std::round(prm.num(QStringLiteral("count")))),
                                       prm.choice(QStringLiteral("orientation")) == 1 ? 1.0f : 0.0f,
                                       float(prm.num(QStringLiteral("softness"))), 0),
                             {}, {});
        } else if (id == QLatin1String("pixel_dissolve")) {
            const float block = float(std::max(1, int(std::lround(prm.num(QStringLiteral("size")) * ctx.scale))));
            transitionShader(*out, from, to, 7, p, QVector4D(block, 0, 0, 0), {}, {});
        } else {
            transitionShader(*out, from, to, 0, p, {}, {}, {});
        }
        drawLayer(dst, *out, opacity);
    }

    // ---- Tracks and frames ---------------------------------------------------------------

    // The track's adjustment layers at t, on the picture so far (as the CPU's Compositor::applyAdjustments).
    void applyAdjustments(const Track& track, TimeUs t, Layer& canvas, const RenderContext& base)
    {
        for (const Clip& c : track.clips) {
            if (c.start > t)
                break;
            if (c.muted || t >= c.end() || !isAdjustment(c) || !hasActiveEffects(c.effects))
                continue;
            const double opacity = std::clamp(track.compositeAt(t) * c.gain * envelopeGain(c, ClipEnvelope{c.fadeIn, c.fadeOut}, t), 0.0, 1.0);
            if (opacity <= 0.002)
                continue;
            Layer adjusted = acquire(canvas->size(), false);
            drawLayerReplace(*adjusted, *canvas);
            RenderContext ctx = base;
            ctx.time = t - c.start;
            ctx.contentRect = QRectF(0, 0, canvas->w, canvas->h);
            applyChain(c.effects, adjusted, ctx);
            if (opacity >= 0.999) {
                canvas = std::move(adjusted);
            } else {
                Layer out = acquire(canvas->size(), false);
                transitionShader(*out, &*canvas, &*adjusted, 0, opacity, {}, {}, {});
                canvas = std::move(out);
            }
        }
    }

    // With a matte (the track matte above it, rendered), the track shows only through it (TrackMatte matteMode).
    void renderTrack(const ProjectSnapshot& snap, const Track& track, TimeUs t, const Target& canvas, const RenderContext& base,
                     const Target* matte = nullptr, int matteMode = 0)
    {
        std::vector<size_t> active;
        for (size_t ci = 0; ci < track.clips.size(); ++ci) {
            const Clip& c = track.clips[ci];
            if (c.start > t)
                break;
            if (!c.muted && t < c.end() && !isAdjustment(c)) // (adjustment layers: applyAdjustments)
                active.push_back(ci);
        }
        if (active.empty())
            return;
        // Track FX, blend modes and mattes work on the whole track's picture.
        const bool useTrackLayer = hasActiveEffects(track.effects) || track.blend != 0 || matte;
        Layer trackLayer;
        if (useTrackLayer)
            trackLayer = acquire(canvas.size());
        const Target& target = useTrackLayer ? *trackLayer : canvas;
        const double trackLevel = track.compositeAt(t); // level slider x composite envelope
        const double levelMul = useTrackLayer ? 1.0 : trackLevel;
        const QSize size = canvas.size();

        if (active.size() >= 2) {
            const Clip& a = track.clips[active[active.size() - 2]];
            const Clip& b = track.clips[active.back()];
            const double p = fadeShape(b.fadeInCurve, double(t - b.start) / double(std::max<TimeUs>(1, std::min(a.end(), b.end()) - b.start)));
            if (b.transitionIn.isPlainFade() && !hasActiveEffects(a.effects) && !hasActiveEffects(b.effects)) {
                drawClip(snap, a, t, target, levelMul * a.gain);
                drawClip(snap, b, t, target, levelMul * b.gain * std::clamp(p, 0.0, 1.0));
            } else if (b.transitionIn.isPlainFade()) {
                if (Layer la = clipLayer(snap, a, t, size, base))
                    drawLayer(target, *la, levelMul);
                if (Layer lb = clipLayer(snap, b, t, size, base))
                    drawLayer(target, *lb, levelMul * std::clamp(p, 0.0, 1.0));
            } else {
                Layer la = clipLayer(snap, a, t, size, base);
                Layer lb = clipLayer(snap, b, t, size, base);
                applyTransition(b.transitionIn, target, la.get(), lb.get(), p, levelMul, base);
            }
        } else {
            const Clip& c = track.clips[active.front()];
            const bool inFadeIn = c.fadeIn > 0 && t < c.start + c.fadeIn;
            const bool inFadeOut = c.fadeOut > 0 && t >= c.end() - c.fadeOut;
            if (inFadeIn && !c.transitionIn.isPlainFade()) {
                Layer layer = clipLayer(snap, c, t, size, base);
                applyTransition(c.transitionIn, target, nullptr, layer.get(), fadeShape(c.fadeInCurve, double(t - c.start) / double(c.fadeIn)), levelMul, base);
            } else if (inFadeOut && !c.transitionOut.isPlainFade()) {
                Layer layer = clipLayer(snap, c, t, size, base);
                applyTransition(c.transitionOut, target, layer.get(), nullptr, 1.0 - fadeShape(c.fadeOutCurve, double(c.end() - t) / double(c.fadeOut)),
                                levelMul, base);
            } else {
                const double env = envelopeGain(c, ClipEnvelope{c.fadeIn, c.fadeOut}, t);
                if (hasActiveEffects(c.effects)) {
                    if (Layer layer = clipLayer(snap, c, t, size, base))
                        drawLayer(target, *layer, levelMul * env);
                } else {
                    drawClip(snap, c, t, target, levelMul * c.gain * env);
                }
            }
        }
        if (useTrackLayer) {
            RenderContext ctx = base;
            ctx.time = t;
            ctx.contentRect = QRectF(0, 0, canvas.w, canvas.h);
            applyChain(track.effects, trackLayer, ctx);
            if (matte && trackLayer->size() == canvas.size()) {
                Layer out = acquire(canvas.size(), false);
                progMatte->bind();
                bindTexture(0, trackLayer->tex);
                bindTexture(1, matte->tex);
                progMatte->setUniformValue("uLayer", 0);
                progMatte->setUniformValue("uMatte", 1);
                progMatte->setUniformValue("uMode", matteMode);
                fullscreen(*progMatte, *out);
                trackLayer = std::move(out);
            }
            if (track.blend != 0 && trackLayer->size() == canvas.size()) {
                // Same blend as rack chains (and imageops::blendLayer on the CPU).
                Layer out = acquire(canvas.size(), false);
                progRackBlend->bind();
                bindTexture(0, canvas.tex);
                bindTexture(1, trackLayer->tex);
                progRackBlend->setUniformValue("uBase", 0);
                progRackBlend->setUniformValue("uLayer", 1);
                progRackBlend->setUniformValue("uOpacity", float(std::clamp(trackLevel, 0.0, 1.0)));
                progRackBlend->setUniformValue("uMode", track.blend);
                fullscreen(*progRackBlend, *out);
                drawLayerReplace(canvas, *out);
            } else {
                drawLayer(canvas, *trackLayer, trackLevel);
            }
        }
    }

    void evict()
    {
        for (auto it = entries.begin(); it != entries.end();) {
            Entry& e = **it;
            const bool stale = !e.failed && tick - e.lastUsed > kEvictAfterFrames;
            if (stale || (entries.size() > kMaxDecoders && e.lastUsed != tick && !e.failed)) {
                deleteEntryGl(e);
                it = entries.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = generators.begin(); it != generators.end();) {
            if (tick - it->second.lastUsed > kEvictAfterFrames) {
                deleteTarget(it->second.target);
                it = generators.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = effectStates.begin(); it != effectStates.end();)
            it = tick - it->second.lastUsed > kEvictAfterFrames ? effectStates.erase(it) : std::next(it);
    }

    QImage render(const ProjectSnapshot& snap, TimeUs t, QSize outSize)
    {
        if (!context->makeCurrent(surface))
            return {};
        ++tick;
        Layer canvas = compose(snap, t, outSize, false);
        QImage img = download(*canvas);
        canvas.reset();
        evict();
        return img;
    }

    Layer compose(const ProjectSnapshot& snap, TimeUs t, QSize outSize, bool transparent)
    {
        // Called with this renderer's GL context current, including nested sequences.
        if (session != snap.session) {
            for (auto& [id, cache] : sequenceResources) deleteSequenceResources(*cache);
            sequenceResources.clear(); sequenceSnapshots.clear();
            effectStates.clear();
            for (auto& e : entries) deleteEntryGl(*e);
            entries.clear();
            for (auto& [id, g] : generators) deleteTarget(g.target);
            generators.clear();
            session = snap.session;
        }
        Layer canvas = acquire(outSize, false);
        clear(*canvas, 0, 0, 0, transparent ? 0 : 1);

        RenderContext base;
        base.time = t;
        base.timelineTime = t;
        base.scale = double(outSize.width()) / std::max(1, snap.settings.width);
        base.fps = snap.settings.fps.toDouble();
        base.highQuality = highQuality;
        base.contentRect = QRectF(0, 0, outSize.width(), outSize.height());

        const auto& tracks = snap.timeline.tracks;
        const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
                                         [](const Track& tr) { return isVisualTrack(tr.type) && tr.solo; });
        const int covering = coveringTrack(snap, t); // (the topmost track hiding everything below it)
        for (int ti = covering >= 0 ? covering : int(tracks.size()) - 1; ti >= 0; --ti) {
            const Track& track = tracks[ti];
            if (!isVisualTrack(track.type) || track.muted || (anySolo && !track.solo) || track.matte) // (a matte: with the track below)
                continue;
            if (const int mi = matteAbove(snap.timeline, ti); mi >= 0) {
                Layer matte = acquire(outSize);
                renderTrack(snap, tracks[size_t(mi)], t, *matte, base);
                renderTrack(snap, track, t, *canvas, base, &*matte, tracks[size_t(mi)].matte);
            } else {
                renderTrack(snap, track, t, *canvas, base);
            }
            applyAdjustments(track, t, canvas, base);
        }
        if (hasActiveEffects(snap.timeline.outputEffects)) {
            applyChain(snap.timeline.outputEffects, canvas, base);
            if (!transparent) {
                Layer flat = acquire(outSize, false);
                clear(*flat, 0, 0, 0, 1);
                drawLayer(*flat, *canvas, 1.0);
                canvas = std::move(flat);
            }
        }
        return canvas;
    }

    bool renderYuv(const ProjectSnapshot& snap, TimeUs t, AVFrame* frame)
    {
        if (!frame || qEnvironmentVariableIsSet("MVP_CPU_COLOR")
            || (frame->format != AV_PIX_FMT_YUV420P && frame->format != AV_PIX_FMT_NV12)
            || (frame->width & 1) || (frame->height & 1)
            || (frame->colorspace != AVCOL_SPC_BT709 && frame->colorspace != AVCOL_SPC_SMPTE170M)
            || !context->makeCurrent(surface)) return false;
        ++tick;
        const int w = frame->width, h = frame->height;
        Layer canvas = compose(snap, t, {w,h}, false);
        Layer packed = acquire({w,h+h/2}, false, GL_R8);
        progEncodeYuv->bind();
        bindTexture(0, canvas->tex);
        progEncodeYuv->setUniformValue("uTex", 0);
        progEncodeYuv->setUniformValue("uSize", QVector2D(w,h));
        const bool hd = frame->colorspace == AVCOL_SPC_BT709;
        progEncodeYuv->setUniformValue("uLuma", QVector2D(hd ? .2126f : .299f, hd ? .0722f : .114f));
        progEncodeYuv->setUniformValue("uFullRange", frame->color_range == AVCOL_RANGE_JPEG);
        fullscreen(*progEncodeYuv, *packed);
        // One readback of 1.5 bytes/pixel replaces RGBA readback (4 bytes/pixel)
        // and CPU RGB-to-YUV conversion. Encoder row padding stays untouched.
        std::vector<uint8_t> pixels(size_t(w)*(h+h/2));
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glReadPixels(0, 0, w, h+h/2, GL_RED, GL_UNSIGNED_BYTE, pixels.data());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        for (int y = 0; y < h; ++y)
            memcpy(frame->data[0]+size_t(y)*frame->linesize[0], pixels.data()+size_t(y)*w, w);
        for (int y = 0; y < h/2; ++y) {
            const uint8_t* row = pixels.data()+size_t(h+y)*w;
            if (frame->format == AV_PIX_FMT_NV12) {
                uint8_t* uv = frame->data[1]+size_t(y)*frame->linesize[1];
                for (int x = 0; x < w/2; ++x) { uv[x*2]=row[x]; uv[x*2+1]=row[w/2+x]; }
            } else {
                memcpy(frame->data[1]+size_t(y)*frame->linesize[1], row, w/2);
                memcpy(frame->data[2]+size_t(y)*frame->linesize[2], row+w/2, w/2);
            }
        }
        packed.reset(); canvas.reset(); evict();
        return true;
    }

    // The chains one after the other, each flattened onto black (unless keepAlpha), with one upload and one read back.
    QImage renderLive(const QImage& frame, const std::vector<const EffectChain*>& all, TimeUs t, double fps, bool keepAlpha)
    {
        LiveFrameJob job;
        job.frame = frame;
        job.after = all;
        job.time = t;
        job.fps = fps;
        return renderLiveFrame(job, nullptr, keepAlpha);
    }

    RenderContext liveContext(QSize size, TimeUs t, double fps) const
    {
        RenderContext ctx;
        ctx.time = t; // (no events: local time is the Live tab's, so animated sources in user effects move)
        ctx.timelineTime = t;
        ctx.scale = double(size.width()) / 1920.0; // effect sizes as if the picture were 1080p
        ctx.fps = fps;
        ctx.highQuality = highQuality;
        ctx.contentRect = QRectF(0, 0, size.width(), size.height());
        return ctx;
    }

    // A chain on the picture, flattened onto black after (unless keepAlpha); nothing when it has nothing on.
    void liveChain(const EffectChain* chain, Layer& canvas, const RenderContext& ctx, bool keepAlpha)
    {
        if (!chain || !hasActiveEffects(*chain))
            return;
        const QSize size = canvas->size();
        applyChain(*chain, canvas, ctx);
        if (keepAlpha)
            return;
        Layer flat = acquire(size, false);
        clear(*flat, 0, 0, 0, 1);
        drawLayer(*flat, *canvas, 1.0);
        canvas = std::move(flat);
    }

    // A Live layer fitted into its place (cropped to fill it, or squeezed in), its own FX, then laid over the canvas.
    void layOver(Layer& canvas, const LiveFrameLayer& layer, TimeUs t, double fps)
    {
        const QSize size = canvas->size(), ls = layer.image.size(), rs = layer.rect.size();
        Layer src = acquire(ls, false);
        upload(*src, layer.image);
        QRectF uv(0, 0, 1, 1);
        if (layer.cover) {
            const double k = std::max(double(rs.width()) / ls.width(), double(rs.height()) / ls.height());
            const double vw = rs.width() / k / ls.width(), vh = rs.height() / k / ls.height();
            uv = QRectF((1 - vw) / 2, (1 - vh) / 2, vw, vh);
        }
        Layer fitted = acquire(rs, false);
        drawTexture(*fitted, src->tex, QSizeF(rs), QTransform(), 1.0, uv, false);
        src.reset();
        liveChain(layer.fx, fitted, liveContext(rs, t, fps), true);
        const float opacity = float(std::clamp(layer.opacity, 0.0, 1.0));
        const QTransform at = QTransform::fromTranslate(layer.rect.x(), layer.rect.y());
        if (BlendMode(layer.blend) == BlendMode::Normal) {
            drawTexture(*canvas, fitted->tex, QSizeF(rs), at, opacity);
            return;
        }
        Layer placed = acquire(size); // (see-through around it: the blends leave the picture there)
        drawTexture(*placed, fitted->tex, QSizeF(rs), at, 1.0, QRectF(0, 0, 1, 1), false);
        Layer out = acquire(size, false); // (the rack's blends: QPainter's modes at an opacity)
        progRackBlend->bind();
        bindTexture(0, canvas->tex);
        bindTexture(1, placed->tex);
        progRackBlend->setUniformValue("uBase", 0);
        progRackBlend->setUniformValue("uLayer", 1);
        progRackBlend->setUniformValue("uOpacity", opacity);
        progRackBlend->setUniformValue("uMode", layer.blend);
        fullscreen(*progRackBlend, *out);
        canvas = std::move(out);
    }

    // As FrameRenderer::renderLiveFrame (the CPU's), with one upload (and one per layer) and one read back.
    QImage renderLiveFrame(const LiveFrameJob& job, QImage* clean, bool keepAlpha = false)
    {
        auto active = [](const EffectChain* c) { return c && hasActiveEffects(*c); };
        auto shown = [](const LiveFrameLayer& l) { return !l.image.isNull() && !l.rect.isEmpty(); };
        const bool under = std::any_of(job.layers.begin(), job.layers.end(), [&](const LiveFrameLayer& l) { return shown(l) && !l.overEffects; });
        bool any = std::any_of(job.layers.begin(), job.layers.end(), shown) || job.fadeProgress >= 0 || active(job.liveFx);
        for (const EffectChain* c : job.after)
            any = any || active(c);
        if (job.frame.isNull() || !any) {
            if (clean)
                *clean = job.frame;
            return job.frame;
        }
        if (!context->makeCurrent(surface))
            return {};
        ++tick;
        const QSize size = job.frame.size();
        const RenderContext ctx = liveContext(size, job.time, job.fps);
        Layer canvas = acquire(size, false);
        upload(*canvas, job.frame);
        if (under) {
            for (const LiveFrameLayer& l : job.layers)
                if (shown(l) && !l.overEffects)
                    layOver(canvas, l, job.time, job.fps);
            if (clean)
                *clean = download(*canvas);
        } else if (clean) {
            *clean = job.frame;
        }
        liveChain(job.liveFx, canvas, ctx, keepAlpha);
        if (job.fadeProgress >= 0) {
            Layer from = acquire(size, false), to = acquire(size, false);
            drawLayerReplace(*from, *canvas);
            drawLayerReplace(*to, *canvas);
            liveChain(job.fadeFrom, from, ctx, false);
            liveChain(job.fadeTo, to, ctx, false);
            Layer mixed = acquire(size);
            RenderContext tc = ctx;
            tc.timelineTime = 0; // (its settings as liveTransition reads them)
            applyTransition(job.fadeSpec, *mixed, &*from, &*to, job.fadeProgress, 1.0, tc);
            canvas = acquire(size, false);
            clear(*canvas, 0, 0, 0, 1);
            drawLayer(*canvas, *mixed, 1.0);
        }
        for (const EffectChain* c : job.after)
            liveChain(c, canvas, ctx, keepAlpha);
        for (const LiveFrameLayer& l : job.layers)
            if (shown(l) && l.overEffects)
                layOver(canvas, l, job.time, job.fps);
        QImage img = download(*canvas);
        canvas.reset();
        evict();
        return img;
    }
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

GpuCompositor::GpuCompositor(std::unique_ptr<Impl> impl) : d(std::move(impl)) {}
GpuCompositor::~GpuCompositor() = default;

std::unique_ptr<GpuCompositor> GpuCompositor::create(QOffscreenSurface* surface, bool highQuality, QString* error)
{
    if (!surface || !surface->isValid()) {
        if (error)
            *error = QStringLiteral("no GPU surface");
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->surface = surface;
    impl->highQuality = highQuality;
    if (!impl->init(error))
        return nullptr;
    return std::unique_ptr<GpuCompositor>(new GpuCompositor(std::move(impl)));
}

QImage GpuCompositor::renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha)
{
    return d->renderLive(frame, {&chain}, t, fps, keepAlpha);
}

QImage GpuCompositor::renderLiveFrame(const LiveFrameJob& job, QImage* clean)
{
    return d->renderLiveFrame(job, clean);
}

QImage GpuCompositor::render(const ProjectSnapshot& snap, TimeUs t, QSize outSize)
{
    d->useProxies = m_useProxies;
    return d->render(snap, t, outSize);
}
bool GpuCompositor::renderYuv(const ProjectSnapshot& snap, TimeUs t, AVFrame* frame)
{
    d->useProxies = m_useProxies;
    return d->renderYuv(snap, t, frame);
}

QImage GpuCompositor::renderGraph(const nodes::Graph& graph, const QImage& source, const RenderContext& ctx, std::map<int, QImage>* thumbs,
                                  QSize thumb)
{
    return d->renderGraph(graph, source, ctx, thumbs, thumb);
}

int GpuCompositor::cpuFallbacks() const
{
    return d->cpuFallbacks;
}

QString GpuCompositor::name() const
{
    return QStringLiteral("GPU (%1)").arg(d->rendererName);
}

std::unique_ptr<FrameRenderer> createFrameRenderer(bool highQuality, QOffscreenSurface* gpuSurface, QString* gpuError)
{
    if (gpuSurface && hw::settings().gpuCompositing) {
        if (auto gpu = GpuCompositor::create(gpuSurface, highQuality, gpuError))
            return gpu;
    }
    return std::make_unique<Compositor>(highQuality);
}

std::unique_ptr<QOffscreenSurface> createGpuSurface()
{
    auto surface = std::make_unique<QOffscreenSurface>();
    surface->setFormat(glFormat());
    surface->create();
    if (!surface->isValid())
        return nullptr;
    return surface;
}
