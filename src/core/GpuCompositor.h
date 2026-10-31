#pragma once

#include "core/FrameRenderer.h"

#include <map>
#include <memory>

class QOffscreenSurface;
namespace nodes {
struct Graph;
}

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
    QImage renderLiveFrame(const LiveFrameJob& job, QImage* clean = nullptr) override;
    QString name() const override;
    bool isGpu() const override { return true; }
    bool renderYuv(const ProjectSnapshot& snap, TimeUs t, AVFrame* frame) override;
    // Effects run through their CPU code so far (download/upload round trips).
    int cpuFallbacks() const;
    // The Effect Builder's preview: `graph` on `source` (each node's state kept from one call to the next), with each
    // node's picture, at most `thumb` big, in `thumbs`.
    QImage renderGraph(const nodes::Graph& graph, const QImage& source, const RenderContext& ctx, std::map<int, QImage>* thumbs, QSize thumb);

    struct Impl;

private:
    explicit GpuCompositor(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};
