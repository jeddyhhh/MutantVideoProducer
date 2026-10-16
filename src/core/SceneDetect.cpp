#include "core/SceneDetect.h"

#include "core/FrameConverter.h"
#include "core/VideoDecoder.h"

#include <QImage>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>

namespace scenes {
namespace {
constexpr int kBins = 16; // per channel

using Histogram = std::array<float, 3 * kBins>;

Histogram histogram(const QImage& img)
{
    Histogram h{};
    const int n = img.width() * img.height();
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            h[size_t(qRed(row[x]) * kBins / 256)] += 1;
            h[size_t(kBins + qGreen(row[x]) * kBins / 256)] += 1;
            h[size_t(2 * kBins + qBlue(row[x]) * kBins / 256)] += 1;
        }
    }
    for (float& v : h)
        v /= float(n);
    return h;
}

// 0 (the same colours) .. 1 (nothing in common).
double difference(const Histogram& a, const Histogram& b)
{
    double common = 0;
    for (size_t i = 0; i < a.size(); ++i)
        common += std::min(a[i], b[i]);
    return 1.0 - common / 3.0;
}
} // namespace

std::vector<TimeUs> detect(const MediaItem& media, int stream, TimeUs from, TimeUs to, double sensitivity,
                           const std::function<bool(double)>& progress, QString* error)
{
    const VideoStreamInfo* vs = media.info.videoStream(stream);
    if (!vs || media.info.isStill) {
        if (error)
            *error = QStringLiteral("not a video");
        return {};
    }
    VideoDecoder decoder;
    if (!decoder.open(media.path, stream, error, false))
        return {};
    FrameConverter converter;
    from = std::max<TimeUs>(0, from);
    to = std::min(to, media.info.duration > 0 ? media.info.duration : to);
    const TimeUs step = std::max<TimeUs>(1000, vs->frameRate.frameDuration());
    const TimeUs minShot = kUsPerSecond / 2;
    // A cut is a jump well above both a fixed floor and the recent frame-to-frame changes (so fast
    // motion and camera moves within a shot don't count).
    const double s = std::clamp(sensitivity, 0.0, 1.0);
    const double floor = 0.55 - 0.4 * s, ratio = 6.0 - 3.0 * s;
    std::vector<TimeUs> cuts;
    std::deque<double> recent;
    Histogram previous{};
    bool havePrevious = false;
    TimeUs lastCut = from, lastPts = -1;
    for (TimeUs t = from; t < to; t += step) {
        const AVFrame* f = decoder.frameAt(t, true);
        if (!f)
            break;
        const TimeUs pts = decoder.currentPts();
        if (pts == lastPts)
            continue; // (a variable frame rate: the same frame again)
        lastPts = pts;
        const Histogram h = histogram(converter.convert(f, QSize(64, 36), false).convertToFormat(QImage::Format_RGB32));
        if (havePrevious) {
            const double d = difference(previous, h);
            double mean = 0;
            for (double r : recent)
                mean += r;
            mean = recent.empty() ? 0 : mean / double(recent.size());
            if (d > floor && d > ratio * mean && pts - lastCut >= minShot && to - pts >= minShot / 2) {
                cuts.push_back(pts);
                lastCut = pts;
                recent.clear(); // a new shot: its own motion from here
            } else {
                recent.push_back(d);
                if (recent.size() > 12)
                    recent.pop_front();
            }
        }
        previous = h;
        havePrevious = true;
        if (progress && !progress(double(t - from) / double(std::max<TimeUs>(1, to - from))))
            return {};
    }
    return cuts;
}

} // namespace scenes
