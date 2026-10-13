#pragma once

// GLSL for the broken-console effects (ConsoleEngine.h). kFragConsole mirrors
// ConsoleEngine.cpp statement by statement; the uG[] globals are named by the
// "#define G_..." block from vg::shaderDefines(), which is inserted after the
// shared header. Passes (uPass): 0 framebuffer (area average + console
// colours, float target), 1 attribute cells (float target), 2 the screen,
// 3 LCD persistence blend. The mesh program draws the 3D glitches.

namespace gpuvg {

inline const char* kFragConsole = R"(
uniform sampler2D uTex;    // unit 0: layer (RGBA8) or framebuffer (float)
uniform sampler2D uTex1;   // unit 1: held / previous source (same kind as uTex)
uniform sampler2D uCells;  // unit 2: attribute cells (float)
uniform sampler2D uData;   // unit 3: plan tables (float)
uniform sampler2D uOver;   // unit 4: overlay (RGBA8)
uniform int uPass;
uniform int uGhost;        // pass 3: share of the previous output (0..256)
uniform float uG[VG_GLOBAL_COUNT];

const int kBayer4[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
const int kPs1[16] = int[16](-4, 0, -3, 1, 2, -2, 3, -1, -3, 1, -4, 0, 3, -1, 2, -2);

vec4 D(int x, int lane) { return texelFetch(uData, ivec2(x, lane), 0); }
vec3 unpackC(float p) { int i = int(p); return vec3(float((i >> 16) & 255), float((i >> 8) & 255), float(i & 255)); }
float hash01(uint x, uint y, uint seed) { return float(hash32(x, y, seed) & 0xffffffu) / 16777216.0; }
int imod(int a, int n) { return a >= 0 ? a % n : (n - 1) - ((-a - 1) % n); }
vec4 B(vec4 c) { return floor(c * 255.0 + 0.5); }
vec4 fetchClamp(sampler2D t, ivec2 p, ivec2 size) { return B(texelFetch(t, clamp(p, ivec2(0), size - 1), 0)); }
// Bilinear sample in bytes with 8-bit weights (fx::Pixels::sample, clamped edges).
vec4 sampleB(sampler2D t, vec2 pix, ivec2 size) {
    pix -= 0.5;
    vec2 f0 = floor(pix);
    ivec2 i0 = ivec2(f0);
    float wx = floor((pix.x - f0.x) * 256.0 + 0.5), wy = floor((pix.y - f0.y) * 256.0 + 0.5);
    vec4 p00 = fetchClamp(t, i0, size), p10 = fetchClamp(t, i0 + ivec2(1, 0), size);
    vec4 p01 = fetchClamp(t, i0 + ivec2(0, 1), size), p11 = fetchClamp(t, i0 + ivec2(1, 1), size);
    vec4 a = floor((p00 * (256.0 - wx) + p10 * wx) / 256.0);
    vec4 b = floor((p01 * (256.0 - wx) + p11 * wx) / 256.0);
    return floor((a * (256.0 - wy) + b * wy) / 256.0);
}
float ditherAt(int x, int y) {
    int mode = int(G_DITHER);
    if (mode == 1) return (float(kBayer4[(y & 3) * 4 + (x & 3)]) + 0.5) / 16.0 - 0.5;
    if (mode == 2) return float(kPs1[(y & 3) * 4 + (x & 3)]) / 8.0;
    if (mode == 3) return ((x + y) & 1) != 0 ? 0.25 : -0.25;
    return 0.0;
}
int quantBits(int v, int n, float d) {
    if (n >= 8) return v;
    int L = (1 << n) - 1;
    float stp = 255.0 / float(L);
    int k = clamp(int(floor((float(v) + d * stp) / stp + 0.5)), 0, L);
    return int(floor(float(k) * 255.0 / float(L) + 0.5));
}
struct Smp { vec3 c; float a; int idx; float luma; };
Smp quantize(int r, int g, int b, int x, int y) {
    int mode = int(G_QMODE);
    float d = ditherAt(x, y) * G_DITHER_AMT;
    if (mode == 1)
        return Smp(vec3(float(quantBits(r, int(G_QBITS_R), d)), float(quantBits(g, int(G_QBITS_G), d)), float(quantBits(b, int(G_QBITS_B), d))), 0.0, -1, 0.0);
    int n = int(G_PAL_N);
    if (mode == 2 && n > 0) {
        float off = d * 32.0;
        int qr = clamp(int(floor(float(r) + off + 0.5)), 0, 255) >> 3;
        int qg = clamp(int(floor(float(g) + off + 0.5)), 0, 255) >> 3;
        int qb = clamp(int(floor(float(b) + off + 0.5)), 0, 255) >> 3;
        int i = (qr << 10) | (qg << 5) | qb;
        int W = int(G_DATA_W), j = i / 4;
        int idx = min(int(texelFetch(uData, ivec2(j % W, int(G_LANE_LUT) + j / W), 0)[i % 4]), n - 1);
        return Smp(unpackC(D(idx, 5).x), 0.0, idx, 0.0);
    }
    if (mode == 3 && n > 0) {
        float l = 0.299 * float(r) + 0.587 * float(g) + 0.114 * float(b) + d * 256.0 / float(n);
        int idx = clamp(int(floor(l * float(n) / 256.0)), 0, n - 1);
        return Smp(unpackC(D(idx, 5).x), 0.0, idx, 0.0);
    }
    return Smp(vec3(float(r), float(g), float(b)), 0.0, -1, 0.0);
}
int unpremul(int v, int a) { return a > 0 ? min(255, (v * 255 + a / 2) / a) : 0; }
int lumaInt(int r, int g, int b) { return (299 * r + 587 * g + 114 * b + 500) / 1000; }
Smp sampleAt(bool alt, float sx, float sy, int px, int py) {
    int cw = int(G_CW), ch = int(G_CH);
    if (int(G_LOOK) == 0) {
        ivec2 i = ivec2(clamp(int(floor(sx)), 0, cw - 1), clamp(int(floor(sy)), 0, ch - 1));
        vec4 t = alt ? texelFetch(uTex1, i, 0) : texelFetch(uTex, i, 0);
        return Smp(unpackC(t.x), t.z, int(t.y), t.w);
    }
    vec2 pix = vec2(sx * G_SRC_W / float(cw), sy * G_SRC_H / float(ch));
    ivec2 size = ivec2(int(G_SRC_W), int(G_SRC_H));
    vec4 p = alt ? sampleB(uTex1, pix, size) : sampleB(uTex, pix, size);
    int a = int(p.a);
    int r = unpremul(int(p.r), a), g = unpremul(int(p.g), a), b = unpremul(int(p.b), a);
    Smp q = Smp(vec3(float(r), float(g), float(b)), 0.0, -1, 0.0);
    int mode = int(G_QMODE);
    if (int(G_LOOK) == 1 && mode >= 1 && mode <= 3) q = quantize(r, g, b, px, py);
    q.a = float(a);
    q.luma = float(lumaInt(r, g, b));
    return q;
}
float wrapf(float v, float n) {
    v -= n * floor(v / n);
    return v >= n ? 0.0 : v;
}
ivec3 permute(ivec3 c, int perm) {
    if (perm == 1) return c.rbg;
    if (perm == 2) return c.grb;
    if (perm == 3) return c.gbr;
    if (perm == 4) return c.brg;
    if (perm == 5) return c.bgr;
    return c;
}
)"
R"(
vec4 shade(int x, int y) {
    int GW = int(G_GW), GH = int(G_GH), AX = int(G_AX), AY = int(G_AY), CW = int(G_CW), CH = int(G_CH);
    float CWf = float(CW), CHf = float(CH);
    int W = int(G_SRC_W), H = int(G_SRC_H);
    int nx = (2 * x + 1) * GW, ny = (2 * y + 1) * GH;
    int gx = min(nx / (2 * W), GW - 1), gy = min(ny / (2 * H), GH - 1);
    float fu = float(nx - gx * 2 * W) / float(2 * W), fv = float(ny - gy * 2 * H) / float(2 * H);
    float u = float(gx) + fu;
    vec4 rowMap = D(gy, 0), rowFx = D(gy, 1), rowEx = D(gy, 2), rowEx2 = D(gy, 3), col = D(gx, 4);
    vec3 c = vec3(0.0);
    float a = 255.0;
    bool inside = gx >= AX && gx < AX + CW && gy >= AY && gy < AY + CH;
    if (!inside) {
        c = unpackC(rowEx.x >= 0.0 ? rowEx.x : G_BORDER);
    } else {
        float lu = u - float(AX);
        float sx = rowMap.x + rowMap.y * lu;
        float sy = rowMap.z + rowMap.w * lu + (fv - 0.5);
        bool backdrop = false, synth = false;
        int edge = int(G_EDGE);
        if (edge == 0) {
            sx = wrapf(sx, CWf);
            sy = wrapf(sy, CHf);
        } else if (edge == 1) {
            sx = clamp(sx, 0.0, CWf - 0.001);
            sy = clamp(sy, 0.0, CHf - 0.001);
        } else if (sx < 0.0 || sy < 0.0 || sx >= CWf || sy >= CHf) {
            backdrop = true;
        }
        float M = G_MOSAIC;
        if (M > 1.0) {
            sx = (floor(sx / M) + 0.5) * M;
            sy = (floor(sy / M) + 0.5) * M;
        }
        int shift = 0, perm = -1, txor = 0, ink = -1, paper = -1, flash = 0;
        if (!backdrop) {
            float T = G_TILE;
            int tx = clamp(int(floor(sx / T)), 0, int(G_TX) - 1);
            int ty = clamp(int(floor(sy / T)), 0, int(G_TY) - 1);
            vec4 tile = D(tx, int(G_LANE_TILES) + ty);
            int m = int(tile.z), op = m & 7;
            float lx = sx - float(tx) * T, ly = sy - float(ty) * T;
            if (op == 1) {
                if ((m & 8) != 0) lx = T - lx;
                if ((m & 16) != 0) ly = T - ly;
                sx = wrapf(tile.x * T + lx, CWf);
                sy = wrapf(tile.y * T + ly, CHf);
            } else if (op == 2) {
                int ix = clamp(int(floor(lx)), 0, int(T) - 1), iy = clamp(int(floor(ly)), 0, int(T) - 1);
                uint ts = uint(int(tile.w));
                uint bits = hash32(uint(iy), ts + uint(ix >> 3) * 7919u, 11u);
                int bit = 7 - (ix & 7);
                int v = int((bits >> uint(bit)) & 1u);
                if (G_GARB_BPP > 1.5) v |= int((bits >> uint(bit + 8)) & 1u) << 1;
                int n = int(G_PAL_N);
                if (v == 0 && G_GARB_BPP < 1.5) c = unpackC(G_BACKDROP);
                else if (n > 0) c = unpackC(D(imod(int(tile.x) + v, n), 5).y);
                else if (v == 0) c = unpackC(G_BACKDROP);
                else c = unpackC(float(hash32(uint(v), ts, 13u) & 0xffffffu));
                synth = true;
            } else if (op == 3) {
                c = unpackC(tile.w);
                synth = true;
            } else if (op == 4) {
                backdrop = true;
            } else if (op == 5) {
                shift = int(tile.x);
                perm = int(tile.y);
                txor = int(tile.w);
            } else if (op == 6) {
                ink = int(tile.x);
                paper = int(tile.y);
                flash = int(tile.w);
            }
        }
        if (backdrop) {
            c = unpackC(G_BACKDROP);
            synth = true;
        }
        if (!synth) {
            bool alt = rowEx.y >= 0.5;
            Smp s = sampleAt(alt, sx, sy, x, y);
            int sub = int(G_SUB_OP);
            if (sub > 0 && G_SUB_AMT > 0.0) {
                Smp t = sampleAt(alt, wrapf(sx + G_SUB_DX, CWf), wrapf(sy + G_SUB_DY, CHf), x, y);
                float k = G_SUB_AMT;
                if (sub == 1) s.c = min(vec3(255.0), s.c + t.c * k);
                else if (sub == 2) s.c = max(vec3(0.0), s.c - t.c * k);
                else if (sub == 3) s.c = s.c + (t.c - s.c) * k * 0.5;
                else if (sub == 4) { if (((gx + gy) & 1) != 0) s.c = s.c + (t.c - s.c) * k; }
                else s.c = max(s.c, t.c * k);
                s.idx = -1;
            }
            int idx = s.idx;
            int n = int(G_PAL_N);
            if (G_CELLS > 0.5 && idx >= 0) {
                ivec2 csz = textureSize(uCells, 0);
                int cx = clamp(int(floor(sx / 8.0)), 0, csz.x - 1), cy = clamp(int(floor(sy / 8.0)), 0, csz.y - 1);
                vec4 cell = texelFetch(uCells, ivec2(cx, cy), 0);
                int ci0 = int(cell.x), cp0 = int(cell.y);
                vec3 P = unpackC(D(idx, 5).x), I = unpackC(D(ci0, 5).x), Q = unpackC(D(cp0, 5).x);
                int di = int(2.0 * (P.r - I.r) * (P.r - I.r) + 4.0 * (P.g - I.g) * (P.g - I.g) + 3.0 * (P.b - I.b) * (P.b - I.b));
                int dp = int(2.0 * (P.r - Q.r) * (P.r - Q.r) + 4.0 * (P.g - Q.g) * (P.g - Q.g) + 3.0 * (P.b - Q.b) * (P.b - Q.b));
                int ci = ink >= 0 ? ink : ci0, cp = paper >= 0 ? paper : cp0;
                if (flash != 0 && G_FLASH > 0.5) { int tmp = ci; ci = cp; cp = tmp; }
                idx = di <= dp ? ci : cp;
            }
            if (idx >= 0 && n > 0) s.c = unpackC(D(imod(idx + int(G_PAL_SHIFT) + shift, n), 5).y);
            if (int(G_QMODE) == 4) {
                float l0 = sampleAt(alt, sx - 1.0, sy, x, y).luma, l1 = sampleAt(alt, sx + 1.0, sy, x, y).luma;
                float l2 = sampleAt(alt, sx, sy - 1.0, x, y).luma, l3 = sampleAt(alt, sx, sy + 1.0, x, y).luma;
                float e = abs(l1 - l0) + abs(l3 - l2);
                float k = clamp(e * G_VEC_GAIN / 255.0, 0.0, 1.0);
                s.c = unpackC(G_VEC_COLOR) * k;
            }
            c = s.c;
            a = s.a;
        }
        float gain = rowFx.x * col.x * G_GAIN;
        c = (c - 128.0) * G_CONTRAST + 128.0 + G_BRIGHT;
        float l = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        c = (l + (c - l) * G_SAT) * gain;
        if (G_TINT_AMT > 0.0) c = c + (c * unpackC(G_TINT) / 255.0 - c) * G_TINT_AMT;
        ivec3 ic = clamp(ivec3(floor(c + 0.5)), ivec3(0), ivec3(255));
        int rx = int(rowFx.z) ^ txor;
        ic.r = (((ic.r & int(G_AND_R)) | int(G_OR_R)) ^ int(G_XOR_R) ^ rx) & 255;
        ic.g = (((ic.g & int(G_AND_G)) | int(G_OR_G)) ^ int(G_XOR_G) ^ rx) & 255;
        ic.b = (((ic.b & int(G_AND_B)) | int(G_OR_B)) ^ int(G_XOR_B) ^ rx) & 255;
        ic = permute(ic, perm >= 0 ? perm : int(G_PERM));
        int drop = int(G_DROP);
        if ((drop & 1) != 0) ic.r = 0;
        if ((drop & 2) != 0) ic.g = 0;
        if ((drop & 4) != 0) ic.b = 0;
        c = vec3(ic);
        if (rowEx2.x > 0.5) {
            float v = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b >= 128.0 ? 215.0 : 0.0;
            c = vec3(v);
        }
        if (rowEx.z > 0.0) {
            float nv = hash01(uint(gx), uint(gy), uint(int(G_NOISE_SEED))) * 255.0;
            c = c + (nv - c) * rowEx.z;
        }
        if (rowFx.y >= 0.0) {
            float k = rowEx.w;
            c = c + (unpackC(rowFx.y) - c) * k;
            a = a + (255.0 - a) * k;
        }
        if (col.y >= 0.0) {
            float k = col.z;
            c = c + (unpackC(col.y) - c) * k;
            a = a + (255.0 - a) * k;
        }
        int spark = int(G_SPARK_MODE);
        if (spark > 0) {
            uint seed = uint(int(G_SPARK_SEED));
            bool lit;
            if (spark == 3) {
                int len = max(1, int(G_SPARK_LEN));
                int off = int(hash32(uint(gy), 77u, seed) % uint(len));
                lit = hash01(uint((gx + off) / len), uint(gy), seed) < G_SPARK_DENS * rowEx2.y;
            } else {
                lit = hash01(uint(gx), uint(gy), seed) < G_SPARK_DENS * rowEx2.y;
            }
            if (lit) {
                if (spark == 2) c = unpackC(float(hash32(uint(gx), uint(gy), seed + 1u) & 0xffffffu));
                else if (spark == 4) {
                    int k = int(hash32(uint(gx), uint(gy), seed + 2u) % 5u);
                    c = k == 0 ? vec3(0.0) : k == 1 ? vec3(255.0) : k == 2 ? vec3(255.0, 0.0, 0.0) : k == 3 ? vec3(0.0, 255.0, 0.0) : vec3(0.0, 0.0, 255.0);
                } else c = unpackC(G_SPARK_COLOR);
                a = 255.0;
            }
        }
        int art = int(G_ART_MODE);
        if (art > 0) {
            int C = max(2, int(G_ART_CELL));
            uint seed = uint(int(G_ART_SEED));
            float dens = G_ART_DENS;
            int pick = -1;
            if (art == 1) {
                if ((gx % C) < max(2, C / 2) && (gy % C) < max(1, C / 8) && hash01(uint(gx / C), uint(gy / C), seed) < dens)
                    pick = int(hash32(uint(gx / C), uint(gy / C), seed + 1u) & 1u);
            } else if (art == 2) {
                if (hash01(uint(gx / C), uint(gy / C), seed) < dens) pick = ((gx >> 1) + (gy >> 1)) & 1;
            } else if (art == 3) {
                if (hash01(uint(gx), 0u, seed) < dens && hash01(uint(gx), uint(gy / C), seed + 2u) < 0.85)
                    pick = int(hash32(uint(gx), 1u, seed) & 1u);
            } else {
                if (hash01(uint(gx / C), uint(gy / C), seed) < dens) pick = 2;
            }
            if (pick == 2) {
                uint h = hash32(uint(gx), uint(gy), seed + 3u);
                c = vec3(float((h & 7u) * 36u), float(((h >> 3u) & 7u) * 36u), float(((h >> 6u) & 3u) * 85u));
                a = 255.0;
            } else if (pick >= 0) {
                c = unpackC(pick == 0 ? G_ART_C1 : G_ART_C2);
                a = 255.0;
            }
        }
        if (lu < rowFx.w) {
            c = vec3(0.0);
            a = 255.0;
        }
    }
    if (G_BLANK > 0.0) {
        float k = G_BLANK;
        c = c + (unpackC(G_BLANK_COLOR) - c) * k;
        a = a + (255.0 - a) * k;
    }
    if (G_OVERLAY > 0.5) {
        int ow = int(G_OVL_W), oh = int(G_OVL_H);
        int ox = clamp(int(floor((float(gx) + fu) * float(ow) / float(GW))), 0, ow - 1);
        int oy = clamp(int(floor((float(gy) + fv) * float(oh) / float(GH))), 0, oh - 1);
        vec4 o = B(texelFetch(uOver, ivec2(ox, oy), 0));
        c = c * (255.0 - o.a) / 255.0 + o.rgb;
        a = max(a, o.a);
    }
    if (int(G_LOOK) == 0 && G_SCREEN_AMT > 0.0) {
        float cwPx = float(W) / float(GW), chPx = float(H) / float(GH);
        int type = int(G_SCREEN_TYPE);
        if (type == 1) {
            float fade = clamp((chPx - 1.5) * 0.5, 0.0, 1.0);
            float dark = fv > 0.5 ? (fv - 0.5) * 2.0 : 0.0;
            c *= 1.0 - G_SCREEN_AMT * 0.6 * fade * dark;
        } else if (type == 2) {
            float fade = clamp((min(cwPx, chPx) - 2.0) * 0.5, 0.0, 1.0);
            bool gapX = x == 0 || min((2 * x - 1) * GW / (2 * W), GW - 1) != gx;
            bool gapY = y == 0 || min((2 * y - 1) * GH / (2 * H), GH - 1) != gy;
            if (gapX || gapY) c *= 1.0 - G_SCREEN_AMT * 0.4 * fade;
        }
    }
    float A = float(clamp(int(floor(a + 0.5)), 0, 255));
    vec3 o = min(vec3(A), clamp(floor(c * A / 255.0 + 0.5), 0.0, 255.0));
    return vec4(o, A) / 255.0;
}

void main() {
    ivec2 ip = ivec2(floor(vPix));
    if (uPass == 0) {
        // Console framebuffer: area average of the layer, console colours.
        int cw = int(G_CW), ch = int(G_CH), W = int(G_SRC_W), H = int(G_SRC_H);
        int gx = ip.x, gy = ip.y;
        int sy0 = min(H - 1, gy * H / ch), sy1 = min(H, max(sy0 + 1, (gy + 1) * H / ch));
        int sx0 = min(W - 1, gx * W / cw), sx1 = min(W, max(sx0 + 1, (gx + 1) * W / cw));
        ivec4 sum = ivec4(0);
        for (int y = sy0; y < sy1; ++y)
            for (int x = sx0; x < sx1; ++x)
                sum += ivec4(B(texelFetch(uTex, ivec2(x, y), 0)));
        int n = (sy1 - sy0) * (sx1 - sx0);
        int A = (sum.a + n / 2) / n;
        int r = unpremul((sum.r + n / 2) / n, A), g = unpremul((sum.g + n / 2) / n, A), b = unpremul((sum.b + n / 2) / n, A);
        Smp q = quantize(r, g, b, gx, gy);
        ivec3 qc = ivec3(q.c);
        fragColor = vec4(float((qc.r << 16) | (qc.g << 8) | qc.b), float(q.idx), float(A), float(lumaInt(r, g, b)));
    } else if (uPass == 1) {
        // Attribute cells: the two most common palette colours of each 8x8 cell.
        int cw = int(G_CW), ch = int(G_CH);
        int n = clamp(int(G_PAL_N), 1, 16);
        int counts[16] = int[16](0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        for (int y = ip.y * 8; y < min(ch, ip.y * 8 + 8); ++y)
            for (int x = ip.x * 8; x < min(cw, ip.x * 8 + 8); ++x) {
                int i = int(texelFetch(uTex, ivec2(x, y), 0).y);
                if (i >= 0 && i < n) counts[i] += 1;
            }
        int first = 0;
        for (int i = 1; i < n; ++i)
            if (counts[i] > counts[first]) first = i;
        int second = -1;
        for (int i = 0; i < n; ++i)
            if (i != first && counts[i] > 0 && (second < 0 || counts[i] > counts[second])) second = i;
        fragColor = vec4(float(first), float(second < 0 ? first : second), 0.0, 0.0);
    } else if (uPass == 2) {
        fragColor = shade(ip.x, ip.y);
    } else {
        // LCD persistence: blend with the previous output.
        vec4 cur = B(texelFetch(uTex, ip, 0)), prev = B(texelFetch(uTex1, ip, 0));
        fragColor = floor((cur * float(256 - uGhost) + prev * float(uGhost)) / 256.0) / 255.0;
    }
}
)";

inline const char* kVertMesh = R"(#version 330 core
layout(location = 0) in vec2 aPos;   // 0..1 of the target
layout(location = 1) in vec2 aUv;    // 0..1 of the source
uniform vec2 uSize;
out vec2 vSrc;
void main() {
    vSrc = aUv * uSize;
    gl_Position = vec4(aPos * 2.0 - 1.0, 0.0, 1.0);
}
)";

inline const char* kFragMesh = R"(#version 330 core
in vec2 vSrc;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2 uSize;
vec4 B(vec4 c) { return floor(c * 255.0 + 0.5); }
vec4 fetchClamp(ivec2 p, ivec2 size) { return B(texelFetch(uTex, clamp(p, ivec2(0), size - 1), 0)); }
void main() {
    ivec2 size = ivec2(uSize);
    vec2 pix = vSrc - 0.5;
    vec2 f0 = floor(pix);
    ivec2 i0 = ivec2(f0);
    float wx = floor((pix.x - f0.x) * 256.0 + 0.5), wy = floor((pix.y - f0.y) * 256.0 + 0.5);
    vec4 p00 = fetchClamp(i0, size), p10 = fetchClamp(i0 + ivec2(1, 0), size);
    vec4 p01 = fetchClamp(i0 + ivec2(0, 1), size), p11 = fetchClamp(i0 + ivec2(1, 1), size);
    vec4 a = floor((p00 * (256.0 - wx) + p10 * wx) / 256.0);
    vec4 b = floor((p01 * (256.0 - wx) + p11 * wx) / 256.0);
    fragColor = floor((a * (256.0 - wy) + b * wy) / 256.0) / 255.0;
}
)";

} // namespace gpuvg
