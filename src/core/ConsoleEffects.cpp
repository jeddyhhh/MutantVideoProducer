// "Broken Video Game Systems": one sub-menu per console, each with glitches
// modelled on how that machine's picture really breaks. Every effect is a
// console look (resolution, colours, screen) plus a glitch "kind" that fills
// in a vg::Plan; ConsoleEngine.cpp and the GPU shader render the plan.

#include "core/ConsoleEngine.h"

#include "core/EffectHelpers.h"

#include <QFont>
#include <QFontMetrics>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <map>

namespace vg {

QString categoryRoot() { return QStringLiteral("Broken Video Game Systems"); }

namespace {

constexpr float kPi = 3.14159265358979f;

// ---- Palettes --------------------------------------------------------------------

enum PaletteId { PalNone, PalNes, PalAtari, PalC64, PalZx, PalCga, PalCgaAlt, PalGb, PalVb, PalTms };

uint32_t rgbOf(float r, float g, float b)
{
    auto c = [](float v) { return uint32_t(std::clamp(int(std::lround(v * 255.0f)), 0, 255)); };
    return (c(r) << 16) | (c(g) << 8) | c(b);
}
uint32_t fromYiq(float y, float i, float q)
{
    return rgbOf(y + 0.956f * i + 0.621f * q, y - 0.272f * i - 0.647f * q, y - 1.106f * i + 1.703f * q);
}

// NES PPU (2C02) colours, the widely used emulator table.
std::vector<uint32_t> makeNesPalette()
{
    return {0x7c7c7c, 0x0000fc, 0x0000bc, 0x4428bc, 0x940084, 0xa80020, 0xa81000, 0x881400, 0x503000, 0x007800, 0x006800,
            0x005800, 0x004058, 0x000000, 0x000000, 0x000000, 0xbcbcbc, 0x0078f8, 0x0058f8, 0x6844fc, 0xd800cc, 0xe40058,
            0xf83800, 0xe45c10, 0xac7c00, 0x00b800, 0x00a800, 0x00a844, 0x008888, 0x000000, 0x000000, 0x000000, 0xf8f8f8,
            0x3cbcfc, 0x6888fc, 0x9878f8, 0xf878f8, 0xf85898, 0xf87858, 0xfca044, 0xf8b800, 0xb8f818, 0x58d854, 0x58f898,
            0x00e8d8, 0x787878, 0x000000, 0x000000, 0xfcfcfc, 0xa4e4fc, 0xb8b8f8, 0xd8b8f8, 0xf8b8f8, 0xf8a4c0, 0xf0d0b0,
            0xfce0a8, 0xf8d878, 0xd8f878, 0xb8f8b8, 0xb8f8d8, 0x00fcfc, 0xf8d8f8, 0x000000, 0x000000};
}

// Atari 2600 NTSC: 16 hues x 8 luminances.
std::vector<uint32_t> makeAtariPalette()
{
    std::vector<uint32_t> pal;
    for (int hue = 0; hue < 16; ++hue)
        for (int lum = 0; lum < 8; ++lum) {
            const float y = 0.06f + float(lum) * 0.125f;
            if (hue == 0) {
                pal.push_back(rgbOf(y, y, y));
                continue;
            }
            const float a = (float(hue - 1) * 24.0f + 180.0f) * kPi / 180.0f, s = 0.3f;
            pal.push_back(fromYiq(y, s * std::cos(a), s * std::sin(a)));
        }
    return pal;
}

const std::vector<uint32_t>& paletteOf(int id)
{
    static const std::vector<uint32_t> none;
    static const std::vector<uint32_t> nes = makeNesPalette();
    static const std::vector<uint32_t> atari = makeAtariPalette();
    static const std::vector<uint32_t> c64 = {0x000000, 0xffffff, 0x68372b, 0x70a4b2, 0x6f3d86, 0x588d43, 0x352879, 0xb8c76f,
                                              0x6f4f25, 0x433900, 0x9a6759, 0x444444, 0x6c6c6c, 0x9ad284, 0x6c5eb5, 0x959595};
    static const std::vector<uint32_t> zx = {0x000000, 0x0000d7, 0xd70000, 0xd700d7, 0x00d700, 0x00d7d7, 0xd7d700, 0xd7d7d7,
                                             0x000000, 0x0000ff, 0xff0000, 0xff00ff, 0x00ff00, 0x00ffff, 0xffff00, 0xffffff};
    static const std::vector<uint32_t> cga = {0x000000, 0x55ffff, 0xff55ff, 0xffffff};
    static const std::vector<uint32_t> cgaAlt = {0x000000, 0x55ff55, 0xff5555, 0xffff55};
    static const std::vector<uint32_t> gb = {0x0f380f, 0x306230, 0x8bac0f, 0x9bbc0f};
    static const std::vector<uint32_t> vb = {0x000000, 0x550000, 0xa40000, 0xef0000};
    static const std::vector<uint32_t> tms = {0x000000, 0x21c842, 0x5edc78, 0x5455ed, 0x7d76fc, 0xd4524d, 0x42ebf5, 0xfc5554,
                                              0xff7978, 0xd4c154, 0xe6ce80, 0x21b03b, 0xc95bba, 0xcccccc, 0xffffff};
    switch (id) {
    case PalNes: return nes;
    case PalAtari: return atari;
    case PalC64: return c64;
    case PalZx: return zx;
    case PalCga: return cga;
    case PalCgaAlt: return cgaAlt;
    case PalGb: return gb;
    case PalVb: return vb;
    case PalTms: return tms;
    default: return none;
    }
}

// ---- Consoles ----------------------------------------------------------------------

struct LookSpec {
    const char* key;
    const char* name; // sub-menu
    const char* tag;  // effect name prefix
    int w, h;         // picture
    int bx, by;       // border on each side
    int qmode;
    int bits[3];
    int palette;
    int dither;
    float ditherAmt;
    int screen;
    float screenDef;
    int tile;
    uint32_t backdrop, border;
};

const LookSpec kLooks[] = {
    {"3do", "3DO", "3DO", 320, 240, 0, 0, QBits, {5, 5, 5}, PalNone, 1, 0.7f, ScreenCrt, 0.45f, 16, 0x000000, 0},
    {"a26", "Atari 2600", "Atari 2600", 160, 192, 0, 0, QPalette, {8, 8, 8}, PalAtari, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"jag", "Atari Jaguar", "Jaguar", 320, 240, 0, 0, QBits, {5, 6, 5}, PalNone, 0, 1, ScreenCrt, 0.45f, 16, 0x000000, 0},
    {"lynx", "Atari Lynx", "Lynx", 160, 102, 0, 0, QBits, {4, 4, 4}, PalNone, 0, 1, ScreenLcd, 0.6f, 8, 0x101010, 0},
    {"coleco", "ColecoVision", "ColecoVision", 256, 192, 0, 0, QPalette, {8, 8, 8}, PalTms, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"c64", "Commodore 64", "C64", 320, 200, 32, 36, QPalette, {8, 8, 8}, PalC64, 0, 1, ScreenCrt, 0.5f, 8, 0x352879, 0x6c5eb5},
    {"amiga", "Commodore Amiga", "Amiga", 320, 256, 0, 0, QBits, {4, 4, 4}, PalNone, 0, 1, ScreenCrt, 0.5f, 8, 0x0055aa, 0},
    {"cga", "IBM PC (CGA)", "CGA", 320, 200, 0, 0, QPalette, {8, 8, 8}, PalCga, 0, 1, ScreenCrt, 0.45f, 8, 0x000000, 0},
    {"xbox", "Microsoft Xbox", "Xbox", 640, 480, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenCrt, 0.3f, 16, 0x000000, 0},
    {"x360", "Microsoft Xbox 360", "Xbox 360", 1280, 720, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenNone, 0, 32, 0x000000, 0},
    {"tg16", "NEC TurboGrafx-16 (PC Engine)", "TG-16", 256, 224, 0, 0, QBits, {3, 3, 3}, PalNone, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"n64", "Nintendo 64", "N64", 320, 240, 0, 0, QBits, {5, 5, 5}, PalNone, 1, 1, ScreenCrt, 0.45f, 16, 0x000000, 0},
    {"ds", "Nintendo DS", "DS", 256, 192, 0, 0, QBits, {6, 6, 6}, PalNone, 0, 1, ScreenLcd, 0.45f, 8, 0x000000, 0},
    {"gb", "Nintendo Game Boy", "Game Boy", 160, 144, 0, 0, QShades, {8, 8, 8}, PalGb, 0, 1, ScreenLcd, 0.6f, 8, 0x9bbc0f, 0},
    {"gba", "Nintendo Game Boy Advance", "GBA", 240, 160, 0, 0, QBits, {5, 5, 5}, PalNone, 0, 1, ScreenLcd, 0.5f, 8, 0x000000, 0},
    {"gbc", "Nintendo Game Boy Color", "Game Boy Color", 160, 144, 0, 0, QBits, {5, 5, 5}, PalNone, 0, 1, ScreenLcd, 0.55f, 8, 0xf8f8f8, 0},
    {"gc", "Nintendo GameCube", "GameCube", 640, 480, 0, 0, QBits, {6, 6, 6}, PalNone, 1, 1, ScreenCrt, 0.3f, 16, 0x000000, 0},
    {"nes", "Nintendo NES (Famicom)", "NES", 256, 240, 0, 0, QPalette, {8, 8, 8}, PalNes, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"snes", "Nintendo SNES (Super Famicom)", "SNES", 256, 224, 0, 0, QBits, {5, 5, 5}, PalNone, 0, 1, ScreenCrt, 0.5f, 8, 0x3858a8, 0},
    {"switch", "Nintendo Switch", "Switch", 1280, 720, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenNone, 0, 32, 0x000000, 0},
    {"vb", "Nintendo Virtual Boy", "Virtual Boy", 384, 224, 0, 0, QShades, {8, 8, 8}, PalVb, 0, 1, ScreenNone, 0, 8, 0x000000, 0},
    {"wii", "Nintendo Wii", "Wii", 640, 480, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenCrt, 0.3f, 16, 0x000000, 0},
    {"dc", "Sega Dreamcast", "Dreamcast", 640, 480, 0, 0, QBits, {5, 6, 5}, PalNone, 1, 1, ScreenCrt, 0.25f, 16, 0x000000, 0},
    {"gg", "Sega Game Gear", "Game Gear", 160, 144, 0, 0, QBits, {4, 4, 4}, PalNone, 0, 1, ScreenLcd, 0.6f, 8, 0x000000, 0},
    {"gen", "Sega Genesis (Mega Drive)", "Genesis", 320, 224, 0, 0, QBits, {3, 3, 3}, PalNone, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"sms", "Sega Master System", "Master System", 256, 192, 0, 0, QBits, {2, 2, 2}, PalNone, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0},
    {"saturn", "Sega Saturn", "Saturn", 320, 224, 0, 0, QBits, {5, 5, 5}, PalNone, 0, 1, ScreenCrt, 0.45f, 16, 0x000000, 0},
    {"zx", "Sinclair ZX Spectrum", "ZX Spectrum", 256, 192, 32, 24, QPalette, {8, 8, 8}, PalZx, 0, 1, ScreenCrt, 0.5f, 8, 0x000000, 0xd7d7d7},
    {"neo", "SNK Neo Geo", "Neo Geo", 320, 224, 0, 0, QBits, {5, 5, 5}, PalNone, 0, 1, ScreenCrt, 0.5f, 16, 0x000000, 0},
    {"ps1", "Sony PlayStation", "PS1", 320, 240, 0, 0, QBits, {5, 5, 5}, PalNone, 2, 1, ScreenCrt, 0.45f, 16, 0x000000, 0},
    {"ps2", "Sony PlayStation 2", "PS2", 640, 448, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenCrt, 0.35f, 16, 0x000000, 0},
    {"ps3", "Sony PlayStation 3", "PS3", 1280, 720, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenNone, 0, 32, 0x000000, 0},
    {"psp", "Sony PSP", "PSP", 480, 272, 0, 0, QNone, {8, 8, 8}, PalNone, 0, 1, ScreenLcd, 0.3f, 16, 0x000000, 0},
    {"vectrex", "Vectrex", "Vectrex", 320, 240, 0, 0, QVector, {8, 8, 8}, PalNone, 0, 1, ScreenNone, 0, 8, 0x000000, 0},
};

// ---- Glitches ------------------------------------------------------------------------

enum Kind {
    KGarble,      // 0 scatter, 1 clusters, 2 garbage screen, 3 texture blocks, 4 glitch city, 5 cartridge tilt
    KFlicker,     // sprite multiplexing / overflow
    KLockout,     // lockout chip reset loop
    KScroll,      // 0 split error, 1 mirroring error
    KRoll,        // 0 vertical hold, 1 wrong region TV, 2 collapse
    KHmove,       // Atari HMOVE combs and jitter
    KRowBars,     // 0 dirty-cartridge rainbow, 1 copper list, 2 framebuffer RAM fault
    KPalette,     // 0 corruption, 1 palette swap, 2 colour loss, 3 per-tile palettes
    KBits,        // 0 stuck data bits, 1 missing bitplane
    KCable,       // 0 dead gun, 1 swapped wires, 2 component tint, 3 bad AV cable
    KJailbars,    // 0 bars, 1 thin lines
    KDeadLines,   // 0 columns, 1 ribbon cable, 2 dead pixels, 3 rows + columns, 4 line loss, 5 hinge cable
    KGhost,       // LCD persistence
    KDim,         // 0 unlit, 1 bad capacitors, 2 contrast drift, 3 dying backlight, 4 brightness register
    KMode7,       // SNES perspective plane with corrupted HDMA rows
    KAffine,      // GBA affine bands
    KWave,        // 0 HDMA wave, 1 line scroll tear, 2 Virtual Boy mirror wobble
    KMosaic,      // stuck mosaic register
    KColorMath,   // 0 SNES colour math, 1 half-pixel ghost, 2 3D misalignment, 3 mesh transparency
    KMeshExplode, // vertex explosion
    KMeshWobble,  // integer vertices jitter, affine textures
    KMeshCracks,  // gaps between polygons
    KMeshTexture, // 0 scrambled texture coordinates, 1 smeared (cartridge tilt)
    KMeshWarp,    // big quads distorting
    KMeshDrop,    // missing / z-fighting polygons
    KArtifacts,   // 0 dot grid, 1 checkerboard, 2 vertical lines, 3 blocks + lines
    KFreeze,      // 0 read stutter, 1 hard crash, 2 disc read error, 3 Guru Meditation
    KInterlace,   // field combing
    KTear,        // screen tearing
    KSparkle,     // 0 VIC-II sparkle, 1 CRAM dots, 2 CGA snow, 3 HDMI sparkles
    KAttr,        // ZX attribute clash corruption
    KTape,        // 0 ZX Spectrum loading, 1 C64 loading
    KCgaMode,     // wrong video mode (interleaved rows)
    KVector,      // 0 beam wobble, 1 phosphor burn, 2 collapsed deflection
};

struct Spec {
    const char* look;
    const char* id;
    const char* name;
    Kind kind;
    int variant;
    float amount, speed;
    const char* description;
};

const Spec kSpecs[] = {
    // 3DO
    {"3do", "warp", "Polygon Warping", KMeshWarp, 0, 0.55f, 1.0f, "Big textured quads bend and swim as the geometry engine loses precision."},
    {"3do", "texture", "Texture Corruption", KMeshTexture, 0, 0.5f, 2.0f, "Polygons pick up the wrong textures from a bad CD read."},
    {"3do", "freeze", "Frozen Load", KFreeze, 0, 0.6f, 0.4f, "The picture locks up for a moment while the drive struggles."},
    // Atari 2600
    {"a26", "roll", "Rolling Picture", KRoll, 0, 0.6f, 0.5f, "The game sends the wrong number of scanlines and the TV loses vertical hold."},
    {"a26", "hmove", "HMOVE Combs", KHmove, 0, 0.6f, 8.0f, "Black comb marks on the left edge and jittering lines from mistimed object moves."},
    {"a26", "rainbow", "Dirty Cartridge Rainbow", KRowBars, 0, 0.6f, 6.0f, "Garbage register writes paint bands of random colour across the screen."},
    {"a26", "flicker", "Flicker Overload", KFlicker, 0, 0.55f, 2.0f, "Too many objects: the game multiplexes them and everything flickers."},
    {"a26", "colorloss", "Colour Switch Glitch", KPalette, 2, 0.6f, 1.5f, "The colour / B&W switch contact fails and the picture drops to grey."},
    // Atari Jaguar
    {"jag", "scramble", "Texture Scramble", KGarble, 3, 0.45f, 2.0f, "Blocks of the picture come from the wrong place in memory."},
    {"jag", "explode", "Polygon Explosion", KMeshExplode, 0, 0.5f, 1.5f, "A few vertices shoot off to infinity, stretching spikes across the screen."},
    {"jag", "crash", "Crash", KFreeze, 1, 0.6f, 0.25f, "The console hangs with a band of garbage across the frozen picture."},
    // Atari Lynx
    {"lynx", "smear", "LCD Smear", KGhost, 0, 0.6f, 0.0f, "The slow passive LCD leaves a long smear behind anything that moves."},
    {"lynx", "backlight", "Dying Backlight", KDim, 3, 0.6f, 1.0f, "The backlight flickers and fades unevenly."},
    {"lynx", "lines", "Dead Rows & Columns", KDeadLines, 3, 0.4f, 0.5f, "Broken LCD connections leave dead rows and columns."},
    // ColecoVision
    {"coleco", "flicker", "Sprite Flicker", KFlicker, 0, 0.55f, 2.0f, "More than four sprites on a line: the video chip drops them and they flicker."},
    {"coleco", "vram", "VRAM Garbage", KGarble, 0, 0.5f, 2.0f, "Bad video RAM fills the screen with wrong pattern tiles."},
    // Commodore 64
    {"c64", "sparkle", "VIC-II Sparkle", KSparkle, 0, 0.5f, 0.0f, "A failing VIC-II chip throws bright sparkles over the picture."},
    {"c64", "garbage", "Garbage Screen", KGarble, 2, 0.7f, 0.5f, "Bad RAM or a dead PLA: the screen fills with random characters and colours."},
    {"c64", "tape", "Tape Loading", KTape, 1, 0.3f, 0.12f, "Loading from tape: striped border while the picture appears line by line."},
    {"c64", "raster", "Raster Split Jitter", KScroll, 0, 0.5f, 3.0f, "Raster interrupts fire at the wrong time and the screen splits and shakes."},
    // Commodore Amiga
    {"amiga", "guru", "Guru Meditation", KFreeze, 3, 0.7f, 1.0f, "Software failure: the machine halts under a blinking red alert box."},
    {"amiga", "copper", "Copper List Corruption", KRowBars, 1, 0.6f, 2.0f, "A corrupted copper list paints colour bars and wrong registers down the screen."},
    {"amiga", "bitplane", "Missing Bitplane", KBits, 1, 0.6f, 1.0f, "Bitplanes go missing or get stuck, so colours collapse and shift."},
    // IBM PC (CGA)
    {"cga", "snow", "CGA Snow", KSparkle, 2, 0.5f, 0.0f, "Writing to video memory during display: streaks of snow all over the screen."},
    {"cga", "mode", "Wrong Video Mode", KCgaMode, 0, 0.5f, 1.0f, "The picture is read in the wrong mode: interleaved lines show two squashed copies."},
    {"cga", "palette", "Palette Swap", KPalette, 1, 0.6f, 1.0f, "The palette register flips between cyan/magenta and red/green."},
    // Microsoft Xbox
    {"xbox", "freeze", "Hard Freeze", KFreeze, 1, 0.6f, 0.25f, "The console locks up with garbage across a frozen frame."},
    {"xbox", "texture", "Texture Corruption", KMeshTexture, 0, 0.45f, 2.0f, "Polygons sample the wrong textures."},
    {"xbox", "av", "Bad AV Cable", KCable, 3, 0.6f, 4.0f, "A loose composite cable: jitter, washed-out colour and noise."},
    // Microsoft Xbox 360
    {"x360", "dots", "Red Ring Artifacts", KArtifacts, 0, 0.6f, 0.3f, "Failing GPU memory sprinkles a grid of coloured dots over everything."},
    {"x360", "checker", "Checkerboard of Death", KArtifacts, 1, 0.6f, 0.3f, "A dying GPU turns patches of the screen into checkerboards."},
    {"x360", "lines", "Vertical Line Artifacts", KArtifacts, 2, 0.6f, 0.3f, "Coloured vertical lines from a cracked GPU solder joint."},
    {"x360", "freeze", "Hard Freeze", KFreeze, 1, 0.6f, 0.25f, "The picture freezes and garbage bands appear."},
    // NEC TurboGrafx-16
    {"tg16", "hucard", "Dirty HuCard", KGarble, 0, 0.45f, 2.0f, "Dirty card contacts garble tiles and colours."},
    {"tg16", "flicker", "Sprite Dropout", KFlicker, 0, 0.55f, 2.0f, "Sprite overflow makes objects flicker and vanish."},
    {"tg16", "jailbars", "Composite Jailbars", KJailbars, 0, 0.6f, 0.0f, "Vertical bars from noise on the composite video output."},
    // Nintendo 64
    {"n64", "tilt", "Cartridge Tilt", KMeshTexture, 1, 0.55f, 2.0f, "The tilted cartridge returns bad data: textures smear into streaks and colours break."},
    {"n64", "explode", "Polygon Explosion", KMeshExplode, 0, 0.5f, 1.5f, "Corrupted vertices fly off and stretch polygons into spikes."},
    {"n64", "expansion", "Expansion Pak Fault", KRowBars, 2, 0.55f, 3.0f, "Bad RDRAM: framebuffer rows turn to noise, repeat or flip bits."},
    {"n64", "jailbars", "Jailbars", KJailbars, 1, 0.6f, 0.0f, "Faint vertical lines from the video encoder."},
    {"n64", "zbuffer", "Z-Buffer Corruption", KMeshDrop, 0, 0.5f, 0.0f, "Polygons drop out and fight each other for depth."},
    // Nintendo DS
    {"ds", "hinge", "Hinge Cable Failure", KDeadLines, 5, 0.55f, 3.0f, "The worn hinge ribbon cable: white-outs, coloured lines and corrupted bands."},
    {"ds", "pixels", "Dead Pixels", KDeadLines, 2, 0.5f, 0.0f, "Stuck and dead pixels on the LCD."},
    {"ds", "jitter", "3D Polygon Jitter", KMeshWobble, 0, 0.5f, 0.0f, "Low-precision 3D: polygons wobble and snap between pixels."},
    // Nintendo Game Boy
    {"gb", "lines", "Vertical Line Death", KDeadLines, 0, 0.45f, 0.5f, "The LCD ribbon cable fails and columns of pixels go blank."},
    {"gb", "ghost", "LCD Ghosting", KGhost, 0, 0.6f, 0.0f, "The slow LCD leaves ghost trails behind motion."},
    {"gb", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "Dirty pins scramble the background tiles."},
    {"gb", "glitchcity", "Glitch City", KGarble, 4, 0.55f, 0.7f, "Corrupted map data: runs of the wrong tiles, mirrored blocks and swapped shades."},
    {"gb", "contrast", "Contrast Wheel Drift", KDim, 2, 0.6f, 0.6f, "A scratchy contrast pot: the screen drifts between too dark and washed out."},
    // Nintendo Game Boy Advance
    {"gba", "unlit", "Unlit Screen", KDim, 0, 0.6f, 0.0f, "The original front-lit-less screen: dark, murky and low contrast."},
    {"gba", "ribbon", "Ribbon Cable Lines", KDeadLines, 1, 0.5f, 6.0f, "A damaged ribbon cable: flickering lines and corrupted bands."},
    {"gba", "affine", "Affine Sprite Glitch", KAffine, 0, 0.5f, 1.5f, "Rotation/scaling parameters go wrong: bands of the screen spin and stretch."},
    {"gba", "tilt", "Cartridge Tilt", KGarble, 5, 0.5f, 2.0f, "A tilted cartridge: runs of garbage tiles and stuck colour bits."},
    // Nintendo Game Boy Color
    {"gbc", "palette", "Palette Corruption", KPalette, 3, 0.5f, 1.5f, "Tiles pick up the wrong colour palettes."},
    {"gbc", "lines", "Dead Lines", KDeadLines, 0, 0.4f, 0.5f, "Dead columns from a failing LCD connection."},
    {"gbc", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "Dirty pins scramble tiles and colours."},
    {"gbc", "unlit", "Dim Screen", KDim, 0, 0.5f, 0.0f, "The unlit reflective screen in bad light."},
    // Nintendo GameCube
    {"gc", "disc", "Disc Read Freeze", KFreeze, 0, 0.6f, 0.5f, "The disc drive struggles: the picture freezes and stutters."},
    {"gc", "texture", "Texture Corruption", KMeshTexture, 0, 0.45f, 2.0f, "Polygons pick up the wrong textures."},
    {"gc", "component", "Component Cable Fault", KCable, 2, 0.6f, 1.0f, "A failing component cable tints the picture green or pink."},
    {"gc", "dither", "Dither Banding", KBits, 2, 0.6f, 1.0f, "Colour depth drops: heavy dithering and banding."},
    // Nintendo NES
    {"nes", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "The classic dirty cartridge: garbled tiles and wrong pattern data."},
    {"nes", "blink", "Blinking Lockout", KLockout, 0, 0.6f, 1.0f, "The lockout chip resets the console once a second: blank flashes and garbage."},
    {"nes", "split", "Scroll Split Error", KScroll, 0, 0.5f, 3.0f, "The status-bar split fires at the wrong line: the playfield jumps and shakes."},
    {"nes", "flicker", "Sprite Overflow Flicker", KFlicker, 0, 0.55f, 2.0f, "More than eight sprites on a line: they flicker and vanish."},
    {"nes", "palette", "Palette Corruption", KPalette, 0, 0.5f, 1.5f, "Palette RAM gets wrong values: colours swap around."},
    {"nes", "mirror", "Mirroring Error", KScroll, 1, 0.5f, 1.0f, "Wrong nametable mirroring: the screen repeats itself as it scrolls."},
    // Nintendo SNES
    {"snes", "mode7", "Mode 7 Meltdown", KMode7, 0, 0.45f, 0.5f, "The picture becomes a spinning Mode 7 floor and corrupted HDMA rows tear it apart."},
    {"snes", "hdma", "HDMA Wave Corruption", KWave, 0, 0.5f, 1.0f, "Per-line scroll tables go wrong: waves and torn lines."},
    {"snes", "mosaic", "Stuck Mosaic", KMosaic, 0, 0.6f, 2.0f, "The mosaic register gets stuck at random sizes."},
    {"snes", "colormath", "Colour Math Error", KColorMath, 0, 0.6f, 1.5f, "Colour addition and subtraction blend the screen with an offset copy."},
    {"snes", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "Dirty contacts garble tiles."},
    {"snes", "brightness", "Brightness Register Glitch", KDim, 4, 0.5f, 3.0f, "Master brightness jumps around and lines are force-blanked."},
    // Nintendo Switch
    {"switch", "hdmi", "Dock HDMI Sparkles", KSparkle, 3, 0.5f, 0.3f, "A marginal HDMI link: sparkles and black dropouts."},
    {"switch", "tear", "Screen Tearing", KTear, 0, 0.6f, 1.0f, "Frames swap mid-scan and the picture tears."},
    {"switch", "gpu", "GPU Crash Artifacts", KArtifacts, 1, 0.5f, 0.5f, "A crashing GPU leaves checkerboard blocks."},
    // Nintendo Virtual Boy
    {"vb", "lines", "Display Line Loss", KDeadLines, 4, 0.5f, 4.0f, "The display cable fails and horizontal lines drop out."},
    {"vb", "mirror", "Mirror Wobble", KWave, 2, 0.5f, 1.0f, "The oscillating mirror loses sync: wobbling, doubled picture."},
    {"vb", "3d", "3D Misalignment", KColorMath, 2, 0.6f, 0.0f, "Left and right images no longer line up."},
    // Nintendo Wii
    {"wii", "gpu", "GPU Artifacting", KArtifacts, 3, 0.5f, 0.3f, "A failing GPU: garbled blocks and coloured lines."},
    {"wii", "disc", "Disc Read Freeze", KFreeze, 0, 0.6f, 0.5f, "The disc drive struggles: the picture freezes and stutters."},
    {"wii", "interlace", "480i Combing", KInterlace, 0, 0.5f, 0.0f, "Interlaced fields from different moments comb moving edges."},
    // Sega Dreamcast
    {"dc", "gdrom", "GD-ROM Freeze", KFreeze, 0, 0.6f, 0.5f, "A worn GD-ROM drive makes the game freeze and stutter."},
    {"dc", "vga", "VGA Colour Fault", KCable, 1, 0.6f, 1.0f, "A bad VGA box swaps the colour wires: pink and green pictures."},
    {"dc", "explode", "Polygon Explosion", KMeshExplode, 0, 0.5f, 1.5f, "Vertices fly off and stretch polygons across the screen."},
    {"dc", "jailbars", "Composite Jailbars", KJailbars, 0, 0.5f, 0.0f, "Vertical bars on the composite output."},
    // Sega Game Gear
    {"gg", "caps", "Bad Capacitors", KDim, 1, 0.6f, 1.0f, "Leaking capacitors: faded, flickering, wavy picture."},
    {"gg", "smear", "LCD Smear", KGhost, 0, 0.65f, 0.0f, "The blurry passive LCD smears motion."},
    {"gg", "lines", "Dead Lines", KDeadLines, 0, 0.4f, 0.5f, "Broken LCD connections leave dead columns."},
    // Sega Genesis
    {"gen", "jailbars", "Jailbars", KJailbars, 0, 0.6f, 0.0f, "The infamous vertical bars of the Model 2 video output."},
    {"gen", "cram", "CRAM Dots", KSparkle, 1, 0.5f, 0.0f, "Palette writes during display leave coloured dots on the screen."},
    {"gen", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "Dirty contacts garble tiles."},
    {"gen", "linescroll", "Line Scroll Tear", KWave, 1, 0.5f, 0.0f, "Per-line scroll values go wrong and bands of the screen tear sideways."},
    {"gen", "region", "Region Mismatch", KRoll, 1, 0.6f, 0.15f, "A console from the wrong region: the TV rolls the picture and loses colour."},
    // Sega Master System
    {"sms", "dirty", "Dirty Cartridge", KGarble, 0, 0.45f, 2.0f, "Dirty contacts garble tiles."},
    {"sms", "flicker", "Sprite Flicker", KFlicker, 0, 0.55f, 2.0f, "More than eight sprites on a line: they flicker and vanish."},
    {"sms", "split", "Line Scroll Split", KScroll, 0, 0.5f, 3.0f, "The scroll split happens on the wrong line."},
    {"sms", "cram", "Colour RAM Corruption", KPalette, 0, 0.5f, 1.5f, "Colour RAM holds wrong values: colours swap and invert."},
    // Sega Saturn
    {"saturn", "warp", "Polygon Warping", KMeshWarp, 0, 0.55f, 1.0f, "Distorted quads: the picture folds and swims."},
    {"saturn", "mesh", "Mesh Transparency Glitch", KColorMath, 3, 0.6f, 1.0f, "Checkerboard mesh transparency shows a displaced copy of the screen."},
    {"saturn", "vdp2", "VDP2 Tile Corruption", KGarble, 0, 0.45f, 2.0f, "Background tiles come from the wrong place."},
    {"saturn", "interlace", "Interlace Combing", KInterlace, 0, 0.5f, 0.0f, "High-resolution interlace combs moving edges."},
    // Sinclair ZX Spectrum
    {"zx", "clash", "Attribute Clash Chaos", KAttr, 0, 0.4f, 1.5f, "Two colours per 8x8 cell, and corrupted attributes flash the wrong ones."},
    {"zx", "tape", "Tape Loading", KTape, 0, 0.3f, 0.12f, "Loading from tape: pilot-tone border stripes, then the picture arrives in the Spectrum's odd line order."},
    {"zx", "crash", "Crash Garbage", KGarble, 2, 0.7f, 0.5f, "A crash dumps random bytes into screen memory."},
    // SNK Neo Geo
    {"neo", "sprites", "Bad Sprite ROM", KGarble, 3, 0.45f, 2.0f, "Bad sprite ROM data: blocks of the picture from the wrong place."},
    {"neo", "gun", "Dead Colour Channel", KCable, 0, 0.6f, 1.0f, "One of the RGB outputs drops out."},
    {"neo", "palette", "Palette RAM Fault", KBits, 0, 0.6f, 1.0f, "Stuck bits in palette RAM shift every colour."},
    // Sony PlayStation
    {"ps1", "wobble", "Wobbly Polygons", KMeshWobble, 0, 0.5f, 0.0f, "Integer vertices and affine textures: everything wobbles and warps."},
    {"ps1", "cracks", "Polygon Cracks", KMeshCracks, 0, 0.5f, 0.0f, "Gaps open between polygons and sparkle."},
    {"ps1", "explode", "Polygon Explosion", KMeshExplode, 0, 0.5f, 1.5f, "Vertices shoot off and stretch polygons into spikes."},
    {"ps1", "disc", "Disc Read Stutter", KFreeze, 0, 0.6f, 0.5f, "A worn laser: the game freezes and stutters while it rereads the disc."},
    {"ps1", "texture", "Texture Corruption", KMeshTexture, 0, 0.45f, 2.0f, "Bad reads load the wrong textures."},
    {"ps1", "pal", "NTSC on a PAL TV", KRoll, 1, 0.8f, 0.2f, "The wrong TV standard: black and white picture rolling slowly."},
    // Sony PlayStation 2
    {"ps2", "disc", "Disc Read Error", KFreeze, 2, 0.6f, 0.2f, "The game freezes, then drops to a disc read error screen."},
    {"ps2", "interlace", "Interlace Combing", KInterlace, 0, 0.5f, 0.0f, "Field rendering combs moving edges."},
    {"ps2", "explode", "Vertex Explosion", KMeshExplode, 0, 0.5f, 1.5f, "Vertices fly off and stretch spikes across the screen."},
    {"ps2", "ghost", "Half-Pixel Ghosting", KColorMath, 1, 0.6f, 0.0f, "Misaligned post-processing leaves a soft double image."},
    // Sony PlayStation 3
    {"ps3", "ylod", "YLOD Artifacting", KArtifacts, 1, 0.6f, 0.3f, "A failing RSX GPU breaks the picture into checkerboard blocks."},
    {"ps3", "hdmi", "HDMI Dropout", KSparkle, 3, 0.5f, 0.3f, "The HDMI handshake keeps failing: black flashes and sparkles."},
    {"ps3", "lines", "RSX Vertical Lines", KArtifacts, 2, 0.6f, 0.3f, "Coloured vertical lines from a failing GPU."},
    {"ps3", "freeze", "Hard Freeze", KFreeze, 1, 0.6f, 0.25f, "The console locks up with garbage across the frozen frame."},
    // Sony PSP
    {"psp", "ghost", "LCD Ghosting", KGhost, 0, 0.55f, 0.0f, "The slow first-generation LCD smears motion."},
    {"psp", "pixels", "Dead Pixels", KDeadLines, 2, 0.5f, 0.0f, "Stuck and dead pixels."},
    {"psp", "umd", "UMD Read Stutter", KFreeze, 0, 0.6f, 0.5f, "The UMD drive struggles: freezes and stutters."},
    // Vectrex
    {"vectrex", "wobble", "Beam Wobble", KVector, 0, 0.5f, 1.0f, "Vector display: the deflection drifts and the beam wobbles."},
    {"vectrex", "burn", "Phosphor Burn", KVector, 1, 0.6f, 0.0f, "Long phosphor persistence trails every line."},
    {"vectrex", "collapse", "Collapsed Deflection", KVector, 2, 0.6f, 0.5f, "The deflection amplifier fails and the picture collapses to a line."},
};

// ---- Plan building --------------------------------------------------------------------

struct Rng {
    uint32_t s;
    uint32_t u()
    {
        s = fx::hash32(s, 0x2545f491u, 0x9e3779b9u);
        return s;
    }
    float f() { return float(u() & 0xffffffu) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * f(); }
    int i(int n) { return n <= 0 ? 0 : int(u() % uint32_t(n)); }
    bool chance(float p) { return f() < p; }
};

struct Builder {
    Plan& plan;
    const LookSpec& look;
    const Spec& spec;
    const Params& p;
    const RenderContext& ctx;
    float A = 0, speed = 0;
    uint32_t seed = 0, frame = 0;
    double t = 0;
    int64_t step = 0;
    int CW = 0, CH = 0, GW = 0, GH = 0, TX = 0, TY = 0, AX = 0, AY = 0;

    Rng rng(uint32_t salt) const { return Rng{fx::hash32(uint32_t(step), seed, salt)}; }
    Rng frameRng(uint32_t salt) const { return Rng{fx::hash32(frame, seed, salt + 1000u)}; }
    Rng fixedRng(uint32_t salt) const { return Rng{fx::hash32(seed, 0x51u, salt)}; }
    int palN() const { return int(plan.palette.size()); }
    bool hasIndex() const
    {
        const int q = int(plan[G_QMODE]);
        return palN() > 0 && (q == QPalette || q == QShades);
    }
    // A colour the console can show.
    uint32_t colour(Rng& r) const
    {
        if (palN() > 0)
            return plan.palette[size_t(r.i(palN()))];
        const uint32_t c = r.u() & 0xffffffu;
        return c | 0x202020u;
    }
    Px4& tile(int x, int y) { return plan.tiles[size_t(std::clamp(y, 0, TY - 1) * TX + std::clamp(x, 0, TX - 1))]; }
    Px4& rowMap(int ly) { return plan.rowMap[size_t(ly + AY)]; }
    Px4& rowFx(int ly) { return plan.rowFx[size_t(ly + AY)]; }
    Px4& rowEx(int ly) { return plan.rowEx[size_t(ly + AY)]; }
    Px4& rowEx2(int ly) { return plan.rowEx2[size_t(ly + AY)]; }
    void fillRow(int ly, uint32_t rgb, float opacity)
    {
        rowFx(ly)[1] = float(rgb & 0xffffffu);
        rowEx(ly)[3] = opacity;
    }
    void allRowsHeld()
    {
        for (auto& r : plan.rowEx)
            r[1] = 1;
    }
    void garbageTile(Px4& t, Rng& r) { t = {float(r.i(std::max(1, palN()))), 0, float(TileGarbage), float(r.u() & 0xffffffu)}; }
    void copyTile(Px4& t, Rng& r, bool flips)
    {
        int m = TileCopy;
        if (flips && r.chance(0.3f))
            m |= 8;
        if (flips && r.chance(0.2f))
            m |= 16;
        t = {float(r.i(TX)), float(r.i(TY)), float(m), 0};
    }
};

// Mesh helpers: a grid of vertices in 0..1, emitted as triangles.
struct Grid {
    int nx, ny;
    std::vector<std::array<float, 2>> pos, uv;
    Grid(int x, int y) : nx(x), ny(y)
    {
        for (int j = 0; j <= ny; ++j)
            for (int i = 0; i <= nx; ++i) {
                const std::array<float, 2> v = {float(i) / float(nx), float(j) / float(ny)};
                pos.push_back(v);
                uv.push_back(v);
            }
    }
    int at(int i, int j) const { return j * (nx + 1) + i; }
    // Triangles as vertex index triples; alternate = flip the diagonal on a checkerboard.
    std::vector<std::array<int, 3>> tris(bool alternate) const
    {
        std::vector<std::array<int, 3>> t;
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const int a = at(i, j), b = at(i + 1, j), c = at(i, j + 1), d = at(i + 1, j + 1);
                if (alternate && ((i + j) & 1)) {
                    t.push_back({a, b, c});
                    t.push_back({b, d, c});
                } else {
                    t.push_back({a, b, d});
                    t.push_back({a, d, c});
                }
            }
        return t;
    }
};
void emitVertex(Plan& plan, const std::array<float, 2>& p, const std::array<float, 2>& t) { plan.mesh.push_back({p[0], p[1], t[0], t[1]}); }
float snap(float v, int n) { return std::floor(v * float(n) + 0.5f) / float(n); }

void kindGarble(Builder& b, int variant)
{
    Rng r = b.rng(1);
    const int n = b.TX * b.TY;
    if (variant == 0 || variant == 3) {
        const float share = variant == 3 ? b.A * 0.6f : b.A * 0.45f;
        for (Px4& t : b.plan.tiles) {
            if (!r.chance(share))
                continue;
            const float x = variant == 3 ? 0.0f : r.f();
            if (x < 0.45f)
                b.copyTile(t, r, true);
            else if (x < 0.85f)
                b.garbageTile(t, r);
            else
                t = {0, 0, float(TileSolid), float(b.colour(r))};
        }
        if (variant == 0) { // a few tiles change every frame as the contacts crackle
            Rng fr = b.frameRng(2);
            for (int k = 0; k < int(b.A * float(n) * 0.03f); ++k)
                b.garbageTile(b.plan.tiles[size_t(fr.i(n))], fr);
        }
    } else if (variant == 1 || variant == 4 || variant == 5) {
        const int clusters = 1 + int(b.A * 6.0f);
        for (int k = 0; k < clusters; ++k) {
            const int start = r.i(n), len = 3 + r.i(int(4 + b.A * float(b.TX) * 4.0f));
            const bool copy = r.chance(0.5f);
            const int from = r.i(n);
            const uint32_t s0 = r.u() & 0xffffu;
            const int flip = variant == 4 && r.chance(0.4f) ? 8 : 0;
            for (int i = 0; i < len; ++i) {
                Px4& t = b.plan.tiles[size_t((start + i) % n)];
                if (copy) {
                    const int src = (from + (flip ? len - i : i)) % n;
                    t = {float(src % b.TX), float(src / b.TX), float(TileCopy | flip), 0};
                } else {
                    t = {float((r.i(std::max(1, b.palN())))), 0, float(TileGarbage), float(s0 + uint32_t(i % 5))};
                }
            }
        }
        if (variant == 4) { // swapped shades on some blocks
            for (int k = 0; k < int(b.A * 12.0f); ++k) {
                const int tx = r.i(b.TX), ty = r.i(b.TY);
                for (int y = ty; y < ty + 2; ++y)
                    for (int x = tx; x < tx + 3; ++x)
                        if (b.tile(x, y)[2] == 0)
                            b.tile(x, y) = {float(1 + r.i(3)), b.hasIndex() ? 0.0f : float(r.i(6)), float(TileShift), 0};
            }
        }
        if (variant == 5) { // stuck data lines
            Rng br = b.rng(5);
            for (Global gch : {G_AND_R, G_AND_G, G_AND_B})
                if (br.chance(b.A))
                    b.plan[gch] = float(0xff & ~(1 << (7 - br.i(3))));
            if (br.chance(b.A * 0.5f))
                b.plan[G_XOR_G] = 64;
        }
    } else if (variant == 2) {
        // A screen full of junk: a handful of repeating "characters" in random colours.
        b.plan[G_GARB_BPP] = (QString::fromLatin1(b.look.key) == QLatin1String("c64") || QString::fromLatin1(b.look.key) == QLatin1String("zx")) ? 1 : 2;
        uint32_t chars[12];
        for (uint32_t& c : chars)
            c = r.u() & 0xffffffu;
        const float share = 0.55f + 0.45f * b.A;
        for (Px4& t : b.plan.tiles)
            if (r.chance(share))
                t = {float(r.i(std::max(1, b.palN()))), 0, float(TileGarbage), float(chars[r.i(12)])};
        Rng fr = b.frameRng(3);
        for (int k = 0; k < int(b.A * float(n) * 0.02f); ++k)
            b.garbageTile(b.plan.tiles[size_t(fr.i(n))], fr);
    }
}

void kindFlicker(Builder& b)
{
    Rng r = b.rng(7);
    for (int ty = 0; ty < b.TY; ++ty) {
        if (!r.chance(b.A * 0.6f))
            continue;
        const int x0 = r.i(b.TX), len = 2 + r.i(2 + int(b.A * 8.0f));
        const int phase = int((b.frame + uint32_t(ty)) & 1u);
        for (int x = x0; x < x0 + len; ++x)
            if (((x + phase) & 1) == 0)
                b.tile(x % b.TX, ty) = {0, 0, float(TileHide), 0};
    }
}

void kindLockout(Builder& b)
{
    const double cycle = b.t * std::max(0.05f, b.speed);
    const int64_t k = int64_t(std::floor(cycle));
    const float ph = float(cycle - double(k));
    Rng r{fx::hash32(uint32_t(k), b.seed, 9u)};
    const float blank = 0.2f + 0.45f * b.A;
    if (ph < blank) {
        const uint32_t cols[4] = {0x000000, 0x666666, 0x101010, 0x8c1810};
        b.plan[G_BLANK] = 1;
        b.plan[G_BLANK_COLOR] = float(cols[r.i(4)]);
    } else if (ph < blank + 0.08f) {
        for (Px4& t : b.plan.tiles)
            if (r.chance(0.8f))
                b.garbageTile(t, r);
    } else {
        const float dy = r.range(-1.0f, 1.0f) * b.A * 10.0f;
        for (int y = 0; y < b.CH; ++y)
            b.rowMap(y)[2] += dy;
    }
}

void kindScroll(Builder& b, int variant)
{
    if (variant == 0) {
        Rng r = b.rng(11);
        Rng fr = b.frameRng(12);
        const int status = int(float(b.CH) * r.range(0.08f, 0.2f));
        const int splits = 1 + int(b.A * 2.99f);
        std::vector<int> ys;
        for (int k = 0; k < splits; ++k)
            ys.push_back(std::clamp(status + r.i(b.CH - status) + int(fr.range(-1, 1) * b.A * 3.0f), status, b.CH - 1));
        std::sort(ys.begin(), ys.end());
        int band = 0;
        float dx = 0, dy = 0;
        for (int y = status; y < b.CH; ++y) {
            while (band < int(ys.size()) && y >= ys[size_t(band)]) {
                dx = r.range(-1, 1) * float(b.CW) * 0.5f * b.A + fr.range(-1, 1) * b.A * 4.0f;
                dy = r.range(-1, 1) * float(b.CH) * 0.3f * b.A;
                b.rowEx(y)[2] = 0.6f; // a line of junk where the split lands
                ++band;
            }
            b.rowMap(y)[0] = dx;
            b.rowMap(y)[2] += dy;
        }
    } else {
        const float half = float(b.CH) * 0.5f;
        const float scroll = float(std::fmod(b.t * b.speed * double(b.CH) * 0.15, double(half)));
        const float xs = float(std::fmod(b.t * b.speed * double(b.CW) * 0.08 * b.A, double(b.CW)));
        for (int y = 0; y < b.CH; ++y) {
            const float lv = float(y) + 0.5f;
            const float m = std::fmod(lv + scroll, half);
            b.rowMap(y)[2] = m + float(b.CH) * 0.25f;
            b.rowMap(y)[0] = xs;
            if (m < 1.0f)
                b.rowEx(y)[2] = 0.7f * b.A + 0.3f;
        }
    }
}

void kindRoll(Builder& b, int variant)
{
    Rng fr = b.frameRng(21);
    if (variant == 2) {
        b.plan[G_EDGE] = 2;
        const float h = std::max(0.02f, 1.0f - 0.97f * b.A);
        const float centre = float(b.CH) * 0.5f + fx::noise1(b.t * std::max(0.1f, b.speed), b.seed) * float(b.CH) * 0.1f;
        for (int y = 0; y < b.CH; ++y) {
            const float lv = float(y) + 0.5f;
            b.rowMap(y)[2] = float(b.CH) * 0.5f + (lv - centre) / h;
            b.rowFx(y)[0] = 1.0f + b.A * 0.6f;
        }
        return;
    }
    const float bar = float(b.CH) * 0.08f;
    const float period = float(b.CH) + bar;
    const float rate = variant == 1 ? 0.25f : 1.0f;
    const float off = float(std::fmod(b.t * double(b.speed * rate) * double(period), double(period)));
    const float squash = variant == 1 ? 1.15f : 1.0f;
    for (int y = 0; y < b.CH; ++y) {
        const float lv = float(y) + 0.5f;
        float s = std::fmod(lv + off, period);
        if (s >= float(b.CH)) {
            b.fillRow(y, 0x000000, 1.0f);
            continue;
        }
        b.rowMap(y)[2] = s * squash;
        if (s < 8.0f)
            b.rowMap(y)[0] = b.A * 6.0f * (1.0f - s / 8.0f);
        b.rowMap(y)[0] += fr.range(-1, 1) * b.A * 0.6f;
    }
    if (variant == 1) {
        b.plan[G_SAT] = 1.0f - b.A;
        b.plan[G_GAIN] = 1.0f - 0.1f * b.A * fr.f();
    }
}

void kindHmove(Builder& b)
{
    Rng r = b.rng(31);
    Rng fr = b.frameRng(32);
    for (int y = 0; y < b.CH; ++y) {
        if (r.chance(0.25f + 0.5f * b.A))
            b.rowFx(y)[3] = 8;
        if (fr.chance(b.A * 0.3f))
            b.rowMap(y)[0] = float(fr.i(4) + 1) * (fr.chance(0.5f) ? 1.0f : -1.0f) * b.A * 2.0f;
    }
}

uint32_t hsv(float h, float s, float v)
{
    h = (h - std::floor(h)) * 6.0f;
    const int i = int(h);
    const float f = h - float(i), p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    switch (i) {
    case 0: return rgbOf(v, t, p);
    case 1: return rgbOf(q, v, p);
    case 2: return rgbOf(p, v, t);
    case 3: return rgbOf(p, q, v);
    case 4: return rgbOf(t, p, v);
    default: return rgbOf(v, p, q);
    }
}

void kindRowBars(Builder& b, int variant)
{
    Rng r = b.rng(41);
    if (variant == 0) {
        const int off = int(b.t * double(b.speed) * 4.0) % 1000;
        for (int y = 0; y < b.CH; ++y) {
            const int band = (y + off) / 4;
            Rng br{fx::hash32(uint32_t(band), b.seed, uint32_t(b.step))};
            if (br.chance(b.A * 0.7f))
                b.fillRow(y, b.colour(br), 0.6f + 0.4f * br.f());
        }
    } else if (variant == 1) {
        for (int y = 0; y < b.CH; ++y) {
            const int band = y / 12;
            Rng br{fx::hash32(uint32_t(band), b.seed, uint32_t(b.step) + 7u)};
            if (br.chance(0.6f))
                b.fillRow(y, hsv(float(y) / float(b.CH) * 2.0f + float(b.t * b.speed * 0.2), 0.8f, 0.9f), b.A * 0.55f);
            if (r.chance(b.A * 0.08f))
                b.rowFx(y)[2] = 0x55;
        }
    } else {
        for (int y = 0; y < b.CH; ++y) {
            const int band = y / 3;
            Rng br{fx::hash32(uint32_t(band), b.seed, uint32_t(b.step) + 13u)};
            const float x = br.f();
            if (x < b.A * 0.12f)
                b.rowEx(y)[2] = 0.85f;
            else if (x < b.A * 0.3f)
                b.rowMap(y)[2] = float(std::max(0, y - 1 - br.i(20))) + 0.5f;
            else if (x < b.A * 0.4f)
                b.rowFx(y)[2] = 0x80;
        }
    }
    b.plan[G_NOISE_SEED] = float(b.frame & 0xffffu);
}

void kindPalette(Builder& b, int variant)
{
    Rng r = b.rng(51);
    const int n = b.palN();
    if (variant == 1) {
        if (r.chance(b.A))
            b.plan.display = paletteOf(PalCgaAlt);
        if (r.chance(b.A * 0.3f))
            b.plan[G_PAL_SHIFT] = 1;
        if (!b.hasIndex())
            b.plan[G_PERM] = r.chance(b.A) ? 1 + r.i(5) : 0;
        return;
    }
    if (variant == 2) {
        const bool grey = r.chance(b.A) || b.frameRng(52).chance(b.A * 0.15f);
        if (!grey)
            return;
        if (b.hasIndex()) {
            for (size_t i = 0; i < b.plan.display.size(); ++i) {
                const uint32_t c = b.plan.palette[i];
                const int l = (299 * int((c >> 16) & 255) + 587 * int((c >> 8) & 255) + 114 * int(c & 255)) / 1000;
                b.plan.display[i] = uint32_t(l * 0x010101);
            }
        } else {
            b.plan[G_SAT] = 0;
        }
        return;
    }
    if (variant == 3) {
        for (int ty = 0; ty < b.TY; ty += 2)
            for (int tx = 0; tx < b.TX; tx += 2) {
                if (!r.chance(b.A * 0.5f))
                    continue;
                const Px4 v = {float(1 + r.i(std::max(1, n - 1))), float(1 + r.i(5)), float(TileShift), float(r.chance(0.3f) ? 0x40 : 0)};
                for (int y = ty; y < ty + 2; ++y)
                    for (int x = tx; x < tx + 2; ++x)
                        b.tile(x, y) = v;
            }
        return;
    }
    if (b.hasIndex()) {
        for (int i = 0; i < n; ++i)
            if (r.chance(b.A * 0.7f))
                b.plan.display[size_t(i)] = b.plan.palette[size_t(r.i(n))];
        if (r.chance(b.A * 0.3f))
            b.plan[G_PAL_SHIFT] = float(r.i(n));
    } else {
        b.plan[G_PERM] = r.chance(b.A) ? float(1 + r.i(5)) : 0.0f;
        if (r.chance(b.A * 0.5f))
            b.plan[r.chance(0.5f) ? G_XOR_R : G_XOR_B] = float(0x80 >> r.i(3));
    }
}

void kindBits(Builder& b, int variant)
{
    Rng r = b.rng(61);
    if (variant == 0) {
        for (Global g : {G_AND_R, G_AND_G, G_AND_B}) {
            int m = 255;
            for (int bit = 7; bit >= 5; --bit)
                if (r.chance(b.A * 0.5f))
                    m &= ~(1 << bit);
            b.plan[g] = float(m);
        }
        for (Global g : {G_OR_R, G_OR_G, G_OR_B})
            if (r.chance(b.A * 0.3f))
                b.plan[g] = float(1 << (3 + r.i(4)));
        if (r.chance(b.A * 0.3f))
            b.plan[G_XOR_G] = 0x20;
        const int y0 = r.i(b.CH), y1 = std::min(b.CH, y0 + 4 + r.i(int(4 + b.A * float(b.CH) * 0.2f)));
        for (int y = y0; y < y1; ++y)
            b.rowFx(y)[2] = 0x40;
    } else if (variant == 1) {
        int m = 255;
        for (int k = 0; k < 1 + int(b.A * 3.0f); ++k)
            m &= ~(1 << (7 - r.i(4)));
        if (r.chance(0.2f))
            m = 255; // sometimes it works again
        b.plan[G_AND_R] = b.plan[G_AND_G] = b.plan[G_AND_B] = float(m);
        if (r.chance(b.A * 0.4f))
            b.plan[G_OR_B] = 0x40;
    } else { // colour depth loss: few bits, strong dither, banding
        const int bits = std::max(1, 5 - int(b.A * 3.5f));
        b.plan[G_QMODE] = QBits;
        b.plan[G_QBITS_R] = b.plan[G_QBITS_G] = b.plan[G_QBITS_B] = float(bits);
        b.plan[G_DITHER] = 1;
        b.plan[G_DITHER_AMT] = 1.0f + b.A;
        if (int(b.plan[G_LOOK]) == LookGlitchOnly)
            b.plan[G_LOOK] = LookColours;
        for (int y = 0; y < b.CH; ++y) {
            const int band = y / 16;
            Rng br{fx::hash32(uint32_t(band), b.seed, uint32_t(b.step) + 3u)};
            if (br.chance(b.A * 0.2f))
                b.rowFx(y)[0] = 0.85f;
        }
    }
}

void kindCable(Builder& b, int variant)
{
    Rng r = b.rng(71);
    Rng fr = b.frameRng(72);
    if (variant == 0) {
        if (r.chance(b.A))
            b.plan[G_DROP] = float(1 << r.i(3));
        if (fr.chance(b.A * 0.1f))
            b.plan[G_DROP] = float(int(b.plan[G_DROP]) | (1 << fr.i(3)));
    } else if (variant == 1) {
        if (r.chance(b.A))
            b.plan[G_PERM] = float(1 + r.i(5));
        b.plan[G_TINT] = float(0xff90ff);
        b.plan[G_TINT_AMT] = b.A * 0.3f;
        for (int y = 0; y < b.CH; ++y)
            if (fr.chance(b.A * 0.05f))
                b.rowMap(y)[0] = fr.range(-2, 2) * b.A;
    } else if (variant == 2) {
        b.plan[G_TINT] = float(r.chance(0.5f) ? 0x80ff80 : 0xff70ff);
        b.plan[G_TINT_AMT] = b.A * 0.6f;
        b.plan[G_SAT] = 1.0f + b.A * 0.4f;
        if (r.chance(b.A * 0.2f))
            b.plan[G_DROP] = 4;
    } else {
        b.plan[G_SAT] = 1.0f - b.A * 0.6f;
        for (int y = 0; y < b.CH; ++y) {
            b.rowMap(y)[0] = (fr.f() - 0.5f) * b.A * 4.0f;
            if (fr.chance(b.A * 0.04f))
                b.rowEx(y)[2] = 0.4f;
        }
        if (fr.chance(b.A * 0.05f))
            b.plan[G_DROP] = float(1 << fr.i(3));
        b.plan[G_NOISE_SEED] = float(b.frame & 0xffffu);
    }
}

void kindJailbars(Builder& b, int variant)
{
    for (int x = 0; x < b.GW; ++x) {
        float gain;
        if (variant == 0) {
            const int period = 4;
            const float bar = (x % period) < period / 2 ? 1.0f : -1.0f;
            gain = 1.0f + b.A * 0.12f * bar * (0.7f + 0.3f * std::sin(float(x) * 0.01f));
        } else {
            gain = (x % 8) == 0 ? 1.0f + b.A * 0.25f : 1.0f;
        }
        b.plan.cols[size_t(x)][0] = gain;
    }
}

void kindDeadLines(Builder& b, int variant)
{
    Rng r = b.fixedRng(81);
    Rng sr = b.rng(82);
    Rng fr = b.frameRng(83);
    auto light = [&]() {
        uint32_t best = 0xffffff;
        int bestL = -1;
        for (uint32_t c : b.plan.palette) {
            const int l = int((c >> 16) & 255) + int((c >> 8) & 255) + int(c & 255);
            if (l > bestL)
                bestL = l, best = c;
        }
        return best;
    };
    auto deadColumns = [&](int count, bool flicker) {
        for (int k = 0; k < count; ++k) {
            const int x0 = r.i(b.CW), w = 1 + r.i(3);
            const uint32_t c = b.palN() > 0 ? light() : (r.chance(0.5f) ? 0x000000 : 0xffffff);
            const bool off = flicker && sr.chance(0.2f);
            for (int x = x0; x < std::min(b.CW, x0 + w); ++x)
                if (!off)
                    b.plan.cols[size_t(b.AX + x)] = {1, float(c), 1, 0};
        }
    };
    if (variant == 0) {
        deadColumns(std::max(1, int(b.A * float(b.CW) * 0.12f)), b.speed > 0);
        if (b.A > 0.6f)
            for (int k = 0; k < int((b.A - 0.6f) * 10.0f); ++k)
                b.fillRow(r.i(b.CH), light(), 1.0f);
    } else if (variant == 1 || variant == 5) {
        const int bands = 1 + int(b.A * 5.0f);
        for (int k = 0; k < bands; ++k) {
            const int y0 = fr.i(b.CH), h = 1 + fr.i(6);
            for (int y = y0; y < std::min(b.CH, y0 + h); ++y) {
                b.rowFx(y)[2] = float(0x20 << fr.i(3));
                if (fr.chance(0.3f))
                    b.fillRow(y, fr.chance(0.5f) ? 0xffffff : b.colour(fr), 0.7f);
            }
        }
        deadColumns(1 + int(b.A * 3.0f), true);
        if (variant == 5) {
            for (int k = 0; k < int(b.A * 10.0f); ++k)
                b.plan.cols[size_t(b.AX + sr.i(b.CW))] = {1, float(b.colour(sr)), 1, 0};
            if (sr.chance(b.A * 0.4f)) {
                b.plan[G_BLANK] = 0.6f + 0.3f * fr.f();
                b.plan[G_BLANK_COLOR] = float(0xffffff);
            }
        }
    } else if (variant == 2) {
        b.plan[G_SPARK_MODE] = 4;
        b.plan[G_SPARK_DENS] = b.A * 0.004f;
        b.plan[G_SPARK_SEED] = float(b.seed & 0xffffu);
    } else if (variant == 3) {
        deadColumns(std::max(1, int(b.A * float(b.CW) * 0.06f)), false);
        for (int k = 0; k < int(b.A * float(b.CH) * 0.06f) + 1; ++k)
            b.fillRow(r.i(b.CH), 0x000000, 0.9f);
    } else if (variant == 4) {
        const int y0 = r.i(b.CH / 2), span = int(float(b.CH) * (0.2f + 0.5f * b.A));
        for (int y = y0; y < std::min(b.CH, y0 + span); ++y)
            if (r.chance(0.35f + 0.5f * b.A) && !fr.chance(0.15f))
                b.fillRow(y, 0x000000, 1.0f);
    }
}

void kindDim(Builder& b, int variant)
{
    Rng r = b.rng(91);
    Rng fr = b.frameRng(92);
    const double ts = b.t * std::max(0.05f, b.speed);
    if (variant == 0) {
        b.plan[G_GAIN] = 1.0f - b.A * 0.55f;
        b.plan[G_CONTRAST] = 1.0f - b.A * 0.35f;
        b.plan[G_SAT] = 1.0f - b.A * 0.45f;
        b.plan[G_BRIGHT] = -b.A * 12.0f;
        for (int y = 0; y < b.CH; ++y)
            b.rowFx(y)[0] = 1.0f + b.A * 0.15f * (1.0f - float(y) / float(b.CH));
    } else if (variant == 1) {
        b.plan[G_SAT] = 1.0f - b.A * 0.6f;
        b.plan[G_CONTRAST] = 1.0f - b.A * 0.5f;
        b.plan[G_BRIGHT] = b.A * 25.0f;
        b.plan[G_GAIN] = 1.0f - b.A * 0.3f * (0.5f + 0.5f * fx::noise1(ts * 4.0, b.seed));
        const float band = float(std::fmod(b.t * 0.3, 1.0)) * float(b.CH);
        for (int y = 0; y < b.CH; ++y) {
            b.rowMap(y)[0] = std::sin(float(y) * 0.15f + float(b.t) * 6.0f) * b.A * 2.5f;
            if (std::abs(float(y) - band) < float(b.CH) * 0.1f)
                b.rowFx(y)[0] = 1.0f - b.A * 0.25f;
        }
    } else if (variant == 2) {
        b.plan[G_CONTRAST] = 1.0f + fx::noise1(ts, b.seed) * b.A * 0.9f;
        b.plan[G_BRIGHT] = fx::noise1(ts * 0.7, b.seed + 1) * b.A * 60.0f;
    } else if (variant == 3) {
        b.plan[G_GAIN] = 1.0f - b.A * 0.6f * std::max(0.0f, fx::noise1(ts * 3.0, b.seed));
        for (int y = 0; y < b.CH; ++y)
            b.rowFx(y)[0] = 1.0f - b.A * 0.2f * (0.5f + 0.5f * fx::noise1(double(y) * 0.05 + ts * 0.3, b.seed + 2));
        if (fr.chance(b.A * 0.05f))
            b.plan[G_BLANK] = 1;
    } else {
        const int level = 15 - r.i(int(b.A * 15.0f) + 1);
        b.plan[G_GAIN] = float(level) / 15.0f;
        if (r.chance(b.A * 0.15f))
            b.plan[G_BLANK] = 1;
        if (r.chance(b.A * 0.4f)) {
            const int y0 = r.i(b.CH), h = 2 + r.i(int(4 + b.A * 30.0f));
            for (int y = y0; y < std::min(b.CH, y0 + h); ++y)
                b.fillRow(y, 0x000000, 1.0f);
        }
    }
}

void kindMode7(Builder& b)
{
    b.plan[G_EDGE] = 0;
    const float CWf = float(b.CW), CHf = float(b.CH);
    const float hz = CHf * (0.22f + 0.06f * std::sin(float(b.t) * 0.3f));
    const float theta = float(b.t * double(b.speed) * 0.6) + float(b.seed % 100) * 0.1f;
    const float f = CHf * 0.6f, height = CHf * 0.9f;
    const float cx = CWf * 0.5f + std::sin(float(b.t * double(b.speed)) * 0.4f) * CWf * 0.3f;
    const float cy = CHf * 0.5f + float(std::fmod(b.t * double(b.speed) * double(CHf) * 0.2, double(CHf)));
    const float Rx = std::cos(theta), Ry = std::sin(theta), Fx = std::sin(theta), Fy = -std::cos(theta);
    for (int y = 0; y < b.CH; ++y) {
        const float lv = float(y) + 0.5f;
        if (lv < hz) {
            b.fillRow(y, b.look.backdrop, 1.0f);
            continue;
        }
        const float z = height * f / (lv - hz + 0.5f);
        float k = z / f, bx = cx + Fx * z * 0.25f, by = cy + Fy * z * 0.25f, rx = Rx, ry = Ry;
        Rng br{fx::hash32(uint32_t(y / 4), b.seed, uint32_t(b.step) + 5u)};
        if (br.chance(b.A * 0.35f)) { // corrupted HDMA entry
            k *= br.range(0.2f, 4.0f);
            const float d = br.range(-1, 1);
            rx = std::cos(theta + d);
            ry = std::sin(theta + d);
            bx += br.range(-1, 1) * CWf;
            by += br.range(-1, 1) * CHf * 0.5f;
        }
        b.rowMap(y) = {bx - rx * CWf * 0.5f * k, rx * k, by - ry * CWf * 0.5f * k, ry * k};
    }
}

void kindAffine(Builder& b)
{
    Rng r = b.rng(101);
    Rng fr = b.frameRng(102);
    int y = 0;
    while (y < b.CH) {
        const int h = 8 + r.i(25);
        if (r.chance(b.A * 0.6f)) {
            const float ang = r.range(-1, 1) * b.A * 1.2f + fr.range(-1, 1) * 0.02f;
            const float sx = r.range(0.5f, 2.0f), sy = r.range(0.5f, 2.0f);
            const float m00 = std::cos(ang) * sx, m01 = -std::sin(ang) * sy, m10 = std::sin(ang) * sx, m11 = std::cos(ang) * sy;
            const float cx = r.range(0, float(b.CW)), cy = float(y) + float(h) * 0.5f;
            for (int yy = y; yy < std::min(b.CH, y + h); ++yy) {
                const float dy = float(yy) + 0.5f - cy;
                b.rowMap(yy) = {cx - m00 * cx + m01 * dy, m00, cy - m10 * cx + m11 * dy, m10};
            }
        }
        y += h;
    }
}

void kindWave(Builder& b, int variant)
{
    Rng r = b.rng(111);
    Rng fr = b.frameRng(112);
    if (variant == 0) {
        const float amp = b.A * float(b.CW) * 0.04f;
        for (int y = 0; y < b.CH; ++y) {
            b.rowMap(y)[0] = std::sin(float(y) * 0.12f + float(b.t * double(b.speed)) * 2.0f) * amp;
            if (r.chance(b.A * 0.05f))
                b.rowMap(y)[0] += r.range(-1, 1) * float(b.CW) * 0.4f;
        }
    } else if (variant == 1) {
        int y = 0;
        while (y < b.CH) {
            const int h = 2 + fr.i(11);
            if (fr.chance(b.A * 0.4f)) {
                const float dx = fr.range(-1, 1) * float(b.CW) * 0.3f * b.A;
                for (int yy = y; yy < std::min(b.CH, y + h); ++yy)
                    b.rowMap(yy)[0] = dx;
            }
            y += h;
        }
    } else {
        const float ts = float(b.t * double(std::max(0.05f, b.speed)));
        for (int y = 0; y < b.CH; ++y)
            b.rowMap(y)[0] = std::sin(ts * 6.0f + float(y) * 0.04f) * b.A * 4.0f + fr.range(-1, 1) * b.A * 0.5f;
        b.plan[G_SUB_OP] = 5;
        b.plan[G_SUB_DX] = 3.0f + b.A * 8.0f;
        b.plan[G_SUB_AMT] = 0.6f;
    }
}

void kindMosaic(Builder& b)
{
    Rng r = b.rng(121);
    b.plan[G_MOSAIC] = r.chance(0.85f) ? float(2 + int(b.A * 14.0f * r.f())) : 1.0f;
}

void kindColorMath(Builder& b, int variant)
{
    Rng r = b.rng(131);
    Rng fr = b.frameRng(132);
    if (variant == 0) {
        if (r.chance(0.3f))
            return;
        b.plan[G_SUB_OP] = float(1 + r.i(3));
        b.plan[G_SUB_DX] = r.range(-1, 1) * float(b.CW) * 0.25f * b.A;
        b.plan[G_SUB_DY] = r.range(-1, 1) * float(b.CH) * 0.25f * b.A;
        b.plan[G_SUB_AMT] = b.A * (0.5f + 0.5f * r.f());
        b.plan[G_BRIGHT] = r.range(-1, 1) * b.A * 20.0f;
    } else if (variant == 1) {
        b.plan[G_SUB_OP] = 3;
        b.plan[G_SUB_DX] = 1.0f + b.A * 2.0f;
        b.plan[G_SUB_DY] = b.A;
        b.plan[G_SUB_AMT] = 1.0f;
        b.plan[G_BRIGHT] = b.A * 12.0f;
    } else if (variant == 2) {
        b.plan[G_SUB_OP] = 5;
        b.plan[G_SUB_DX] = b.A * 14.0f + fr.range(-1, 1) * b.A;
        b.plan[G_SUB_AMT] = 0.75f;
    } else {
        const float ts = float(b.t * double(std::max(0.05f, b.speed)));
        b.plan[G_SUB_OP] = 4;
        b.plan[G_SUB_DX] = std::sin(ts) * float(b.CW) * 0.15f * b.A + 1.0f;
        b.plan[G_SUB_DY] = std::cos(ts * 0.7f) * float(b.CH) * 0.1f * b.A;
        b.plan[G_SUB_AMT] = 1.0f;
    }
}

void kindMesh(Builder& b, Kind kind, int variant)
{
    Plan& plan = b.plan;
    const bool ps1 = QString::fromLatin1(b.look.key) == QLatin1String("ps1");
    auto vertexSnap = [&](std::array<float, 2> v) {
        if (ps1 || kind == KMeshWobble)
            v = {snap(v[0], b.CW), snap(v[1], b.CH)};
        return v;
    };
    Rng r = b.rng(141);
    Rng fr = b.frameRng(142);
    if (kind == KMeshExplode) {
        Grid g(16, 9);
        std::vector<char> exploded(g.pos.size(), 0);
        const int count = 1 + int(b.A * 5.0f);
        for (int k = 0; k < count; ++k) {
            const int v = r.i(int(g.pos.size()));
            const float ang = r.range(0, 2 * kPi), len = r.range(0.6f, 2.5f);
            g.pos[size_t(v)][0] += std::cos(ang) * len;
            g.pos[size_t(v)][1] += std::sin(ang) * len;
            exploded[size_t(v)] = 1;
        }
        for (auto& p : g.pos) {
            p[0] += fr.range(-1, 1) * b.A * 0.6f / float(b.CW);
            p[1] += fr.range(-1, 1) * b.A * 0.6f / float(b.CH);
        }
        std::vector<std::array<int, 3>> normal, spikes;
        for (const auto& t : g.tris(ps1))
            (exploded[size_t(t[0])] || exploded[size_t(t[1])] || exploded[size_t(t[2])] ? spikes : normal).push_back(t);
        for (const auto* list : {&normal, &spikes})
            for (const auto& t : *list)
                for (int k : t)
                    emitVertex(plan, vertexSnap(g.pos[size_t(k)]), g.uv[size_t(k)]);
    } else if (kind == KMeshWobble) {
        Grid g(12, 8);
        for (size_t i = 0; i < g.pos.size(); ++i) {
            const int ix = int(i) % (g.nx + 1), iy = int(i) / (g.nx + 1);
            const bool edgeX = ix == 0 || ix == g.nx, edgeY = iy == 0 || iy == g.ny;
            const float jx = (fx::hash01(uint32_t(i), b.frame, b.seed) - 0.5f) * b.A * 6.0f / float(b.CW);
            const float jy = (fx::hash01(uint32_t(i), b.frame, b.seed + 1) - 0.5f) * b.A * 6.0f / float(b.CH);
            if (!edgeX)
                g.pos[i][0] += jx;
            if (!edgeY)
                g.pos[i][1] += jy;
            g.uv[i][0] += fx::noise1(b.t * 2.0 + double(i), b.seed) * b.A * 0.004f;
            g.uv[i][1] += fx::noise1(b.t * 2.0 + double(i) + 50.0, b.seed) * b.A * 0.004f;
        }
        for (const auto& t : g.tris(true))
            for (int k : t)
                emitVertex(plan, vertexSnap(g.pos[size_t(k)]), g.uv[size_t(k)]);
    } else if (kind == KMeshCracks) {
        Grid g(16, 10);
        int ti = 0;
        for (const auto& t : g.tris(true)) {
            for (int k : t) {
                std::array<float, 2> p = g.pos[size_t(k)];
                p[0] += (fx::hash01(uint32_t(ti * 3 + k), b.frame, b.seed) - 0.5f) * b.A * 2.5f / float(b.CW);
                p[1] += (fx::hash01(uint32_t(ti * 3 + k), b.frame, b.seed + 7) - 0.5f) * b.A * 2.5f / float(b.CH);
                emitVertex(plan, vertexSnap(p), g.uv[size_t(k)]);
            }
            ++ti;
        }
        b.plan[G_SPARK_MODE] = 1;
        b.plan[G_SPARK_DENS] = b.A * 0.002f;
        b.plan[G_SPARK_SEED] = float(b.frame & 0xffffu);
        b.plan[G_SPARK_COLOR] = float(0xffffff);
    } else if (kind == KMeshTexture) {
        Grid g(14, 8);
        for (const auto& t : g.tris(ps1)) {
            std::array<float, 2> uv[3] = {g.uv[size_t(t[0])], g.uv[size_t(t[1])], g.uv[size_t(t[2])]};
            if (r.chance(b.A * 0.35f)) {
                if (variant == 1 && r.chance(0.6f)) { // smeared: the texture collapses to a streak
                    const std::array<float, 2> c = uv[r.i(3)];
                    const bool horizontal = r.chance(0.5f);
                    for (auto& v : uv)
                        v = horizontal ? std::array<float, 2>{v[0], c[1]} : std::array<float, 2>{c[0], v[1]};
                } else {
                    const float dx = r.range(-0.5f, 0.5f), dy = r.range(-0.5f, 0.5f), s = r.range(0.3f, 2.5f);
                    for (auto& v : uv)
                        v = {v[0] * s + dx, v[1] * s + dy};
                }
            }
            for (int k = 0; k < 3; ++k)
                emitVertex(plan, vertexSnap(g.pos[size_t(t[size_t(k)])]), uv[k]);
        }
        if (variant == 1) {
            Rng br = b.rng(143);
            if (br.chance(b.A))
                b.plan[G_AND_G] = float(0xff & ~(1 << (7 - br.i(3))));
            if (br.chance(b.A * 0.5f))
                b.plan[G_XOR_R] = 0x40;
            for (Px4& t : b.plan.tiles)
                if (br.chance(b.A * 0.06f))
                    b.garbageTile(t, br);
        }
    } else if (kind == KMeshWarp) {
        Grid g(6, 4);
        const double ts = b.t * double(std::max(0.05f, b.speed));
        for (size_t i = 0; i < g.pos.size(); ++i) {
            g.pos[i][0] += fx::noise1(ts + double(i) * 1.7, b.seed) * b.A * 0.08f;
            g.pos[i][1] += fx::noise1(ts + double(i) * 2.3 + 40.0, b.seed) * b.A * 0.08f;
        }
        for (const auto& t : g.tris(false))
            for (int k : t)
                emitVertex(plan, vertexSnap(g.pos[size_t(k)]), g.uv[size_t(k)]);
    } else { // KMeshDrop
        Grid g(16, 9);
        for (const auto& t : g.tris(false)) {
            if (fr.chance(b.A * 0.3f))
                continue;
            std::array<float, 2> uv[3] = {g.uv[size_t(t[0])], g.uv[size_t(t[1])], g.uv[size_t(t[2])]};
            if (fr.chance(b.A * 0.15f)) {
                const float dx = fr.range(-0.1f, 0.1f), dy = fr.range(-0.1f, 0.1f);
                for (auto& v : uv)
                    v = {v[0] + dx, v[1] + dy};
            }
            for (int k = 0; k < 3; ++k)
                emitVertex(plan, vertexSnap(g.pos[size_t(t[size_t(k)])]), uv[k]);
        }
    }
}

void kindArtifacts(Builder& b, int variant)
{
    const float dens[4] = {0.8f, 0.25f, 0.08f, 0.2f};
    const float cell[4] = {8, 16, 16, 16};
    const float scale = float(b.CW) / 640.0f; // artifact size grows with the console resolution
    b.plan[G_ART_MODE] = float(variant == 3 ? 4 : variant + 1);
    b.plan[G_ART_DENS] = b.A * dens[variant];
    b.plan[G_ART_CELL] = std::max(2.0f, std::round(cell[variant] * std::max(0.5f, scale)));
    b.plan[G_ART_SEED] = float((b.seed + uint32_t(b.step) * 7919u) & 0xffffffu);
    Rng r = b.rng(151);
    const uint32_t pairs[4][2] = {{0x00ff40, 0xff00ff}, {0x202020, 0xe0e0e0}, {0x00ffff, 0xff40ff}, {0x40ff40, 0xff4040}};
    const auto& pr = pairs[r.i(4)];
    b.plan[G_ART_C1] = float(pr[0]);
    b.plan[G_ART_C2] = float(pr[1]);
    if (variant == 3)
        for (int k = 0; k < int(b.A * 8.0f); ++k)
            b.plan.cols[size_t(b.AX + r.i(b.CW))] = {1, float(b.colour(r)), 1, 0};
}

QImage textOverlay(int w, int h, const std::function<void(QPainter&, int, int)>& draw)
{
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(0);
    QPainter p(&img);
    draw(p, w, h);
    return img;
}

void kindFreeze(Builder& b, int variant)
{
    const double cycle = b.t * double(std::max(0.02f, b.speed));
    const int64_t k = int64_t(std::floor(cycle));
    const float ph = float(cycle - double(k));
    Rng r{fx::hash32(uint32_t(k), b.seed, 161u)};
    auto freeze = [&] {
        b.plan.freezeKey = k;
        b.allRowsHeld();
    };
    if (variant == 0) {
        if (ph < 0.15f + 0.6f * b.A)
            freeze();
    } else if (variant == 1) {
        if (ph < 0.3f + 0.6f * b.A) {
            freeze();
            const int y0 = r.i(b.TY), h = 1 + r.i(3);
            for (int ty = y0; ty < std::min(b.TY, y0 + h); ++ty)
                for (int tx = 0; tx < b.TX; ++tx)
                    if (r.chance(0.85f))
                        b.garbageTile(b.tile(tx, ty), r);
            for (int y = y0 * b.look.tile; y < std::min(b.CH, (y0 + h) * b.look.tile); ++y)
                b.rowFx(y)[2] = 0x40;
            b.plan[G_ART_MODE] = 3;
            b.plan[G_ART_DENS] = 0.02f * b.A;
            b.plan[G_ART_CELL] = 16;
            b.plan[G_ART_SEED] = float(uint32_t(k) & 0xffffu);
            b.plan[G_ART_C1] = float(0xffffff);
            b.plan[G_ART_C2] = float(0x00ff00);
        }
    } else if (variant == 2) {
        const float live = 0.35f * (1.0f - b.A) + 0.1f, frozen = live + 0.15f;
        if (ph >= live && ph < frozen) {
            freeze();
        } else if (ph >= frozen) {
            b.plan[G_BLANK] = 1;
            b.plan[G_BLANK_COLOR] = 0;
            b.plan[G_OVERLAY] = 1;
            b.plan.overlay = textOverlay(b.GW, b.GH, [](QPainter& p, int w, int h) {
                QFont f(QStringLiteral("Arial"));
                f.setPixelSize(std::max(8, h / 22));
                p.setFont(f);
                p.setPen(QColor(230, 230, 230));
                p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, QStringLiteral("Disc Read Error"));
            });
        }
    } else { // Guru Meditation: the machine halts (10 s cycle) and the alert box blinks
        const double halt = b.t * 0.1;
        const float hph = float(halt - std::floor(halt));
        if (hph >= 0.25f * (1.0f - b.A)) {
            b.plan.freezeKey = int64_t(std::floor(halt));
            b.allRowsHeld();
            const bool on = std::fmod(b.t * double(std::max(0.05f, b.speed)), 1.0) < 0.5;
            b.plan[G_OVERLAY] = 1;
            b.plan.overlay = textOverlay(b.GW * 2, b.GH * 2, [on](QPainter& p, int w, int h) {
                const int boxH = h * 2 / 9, t = std::max(2, h / 64);
                p.fillRect(0, 0, w, boxH, Qt::black);
                if (on) {
                    p.setPen(Qt::NoPen);
                    p.setBrush(QColor(0xdd, 0x11, 0x11));
                    p.drawRect(t, t, w - 2 * t, t);
                    p.drawRect(t, boxH - 2 * t, w - 2 * t, t);
                    p.drawRect(t, t, t, boxH - 2 * t);
                    p.drawRect(w - 2 * t, t, t, boxH - 2 * t);
                }
                const QString line1 = QStringLiteral("Software Failure.    Press left mouse button to continue.");
                QFont f(QStringLiteral("Courier New"));
                f.setStyleHint(QFont::Monospace);
                f.setBold(true);
                int px = std::max(6, boxH / 6);
                f.setPixelSize(px);
                while (px > 6 && QFontMetrics(f).horizontalAdvance(line1) > w * 9 / 10)
                    f.setPixelSize(--px);
                p.setFont(f);
                p.setPen(QColor(0xdd, 0x11, 0x11));
                p.drawText(QRect(0, boxH / 6, w, boxH / 3), Qt::AlignCenter, line1);
                p.drawText(QRect(0, boxH / 2, w, boxH / 3), Qt::AlignCenter, QStringLiteral("Guru Meditation #00000004.0000AAC0"));
            });
        }
    }
}

void kindInterlace(Builder& b)
{
    b.plan.needPrev = true;
    const int field = int(b.frame & 1u);
    for (int y = 0; y < b.CH; ++y) {
        if (((y + field) & 1) != 0) {
            b.rowEx(y)[1] = 1;
            b.rowMap(y)[0] = b.A * 1.5f;
        }
        b.rowMap(y)[2] += (field != 0 ? 0.5f : -0.5f) * b.A;
    }
}

void kindTear(Builder& b)
{
    b.plan.needPrev = true;
    Rng fr = b.frameRng(171);
    if (b.A <= 0 || !fr.chance(0.5f + 0.5f * b.A))
        return;
    const int tears = b.A > 0.6f ? 2 : 1;
    for (int k = 0; k < tears; ++k) {
        const int yt = int(float(std::fmod(b.t * double(std::max(0.05f, b.speed)) * 0.37 + double(fr.f()), 1.0)) * float(b.CH));
        const float dx = (fr.chance(0.5f) ? 1.0f : -1.0f) * fr.range(0.3f, 1.0f) * b.A * float(b.CW) * 0.03f;
        for (int y = k == 0 ? 0 : b.CH / 2; y < std::min(b.CH, yt); ++y) {
            b.rowEx(y)[1] = 1;
            b.rowMap(y)[0] = dx;
        }
    }
}

void kindSparkle(Builder& b, int variant)
{
    Rng fr = b.frameRng(181);
    b.plan[G_SPARK_SEED] = float(b.frame & 0xffffu);
    if (variant == 0) {
        b.plan[G_SPARK_MODE] = 1;
        b.plan[G_SPARK_DENS] = b.A * 0.02f;
        b.plan[G_SPARK_COLOR] = float(fr.chance(0.5f) ? 0xffffff : 0xc0c0c0);
    } else if (variant == 1) {
        b.plan[G_SPARK_MODE] = 2;
        b.plan[G_SPARK_DENS] = 0.05f + b.A * 0.2f;
        for (auto& r : b.plan.rowEx2)
            r[1] = 0;
        for (int k = 0; k < 1 + int(b.A * 3.0f); ++k) { // palette writes land in a few bands of lines
            const int y0 = fr.i(b.GH), h = 1 + fr.i(int(3 + b.A * 12.0f));
            for (int y = y0; y < std::min(b.GH, y0 + h); ++y)
                b.plan.rowEx2[size_t(y)][1] = 1;
        }
    } else if (variant == 2) {
        b.plan[G_SPARK_MODE] = 3;
        b.plan[G_SPARK_LEN] = 6;
        b.plan[G_SPARK_DENS] = b.A * 0.03f;
        b.plan[G_SPARK_COLOR] = float(0xffffff);
    } else {
        b.plan[G_SPARK_MODE] = 3;
        b.plan[G_SPARK_LEN] = 3;
        b.plan[G_SPARK_DENS] = b.A * 0.006f;
        const uint32_t cols[3] = {0xf0f0f0, 0xff40ff, 0x40ff80};
        b.plan[G_SPARK_COLOR] = float(cols[fr.i(3)]);
        const double cycle = b.t * double(std::max(0.05f, b.speed));
        const float ph = float(cycle - std::floor(cycle));
        if (ph < 0.12f * b.A || fr.chance(b.A * 0.03f)) {
            b.plan[G_BLANK] = 1;
            b.plan[G_BLANK_COLOR] = 0;
        }
        for (int y = 0; y < b.CH; ++y)
            if (fr.chance(b.A * 0.01f))
                b.rowEx(y)[2] = 0.7f;
        b.plan[G_NOISE_SEED] = float(b.frame & 0xffffu);
    }
}

void kindAttr(Builder& b)
{
    Rng r = b.rng(191);
    const int n = std::max(1, b.palN());
    if (b.plan.lookFull() && b.hasIndex()) {
        b.plan[G_CELLS] = 1;
        b.plan[G_FLASH] = float(int64_t(std::floor(b.t * 1.5625)) & 1);
        for (Px4& t : b.plan.tiles)
            if (r.chance(b.A * 0.4f))
                t = {float(8 + r.i(8)), float(r.i(8)), float(TileAttr), float(r.chance(0.5f) ? 1 : 0)};
    } else {
        for (Px4& t : b.plan.tiles)
            if (r.chance(b.A * 0.4f))
                t = {float(1 + r.i(n - 1 > 0 ? n - 1 : 1)), float(r.i(6)), float(TileShift), 0};
    }
}

void kindTape(Builder& b, int variant)
{
    const double cycle = b.t * double(std::max(0.02f, b.speed));
    const float ph = float(cycle - std::floor(cycle));
    const uint32_t cyc = uint32_t(int64_t(std::floor(cycle)));
    Rng er{fx::hash32(cyc, b.seed, 201u)};
    const float pilot = 0.08f, end = 0.85f;
    const bool zx = variant == 0;
    // Border stripes.
    for (int gy = 0; gy < b.GH; ++gy) {
        float colour = -1;
        if (ph < pilot) {
            colour = zx ? float(((gy + int(b.frame) * 7) / 10) & 1 ? 0x00d7d7 : 0xd70000) : float(paletteOf(PalC64)[size_t((gy / 8 + b.frame) % 16)]);
        } else if (ph < end) {
            const uint32_t h = fx::hash32(uint32_t(gy / (1 + int(b.frame % 3))), b.frame, b.seed);
            colour = zx ? float((h & 1u) ? 0x0000d7 : 0xd7d700) : float(paletteOf(PalC64)[h % 16]);
        }
        b.plan.rowEx[size_t(gy)][0] = colour;
    }
    if (ph >= end)
        return;
    const uint32_t paper = zx ? 0xd7d7d7 : 0x352879;
    if (ph < pilot) {
        for (int y = 0; y < b.CH; ++y)
            b.fillRow(y, paper, 1.0f);
        return;
    }
    const float q = (ph - pilot) / (end - pilot);
    for (int y = 0; y < b.CH; ++y) {
        bool shown, colour = true;
        if (zx) {
            const int third = y / 64, within = y % 64, charRow = within / 8, line = within % 8;
            const int m = third * 64 + line * 8 + charRow;
            const float pixels = 0.889f;
            shown = float(m) < std::min(q / pixels, 1.0f) * float(b.CH);
            colour = q >= pixels && float(y / 8) < (q - pixels) / (1.0f - pixels) * float(b.CH / 8);
        } else {
            shown = float(y) < q * float(b.CH);
        }
        if (!shown)
            b.fillRow(y, paper, 1.0f);
        else if (!colour)
            b.rowEx2(y)[0] = 1;
        if (shown && er.chance(b.A * 0.05f))
            b.rowFx(y)[2] = float(0x10 << er.i(4));
    }
}

void kindCgaMode(Builder& b)
{
    Rng r = b.rng(211);
    const int half = b.CH / 2;
    const int jump = r.chance(b.A * 0.3f) ? 8 * (1 + r.i(4)) : 0;
    for (int y = 0; y < b.CH; ++y) {
        const int src = y < half ? 2 * y : 2 * (y - half) + 1;
        b.rowMap(y)[2] = float((src + jump) % b.CH) + 0.5f;
        if (b.A > 0.5f)
            b.rowMap(y)[1] = 2;
    }
    if (r.chance(b.A * 0.3f))
        b.plan[G_PAL_SHIFT] = 1;
}

void kindVector(Builder& b, int variant)
{
    Rng fr = b.frameRng(221);
    b.plan[G_VEC_GAIN] = 3;
    b.plan[G_VEC_COLOR] = float(0xd8e8ff);
    if (variant == 0) {
        const float ts = float(b.t * double(std::max(0.05f, b.speed)));
        for (int y = 0; y < b.CH; ++y)
            b.rowMap(y)[0] = std::sin(float(y) * 0.07f + ts * 5.0f) * b.A * 5.0f + fr.range(-1, 1) * b.A * 1.5f;
        b.plan[G_GAIN] = 1.0f - b.A * 0.3f * fr.f();
    } else if (variant == 1) {
        b.plan.ghost = 0.6f + b.A * 0.35f;
        b.plan[G_GAIN] = 1.1f;
    } else {
        b.plan[G_EDGE] = 2;
        const float h = std::max(0.02f, 1.0f - 0.97f * b.A), w = 1.0f - 0.3f * b.A;
        const float wob = fx::noise1(b.t * double(std::max(0.05f, b.speed)), b.seed) * float(b.CH) * 0.05f;
        for (int y = 0; y < b.CH; ++y) {
            const float lv = float(y) + 0.5f;
            b.rowMap(y) = {float(b.CW) * 0.5f - float(b.CW) * 0.5f / w, 1.0f / w, float(b.CH) * 0.5f + (lv - float(b.CH) * 0.5f - wob) / h, 0};
        }
        b.plan[G_GAIN] = 1.0f + b.A * 0.8f;
    }
}

// ---- Registry ------------------------------------------------------------------------

struct Entry {
    const LookSpec* look;
    const Spec* spec;
};

const std::map<QString, Entry>& registry()
{
    static const std::map<QString, Entry> map = [] {
        std::map<QString, Entry> m;
        for (const Spec& s : kSpecs)
            for (const LookSpec& l : kLooks)
                if (qstrcmp(l.key, s.look) == 0)
                    m[QStringLiteral("vg_%1_%2").arg(QString::fromLatin1(l.key), QString::fromLatin1(s.id))] = {&l, &s};
        return m;
    }();
    return map;
}

QString S(const char* s) { return QString::fromLatin1(s); }

} // namespace

bool isConsoleEffect(const QString& id)
{
    return id.startsWith(QLatin1String("vg_")) && registry().count(id) > 0;
}

Plan buildPlan(const QString& id, const Params& p, const RenderContext& ctx, QSize layer)
{
    Plan plan;
    auto it = registry().find(id);
    if (it == registry().end())
        return plan;
    const LookSpec& look = *it->second.look;
    const Spec& spec = *it->second.spec;
    initPlan(plan, look.w + 2 * look.bx, look.h + 2 * look.by, look.bx, look.by, look.w, look.h, look.tile, layer);
    plan.time = ctx.timelineTime;
    plan.fps = ctx.fps;
    const int lookMode = std::clamp(p.choice(S("look")), 0, 2);
    plan[G_LOOK] = float(lookMode);
    plan.palette = paletteOf(look.palette);
    plan.display = plan.palette;
    if (lookMode != LookGlitchOnly) {
        plan[G_QMODE] = float(look.qmode);
        plan[G_QBITS_R] = float(look.bits[0]);
        plan[G_QBITS_G] = float(look.bits[1]);
        plan[G_QBITS_B] = float(look.bits[2]);
        plan[G_DITHER] = float(look.dither);
        plan[G_DITHER_AMT] = look.ditherAmt;
        if (look.qmode == QPalette)
            plan.lut = paletteLut(plan.palette);
    }
    plan[G_SCREEN_TYPE] = float(look.screen);
    plan[G_SCREEN_AMT] = float(std::clamp(p.num(S("screen")), 0.0, 1.0));
    plan[G_BACKDROP] = float(look.backdrop);
    plan[G_BORDER] = float(look.border);

    Builder b{plan, look, spec, p, ctx};
    b.A = float(std::clamp(p.num(S("amount")), 0.0, 1.0));
    b.speed = float(std::max(0.0, p.num(S("speed"))));
    b.seed = uint32_t(std::max(0.0, p.num(S("seed"))));
    b.frame = fx::frameIndex(ctx);
    b.t = fx::seconds(ctx);
    b.step = b.speed > 0 ? int64_t(std::floor(b.t * double(b.speed))) : 0;
    b.CW = look.w;
    b.CH = look.h;
    b.GW = plan.gw();
    b.GH = plan.gh();
    b.TX = int(plan[G_TX]);
    b.TY = int(plan[G_TY]);
    b.AX = look.bx;
    b.AY = look.by;

    switch (spec.kind) {
    case KGarble: kindGarble(b, spec.variant); break;
    case KFlicker: kindFlicker(b); break;
    case KLockout: kindLockout(b); break;
    case KScroll: kindScroll(b, spec.variant); break;
    case KRoll: kindRoll(b, spec.variant); break;
    case KHmove: kindHmove(b); break;
    case KRowBars: kindRowBars(b, spec.variant); break;
    case KPalette: kindPalette(b, spec.variant); break;
    case KBits: kindBits(b, spec.variant); break;
    case KCable: kindCable(b, spec.variant); break;
    case KJailbars: kindJailbars(b, spec.variant); break;
    case KDeadLines: kindDeadLines(b, spec.variant); break;
    case KGhost:
        plan.ghost = 0.45f + 0.5f * b.A;
        plan[G_GAIN] = 1.0f - 0.1f * b.A;
        break;
    case KDim: kindDim(b, spec.variant); break;
    case KMode7: kindMode7(b); break;
    case KAffine: kindAffine(b); break;
    case KWave: kindWave(b, spec.variant); break;
    case KMosaic: kindMosaic(b); break;
    case KColorMath: kindColorMath(b, spec.variant); break;
    case KMeshExplode:
    case KMeshWobble:
    case KMeshCracks:
    case KMeshTexture:
    case KMeshWarp:
    case KMeshDrop: kindMesh(b, spec.kind, spec.variant); break;
    case KArtifacts: kindArtifacts(b, spec.variant); break;
    case KFreeze: kindFreeze(b, spec.variant); break;
    case KInterlace: kindInterlace(b); break;
    case KTear: kindTear(b); break;
    case KSparkle: kindSparkle(b, spec.variant); break;
    case KAttr: kindAttr(b); break;
    case KTape: kindTape(b, spec.variant); break;
    case KCgaMode: kindCgaMode(b); break;
    case KVector: kindVector(b, spec.variant); break;
    }
    if (!plan.overlay.isNull()) {
        plan[G_OVL_W] = float(plan.overlay.width());
        plan[G_OVL_H] = float(plan.overlay.height());
    } else {
        plan[G_OVERLAY] = 0;
    }
    finalize(plan);
    return plan;
}

void addEffects(std::vector<EffectDef>& list)
{
    // Consoles alphabetically, glitches in the order listed.
    std::vector<const LookSpec*> looks;
    for (const LookSpec& l : kLooks)
        looks.push_back(&l);
    std::stable_sort(looks.begin(), looks.end(), [](const LookSpec* a, const LookSpec* b) {
        return QString::fromLatin1(a->name).compare(QString::fromLatin1(b->name), Qt::CaseInsensitive) < 0;
    });
    for (const LookSpec* l : looks)
        for (const Spec& s : kSpecs) {
            if (qstrcmp(l->key, s.look) != 0)
                continue;
            EffectDef d;
            d.id = QStringLiteral("vg_%1_%2").arg(QString::fromLatin1(l->key), QString::fromLatin1(s.id));
            d.name = QStringLiteral("%1: %2").arg(QString::fromUtf8(l->tag), QString::fromUtf8(s.name));
            d.category = categoryRoot() + QLatin1Char('/') + QString::fromUtf8(l->name);
            d.description = QString::fromUtf8(s.description);
            ParamDef amount = fx::num("amount", "Intensity", 0, 1, s.amount);
            amount.tooltip = QStringLiteral("How broken the console is");
            ParamDef speed = fx::num("speed", "Speed", 0, 30, s.speed, 2, "/s");
            speed.tooltip = QStringLiteral("How often the glitch changes or how fast it moves (0 = still)");
            ParamDef look = fx::choice("look", "Console Look", {QStringLiteral("Full (resolution, colours, screen)"), QStringLiteral("Colours only"), QStringLiteral("Glitch only")});
            look.tooltip = QStringLiteral("Draw the picture at the console's resolution and colours, or keep the original picture and add only the glitch");
            ParamDef screen = fx::num("screen", "Screen Texture", 0, 1, l->screenDef);
            screen.tooltip = QStringLiteral("CRT scanlines or LCD pixel grid (full look)");
            ParamDef seed = fx::num("seed", "Seed", 0, 9999, 1, 0);
            d.params = {amount, speed, look, screen, seed};
            d.apply = [id = d.id](QImage& img, const Params& p, const RenderContext& ctx, std::unique_ptr<EffectState>& state) {
                const Plan plan = buildPlan(id, p, ctx, img.size());
                render(plan, img, state);
            };
            list.push_back(std::move(d));
        }
}

} // namespace vg
