#pragma once

#include "core/Effects.h"

#include <QJsonObject>
#include <map>

class QPainter;

// Generated media: events that render their own picture (text, titles,
// credits, solid colours...) instead of decoding a file.

struct GeneratorSpec {
    QString type;              // GeneratorDef::id; empty = not a generated event
    ParamMap params;           // keyframe times are event-local
    QJsonObject data;          // editable graphics scene or embedded audio peak envelope

    bool isEmpty() const { return type.isEmpty(); }
    bool operator==(const GeneratorSpec&) const = default;
};

struct GeneratorContext {
    TimeUs time = 0;           // event-local time
    TimeUs length = 0;         // event length
    TimeUs timelineTime = 0;
    double scale = 1.0;        // output pixels per project pixel
    FrameRate fps;
    bool highQuality = false;
    const QJsonObject* data = nullptr;
};

struct GeneratorDef {
    QString id;
    QString name;
    QString category;
    QString description;
    std::vector<ParamDef> params;
    // Draws into a transparent, canvas-sized premultiplied ARGB32 layer.
    std::function<void(QImage& layer, const Params&, const GeneratorContext&)> render;
    // Optional direct paint path. The GPU compositor supplies an OpenGL paint
    // device; CPU compositing continues to use render(). False requests fallback.
    std::function<bool(QPainter&, QSize, const Params&, const GeneratorContext&)> paint;
    int paintScale = 1; // GPU supersampling for dense, thin primitives
    // True when the picture changes with time even if the parameters don't
    // (built-in animations, credits, timecode). Null = static, cacheable.
    std::function<bool(const Params&, const GeneratorContext&)> isAnimating;
};

// A ready-made starting point shown in the Media Generators tab.
struct GeneratorPreset {
    QString id;
    QString name;
    QString category;
    QString generator;
    std::map<QString, double> numbers;
    std::map<QString, QString> texts;
    TimeUs length = 5 * kUsPerSecond;
};

const std::vector<GeneratorDef>& generators();
const GeneratorDef* findGenerator(const QString& id);
const std::vector<GeneratorPreset>& generatorPresets();
const GeneratorPreset* findGeneratorPreset(const QString& id);

GeneratorSpec makeGenerator(const GeneratorPreset& preset);
// Short description for the timeline, e.g. "Text: Your Title".
QString generatorLabel(const GeneratorSpec& spec);

// Renders a generated event at event-local time t (convenience for previews).
QImage renderGenerator(const GeneratorSpec& spec, QSize size, double scale, TimeUs t, TimeUs length, FrameRate fps);

// The animated backgrounds, overlays, kinetic text etc. (GeneratorsAnimated.cpp),
// appended to generators() / generatorPresets().
void addAnimatedGenerators(std::vector<GeneratorDef>& list);
void addAnimatedGeneratorPresets(std::vector<GeneratorPreset>& list);

QJsonObject generatorToJson(const GeneratorSpec& g);
GeneratorSpec generatorFromJson(const QJsonObject& o);
