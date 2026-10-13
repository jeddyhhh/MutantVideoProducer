#pragma once

// Live video sources for the Live tab: cameras and capture cards, whole
// screens and single windows, opened through FFmpeg's capture devices
// (Windows: DirectShow, Windows.Graphics.Capture with a GDI fallback;
// Linux: V4L2 and X11), or video files played in a loop (a playlist: one
// after the other, at their own speed), read on a background thread.

#include <QImage>
#include <QPointF>
#include <QRectF>
#include <cmath>
#include <QRect>
#include <QSize>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

struct LiveSource {
    enum class Kind { Device, Screen, Window, File };
    Kind kind = Kind::Device;
    QString name;   // shown in the UI
    QString id;     // device name / path, monitor or window handle; files: paths, one per line
    QRect geometry; // screens: desktop area in pixels
    bool operator==(const LiveSource&) const = default;
};

// Cameras / capture cards, then screens, then windows (top-level, visible, titled).
std::vector<LiveSource> listLiveSources();

// An output of a fixed shape (portrait for phones, square...) and how a
// picture of another shape goes into it.
enum class LiveFraming { CropFill, Fit, FitBlur };
// Pan and zoom of the source in the output: the point of the source (0..1
// across and down) at the middle of the output, and how far in (1 = as framed).
struct LivePanZoom {
    QPointF center{0.5, 0.5};
    double zoom = 1.0;
    bool isDefault() const { return zoom <= 1.0001 && std::abs(center.x() - 0.5) < 1e-4 && std::abs(center.y() - 0.5) < 1e-4; }
    bool operator==(const LivePanZoom&) const = default;
};
inline constexpr double kLiveMaxZoom = 8.0;
// Where the whole source picture lands in a `canvas`-sized output (it may
// reach outside it): scaled to cover it (CropFill) or fit in it, times the
// zoom, placed so the pan's centre is in the middle, but never leaving an
// edge of the output uncovered that the framing would cover (a picture
// smaller than the output stays centred). `used` gets the centre that was
// possible.
QRectF sourceRectInCanvas(QSizeF source, QSizeF canvas, LiveFraming framing, const LivePanZoom& pz, QPointF* used = nullptr);
// The picture in a `canvas`-sized frame: cropped to fill it, fitted with
// black bars, or fitted over a blurred, darkened copy of itself; panned and
// zoomed by `pz` (the middle, as it is, by default).
QImage frameToCanvas(const QImage& picture, QSize canvas, LiveFraming framing, const LivePanZoom& pz = {});
// What to capture at so CropFill only has to crop (never above the source).
QSize coverSize(QSize source, QSize canvas);

// What to ask a camera for (empty / 0 = its default).
struct LiveFormat {
    QSize size;
    double fps = 0;
};

class LiveCapture {
public:
    LiveCapture();
    ~LiveCapture();
    LiveCapture(const LiveCapture&) = delete;
    LiveCapture& operator=(const LiveCapture&) = delete;

    // Opens the source and starts reading frames on a thread. Pictures are
    // scaled to fit `maxSize` (keeping the aspect; empty = source size).
    bool start(const LiveSource& source, const LiveFormat& format, QSize maxSize, QString* error);
    void stop();
    // Changes the size pictures are scaled to fit (takes effect on the next frame).
    void setMaxSize(QSize maxSize);

    // Latest picture (premultiplied ARGB32), or null before the first one.
    // `serial` counts captured frames.
    QImage latest(uint64_t* serial = nullptr) const;
    QSize sourceSize() const;
    double captureFps() const;  // measured
    QString failure() const;    // set when reading stopped because of an error
    QString description() const; // how the source was opened ("DirectShow 1280x720 mjpeg"...)
    // Files: skips to the next one in the playlist; the one playing now.
    void nextFile();
    QString currentFile() const;

    struct Impl;

private:
    std::shared_ptr<Impl> d; // shared with the reading thread, which may outlive a stop()
};
