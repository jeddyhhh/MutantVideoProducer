#pragma once

#include "core/Project.h"

#include <QImage>
#include <QString>
#include <memory>
#include <vector>

class QOffscreenSurface;
struct AVFrame;

// The Live tab's picture for one frame: the captured picture (at the output size), the second source (the layer) with
// its own FX laid over it, then the Live FX, the active pad's chain (or, while switching, the last pad's fading into
// it with a transition) and the held pads'. Each chain's result is flattened onto black, as renderLive does.
struct LiveFrameLayer {
    QImage image;                        // as captured; fitted into rect
    QRect rect;
    bool cover = false;                  // fill rect, cropped (a full-frame layer), rather than squeezed in
    double opacity = 1.0;
    int blend = 0;                       // BlendMode
    const EffectChain* fx = nullptr;
    bool overEffects = false;            // laid over after every chain, rather than under the Live FX
};
struct LiveFrameJob {
    QImage frame;
    std::vector<LiveFrameLayer> layers;  // lowest first
    const EffectChain* liveFx = nullptr;
    double fadeProgress = -1;            // switching pads (0..1): fadeFrom's picture into fadeTo's; < 0: not
    const EffectChain* fadeFrom = nullptr;
    const EffectChain* fadeTo = nullptr;
    TransitionSpec fadeSpec;
    std::vector<const EffectChain*> after; // the active pad's (when not switching), the held pads'
    TimeUs time = 0;
    double fps = 30;
};

// Live helpers (the CPU's way, which the GPU follows): a picture fitted to `size` (squeezed, or filling it and
// cropped), and two pictures blended by a transition at progress p, flattened onto black.
QImage fitLayer(const QImage& layer, QSize size, bool cover);
QImage liveTransition(const QImage& from, const QImage& to, double p, const TransitionSpec& spec, double fps);

// Renders composited timeline pictures. Implemented by the CPU Compositor
// and the OpenGL GpuCompositor. Each instance belongs to one thread.
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;
    virtual QImage render(const ProjectSnapshot& snap, TimeUs t, QSize outSize) = 0;
    // Live tab: applies `chain` to a captured picture (premultiplied ARGB32,
    // already at the output size). `t` is the live clock (animation and
    // time-based effects, and keyframes). Returns an opaque image.
    // keepAlpha: leave transparency (a keyed layer) instead of flattening onto black.
    virtual QImage renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha = false) = 0;
    // The Live tab's whole picture for a frame (LiveFrameJob); `clean`, if given, gets the picture before the effects
    // (with the layer). This one works on the CPU, a step at a time; the GPU's sends the picture up and reads it back
    // once.
    virtual QImage renderLiveFrame(const LiveFrameJob& job, QImage* clean = nullptr);
    virtual QString name() const = 0;
    virtual bool isGpu() const { return false; }
    // Optional direct encoder-plane output. Unsupported layouts use render().
    virtual bool renderYuv(const ProjectSnapshot&, TimeUs, AVFrame*) { return false; }
    // The editor's preview: decode proxies where they exist (core/Proxy.h).
    // Off by default, so exports and stills always read the original files.
    void setUseProxies(bool on) { m_useProxies = on; }
    bool useProxies() const { return m_useProxies; }

protected:
    bool m_useProxies = false;
};

// GPU renderer when enabled (hw::settings().gpuCompositing) and OpenGL 3.3
// works on `gpuSurface`; otherwise the CPU compositor. Call from the thread
// that will render. `gpuSurface` must have been created on the GUI thread.
std::unique_ptr<FrameRenderer> createFrameRenderer(bool highQuality, QOffscreenSurface* gpuSurface,
                                                   QString* gpuError = nullptr);

// Offscreen surface for GPU rendering. Must be called on the GUI thread.
std::unique_ptr<QOffscreenSurface> createGpuSurface();
