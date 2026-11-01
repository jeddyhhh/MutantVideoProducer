#pragma once

// Screen shaders in the libretro format (RetroArch's GLSL shader presets: CRT screens, scanlines, handheld LCDs, NTSC,
// pixel-art upscalers...), run on the GPU renderer as video effects. MVP has no shaders of its own: the user downloads
// libretro's collection (Tools > Get Screen Shaders), each shader under its own licence, and every preset (.glslp) in it
// becomes an effect "retro.<folder>/<name>" under Retro Shaders, its #pragma parameters its settings.

#include "core/Effects.h"

#include <QString>
#include <functional>
#include <map>
#include <vector>

namespace retro {

enum class Scale { Source, Viewport, Absolute };
enum class Wrap { Border, Edge, Repeat, Mirror };

struct Pass {
    QString path;                 // the .glsl file
    bool linear = false;          // its input filtered linearly (else nearest)
    Wrap wrap = Wrap::Border;     // its input's edges
    bool mipmapInput = false;
    Scale scaleX = Scale::Source, scaleY = Scale::Source;
    double sx = 1, sy = 1;        // scale factors, or pixels (Absolute)
    bool scaleSet = false;        // (unset on the last pass: the viewport)
    bool floatBuffer = false, srgbBuffer = false;
    int frameCountMod = 0;
    QString alias;                // its output also as <alias>Texture
};
struct Lut {
    QString name, path;
    bool linear = true;
    Wrap wrap = Wrap::Border;
    bool mipmap = false;
};
struct Parameter {
    QString id, name;
    double def = 0, min = 0, max = 1, step = 0.01;
};
struct Preset {
    QString path;
    std::vector<Pass> passes;
    std::vector<Lut> luts;
    std::vector<Parameter> parameters;     // every pass's #pragma parameter, preset overrides applied
    int history = 0;                       // how many earlier frames the passes read (PrevNTexture)
};

// Reads a .glslp (and its passes' parameters). False with `error` when it can't be used.
bool loadPreset(const QString& path, Preset* out, QString* error = nullptr);
// A pass's source, with the lines RetroArch adds before it (the stage, the parameters as uniforms).
QString passSource(const QString& glsl, bool vertex);

// Where the shaders are kept (the app data folder's "screen-shaders"), and whether there are any.
QString folder();
bool installed();
// Unpacks libretro's glsl-shaders zip (as GitHub gives it) into the folder, replacing what was there.
bool installZip(const QString& zipPath, QString* error, const std::function<bool(double)>& progress = {});

// The presets in the folder as effects (read once; again after reload()).
std::vector<const EffectDef*> effects();
const EffectDef* findEffect(const QString& id);
const Preset* preset(const QString& id); // (null: not one, or it couldn't be read)
void reload();
// The settings MVP adds in front of a preset's own: the picture fed to it at a retro height (scanlines need lines).
constexpr const char* kSourceLines = "retro.lines";

} // namespace retro
