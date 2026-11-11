#include "ui/ParamDisplay.h"

#include "core/AudioEffects.h"
#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace {

constexpr double kPi = 3.14159265358979323846;

QColor alpha(QColor c, int a)
{
    c.setAlpha(a);
    return c;
}

// A picture with handles: each sits somewhere on it, moves settings when dragged (and when the wheel turns over it).
class HandleDisplay : public ParamDisplay {
public:
    using ParamDisplay::ParamDisplay;

protected:
    struct Handle {
        QPointF at;
        QString label;
        QString tip;
        std::function<void(QPointF)> drag;      // the mouse, in widget pixels
        std::function<void(double)> wheel = {}; // notches
        bool on = true;                         // drawn filled (a band that does something) or hollow
    };
    virtual std::vector<Handle> handles() const { return {}; }
    virtual void paintPlot(QPainter& p, const QRectF& r) const = 0;

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = plot();
        paintFrame(p, r);
        p.save();
        p.setClipRect(r);
        paintPlot(p, r);
        p.restore();
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.7);
        f.setBold(true);
        p.setFont(f);
        const std::vector<Handle> hs = handles();
        for (int i = 0; i < int(hs.size()); ++i) {
            const Handle& h = hs[size_t(i)];
            const QPointF at(std::clamp(h.at.x(), r.left(), r.right()), std::clamp(h.at.y(), r.top(), r.bottom()));
            const bool hot = i == m_drag || (m_drag < 0 && i == m_hover);
            const double rad = hot ? 7 : 6;
            p.setPen(QPen(theme::c().bright, 1.2));
            p.setBrush(h.on ? (hot ? theme::c().key : theme::c().accent) : alpha(theme::c().base, 200));
            p.drawEllipse(at, rad, rad);
            if (!h.label.isEmpty()) {
                p.setPen(h.on ? theme::c().onAccent : theme::c().text);
                p.drawText(QRectF(at.x() - rad, at.y() - rad, rad * 2, rad * 2), Qt::AlignCenter, h.label);
            }
        }
    }
    int handleAt(QPointF pos) const
    {
        const std::vector<Handle> hs = handles();
        int best = -1;
        double bestD = 10;
        for (int i = 0; i < int(hs.size()); ++i) {
            const QRectF r = plot();
            const QPointF at(std::clamp(hs[size_t(i)].at.x(), r.left(), r.right()), std::clamp(hs[size_t(i)].at.y(), r.top(), r.bottom()));
            const double d = QLineF(at, pos).length();
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return best;
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        m_drag = e->button() == Qt::LeftButton ? handleAt(e->position()) : -1;
        if (m_drag < 0)
            ParamDisplay::mousePressEvent(e);
        update();
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const std::vector<Handle> hs = handles();
        if (m_drag >= 0 && m_drag < int(hs.size())) {
            hs[size_t(m_drag)].drag(e->position());
            showTip(e, handles()[size_t(m_drag)].tip);
            return;
        }
        const int hover = handleAt(e->position());
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        setCursor(hover >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
        if (hover >= 0)
            showTip(e, hs[size_t(hover)].tip);
        else
            QToolTip::hideText();
    }
    void mouseReleaseEvent(QMouseEvent*) override
    {
        m_drag = -1;
        update();
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }
    void wheelEvent(QWheelEvent* e) override
    {
        const int i = handleAt(e->position());
        const std::vector<Handle> hs = handles();
        if (i < 0 || !hs[size_t(i)].wheel) {
            ParamDisplay::wheelEvent(e);
            return;
        }
        hs[size_t(i)].wheel(e->angleDelta().y() / 120.0);
        e->accept();
    }

private:
    void showTip(QMouseEvent* e, const QString& tip)
    {
        if (!tip.isEmpty())
            QToolTip::showText(e->globalPosition().toPoint() + QPoint(14, 8), tip, this);
    }
    int m_drag = -1, m_hover = -1;
};

QString hz(double f)
{
    return f >= 1000 ? QStringLiteral("%1 kHz").arg(f / 1000, 0, 'f', f >= 10000 ? 1 : 2) : QStringLiteral("%1 Hz").arg(int(std::lround(f)));
}

// ---- EQ and filter: the response over 20 Hz - 20 kHz ---------------------------------------------------------------

class FrequencyDisplay : public HandleDisplay {
public:
    FrequencyDisplay(const QString& type, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_type(type), m_eq(type == QLatin1String("a_eq"))
    {
        setFixedWidth(230);
        setToolTip(m_eq ? tr("Drag a band: across for its frequency, up and down for its gain. The wheel over a mid band changes its width.")
                        : tr("Drag the point: across for the frequency, up and down for the resonance (the wheel too)."));
        if (type == QLatin1String("a_dehum"))
            setToolTip(tr("The notches that take the hum out: the mains frequency and its harmonics."));
    }

private:
    double lo() const { return m_eq ? -24 : -36; }
    double hi() const { return 24; }
    double x(double f, const QRectF& r) const { return r.left() + std::log(std::max(f, 1.0) / 20.0) / std::log(1000.0) * r.width(); }
    double freq(double px, const QRectF& r) const { return 20.0 * std::pow(1000.0, std::clamp((px - r.left()) / r.width(), 0.0, 1.0)); }
    double y(double db, const QRectF& r) const { return r.top() + (hi() - db) / (hi() - lo()) * r.height(); }
    double db(double py, const QRectF& r) const { return hi() - (py - r.top()) / r.height() * (hi() - lo()); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.7);
        p.setFont(f);
        for (double g : {50, 100, 200, 500, 1000, 2000, 5000, 10000}) {
            p.setPen(QPen(alpha(theme::c().ink, g == 100 || g == 1000 || g == 10000 ? 40 : 18), 1));
            p.drawLine(QPointF(x(g, r), r.top()), QPointF(x(g, r), r.bottom()));
        }
        for (double d = -24; d <= 24; d += 12) {
            p.setPen(QPen(alpha(theme::c().ink, d == 0 ? 50 : 18), 1));
            p.drawLine(QPointF(r.left(), y(d, r)), QPointF(r.right(), y(d, r)));
        }
        p.setPen(theme::c().faint);
        p.drawText(QPointF(x(100, r) + 2, r.bottom() - 2), QStringLiteral("100"));
        p.drawText(QPointF(x(1000, r) + 2, r.bottom() - 2), QStringLiteral("1k"));
        p.drawText(QPointF(x(10000, r) + 2, r.bottom() - 2), QStringLiteral("10k"));
        p.drawText(QPointF(r.left() + 2, y(12, r) - 2), QStringLiteral("+12"));
        p.drawText(QPointF(r.left() + 2, y(-12, r) - 2), QStringLiteral("-12"));
        const Params ps = params();
        QPainterPath curve;
        for (int i = 0; i <= int(r.width()); i += 2) {
            const double px = r.left() + i;
            const QPointF pt(px, y(std::clamp(audioEffectResponseDb(m_type, ps, freq(px, r)), lo() - 6, hi() + 6), r));
            if (i == 0)
                curve.moveTo(pt);
            else
                curve.lineTo(pt);
        }
        QPainterPath fill = curve;
        fill.lineTo(r.right(), y(0, r));
        fill.lineTo(r.left(), y(0, r));
        fill.closeSubpath();
        p.fillPath(fill, alpha(theme::c().accent, 55));
        p.setPen(QPen(theme::c().accent, 2));
        p.drawPath(curve);
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        std::vector<Handle> hs;
        if (m_type == QLatin1String("a_dehum"))
            return hs; // (its notches: set by the mains and the harmonics)
        if (!m_eq) {
            const double f = value(QStringLiteral("frequency")), q = value(QStringLiteral("q"));
            const auto setQ = [this](double v) { setValue(QStringLiteral("q"), v); };
            hs.push_back({QPointF(x(f, r), y(20 * std::log10(q), r)), QString(), tr("%1, resonance %2").arg(hz(f)).arg(q, 0, 'f', 2),
                          [this, r, setQ](QPointF m) {
                              setValue(QStringLiteral("frequency"), freq(m.x(), r));
                              setQ(std::pow(10.0, db(m.y(), r) / 20.0));
                          },
                          [this, q, setQ](double n) { setQ(q * std::pow(1.15, n)); }});
            return hs;
        }
        // The cuts sit on the 0 dB line and move across; the shelves and mid bands move across and up and down.
        auto cut = [&](const char* id, const QString& label, const QString& name, bool on) {
            const QString f = QString::fromLatin1(id);
            hs.push_back({QPointF(x(value(f), r), y(0, r)), label, on ? tr("%1 %2").arg(name, hz(value(f))) : tr("%1: off (drag in)").arg(name),
                          [this, r, f](QPointF m) { setValue(f, freq(m.x(), r)); }, {}, on});
        };
        auto band = [&](const char* fid, const char* gid, const char* qid, const QString& label, const QString& name) {
            const QString f = QString::fromLatin1(fid), g = QString::fromLatin1(gid), q = qid ? QString::fromLatin1(qid) : QString();
            const double gain = value(g);
            QString tip = tr("%1 %2, %3 dB").arg(name, hz(value(f))).arg(gain, 0, 'f', 1);
            if (!q.isEmpty())
                tip += tr(", width %1").arg(value(q), 0, 'f', 2);
            Handle h{QPointF(x(value(f), r), y(gain, r)), label, tip, [this, r, f, g](QPointF m) {
                         setValue(f, freq(m.x(), r));
                         setValue(g, std::abs(db(m.y(), r)) < 0.4 ? 0.0 : db(m.y(), r)); // (snaps to 0 dB: off)
                     }};
            if (!q.isEmpty())
                h.wheel = [this, q](double n) { setValue(q, value(q) * std::pow(1.15, n)); };
            h.on = gain != 0;
            hs.push_back(h);
        };
        cut("lowCut", QStringLiteral("1"), tr("Low cut"), value(QStringLiteral("lowCut")) > 20.5);
        band("lowFreq", "lowGain", nullptr, QStringLiteral("2"), tr("Low shelf"));
        band("mid1Freq", "mid1Gain", "mid1Q", QStringLiteral("3"), tr("Mid 1"));
        band("mid2Freq", "mid2Gain", "mid2Q", QStringLiteral("4"), tr("Mid 2"));
        band("highFreq", "highGain", nullptr, QStringLiteral("5"), tr("High shelf"));
        cut("highCut", QStringLiteral("6"), tr("High cut"), value(QStringLiteral("highCut")) < 19999.5);
        return hs;
    }

    QString m_type;
    bool m_eq;
};

// ---- Compressor, gate, limiter: level in across, level out up -------------------------------------------------------

class DynamicsDisplay : public HandleDisplay {
public:
    DynamicsDisplay(const QString& type, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_type(type)
    {
        setFixedWidth(150);
        setToolTip(tr("Level in (across) against level out (up). Drag the points to change it."));
    }

private:
    double lo() const { return m_type == QLatin1String("a_gate") ? -90 : -60; }
    double x(double d, const QRectF& r) const { return r.left() + (d - lo()) / -lo() * r.width(); }
    double y(double d, const QRectF& r) const { return r.bottom() - (d - lo()) / -lo() * r.height(); }
    double inAt(double px, const QRectF& r) const { return lo() + std::clamp((px - r.left()) / r.width(), 0.0, 1.0) * -lo(); }
    double outAt(double py, const QRectF& r) const { return lo() + std::clamp((r.bottom() - py) / r.height(), 0.0, 1.0) * -lo(); }
    double out(double in) const { return audioEffectTransferDb(m_type, params(), in); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const double step = m_type == QLatin1String("a_gate") ? 15 : 12;
        p.setPen(QPen(alpha(theme::c().ink, 18), 1));
        for (double d = lo() + step; d < 0; d += step) {
            p.drawLine(QPointF(x(d, r), r.top()), QPointF(x(d, r), r.bottom()));
            p.drawLine(QPointF(r.left(), y(d, r)), QPointF(r.right(), y(d, r)));
        }
        p.setPen(QPen(alpha(theme::c().ink, 45), 1, Qt::DashLine));
        p.drawLine(QPointF(x(lo(), r), y(lo(), r)), QPointF(x(0, r), y(0, r))); // (unchanged)
        const QString level = m_type == QLatin1String("a_limiter") ? QStringLiteral("ceiling") : QStringLiteral("threshold");
        const double t = value(level);
        p.setPen(QPen(alpha(theme::c().key, 120), 1, Qt::DashLine));
        if (m_type == QLatin1String("a_limiter"))
            p.drawLine(QPointF(r.left(), y(t, r)), QPointF(r.right(), y(t, r)));
        else
            p.drawLine(QPointF(x(t, r), r.top()), QPointF(x(t, r), r.bottom()));
        QPainterPath curve;
        QPainterPath fill;
        fill.moveTo(r.left(), r.bottom());
        for (int i = 0; i <= int(r.width()); ++i) {
            const double in = inAt(r.left() + i, r);
            const QPointF pt(r.left() + i, y(std::max(lo() - 6, out(in)), r));
            if (i == 0)
                curve.moveTo(pt);
            else
                curve.lineTo(pt);
            fill.lineTo(pt);
        }
        fill.lineTo(r.right(), r.bottom());
        fill.closeSubpath();
        p.fillPath(fill, alpha(theme::c().accent, 45));
        p.setPen(QPen(theme::c().accent, 2));
        p.drawPath(curve);
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        std::vector<Handle> hs;
        if (m_type == QLatin1String("a_limiter")) {
            const double c = value(QStringLiteral("ceiling"));
            hs.push_back({QPointF(x(c, r), y(c, r)), QString(), tr("Ceiling %1 dB").arg(c, 0, 'f', 1),
                          [this, r](QPointF m) { setValue(QStringLiteral("ceiling"), outAt(m.y(), r)); }});
            return hs;
        }
        const double t = value(QStringLiteral("threshold"));
        const double tOut = m_type == QLatin1String("a_gate") ? t : out(t); // (a gate's curve jumps there: the point sits on the top)
        hs.push_back({QPointF(x(t, r), y(tOut, r)), QStringLiteral("T"), tr("Threshold %1 dB").arg(t, 0, 'f', 1),
                      [this, r](QPointF m) { setValue(QStringLiteral("threshold"), inAt(m.x(), r)); }});
        if (m_type == QLatin1String("a_compressor")) {
            // The curve's end (0 dB in) moves up and down: the ratio.
            const double ratio = value(QStringLiteral("ratio")), makeup = value(QStringLiteral("makeup"));
            hs.push_back({QPointF(x(0, r), y(out(0), r)), QStringLiteral("R"), tr("Ratio %1:1").arg(ratio, 0, 'f', 1),
                          [this, r, t, makeup](QPointF m) {
                              const double above = outAt(m.y(), r) - makeup - t;
                              setValue(QStringLiteral("ratio"), above <= 0.01 ? 20.0 : -t / above);
                          },
                          [this, ratio](double n) { setValue(QStringLiteral("ratio"), ratio + n * 0.5); }});
        } else { // gate: below the threshold, the level out goes down by the range
            const double range = value(QStringLiteral("range")), in = (lo() + t) / 2;
            hs.push_back({QPointF(x(in, r), y(in + range, r)), QStringLiteral("R"), tr("Range %1 dB").arg(range, 0, 'f', 0),
                          [this, r, in](QPointF m) { setValue(QStringLiteral("range"), outAt(m.y(), r) - in); }});
        }
        return hs;
    }

    QString m_type;
};

// ---- Delay: the echoes ---------------------------------------------------------------------------------------------------

class EchoDisplay : public HandleDisplay {
public:
    EchoDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(210);
        setToolTip(tr("The sound and its echoes over 2.4 seconds (ping-pong: left above, right below). "
                      "Drag the first echo for the time and the mix, the second for the feedback."));
    }

private:
    static constexpr double kSpan = 2400; // ms
    double x(double ms, const QRectF& r) const { return r.left() + 6 + ms / kSpan * (r.width() - 10); }
    bool pingPong() const { return value(QStringLiteral("pingpong")) >= 0.5; }
    double base(const QRectF& r) const { return pingPong() ? r.center().y() : r.bottom() - 2; }
    double reach(const QRectF& r) const { return pingPong() ? r.height() / 2 - 4 : r.height() - 6; }
    double tapY(int n, double h, const QRectF& r) const { return base(r) + ((pingPong() && n % 2 == 0) ? 1 : -1) * h * reach(r); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        p.setPen(QPen(alpha(theme::c().ink, 18), 1));
        for (double ms = 500; ms < kSpan; ms += 500)
            p.drawLine(QPointF(x(ms, r), r.top()), QPointF(x(ms, r), r.bottom()));
        p.setPen(QPen(alpha(theme::c().ink, 40), 1));
        p.drawLine(QPointF(r.left(), base(r)), QPointF(r.right(), base(r)));
        const double time = value(QStringLiteral("time")), fb = value(QStringLiteral("feedback")), mix = value(QStringLiteral("mix"));
        const double bright = std::clamp(std::log(value(QStringLiteral("damping")) / 500.0) / std::log(40.0), 0.0, 1.0);
        p.setPen(QPen(theme::c().bright, 3, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x(0, r), base(r)), QPointF(x(0, r), base(r) - reach(r) * (1 - mix * 0.5))); // the sound itself
        double h = mix;
        for (int n = 1; n * time < kSpan && h > 0.01; ++n, h *= fb) {
            // Darker echoes lose their highs: each fades a little more.
            const int a = int(255 * std::pow(0.55 + 0.45 * bright, n - 1));
            p.setPen(QPen(alpha(pingPong() && n % 2 == 0 ? theme::c().info : theme::c().accent, std::max(40, a)), 3, Qt::SolidLine, Qt::FlatCap));
            p.drawLine(QPointF(x(n * time, r), base(r)), QPointF(x(n * time, r), tapY(n, h, r)));
        }
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const double time = value(QStringLiteral("time")), fb = value(QStringLiteral("feedback")), mix = value(QStringLiteral("mix"));
        std::vector<Handle> hs;
        hs.push_back({QPointF(x(time, r), tapY(1, mix, r)), QStringLiteral("1"), tr("Delay %1 ms, mix %2").arg(int(std::lround(time))).arg(mix, 0, 'f', 2),
                      [this, r](QPointF m) {
                          setValue(QStringLiteral("time"), (m.x() - r.left() - 6) / (r.width() - 10) * kSpan);
                          setValue(QStringLiteral("mix"), std::abs(m.y() - base(r)) / reach(r));
                      }});
        if (2 * time < kSpan)
            hs.push_back({QPointF(x(2 * time, r), tapY(2, mix * fb, r)), QStringLiteral("2"), tr("Feedback %1").arg(fb, 0, 'f', 2),
                          [this, r, mix](QPointF m) { setValue(QStringLiteral("feedback"), std::abs(m.y() - base(r)) / reach(r) / std::max(0.01, mix)); }});
        return hs;
    }
};

// ---- Reverb: the sound, then (after the pre-delay) the tail dying away ------------------------------------------------------

class ReverbDisplay : public HandleDisplay {
public:
    ReverbDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(210);
        setToolTip(tr("The sound and the reverb's tail over 4 seconds, in dB (down 60 dB at its end): the highs inside, dying "
                      "sooner with more damping. Drag its start for the pre-delay and the wet level, its end for the room size."));
    }

private:
    static constexpr double kSpan = 4.0; // s
    double x(double s, const QRectF& r) const { return r.left() + 6 + s / kSpan * (r.width() - 10); }
    double seconds(double px, const QRectF& r) const { return (px - r.left() - 6) / (r.width() - 10) * kSpan; }
    double decay() const { return 0.3 + 3.2 * value(QStringLiteral("room")); } // (to about -60 dB, as Freeverb's room size goes)
    double y(double level, const QRectF& r) const { return r.bottom() - 2 - level * (r.height() - 8); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        p.setPen(QPen(alpha(theme::c().ink, 18), 1));
        for (double s = 1; s < kSpan; s += 1)
            p.drawLine(QPointF(x(s, r), r.top()), QPointF(x(s, r), r.bottom()));
        const double pre = value(QStringLiteral("predelay")) / 1000.0, wet = value(QStringLiteral("wet")), dry = value(QStringLiteral("dry"));
        const double damping = value(QStringLiteral("damping"));
        auto tail = [&](double length) { // in dB: a straight fall, from the wet level to 60 dB below it at `length`
            QPainterPath path;
            path.moveTo(x(pre, r), y(0, r));
            path.lineTo(x(pre, r), y(wet, r));
            path.lineTo(x(pre + length, r), y(0, r));
            path.closeSubpath();
            return path;
        };
        p.fillPath(tail(decay()), alpha(theme::c().accent, 90));
        p.fillPath(tail(decay() * (1 - 0.8 * damping)), alpha(theme::c().bright, 70)); // (the highs)
        p.setPen(QPen(theme::c().bright, 3, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(QPointF(x(0, r), y(0, r)), QPointF(x(0, r), y(dry, r)));
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const double pre = value(QStringLiteral("predelay")) / 1000.0, wet = value(QStringLiteral("wet"));
        const double end = pre + decay();
        return {{QPointF(x(pre, r), y(wet, r)), QString(), tr("Pre-delay %1 ms, wet %2").arg(int(std::lround(pre * 1000))).arg(wet, 0, 'f', 2),
                 [this, r](QPointF m) {
                     setValue(QStringLiteral("predelay"), seconds(m.x(), r) * 1000);
                     setValue(QStringLiteral("wet"), (r.bottom() - 2 - m.y()) / (r.height() - 8));
                 }},
                {QPointF(x(end, r), y(0, r) - 6), QString(), tr("Room size %1 (about %2 s)").arg(value(QStringLiteral("room")), 0, 'f', 2).arg(decay(), 0, 'f', 1),
                 [this, r, pre](QPointF m) { setValue(QStringLiteral("room"), (seconds(m.x(), r) - pre - 0.3) / 3.2); }}};
    }
};

// ---- Chorus, flanger: the delay sweeping over 2 seconds ---------------------------------------------------------------

class SweepDisplay : public HandleDisplay {
public:
    SweepDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(190);
        setToolTip(tr("The delay over 2 seconds, sweeping. Drag the line for the delay, the top of the sweep for the depth "
                      "(up and down) and the rate (across)."));
    }

private:
    static constexpr double kSpan = 2.0;
    double top() const
    {
        const ParamDef* d = def(QStringLiteral("delay"));
        const ParamDef* k = def(QStringLiteral("depth"));
        return (d ? d->max : 40) + (k ? k->max : 15);
    }
    double x(double s, const QRectF& r) const { return r.left() + s / kSpan * r.width(); }
    double y(double ms, const QRectF& r) const { return r.bottom() - 4 - ms / top() * (r.height() - 8); }
    double ms(double py, const QRectF& r) const { return (r.bottom() - 4 - py) / (r.height() - 8) * top(); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const double rate = value(QStringLiteral("rate")), depth = value(QStringLiteral("depth")), delay = value(QStringLiteral("delay"));
        p.setPen(QPen(alpha(theme::c().ink, 45), 1, Qt::DashLine));
        p.drawLine(QPointF(r.left(), y(delay, r)), QPointF(r.right(), y(delay, r)));
        QPainterPath band, curve;
        for (int i = 0; i <= int(r.width()); ++i) {
            const double t = double(i) / r.width() * kSpan;
            const QPointF pt(r.left() + i, y(std::max(0.0, delay + depth * std::sin(2 * kPi * rate * t)), r));
            if (i == 0)
                curve.moveTo(pt);
            else
                curve.lineTo(pt);
        }
        band = curve;
        band.lineTo(r.right(), y(delay, r));
        band.lineTo(r.left(), y(delay, r));
        band.closeSubpath();
        p.fillPath(band, alpha(theme::c().accent, 45));
        p.setPen(QPen(theme::c().accent, 2));
        p.drawPath(curve);
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const double rate = value(QStringLiteral("rate")), depth = value(QStringLiteral("depth")), delay = value(QStringLiteral("delay"));
        const double peak = std::min(kSpan, 1.0 / (4 * rate)); // (the first top of the sweep)
        return {{QPointF(r.right() - 10, y(delay, r)), QString(), tr("Delay %1 ms").arg(delay, 0, 'f', 1),
                 [this, r](QPointF m) { setValue(QStringLiteral("delay"), ms(m.y(), r)); }},
                {QPointF(x(peak, r), y(delay + depth, r)), QString(), tr("Depth %1 ms, rate %2 Hz").arg(depth, 0, 'f', 1).arg(rate, 0, 'f', 2),
                 [this, r, delay](QPointF m) {
                     setValue(QStringLiteral("depth"), ms(m.y(), r) - delay);
                     setValue(QStringLiteral("rate"), 1.0 / (4 * std::max(0.01, (m.x() - r.left()) / r.width() * kSpan)));
                 }}};
    }
};

// ---- Stereo Tools: where the sound sits between the speakers, and how wide ------------------------------------------------

class StereoDisplay : public HandleDisplay {
public:
    StereoDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(160);
        setToolTip(tr("The sound between the left and right speakers. Drag the point: across for the balance, up and down for the width."));
    }

private:
    QPointF centre(const QRectF& r) const { return QPointF(r.center().x(), r.bottom() - 6); }
    double radius(const QRectF& r) const { return std::min(r.width() / 2 - 8, r.height() - 14); }
    double width() const { return value(QStringLiteral("mono")) >= 0.5 ? 0.0 : value(QStringLiteral("width")); }

    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const QPointF c = centre(r);
        const double rad = radius(r);
        const QRectF circle(c.x() - rad, c.y() - rad, rad * 2, rad * 2);
        p.setPen(QPen(alpha(theme::c().ink, 40), 1));
        p.drawArc(circle, 0, 180 * 16);
        p.drawLine(QPointF(c.x(), c.y()), QPointF(c.x(), c.y() - rad));
        // The sound: a wedge centred on the balance, as wide as the width (1: the speakers, 2: past them).
        const double mid = 90 - value(QStringLiteral("balance")) * 45, half = std::max(1.0, width() * 45);
        QPainterPath wedge;
        wedge.moveTo(c);
        wedge.arcTo(circle, std::clamp(mid - half, 0.0, 180.0), std::clamp(mid + half, 0.0, 180.0) - std::clamp(mid - half, 0.0, 180.0));
        wedge.closeSubpath();
        p.fillPath(wedge, alpha(theme::c().accent, 110));
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 0.75);
        f.setBold(true);
        p.setFont(f);
        const bool swap = value(QStringLiteral("swap")) >= 0.5;
        p.setPen(theme::c().text);
        p.drawText(QPointF(r.left() + 2, r.bottom() - 2), swap ? QStringLiteral("R") : QStringLiteral("L"));
        p.drawText(QPointF(r.right() - 9, r.bottom() - 2), swap ? QStringLiteral("L") : QStringLiteral("R"));
    }

    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const double balance = value(QStringLiteral("balance")), w = value(QStringLiteral("width"));
        const QPointF c = centre(r);
        return {{QPointF(c.x() + balance * radius(r) * 0.7, c.y() - (0.15 + w / 2 * 0.8) * radius(r)), QString(),
                 tr("Balance %1, width %2").arg(balance, 0, 'f', 2).arg(w, 0, 'f', 2), [this, r, c](QPointF m) {
                     setValue(QStringLiteral("balance"), (m.x() - c.x()) / (radius(r) * 0.7));
                     setValue(QStringLiteral("width"), ((c.y() - m.y()) / radius(r) - 0.15) / 0.8 * 2);
                 }}};
    }
};

// ---- Colour effects: the curves (a LUT effect) or the hues before and after (a colour matrix) ------------------------------

class CurvesDisplay : public HandleDisplay {
public:
    CurvesDisplay(const EffectDef& fx, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_fx(fx)
    {
        setFixedWidth(130);
        setToolTip(tr("What the effect does to each brightness: in across, out up (red, green and blue where they differ)."));
    }

private:
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const QRectF sq(r.center().x() - std::min(r.width(), r.height()) / 2, r.center().y() - std::min(r.width(), r.height()) / 2,
                        std::min(r.width(), r.height()), std::min(r.width(), r.height()));
        p.setPen(QPen(alpha(theme::c().ink, 18), 1));
        for (int i = 1; i < 4; ++i) {
            p.drawLine(QPointF(sq.left() + sq.width() * i / 4, sq.top()), QPointF(sq.left() + sq.width() * i / 4, sq.bottom()));
            p.drawLine(QPointF(sq.left(), sq.top() + sq.height() * i / 4), QPointF(sq.right(), sq.top() + sq.height() * i / 4));
        }
        p.setPen(QPen(alpha(theme::c().ink, 45), 1, Qt::DashLine));
        p.drawLine(sq.bottomLeft(), sq.topRight());
        ChannelLuts luts;
        for (auto& l : luts)
            for (int i = 0; i < 256; ++i)
                l[size_t(i)] = uint8_t(i);
        if (m_fx.luts && !m_fx.luts(params(), luts))
            for (auto& l : luts) // (it changes nothing)
                for (int i = 0; i < 256; ++i)
                    l[size_t(i)] = uint8_t(i);
        const bool grey = luts[0] == luts[1] && luts[1] == luts[2];
        const QColor colours[3] = {QColor(235, 70, 70), QColor(80, 210, 90), QColor(80, 140, 255)};
        for (int c = 0; c < (grey ? 1 : 3); ++c) {
            QPainterPath curve;
            for (int i = 0; i < 256; ++i) {
                const QPointF pt(sq.left() + sq.width() * i / 255.0, sq.bottom() - sq.height() * luts[size_t(c)][size_t(i)] / 255.0);
                if (i == 0)
                    curve.moveTo(pt);
                else
                    curve.lineTo(pt);
            }
            p.setPen(QPen(grey ? theme::c().bright : colours[c], 1.8));
            p.drawPath(curve);
        }
    }

    const EffectDef& m_fx;
};

class HueDisplay : public HandleDisplay {
public:
    HueDisplay(const EffectDef& fx, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_fx(fx)
    {
        setFixedWidth(130);
        setToolTip(tr("The colours before (outside) and after the effect (inside), with greys from black to white in the middle."));
    }

private:
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        ColorMatrix m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        if (m_fx.colorMatrix)
            m_fx.colorMatrix(params(), m);
        auto apply = [&](const QColor& c) {
            const float in[3] = {float(c.redF()), float(c.greenF()), float(c.blueF())};
            float out[3];
            for (int i = 0; i < 3; ++i)
                out[i] = std::clamp(m[size_t(i * 4)] * in[0] + m[size_t(i * 4 + 1)] * in[1] + m[size_t(i * 4 + 2)] * in[2] + m[size_t(i * 4 + 3)], 0.0f, 1.0f);
            return QColor::fromRgbF(out[0], out[1], out[2]);
        };
        const double rad = std::min(r.width(), r.height()) / 2 - 2;
        const QPointF c = r.center();
        constexpr int kSegments = 48;
        for (int i = 0; i < kSegments; ++i) {
            const QColor hue = QColor::fromHsvF(double(i) / kSegments, 1.0, 1.0);
            for (const auto& [outer, inner, colour] : {std::tuple{rad, rad * 0.72, hue}, std::tuple{rad * 0.7, rad * 0.42, apply(hue)}}) {
                QPainterPath seg;
                const double a0 = 360.0 * i / kSegments, sweep = 360.0 / kSegments + 0.6;
                seg.arcMoveTo(QRectF(c.x() - outer, c.y() - outer, outer * 2, outer * 2), a0);
                seg.arcTo(QRectF(c.x() - outer, c.y() - outer, outer * 2, outer * 2), a0, sweep);
                seg.arcTo(QRectF(c.x() - inner, c.y() - inner, inner * 2, inner * 2), a0 + sweep, -sweep);
                seg.closeSubpath();
                p.fillPath(seg, colour);
            }
        }
        const double g = rad * 0.4;
        for (int i = 0; i < 8; ++i) { // the greys, before (left) and after (right)
            const QColor grey = QColor::fromRgbF(i / 7.0, i / 7.0, i / 7.0);
            const QRectF cell(c.x() - g * 0.7, c.y() - g * 0.7 + i * g * 1.4 / 8, g * 0.7, g * 1.4 / 8 + 0.5);
            p.fillRect(cell, grey);
            p.fillRect(cell.translated(g * 0.7, 0), apply(grey));
        }
    }

    const EffectDef& m_fx;
};

// ---- A centre (or position) on the picture: centerX / centerY, x / y ---------------------------------------------------

class PointDisplay : public HandleDisplay {
public:
    PointDisplay(std::vector<std::pair<QString, QString>> pairs, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_pairs(std::move(pairs))
    {
        setFixedWidth(176);
        setToolTip(tr("Where on the picture: drag the point."));
    }

private:
    QRectF frame(const QRectF& r) const // a 16:9 picture
    {
        const double w = std::min(r.width() - 8, (r.height() - 8) * 16 / 9);
        return QRectF(r.center().x() - w / 2, r.center().y() - w * 9 / 32, w, w * 9 / 16);
    }
    double share(const QString& id) const
    {
        const ParamDef* d = def(id);
        return d ? (value(id) - d->min) / std::max(1e-9, d->max - d->min) : 0.5;
    }
    void setShare(const QString& id, double s) const
    {
        if (const ParamDef* d = def(id))
            setValue(id, d->min + s * (d->max - d->min));
    }
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const QRectF f = frame(r);
        p.fillRect(f, alpha(theme::c().ink, 14));
        p.setPen(QPen(alpha(theme::c().ink, 60), 1));
        p.drawRect(f);
        p.setPen(QPen(alpha(theme::c().ink, 18), 1));
        for (int i = 1; i < 3; ++i) {
            p.drawLine(QPointF(f.left() + f.width() * i / 3, f.top()), QPointF(f.left() + f.width() * i / 3, f.bottom()));
            p.drawLine(QPointF(f.left(), f.top() + f.height() * i / 3), QPointF(f.right(), f.top() + f.height() * i / 3));
        }
        // Positions that reach past the picture (-2..2) show the picture in the middle of their range.
        if (m_pairs.size() == 4) { // four corners (Corner Pin): where the picture goes, and the frame itself
            const ParamDef* dx = def(m_pairs[0].first);
            const ParamDef* dy = def(m_pairs[0].second);
            if (dx && dy) {
                auto to = [&](double x, double y) {
                    return QPointF(f.left() + (x - dx->min) / (dx->max - dx->min) * f.width(), f.top() + (y - dy->min) / (dy->max - dy->min) * f.height());
                };
                p.setPen(QPen(alpha(theme::c().ink, 90), 1, Qt::DashLine));
                p.drawRect(QRectF(to(0, 0), to(1, 1)));
                QPolygonF quad;
                for (const auto& [xId, yId] : m_pairs)
                    quad << to(value(xId), value(yId));
                p.setPen(QPen(theme::c().accent, 1.5));
                p.setBrush(alpha(theme::c().accent, 50));
                p.drawPolygon(quad);
                p.setBrush(Qt::NoBrush);
            }
            return;
        }
        p.setPen(QPen(alpha(theme::c().key, 110), 1, Qt::DashLine));
        for (const auto& [xId, yId] : m_pairs) {
            const QPointF at(f.left() + share(xId) * f.width(), f.top() + share(yId) * f.height());
            p.drawLine(QPointF(at.x(), f.top()), QPointF(at.x(), f.bottom()));
            p.drawLine(QPointF(f.left(), at.y()), QPointF(f.right(), at.y()));
        }
    }
    std::vector<Handle> handles() const override
    {
        const QRectF f = frame(plot());
        std::vector<Handle> hs;
        for (const auto& [xId, yId] : m_pairs) {
            const ParamDef* dx = def(xId);
            hs.push_back({QPointF(f.left() + share(xId) * f.width(), f.top() + share(yId) * f.height()), QString(),
                          tr("%1 %2, %3").arg(dx ? dx->name : xId).arg(value(xId), 0, 'f', 2).arg(value(yId), 0, 'f', 2),
                          [this, f, xId, yId](QPointF m) {
                              setShare(xId, std::clamp((m.x() - f.left()) / f.width(), 0.0, 1.0));
                              setShare(yId, std::clamp((m.y() - f.top()) / f.height(), 0.0, 1.0));
                          }});
        }
        return hs;
    }

    std::vector<std::pair<QString, QString>> m_pairs;
};

// ---- Blur: how far each pixel spreads (the bell curves across and down, and the footprint) ------------------------------

class BlurDisplay : public HandleDisplay {
public:
    BlurDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(130);
        setToolTip(tr("How far each point spreads: across (the curve at the bottom) and down (at the left). Drag the handles."));
    }

private:
    static constexpr double kMost = 200; // (the radius at the plot's edge; square-root scaled so small radii show)
    double reach(double radius, double half) const { return std::sqrt(std::clamp(radius, 0.0, kMost) / kMost) * half; }
    double fromReach(double d, double half) const { return std::pow(std::clamp(d / half, 0.0, 1.0), 2) * kMost; }
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const QPointF c = r.center();
        const double rx = std::max(1.5, reach(value("horizontal"), r.width() / 2 - 6));
        const double ry = std::max(1.5, reach(value("vertical"), r.height() / 2 - 6));
        QRadialGradient g(c, 1);
        g.setColorAt(0, alpha(theme::c().accent, 200));
        g.setColorAt(1, alpha(theme::c().accent, 0));
        p.save();
        p.translate(c);
        p.scale(rx, ry);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QPointF(0, 0), 1, 1);
        p.restore();
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(alpha(theme::c().ink, 60), 1, Qt::DashLine));
        p.drawEllipse(c, rx, ry);
        auto bell = [&](bool across) { // a Gaussian with sigma = reach / 2.5, along the bottom or the left edge
            QPainterPath path;
            const double sigma = (across ? rx : ry) / 2.5;
            for (int i = 0; i <= 80; ++i) {
                const double u = (i / 80.0 - 0.5) * (across ? r.width() : r.height());
                const double v = std::exp(-u * u / (2 * sigma * sigma)) * 22;
                const QPointF pt = across ? QPointF(c.x() + u, r.bottom() - 3 - v) : QPointF(r.left() + 3 + v, c.y() + u);
                if (i == 0)
                    path.moveTo(pt);
                else
                    path.lineTo(pt);
            }
            p.setPen(QPen(theme::c().bright, 1.4));
            p.drawPath(path);
        };
        bell(true);
        bell(false);
    }
    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const QPointF c = r.center();
        const double hx = r.width() / 2 - 6, hy = r.height() / 2 - 6;
        return {{QPointF(c.x() + reach(value("horizontal"), hx), c.y()), QString(), tr("Across %1 px").arg(value("horizontal"), 0, 'f', 1),
                 [this, c, hx](QPointF m) { setValue("horizontal", fromReach(m.x() - c.x(), hx)); }},
                {QPointF(c.x(), c.y() - reach(value("vertical"), hy)), QString(), tr("Down %1 px").arg(value("vertical"), 0, 'f', 1),
                 [this, c, hy](QPointF m) { setValue("vertical", fromReach(c.y() - m.y(), hy)); }}};
    }
};

// ---- Sharpen: an edge, as it is (dashed) and sharpened (unsharp masking: the overshoot either side) --------------------

class SharpenDisplay : public HandleDisplay {
public:
    SharpenDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(130);
        setToolTip(tr("A dark-to-light edge, as it is (dashed) and sharpened: Amount is how far it overshoots, Radius how wide."));
    }

private:
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const double amount = value("amount"), sigma = std::max(0.3, value("radius"));
        constexpr double kSpan = 24; // pixels either side of the edge
        auto y = [&](double v) { return r.bottom() - 8 - (r.height() - 16) * (0.15 + 0.7 * std::clamp(v, -0.2, 1.2) / 1.0) / 1.15; };
        QPainterPath before, after;
        for (int i = 0; i <= 120; ++i) {
            const double x = (i / 120.0 - 0.5) * 2 * kSpan;
            const double step = x >= 0 ? 1.0 : 0.0;
            const double blurred = 0.5 * std::erfc(-x / (sigma * std::sqrt(2.0)));
            const double out = step + amount * (step - blurred);
            const double px = r.left() + r.width() * i / 120.0;
            if (i == 0) {
                before.moveTo(px, y(step));
                after.moveTo(px, y(out));
            } else {
                before.lineTo(px, y(step));
                after.lineTo(px, y(out));
            }
        }
        p.setPen(QPen(alpha(theme::c().ink, 70), 1, Qt::DashLine));
        p.drawPath(before);
        p.setPen(QPen(theme::c().accent, 1.8));
        p.drawPath(after);
    }
};

// ---- Glow: which brightnesses glow, and how strongly ---------------------------------------------------------------------

class GlowDisplay : public HandleDisplay {
public:
    GlowDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(130);
        setToolTip(tr("Brightness across (dark to bright): what is above the threshold glows, as strongly as the curve is high. Drag "
                      "the handle: across for the threshold, up for the intensity."));
    }

private:
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const double th = value("threshold"), in = value("intensity");
        QLinearGradient ramp(r.topLeft(), r.topRight());
        ramp.setColorAt(0, QColor(0, 0, 0));
        ramp.setColorAt(1, QColor(255, 255, 255));
        p.fillRect(QRectF(r.left(), r.bottom() - 6, r.width(), 6), ramp);
        QPainterPath area;
        area.moveTo(r.left(), r.bottom() - 6);
        for (int i = 0; i <= 100; ++i) {
            const double l = i / 100.0, e0 = th, e1 = std::min(1.0, th + 0.15);
            const double t = std::clamp((l - e0) / std::max(1e-4, e1 - e0), 0.0, 1.0);
            const double k = t * t * (3 - 2 * t) * in / 3.0;
            area.lineTo(r.left() + r.width() * l, r.bottom() - 6 - (r.height() - 12) * k);
        }
        area.lineTo(r.right(), r.bottom() - 6);
        area.closeSubpath();
        QLinearGradient fill(r.bottomLeft(), r.topLeft());
        fill.setColorAt(0, alpha(theme::c().accent, 60));
        fill.setColorAt(1, alpha(QColor(255, 240, 200), 200));
        p.fillPath(area, fill);
        p.setPen(QPen(theme::c().bright, 1.4));
        p.drawPath(area);
        p.setPen(QPen(alpha(theme::c().key, 160), 1, Qt::DashLine));
        const double x = r.left() + r.width() * th;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    }
    std::vector<Handle> handles() const override
    {
        const QRectF r = plot();
        const double x = r.left() + r.width() * value("threshold");
        const double y = r.bottom() - 6 - (r.height() - 12) * value("intensity") / 3.0;
        return {{QPointF(x, y), QString(), tr("Threshold %1, intensity %2").arg(value("threshold"), 0, 'f', 2).arg(value("intensity"), 0, 'f', 2),
                 [this, r](QPointF m) {
                     setValue("threshold", (m.x() - r.left()) / r.width());
                     setValue("intensity", (r.bottom() - 6 - m.y()) / (r.height() - 12) * 3.0);
                 }}};
    }
};

// ---- An effect shown on a test picture (vignette, pixelate...): the effect itself, run on a small card ------------------------

class SwatchDisplay : public HandleDisplay {
public:
    SwatchDisplay(const EffectDef& fx, const std::vector<ParamDef>& defs, Get get, Set set)
        : HandleDisplay(defs, std::move(get), std::move(set)), m_fx(fx)
    {
        setFixedWidth(176);
        setToolTip(tr("The effect on a test picture, with these settings."));
    }

private:
    static QImage card(QSize size)
    {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        img.fill(Qt::black);
        QPainter p(&img);
        QLinearGradient sky(0, 0, 0, size.height());
        sky.setColorAt(0, QColor(70, 150, 220));
        sky.setColorAt(0.6, QColor(250, 210, 150));
        sky.setColorAt(1, QColor(60, 90, 70));
        p.fillRect(img.rect(), sky);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 240, 180));
        p.drawEllipse(QPointF(size.width() * 0.7, size.height() * 0.38), size.height() * 0.14, size.height() * 0.14);
        const QColor bars[6] = {QColor(230, 60, 60), QColor(240, 170, 40), QColor(240, 230, 60), QColor(70, 190, 90), QColor(60, 120, 230), QColor(160, 80, 210)};
        for (int i = 0; i < 6; ++i)
            p.fillRect(QRectF(size.width() * (0.08 + i * 0.06), size.height() * 0.62, size.width() * 0.05, size.height() * 0.3), bars[i]);
        p.end(); // (finished before it is handed on: the effect then works on the picture, not a copy being painted)
        return img;
    }
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const double w = std::min(r.width() - 6, (r.height() - 6) * 16 / 9);
        const QRectF f(r.center().x() - w / 2, r.center().y() - w * 9 / 32, w, w * 9 / 16);
        QImage img = card(f.size().toSize());
        if (m_fx.apply) {
            RenderContext ctx;
            ctx.scale = img.width() / 1920.0;
            ctx.contentRect = QRectF(img.rect());
            std::unique_ptr<EffectState> state;
            m_fx.apply(img, params(), ctx, state);
        }
        p.drawImage(f, img);
        p.setPen(QPen(alpha(theme::c().ink, 60), 1));
        p.setBrush(Qt::NoBrush); // (the frame's brush is still set: it would cover the picture)
        p.drawRect(f);
    }

    const EffectDef& m_fx;
};

// ---- Transform: where the picture goes (its frame, moved, scaled, turned and cropped) -----------------------------------------

class TransformDisplay : public HandleDisplay {
public:
    TransformDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : HandleDisplay(defs, std::move(get), std::move(set))
    {
        setFixedWidth(176);
        setToolTip(tr("The frame (dashed) and where the picture goes in it. Drag the middle to move it, the corner to scale it."));
    }

private:
    QRectF frame(const QRectF& r) const // a 16:9 frame, a third of the plot each way (so positions off the frame show)
    {
        const double w = std::min(r.width() * 0.62, (r.height() * 0.62) * 16 / 9);
        return QRectF(r.center().x() - w / 2, r.center().y() - w * 9 / 32, w, w * 9 / 16);
    }
    QPolygonF placed(const QRectF& f) const
    {
        const double W = f.width(), H = f.height();
        const QRectF crop(value("cropLeft") * W, value("cropTop") * H, W * (1 - value("cropLeft") - value("cropRight")),
                          H * (1 - value("cropTop") - value("cropBottom")));
        QTransform m;
        m.translate(f.left() + W / 2 + value("x") * W, f.top() + H / 2 + value("y") * H);
        m.rotate(value("rotation"));
        m.scale(value("scale"), value("scale"));
        m.translate(-W / 2, -H / 2);
        return m.map(QPolygonF(crop));
    }
    void paintPlot(QPainter& p, const QRectF& r) const override
    {
        const QRectF f = frame(r);
        p.fillRect(f, alpha(theme::c().ink, 14));
        p.setPen(QPen(alpha(theme::c().ink, 90), 1, Qt::DashLine));
        p.drawRect(f);
        const QPolygonF q = placed(f);
        p.setPen(QPen(theme::c().accent, 1.6));
        p.setBrush(alpha(theme::c().accent, int(40 + 60 * value("opacity"))));
        p.drawPolygon(q);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(theme::c().bright, 1.2));
        p.drawLine(q[0], q[1]); // (its top edge brighter: which way up it is)
    }
    std::vector<Handle> handles() const override
    {
        const QRectF f = frame(plot());
        const QPolygonF q = placed(f);
        const QPointF mid = (q[0] + q[2]) / 2;
        const double W = f.width(), H = f.height();
        return {{mid, QString(), tr("Position %1, %2").arg(value("x"), 0, 'f', 3).arg(value("y"), 0, 'f', 3),
                 [this, f, W, H](QPointF m) {
                     setValue("x", (m.x() - f.left() - W / 2) / W);
                     setValue("y", (m.y() - f.top() - H / 2) / H);
                 }},
                {q[2], QString(), tr("Scale %1").arg(value("scale"), 0, 'f', 3),
                 [this, mid, W, H](QPointF m) { // (the corner's distance from the middle, against the full frame's half diagonal)
                     setValue("scale", QLineF(mid, m).length() / (std::hypot(W, H) / 2));
                 }}};
    }
};

} // namespace

ParamDisplay::ParamDisplay(const std::vector<ParamDef>& defs, Get get, Set set) : m_defs(defs), m_get(std::move(get)), m_set(std::move(set))
{
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setMinimumHeight(90);
    setMouseTracking(true);
    setObjectName(QStringLiteral("paramDisplay"));
}

const ParamDef* ParamDisplay::def(const QString& id) const
{
    for (const ParamDef& d : m_defs)
        if (d.id == id)
            return &d;
    return nullptr;
}

void ParamDisplay::setValue(const QString& id, double v) const
{
    const ParamDef* d = def(id);
    if (!d || !std::isfinite(v))
        return;
    const double step = std::pow(10.0, -d->decimals);
    v = std::clamp(std::round(v / step) * step, d->min, d->max);
    if (v != m_get(id))
        m_set(id, v);
    const_cast<ParamDisplay*>(this)->update();
}

Params ParamDisplay::params() const
{
    Params p;
    for (const ParamDef& d : m_defs)
        if (d.type != ParamType::Text && d.type != ParamType::Font)
            p.numbers[d.id] = m_get(d.id);
    return p;
}

void ParamDisplay::paintFrame(QPainter& p, const QRectF& r) const
{
    p.setPen(QPen(theme::c().deep, 1));
    p.setBrush(theme::c().base);
    p.drawRoundedRect(r, 3, 3);
}

ParamDisplay* ParamDisplay::create(const QString& type, const std::vector<ParamDef>& defs, Get get, Set set)
{
    if (type == QLatin1String("a_eq") || type == QLatin1String("a_filter") || type == QLatin1String("a_dehum"))
        return new FrequencyDisplay(type, defs, std::move(get), std::move(set));
    if (type == QLatin1String("a_compressor") || type == QLatin1String("a_gate") || type == QLatin1String("a_limiter"))
        return new DynamicsDisplay(type, defs, std::move(get), std::move(set));
    if (type == QLatin1String("a_delay"))
        return new EchoDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("a_reverb"))
        return new ReverbDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("a_chorus") || type == QLatin1String("a_flanger"))
        return new SweepDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("a_stereo"))
        return new StereoDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("gaussian_blur"))
        return new BlurDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("sharpen"))
        return new SharpenDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("glow"))
        return new GlowDisplay(defs, std::move(get), std::move(set));
    if (type == QLatin1String("transform"))
        return new TransformDisplay(defs, std::move(get), std::move(set));
    const EffectDef* fx = findEffect(type);
    if (!fx)
        return nullptr;
    if ((type == QLatin1String("vignette") || type == QLatin1String("pixelate")) && fx->apply)
        return new SwatchDisplay(*fx, defs, std::move(get), std::move(set));
    if (fx->luts)
        return new CurvesDisplay(*fx, defs, std::move(get), std::move(set));
    if (fx->colorMatrix)
        return new HueDisplay(*fx, defs, std::move(get), std::move(set));
    // A centre or a position on the picture: a setting ending in X with one ending in Y beside it (centerX, centerY; x, y).
    std::vector<std::pair<QString, QString>> pairs;
    for (const ParamDef& d : defs) {
        if (d.hidden || d.type != ParamType::Number || !(d.id.endsWith(QLatin1Char('X')) || d.id == QLatin1String("x")))
            continue;
        const QString y = d.id.left(d.id.size() - 1) + (d.id == QLatin1String("x") ? QStringLiteral("y") : QStringLiteral("Y"));
        if (std::any_of(defs.begin(), defs.end(), [&](const ParamDef& e) { return e.id == y && !e.hidden && e.type == ParamType::Number; }))
            pairs.emplace_back(d.id, y);
    }
    if (!pairs.empty())
        return new PointDisplay(std::move(pairs), defs, std::move(get), std::move(set));
    return nullptr;
}
