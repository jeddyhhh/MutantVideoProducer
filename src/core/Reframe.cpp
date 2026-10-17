#include "core/Reframe.h"

#include "core/FrameConverter.h"
#include "core/PanCrop.h"
#include "core/Segmentation.h"
#include "core/VideoDecoder.h"

#include <algorithm>
#include <cmath>

namespace reframe {

double visibleWidth(double sourceAspect, double frameAspect)
{
    return sourceAspect > 0 ? std::clamp(frameAspect / sourceAspect, 0.0, 1.0) : 1.0;
}

Sample locate(const QImage& frame, const QImage& previous, const QImage& personMask)
{
    Sample s;
    // The person, if one is clearly there.
    if (!personMask.isNull()) {
        const QImage m = personMask.convertToFormat(QImage::Format_Grayscale8);
        double sum = 0, xs = 0;
        for (int y = 0; y < m.height(); ++y) {
            const uchar* row = m.constScanLine(y);
            for (int x = 0; x < m.width(); ++x)
                if (row[x] > 128) {
                    sum += row[x];
                    xs += row[x] * (x + 0.5);
                }
        }
        const double cover = sum / (255.0 * m.width() * m.height());
        if (cover > 0.01) {
            s.x = xs / sum / m.width();
            s.weight = std::min(1.0, cover * 10);
            return s;
        }
    }
    // Else where the picture moves.
    if (previous.isNull() || previous.size() != frame.size())
        return s;
    const QImage a = frame.convertToFormat(QImage::Format_Grayscale8), b = previous.convertToFormat(QImage::Format_Grayscale8);
    double sum = 0, xs = 0;
    for (int y = 0; y < a.height(); ++y) {
        const uchar *ra = a.constScanLine(y), *rb = b.constScanLine(y);
        for (int x = 0; x < a.width(); ++x) {
            const int d = std::abs(int(ra[x]) - int(rb[x]));
            if (d > 12) {
                sum += d;
                xs += d * (x + 0.5);
            }
        }
    }
    const double moving = sum / (255.0 * a.width() * a.height());
    if (moving > 0.002) {
        s.x = xs / sum / a.width();
        s.weight = std::min(1.0, moving * 20);
    }
    return s;
}

std::vector<Sample> follow(const MediaItem& media, int stream, TimeUs from, TimeUs to, TimeUs step,
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
    const bool person = segmentationAvailable(SegmentModel::Fast);
    const QSizeF display = vs->displaySize();
    const QSize size(256, std::max(16, int(std::lround(256.0 * display.height() / std::max(1.0, display.width())))));
    std::vector<Sample> out;
    QImage previous;
    step = std::max<TimeUs>(step, 40000);
    for (TimeUs t = std::max<TimeUs>(0, from); t < to; t += step) {
        const AVFrame* f = decoder.frameAt(t, false);
        if (!f)
            break;
        QImage frame = converter.convert(f, size, false).convertToFormat(QImage::Format_RGB32);
        if (vs->rotation)
            frame = frame.transformed(QTransform().rotate(vs->rotation)).scaled(size);
        Sample s = locate(frame, previous, person ? personMask(frame) : QImage());
        s.t = t;
        out.push_back(s);
        previous = frame;
        if (progress && !progress(double(t - from) / double(std::max<TimeUs>(1, to - from))))
            return {};
    }
    return out;
}

std::vector<Keyframe> path(const std::vector<Sample>& samples, double visible)
{
    std::vector<Keyframe> keys;
    if (samples.empty())
        return keys;
    const double lo = visible / 2, hi = 1 - visible / 2;
    if (lo >= hi) { // the whole width shows: nothing to follow
        keys.push_back({samples.front().t, 0.5, Keyframe::Interp::Linear});
        return keys;
    }
    // Gaps hold the last sure place (the centre before the first).
    std::vector<double> x(samples.size());
    double last = 0.5;
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].weight > 0.05)
            last = samples[i].x;
        x[i] = last;
    }
    // Smoothed over about a second either side (weighted by how sure each sample is).
    std::vector<double> smooth(x.size());
    for (size_t i = 0; i < x.size(); ++i) {
        double sum = 0, weights = 0;
        for (size_t j = 0; j < x.size(); ++j) {
            const double dt = std::abs(double(samples[j].t - samples[i].t)) / kUsPerSecond;
            if (dt > 1.0)
                continue;
            const double w = (1.0 - dt) * (0.2 + samples[j].weight);
            sum += w * x[j];
            weights += w;
        }
        smooth[i] = weights > 0 ? sum / weights : x[i];
    }
    // The crop holds still while the subject stays in its middle part, then eases after it, no
    // faster than a picture width in 1.5 s.
    const double hold = visible * 0.18, maxSpeed = 1.0 / 1.5;
    double centre = std::clamp(smooth.front(), lo, hi);
    keys.push_back({samples.front().t, centre, Keyframe::Interp::Smooth});
    for (size_t i = 1; i < smooth.size(); ++i) {
        const double dt = double(samples[i].t - samples[i - 1].t) / kUsPerSecond;
        const double offset = smooth[i] - centre;
        double move = 0;
        if (std::abs(offset) > hold)
            move = std::clamp(offset - std::copysign(hold, offset), -maxSpeed * dt, maxSpeed * dt);
        centre = std::clamp(centre + move, lo, hi);
        keys.push_back({samples[i].t, centre, Keyframe::Interp::Smooth});
    }
    // Only the keyframes that change the path: drop those a straight line from their neighbours covers.
    std::vector<Keyframe> kept{keys.front()};
    for (size_t i = 1; i + 1 < keys.size(); ++i) {
        const Keyframe& a = kept.back();
        const Keyframe& b = keys[i + 1];
        const double f = double(keys[i].time - a.time) / double(std::max<TimeUs>(1, b.time - a.time));
        if (std::abs(a.value + (b.value - a.value) * f - keys[i].value) > 0.004)
            kept.push_back(keys[i]);
    }
    if (keys.size() > 1)
        kept.push_back(keys.back());
    return kept;
}

int apply(Timeline& tl, const MediaMap& media, const std::vector<int>& events, QSize frame, const std::function<bool(double)>& progress)
{
    int framed = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        Clip* c = tl.findClip(events[i]);
        const auto m = c ? media.find(c->mediaId) : media.end();
        const VideoStreamInfo* vs = m != media.end() ? m->second->info.videoStream(c->streamIndex) : nullptr;
        if (!vs || m->second->info.isStill)
            continue;
        // A reversed or speed-ramped event plays its source out of step with its own time: it gets a still crop
        // where the subject spends most of the time instead of a path.
        const bool still = c->reverse || c->hasRamp();
        const QSize display = vs->displaySize();
        const double visible = visibleWidth(double(display.width()) / std::max(1, display.height()), double(frame.width()) / std::max(1, frame.height()));
        if (visible >= 0.999)
            continue; // the frame is as wide as the picture
        const TimeUs from = c->sourceIn, to = c->sourceIn + (still ? c->sourceSpan() : TimeUs(double(c->length) * c->speed));
        bool cancelled = false;
        const auto samples = follow(*m->second, c->streamIndex, from, to, kUsPerSecond / 4, [&](double f) {
            cancelled = progress && !progress((double(i) + f) / double(events.size()));
            return !cancelled;
        });
        if (cancelled)
            return -1;
        std::vector<Keyframe> keys = path(samples, visible);
        if (keys.empty())
            continue;
        if (still) {
            std::vector<double> xs;
            for (const Keyframe& k : keys)
                xs.push_back(k.value);
            std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
            keys = {Keyframe{0, xs[xs.size() / 2], Keyframe::Interp::Linear}};
        }
        for (Keyframe& k : keys) // source times to the event's own
            k.time = std::clamp<TimeUs>(TimeUs(double(k.time - c->sourceIn) / c->speed), 0, c->length);
        if (c->panCrop.empty())
            c->panCrop = defaultParams(panCropParams());
        c->panCrop[QStringLiteral("mode")].value = 1; // Fill
        c->panCrop[QStringLiteral("centerY")] = ParamValue{0.5};
        ParamValue x;
        x.value = keys.front().value;
        if (keys.size() > 1)
            x.keys = keys;
        c->panCrop[QStringLiteral("centerX")] = x;
        ++framed;
    }
    return framed;
}

} // namespace reframe
