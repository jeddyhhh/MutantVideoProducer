#include "core/MotionTracker.h"

#include "core/FrameConverter.h"
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"

#include <QImage>
#include <algorithm>
#include <cmath>

namespace tracker {
namespace {

constexpr int kWidth = 480; // analysis width

struct Gray {
    int w = 0, h = 0;
    std::vector<float> p;
    float at(int x, int y) const { return p[size_t(std::clamp(y, 0, h - 1)) * size_t(w) + size_t(std::clamp(x, 0, w - 1))]; }
};

Gray toGray(const QImage& img)
{
    Gray g;
    g.w = img.width();
    g.h = img.height();
    g.p.resize(size_t(g.w) * size_t(g.h));
    for (int y = 0; y < g.h; ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < g.w; ++x)
            g.p[size_t(y) * size_t(g.w) + size_t(x)] = float(qRed(row[x]) * 0.299 + qGreen(row[x]) * 0.587 + qBlue(row[x]) * 0.114);
    }
    return g;
}

} // namespace

QPointF storedToDisplay(QPointF p, int rotation)
{
    switch (rotation) {
    case 90: return {1 - p.y(), p.x()};
    case 180: return {1 - p.x(), 1 - p.y()};
    case 270: return {p.y(), 1 - p.x()};
    default: return p;
    }
}

QPointF displayToStored(QPointF p, int rotation)
{
    switch (rotation) {
    case 90: return {p.y(), 1 - p.x()};
    case 180: return {1 - p.x(), 1 - p.y()};
    case 270: return {1 - p.y(), p.x()};
    default: return p;
    }
}

std::vector<Point> track(const QString& path, int stream, TimeUs from, TimeUs to, QRectF box, const std::function<bool(double)>& progress,
                         QString* error)
{
    std::vector<Point> out;
    const MediaInfo info = probeMedia(path);
    const VideoStreamInfo* vs = info.videoStream(stream);
    VideoDecoder dec;
    QString err;
    if (!vs || !dec.open(path, stream, &err, false)) {
        if (error)
            *error = vs ? err : QStringLiteral("no video");
        return out;
    }
    FrameConverter conv;
    const QSize size(kWidth, std::max(16, int(std::lround(double(kWidth) * vs->height / std::max(1, vs->width)))));
    const AVFrame* f = dec.frameAt(from);
    if (!f) {
        if (error)
            *error = QStringLiteral("no picture there");
        return out;
    }
    Gray g = toGray(conv.convert(f, size, false).convertToFormat(QImage::Format_RGB32));
    // The template: the box's picture, minus its mean (brightness changes don't count).
    const int tw = std::clamp(int(std::lround(box.width() * size.width())), 8, size.width() / 2);
    const int th = std::clamp(int(std::lround(box.height() * size.height())), 8, size.height() / 2);
    double cx = box.center().x() * size.width(), cy = box.center().y() * size.height();
    const int step = std::max(1, std::max(tw, th) / 32); // sample bigger boxes sparsely
    std::vector<std::pair<int, int>> samples;
    for (int y = -th / 2; y < th / 2; y += step)
        for (int x = -tw / 2; x < tw / 2; x += step)
            samples.emplace_back(x, y);
    std::vector<float> tpl(samples.size());
    auto patch = [&](const Gray& img, double px, double py, std::vector<float>& v) {
        double mean = 0;
        for (size_t i = 0; i < samples.size(); ++i) {
            // bilinear, for fractional positions
            const double x = px + samples[i].first, y = py + samples[i].second;
            const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
            const double fx = x - x0, fy = y - y0;
            v[i] = float((img.at(x0, y0) * (1 - fx) + img.at(x0 + 1, y0) * fx) * (1 - fy) + (img.at(x0, y0 + 1) * (1 - fx) + img.at(x0 + 1, y0 + 1) * fx) * fy);
            mean += v[i];
        }
        mean /= double(std::max<size_t>(1, samples.size()));
        for (float& s : v)
            s -= float(mean);
    };
    patch(g, cx, cy, tpl);
    double contrast = 0;
    for (float s : tpl)
        contrast += std::abs(s);
    contrast = std::max(1.0, contrast);
    out.push_back({dec.currentPts(), QPointF(cx / size.width(), cy / size.height()), 1.0f});
    double vx = 0, vy = 0; // where it was heading (pixels per frame)
    std::vector<float> cand(samples.size());
    auto cost = [&](double px, double py) {
        patch(g, px, py, cand);
        double s = 0;
        for (size_t i = 0; i < cand.size(); ++i)
            s += std::abs(cand[i] - tpl[i]);
        return s;
    };
    const int rx = std::max(12, int(tw * 0.6)), ry = std::max(12, int(th * 0.6));
    while (true) {
        if (!dec.nextFrame())
            break;
        const TimeUs prevPts = dec.currentPts();
        f = dec.frameAt(dec.nextPts());
        if (!f || dec.currentPts() <= prevPts || dec.currentPts() > to)
            break;
        g = toGray(conv.convert(f, size, false).convertToFormat(QImage::Format_RGB32));
        // Coarse (every 2 pixels) around where it should be, then every pixel, then a parabola.
        const double ex = cx + vx, ey = cy + vy;
        double best = 1e300, bx = ex, by = ey;
        for (int dy = -ry; dy <= ry; dy += 2)
            for (int dx = -rx; dx <= rx; dx += 2)
                if (const double c = cost(ex + dx, ey + dy); c < best) {
                    best = c;
                    bx = ex + dx;
                    by = ey + dy;
                }
        const double cx0 = bx, cy0 = by;
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (const double c = cost(cx0 + dx, cy0 + dy); c < best) {
                    best = c;
                    bx = cx0 + dx;
                    by = cy0 + dy;
                }
        auto sub = [](double l, double c, double r) {
            const double d = l - 2 * c + r;
            return d > 0 ? std::clamp(0.5 * (l - r) / d, -0.5, 0.5) : 0.0;
        };
        const double sx = sub(cost(bx - 1, by), best, cost(bx + 1, by)), sy = sub(cost(bx, by - 1), best, cost(bx, by + 1));
        bx += sx;
        by += sy;
        vx = 0.7 * vx + 0.3 * (bx - cx);
        vy = 0.7 * vy + 0.3 * (by - cy);
        cx = std::clamp(bx, 0.0, double(size.width()));
        cy = std::clamp(by, 0.0, double(size.height()));
        const float match = float(std::clamp(1.0 - best / contrast / 2.0, 0.0, 1.0));
        out.push_back({dec.currentPts(), QPointF(cx / size.width(), cy / size.height()), match});
        // Follow slow changes of the picture.
        patch(g, cx, cy, cand);
        for (size_t i = 0; i < tpl.size(); ++i)
            tpl[i] = 0.85f * tpl[i] + 0.15f * cand[i];
        if (progress && to > from && !progress(double(dec.currentPts() - from) / double(to - from)))
            break;
    }
    return out;
}

} // namespace tracker
