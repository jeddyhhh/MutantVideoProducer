#pragma once

#include "core/OpticalFlow.h"

#include "core/Effects.h"
#include "core/FrameConverter.h"
#include "core/FrameRenderer.h"
#include "core/Project.h"
#include "core/VideoDecoder.h"

#include <QImage>
#include <map>
#include <memory>
#include <vector>

// Renders the composited timeline picture at a given time. Owns its own
// decoders and effect state, so each thread that renders (preview, export)
// needs its own Compositor. Not thread-safe.
//
// Pipeline per video track (bottom track first):
//   event frame -> event FX -> transition with neighbour (if any)
//   -> track FX -> composite with track level; then video output FX.
class Compositor : public FrameRenderer {
public:
    explicit Compositor(bool highQuality = false, bool transparent = false);
    ~Compositor() override;

    // Renders timeline time t at outSize (normally the project size, or a
    // fraction of it for preview).
    QImage render(const ProjectSnapshot& snap, TimeUs t, QSize outSize) override;
    QImage renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha = false) override;
    QString name() const override { return QStringLiteral("CPU"); }

    // Drops all decoders and effect state (e.g. after media was relinked).
    void reset();

private:
    struct Entry {
        int mediaId = 0;
        int revision = 0; // MediaItem::revision (relinked media gets new decoders)
        int streamIndex = -1;
        QString path; // the file decoded (the media's, or its proxy)
        std::unique_ptr<VideoDecoder> decoder;
        FrameConverter converter;
        bool failed = false;
        int lastClipId = 0;
        uint64_t lastUsed = 0;
        // Converted image cache: skips conversion when the frame repeats.
        QImage image;
        TimeUs imagePts = -1;
        QSize imageSize;
        // Same for the following frame (frame blending).
        QImage nextImage;
        TimeUs nextImagePts = -1;
        QSize nextImageSize;
        // Optical flow between the frame and the next (the same pair for every in-between frame).
        flow::Field flow;
        TimeUs flowPts = -1;
        QSize flowSize;
        // Noise reduction: the frame before, and the cleaned-up frame (until the frame or the strength changes).
        QImage prevImage;
        TimeUs prevImagePts = -1;
        QSize prevImageSize;
        QImage denoised;
        TimeUs denoisedPts = -1;
        double denoisedStrength = 0;
    };
    struct GeneratorCache {
        QString type;
        Params params;
        QJsonObject data;
        QSize size;
        QImage image;
        uint64_t lastUsed = 0;
    };
    struct StateEntry {
        std::unique_ptr<EffectState> state;
        QString type; // the effect the state belongs to (an id reused by another kind starts afresh)
        uint64_t lastUsed = 0;
    };

    // With a matte (the track matte above it, rendered), the track shows only through it (TrackMatte matteMode).
    void renderTrack(const ProjectSnapshot& snap, const Track& track, TimeUs t, QImage& canvas, const RenderContext& base,
                     const QImage* matte = nullptr, int matteMode = 0);
    // Draws the event's picture into `target` (fitted, letterboxed); returns
    // the rectangle it occupies, or an empty rect if nothing was drawn.
    QRectF drawClip(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QImage& target, double opacity);
    QRectF drawGenerated(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QImage& target, double opacity);
    // The event as a canvas-sized layer with its FX applied (null if nothing to show).
    QImage clipLayer(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QSize size, const RenderContext& base);
    // The track's adjustment layers at t, on the picture so far (everything below them).
    void applyAdjustments(const Track& track, TimeUs t, QImage& canvas, const RenderContext& base);
    void applyChain(const EffectChain& chain, QImage& img, const RenderContext& ctx);
    void applyRack(const EffectInstance& rack, QImage& img, const RenderContext& ctx);
    void applyTransition(const TransitionSpec& spec, QImage& target, const QImage* from, const QImage* to, double p,
                         double opacity, const RenderContext& ctx);

    Entry* acquire(const MediaItem& media, int streamIndex, int clipId, TimeUs sourceTime);
    void evict();

    std::vector<std::unique_ptr<Entry>> m_entries;
    std::map<int, StateEntry> m_effectStates; // by EffectInstance::id
    std::map<int, GeneratorCache> m_generatorCache; // by clip id
    std::shared_ptr<const int> m_session;
    uint64_t m_tick = 0;
    bool m_highQuality;
    bool m_transparent = false;
    std::map<int, std::unique_ptr<Compositor>> m_sequences;
    std::map<int, SequenceSnapshotCache> m_sequenceSnapshots;
};

bool hasActiveEffects(const EffectChain& chain);
