#include "ui/BeatTuner.h"

#include "ui/Theme.h"
#include "core/EffectHelpers.h"
#include "ui/Knob.h"

#include <QComboBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

// ---------------------------------------------------------------------------
// BeatGraph: the onset curve scrolling right to left.
// ---------------------------------------------------------------------------

class BeatGraph : public QWidget {
public:
    BeatGraph(QWidget* parent = nullptr) : QWidget(parent)
    {
        setMinimumSize(520, 150);
        setAttribute(Qt::WA_OpaquePaintEvent); // paints every pixel itself
        setToolTip(tr("What the detector hears (newest on the right): green = how sharply the sound rises, "
                      "orange = the threshold a hit must cross, white ticks = hits, green bars = beats fired."));
    }
    void setTrace(BeatTracker::Trace t)
    {
        m_trace = std::move(t);
        update();
    }

    double busyMs = 0;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QElapsedTimer timer;
        timer.start();
        struct Count {
            QElapsedTimer& t;
            double& ms;
            ~Count() { ms += double(t.nsecsElapsed()) / 1e6; }
        } count{timer, busyMs};
        QPainter p(this);
        p.fillRect(rect(), theme::c().field);
        const auto& t = m_trace;
        const int n = int(t.strength.size());
        if (n < 2) {
            p.setPen(theme::c().faint);
            p.drawText(rect(), Qt::AlignCenter, tr("No audio yet: pick an Audio input and start capturing."));
            return;
        }
        const double w = width(), h = height() - 14;
        auto y = [&](float v) { return 4 + (h - 4) * (1.0 - std::clamp(double(v), 0.0, 1.0)); };
        // Seconds grid.
        p.setPen(theme::c().line);
        const double secs = n * t.frameSeconds;
        for (int s = 1; s < secs; ++s) {
            const double gx = w * (1.0 - s / secs);
            p.drawLine(QPointF(gx, 0), QPointF(gx, h));
        }
        // One point per pixel column (the loudest frame in it, so no hit is
        // lost), drawn without antialiasing: cheap enough to scroll smoothly
        // without taking time from the Live picture.
        const int cols = std::max(2, width());
        QPolygonF curve(cols), thr(cols);
        QVector<QLineF> hits;
        QVector<QRectF> beats;
        for (int c = 0; c < cols; ++c) {
            const int a = int(int64_t(c) * n / cols), b = std::max(a + 1, int(int64_t(c + 1) * n / cols));
            float top = 0, level = 0;
            bool hit = false, beat = false;
            for (int i = a; i < b && i < n; ++i) {
                top = std::max(top, t.strength[size_t(i)]);
                level = std::max(level, t.threshold[size_t(i)]);
                hit = hit || t.hit[size_t(i)];
                beat = beat || t.beat[size_t(i)];
            }
            const double cx = w * c / double(cols - 1);
            curve[c] = QPointF(cx, y(top));
            thr[c] = QPointF(cx, y(level));
            if (hit)
                hits.push_back(QLineF(cx, 2, cx, 12));
            if (beat)
                beats.push_back(QRectF(cx - 2, h + 2, 4, 10));
        }
        p.setPen(QPen(theme::c().key, 1));
        p.drawPolyline(thr);
        p.setPen(QPen(theme::c().good, 1));
        p.drawPolyline(curve);
        p.setPen(QPen(theme::c().bright, 1));
        p.drawLines(hits);
        for (const QRectF& r : beats)
            p.fillRect(r, theme::c().good);
    }

private:
    BeatTracker::Trace m_trace;
};

// ---------------------------------------------------------------------------
// BeatTuner
// ---------------------------------------------------------------------------

namespace {

struct Preset {
    const char* name;
    BeatSettings s;
};
std::vector<Preset> presets()
{
    BeatSettings any;
    any.band = BeatSettings::Full;
    BeatSettings house;
    house.minBpm = 100;
    house.maxBpm = 140;
    house.stability = 0.8;
    BeatSettings hiphop;
    hiphop.minBpm = 65;
    hiphop.maxBpm = 110;
    hiphop.sensitivity = 0.55;
    BeatSettings dnb;
    dnb.minBpm = 150;
    dnb.maxBpm = 185;
    dnb.minGapMs = 150;
    BeatSettings band;
    band.band = BeatSettings::Mids;
    band.minBpm = 80;
    band.maxBpm = 170;
    band.stability = 0.5;
    band.sensitivity = 0.6;
    BeatSettings hits;
    hits.locked = false;
    hits.sensitivity = 0.6;
    return {{"Default (kick drum, 70-180 BPM)", BeatSettings{}},
            {"Anything (full range)", any},
            {"House / techno (100-140, steady)", house},
            {"Hip-hop (65-110)", hiphop},
            {"Drum & bass (150-185)", dnb},
            {"Live band / rock (snare, 80-170)", band},
            {"Every hit (no tempo grid)", hits}};
}

} // namespace

BeatTuner::BeatTuner(QWidget* parent) : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Beat Detection"));
    auto* l = new QVBoxLayout(this);

    auto* top = new QHBoxLayout;
    m_bpm = new QLabel(tr("--- BPM"));
    QFont f = m_bpm->font();
    f.setPointSizeF(f.pointSizeF() * 2.0);
    f.setBold(true);
    m_bpm->setFont(f);
    m_bpm->setMinimumWidth(170);
    top->addWidget(m_bpm);
    m_confidence = new QLabel;
    m_confidence->setToolTip(tr("How clearly the audio repeats at that tempo"));
    top->addWidget(m_confidence, 1);
    top->addWidget(new QLabel(tr("Preset:")));
    m_preset = new QComboBox;
    m_preset->addItem(tr("(choose)"));
    for (const Preset& pr : presets())
        m_preset->addItem(tr(pr.name));
    top->addWidget(m_preset);
    l->addLayout(top);

    m_graph = new BeatGraph;
    l->addWidget(m_graph, 1);

    auto* grid = new QGridLayout;
    grid->addWidget(new QLabel(tr("Listen to")), 0, 0);
    m_band = new QComboBox;
    m_band->addItems({tr("Full range"), tr("Bass (kick drum)"), tr("Mids (snare, claps)"), tr("Highs (hi-hats)")});
    m_band->setToolTip(tr("The part of the sound hits are found in. Kick drum works for most dance music; "
                          "hi-hats usually count eighth notes (twice the tempo)."));
    grid->addWidget(m_band, 0, 1);
    grid->addWidget(new QLabel(tr("Beats")), 1, 0);
    m_mode = new QComboBox;
    m_mode->addItems({tr("Locked to the tempo (steady)"), tr("On every hit")});
    m_mode->setToolTip(tr("Locked: steady beats on the tempo grid, following the hits (keeps going through a missed kick, "
                          "and can fire early to make up for latency). Every hit: a beat for each hit heard."));
    grid->addWidget(m_mode, 1, 1);
    l->addLayout(grid);

    auto* knobs = new QHBoxLayout;
    auto knob = [&](const char* id, const char* name, double min, double max, double def, int decimals, const char* suffix, const QString& tip) {
        ParamDef d = fx::num(id, name, min, max, def, decimals, suffix);
        d.tooltip = tip;
        auto* k = new Knob(d);
        k->setToolTip(tip);
        knobs->addWidget(k);
        connect(k, &Knob::valueEdited, this, [this] { apply(); });
        return k;
    };
    m_sensitivity = knob("sensitivity", "Sensitivity", 0, 1, 0.5, 2, "",
                         tr("Higher catches quieter hits (and more false ones): lowers the orange threshold."));
    m_gap = knob("gap", "Min gap", 50, 600, 200, 0, " ms", tr("Hits closer together than this are ignored (stops double triggers)."));
    m_minBpm = knob("minBpm", "Min BPM", 40, 200, 70, 0, "", tr("Slowest tempo: slower ones are read as double."));
    m_maxBpm = knob("maxBpm", "Max BPM", 60, 300, 180, 0, "", tr("Fastest tempo: faster ones are read as half."));
    m_stability = knob("stability", "Stability", 0, 1, 0.6, 2, "",
                       tr("High: the tempo and beat grid hold steady and ignore stray hits. Low: they follow changes quickly."));
    m_window = knob("window", "Window", 2, 12, 6, 1, " s", tr("How much of the recent audio the tempo is worked out from."));
    m_latency = knob("latency", "Latency", 0, 250, 40, 0, " ms",
                     tr("Locked beats fire this much early, to make up for the input's delay. Raise it if the switches lag the music."));
    l->addLayout(knobs);

    auto* buttons = new QHBoxLayout;
    auto* redetect = new QPushButton(tr("Find Tempo Again"));
    redetect->setToolTip(tr("Forget the tempo and beat grid and listen afresh (after a song change); unlocks a locked tempo"));
    auto* defaults = new QPushButton(tr("Defaults"));
    buttons->addWidget(redetect);
    buttons->addStretch(1);
    buttons->addWidget(defaults);
    l->addLayout(buttons);

    connect(m_band, qOverload<int>(&QComboBox::activated), this, [this] { apply(); });
    connect(m_mode, qOverload<int>(&QComboBox::activated), this, [this] { apply(); });
    connect(m_preset, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (i > 0)
            setSettings(presets()[size_t(i - 1)].s);
    });
    connect(defaults, &QPushButton::clicked, this, [this] { setSettings(BeatSettings{}); });
    connect(redetect, &QPushButton::clicked, this, [this] {
        if (m_tracker)
            m_tracker->forget();
        emit tempoForgotten();
    });
    m_timer.setInterval(50); // 20 updates a second are plenty for the graph
    connect(&m_timer, &QTimer::timeout, this, &BeatTuner::refresh);
    m_timer.start();
    setSettings(loadSettings());
    resize(720, 470);
}

void BeatTuner::setTracker(BeatTracker* tracker)
{
    m_tracker = tracker;
    if (m_tracker)
        m_tracker->setSettings(m_settings);
}

void BeatTuner::setSettings(const BeatSettings& s)
{
    m_settings = s;
    m_updating = true;
    m_band->setCurrentIndex(s.band);
    m_mode->setCurrentIndex(s.locked ? 0 : 1);
    m_sensitivity->setValue(s.sensitivity);
    m_gap->setValue(s.minGapMs);
    m_minBpm->setValue(s.minBpm);
    m_maxBpm->setValue(s.maxBpm);
    m_stability->setValue(s.stability);
    m_window->setValue(s.windowSeconds);
    m_latency->setValue(s.latencyMs);
    m_updating = false;
    if (m_tracker)
        m_tracker->setSettings(m_settings);
    saveSettings(m_settings);
    emit settingsChanged(m_settings);
}

void BeatTuner::apply()
{
    if (m_updating)
        return;
    BeatSettings s;
    s.band = m_band->currentIndex();
    s.locked = m_mode->currentIndex() == 0;
    s.sensitivity = m_sensitivity->value();
    s.minGapMs = m_gap->value();
    s.minBpm = m_minBpm->value();
    s.maxBpm = std::max(m_maxBpm->value(), s.minBpm + 10);
    s.stability = m_stability->value();
    s.windowSeconds = m_window->value();
    s.latencyMs = m_latency->value();
    m_preset->setCurrentIndex(0);
    setSettings(s);
}

double BeatTuner::busyMs() const
{
    return m_busyMs + m_graph->busyMs;
}

void BeatTuner::refresh()
{
    if (!isVisible() || isMinimized() || !m_tracker)
        return;
    QElapsedTimer timer;
    timer.start();
    m_graph->setTrace(m_tracker->trace(int(5.0 / (256.0 / 48000.0)))); // the last ~5 s
    // Labels only when they change: a new text or style sheet relays out and
    // repaints the window, which adds up at this rate.
    const double bpm = m_tracker->bpm();
    const QString bpmText = bpm > 0 ? tr("%1 BPM").arg(bpm, 0, 'f', 2) : tr("--- BPM");
    if (m_bpm->text() != bpmText)
        m_bpm->setText(bpmText);
    const int conf = int(std::lround(m_tracker->confidence() * 100));
    const QString confText = bpm > 0 ? (m_tracker->tempoLocked() ? tr("locked, confidence %1%") : tr("confidence %1%")).arg(conf)
                                     : tr("listening...");
    if (m_confidence->text() != confText)
        m_confidence->setText(confText);
    const int level = bpm <= 0 || conf < 12 ? 0 : conf < 30 ? 1 : 2;
    if (level != m_confidenceLevel) {
        m_confidenceLevel = level;
        QPalette pal = m_confidence->palette();
        pal.setColor(QPalette::WindowText, level == 2 ? theme::c().good : level == 1 ? theme::c().warn : theme::c().bad);
        m_confidence->setPalette(pal);
    }
    m_busyMs += double(timer.nsecsElapsed()) / 1e6;
}

BeatSettings BeatTuner::loadSettings()
{
    QSettings q;
    BeatSettings s;
    q.beginGroup(QStringLiteral("live/beat"));
    s.band = std::clamp(q.value(QStringLiteral("band"), s.band).toInt(), 0, 3);
    s.sensitivity = q.value(QStringLiteral("sensitivity"), s.sensitivity).toDouble();
    s.minGapMs = q.value(QStringLiteral("minGap"), s.minGapMs).toDouble();
    s.minBpm = q.value(QStringLiteral("minBpm"), s.minBpm).toDouble();
    s.maxBpm = q.value(QStringLiteral("maxBpm"), s.maxBpm).toDouble();
    s.stability = q.value(QStringLiteral("stability"), s.stability).toDouble();
    s.windowSeconds = q.value(QStringLiteral("window"), s.windowSeconds).toDouble();
    s.locked = q.value(QStringLiteral("locked"), s.locked).toBool();
    s.latencyMs = q.value(QStringLiteral("latency"), s.latencyMs).toDouble();
    return s;
}

void BeatTuner::saveSettings(const BeatSettings& s)
{
    QSettings q;
    q.beginGroup(QStringLiteral("live/beat"));
    q.setValue(QStringLiteral("band"), s.band);
    q.setValue(QStringLiteral("sensitivity"), s.sensitivity);
    q.setValue(QStringLiteral("minGap"), s.minGapMs);
    q.setValue(QStringLiteral("minBpm"), s.minBpm);
    q.setValue(QStringLiteral("maxBpm"), s.maxBpm);
    q.setValue(QStringLiteral("stability"), s.stability);
    q.setValue(QStringLiteral("window"), s.windowSeconds);
    q.setValue(QStringLiteral("locked"), s.locked);
    q.setValue(QStringLiteral("latency"), s.latencyMs);
}
