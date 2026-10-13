#pragma once

#include "core/Project.h"

#include <QImage>
#include <QString>
#include <memory>

class QOffscreenSurface;
struct AVFrame;

// Renders composited timeline pictures. Implemented by the CPU Compositor
// and the OpenGL GpuCompositor. Each instance belongs to one thread.
class FrameRenderer {
public:
    virtual ~FrameRenderer() = default;
    virtual QImage render(const ProjectSnapshot& snap, TimeUs t, QSize outSize) = 0;
    // Live tab: applies `chain` to a captured picture (premultiplied ARGB32,
    // already at the output size). `t` is the live clock (animation and
    // time-based effects); keyframes are read at 0. Returns an opaque image.
    // keepAlpha: leave transparency (a keyed layer) instead of flattening onto black.
    virtual QImage renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha = false) = 0;
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
