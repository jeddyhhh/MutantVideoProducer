// TimelineWidget: painting.
#include "ui/TimelineWidget.h"
#include "ui/TimelineLayout.h"
#include "core/AudioFinishing.h"
#include "core/AudioMixer.h"
#include "core/PanCrop.h"
#include "core/Presets.h"
#include "core/TimelineOps.h"
#include "engine/MediaCache.h"
#include "ui/EffectsBrowser.h"
#include "ui/AudioLevels.h"
#include "ui/MediaBin.h"
#include "ui/Theme.h"
#include <QClipboard>
#include <QContextMenuEvent>
#include <QCursor>
#include <QPixmap>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QApplication>
#include <QPaintEvent>
#include <QPainterPath>
#include <QJsonArray>
#include <QJsonObject>
#include <QToolTip>
#include <QWheelEvent>
#include <QTemporaryDir>
#include <QScrollBar>
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

using namespace timeline_layout;

void TimelineWidget::paintMarkers(QPainter& p)
{
    const int W = viewport()->width();
    const QRect bar(kHeaderWidth, kRegionBarH, W - kHeaderWidth, kMarkerBarH);
    p.fillRect(bar, theme::c().markerBar);
    p.setPen(theme::c().deep);
    p.drawLine(bar.left(), bar.bottom(), bar.right(), bar.bottom());
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.8);
    p.setFont(f);
    const QFontMetrics fm(f);
    const auto& markers = m_project->timeline().markers;
    for (const Marker& m : markers) {
        if (!m.isRegion())
            continue;
        const double x0 = timeToX(m.time), x1 = timeToX(m.end());
        if (x1 < kHeaderWidth || x0 > W)
            continue;
        const QColor c = markerColor(m.color);
        QColor fill = c;
        fill.setAlpha(70);
        const QRectF r(x0, bar.top() + 2, x1 - x0, bar.height() - 4);
        p.fillRect(r, fill);
        p.fillRect(QRectF(x0, bar.top(), 2, bar.height()), c);
        p.fillRect(QRectF(x1 - 2, bar.top(), 2, bar.height()), c);
        p.setPen(c.lighter(150));
        const double tx = std::max(x0, double(kHeaderWidth)) + 5;
        p.drawText(QRectF(tx, r.top(), std::max(0.0, x1 - tx - 4), r.height()), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(markerLabel(m), Qt::ElideRight, int(std::max(0.0, x1 - tx - 4))));
    }
    for (const Marker& m : markers) {
        if (m.isRegion())
            continue;
        const double x = timeToX(m.time);
        const QString label = markerLabel(m);
        const int w = std::min(160, fm.horizontalAdvance(label) + 8);
        if (x + w < kHeaderWidth || x > W)
            continue;
        const QColor c = markerColor(m.color);
        p.fillRect(QRectF(x, bar.top() + 1, w, bar.height() - 3), c);
        p.fillRect(QRectF(x, bar.top(), 1.5, bar.height()), c.lighter(130));
        p.setPen(QColor(0x18, 0x18, 0x18));
        p.drawText(QRectF(x + 4, bar.top() + 1, w - 6, bar.height() - 3), Qt::AlignVCenter | Qt::AlignLeft,
                   fm.elidedText(label, Qt::ElideRight, w - 6));
    }
}

void TimelineWidget::paintGrid(QPainter& p, const QRect& band)
{
    const TimeUs g = gridStep();
    if (g <= 0 || double(g) * m_pxPerSec / kUsPerSecond < 5) // too dense to draw (snapping still works)
        return;
    const TimeUs from = xToTime(band.left()), to = xToTime(band.right());
    int64_t n = int64_t(std::floor(double(from - m_gridOffset) / double(g)));
    for (TimeUs t = m_gridOffset + n * g; t <= to; t += g, ++n) {
        if (t < 0)
            continue;
        const bool strong = m_grid == Grid::Beats ? n % 4 == 0 : m_grid == Grid::Frames ? false : (t % (5 * kUsPerSecond)) == 0;
        p.setPen(ink(strong ? 70 : 34));
        const double x = timeToX(t);
        p.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
    }
}

void TimelineWidget::paintEnvelopes(QPainter& p, const Track& track, int top, int h)
{
    const int W = viewport()->width();
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.75);
    p.setFont(f);
    auto drawLine = [&](const Envelope& env, EnvelopeKind k, const QColor& color, TimeUs origin, int bandTop, int bandH,
                        double x0, double x1, TimeUs maxT, const QString& name) {
        // Sample the curve every 2 px (smooth/hold segments are drawn as they play).
        QPolygonF line;
        for (double x = x0; x <= x1 + 2; x += 2)
            line << QPointF(x, envValueToY(k, env.at(xToTime(x) - origin), bandTop, bandH));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(0, 0, 0, 140), 3));
        p.drawPolyline(line);
        p.setPen(QPen(color, 1.5));
        p.drawPolyline(line);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const Keyframe& pt : env.points) {
            const double x = timeToX(origin + pt.time);
            if (x < x0 - 4 || x > x1 + 4 || pt.time < 0 || pt.time > maxT)
                continue;
            const QRectF r(x - 3.5, envValueToY(k, pt.value, bandTop, bandH) - 3.5, 7, 7);
            p.fillRect(r, Qt::white);
            p.setPen(color);
            p.drawRect(r);
        }
        p.setPen(color);
        p.drawText(QPointF(x0 + 4, envValueToY(k, env.at(xToTime(x0 + 4) - origin), bandTop, bandH) - 4), name);
    };
    for (EnvelopeKind k : {EnvelopeKind::Composite, EnvelopeKind::Pan, EnvelopeKind::Volume}) {
        const Envelope& env = *track.envelope(k);
        if (!env.active)
            continue;
        const QColor color = k == EnvelopeKind::Volume ? QColor(0x5a, 0xa8, 0xff)
                             : k == EnvelopeKind::Pan  ? QColor(0xff, 0x6e, 0x6e)
                                                       : QColor(0xc8, 0x8c, 0xff);
        const QString name = k == EnvelopeKind::Volume ? tr("Vol") : (k == EnvelopeKind::Pan ? tr("Pan") : tr("Level"));
        drawLine(env, k, color, 0, top, h, kHeaderWidth, W, std::numeric_limits<TimeUs>::max() / 4, name);
    }
    // Speed ramps: each event's velocity envelope inside its body.
    const QColor velocityColor(0xff, 0xb8, 0x4d);
    for (const Clip& c : track.clips) {
        if (!c.velocity.active)
            continue;
        const double x0 = std::max(timeToX(c.start), double(kHeaderWidth)), x1 = std::min(timeToX(c.end()), double(W));
        if (x1 - x0 < 2)
            continue;
        const int bandTop = top + 1 + kTitleH, bandH = h - 2 - kTitleH;
        p.save();
        p.setClipRect(QRectF(x0, bandTop, x1 - x0, bandH), Qt::IntersectClip);
        const double y100 = envValueToY(EnvelopeKind::Velocity, 1.0, bandTop, bandH);
        p.setPen(QPen(QColor(0xff, 0xb8, 0x4d, 110), 1, Qt::DotLine));
        p.drawLine(QPointF(x0, y100), QPointF(x1, y100)); // 100% reference
        drawLine(c.velocity, EnvelopeKind::Velocity, velocityColor, c.start, bandTop, bandH, x0, x1, c.length, tr("Velocity"));
        p.restore();
    }
}

void TimelineWidget::paintGenerated(QPainter& p, const Clip& c, const QRectF& body)
{
    if (body.height() < 8)
        return;
    const ProjectSettings& s = m_project->settings();
    const double thumbH = body.height();
    const double thumbW = std::max(8.0, thumbH * s.width / std::max(1, s.height));
    const QSize size{int(thumbW), int(thumbH)};
    // Rendered once per settings change; shows the picture mid-way through the event.
    GeneratedThumb& thumb = m_generatedThumbs[c.id];
    if (thumb.image.isNull() || thumb.size != size || !(thumb.spec == c.generator) || thumb.length != c.length) {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(QColor(0x2a, 0x22, 0x3a));
        const QImage gen = renderGenerator(c.generator, size, thumbW / std::max(1, s.width), c.length / 2, c.length, s.fps);
        QPainter(&img).drawImage(0, 0, gen);
        thumb = {c.generator, c.length, size, img};
    }
    const int W = viewport()->width();
    const double vis0 = std::max(body.left(), double(kHeaderWidth)), vis1 = std::min(body.right(), double(W));
    for (int i = std::max(0, int((vis0 - body.left()) / thumbW)); body.left() + i * thumbW < vis1; ++i)
        p.drawImage(QPointF(body.left() + i * thumbW, body.top()), thumb.image);
}

std::vector<TimelineWidget::MoodMark> TimelineWidget::moodMarks(const Clip& c, const QRectF& body) const
{
    std::vector<MoodMark> out;
    if (c.generator.type != QLatin1String("graphics_overlay") || body.height() < 16)
        return out;
    const QJsonObject variables = c.generator.data.value(QStringLiteral("variables")).toObject();
    int row = 0;
    for (auto it = variables.begin(); it != variables.end(); ++it) {
        if (!it.key().endsWith(QLatin1String("_mood")) || !it.value().isObject())
            continue;
        const QJsonArray keys = it.value().toObject().value(QStringLiteral("keyframes")).toArray();
        const double y = body.bottom() - 13 - (row++ % 2) * 13; // (two rows, so neighbours' marks don't sit on each other)
        for (int i = 0; i < keys.size(); ++i) {
            const QJsonArray k = keys[i].toArray();
            const double x = timeToX(c.start + TimeUs(k[0].toDouble() * kUsPerSecond));
            out.push_back({it.key(), i, k[0].toDouble(), k[1].toString(), QRectF(x - 6, y, 13, 12)});
        }
    }
    return out;
}

void TimelineWidget::paintMoods(QPainter& p, const Clip& c, const QRectF& body)
{
    static const std::map<QString, QColor> colours{{QStringLiteral("angry"), QColor(0xd0, 0x00, 0x00)}, {QStringLiteral("sad"), QColor(0x3a, 0x86, 0xff)},
                                                   {QStringLiteral("surprised"), QColor(0xff, 0xbe, 0x0b)}, {QStringLiteral("smug"), QColor(0x83, 0x38, 0xec)},
                                                   {QStringLiteral("neutral"), QColor(0x8d, 0x99, 0xae)}};
    const auto marks = moodMarks(c, body);
    if (marks.empty())
        return;
    p.save();
    QFont f = font();
    f.setPixelSize(9);
    f.setBold(true);
    p.setFont(f);
    p.setRenderHint(QPainter::Antialiasing, true);
    for (const MoodMark& m : marks) {
        const auto colour = colours.find(m.mood);
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.setBrush(colour == colours.end() ? QColor(0x8d, 0x99, 0xae) : colour->second);
        p.drawRoundedRect(m.rect, 3, 3);
        p.setPen(Qt::white);
        p.drawText(m.rect, Qt::AlignCenter, m.variable.left(1).toUpper());
    }
    p.restore();
}

void TimelineWidget::paintEvent(QPaintEvent* e)
{
    m_paintArea = e->region();
    QPainter p(viewport());
    const int W = viewport()->width();
    const int H = viewport()->height();
    p.fillRect(viewport()->rect(), theme::c().timelineBg);
    const auto& tracks = m_project->timeline().tracks;

    p.save();
    p.setClipRect(QRect(kHeaderWidth, kTopH, W - kHeaderWidth, H - kTopH));
    int y = kTopH - m_scrollY;
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const int h = trackHeight(tracks[ti]);
        if (y + h >= kTopH && y <= H) {
            const QRect band(kHeaderWidth, y, W - kHeaderWidth, h);
            p.fillRect(band, ti == m_selectedTrack ? theme::c().trackSelected : (ti % 2 ? theme::c().trackBgAlt : theme::c().trackBg));
            paintGrid(p, band);
            p.setPen(theme::c().deep);
            p.drawLine(band.left(), band.bottom(), band.right(), band.bottom());
            paintTrackClips(p, tracks[ti], y, h);
            paintEnvelopes(p, tracks[ti], y, h);
        }
        y += h;
    }

    if (hasRegion()) {
        const double x0 = timeToX(m_regionStart), x1 = timeToX(m_regionEnd);
        QColor c = theme::c().region;
        c.setAlpha(40);
        p.fillRect(QRectF(x0, kTopH, x1 - x0, H - kTopH), c);
    }
    for (const Marker& m : m_project->timeline().markers) {
        if (m.isRegion())
            continue;
        const double x = timeToX(m.time);
        if (x < kHeaderWidth || x > W)
            continue;
        QColor c = markerColor(m.color);
        c.setAlpha(120);
        p.setPen(QPen(c, 1, Qt::DotLine));
        p.drawLine(QPointF(x, kTopH), QPointF(x, H));
    }
    if (m_dropX) {
        p.setPen(QPen(theme::c().accent, 2));
        p.drawLine(*m_dropX, kTopH, *m_dropX, H);
    }
    if (m_drag == Drag::RubberBand && m_dragMoved) {
        const QRectF band = QRectF(QPointF(timeToX(m_bandTime), m_bandY - m_scrollY), QPointF(m_bandNow)).normalized();
        QColor fill = theme::c().accent;
        fill.setAlpha(40);
        p.fillRect(band, fill);
        p.setPen(QPen(theme::c().accent, 1, Qt::DashLine));
        p.drawRect(band);
    }
    // Where a dragged effect would go.
    if (m_fxDropTrack >= 0 && m_fxDropTrack < int(tracks.size())) {
        const QRectF band(kHeaderWidth, trackTop(m_fxDropTrack), W - kHeaderWidth, trackHeight(tracks[size_t(m_fxDropTrack)]));
        QColor wash = theme::c().good;
        wash.setAlpha(45);
        p.fillRect(band, wash);
        p.setPen(QPen(theme::c().good, 2));
        p.drawRect(band.adjusted(1, 1, -1, -1));
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        p.setPen(theme::c().bright);
        p.drawText(band.adjusted(10, 0, -10, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Track FX: applies to everything on this track"));
    } else if (m_fxDropClip) {
        int ti = -1;
        if (const Clip* c = m_project->timeline().findClip(m_fxDropClip, &ti)) {
            const QRectF r(timeToX(c->start), trackTop(ti) + 1, timeToX(c->end()) - timeToX(c->start), trackHeight(tracks[size_t(ti)]) - 2);
            p.setPen(QPen(theme::c().good, 3));
            p.drawRect(r.adjusted(1, 1, -1, -1));
        }
    }
    const double cx = timeToX(m_cursor);
    p.setPen(QPen(theme::c().cursor, 1, Qt::DashLine));
    p.drawLine(QPointF(cx, kTopH), QPointF(cx, H));
    if (m_playhead) {
        const double px = timeToX(*m_playhead);
        p.setPen(QPen(theme::c().playhead, 1));
        p.drawLine(QPointF(px, kTopH), QPointF(px, H));
    }
    // An empty timeline says how to start.
    if (std::all_of(tracks.begin(), tracks.end(), [](const Track& t) { return t.clips.empty(); })) {
        p.setPen(theme::c().faint);
        p.drawText(QRect(kHeaderWidth, kTopH, W - kHeaderWidth, H - kTopH).adjusted(20, 20, -20, -20), Qt::AlignCenter | Qt::TextWordWrap,
                   tr("Drag video, music or pictures here, or press Ctrl+I to import them.\n"
                      "Ctrl+T adds a title; the Generators tab has backgrounds.\n"
                      "New here? Help > Tutorials, or Shift+F1 for help on any part of the window."));
    }
    p.restore();

    paintRuler(p);
    paintHeaders(p);
    // Where a dragged track header would go.
    if (m_drag == Drag::TrackReorder && m_reorderGap >= 0) {
        const int y = std::max(kTopH, trackTop(m_reorderGap));
        p.fillRect(QRect(0, y - 2, W, 4), theme::c().accent);
    }

    // Timecode display in the corner.
    const QRect corner(0, 0, kHeaderWidth, kTopH);
    p.fillRect(corner, theme::c().corner);
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.6);
    f.setBold(true);
    p.setFont(f);
    p.setPen(theme::c().timecode);
    p.drawText(corner, Qt::AlignCenter, formatTimecode(m_playhead.value_or(m_cursor), m_project->settings().fps));
    p.setPen(theme::c().deep);
    p.drawLine(kHeaderWidth - 1, 0, kHeaderWidth - 1, H);
}

void TimelineWidget::paintTrackClips(QPainter& p, const Track& track, int top, int height)
{
    const int W = viewport()->width();
    const TimeUs t0 = xToTime(kHeaderWidth);
    const TimeUs t1 = xToTime(W);
    const double pxPerUs = m_pxPerSec / kUsPerSecond;
    for (size_t ci = 0; ci < track.clips.size(); ++ci) {
        const Clip& c = track.clips[ci];
        const QColor base = clipColor(track.type, c);
        if (c.end() < t0 || c.start > t1)
            continue;
        // Clamp to the visible area so huge zoom levels don't overflow.
        const double rx0 = timeToX(c.start), rx1 = timeToX(c.end());
        const double vx0 = std::max(rx0, double(kHeaderWidth) - 4), vx1 = std::min(rx1, double(W) + 4);
        const QRectF r(rx0, top + 1, rx1 - rx0, height - 2);
        const QRectF vr(vx0, r.top(), vx1 - vx0, r.height());
        if (!m_paintArea.intersects(vr.toAlignedRect().adjusted(-2, -2, 2, 2)))
            continue; // (not being repainted: its title and picture would be laid out for nothing)
        const QRectF body(vx0, r.top() + kTitleH, vx1 - vx0, r.height() - kTitleH);
        const bool selected = m_selection.count(c.id) > 0;
        auto media = m_project->mediaItem(c.mediaId);
        const bool offline = !c.isGenerated() && (!media || !media->info.ok());

        p.fillRect(vr, base.darker(230));
        p.save();
        p.setClipRect(body.intersected(QRectF(kHeaderWidth, top, W - kHeaderWidth, height)), Qt::IntersectClip);
        if (offline) {
            p.fillRect(body, QColor(0x60, 0x20, 0x20));
            p.setPen(theme::c().clipText);
            p.drawText(body.adjusted(6, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Media offline"));
        } else if (c.isGenerated()) {
            paintGenerated(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
            paintMoods(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        } else if (isVisualTrack(track.type)) {
            paintThumbnails(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        } else {
            paintWaveform(p, c, QRectF(rx0, body.top(), rx1 - rx0, body.height()));
        }

        // Fades (effective, so automatic crossfades show too).
        const ClipEnvelope env = effectiveEnvelope(track, ci);
        QColor shade(0, 0, 0, 110);
        p.setPen(QPen(QColor(0xff, 0xff, 0xff, 200), 1));
        // Fades drawn in their curve's shape (the level line), shaded above it.
        auto fadeLine = [&](double xEdge, double xInner, int curve) {
            QPolygonF line;
            constexpr int kSteps = 24;
            for (int k = 0; k <= kSteps; ++k) {
                const double f = double(k) / kSteps;
                line << QPointF(xEdge + (xInner - xEdge) * f, body.bottom() - body.height() * fadeShape(curve, f));
            }
            QPainterPath path;
            path.moveTo(xEdge, body.top());
            for (const QPointF& pt : line)
                path.lineTo(pt);
            path.closeSubpath();
            p.fillPath(path, shade);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.drawPolyline(line);
            p.setRenderHint(QPainter::Antialiasing, false);
        };
        if (env.fadeIn > 0)
            fadeLine(rx0, rx0 + env.fadeIn * pxPerUs, c.fadeInCurve);
        if (env.fadeOut > 0)
            fadeLine(rx1, rx1 - env.fadeOut * pxPerUs, c.fadeOutCurve);
        // Looping (past the end of its file): a notch where the file starts again.
        if (c.loops() && !c.reverse && !c.hasRamp() && c.speed > 0) {
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0xff, 0xff, 0xff, 230));
            const double every = double(c.loopLength) / c.speed;
            for (double t = double(c.start) + double(c.loopLength - c.sourceIn % c.loopLength) / c.speed; t < double(c.end()); t += every) {
                const double x = timeToX(TimeUs(t));
                p.drawPolygon(QPolygonF{QPointF(x - 4, body.top()), QPointF(x + 4, body.top()), QPointF(x, body.top() + 6)});
                p.fillRect(QRectF(x - 0.5, body.top(), 1, body.height()), QColor(0xff, 0xff, 0xff, 70));
            }
            p.setBrush(Qt::NoBrush);
            p.setRenderHint(QPainter::Antialiasing, false);
        }
        // The transition's name inside its fade / overlap area, with a × to remove
        // it (audio: its fade, which follows the video's transition when grouped).
        {
            QFont tf = font();
            tf.setPointSizeF(tf.pointSizeF() * 0.8);
            p.setFont(tf);
            for (bool in : {true, false}) {
                const QRectF lr = transitionLabelRect(track, ci, top, height, in);
                if (lr.isEmpty())
                    continue;
                const EndTransition e = endTransition(track, ci, in);
                p.fillRect(lr, QColor(0, 0, 0, 160));
                QRectF textRect = lr;
                if (lr.width() >= kTransitionCloseMinW) {
                    const QRectF x = transitionCloseRect(lr, in);
                    p.fillRect(x.adjusted(1, 1, -1, -1), QColor(0x8a, 0x34, 0x34));
                    p.setPen(Qt::white);
                    p.drawText(x, Qt::AlignCenter, QStringLiteral("\u00D7"));
                    textRect = in ? lr.adjusted(0, 0, -15, 0) : lr.adjusted(15, 0, 0, 0);
                }
                p.setPen(e.spec && !e.spec->isPlainFade() ? QColor(0xff, 0xd0, 0x80) : QColor(0xd8, 0xd8, 0xdc));
                p.drawText(textRect, Qt::AlignCenter,
                           p.fontMetrics().elidedText(endTransitionName(e, in), Qt::ElideRight, int(std::max(0.0, textRect.width()))));
            }
        }
        // Event level line.
        if (c.gain < 0.999f) {
            const double gy = body.top() + (1.0 - c.gain) * body.height();
            p.fillRect(QRectF(body.left(), body.top(), body.width(), gy - body.top()), QColor(0, 0, 0, 90));
            p.setPen(QPen(QColor(0x60, 0xb0, 0xff), 1));
            p.drawLine(QPointF(body.left(), gy), QPointF(body.right(), gy));
        }
        if (c.muted)
            p.fillRect(body, QBrush(QColor(0, 0, 0, 140), Qt::BDiagPattern));
        p.restore();

        // Title strip.
        const QRectF title(vx0, r.top(), vx1 - vx0, kTitleH);
        p.fillRect(title, selected ? theme::c().clipSelected.darker(140) : base);
        p.setPen(theme::c().clipText);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.85);
        p.setFont(f);
        double textX = std::max(rx0, double(kHeaderWidth)) + 4;
        if (c.groupId && vx1 - textX > 40) {
            // Grouped (e.g. a video file's picture and sound): a chain link.
            p.save();
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(QColor(0xe8, 0xe8, 0xec), 1.4));
            p.setBrush(Qt::NoBrush);
            const double cy = title.top() + kTitleH / 2.0;
            p.drawRoundedRect(QRectF(textX, cy - 2.5, 7, 5), 2.5, 2.5);
            p.drawRoundedRect(QRectF(textX + 4.5, cy - 2.5, 7, 5), 2.5, 2.5);
            p.restore();
            textX += 15;
        }
        const QRectF textRect(textX, title.top(), std::max(0.0, vx1 - textX - 4), kTitleH);
        QString label = c.isGenerated() ? generatorLabel(c.generator) : c.name;
        if (c.muted)
            label = tr("[muted] ") + label;
        if (c.speed != 1.0 || c.reverse || c.hasRamp())
            label += QStringLiteral("  [%1%2%%3]")
                         .arg(c.reverse ? QStringLiteral("-") : QString())
                         .arg(c.speed * 100, 0, 'f', 0)
                         .arg(c.hasRamp() ? tr(" ramp") : QString());
        const QRectF badge = fxBadgeRect(c, top);
        const QRectF nameRect = badge.isEmpty() ? textRect : textRect.adjusted(0, 0, -50, 0);
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(label, Qt::ElideRight, int(std::max(0.0, nameRect.width()))));
        if (!badge.isEmpty() && isVisualTrack(track.type)) {
            const QRectF pc = badge.translated(-24, 0);
            const bool framed = !isDefaultPanCrop(c.panCrop);
            p.fillRect(pc, framed ? QColor(0x3c, 0x7c, 0xd0) : QColor(0, 0, 0, 70));
            p.setPen(QPen(framed ? Qt::white : QColor(0xc0, 0xc0, 0xc0), 1));
            p.drawRect(pc.adjusted(4, 3, -4, -3));
            p.drawRect(pc.adjusted(7, 5, -7, -5));
        }
        if (!badge.isEmpty()) {
            const bool hasFx = hasActiveEffectsList(c.effects);
            p.fillRect(badge, hasFx ? QColor(0x3c, 0xb0, 0x5a) : QColor(0, 0, 0, 70));
            p.setPen(hasFx ? Qt::white : QColor(0xc0, 0xc0, 0xc0));
            p.drawText(badge, Qt::AlignCenter, QStringLiteral("fx"));
        }

        // Fade handles.
        p.setBrush(Qt::white);
        p.setPen(Qt::NoPen);
        p.drawRect(QRectF(rx0 + c.fadeIn * pxPerUs - 2, r.top(), 4, 4));
        p.drawRect(QRectF(rx1 - c.fadeOut * pxPerUs - 2, r.top(), 4, 4));
        p.setBrush(Qt::NoBrush);

        p.setPen(QPen(selected ? theme::c().clipSelected : base.lighter(140), selected ? 2 : 1));
        p.drawRect(QRectF(rx0 + 0.5, r.top() + 0.5, rx1 - rx0 - 1, r.height() - 1).intersected(vr.adjusted(-1, 0, 1, 0)));
    }
    if (track.locked) // hatched: hands off
        p.fillRect(QRect(kHeaderWidth, top, W - kHeaderWidth, height), QBrush(ink(45), Qt::BDiagPattern));
}

void TimelineWidget::paintThumbnails(QPainter& p, const Clip& c, const QRectF& body)
{
    auto media = m_project->mediaItem(c.mediaId);
    const VideoStreamInfo* vs = media ? media->info.videoStream(c.streamIndex) : nullptr;
    if (!vs || body.height() < 10)
        return;
    const QSize disp = vs->displaySize();
    const double thumbH = body.height();
    const double thumbW = std::max(8.0, thumbH * disp.width() / std::max(1, disp.height()));
    const int W = viewport()->width();
    const double vis0 = std::max(body.left(), double(kHeaderWidth));
    const double vis1 = std::min(body.right(), double(W));
    if (vis1 <= vis0)
        return;

    // Quantise source times so neighbouring repaints reuse cached thumbnails.
    static const TimeUs steps[] = {0, 100000, 250000, 500000, 1000000, 2000000, 5000000, 10000000,
                                   30000000, 60000000, 120000000, 300000000, 600000000};
    const TimeUs slotDur = TimeUs(thumbW * kUsPerSecond / m_pxPerSec);
    TimeUs step = m_project->settings().fps.frameDuration();
    for (TimeUs s : steps)
        if (s > 0 && s <= slotDur)
            step = s;

    const int first = std::max(0, int((vis0 - body.left()) / thumbW));
    const int last = int((vis1 - body.left()) / thumbW);
    for (int i = first; i <= last; ++i) {
        const double sx = body.left() + i * thumbW;
        TimeUs src = c.sourceTimeAt(xToTime(sx));
        src = (src / step) * step;
        const QImage img = m_cache->thumbnail(media, c.streamIndex, src);
        const QRectF target(sx, body.top(), thumbW, thumbH);
        if (img.isNull())
            p.fillRect(target.adjusted(1, 1, -1, -1), QColor(0x20, 0x20, 0x24));
        else
            p.drawImage(target, img);
    }
}

void TimelineWidget::paintWaveform(QPainter& p, const Clip& c, const QRectF& body)
{
    auto media = m_project->mediaItem(c.mediaId);
    if (!media)
        return;
    auto wave = m_cache->waveform(media, c.streamIndex);
    const int W = viewport()->width();
    const int vis0 = int(std::max(body.left(), double(kHeaderWidth)));
    const int vis1 = int(std::min(body.right(), double(W)));
    const double cy = body.center().y();
    const double half = body.height() / 2 - 1;
    p.setPen(QColor(0, 0, 0, 80));
    p.drawLine(QPointF(vis0, cy), QPointF(vis1, cy));
    if (!wave || wave->buckets() == 0)
        return;

    const double bucketsPerUs = double(wave->bucketsPerSecond) / kUsPerSecond;
    const int64_t nb = int64_t(wave->buckets());
    std::vector<QLineF> lines;
    lines.reserve(size_t(std::max(0, vis1 - vis0)));
    for (int x = vis0; x < vis1; ++x) {
        const double sa = double(c.sourceTimeAt(xToTime(x))), sb = double(c.sourceTimeAt(xToTime(x + 1)));
        int64_t b0 = int64_t(std::min(sa, sb) * bucketsPerUs);
        int64_t b1 = std::max(b0 + 1, int64_t(std::max(sa, sb) * bucketsPerUs));
        if (b0 >= nb || b1 <= 0)
            continue;
        b0 = std::max<int64_t>(0, b0);
        b1 = std::min(b1, nb);
        int lo = 0, hi = 0;
        for (int64_t b = b0; b < b1; ++b) {
            lo = std::min<int>(lo, wave->peaks[size_t(b) * 2]);
            hi = std::max<int>(hi, wave->peaks[size_t(b) * 2 + 1]);
        }
        lines.emplace_back(x + 0.5, cy - hi * half / 127.0, x + 0.5, cy - lo * half / 127.0 + 1);
    }
    p.setPen(theme::c().waveform);
    p.drawLines(lines.data(), int(lines.size()));
}

void TimelineWidget::paintRuler(QPainter& p)
{
    const int W = viewport()->width();
    const QRect bar(kHeaderWidth, 0, W - kHeaderWidth, kRegionBarH);
    const QRect ruler(kHeaderWidth, kRulerTop, W - kHeaderWidth, kRulerH);
    p.fillRect(bar, theme::c().markerBar);
    p.fillRect(ruler, theme::c().ruler);
    p.save();
    p.setClipRect(QRect(kHeaderWidth, 0, W - kHeaderWidth, kTopH));

    if (hasRegion()) {
        const double x0 = timeToX(m_regionStart), x1 = timeToX(m_regionEnd);
        p.fillRect(QRectF(x0, 2, x1 - x0, kRegionBarH - 4), theme::c().region);
        p.setBrush(QColor(0xe0, 0xe8, 0xff));
        p.setPen(Qt::NoPen);
        const QPointF a[] = {{x0, 0}, {x0 + 6, 0}, {x0, double(kRegionBarH)}};
        const QPointF b[] = {{x1, 0}, {x1 - 6, 0}, {x1, double(kRegionBarH)}};
        p.drawPolygon(a, 3);
        p.drawPolygon(b, 3);
        p.setBrush(Qt::NoBrush);
    }
    paintMarkers(p);

    // Major tick spacing: the smallest "nice" interval at least ~100 px apart.
    const FrameRate fps = m_project->settings().fps;
    const TimeUs fd = fps.frameDuration();
    const TimeUs candidates[] = {fd, 2 * fd, 5 * fd, 10 * fd, kUsPerSecond, 2 * kUsPerSecond, 5 * kUsPerSecond,
                                 10 * kUsPerSecond, 15 * kUsPerSecond, 30 * kUsPerSecond, 60 * kUsPerSecond,
                                 120 * kUsPerSecond, 300 * kUsPerSecond, 600 * kUsPerSecond, 900 * kUsPerSecond,
                                 1800 * kUsPerSecond, 3600 * kUsPerSecond, 7200 * kUsPerSecond};
    TimeUs major = candidates[std::size(candidates) - 1];
    for (TimeUs c : candidates)
        if (c * m_pxPerSec / kUsPerSecond >= 100) {
            major = c;
            break;
        }
    const TimeUs minor = major >= kUsPerSecond && major % (5 * kUsPerSecond) == 0 ? major / 5
                         : major <= 10 * fd                                      ? std::max<TimeUs>(fd, major / 5)
                                                                                 : major / 2;
    const bool frameTicks = major < kUsPerSecond;
    auto tickTime = [&](int64_t n, TimeUs step) {
        if (frameTicks)
            return fps.timeOf(fps.frameAt(n * step + step / 2));
        return n * step;
    };

    const TimeUs t0 = xToTime(kHeaderWidth), t1 = xToTime(W);
    p.setPen(theme::c().dim);
    for (int64_t n = std::max<int64_t>(0, t0 / minor); n * minor <= t1 + minor; ++n) {
        const double x = std::round(timeToX(tickTime(n, minor))) + 0.5;
        p.drawLine(QPointF(x, kTopH - 5), QPointF(x, kTopH));
    }
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    for (int64_t n = std::max<int64_t>(0, t0 / major); n * major <= t1 + major; ++n) {
        const TimeUs t = tickTime(n, major);
        const double x = std::round(timeToX(t)) + 0.5;
        p.setPen(theme::c().text);
        p.drawLine(QPointF(x, kRulerTop + 4), QPointF(x, kTopH));
        p.setPen(theme::c().text);
        p.drawText(QPointF(x + 4, kRulerTop + 14), formatTimecode(t, fps));
    }

    // Cursor marker on the ruler.
    const double cx = timeToX(m_cursor);
    p.setBrush(theme::c().cursor);
    p.setPen(Qt::NoPen);
    const QPointF tri[] = {{cx - 5, double(kRulerTop)}, {cx + 5, double(kRulerTop)}, {cx, double(kRulerTop + 7)}};
    p.drawPolygon(tri, 3);
    if (m_playhead) {
        const double px = timeToX(*m_playhead);
        p.setBrush(theme::c().playhead);
        const QPointF tri2[] = {{px - 5, double(kRulerTop)}, {px + 5, double(kRulerTop)}, {px, double(kRulerTop + 7)}};
        p.drawPolygon(tri2, 3);
    }
    p.restore();
}

void TimelineWidget::paintHeaders(QPainter& p)
{
    const int H = viewport()->height();
    const auto& tracks = m_project->timeline().tracks;
    if (!m_paintArea.intersects(QRect(0, kTopH, kHeaderWidth, H - kTopH)))
        return; // (the headers are not being repainted)
    p.save();
    p.setClipRect(QRect(0, kTopH, kHeaderWidth, H - kTopH));
    p.fillRect(QRect(0, kTopH, kHeaderWidth, H - kTopH), theme::c().timelineBg);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);

    int videoNo = 0, audioNo = 0, imageNo = 0;
    for (int ti = 0; ti < int(tracks.size()); ++ti) {
        const Track& t = tracks[ti];
        const int number = t.type == TrackType::Image ? ++imageNo : t.type == TrackType::Video ? ++videoNo : ++audioNo;
        const QRect hr = headerRect(ti);
        if (hr.bottom() < kTopH || hr.top() > H)
            continue;
        const bool isVideo = isVisualTrack(t.type);
        const QColor color = trackColor(t);
        p.fillRect(hr, ti == m_selectedTrack ? theme::c().headerSelected : theme::c().headerBg);
        p.fillRect(QRect(hr.left(), hr.top(), 4, hr.height()), color.lighter(130));
        p.setPen(theme::c().deep);
        p.drawLine(hr.left(), hr.bottom(), hr.right(), hr.bottom());

        // Number badge + name.
        const QRect badge(hr.left() + 8, hr.top() + 4, 18, 18);
        p.fillRect(badge, color);
        p.setPen(theme::c().clipText);
        p.setFont(small);
        p.drawText(badge, Qt::AlignCenter, QString::number(number));
        p.setFont(font());
        p.setPen(theme::c().text);
        const int lockW = t.locked ? 14 : 0;
        if (t.locked)
            drawPadlock(p, QRectF(hr.left() + 30, hr.top() + 6, 10, 13), theme::c().warn);
        const QRect nameRect(hr.left() + 30 + lockW, hr.top() + 4, hr.width() - (t.duckUnder ? 118 : 90) - lockW, 18);
        const QString name = t.name.isEmpty() ? (t.type == TrackType::Image ? tr("Image") : isVideo ? tr("Video") : tr("Audio")) : t.name;
        p.drawText(nameRect, Qt::AlignVCenter | Qt::AlignLeft, p.fontMetrics().elidedText(name, Qt::ElideRight, nameRect.width()));

        // Mute / solo.
        auto button = [&](const QRect& r, const QString& label, bool on, const QColor& onColor) {
            p.fillRect(r, on ? onColor : theme::c().raised);
            p.setPen(on ? theme::c().window : theme::c().dim);
            p.setFont(small);
            p.drawText(r, Qt::AlignCenter, label);
        };
        button(fxRect(hr), QStringLiteral("fx"), hasActiveEffectsList(t.effects), theme::c().good);
        button(muteRect(hr), QStringLiteral("M"), t.muted, theme::c().key);
        button(soloRect(hr), QStringLiteral("S"), t.solo, theme::c().warn);

        // Level fader.
        if (hr.height() >= 44) {
            const QRect lr = levelRect(hr);
            const double pos = isVideo ? double(t.level) : posFromVolume(t.level);
            p.fillRect(lr, theme::c().field);
            p.fillRect(QRect(lr.left(), lr.top(), int(lr.width() * pos), lr.height()),
                       color);
            p.setPen(theme::c().text);
            p.setFont(small);
            const QString text = isVideo ? tr("Level: %1%").arg(int(std::lround(t.level * 100)))
                                         : tr("Vol: %1").arg(volumeText(t.level));
            p.drawText(lr.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
        }
        // Pan.
        if (!isVideo && hr.height() >= 62) {
            const QRect pr = panRect(hr);
            p.fillRect(pr, theme::c().field);
            const int cx = pr.left() + pr.width() / 2;
            const int px = pr.left() + int((t.pan + 1.0f) / 2.0f * pr.width());
            p.fillRect(QRect(std::min(cx, px), pr.top(), std::max(2, std::abs(px - cx)), pr.height()), theme::c().region);
            p.setPen(theme::c().text);
            p.setFont(small);
            p.drawText(pr.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Pan: %1").arg(panText(t.pan)));
        }
        // Audio meter (left / right) at the header's right edge.
        if (!isVideo && hr.height() >= 40) {
            const auto it = m_meters.find(t.id);
            const std::pair<float, float> lv = it == m_meters.end() ? std::pair<float, float>{0, 0} : it->second;
            const int top = hr.top() + 26, bottom = hr.bottom() - 3;
            for (int ch = 0; ch < 2; ++ch) {
                const QRect bar(hr.right() - 8 + ch * 3, top, 2, bottom - top);
                p.fillRect(bar, theme::c().field);
                const float peak = ch ? lv.second : lv.first;
                const int fill = int(bar.height() * levels::meterPos(peak));
                if (fill > 0)
                    p.fillRect(QRect(bar.left(), bar.bottom() - fill + 1, bar.width(), fill),
                               peak >= 0.98f ? theme::c().bad : peak >= 0.5f ? theme::c().warn : theme::c().good);
            }
        }
        if (!isVideo && t.duckUnder && m_project->timeline().trackIndex(t.duckUnder) >= 0) {
            // Auto ducking: a badge next to the fx button.
            const QRect badge(hr.right() - 104, hr.top() + 4, 25, 18);
            p.fillRect(badge, theme::c().key);
            p.setPen(theme::c().window);
            p.setFont(small);
            p.drawText(badge, Qt::AlignCenter, tr("duck"));
        }
        // Compositing mode.
        if (isVideo && hr.height() >= 62) {
            const QRect br = blendRect(hr);
            p.fillRect(br, t.blend ? theme::c().checked : theme::c().field);
            p.setPen(t.blend ? theme::c().bright : theme::c().dim);
            p.setFont(small);
            p.drawText(br.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, tr("Mode: %1").arg(blendModeNames().value(t.blend)));
            p.drawText(br.adjusted(4, 0, -4, 0), Qt::AlignVCenter | Qt::AlignRight, QStringLiteral("\u25BE"));
        }
    }
    p.restore();
}
