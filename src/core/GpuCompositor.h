#pragma once

#include "core/FrameRenderer.h"

#include <memory>

class QOffscreenSurface;

// OpenGL 3.3 renderer with the same output as the CPU Compositor:
// YUV->RGB conversion, scaling, pan/crop, blending, effects and transitions
// run as shaders. Effects without a GPU implementation (e.g. FFmpeg Filter)
// transparently fall back to their CPU code. Not thread-safe; create and use
// it on one thread.
class GpuCompositor : public FrameRenderer {
public:
    static std::unique_ptr<GpuCompositor> create(QOffscreenSurface* surface, bool highQuality, QString* error);
    ~GpuCompositor() override;

    QImage render(const ProjectSnapshot& snap, TimeUs t, QSize outSize) override;
    QImage renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha = false) override;
    QImage renderLiveChains(const QImage& frame, const std::vector<const EffectChain*>& chains, TimeUs t, double fps) override;
    QString name() const override;
    bool isGpu() const override { return true; }
    bool renderYuv(const ProjectSnapshot& snap, TimeUs t, AVFrame* frame) override;
    // Effects run through their CPU code so far (download/upload round trips).
    int cpuFallbacks() const;

    struct Impl;

private:
    explicit GpuCompositor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};
