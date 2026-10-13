#include "core/Scopes.h"

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace scopes {
namespace {

constexpr double kKr = 0.2126, kKb = 0.0722; // Rec. 709

// The picture as 32-bit RGB, and the step between the pixels read.
struct Samples {
    QImage img;
    int sx = 1, sy = 1;
};

Samples prepare(const QImage& picture)
{
    Samples s;
    s.img = picture.format() == QImage::Format_RGB32 || picture.format() == QImage::Format_ARGB32 ? picture
                                                                                                 : picture.convertToFormat(QImage::Format_RGB32);
    s.sx = std::max(1, s.img.width() / 512);
    s.sy = std::max(1, s.img.height() / 288);
    return s;
}

template <class F>
void forEachSample(const Samples& s, F&& f)
{
    for (int y = 0; y < s.img.height(); y += s.sy) {
        const QRgb* row = reinterpret_cast<const QRgb*>(s.img.constScanLine(y));
        for (int x = 0; x < s.img.width(); x += s.sx)
            f(x, row[x]);
    }
}

QFont labelFont()
{
    QFont f;
    f.setPixelSize(9);
    return f;
}

const QColor kBackground(16, 16, 20);
const QColor kGrid(255, 255, 255, 40);
const QColor kLabel(170, 170, 178);

// Levels as brightness: about half on for an evenly spread picture, full where many pixels pile up.
QImage density(const std::vector<uint32_t>& counts, const std::vector<std::array<uint32_t, 3>>* colours, int w, int h, uint32_t samples,
               QColor tint)
{
    QImage out(w, h, QImage::Format_ARGB32_Premultiplied);
    out.fill(Qt::transparent);
    const double k = 0.69 * double(w) * h / std::max<uint32_t>(1, samples) * 0.25;
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const size_t i = size_t(y) * size_t(w) + size_t(x);
            const uint32_t n = counts[i];
            if (!n)
                continue;
            const double v = std::min(1.0, 0.4 + 0.6 * (1.0 - std::exp(-k * n)));
            int r = tint.red(), g = tint.green(), b = tint.blue();
            if (colours) { // the pixels' own colour, at full brightness
                const auto& c = (*colours)[i];
                const double m = std::max({c[0], c[1], c[2], 1u});
                r = int(80 + 175 * c[0] / m);
                g = int(80 + 175 * c[1] / m);
                b = int(80 + 175 * c[2] / m);
            }
            const int a = int(v * 255);
            row[x] = qRgba(r * a / 255, g * a / 255, b * a / 255, a);
        }
    }
    return out;
}

void levelGrid(QPainter& p, const QRect& area, bool labels)
{
    p.setFont(labelFont());
    for (int pct = 0; pct <= 100; pct += 25) {
        const int y = area.bottom() - int(std::lround((area.height() - 1) * pct / 100.0));
        p.setPen(QPen(kGrid, 1, pct == 0 || pct == 100 ? Qt::SolidLine : Qt::DashLine));
        p.drawLine(area.left(), y, area.right(), y);
        if (labels) {
            p.setPen(kLabel);
            p.drawText(QRect(0, y - 6, area.left() - 3, 12), Qt::AlignRight | Qt::AlignVCenter, QString::number(pct));
        }
    }
}

QImage waveform(const Samples& s, QSize size, bool parade)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(kBackground);
    const QRect area(28, 6, std::max(16, size.width() - 34), std::max(16, size.height() - 12));
    QPainter p(&out);
    levelGrid(p, area, true);
    const int panels = parade ? 3 : 1, gap = parade ? 4 : 0;
    const int pw = std::max(4, (area.width() - gap * (panels - 1)) / panels), ph = area.height();
    const int iw = std::max(1, s.img.width());
    std::vector<std::vector<uint32_t>> counts(size_t(panels), std::vector<uint32_t>(size_t(pw) * size_t(ph), 0));
    uint32_t samples = 0;
    forEachSample(s, [&](int x, QRgb c) {
        const int col = std::min(pw - 1, x * pw / iw);
        ++samples;
        auto add = [&](int panel, int level) {
            const int row = (255 - level) * (ph - 1) / 255;
            ++counts[size_t(panel)][size_t(row) * size_t(pw) + size_t(col)];
        };
        if (parade) {
            add(0, qRed(c));
            add(1, qGreen(c));
            add(2, qBlue(c));
        } else {
            add(0, luma(c));
        }
    });
    static const QColor tints[3] = {QColor(255, 50, 50), QColor(50, 235, 80), QColor(60, 110, 255)};
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    for (int i = 0; i < panels; ++i)
        p.drawImage(area.left() + i * (pw + gap), area.top(),
                    density(counts[size_t(i)], nullptr, pw, ph, samples, parade ? tints[i] : QColor(150, 255, 160)));
    return out;
}

QImage vectorscope(const Samples& s, QSize size)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(kBackground);
    const int side = std::max(32, std::min(size.width(), size.height()) - 12);
    const QPointF c(size.width() / 2.0, size.height() / 2.0);
    const double radius = side / 2.0;
    const double scale = radius * 0.95 / 0.5; // Cb / Cr of +-0.5 near the rim
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    // Graticule: rim, cross, the skin tone line, the 75% colour bar targets.
    p.setPen(QPen(kGrid, 1));
    p.drawEllipse(c, radius, radius);
    p.drawLine(QPointF(c.x() - radius, c.y()), QPointF(c.x() + radius, c.y()));
    p.drawLine(QPointF(c.x(), c.y() - radius), QPointF(c.x(), c.y() + radius));
    const double skin = 123.0 * std::numbers::pi / 180.0;
    p.setPen(QPen(QColor(255, 200, 150, 90), 1, Qt::DashLine));
    p.drawLine(c, c + QPointF(std::cos(skin), -std::sin(skin)) * radius);
    p.setFont(labelFont());
    struct Target {
        const char* name;
        QRgb rgb;
    };
    static const Target targets[] = {{"R", qRgb(191, 0, 0)},   {"Yl", qRgb(191, 191, 0)}, {"G", qRgb(0, 191, 0)},
                                     {"Cy", qRgb(0, 191, 191)}, {"B", qRgb(0, 0, 191)},    {"Mg", qRgb(191, 0, 191)}};
    for (const Target& t : targets) {
        const QPointF v = vectorPosition(t.rgb);
        const QPointF pt = c + QPointF(v.x(), -v.y()) * scale;
        p.setPen(QPen(QColor(qRed(t.rgb), qGreen(t.rgb), qBlue(t.rgb)).lighter(160), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(pt.x() - 5, pt.y() - 5, 10, 10));
        const QPointF dir = (pt - c) / std::max(1.0, std::hypot(pt.x() - c.x(), pt.y() - c.y()));
        p.setPen(kLabel);
        p.drawText(QRectF(pt.x() + dir.x() * 14 - 10, pt.y() + dir.y() * 14 - 6, 20, 12), Qt::AlignCenter, QString::fromLatin1(t.name));
    }
    // The picture's colours.
    const int n = std::max(8, side);
    std::vector<uint32_t> counts(size_t(n) * size_t(n), 0);
    std::vector<std::array<uint32_t, 3>> colours(size_t(n) * size_t(n), {0, 0, 0});
    uint32_t samples = 0;
    forEachSample(s, [&](int, QRgb rgb) {
        const QPointF v = vectorPosition(rgb);
        const int x = int(std::lround(n / 2.0 + v.x() * scale));
        const int y = int(std::lround(n / 2.0 - v.y() * scale));
        ++samples;
        if (x < 0 || y < 0 || x >= n || y >= n)
            return;
        const size_t i = size_t(y) * size_t(n) + size_t(x);
        ++counts[i];
        colours[i][0] += uint32_t(qRed(rgb));
        colours[i][1] += uint32_t(qGreen(rgb));
        colours[i][2] += uint32_t(qBlue(rgb));
    });
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    p.drawImage(QPointF(c.x() - n / 2.0, c.y() - n / 2.0), density(counts, &colours, n, n, samples / 4 + 1, Qt::white));
    return out;
}

QImage histogramImage(const Samples& s, QSize size)
{
    QImage out(size, QImage::Format_ARGB32_Premultiplied);
    out.fill(kBackground);
    Levels lv;
    forEachSample(s, [&](int, QRgb c) {
        ++lv.bins[0][size_t(qRed(c))];
        ++lv.bins[1][size_t(qGreen(c))];
        ++lv.bins[2][size_t(qBlue(c))];
        ++lv.bins[3][size_t(luma(c))];
    });
    const QRect area(6, 6, std::max(16, size.width() - 12), std::max(16, size.height() - 20));
    QPainter p(&out);
    p.setFont(labelFont());
    for (int pct = 0; pct <= 100; pct += 25) {
        const int x = area.left() + int(std::lround((area.width() - 1) * pct / 100.0));
        p.setPen(QPen(kGrid, 1, pct == 0 || pct == 100 ? Qt::SolidLine : Qt::DashLine));
        p.drawLine(x, area.top(), x, area.bottom());
        p.setPen(kLabel);
        p.drawText(QRect(x - 15, area.bottom() + 2, 30, 12), Qt::AlignCenter, QString::number(pct));
    }
    // Scaled to the tallest level inside (pure black and white often spike).
    uint32_t top = 1;
    for (int ch = 0; ch < 4; ++ch)
        for (int i = 1; i < 255; ++i)
            top = std::max(top, lv.bins[size_t(ch)][size_t(i)]);
    auto curve = [&](int ch) {
        QPainterPath path;
        path.moveTo(area.left(), area.bottom());
        for (int i = 0; i < 256; ++i) {
            const double x = area.left() + (area.width() - 1) * i / 255.0;
            const double v = std::min(1.0, double(lv.bins[size_t(ch)][size_t(i)]) / top);
            path.lineTo(x, area.bottom() - v * (area.height() - 1));
        }
        path.lineTo(area.right(), area.bottom());
        path.closeSubpath();
        return path;
    };
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setCompositionMode(QPainter::CompositionMode_Plus);
    static const QColor fills[3] = {QColor(200, 40, 40, 200), QColor(40, 190, 60, 200), QColor(50, 80, 220, 200)};
    for (int ch = 0; ch < 3; ++ch) {
        p.setBrush(fills[ch]);
        p.drawPath(curve(ch));
    }
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(QColor(235, 235, 240, 220), 1.2));
    p.drawPath(curve(3));
    return out;
}

} // namespace

QStringList names()
{
    return {QCoreApplication::translate("Scopes", "Waveform"), QCoreApplication::translate("Scopes", "RGB Parade"),
            QCoreApplication::translate("Scopes", "Vectorscope"), QCoreApplication::translate("Scopes", "Histogram")};
}

int luma(QRgb c)
{
    return std::clamp(int(std::lround(kKr * qRed(c) + (1 - kKr - kKb) * qGreen(c) + kKb * qBlue(c))), 0, 255);
}

QPointF vectorPosition(QRgb c)
{
    const double r = qRed(c) / 255.0, g = qGreen(c) / 255.0, b = qBlue(c) / 255.0;
    const double y = kKr * r + (1 - kKr - kKb) * g + kKb * b;
    return QPointF((b - y) / (2 * (1 - kKb)), (r - y) / (2 * (1 - kKr)));
}

Levels histogram(const QImage& picture)
{
    Levels lv;
    if (picture.isNull())
        return lv;
    const Samples s = prepare(picture);
    forEachSample(s, [&](int, QRgb c) {
        ++lv.bins[0][size_t(qRed(c))];
        ++lv.bins[1][size_t(qGreen(c))];
        ++lv.bins[2][size_t(qBlue(c))];
        ++lv.bins[3][size_t(luma(c))];
        ++lv.samples;
    });
    return lv;
}

QImage render(const QImage& picture, Kind kind, QSize size)
{
    size = size.expandedTo(QSize(64, 48));
    if (picture.isNull()) {
        QImage out(size, QImage::Format_ARGB32_Premultiplied);
        out.fill(kBackground);
        return out;
    }
    const Samples s = prepare(picture);
    switch (kind) {
    case Kind::Waveform:
        return waveform(s, size, false);
    case Kind::Parade:
        return waveform(s, size, true);
    case Kind::Vectorscope:
        return vectorscope(s, size);
    case Kind::Histogram:
        return histogramImage(s, size);
    }
    return {};
}

} // namespace scopes
