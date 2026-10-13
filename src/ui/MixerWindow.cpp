#include "ui/MixerWindow.h"

#include "ui/Theme.h"

#include "core/Project.h"
#include "ui/AudioLevels.h"
#include "ui/AudioFinishingWindow.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollArea>
#include <QSlider>
#include <QToolButton>
#include <QVBoxLayout>
#include <optional>

// ---------------------------------------------------------------------------
// LevelMeter: vertical stereo peak meter with peak hold and clip lights.
// ---------------------------------------------------------------------------

class LevelMeter : public QWidget {
public:
    LevelMeter(QWidget* parent = nullptr) : QWidget(parent)
    {
        setFixedWidth(22);
        setMinimumHeight(140);
        setToolTip(tr("Peak level, -60 to 0 dB. The top light shows clipping; click to reset it."));
    }
    void push(std::pair<float, float> peak)
    {
        for (int ch = 0; ch < 2; ++ch) {
            const float v = ch ? peak.second : peak.first;
            m_level[ch] = std::max(m_level[ch] * 0.82f, v);
            if (v >= m_hold[ch]) {
                m_hold[ch] = v;
                m_holdAge[ch] = 0;
            } else if (++m_holdAge[ch] > 45) { // ~1.5 s, then falls
                m_hold[ch] *= 0.9f;
            }
            m_clip[ch] |= v >= 0.999f;
        }
        update();
    }
    float level(int ch) const { return m_level[ch]; }

protected:
    void mousePressEvent(QMouseEvent*) override
    {
        m_clip[0] = m_clip[1] = false;
        update();
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), theme::c().field);
        const int top = 6, h = height() - top - 2;
        for (int ch = 0; ch < 2; ++ch) {
            const QRect bar(2 + ch * 10, top, 8, h);
            p.fillRect(bar, theme::c().panel);
            const int fill = int(h * levels::meterPos(m_level[ch]));
            // Green to -6 dB, yellow to -1 dB, red above.
            for (int y = 0; y < fill; ++y) {
                const double pos = double(y) / h;
                const QColor c = pos > 59.0 / 60 ? theme::c().bad : pos > 54.0 / 60 ? theme::c().warn : theme::c().good;
                p.fillRect(QRect(bar.left(), bar.bottom() - y, bar.width(), 1), c);
            }
            const int hold = int(h * levels::meterPos(m_hold[ch]));
            if (hold > 0)
                p.fillRect(QRect(bar.left(), bar.bottom() - hold, bar.width(), 2), theme::c().bright);
            p.fillRect(QRect(bar.left(), 0, bar.width(), 4), m_clip[ch] ? theme::c().bad : theme::c().field.lighter(130));
        }
    }

private:
    float m_level[2] = {0, 0};
    float m_hold[2] = {0, 0};
    int m_holdAge[2] = {0, 0};
    bool m_clip[2] = {false, false};
};

// ---------------------------------------------------------------------------
// ChannelStrip
// ---------------------------------------------------------------------------

class ChannelStrip : public QFrame {
public:
    ChannelStrip(MixerWindow* window, Project* project, int trackId) : m_window(window), m_project(project), m_trackId(trackId)
    {
        setFrameShape(QFrame::StyledPanel);
        setFixedWidth(118);
        auto* l = new QVBoxLayout(this);
        l->setContentsMargins(6, 6, 6, 6);
        l->setSpacing(4);
        m_name = new QLabel;
        m_name->setAlignment(Qt::AlignCenter);
        m_name->setStyleSheet(QStringLiteral("font-weight: bold;"));
        l->addWidget(m_name);
        auto* buttons = new QHBoxLayout;
        auto button = [&](const QString& text, const QString& tip, const QString& onStyle) {
            auto* b = new QToolButton;
            b->setText(text);
            b->setToolTip(tip);
            b->setCheckable(!onStyle.isEmpty());
            if (!onStyle.isEmpty())
                theme::style(b, QStringLiteral("QToolButton:checked { %1 color: @window; }").arg(onStyle));
            buttons->addWidget(b);
            return b;
        };
        m_mute = button(QStringLiteral("M"), tr("Mute"), QStringLiteral("background: @key;"));
        m_solo = button(QStringLiteral("S"), tr("Solo"), QStringLiteral("background: @warn;"));
        m_fx = button(QStringLiteral("fx"), tr("Track FX"), QString());
        l->addLayout(buttons);
        auto* meterRow = new QHBoxLayout;
        m_meter = new LevelMeter;
        m_fader = new QSlider(Qt::Vertical);
        m_fader->setRange(0, 1000);
        m_fader->setToolTip(tr("Volume (-inf to +6 dB); double-click the value below for 0 dB"));
        meterRow->addWidget(m_fader, 1);
        meterRow->addWidget(m_meter);
        l->addLayout(meterRow, 1);
        m_volume = new QLabel;
        m_volume->setAlignment(Qt::AlignCenter);
        m_volume->installEventFilter(this);
        l->addWidget(m_volume);
        m_pan = new QSlider(Qt::Horizontal);
        m_pan->setRange(-100, 100);
        m_pan->setToolTip(tr("Pan"));
        l->addWidget(m_pan);
        m_panText = new QLabel;
        m_panText->setAlignment(Qt::AlignCenter);
        l->addWidget(m_panText);
        l->addWidget(new QLabel(tr("Duck under:")));
        m_duck = new QComboBox;
        m_duck->setToolTip(tr("Auto duck: turn this track down while the chosen track has sound (e.g. music under a voice)"));
        l->addWidget(m_duck);
        m_duckAmount = new QDoubleSpinBox;
        m_duckAmount->setRange(-60, 0);
        m_duckAmount->setDecimals(0);
        m_duckAmount->setSuffix(tr(" dB"));
        m_duckAmount->setKeyboardTracking(false);
        m_duckAmount->setToolTip(tr("How far it is turned down"));
        l->addWidget(m_duckAmount);

        auto edit = [this](const std::function<void(Track&)>& fn, const QString& label, bool live) {
            Timeline tl = m_project->timeline();
            const int ti = tl.trackIndex(m_trackId);
            if (ti < 0)
                return;
            if (live && !m_before)
                m_before = m_project->timeline();
            fn(tl.tracks[size_t(ti)]);
            if (live)
                m_project->setTimelineLive(tl);
            else
                m_project->applyEdit(tl, label);
        };
        connect(m_mute, &QToolButton::clicked, this, [edit](bool on) { edit([on](Track& t) { t.muted = on; }, tr("Mute Track"), false); });
        connect(m_solo, &QToolButton::clicked, this, [edit](bool on) { edit([on](Track& t) { t.solo = on; }, tr("Solo Track"), false); });
        connect(m_fx, &QToolButton::clicked, this, [this] { emit m_window->trackEffectsRequested(m_trackId); });
        connect(m_fader, &QSlider::valueChanged, this, [this, edit](int v) {
            if (m_updating)
                return;
            const float g = levels::volumeFromPos(v / 1000.0);
            m_volume->setText(levels::volumeText(g));
            edit([g](Track& t) { t.level = g; }, tr("Track Volume"), m_fader->isSliderDown());
        });
        connect(m_pan, &QSlider::valueChanged, this, [this, edit](int v) {
            if (m_updating)
                return;
            const float pan = std::abs(v) < 4 ? 0.0f : v / 100.0f;
            m_panText->setText(levels::panText(pan));
            edit([pan](Track& t) { t.pan = pan; }, tr("Track Pan"), m_pan->isSliderDown());
        });
        for (QSlider* s : {m_fader, m_pan})
            connect(s, &QSlider::sliderReleased, this, [this, s] {
                if (m_before) {
                    const Timeline before = std::move(*m_before);
                    m_before.reset();
                    m_project->commitEdit(before, s == m_fader ? tr("Track Volume") : tr("Track Pan"));
                }
            });
        connect(m_duck, qOverload<int>(&QComboBox::activated), this, [this, edit](int) {
            const int under = m_duck->currentData().toInt();
            edit([under](Track& t) { t.duckUnder = under; }, under ? tr("Auto Duck") : tr("Auto Duck Off"), false);
        });
        connect(m_duckAmount, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, edit](double db) {
            if (!m_updating)
                edit([db](Track& t) { t.duckAmount = float(db); }, tr("Duck Amount"), false);
        });
        refresh();
    }

    int trackId() const { return m_trackId; }
    LevelMeter* meter() const { return m_meter; }
    QSlider* fader() const { return m_fader; }
    QComboBox* duckBox() const { return m_duck; }

    void refresh()
    {
        const Timeline& tl = m_project->timeline();
        const int ti = tl.trackIndex(m_trackId);
        if (ti < 0)
            return;
        const Track& t = tl.tracks[size_t(ti)];
        m_updating = true;
        m_name->setText(t.name.isEmpty() ? tr("Audio") : t.name);
        m_mute->setChecked(t.muted);
        m_solo->setChecked(t.solo);
        if (!m_fader->isSliderDown())
            m_fader->setValue(int(std::lround(levels::posFromVolume(t.level) * 1000)));
        m_volume->setText(levels::volumeText(t.level));
        if (!m_pan->isSliderDown())
            m_pan->setValue(int(std::lround(t.pan * 100)));
        m_panText->setText(levels::panText(t.pan));
        theme::style(m_fx, std::any_of(t.effects.begin(), t.effects.end(), [](const EffectInstance& e) { return e.enabled; })
                                ? QStringLiteral("background: @good; color: @window;")
                                : QString());
        m_duck->clear();
        m_duck->addItem(tr("(off)"), 0);
        for (const Track& other : tl.tracks)
            if (other.type == TrackType::Audio && other.id != t.id)
                m_duck->addItem(other.name.isEmpty() ? tr("Audio") : other.name, other.id);
        m_duck->setCurrentIndex(std::max(0, m_duck->findData(t.duckUnder)));
        m_duckAmount->setValue(t.duckAmount);
        m_duckAmount->setEnabled(t.duckUnder != 0);
        m_updating = false;
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == m_volume && e->type() == QEvent::MouseButtonDblClick) {
            m_fader->setValue(int(std::lround(levels::posFromVolume(1.0f) * 1000))); // 0 dB
            return true;
        }
        return QFrame::eventFilter(o, e);
    }

private:
    MixerWindow* m_window;
    Project* m_project;
    int m_trackId;
    QLabel* m_name;
    QToolButton* m_mute;
    QToolButton* m_solo;
    QToolButton* m_fx;
    LevelMeter* m_meter;
    QSlider* m_fader;
    QLabel* m_volume;
    QSlider* m_pan;
    QLabel* m_panText;
    QComboBox* m_duck;
    QDoubleSpinBox* m_duckAmount;
    std::optional<Timeline> m_before; // while a fader is dragged
    bool m_updating = false;
};

// ---------------------------------------------------------------------------
// MixerWindow
// ---------------------------------------------------------------------------

MixerWindow::MixerWindow(Project* project, QWidget* parent) : QWidget(parent, Qt::Window), m_project(project)
{
    setWindowTitle(tr("Audio Mixer"));
    resize(560, 470);
    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* strips = new QWidget;
    m_row = new QHBoxLayout(strips);
    m_row->setContentsMargins(0, 0, 0, 0);
    m_row->setSpacing(6);
    scroll->setWidget(strips);
    outer->addWidget(scroll, 1);

    auto* master = new QFrame;
    master->setFrameShape(QFrame::StyledPanel);
    master->setFixedWidth(90);
    auto* ml = new QVBoxLayout(master);
    auto* title = new QLabel(tr("Master"));
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral("font-weight: bold;"));
    ml->addWidget(title);
    auto* fx = new QToolButton;
    fx->setText(tr("Master FX"));
    connect(fx, &QToolButton::clicked, this, &MixerWindow::masterEffectsRequested);
    ml->addWidget(fx, 0, Qt::AlignHCenter);
    auto* finish = new QToolButton;
    finish->setText(tr("Finish mix…"));
    finish->setObjectName(QStringLiteral("mixerAudioFinishing"));
    finish->setToolTip(tr("Measure integrated loudness and true peaks, check the final mix and apply safe master gain."));
    connect(finish, &QToolButton::clicked, this, [this] { showAudioFinishing(this, m_project); });
    ml->addWidget(finish, 0, Qt::AlignHCenter);
    m_master = new LevelMeter;
    ml->addWidget(m_master, 1, Qt::AlignHCenter);
    outer->addWidget(master);

    connect(m_project, &Project::changed, this, [this] {
        if (structureKey() != m_key)
            rebuild();
        else
            refresh();
    });
    rebuild();
}

QString MixerWindow::structureKey() const
{
    QString key;
    for (const Track& t : m_project->timeline().tracks)
        if (t.type == TrackType::Audio)
            key += QString::number(t.id) + QLatin1Char(',');
    return key;
}

void MixerWindow::rebuild()
{
    m_key = structureKey();
    for (ChannelStrip* s : m_strips)
        s->deleteLater();
    m_strips.clear();
    while (QLayoutItem* item = m_row->takeAt(0))
        delete item;
    for (const Track& t : m_project->timeline().tracks)
        if (t.type == TrackType::Audio) {
            auto* s = new ChannelStrip(this, m_project, t.id);
            m_strips.push_back(s);
            m_row->addWidget(s);
        }
    if (m_strips.empty()) {
        auto* none = new QLabel(tr("No audio tracks (Insert > Audio Track)."));
        theme::style(none, QStringLiteral("color: @faint;"));
        m_row->addWidget(none);
    }
    m_row->addStretch();
}

void MixerWindow::refresh()
{
    for (ChannelStrip* s : m_strips)
        s->refresh();
}

void MixerWindow::setPeaks(const std::map<int, std::pair<float, float>>& tracks, std::pair<float, float> master)
{
    if (!isVisible())
        return;
    for (ChannelStrip* s : m_strips) {
        const auto it = tracks.find(s->trackId());
        s->meter()->push(it == tracks.end() ? std::pair<float, float>{0, 0} : it->second);
    }
    m_master->push(master);
}
