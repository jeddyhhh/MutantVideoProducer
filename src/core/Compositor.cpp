#include "core/Compositor.h"

#include "core/EffectHelpers.h"

#include "core/ImageOps.h"
#include "core/PanCrop.h"
#include "core/EffectMask.h"
#include "core/Proxy.h"
#include "core/Stabilizer.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace {
constexpr uint64_t kEvictAfterFrames = 90;
constexpr size_t kMaxDecoders = 24;

QImage transparentImage(QSize size)
{
    QImage img(size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

void drawLayer(QImage& target, const QImage& layer, double opacity)
{
    if (opacity <= 0.002 || layer.isNull())
        return;
    QPainter p(&target);
    p.setOpacity(std::min(opacity, 1.0));
    p.drawImage(QPointF(0, 0), layer);
}
} // namespace

bool hasActiveEffects(const EffectChain& chain)
{
    return std::any_of(chain.begin(), chain.end(), [](const EffectInstance& e) { return e.enabled; });
}

Compositor::Compositor(bool highQuality, bool transparent) : m_highQuality(highQuality), m_transparent(transparent) {}
Compositor::~Compositor() = default;

void Compositor::reset()
{
    m_sequences.clear(); m_sequenceSnapshots.clear();
    m_entries.clear();
    m_effectStates.clear();
    m_generatorCache.clear();
    m_session.reset();
}

Compositor::Entry* Compositor::acquire(const MediaItem& media, int streamIndex, int clipId, TimeUs sourceTime)
{
    // Reuse a decoder of the same stream that isn't already used for this
    // frame: first the one this clip used last time, otherwise the one
    // positioned closest to (preferably just before) the wanted time, which
    // makes playback across cuts between pieces of one file seamless.
    const QString path = m_useProxies ? proxy::playbackPath(media.path, streamIndex) : media.path;
    Entry* best = nullptr;
    TimeUs bestCost = 0;
    for (auto& e : m_entries) {
        if (e->mediaId != media.id || e->revision != media.revision || e->streamIndex != streamIndex || e->lastUsed == m_tick
            || e->path != path)
            continue;
        if (e->failed)
            return nullptr;
        if (e->lastClipId == clipId) {
            best = e.get();
            break;
        }
        const TimeUs pos = e->decoder->lastRequest();
        const TimeUs cost = sourceTime >= pos ? sourceTime - pos : (pos - sourceTime) * 4;
        if (!best || cost < bestCost) {
            best = e.get();
            bestCost = cost;
        }
    }
    if (!best) {
        auto e = std::make_unique<Entry>();
        e->mediaId = media.id;
        e->revision = media.revision;
        e->streamIndex = streamIndex;
        e->path = path;
        e->decoder = std::make_unique<VideoDecoder>();
        // A proxy holds the video alone, as stream 0.
        e->failed = !e->decoder->open(path, path == media.path ? streamIndex : 0);
        best = e.get();
        m_entries.push_back(std::move(e));
        if (best->failed)
            return nullptr;
    }
    best->lastClipId = clipId;
    best->lastUsed = m_tick;
    return best;
}

void Compositor::evict()
{
    m_entries.erase(std::remove_if(m_entries.begin(), m_entries.end(),
                                   [&](const std::unique_ptr<Entry>& e) {
                                       return !e->failed && m_tick - e->lastUsed > kEvictAfterFrames;
                                   }),
                    m_entries.end());
    while (m_entries.size() > kMaxDecoders) {
        auto oldest = std::min_element(m_entries.begin(), m_entries.end(),
                                       [](const auto& a, const auto& b) { return a->lastUsed < b->lastUsed; });
        if ((*oldest)->lastUsed == m_tick)
            break;
        m_entries.erase(oldest);
    }
    for (auto it = m_effectStates.begin(); it != m_effectStates.end();)
        it = m_tick - it->second.lastUsed > kEvictAfterFrames ? m_effectStates.erase(it) : std::next(it);
    for (auto it = m_generatorCache.begin(); it != m_generatorCache.end();)
        it = m_tick - it->second.lastUsed > kEvictAfterFrames ? m_generatorCache.erase(it) : std::next(it);
}

QRectF Compositor::drawClip(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QImage& target, double opacity)
{
    if (opacity <= 0.002)
        return {};
    if (clip.isSequence()) {
        auto& renderer = m_sequences[clip.id];
        if (!renderer) renderer = std::make_unique<Compositor>(m_highQuality, true);
        renderer->setUseProxies(useProxies());
        const auto& child=m_sequenceSnapshots[clip.id].get(snap,clip.sequence);
        QImage image = renderer->render(child, clip.sourceTimeAt(t), target.size());
        QPainter painter(&target); painter.setOpacity(std::min(opacity, 1.0));
        painter.setTransform(panCropTransform(resolveParams(panCropParams(), clip.panCrop, t - clip.start), QSizeF(target.size()), QSizeF(target.size())));
        painter.drawImage(QPoint(0,0), image);
        return target.rect();
    }
    if (clip.isGenerated())
        return drawGenerated(snap, clip, t, target, opacity);
    auto mit = snap.media.find(clip.mediaId);
    if (mit == snap.media.end())
        return {};
    const MediaItem& media = *mit->second;
    const VideoStreamInfo* vs = media.info.videoStream(clip.streamIndex);
    if (!vs)
        return {};

    const TimeUs srcT = media.info.isStill ? 0 : clip.sourceTimeAt(t);
    Entry* e = acquire(media, clip.streamIndex, clip.id, srcT);
    if (!e)
        return {};
    const AVFrame* frame = e->decoder->frameAt(srcT);
    if (!frame)
        return {};

    // Geometry: source display space (SAR + rotation metadata applied) -> canvas,
    // through the event's Pan/Crop.
    const QSizeF disp(vs->displaySize());
    const Params pc = resolveParams(panCropParams(), clip.panCrop, t - clip.start);
    QTransform view = panCropTransform(pc, disp, QSizeF(target.size()));
    if (clip.stabilize > 0 && !media.info.isStill) // the shake taken out, before the framing
        view = stabilize::correction(media.path, *vs, srcT, clip.stabilize, clip.stabilizeZoom, clip.sourceIn, clip.sourceIn + clip.sourceSpan()) * view;
    const bool sideways = vs->rotation == 90 || vs->rotation == 270;
    const QSizeF upright = sideways ? QSizeF(disp.height(), disp.width()) : disp;
    const bool axisAligned = vs->rotation == 0 && view.type() <= QTransform::TxScale && view.m11() > 0 && view.m22() > 0;
    QRectF dest = view.mapRect(QRectF(QPointF(0, 0), disp));
    if (axisAligned)
        dest = QRectF(QPointF(std::round(dest.left()), std::round(dest.top())), QPointF(std::round(dest.right()), std::round(dest.bottom())));

    // Convert at the size the picture appears, never above source resolution.
    const double viewScale = std::sqrt(std::abs(view.determinant()));
    QSize conv = axisAligned ? dest.size().toSize() : (upright * viewScale).toSize();
    conv = conv.boundedTo(QSize(int(std::lround(upright.width())), int(std::lround(upright.height())))).expandedTo(QSize(1, 1));

    auto convert = [&](const AVFrame* f, TimeUs pts, QImage& img, TimeUs& imgPts, QSize& imgSize) {
        if (img.isNull() || imgPts != pts || imgSize != conv) {
            img = e->converter.convert(f, conv, m_highQuality);
            imgPts = pts;
            imgSize = conv;
        }
        return img;
    };
    QImage picture = convert(frame, e->decoder->currentPts(), e->image, e->imagePts, e->imageSize);
    if (picture.isNull())
        return {};

    // Slow motion with frame blending: mix towards the next frame.
    if (clip.frameBlend && !media.info.isStill)
        if (const AVFrame* next = e->decoder->nextFrame()) {
            const TimeUs p0 = e->decoder->currentPts(), p1 = e->decoder->nextPts();
            const double w = p1 > p0 ? double(srcT - p0) / double(p1 - p0) : 0.0;
            if (w > 0.02 && w < 0.98) {
                const QImage nextPicture = convert(next, p1, e->nextImage, e->nextImagePts, e->nextImageSize);
                if (nextPicture.size() == picture.size()) {
                    QImage blended(picture.size(), QImage::Format_ARGB32_Premultiplied);
                    imageops::mix(blended, &picture, &nextPicture, float(w));
                    picture = std::move(blended);
                }
            }
        }

    QPainter painter(&target);
    painter.setOpacity(std::min(opacity, 1.0));
    if (axisAligned && picture.size() == dest.size().toSize()) {
        painter.drawImage(dest.topLeft(), picture); // 1:1, the common case
    } else {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        if (axisAligned) {
            painter.drawImage(dest, picture);
        } else {
            QTransform m = view; // display -> canvas, then upright picture -> display
            m.translate(disp.width() / 2, disp.height() / 2);
            m.rotate(vs->rotation);
            m.translate(-upright.width() / 2, -upright.height() / 2);
            m.scale(upright.width() / picture.width(), upright.height() / picture.height());
            painter.setTransform(m);
            painter.drawImage(QPointF(0, 0), picture);
        }
    }
    return dest.intersected(QRectF(target.rect()));
}

QRectF Compositor::drawGenerated(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QImage& target, double opacity)
{
    const GeneratorDef* def = findGenerator(clip.generator.type);
    if (!def)
        return {};
    GeneratorContext ctx;
    ctx.time = t - clip.start;
    ctx.length = clip.length;
    ctx.timelineTime = t;
    ctx.scale = double(target.width()) / std::max(1, snap.settings.width);
    ctx.fps = snap.settings.fps;
    ctx.highQuality = m_highQuality;
    ctx.data = &clip.generator.data;
    const Params params = resolveParams(def->params, clip.generator.params, ctx.time);
    const bool animating = def->isAnimating && def->isAnimating(params, ctx);

    // Static generated pictures (most titles) are rendered once and reused.
    GeneratorCache& cache = m_generatorCache[clip.id];
    cache.lastUsed = m_tick;
    QImage rendered;
    if (!animating && cache.type == def->id && cache.size == target.size() && cache.params == params && cache.data == clip.generator.data && !cache.image.isNull()) {
        rendered = cache.image;
    } else {
        rendered = QImage(target.size(), QImage::Format_ARGB32_Premultiplied);
        rendered.fill(Qt::transparent);
        def->render(rendered, params, ctx);
        if (!animating) {
            cache.type = def->id;
            cache.size = target.size();
            cache.params = params;
            cache.data = clip.generator.data;
            cache.image = rendered;
        }
    }
    if (isDefaultPanCrop(clip.panCrop)) {
        drawLayer(target, rendered, opacity);
    } else {
        // Pan/Crop on generated media frames the rendered full-size picture.
        const Params pc = resolveParams(panCropParams(), clip.panCrop, ctx.time);
        QPainter painter(&target);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setOpacity(std::min(opacity, 1.0));
        painter.setTransform(panCropTransform(pc, QSizeF(target.size()), QSizeF(target.size())));
        painter.drawImage(QPointF(0, 0), rendered);
    }
    return QRectF(target.rect());
}

QImage Compositor::clipLayer(const ProjectSnapshot& snap, const Clip& clip, TimeUs t, QSize size, const RenderContext& base)
{
    QImage layer = transparentImage(size);
    const QRectF rect = drawClip(snap, clip, t, layer, 1.0);
    if (rect.isEmpty())
        return {};
    if (hasActiveEffects(clip.effects)) {
        RenderContext ctx = base;
        ctx.time = t - clip.start;
        ctx.contentRect = rect;
        applyChain(clip.effects, layer, ctx);
    }
    if (clip.gain < 0.999f) {
        QPainter p(&layer);
        p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        p.fillRect(layer.rect(), QColor(0, 0, 0, int(std::clamp(clip.gain, 0.0f, 1.0f) * 255)));
    }
    return layer;
}

void Compositor::applyChain(const EffectChain& chain, QImage& img, const RenderContext& ctx)
{
    for (const EffectInstance& e : chain) {
        if (!e.enabled)
            continue;
        // A mask limits the effect (or rack) to part of the picture.
        const EffectMask mask = effectMask(e.params, ctx.time, img.size());
        const QImage unmasked = mask.active() ? img : QImage(); // shared: the effect detaches img
        if (e.isRack()) {
            applyRack(e, img, ctx);
            applyEffectMask(img, unmasked, mask);
            continue;
        }
        const EffectDef* def = findEffect(e.type);
        if (!def || !def->apply)
            continue;
        StateEntry& st = m_effectStates[e.id];
        st.lastUsed = m_tick;
        if (st.type != e.type) {
            st.state.reset();
            st.type = e.type;
        }
        const Params params = resolveParams(def->params, e.params, ctx.time);
        const fx::PostMask post = def->glitchPost ? fx::postMask(params, img.size()) : fx::PostMask{};
        const QImage before = post.active() ? img : QImage(); // shared: the effect detaches img
        if (!unmasked.isNull() || !before.isNull())
            img.bits(); // detach here: effects write rows from several threads, and each scanLine() would detach
        def->apply(img, params, ctx, st.state);
        if (post.active())
            fx::applyPostMask(img, before, post);
        applyEffectMask(img, unmasked, mask);
    }
}

void Compositor::applyRack(const EffectInstance& rack, QImage& img, const RenderContext& ctx)
{
    // Each chain processes the rack's input; results are layered bottom-up
    // over the dry picture, then mixed with it by Dry/Wet.
    const auto chains = activeRackChains(rack);
    if (chains.empty())
        return;
    QImage acc = img.copy();
    for (const RackChain* c : chains) {
        QImage layer = img.copy();
        applyChain(c->effects, layer, ctx);
        const Params cp = resolveParams(rackChainParams(false), c->params, ctx.time);
        imageops::blendLayer(acc, layer, float(cp.num(QStringLiteral("level")) / 100.0), cp.choice(QStringLiteral("blend")));
    }
    const double mix = resolveParams(rackParams(), rack.params, ctx.time).num(QStringLiteral("mix")) / 100.0;
    if (mix >= 0.999) {
        img = std::move(acc);
    } else {
        QImage out(img.size(), QImage::Format_ARGB32_Premultiplied);
        imageops::mix(out, &img, &acc, float(std::max(0.0, mix)));
        img = std::move(out);
    }
}

void Compositor::applyTransition(const TransitionSpec& spec, QImage& target, const QImage* from, const QImage* to,
                                 double p, double opacity, const RenderContext& ctx)
{
    const TransitionDef* def = findTransition(spec.type);
    if (!def)
        def = findTransition(QStringLiteral("dissolve"));
    QImage out = transparentImage(target.size());
    def->apply(out, from && !from->isNull() ? from : nullptr, to && !to->isNull() ? to : nullptr, std::clamp(p, 0.0, 1.0),
               resolveParams(def->params, spec.params, ctx.timelineTime), ctx);
    drawLayer(target, out, opacity);
}

void Compositor::renderTrack(const ProjectSnapshot& snap, const Track& track, TimeUs t, QImage& canvas,
                             const RenderContext& base)
{
    std::vector<size_t> active;
    for (size_t ci = 0; ci < track.clips.size(); ++ci) {
        const Clip& c = track.clips[ci];
        if (c.start > t)
            break;
        if (!c.muted && t < c.end())
            active.push_back(ci);
    }
    if (active.empty())
        return;

    // Track FX and blend modes work on the whole track's picture.
    const bool useTrackLayer = hasActiveEffects(track.effects) || track.blend != 0;
    QImage trackLayer;
    if (useTrackLayer)
        trackLayer = transparentImage(canvas.size());
    QImage& target = useTrackLayer ? trackLayer : canvas;
    // With a track layer the track level is applied once, after track FX.
    const double trackLevel = track.compositeAt(t); // level slider x composite envelope
    const double levelMul = useTrackLayer ? 1.0 : trackLevel;
    const QSize size = canvas.size();

    if (active.size() >= 2) {
        // Overlap: transition from the earlier event to the later one.
        const Clip& a = track.clips[active[active.size() - 2]];
        const Clip& b = track.clips[active.back()];
        const double p = fadeShape(b.fadeInCurve, double(t - b.start) / double(std::max<TimeUs>(1, std::min(a.end(), b.end()) - b.start)));
        if (b.transitionIn.isPlainFade() && !hasActiveEffects(a.effects) && !hasActiveEffects(b.effects)) {
            // Fast path: the earlier event stays opaque, the later fades in on top.
            drawClip(snap, a, t, target, levelMul * a.gain);
            drawClip(snap, b, t, target, levelMul * b.gain * std::clamp(p, 0.0, 1.0));
        } else if (b.transitionIn.isPlainFade()) {
            drawLayer(target, clipLayer(snap, a, t, size, base), levelMul);
            drawLayer(target, clipLayer(snap, b, t, size, base), levelMul * std::clamp(p, 0.0, 1.0));
        } else {
            const QImage la = clipLayer(snap, a, t, size, base);
            const QImage lb = clipLayer(snap, b, t, size, base);
            applyTransition(b.transitionIn, target, &la, &lb, p, levelMul, base);
        }
    } else {
        const Clip& c = track.clips[active.front()];
        const bool inFadeIn = c.fadeIn > 0 && t < c.start + c.fadeIn;
        const bool inFadeOut = c.fadeOut > 0 && t >= c.end() - c.fadeOut;
        if (inFadeIn && !c.transitionIn.isPlainFade()) {
            const QImage layer = clipLayer(snap, c, t, size, base);
            applyTransition(c.transitionIn, target, nullptr, &layer, fadeShape(c.fadeInCurve, double(t - c.start) / double(c.fadeIn)), levelMul, base);
        } else if (inFadeOut && !c.transitionOut.isPlainFade()) {
            const QImage layer = clipLayer(snap, c, t, size, base);
            applyTransition(c.transitionOut, target, &layer, nullptr, 1.0 - fadeShape(c.fadeOutCurve, double(c.end() - t) / double(c.fadeOut)),
                            levelMul, base);
        } else {
            const double env = envelopeGain(c, ClipEnvelope{c.fadeIn, c.fadeOut}, t);
            if (hasActiveEffects(c.effects))
                drawLayer(target, clipLayer(snap, c, t, size, base), levelMul * env);
            else
                drawClip(snap, c, t, target, levelMul * c.gain * env);
        }
    }

    if (useTrackLayer) {
        RenderContext ctx = base;
        ctx.time = t;
        ctx.contentRect = QRectF(trackLayer.rect());
        applyChain(track.effects, trackLayer, ctx);
        if (track.blend != 0)
            imageops::blendLayer(canvas, trackLayer, float(trackLevel), track.blend);
        else
            drawLayer(canvas, trackLayer, trackLevel);
    }
}

QImage Compositor::render(const ProjectSnapshot& snap, TimeUs t, QSize outSize)
{
    if (m_session != snap.session) {
        reset();
        m_session = snap.session;
    }
    ++m_tick;
    QImage canvas(outSize, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(m_transparent ? Qt::transparent : Qt::black);

    RenderContext base;
    base.time = t;
    base.timelineTime = t;
    base.scale = double(outSize.width()) / std::max(1, snap.settings.width);
    base.fps = snap.settings.fps.toDouble();
    base.highQuality = m_highQuality;
    base.contentRect = QRectF(canvas.rect());

    const auto& tracks = snap.timeline.tracks;
    const bool anySolo = std::any_of(tracks.begin(), tracks.end(),
                                     [](const Track& tr) { return isVisualTrack(tr.type) && tr.solo; });
    // Track 0 is the top layer: composite bottom-up.
    for (int ti = int(tracks.size()) - 1; ti >= 0; --ti) {
        const Track& track = tracks[ti];
        if (!isVisualTrack(track.type) || track.muted || (anySolo && !track.solo))
            continue;
        renderTrack(snap, track, t, canvas, base);
    }

    if (hasActiveEffects(snap.timeline.outputEffects)) {
        applyChain(snap.timeline.outputEffects, canvas, base);
        if (!m_transparent) {
        // Effects may leave transparency (keying, transforms); the output is opaque.
        QImage flat(outSize, QImage::Format_ARGB32_Premultiplied);
        flat.fill(Qt::black);
        QPainter(&flat).drawImage(0, 0, canvas);
        canvas = std::move(flat);
        }
    }
    evict();
    return canvas;
}

QImage Compositor::renderLive(const QImage& frame, const EffectChain& chain, TimeUs t, double fps, bool keepAlpha)
{
    ++m_tick;
    QImage canvas = frame.format() == QImage::Format_ARGB32_Premultiplied ? frame : frame.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (canvas.isNull() || !hasActiveEffects(chain))
        return canvas;
    RenderContext ctx;
    ctx.time = 0;
    ctx.timelineTime = t;
    ctx.scale = double(canvas.width()) / 1920.0; // effect sizes as if the picture were 1080p
    ctx.fps = fps;
    ctx.highQuality = m_highQuality;
    ctx.contentRect = QRectF(canvas.rect());
    applyChain(chain, canvas, ctx);
    if (keepAlpha) {
        evict();
        return canvas;
    }
    QImage flat(canvas.size(), QImage::Format_ARGB32_Premultiplied);
    flat.fill(Qt::black);
    QPainter(&flat).drawImage(0, 0, canvas);
    evict();
    return flat;
}
