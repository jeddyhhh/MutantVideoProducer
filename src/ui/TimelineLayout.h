#pragma once
// The timeline's geometry and drawing helpers, shared by TimelineWidget*.cpp and its scripted GUI test.

#include "core/Project.h"
#include "core/Effects.h"
#include "ui/AudioLevels.h"
#include "ui/Theme.h"

#include <QCoreApplication>
#include <QCursor>
#include <QPainter>
#include <QPixmap>
#include <QRectF>

#include <algorithm>
#include <cmath>

namespace timeline_layout {
constexpr int kHeaderWidth = 210;
constexpr int kRegionBarH = 14;
constexpr int kMarkerBarH = 17;
constexpr int kRulerTop = kRegionBarH + kMarkerBarH;
constexpr int kRulerH = 26;
constexpr int kTopH = kRulerTop + kRulerH;

// A transition label's × button, on the side towards the middle of the event, away from its trim edge.
inline QRectF transitionCloseRect(const QRectF& label, bool atStart)
{
    return atStart ? QRectF(label.right() - 14, label.top(), 14, label.height()) : QRectF(label.left(), label.top(), 14, label.height());
}

// A fine line or wash in the theme's contrast colour (white on dark themes, black on light).
inline QColor ink(int alpha)
{
    QColor c = theme::c().ink;
    c.setAlpha(alpha);
    return c;
}
// As in Ableton Live, the wheel zooms only over the time ruler; elsewhere it scrolls the tracks.
inline bool overRuler(QPointF p) { return p.x() >= kHeaderWidth && p.y() >= kRulerTop && p.y() < kTopH; }
inline QCursor magnifierCursor()
{
    // (Not a static: a pixmap must not outlive the application.)
    QPixmap pm(24, 24);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    for (const auto& [colour, width] : {std::pair{QColor(Qt::white), 4.0}, std::pair{QColor(Qt::black), 2.0}}) {
        p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap));
        p.drawEllipse(QPointF(10, 10), 6.5, 6.5);
        p.drawLine(QPointF(15, 15), QPointF(21, 21));
    }
    p.end();
    return QCursor(pm, 10, 10);
}
constexpr int kEdgeGrab = 6;
constexpr int kFadeGrab = 8;
constexpr int kTitleH = 15;
constexpr int kSnapPx = 8;
constexpr int kVideoTrackH = 84;
constexpr int kAudioTrackH = 66;
constexpr TimeUs kScrollUnit = 1000; // horizontal scrollbar unit: 1 ms

inline QColor trackColor(TrackType type)
{
    return type == TrackType::Image ? theme::c().imageClip
         : type == TrackType::Audio ? theme::c().audioClip : theme::c().videoClip;
}

// Captions / text and graphics have their own colours, so their events and tracks stand out.
enum class ClipKind { Plain, Text, Graphics };
inline ClipKind clipKind(const Clip& c)
{
    if (!c.isGenerated())
        return ClipKind::Plain;
    const QString& g = c.generator.type;
    if (g == QLatin1String("text") || g == QLatin1String("kinetic_text"))
        return ClipKind::Text;
    if (g == QLatin1String("graphics_overlay") || g == QLatin1String("animation") || g == QLatin1String("audio_waveform"))
        return ClipKind::Graphics;
    return ClipKind::Plain;
}
inline QColor kindColor(ClipKind k, TrackType type)
{
    return k == ClipKind::Text ? theme::c().captionClip : k == ClipKind::Graphics ? theme::c().graphicsClip : trackColor(type);
}
inline QColor clipColor(TrackType type, const Clip& c) { return kindColor(clipKind(c), type); }
// A track holding only text (a captions track) or only graphics takes that colour in its header.
inline QColor trackColor(const Track& t)
{
    if (t.clips.empty())
        return trackColor(t.type);
    const ClipKind k = clipKind(t.clips.front());
    for (const Clip& c : t.clips)
        if (clipKind(c) != k)
            return trackColor(t.type);
    return kindColor(k, t.type);
}

using levels::panText;
using levels::posFromVolume;
using levels::volumeFromPos;
using levels::volumeText;

// Velocity envelope axis: -100% at the bottom, 0% a quarter up, 100% in the
// middle, 1000% at the top (logarithmic above 100%).
inline double posFromVelocity(double v)
{
    if (v <= 0)
        return 0.25 * (std::max(v, kMinVelocity) + 1.0);
    if (v <= 1)
        return 0.25 + 0.25 * v;
    return 0.5 + 0.5 * std::log10(std::min(v, kMaxVelocity));
}
inline double velocityFromPos(double pos)
{
    if (std::abs(pos - 0.5) < 0.025)
        return 1.0; // detents at 100% and 0%
    if (std::abs(pos - 0.25) < 0.02)
        return 0.0;
    if (pos <= 0.25)
        return std::max(kMinVelocity, pos / 0.25 - 1.0);
    if (pos <= 0.5)
        return (pos - 0.25) / 0.25;
    return std::min(kMaxVelocity, std::pow(10.0, (pos - 0.5) / 0.5));
}

inline bool hasActiveEffectsList(const EffectChain& chain)
{
    return std::any_of(chain.begin(), chain.end(), [](const EffectInstance& e) { return e.enabled; });
}

inline QRect muteRect(const QRect& h) { return QRect(h.right() - 52, h.top() + 4, 22, 18); }
inline QRect soloRect(const QRect& h) { return QRect(h.right() - 27, h.top() + 4, 22, 18); }
inline QRect levelRect(const QRect& h) { return QRect(h.left() + 30, h.top() + 28, h.width() - 40, 14); }
inline QRect panRect(const QRect& h) { return QRect(h.left() + 30, h.top() + 46, h.width() - 40, 12); }
inline QRect fxRect(const QRect& h) { return QRect(h.right() - 77, h.top() + 4, 22, 18); }
inline QRect blendRect(const QRect& h) { return panRect(h); } // video tracks: compositing mode in the pan slot

// The transition at one end of event i of a track: its settings (the next
// event's start for an overlap at the end) and whether it is a crossfade.
struct EndTransition {
    const TransitionSpec* spec = nullptr;
    bool overlap = false;
    int holder = 0; // the event that holds it
    bool holderIn = true;
};
inline EndTransition endTransition(const Track& t, size_t i, bool atStart)
{
    const Clip& c = t.clips[i];
    if (atStart) {
        const bool overlap = i > 0 && t.clips[i - 1].end() > c.start;
        if (!overlap && c.fadeIn <= 0)
            return {};
        return {&c.transitionIn, overlap, c.id, true};
    }
    if (i + 1 < t.clips.size() && t.clips[i + 1].start < c.end())
        return {&t.clips[i + 1].transitionIn, true, t.clips[i + 1].id, true};
    if (c.fadeOut <= 0)
        return {};
    return {&c.transitionOut, false, c.id, false};
}
inline QString endTransitionName(const EndTransition& e, bool atStart)
{
    if (!e.spec)
        return {};
    if (!e.spec->isPlainFade()) {
        const TransitionDef* def = findTransition(e.spec->type);
        return def ? def->name : e.spec->type;
    }
    return QCoreApplication::translate("TimelineWidget", e.overlap ? "Crossfade" : atStart ? "Fade In" : "Fade Out");
}
constexpr double kTransitionCloseMinW = 46; // narrower labels have no ×

inline QColor markerColor(int i)
{
    static const QColor colors[kMarkerColors] = {{0xff, 0x9f, 0x3c}, {0xe8, 0xd4, 0x4d}, {0x5c, 0xcf, 0x6a}, {0x4d, 0xc9, 0xe8},
                                                 {0x5a, 0x8c, 0xff}, {0xb0, 0x7c, 0xff}, {0xff, 0x6f, 0xa8}, {0xff, 0x5a, 0x5a}};
    return colors[std::clamp(i, 0, kMarkerColors - 1)];
}
inline QString markerColorName(int i)
{
    static const char* names[kMarkerColors] = {"Orange", "Yellow", "Green", "Cyan", "Blue", "Purple", "Pink", "Red"};
    return QCoreApplication::translate("TimelineWidget", names[std::clamp(i, 0, kMarkerColors - 1)]);
}
// A small padlock (a locked track's header).
inline void drawPadlock(QPainter& p, const QRectF& r, const QColor& color)
{
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(color, 1.6));
    p.setBrush(Qt::NoBrush);
    const double w = r.width();
    p.drawArc(QRectF(r.left() + w * 0.18, r.top(), w * 0.64, r.height() * 0.7), 0, 180 * 16);
    p.drawLine(QPointF(r.left() + w * 0.18, r.top() + r.height() * 0.35), QPointF(r.left() + w * 0.18, r.top() + r.height() * 0.45));
    p.drawLine(QPointF(r.right() - w * 0.18, r.top() + r.height() * 0.35), QPointF(r.right() - w * 0.18, r.top() + r.height() * 0.45));
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawRoundedRect(QRectF(r.left(), r.top() + r.height() * 0.45, w, r.height() * 0.55), 1.5, 1.5);
    p.restore();
}
} // namespace timeline_layout
