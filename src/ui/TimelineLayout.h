#pragma once
// The timeline's fixed geometry, shared with its scripted GUI test (tests/gui/TimelineWidgetTest.cpp).

#include <QRectF>

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
} // namespace timeline_layout
