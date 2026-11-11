#include "ui/MakingOf.h"

#include "core/Compositor.h"
#include "core/LiveRecorder.h"
#include "core/Project.h"
#include "ui/TimelineLayout.h"

#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <thread>

namespace makingof {

namespace {

constexpr int kW = 1280, kH = 720, kFps = 30;
const QRect kPictureArea(0, 14, kW, 412), kCaptionArea(40, 434, kW - 80, 40), kTimelineArea(40, 484, kW - 80, 220);

struct Placed {
    const Clip* clip;
    int track;
    TrackType type;
};

std::map<int, Placed> placed(const Timeline& tl)
{
    std::map<int, Placed> out;
    for (size_t t = 0; t < tl.tracks.size(); ++t)
        for (const Clip& c : tl.tracks[t].clips)
            out[c.id] = {&c, int(t), tl.tracks[t].type};
    return out;
}

double ease(double t) { return 1 - std::pow(1 - std::clamp(t, 0.0, 1.0), 3); }

// The events a step changed (added, removed or altered) and the middle of where they are (else `keep`).
TimeUs changedSpot(const Timeline& from, const Timeline& to, TimeUs keep, std::set<int>& changed)
{
    const auto a = placed(from), b = placed(to);
    TimeUs lo = std::numeric_limits<TimeUs>::max(), hi = std::numeric_limits<TimeUs>::min();
    auto note = [&](const Clip& c) {
        changed.insert(c.id);
        lo = std::min(lo, c.start);
        hi = std::max(hi, c.end());
    };
    for (const auto& [id, p] : b)
        if (auto it = a.find(id); it == a.end() || !(*it->second.clip == *p.clip) || it->second.track != p.track)
            note(*p.clip);
    for (const auto& [id, p] : a)
        if (!b.count(id))
            note(*p.clip);
    if (lo > hi)
        return keep;
    return std::clamp((lo + hi) / 2, TimeUs(0), std::max(TimeUs(0), to.duration() - 1));
}

// The timeline, part way (`t`, 0..1) from one state to the next: events slide to their new places, new ones pop in,
// removed ones fade; each event's effects are dots along its top.
void drawTimeline(QPainter& p, const Timeline& from, const Timeline& to, double t, const std::set<int>& changed, double glow, TimeUs spot)
{
    const QRect area = kTimelineArea;
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x15161c));
    p.drawRoundedRect(area, 8, 8);
    p.save();
    p.setClipRect(area);
    const double span = double(std::max({TimeUs(kUsPerSecond), from.duration(), to.duration()})) * 1.04;
    const int rows = std::max<int>(1, int(std::max(from.tracks.size(), to.tracks.size())));
    const double rowH = std::min(34.0, (area.height() - 16.0) / rows);
    const double left = area.left() + 10, width = area.width() - 20;
    auto xAt = [&](double time) { return left + time / span * width; };
    for (int r = 0; r < rows; ++r) {
        p.setBrush(QColor(r % 2 ? 0x1b1c24 : 0x1e2029));
        p.drawRect(QRectF(left, area.top() + 8 + r * rowH, width, rowH - 2));
    }
    const auto a = placed(from), b = placed(to);
    std::set<int> ids;
    for (const auto& [id, _] : a)
        ids.insert(id);
    for (const auto& [id, _] : b)
        ids.insert(id);
    for (int id : ids) {
        const auto ia = a.find(id), ib = b.find(id);
        const Placed& now = ib != b.end() ? ib->second : ia->second;
        double start = double(now.clip->start), length = double(now.clip->length), row = now.track, alpha = 1, scale = 1;
        int oldEffects = int(now.clip->effects.size());
        if (ia != a.end() && ib != b.end()) {
            const Clip &ca = *ia->second.clip, &cb = *ib->second.clip;
            start = ca.start + (double(cb.start) - double(ca.start)) * t;
            length = ca.length + (double(cb.length) - double(ca.length)) * t;
            row = ia->second.track + (ib->second.track - ia->second.track) * t;
            oldEffects = int(ca.effects.size());
        } else if (ib != b.end()) { // new
            alpha = t;
            scale = 0.4 + 0.6 * t;
            oldEffects = 0;
        } else { // removed
            alpha = 1 - t;
        }
        const double h = (rowH - 6) * scale;
        const QRectF r(xAt(start), area.top() + 8 + row * rowH + 2 + (rowH - 6 - h) / 2, std::max(2.0, xAt(start + length) - xAt(start) - 1), h);
        p.setOpacity(alpha);
        p.setBrush(timeline_layout::clipColor(now.type, *now.clip));
        p.setPen(changed.count(id) && glow > 0 ? QPen(QColor(255, 255, 255, int(255 * glow)), 2) : Qt::NoPen);
        p.drawRoundedRect(r, 3, 3);
        const int dots = std::min<int>(12, int(now.clip->effects.size()));
        p.setPen(Qt::NoPen);
        for (int d = 0; d < dots && 6 + d * 7 < r.width() - 4; ++d) {
            p.setOpacity(alpha * (d >= oldEffects ? t : 1.0));
            p.setBrush(QColor(0xfff2a0));
            const double s = d >= oldEffects ? 2.5 + 2.5 * (1 - t) : 2.5;
            p.drawEllipse(QPointF(r.left() + 6 + d * 7, r.top() + 5), s, s);
        }
    }
    p.setOpacity(1);
    p.setPen(QPen(QColor(0xef9f27), 2));
    p.drawLine(QPointF(xAt(double(spot)), area.top() + 4), QPointF(xAt(double(spot)), area.bottom() - 4));
    p.restore();
}

} // namespace

bool render(const Project& project, const QString& path, const std::function<bool(double)>& progress, QString* error)
{
    const int steps = project.historySteps();
    if (steps <= 0) {
        if (error)
            *error = QStringLiteral("There is nothing to replay yet: the history starts when the project is opened.");
        return false;
    }
    const int perStep = std::clamp(int(std::lround(kFps * 50.0 / steps)), 4, 15), motion = std::min(perStep, 8);
    const int64_t totalFrames = kFps + int64_t(steps) * perStep + kFps * 5 / 2;
    LiveRecorder rec;
    if (!rec.start(path, QSize(kW, kH), kFps, error))
        return false;
    ProjectSnapshot snap = *project.snapshot();
    const ProjectSettings& ps = snap.settings;
    const QSize picSize = QSize(std::max(16, ps.width), std::max(16, ps.height)).scaled(kPictureArea.size(), Qt::KeepAspectRatio);
    Compositor compositor(false);
    auto picture = [&](const Timeline& tl, TimeUs at) {
        snap.timeline = tl;
        return compositor.render(snap, at, picSize);
    };
    const QString title = project.filePath().isEmpty() ? QStringLiteral("Untitled") : QFileInfo(project.filePath()).completeBaseName();
    int64_t frameNo = 0;
    auto emitFrame = [&](const QImage& before, const QImage& after, double t, const Timeline& from, const Timeline& to,
                         const std::set<int>& changed, double glow, TimeUs spot, const QString& caption) {
        QImage frame(kW, kH, QImage::Format_RGB32);
        frame.fill(QColor(0x0d0e12));
        QPainter p(&frame);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRect at(kPictureArea.center().x() - picSize.width() / 2 + 1, kPictureArea.top(), picSize.width(), picSize.height());
        p.drawImage(at, before);
        p.setOpacity(t);
        p.drawImage(at, after);
        p.setOpacity(1);
        QFont f = p.font();
        f.setPixelSize(24);
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(0xf2f2f2));
        p.drawText(kCaptionArea, Qt::AlignLeft | Qt::AlignVCenter, caption);
        f.setPixelSize(18);
        f.setBold(false);
        p.setFont(f);
        p.setPen(QColor(0x8a8c99));
        p.drawText(kCaptionArea, Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("The making of %1").arg(title));
        drawTimeline(p, from, to, t, changed, glow, spot);
        p.end();
        // (the recorder keeps a few frames queued: wait for room)
        while (!rec.addFrame(frame, rec.startTime() + double(frameNo) / kFps))
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ++frameNo;
        return !progress || progress(double(frameNo) / double(totalFrames));
    };

    Timeline prev = project.historyState(0);
    TimeUs spot = 0;
    QImage prevPic = picture(prev, spot);
    bool going = true;
    for (int f = 0; f < kFps && going; ++f)
        going = emitFrame(prevPic, prevPic, 1, prev, prev, {}, 0, spot, QStringLiteral("Where it started"));
    for (int i = 1; i <= steps && going; ++i) {
        QString name;
        const Timeline cur = project.historyState(i, &name);
        std::set<int> changed;
        spot = changedSpot(prev, cur, spot, changed);
        const QImage curPic = picture(cur, spot);
        const QString caption = QStringLiteral("%1 / %2   %3").arg(i).arg(steps).arg(name);
        for (int f = 0; f < perStep && going; ++f)
            going = emitFrame(prevPic, curPic, ease(double(f + 1) / motion), prev, cur, changed, 1.0 - double(f) / perStep, spot, caption);
        prev = cur;
        prevPic = curPic;
    }
    for (int f = 0; f < kFps * 5 / 2 && going; ++f)
        going = emitFrame(prevPic, prevPic, 1, prev, prev, {}, 0, spot, QStringLiteral("The finished edit"));
    QString finishError;
    const bool finished = rec.finish(&finishError);
    if (!going) {
        QFile::remove(path);
        return false;
    }
    if (!finished && error)
        *error = finishError;
    return finished;
}

} // namespace makingof
