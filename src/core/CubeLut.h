#pragma once

// 3D colour lookup tables from .cube files (Adobe / Resolve format), as used
// by the "LUT (.cube)" effect. 1D .cube files are turned into a 3D table.

#include <QImage>
#include <QString>
#include <memory>
#include <vector>

struct CubeLut {
    int size = 0;                 // entries per side
    std::vector<float> rgb;       // size^3 RGB triples, red fastest (as in the file)
    float domainMin[3] = {0, 0, 0};
    float domainMax[3] = {1, 1, 1};
    QString title;

    // Trilinear lookup of a colour in 0..1.
    void lookup(const float in[3], float out[3]) const;
};

// Loads (and caches by path and modification time) a .cube file; null with
// an error message on failure.
std::shared_ptr<const CubeLut> loadCubeLut(const QString& path, QString* error = nullptr);
// Parses .cube text.
std::shared_ptr<CubeLut> parseCubeLut(const QByteArray& text, QString* error = nullptr);

// Applies a LUT to a premultiplied ARGB32 image, mixed with the original by `mix` (0..1).
void applyCubeLut(QImage& img, const CubeLut& lut, float mix);
