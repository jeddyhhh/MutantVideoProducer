#include "core/CubeLut.h"

#include "core/ImageOps.h"
#include "core/Parallel.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <algorithm>
#include <cmath>
#include <mutex>

std::shared_ptr<CubeLut> parseCubeLut(const QByteArray& text, QString* error)
{
    auto fail = [&](const QString& why) -> std::shared_ptr<CubeLut> {
        if (error)
            *error = why;
        return nullptr;
    };
    auto lut = std::make_shared<CubeLut>();
    int size3d = 0, size1d = 0;
    std::vector<float> values;
    for (const QByteArray& raw : text.split('\n')) {
        const QByteArray line = raw.trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        const QList<QByteArray> parts = line.simplified().split(' ');
        const QByteArray key = parts.front().toUpper();
        if (key == "TITLE") {
            lut->title = QString::fromUtf8(line.mid(5).trimmed()).remove(QLatin1Char('"'));
        } else if (key == "LUT_3D_SIZE" && parts.size() >= 2) {
            size3d = parts[1].toInt();
        } else if (key == "LUT_1D_SIZE" && parts.size() >= 2) {
            size1d = parts[1].toInt();
        } else if (key == "DOMAIN_MIN" && parts.size() >= 4) {
            for (int c = 0; c < 3; ++c)
                lut->domainMin[c] = parts[c + 1].toFloat();
        } else if (key == "DOMAIN_MAX" && parts.size() >= 4) {
            for (int c = 0; c < 3; ++c)
                lut->domainMax[c] = parts[c + 1].toFloat();
        } else if (key == "LUT_1D_INPUT_RANGE" || key == "LUT_3D_INPUT_RANGE") {
            if (parts.size() >= 3)
                for (int c = 0; c < 3; ++c) {
                    lut->domainMin[c] = parts[1].toFloat();
                    lut->domainMax[c] = parts[2].toFloat();
                }
        } else if (parts.size() >= 3) {
            bool ok0, ok1, ok2;
            const float r = parts[0].toFloat(&ok0), g = parts[1].toFloat(&ok1), b = parts[2].toFloat(&ok2);
            if (ok0 && ok1 && ok2) {
                values.push_back(r);
                values.push_back(g);
                values.push_back(b);
            }
        }
    }
    // "nan" and "inf" read as numbers: they would become table positions out of range.
    for (int c = 0; c < 3; ++c) {
        if (!std::isfinite(lut->domainMin[c]))
            lut->domainMin[c] = 0.0f;
        if (!std::isfinite(lut->domainMax[c]))
            lut->domainMax[c] = 1.0f;
    }
    for (float& v : values)
        if (!std::isfinite(v))
            v = 0.0f;
    if (size3d >= 2 && size3d <= 256) {
        if (values.size() < size_t(size3d) * size3d * size3d * 3)
            return fail(QStringLiteral("the file has %1 of the %2 entries it declares").arg(values.size() / 3).arg(size3d * size3d * size3d));
        lut->size = size3d;
        values.resize(size_t(size3d) * size3d * size3d * 3);
        lut->rgb = std::move(values);
        return lut;
    }
    if (size1d >= 2 && size1d <= 65536) {
        if (values.size() < size_t(size1d) * 3)
            return fail(QStringLiteral("the file has %1 of the %2 entries it declares").arg(values.size() / 3).arg(size1d));
        // A 1D LUT (one curve per channel) as a 33-point 3D table.
        auto curve = [&](int ch, float x) {
            const float pos = std::clamp(x, 0.0f, 1.0f) * float(size1d - 1);
            const int i = std::min(size1d - 2, int(pos));
            const float f = pos - float(i);
            return values[size_t(i) * 3 + size_t(ch)] * (1 - f) + values[size_t(i + 1) * 3 + size_t(ch)] * f;
        };
        constexpr int n = 33;
        lut->size = n;
        lut->rgb.resize(size_t(n) * n * n * 3);
        for (int b = 0; b < n; ++b)
            for (int g = 0; g < n; ++g)
                for (int r = 0; r < n; ++r) {
                    float* o = &lut->rgb[(size_t(b) * n * n + size_t(g) * n + size_t(r)) * 3];
                    o[0] = curve(0, float(r) / (n - 1));
                    o[1] = curve(1, float(g) / (n - 1));
                    o[2] = curve(2, float(b) / (n - 1));
                }
        return lut;
    }
    return fail(QStringLiteral("no LUT_3D_SIZE or LUT_1D_SIZE line: not a .cube LUT"));
}

std::shared_ptr<const CubeLut> loadCubeLut(const QString& path, QString* error)
{
    static std::mutex mutex;
    static QHash<QString, std::pair<qint64, std::shared_ptr<const CubeLut>>> cache;
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists()) {
        if (error)
            *error = path.isEmpty() ? QStringLiteral("no LUT file chosen") : QStringLiteral("%1 not found").arg(path);
        return nullptr;
    }
    const qint64 stamp = fi.lastModified().toMSecsSinceEpoch() ^ fi.size();
    {
        std::lock_guard lock(mutex);
        auto it = cache.find(fi.absoluteFilePath());
        if (it != cache.end() && it->first == stamp)
            return it->second;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = f.errorString();
        return nullptr;
    }
    if (f.size() > 1024LL * 1024 * 1024) { // (the biggest, 256 points a side, is about 500 MB of text)
        if (error)
            *error = QStringLiteral("the file is too big to be a .cube LUT");
        return nullptr;
    }
    std::shared_ptr<const CubeLut> lut = parseCubeLut(f.readAll(), error);
    if (lut) {
        std::lock_guard lock(mutex);
        cache.insert(fi.absoluteFilePath(), {stamp, lut});
    }
    return lut;
}

void CubeLut::lookup(const float in[3], float out[3]) const
{
    // Same sampling as the GPU's linear-filtered 3D texture.
    float pos[3];
    int i0[3], i1[3];
    float f[3];
    for (int c = 0; c < 3; ++c) {
        const float n = std::clamp((in[c] - domainMin[c]) / std::max(1e-6f, domainMax[c] - domainMin[c]), 0.0f, 1.0f);
        pos[c] = n * float(size - 1);
        i0[c] = std::min(size - 2, int(pos[c]));
        i1[c] = i0[c] + 1;
        f[c] = pos[c] - float(i0[c]);
    }
    auto at = [&](int r, int g, int b, int ch) { return rgb[(size_t(b) * size * size + size_t(g) * size + size_t(r)) * 3 + size_t(ch)]; };
    for (int ch = 0; ch < 3; ++ch) {
        const float c00 = at(i0[0], i0[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i0[2], ch) * f[0];
        const float c10 = at(i0[0], i1[1], i0[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i0[2], ch) * f[0];
        const float c01 = at(i0[0], i0[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i0[1], i1[2], ch) * f[0];
        const float c11 = at(i0[0], i1[1], i1[2], ch) * (1 - f[0]) + at(i1[0], i1[1], i1[2], ch) * f[0];
        const float c0 = c00 * (1 - f[1]) + c10 * f[1];
        const float c1 = c01 * (1 - f[1]) + c11 * f[1];
        out[ch] = c0 * (1 - f[2]) + c1 * f[2];
    }
}

void applyCubeLut(QImage& img, const CubeLut& lut, float mix)
{
    if (lut.size < 2 || mix <= 0)
        return;
    mix = std::min(mix, 1.0f);
    const int w = img.width();
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const uint32_t px = row[x];
                const int a = int(px >> 24);
                if (a == 0)
                    continue;
                const float fa = float(a) / 255.0f;
                float in[3], out[3];
                for (int c = 0; c < 3; ++c)
                    in[c] = std::clamp(float((px >> (16 - 8 * c)) & 0xff) / 255.0f / fa, 0.0f, 1.0f);
                lut.lookup(in, out);
                uint32_t res = uint32_t(a) << 24;
                for (int c = 0; c < 3; ++c) {
                    const float v = std::clamp(in[c] + (out[c] - in[c]) * mix, 0.0f, 1.0f) * fa;
                    res |= uint32_t(std::clamp(int(v * 255.0f + 0.5f), 0, 255)) << (16 - 8 * c);
                }
                row[x] = res;
            }
        }
    });
}
