#pragma once

// "Broken Video Game Systems": a small virtual games console that redraws a
// layer the way the machine would (its resolution, colours and screen) and
// breaks it the way real hardware breaks: corrupted tiles, scroll and raster
// errors, stuck data bits, palette damage, dead LCD lines, frozen frames,
// exploding polygons...
//
// Every effect builds a Plan on the CPU (settings plus per-row, per-column
// and per-tile tables, packed into one float texture). The CPU renderer here
// and the GPU shader (GpuConsoleShaders.h) run the same per-pixel program on
// that data, so both renderers agree.
//
// Coordinates: the console screen is a grid of GW x GH console pixels that
// fills the layer; the picture area ("active", CW x CH at AX, AY) may sit in a
// border. Row tables map each screen row to a line in picture space, tiles
// (TILE x TILE console pixels) remap or replace parts of the picture.

#include "core/Effects.h"

#include <QByteArray>
#include <QImage>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace vg {

// Global settings of a plan (uG[] in the shader). Colours are packed 0xRRGGBB
// in a float (exact below 2^24).
#define VG_GLOBALS(X) \
    X(GW) X(GH) X(AX) X(AY) X(CW) X(CH) X(TILE) X(TX) X(TY) X(DATA_W) X(LANE_LUT) X(LANE_TILES) \
    X(SRC_W) X(SRC_H) X(EDGE) X(MOSAIC) X(LOOK) X(QMODE) X(QBITS_R) X(QBITS_G) X(QBITS_B) X(DITHER) \
    X(DITHER_AMT) X(PAL_N) X(SCREEN_TYPE) X(SCREEN_AMT) X(BACKDROP) X(BORDER) \
    X(GAIN) X(CONTRAST) X(SAT) X(BRIGHT) X(TINT) X(TINT_AMT) \
    X(AND_R) X(AND_G) X(AND_B) X(OR_R) X(OR_G) X(OR_B) X(XOR_R) X(XOR_G) X(XOR_B) X(PERM) X(DROP) X(PAL_SHIFT) \
    X(SUB_OP) X(SUB_DX) X(SUB_DY) X(SUB_AMT) \
    X(SPARK_MODE) X(SPARK_DENS) X(SPARK_LEN) X(SPARK_SEED) X(SPARK_COLOR) \
    X(ART_MODE) X(ART_DENS) X(ART_CELL) X(ART_SEED) X(ART_C1) X(ART_C2) \
    X(BLANK) X(BLANK_COLOR) X(OVERLAY) X(OVL_W) X(OVL_H) X(CELLS) X(FLASH) X(GARB_BPP) \
    X(VEC_GAIN) X(VEC_COLOR) X(NOISE_SEED)

enum Global : int {
#define VG_ENUM(n) G_##n,
    VG_GLOBALS(VG_ENUM)
#undef VG_ENUM
    kGlobalCount
};

// "#define G_GW uG[0]" ... for the shader.
QByteArray shaderDefines();

// Data texture lanes (rows of the texture); lanes from LANE_LUT on are placed by finalize().
enum Lane : int { LaneRowMap = 0, LaneRowFx, LaneRowEx, LaneRowEx2, LaneCols, LanePalette, kFixedLanes };

// Tile operations (Plan::tiles[].z; +8 flips horizontally, +16 vertically).
enum TileOp : int { TileKeep = 0, TileCopy, TileGarbage, TileSolid, TileHide, TileShift, TileAttr };

enum Look : int { LookFull = 0, LookColours, LookGlitchOnly };
enum QMode : int { QNone = 0, QBits, QPalette, QShades, QVector };
enum Screen : int { ScreenNone = 0, ScreenCrt, ScreenLcd };

using Px4 = std::array<float, 4>;

struct MeshVertex {
    float x, y; // position, 0..1 of the layer
    float u, v; // texture coordinate, 0..1 of the layer
};

struct Plan {
    std::array<float, kGlobalCount> g{};
    // Per screen row (GH):
    std::vector<Px4> rowMap; // picture position for column lu: x = a + b*lu, y = c + d*lu (+ position in the row)
    std::vector<Px4> rowFx;  // gain, fill colour (-1 none), XOR bits, black from the left (console px)
    std::vector<Px4> rowEx;  // border colour (-1 = BORDER), use held/previous frame (0/1), noise, fill opacity
    std::vector<Px4> rowEx2; // 1-bit (paper/ink) display (0/1), sparkle density factor, unused, unused
    std::vector<Px4> cols;   // per screen column (GW): gain, dead colour (-1 none), dead opacity, unused
    std::vector<Px4> tiles;  // TX * TY picture tiles: x, y (source tile / palette / ink), op + flips, param
    std::vector<uint32_t> palette; // colours the picture is matched to (<= 128, RGB)
    std::vector<uint32_t> display; // colours shown for those indices (corrupted palettes differ)
    const std::vector<uint8_t>* lut = nullptr; // 32x32x32 -> nearest palette index (QMODE palette)
    std::vector<MeshVertex> mesh;  // triangles (3 vertices each), drawn over BACKDROP before the rest
    int64_t freezeKey = -1;        // >= 0: rows using the held frame show the frame held for this key
    bool needPrev = false;         // rows using the held frame show the previous frame instead
    float ghost = 0;               // LCD persistence: share of the previous output kept (0..1)
    QImage overlay;                // drawn over the screen (premultiplied ARGB32, stretched to fill)
    int64_t time = 0;              // render time and frame rate (frame history)
    double fps = 30;

    // Filled by finalize():
    std::vector<float> data; // RGBA float texture, dataW x dataH
    int dataW = 0, dataH = 0;

    float& operator[](Global i) { return g[size_t(i)]; }
    float operator[](Global i) const { return g[size_t(i)]; }
    int gw() const { return int(g[G_GW]); }
    int gh() const { return int(g[G_GH]); }
    int cw() const { return int(g[G_CW]); }
    int ch() const { return int(g[G_CH]); }
    bool lookFull() const { return int(g[G_LOOK]) == LookFull; }
    bool usesHistory() const { return freezeKey >= 0 || needPrev || ghost > 0; }
};

// Identity tables and neutral settings for a console screen.
void initPlan(Plan& plan, int gw, int gh, int ax, int ay, int cw, int ch, int tile, QSize layer);
// Packs the tables (data, dataW, dataH, lane globals).
void finalize(Plan& plan);
// Nearest-colour table for a palette (cached; the palette must outlive nothing - it is copied).
const std::vector<uint8_t>* paletteLut(const std::vector<uint32_t>& palette);

// CPU renderer: runs the plan on `layer` (premultiplied ARGB32) in place.
void render(const Plan& plan, QImage& layer, std::unique_ptr<EffectState>& state);

// The effects (ConsoleEffects.cpp).
bool isConsoleEffect(const QString& id);
Plan buildPlan(const QString& id, const Params& p, const RenderContext& ctx, QSize layer);
void addEffects(std::vector<EffectDef>& list);
QString categoryRoot(); // "Broken VGS" (Broken Video Game Systems)

} // namespace vg
