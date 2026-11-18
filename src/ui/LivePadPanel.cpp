#include "ui/LivePadPanel.h"

#include "ui/Theme.h"

#include "core/AbletonLink.h"
#include "core/BeatTracker.h"
#include "ui/BeatTuner.h"

#include "core/LivePadSets.h"
#include "core/RandomChain.h"
#include "ui/EffectsBrowser.h"
#include "ui/LiveSequencer.h"

#include <QCheckBox>
#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QShortcut>
#include <QSlider>
#include <QDoubleSpinBox>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>

namespace {

// A pad: press to switch it on / off; takes dropped effects and presets unless locked.
class PadButton : public QPushButton {
public:
    std::function<void(const QPoint&)> onMenu;
    std::function<void(const QString& mime, const QString& id)> onDrop;
    bool locked = false;

    PadButton()
    {
        setAcceptDrops(true);
        setMinimumSize(62, 50);
        setMaximumHeight(76);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

protected:
    static QString dropMime(const QMimeData* m)
    {
        for (const char* t : {EffectsBrowser::kEffectMime, EffectsBrowser::kPresetMime})
            if (m->hasFormat(QString::fromLatin1(t)))
                return QString::fromLatin1(t);
        return QString();
    }
    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (!locked && !dropMime(e->mimeData()).isEmpty())
            e->acceptProposedAction();
    }
    void dropEvent(QDropEvent* e) override
    {
        const QString mime = dropMime(e->mimeData());
        if (locked || mime.isEmpty() || !onDrop)
            return;
        onDrop(mime, QString::fromUtf8(e->mimeData()->data(mime)));
        e->setDropAction(Qt::CopyAction); // a copy: the browser keeps its item
        e->accept();
    }
    void contextMenuEvent(QContextMenuEvent* e) override
    {
        if (onMenu)
            onMenu(e->globalPos());
    }
};

// What a pad shows: its name, or the effects in it.
QString padSummary(const LivePad& pad)
{
    if (!pad.name.isEmpty())
        return pad.name;
    QStringList names;
    for (const EffectInstance& e : pad.effects) {
        if (e.isRack())
            names << LivePadPanel::tr("Rack");
        else if (const auto meta = effectMeta(e.type))
            names << meta->name;
    }
    return names.join(QStringLiteral(" + "));
}

const QString kLock = QStringLiteral("\U0001F512");

} // namespace

LivePadPanel::LivePadPanel(Project* project, QWidget* parent)
    : QWidget(parent), m_project(project), m_link(std::make_unique<AbletonLink>())
{
    setObjectName(QStringLiteral("livePads"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(6);

    // ---- Set row ----
    auto* setRow = new QHBoxLayout;
    setRow->setSpacing(4);
    auto* title = new QLabel(tr("Pads"));
    title->setStyleSheet(QStringLiteral("font-weight: bold;"));
    title->setToolTip(tr("Press a pad to switch its effect chain on (on top of the Live FX), press it again to switch it off.\n"
                         "Drop effects or presets on a pad, or right-click it to edit, rename, lock or clear it."));
    setRow->addWidget(title);
    auto* prev = new QToolButton;
    prev->setText(QStringLiteral("◀"));
    prev->setToolTip(tr("Previous set"));
    m_set = new QComboBox;
    m_set->setObjectName(QStringLiteral("livePadSet"));
    m_set->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_set->setMinimumContentsLength(10);
    m_set->setToolTip(tr("Pad sets: whole 4x4 grids saved by name. Pick one to load it (Ctrl+Z undoes)."));
    auto* next = new QToolButton;
    next->setText(QStringLiteral("▶"));
    next->setToolTip(tr("Next set"));
    m_setMenu = new QToolButton;
    m_setMenu->setText(QStringLiteral("≡"));
    m_setMenu->setToolTip(tr("Save, save as, new or delete a set"));
    setRow->addWidget(prev);
    setRow->addWidget(m_set, 1);
    setRow->addWidget(next);
    setRow->addWidget(m_setMenu);
    layout->addLayout(setRow);

    // ---- Pads ----
    auto* grid = new QGridLayout;
    grid->setSpacing(4);
    for (int i = 0; i < kLivePads; ++i) {
        auto* b = new PadButton;
        b->setObjectName(QStringLiteral("livePad%1").arg(i + 1));
        b->onMenu = [this, i](const QPoint& pos) { showMenu(i, pos); };
        b->onDrop = [this, i](const QString& mime, const QString& id) { emit dropped(i, mime, id); };
        // A momentary pad is on while held; the others switch on / off when clicked.
        connect(b, &QPushButton::pressed, this, [this, i] {
            emit padTouched(i);
            if (isMomentary(i))
                emit padHeld(i, true);
        });
        connect(b, &QPushButton::released, this, [this, i] {
            if (isMomentary(i))
                emit padHeld(i, false);
        });
        connect(b, &QPushButton::clicked, this, [this, i] {
            if (QApplication::keyboardModifiers() & Qt::ShiftModifier)
                emit padCued(i); // a look in the Cue window first
            else if (!isMomentary(i))
                pressPad(i);
        });
        m_buttons[size_t(i)] = b;
        grid->addWidget(b, i / 4, i % 4);
    }
    layout->addLayout(grid);

    // ---- Transition, edit target ----
    auto* form = new QGridLayout;
    form->setHorizontalSpacing(6);
    form->addWidget(new QLabel(tr("Transition")), 0, 0);
    m_transition = new QComboBox;
    m_transition->addItem(tr("Cut"), QString());
    for (const TransitionDef& t : transitions())
        m_transition->addItem(t.name, t.id);
    m_transition->setToolTip(tr("How the picture changes from one pad (or none) to the next"));
    form->addWidget(m_transition, 0, 1);
    m_duration = new QComboBox;
    for (double s : {0.1, 0.25, 0.5, 1.0, 2.0, 4.0})
        m_duration->addItem(QStringLiteral("%1 s").arg(s), s);
    m_duration->setToolTip(tr("Length of the transition"));
    form->addWidget(m_duration, 0, 2);
    form->addWidget(new QLabel(tr("Edit")), 1, 0);
    m_edit = new QComboBox;
    m_edit->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_edit->setMinimumContentsLength(12);
    m_edit->setToolTip(tr("Chain shown in the device chain below: the Live FX (always on) or a pad's"));
    form->addWidget(m_edit, 1, 1);
    m_follow = new QCheckBox(tr("Follow"));
    m_follow->setObjectName(QStringLiteral("livePadFollow"));
    m_follow->setToolTip(tr("Show the effects of the pad you switch on (click, key, MIDI, scene), and the Live FX again\n"
                            "when it goes off. Auto switch and momentary pads don't move it."));
    m_follow->setChecked(QSettings().value(QStringLiteral("live/editFollows"), true).toBool());
    connect(m_follow, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("live/editFollows"), on); });
    form->addWidget(m_follow, 1, 2);
    // ---- Mix: the edited pad's, all the pads' (a fader), blackout ----
    auto slider = [](const QString& name, const QString& tip) {
        auto* s = new QSlider(Qt::Horizontal);
        s->setObjectName(name);
        s->setRange(0, 100);
        s->setValue(100);
        s->setToolTip(tip);
        return s;
    };
    form->addWidget(new QLabel(tr("Mix")), 2, 0);
    m_padMix = slider(QStringLiteral("livePadMix"), tr("How much of the edited pad's chain shows over the picture under it (a dry / wet):\n"
                                                       "100% is all of it. Saved with the pad. Pick the pad under Edit."));
    form->addWidget(m_padMix, 2, 1);
    m_padMixLabel = new QLabel;
    form->addWidget(m_padMixLabel, 2, 2);
    form->addWidget(new QLabel(tr("All pads")), 3, 0);
    m_padsMix = slider(QStringLiteral("livePadsMix"), tr("A fader for every pad at once: how much of their chains shows over the Live FX.\n"
                                                         "Right-click: Learn MIDI (a knob or fader); OSC: /mvp/padmix 0..1."));
    form->addWidget(m_padsMix, 3, 1);
    auto* blackRow = new QHBoxLayout;
    blackRow->setSpacing(4);
    m_padsMixLabel = new QLabel(QStringLiteral("100%"));
    blackRow->addWidget(m_padsMixLabel);
    m_blackout = new QToolButton;
    m_blackout->setObjectName(QStringLiteral("liveBlackout"));
    m_blackout->setText(tr("Blackout"));
    m_blackout->setCheckable(true);
    m_blackout->setToolTip(tr("Fade the output (recordings and the stream too) to black, and back when pressed again.\n"
                              "Shortcut: Backspace. Right-click: Learn MIDI."));
    blackRow->addWidget(m_blackout);
    form->addLayout(blackRow, 3, 2);

    // ---- Auto switch ----
    form->addWidget(new QLabel(tr("Auto")), 4, 0);
    m_auto = new QComboBox;
    m_auto->addItems({tr("Off"), tr("Tempo"), tr("Follow music"), tr("MIDI clock")});
    m_auto->setToolTip(tr("Switch pads by themselves while capturing.\n"
                          "Tempo: on the BPM typed, tapped or found (Find Tempo Again); kept in time with the music on the\n"
                          "Audio input while it plays.\n"
                          "Follow music: on the beats heard on the Audio input, its tempo changes and all.\n"
                          "Uses the transition above; empty pads are skipped."));
    form->addWidget(m_auto, 4, 1);
    m_every = new QComboBox;
    for (int n : {1, 2, 4, 8, 16})
        m_every->addItem(n == 1 ? tr("every beat") : tr("every %1").arg(n), n);
    m_every->setToolTip(tr("Beats between switches"));
    form->addWidget(m_every, 4, 2);
    auto* tempoRow = new QHBoxLayout;
    tempoRow->setSpacing(4);
    m_bpm = new QDoubleSpinBox;
    m_bpm->setRange(30, 300);
    m_bpm->setDecimals(2);
    m_bpm->setSingleStep(0.01);
    m_bpm->setSuffix(tr(" BPM"));
    m_bpm->setToolTip(tr("Tempo (with Follow music: the tempo heard). The arrows step 0.01, Page Up / Down 0.1."));
    m_findTempo = new QToolButton;
    m_findTempo->setText(tr("Find Tempo Again"));
    m_findTempo->setToolTip(tr("Forget the tempo and beat grid and listen afresh (after a song change); unlocks a locked tempo"));
    connect(m_findTempo, &QToolButton::clicked, this, &LivePadPanel::findTempoAgain);
    m_tap = new QPushButton(tr("Tap"));
    m_tap->setToolTip(tr("Tap along to set the tempo (and the beat)"));
    m_random = new QCheckBox(tr("Random"));
    m_random->setToolTip(tr("Pick the next pad at random instead of in order"));
    m_led = new QLabel;
    m_led->setFixedSize(14, 14);
    m_led->setToolTip(tr("Beat (orange on the 1 of each bar)"));
    auto tool = [this](const QString& text, const QString& tip, auto slot) {
        auto* b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        connect(b, &QToolButton::clicked, this, slot);
        return b;
    };
    m_half = tool(tr("÷2"), tr("Half the tempo (the music heard twice as fast: 120 for a 60 BPM song)"), [this] { scaleTempo(0.5); });
    m_double = tool(tr("×2"), tr("Double the tempo (the music heard twice as slow)"), [this] { scaleTempo(2.0); });
    m_downbeat = tool(tr("Set 1"), tr("Press on the 1, the first beat of a bar (shortcut: B): switches land on bars from there.\n"
                                       "If the beats are off the music (on the off-beats), this also moves them to where you pressed."),
                      [this] { setDownbeat(); });
    auto* shortcut = new QShortcut(QKeySequence(Qt::Key_B), this);
    connect(shortcut, &QShortcut::activated, this, [this] { setDownbeat(); });
    // The tempo and finding it; adjusting it; options.
    tempoRow->addWidget(m_bpm);
    tempoRow->addWidget(m_findTempo);
    auto* adjustRow = new QHBoxLayout;
    adjustRow->setSpacing(4);
    adjustRow->addWidget(m_half);
    adjustRow->addWidget(m_double);
    adjustRow->addWidget(m_tap);
    adjustRow->addWidget(m_downbeat);
    adjustRow->addStretch(1);
    form->addLayout(tempoRow, 5, 1, 1, 2);
    form->addLayout(adjustRow, 6, 1, 1, 2);
    auto* beatRow = new QHBoxLayout;
    beatRow->setSpacing(4);
    beatRow->addWidget(m_random);
    m_tune = new QToolButton;
    m_tune->setText(tr("Tune..."));
    m_tune->setToolTip(tr("Beat detection settings (Follow music, Find Tempo Again, Tempo kept in time): what to listen to, sensitivity, tempo range, "
                          "stability, latency, with a live view of what it hears"));
    connect(m_tune, &QToolButton::clicked, this, &LivePadPanel::showBeatTuner);
    beatRow->addWidget(m_tune);
    m_lockBpm = new QToolButton;
    m_lockBpm->setText(tr("Lock BPM"));
    m_lockBpm->setCheckable(true);
    m_lockBpm->setToolTip(tr("Keep the tempo heard on the Audio input: it stops changing, while the beats still follow the music.\n"
                             "While it is locked you can also type or tap the tempo yourself."));
    connect(m_lockBpm, &QToolButton::toggled, this, &LivePadPanel::setTempoLocked);
    tempoRow->addWidget(m_lockBpm);
    m_linkButton = new QToolButton;
    m_linkButton->setObjectName(QStringLiteral("liveLink"));
    m_linkButton->setText(tr("Link"));
    m_linkButton->setCheckable(true);
    m_linkButton->setToolTip(tr("Ableton Link: share the tempo and beat with music apps on the network (Ableton Live, Traktor,\n"
                                "phone apps...). The beat clock follows the session's beats and bars, and typing, tapping,\n"
                                "x2 or /2 change the tempo for every app. The number is how many other apps are in it."));
    m_linkButton->setVisible(AbletonLink::available());
    adjustRow->insertWidget(adjustRow->count() - 1, m_linkButton); // (before the stretch)
    tempoRow->addStretch(1);
    tempoRow->addWidget(m_led);
    beatRow->addStretch(1);
    form->addLayout(beatRow, 7, 1, 1, 2);
    form->addWidget(new QLabel(tr("Launch")), 8, 0);
    m_launch = new QComboBox;
    m_launch->addItems({tr("At once"), tr("On the next beat"), tr("On the next bar (the 1)")});
    m_launch->setToolTip(tr("With Auto on, when a pressed pad switches: straight away, or on the next beat or bar of the beat\n"
                            "clock, so switches land in time even if you press a little early. With Auto off pads switch at\n"
                            "once (momentary pads always do)."));
    form->addWidget(m_launch, 8, 1, 1, 2);
    form->setColumnStretch(1, 1);
    layout->addLayout(form);
    m_sequencer = new LiveSequencer(project, this);
    layout->addWidget(m_sequencer);
    layout->addStretch(1);
    connect(this, &LivePadPanel::beatTicked, m_sequencer, &LiveSequencer::beat);
    connect(m_sequencer, &LiveSequencer::padPressed, this, &LivePadPanel::pressPad);
    connect(m_sequencer, &LiveSequencer::holdPad, this, &LivePadPanel::padHeld); // (a momentary pad's block: held for it)
    connect(m_sequencer, &LiveSequencer::switchPad, this, [this](int pad) {
        // The loop switches its pads at once (it is in time already), and switches off only what it switched on.
        const int was = m_sequenced;
        m_sequenced = pad;
        m_stepping = true; // (the device chain doesn't follow it, as with auto switch)
        if (pad < 0) {
            if (m_active >= 0 && m_active == was)
                emit padPressed(m_active); // (pressing the live pad switches it off)
        } else if (pad != m_active) {
            emit padPressed(pad);
        }
        m_stepping = false;
    });

    QSettings settings;
    const int t = m_transition->findData(settings.value(QStringLiteral("live/padTransition"), QStringLiteral("dissolve")).toString());
    m_transition->setCurrentIndex(std::max(0, t));
    const int d = m_duration->findData(settings.value(QStringLiteral("live/padSeconds"), 0.5).toDouble());
    m_duration->setCurrentIndex(d >= 0 ? d : 2);
    m_bpm->setValue(settings.value(QStringLiteral("live/autoBpm"), 120).toDouble());
    m_every->setCurrentIndex(std::max(0, m_every->findData(settings.value(QStringLiteral("live/autoEvery"), 4).toInt())));
    m_random->setChecked(settings.value(QStringLiteral("live/autoRandom"), false).toBool());
    m_launch->setCurrentIndex(std::clamp(settings.value(QStringLiteral("live/launch"), 0).toInt(), 0, 2));
    connect(m_launch, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) {
        QSettings().setValue(QStringLiteral("live/launch"), i);
        if (i == LaunchNow && m_pendingPad >= 0) { // nothing waits any more
            const int p = m_pendingPad;
            m_pendingPad = -1;
            emit padPressed(p);
        }
    });
    auto save = [this] {
        QSettings s;
        s.setValue(QStringLiteral("live/padTransition"), m_transition->currentData().toString());
        s.setValue(QStringLiteral("live/padSeconds"), m_duration->currentData().toDouble());
        s.setValue(QStringLiteral("live/autoBpm"), m_bpm->value());
        s.setValue(QStringLiteral("live/autoEvery"), m_every->currentData().toInt());
        s.setValue(QStringLiteral("live/autoRandom"), m_random->isChecked());
    };
    connect(m_transition, qOverload<int>(&QComboBox::currentIndexChanged), this, save);
    connect(m_duration, qOverload<int>(&QComboBox::currentIndexChanged), this, save);
    connect(m_every, qOverload<int>(&QComboBox::currentIndexChanged), this, save);
    connect(m_random, &QCheckBox::toggled, this, save);
    connect(m_bpm, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this, save] {
        m_adoptHeard = false; // (typed: that one)
        save();
        updateAutoTimer();
        if (m_tracker && m_lockBpm->isChecked()) // typed while locked: the new tempo
            m_tracker->setTempoLocked(true, m_bpm->value());
        pushTempo();
    });
    // Launch follows the beat only with Auto on: off, pads switch at once (one waiting goes now).
    auto launchFollows = [this] {
        const bool beats = m_auto->currentIndex() != AutoOff;
        m_launch->setEnabled(beats);
        if (!beats && m_pendingPad >= 0) {
            const int p = m_pendingPad;
            m_pendingPad = -1;
            emit padPressed(p);
            refresh();
        }
    };
    launchFollows();
    connect(m_auto, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, launchFollows] {
        m_beatIndex = 15;
        m_bpm->setReadOnly((m_auto->currentIndex() == AutoAudio || m_auto->currentIndex() == AutoMidi) && !m_lockBpm->isChecked());
        updateAutoTimer();
        launchFollows();
    });
    connect(m_tap, &QPushButton::clicked, this, &LivePadPanel::tap);
    connect(m_edit, qOverload<int>(&QComboBox::activated), this, [this](int index) { emit editRequested(index); });
    connect(m_set, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        const QString name = m_set->itemData(index).toString();
        if (!name.isEmpty() && !loadSet(name))
            emit message(tr("Could not load the pad set \"%1\"").arg(name));
    });
    connect(prev, &QToolButton::clicked, this, [this] { stepSet(-1); });
    connect(next, &QToolButton::clicked, this, [this] { stepSet(1); });
    connect(m_setMenu, &QToolButton::clicked, this, &LivePadPanel::showSetMenu);
    connect(m_project, &Project::changed, this, &LivePadPanel::refresh);
    m_beatTimer.setTimerType(Qt::PreciseTimer);
    m_beatTimer.setSingleShot(true);
    connect(&m_beatTimer, &QTimer::timeout, this, [this] {
        // Each beat is due a period after the last was due (not after it fired), so a timer
        // late by a millisecond, or a period like 689.57 ms, doesn't drift off the music.
        const double period = 60000.0 / std::max(30.0, m_bpm->value());
        const double now = double(m_clock.nsecsElapsed()) / 1e6;
        m_nextBeatMs += period;
        if (m_nextBeatMs < now) // (held up a long while: from here)
            m_nextBeatMs = now + period;
        m_beatTimer.start(int(std::max(1.0, std::round(m_nextBeatMs - now))));
        // Auto off: the clock follows the audio beats while they come.
        if (m_auto->currentIndex() == AutoOff && m_clock.elapsed() - m_lastAudioBeatMs < 2000)
            return;
        beat();
    });
    // Right-click Learn MIDI on the tempo buttons.
    auto learnable = [this](QWidget* w, const QString& action, const QString& label) {
        w->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(w, &QWidget::customContextMenuRequested, this, [this, w, action, label](const QPoint& pos) {
            QMenu menu(this);
            menu.addAction(tr("Learn MIDI..."), this, [this, action, label] { emit midiLearnRequested(action, label); });
            menu.exec(w->mapToGlobal(pos));
        });
    };
    learnable(m_tap, QStringLiteral("tap"), tr("Tap"));
    learnable(m_downbeat, QStringLiteral("setone"), tr("Set 1"));
    learnable(m_findTempo, QStringLiteral("findtempo"), tr("Find Tempo Again"));
    learnable(m_lockBpm, QStringLiteral("lockbpm"), tr("Lock BPM"));
    learnable(m_padsMix, QStringLiteral("padmix"), tr("All pads"));
    learnable(m_blackout, QStringLiteral("blackout"), tr("Blackout"));
    // The edited pad's mix: dragging shows it live, letting go is one undo step.
    connect(m_padMix, &QSlider::sliderPressed, this, [this] { m_mixBefore = m_project->timeline(); });
    connect(m_padMix, &QSlider::valueChanged, this, [this](int value) {
        const int pad = m_editing - 1;
        if (pad < 0 || pad >= kLivePads)
            return;
        m_padMixLabel->setText(QStringLiteral("%1%").arg(value));
        if (m_padMix->isSliderDown()) {
            Timeline tl = m_project->timeline();
            tl.livePads[size_t(pad)].mix = value / 100.0;
            m_project->setTimelineLive(tl);
        } else {
            editPad(pad, [value](LivePad& p) { p.mix = value / 100.0; }, tr("Pad Mix"));
        }
    });
    connect(m_padMix, &QSlider::sliderReleased, this, [this] {
        if (m_mixBefore && *m_mixBefore != m_project->timeline())
            m_project->commitEdit(*m_mixBefore, tr("Pad Mix"));
        m_mixBefore.reset();
    });
    connect(m_padsMix, &QSlider::valueChanged, this, [this](int value) {
        m_padsMixLabel->setText(QStringLiteral("%1%").arg(value));
        emit padsMixChanged(value / 100.0);
    });
    connect(m_blackout, &QToolButton::toggled, this, [this](bool on) {
        theme::style(m_blackout, on ? QStringLiteral("QToolButton { background: @bad; color: @onAccent; font-weight: bold; }") : QString());
        emit blackoutChanged(on);
        emit message(on ? tr("Blackout: the output fades to black (press again to bring it back)") : tr("Blackout off"));
    });
    // Ableton Link: the beat clock follows the session.
    m_linkTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_linkTimer, &QTimer::timeout, this, &LivePadPanel::pollLink);
    connect(m_linkButton, &QToolButton::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("live/link"), on);
        m_link->setEnabled(on);
        if (on) {
            if (m_link->peers() == 0)
                m_link->setTempo(m_bpm->value()); // a new session: ours
            m_linkBeat = std::floor(m_link->beat());
            m_linkTimer.start(4);
        } else {
            m_linkTimer.stop();
            m_linkButton->setText(tr("Link"));
        }
        m_beatIndex = 15;
        updateAutoTimer();
        emit message(on ? tr("Ableton Link on: the beat follows the session") : tr("Ableton Link off"));
    });
    m_ledTimer.setSingleShot(true);
    connect(&m_ledTimer, &QTimer::timeout, this, [this] { theme::style(m_led, QStringLiteral("background: @raised; border-radius: 7px;")); });
    theme::style(m_led, QStringLiteral("background: @raised; border-radius: 7px;"));
    m_clock.start();
    if (AbletonLink::available() && settings.value(QStringLiteral("live/link"), false).toBool())
        m_linkButton->setChecked(true);
    refreshSets(); // starts as "(not saved)": the project's pads may come from anywhere
    refresh();
    updateAutoTimer(); // the beat clock runs from the start (modulation follows it)
}

LivePadPanel::~LivePadPanel() = default;

void LivePadPanel::setPadsMix(double mix)
{
    m_padsMix->setValue(int(std::lround(std::clamp(mix, 0.0, 1.0) * 100)));
}

double LivePadPanel::padsMix() const
{
    return m_padsMix->value() / 100.0;
}

void LivePadPanel::showMasterLevel(double level)
{
    const int percent = int(std::lround(level * 100));
    m_blackout->setText(percent >= 100 ? tr("Blackout") : tr("Output %1%").arg(percent));
}

bool LivePadPanel::linkOn() const
{
    return m_linkButton->isChecked();
}

void LivePadPanel::pushTempo()
{
    if (linkOn())
        m_link->setTempo(m_bpm->value());
}

void LivePadPanel::pollLink()
{
    const double tempo = m_link->tempo();
    if (tempo > 0 && std::abs(tempo - m_bpm->value()) > 0.005) { // another app changed it
        {
            QSignalBlocker block(m_bpm);
            m_bpm->setValue(tempo);
        }
        emit tempoChanged(currentBpm());
    }
    if (++m_linkPolls % 100 == 1) { // (about twice a second)
        const int peers = m_link->peers();
        m_linkButton->setText(peers > 0 ? tr("Link %1").arg(peers) : tr("Link"));
    }
    const double b = std::floor(m_link->beat());
    if (b == m_linkBeat)
        return;
    m_linkBeat = b;
    // The session knows where the bar is: beat 0, 4, 8... is a 1.
    const int barBeat = int(((qint64(b) % 4) + 4) % 4);
    m_beatIndex = ((m_beatIndex / 4) * 4 + barBeat + 15) % 16;
    beat();
}

void LivePadPanel::setActivePad(int pad)
{
    m_active = pad;
    m_sequencer->setActivePad(pad);
    refresh();
}

void LivePadPanel::setEditing(int target)
{
    m_editing = std::clamp(target, 0, kLayerTarget + kLiveLayers - 1);
    refresh();
}

void LivePadPanel::setHeldPads(const std::vector<int>& pads)
{
    m_heldPads = pads;
    m_sequencer->setHeldPads(pads);
    refresh();
}

bool LivePadPanel::isMomentary(int pad) const
{
    return pad >= 0 && pad < kLivePads && m_project->timeline().livePads[size_t(pad)].momentary;
}

void LivePadPanel::setMomentary(int pad, bool momentary)
{
    if (pad >= 0 && pad < kLivePads)
        editPad(pad, [momentary](LivePad& p) { p.momentary = momentary; }, momentary ? tr("Momentary Pad") : tr("Toggle Pad"));
}

TransitionSpec LivePadPanel::transition() const
{
    TransitionSpec spec;
    spec.type = m_transition->currentData().toString();
    if (const TransitionDef* def = findTransition(spec.type))
        spec = makeTransition(*def);
    return spec;
}

double LivePadPanel::transitionSeconds() const
{
    return m_transition->currentData().toString().isEmpty() ? 0.0 : m_duration->currentData().toDouble();
}

bool LivePadPanel::isLocked(int pad) const
{
    return pad >= 0 && pad < kLivePads && m_project->timeline().livePads[size_t(pad)].locked;
}

void LivePadPanel::setLocked(int pad, bool locked)
{
    if (pad >= 0 && pad < kLivePads)
        editPad(pad, [locked](LivePad& p) { p.locked = locked; }, locked ? tr("Lock Pad") : tr("Unlock Pad"));
}

void LivePadPanel::refresh()
{
    const Timeline& tl = m_project->timeline();
    for (int i = 0; i < kLivePads; ++i) {
        const LivePad& pad = tl.livePads[size_t(i)];
        auto* b = static_cast<PadButton*>(m_buttons[size_t(i)]);
        b->locked = pad.locked;
        const QString summary = padSummary(pad);
        const QString elided = summary.size() > 22 ? summary.left(20) + QStringLiteral("...") : summary;
        b->setText(QStringLiteral("%1%2\n%3").arg(i + 1).arg((pad.locked ? QStringLiteral(" ") + kLock : QString()) + (pad.momentary ? tr(" (hold)") : QString()))
                       .arg(elided.isEmpty() ? tr("empty") : elided));
        QString tip = summary.isEmpty() ? tr("Pad %1: no effects yet (drop effects or presets here)").arg(i + 1)
                                        : tr("Pad %1: %2").arg(i + 1).arg(summary);
        if (pad.locked)
            tip += tr("\nLocked: right-click > Unlock Pad to change it");
        b->setToolTip(tip);
        const bool active = i == m_active, editing = i + 1 == m_editing, empty = pad.effects.empty();
        const bool held = std::find(m_heldPads.begin(), m_heldPads.end(), i) != m_heldPads.end();
        const bool pending = i == m_pendingPad, cued = i == m_cued;
        QString style = QStringLiteral("QPushButton { border-radius: 4px; font-size: 11px; padding: 2px; ");
        if (held)
            style += QStringLiteral("background: @key; color: @window; font-weight: bold; ");
        else if (active)
            style += QStringLiteral("background: @accent; color: @onAccent; font-weight: bold; ");
        else
            style += empty ? QStringLiteral("background: @panel; color: @faint; ") : QStringLiteral("background: @raised; color: @text; ");
        style += pending ? QStringLiteral("border: 2px dashed @info; }")
               : cued    ? QStringLiteral("border: 2px solid @cue; }")
               : editing ? QStringLiteral("border: 2px solid @warn; }")
                         : (pad.locked ? QStringLiteral("border: 1px solid @warn; }") : QStringLiteral("border: 1px solid @line; }"));
        style += active ? QStringLiteral("QPushButton:hover { background: @accent; }") : QStringLiteral("QPushButton:hover { background: @hover; }");
        theme::style(b, style);
    }
    {
        QSignalBlocker block(m_edit);
        m_edit->clear();
        m_edit->addItem(tr("Live FX (always on)"));
        for (int i = 0; i < kLivePads; ++i) {
            const LivePad& pad = tl.livePads[size_t(i)];
            const QString summary = padSummary(pad);
            QString text = summary.isEmpty() ? tr("Pad %1").arg(i + 1) : tr("Pad %1: %2").arg(i + 1).arg(summary);
            if (pad.locked)
                text += QStringLiteral(" ") + kLock;
            m_edit->addItem(text);
        }
        for (int i = 0; i < kLiveLayers; ++i)
            m_edit->addItem(tr("Layer %1 FX").arg(i + 1));
        m_edit->setCurrentIndex(m_editing);
    }
    // The edited pad's mix.
    {
        const int pad = m_editing - 1;
        const bool isPad = pad >= 0 && pad < kLivePads;
        const double mix = isPad ? tl.livePads[size_t(pad)].mix : 1.0;
        m_padMix->setEnabled(isPad && !tl.livePads[size_t(pad)].locked);
        if (!m_padMix->isSliderDown()) {
            QSignalBlocker block(m_padMix);
            m_padMix->setValue(int(std::lround(mix * 100)));
        }
        m_padMixLabel->setText(isPad ? QStringLiteral("%1%").arg(m_padMix->value()) : QString());
        m_padMix->setToolTip(isPad ? tr("Pad %1's mix: how much of its chain shows over the picture under it (a dry / wet).\n"
                                        "Saved with the pad.").arg(pad + 1)
                                   : tr("Pick a pad under Edit to set its mix (how much of its chain shows)"));
    }
    // The set's name, starred when the pads differ from it.
    const int index = m_setName.isEmpty() ? -1 : m_set->findData(m_setName);
    if (index >= 0)
        m_set->setItemText(index, m_setName + (setModified() ? QStringLiteral(" *") : QString()));
}

// ---- Sets ------------------------------------------------------------------------------

void LivePadPanel::refreshSets()
{
    QSignalBlocker block(m_set);
    m_set->clear();
    const QStringList names = padsets::names();
    if (m_setName.isEmpty() || !names.contains(m_setName))
        m_set->addItem(names.isEmpty() ? tr("(no saved sets)") : tr("(not saved)"), QString());
    for (const QString& n : names)
        m_set->addItem(n, n);
    m_set->setCurrentIndex(std::max(0, m_set->findData(m_setName.isEmpty() || !names.contains(m_setName) ? QString() : m_setName)));
    refresh();
}

bool LivePadPanel::setModified() const
{
    return !m_setName.isEmpty() && m_project->timeline().livePads != m_loadedPads;
}

bool LivePadPanel::loadSet(const QString& name)
{
    const auto set = padsets::load(name);
    if (!set)
        return false;
    Timeline tl = m_project->timeline();
    for (int i = 0; i < kLivePads; ++i) {
        LivePad pad = set->pads[size_t(i)];
        reassignEffectIds(pad.effects, [&tl] { return tl.allocId(); }); // effects keep their own state
        tl.livePads[size_t(i)] = std::move(pad);
    }
    m_project->applyEdit(tl, tr("Load Pad Set"));
    m_setName = name;
    m_loadedPads = tl.livePads;
    refreshSets();
    emit setLoaded(name);
    return true;
}

bool LivePadPanel::saveSetAs(const QString& name, QString* error)
{
    const QString n = name.trimmed();
    if (!padsets::save({n, m_project->timeline().livePads}, error))
        return false;
    m_setName = n;
    m_loadedPads = m_project->timeline().livePads;
    refreshSets();
    return true;
}

void LivePadPanel::stepSet(int delta)
{
    const QStringList names = padsets::names();
    if (names.isEmpty())
        return;
    int i = int(names.indexOf(m_setName));
    i = i < 0 ? (delta > 0 ? 0 : int(names.size()) - 1) : (i + delta + int(names.size())) % int(names.size());
    loadSet(names[i]);
}

void LivePadPanel::showSetMenu()
{
    QMenu menu(this);
    auto saveAs = [this] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Save Pad Set"), tr("Name of the set:"), QLineEdit::Normal, m_setName, &ok).trimmed();
        if (!ok || name.isEmpty())
            return;
        if (name != m_setName && padsets::names().contains(name)
            && QMessageBox::question(this, tr("Save Pad Set"), tr("Replace the set \"%1\"?").arg(name)) != QMessageBox::Yes)
            return;
        QString error;
        if (!saveSetAs(name, &error))
            QMessageBox::warning(this, tr("Save Pad Set"), tr("Could not save the set: %1").arg(error));
    };
    QAction* save = menu.addAction(m_setName.isEmpty() ? tr("Save Set...") : tr("Save \"%1\"").arg(m_setName), this, [this, saveAs] {
        if (m_setName.isEmpty()) {
            saveAs();
            return;
        }
        QString error;
        if (!saveSetAs(m_setName, &error))
            QMessageBox::warning(this, tr("Save Pad Set"), tr("Could not save the set: %1").arg(error));
    });
    save->setEnabled(m_setName.isEmpty() || setModified());
    menu.addAction(tr("Save As..."), this, saveAs);
    menu.addAction(tr("New Empty Set"), this, [this] {
        Timeline tl = m_project->timeline();
        tl.livePads = LivePads();
        m_project->applyEdit(tl, tr("New Pad Set"));
        m_setName.clear();
        refreshSets();
        emit setLoaded(QString());
    });
    QMenu* fill = menu.addMenu(tr("Fill Empty Pads with Random Chains"));
    for (int t = 0; t < 4; ++t)
        fill->addAction(randomTierName(RandomTier(t)), this, [this, t] { randomizeEmptyPads(t); });
    QMenu* fillAll = menu.addMenu(tr("Replace All Pads with Random Chains"));
    fillAll->setToolTip(tr("Every pad that isn't locked gets a new random chain, replacing its effects (Undo puts them back)"));
    fillAll->setToolTipsVisible(true);
    for (int t = 0; t < 4; ++t)
        fillAll->addAction(randomTierName(RandomTier(t)), this, [this, t] { randomizeEmptyPads(t, true); });
    QAction* allHeld = menu.addAction(tr("Make All Pads Momentary"), this, [this] { makeAllMomentary(); });
    allHeld->setToolTip(tr("Every pad that isn't locked plays only while held (Undo puts them back)"));
    allHeld->setEnabled(std::any_of(m_project->timeline().livePads.begin(), m_project->timeline().livePads.end(),
                                    [](const LivePad& p) { return !p.locked && !p.momentary; }));
    QAction* allNormal = menu.addAction(tr("Make All Pads Normal"), this, [this] { makeAllMomentary(false); });
    allNormal->setToolTip(tr("Every momentary pad that isn't locked switches on and off when pressed again (Undo puts them back)"));
    allNormal->setEnabled(std::any_of(m_project->timeline().livePads.begin(), m_project->timeline().livePads.end(),
                                      [](const LivePad& p) { return !p.locked && p.momentary; }));
    menu.addSeparator();
    QAction* del = menu.addAction(tr("Delete \"%1\"").arg(m_setName), this, [this] {
        if (QMessageBox::question(this, tr("Delete Pad Set"), tr("Delete the saved set \"%1\"? The pads stay as they are.").arg(m_setName))
            != QMessageBox::Yes)
            return;
        padsets::remove(m_setName);
        m_setName.clear();
        refreshSets();
    });
    del->setEnabled(!m_setName.isEmpty());
    menu.addSeparator();
    menu.addAction(tr("Show the Sets Folder"), this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(padsets::directory())); });
    menu.exec(m_setMenu->mapToGlobal(QPoint(0, m_setMenu->height())));
}

// ---- Pads ------------------------------------------------------------------------------

void LivePadPanel::editPad(int pad, const std::function<void(LivePad&)>& change, const QString& description)
{
    Timeline tl = m_project->timeline();
    change(tl.livePads[size_t(pad)]);
    if (tl != m_project->timeline())
        m_project->applyEdit(tl, description);
}

void LivePadPanel::showMenu(int pad, const QPoint& globalPos)
{
    const LivePad& p = m_project->timeline().livePads[size_t(pad)];
    const bool locked = p.locked;
    QMenu menu(this);
    menu.addAction(tr("Cue (preview it, not live; Shift+click)"), this, [this, pad] { emit padCued(pad); });
    menu.addSeparator();
    menu.addAction(locked ? tr("View Chain") : tr("Edit Chain"), this, [this, pad] { emit editRequested(pad + 1); });
    QAction* rename = menu.addAction(tr("Rename..."), this, [this, pad] {
        bool ok = false;
        const QString name = QInputDialog::getText(this, tr("Rename Pad"), tr("Name of pad %1:").arg(pad + 1), QLineEdit::Normal,
                                                   m_project->timeline().livePads[size_t(pad)].name, &ok);
        if (ok)
            editPad(pad, [&](LivePad& lp) { lp.name = name.trimmed(); }, tr("Rename Pad"));
    });
    rename->setEnabled(!locked);
    QAction* copy = menu.addAction(tr("Copy Live FX Here"), this, [this, pad] {
        Timeline tl = m_project->timeline();
        EffectChain chain = tl.liveEffects;
        reassignEffectIds(chain, [&tl] { return tl.allocId(); }); // effects keep their own state per pad
        tl.livePads[size_t(pad)].effects = std::move(chain);
        m_project->applyEdit(tl, tr("Copy Live FX to Pad"));
    });
    copy->setEnabled(!locked && !m_project->timeline().liveEffects.empty());
    QMenu* random = menu.addMenu(tr("Create Random Effects Chain"));
    random->setToolTip(tr("Replaces the pad's effects with a random chain that changes the picture this much"));
    random->setToolTipsVisible(true);
    const QString tierTips[] = {tr("A gentle look: a colour grade and one subtle touch"),
                                tr("A clear style: colour, a texture or distortion and a glitch"),
                                tr("Heavy: distortion, glitches, maybe trails or feedback"),
                                tr("Everything at once: six to eight effects, pushed hard")};
    for (int t = 0; t < 4; ++t) {
        QAction* a = random->addAction(randomTierName(RandomTier(t)), this, [this, pad, t] { randomizePad(pad, t); });
        a->setToolTip(tierTips[t]);
    }
    random->setEnabled(!locked);
    menu.addSeparator();
    QAction* hold = menu.addAction(tr("Momentary (on only while held)"), this, [this, pad](bool on) { setMomentary(pad, on); });
    hold->setCheckable(true);
    hold->setChecked(p.momentary);
    hold->setEnabled(!locked);
    hold->setToolTip(tr("Hold the pad (mouse, key or MIDI) to play it on top of the active pad: strobes, freezes, glitch bursts"));
    menu.addAction(tr("Learn MIDI..."), this, [this, pad] { emit midiLearnRequested(QStringLiteral("pad:%1").arg(pad), tr("pad %1").arg(pad + 1)); });
    QAction* lock = menu.addAction(locked ? tr("Unlock Pad") : tr("Lock Pad"), this, [this, pad, locked] { setLocked(pad, !locked); });
    lock->setToolTip(tr("A locked pad can still be played, but its effects, name and settings cannot be changed"));
    QAction* clear = menu.addAction(tr("Clear Pad"), this, [this, pad] { editPad(pad, [](LivePad& lp) { lp = LivePad(); }, tr("Clear Pad")); });
    clear->setEnabled(!locked && !p.isEmpty());
    menu.exec(globalPos);
}

bool LivePadPanel::randomizePad(int pad, int tier, uint32_t seed)
{
    if (pad < 0 || pad >= kLivePads || isLocked(pad))
        return false;
    Timeline tl = m_project->timeline();
    const RandomChain chain = makeRandomChain(RandomTier(std::clamp(tier, 0, 3)), seed ? seed : QRandomGenerator::global()->generate(),
                                              [&tl] { return tl.allocId(); });
    tl.livePads[size_t(pad)].effects = chain.effects;
    tl.livePads[size_t(pad)].name = chain.name;
    m_project->applyEdit(tl, tr("Random Effects Chain"));
    QStringList names;
    for (const EffectInstance& e : chain.effects)
        if (const EffectDef* def = findEffect(e.type))
            names << def->name;
    emit message(tr("Pad %1: \"%2\" (%3) - %4").arg(pad + 1).arg(chain.name, randomTierName(RandomTier(tier)), names.join(QStringLiteral(", "))));
    return true;
}

int LivePadPanel::randomizeEmptyPads(int tier, bool all)
{
    Timeline tl = m_project->timeline();
    int filled = 0;
    for (int pad = 0; pad < kLivePads; ++pad) {
        LivePad& lp = tl.livePads[size_t(pad)];
        if (lp.locked || (!all && !lp.isEmpty()))
            continue;
        const RandomChain chain = makeRandomChain(RandomTier(std::clamp(tier, 0, 3)), QRandomGenerator::global()->generate(),
                                                  [&tl] { return tl.allocId(); });
        lp.effects = chain.effects;
        lp.name = chain.name;
        ++filled;
    }
    if (filled)
        m_project->applyEdit(tl, tr("Random Effects Chains"));
    if (all)
        emit message(filled ? tr("Replaced %1 pad(s) with %2 random chains").arg(filled).arg(randomTierName(RandomTier(tier)))
                            : tr("Every pad is locked"));
    else
        emit message(filled ? tr("Filled %1 empty pad(s) with %2 random chains").arg(filled).arg(randomTierName(RandomTier(tier)))
                            : tr("There are no empty pads to fill"));
    return filled;
}

int LivePadPanel::makeAllMomentary(bool momentary)
{
    Timeline tl = m_project->timeline();
    int changed = 0;
    for (LivePad& lp : tl.livePads)
        if (!lp.locked && lp.momentary != momentary) {
            lp.momentary = momentary;
            ++changed;
        }
    if (changed)
        m_project->applyEdit(tl, momentary ? tr("Make All Pads Momentary") : tr("Make All Pads Normal"));
    if (momentary)
        emit message(changed ? tr("%1 pad(s) made momentary: each plays only while held").arg(changed) : tr("Every pad is momentary already"));
    else
        emit message(changed ? tr("%1 pad(s) made normal: each switches on and off when pressed").arg(changed) : tr("No pad is momentary"));
    return changed;
}

// ---- Auto switch -------------------------------------------------------------------------

void LivePadPanel::setAutoMode(int mode)
{
    m_auto->setCurrentIndex(std::clamp(mode, 0, int(AutoMidi)));
}

void LivePadPanel::setRunning(bool running)
{
    m_running = running;
    m_beatIndex = 15; // the first beat is a 1
    m_sequenced = -1;
    m_sequencer->setRunning(running);
    m_pendingPad = -1;
    updateAutoTimer();
}

double LivePadPanel::bpm() const
{
    return m_bpm->value();
}

void LivePadPanel::updateAutoTimer()
{
    // The clock runs on the tempo unless it follows the audio beats (Auto off
    // only while they come).
    if (linkOn())
        m_beatTimer.stop(); // (Link's beats instead)
    else if (m_auto->currentIndex() == AutoOff || m_auto->currentIndex() == AutoTempo)
        restartBeatGrid();
    else
        m_beatTimer.stop();
    emit tempoChanged(currentBpm());
}

void LivePadPanel::restartBeatGrid()
{
    const double period = 60000.0 / std::max(30.0, m_bpm->value());
    m_nextBeatMs = double(m_clock.nsecsElapsed()) / 1e6 + period;
    m_beatTimer.start(int(std::lround(period)));
}

void LivePadPanel::tap()
{
    m_adoptHeard = false;
    const qint64 now = m_clock.elapsed();
    if (!m_taps.empty() && now - m_taps.back() > 2500) // a pause starts a new count
        m_taps.clear();
    m_taps.push_back(now);
    if (m_taps.size() > 6)
        m_taps.erase(m_taps.begin());
    if (m_taps.size() >= 2) {
        const double interval = double(m_taps.back() - m_taps.front()) / double(m_taps.size() - 1);
        QSignalBlocker block(m_bpm);
        m_bpm->setValue(std::clamp(60000.0 / interval, 30.0, 300.0));
        QSettings().setValue(QStringLiteral("live/autoBpm"), m_bpm->value());
        if (m_tracker && m_lockBpm->isChecked())
            m_tracker->setTempoLocked(true, m_bpm->value());
        pushTempo();
    }
    if (linkOn())
        return; // (the session keeps the beat)
    // The tap is the beat: restart the tempo from here.
    m_beatIndex = 0;
    if (m_auto->currentIndex() == AutoTempo) {
        updateAutoTimer();
        beat();
    }
}

void LivePadPanel::clockBeat(double bpm, int barBeat)
{
    if (m_auto->currentIndex() != AutoMidi || linkOn())
        return;
    if (bpm > 0) {
        QSignalBlocker block(m_bpm);
        m_bpm->setValue(bpm);
    }
    // The clock knows where the bar is (Start, song position): the next beat is barBeat.
    m_beatIndex = ((m_beatIndex / 4) * 4 + barBeat + 15) % 16;
    beat();
}

void LivePadPanel::pressPad(int pad)
{
    if (pad < 0 || pad >= kLivePads)
        return;
    if (m_launch->currentIndex() == LaunchNow || m_auto->currentIndex() == AutoOff || !m_running || isMomentary(pad)) {
        emit padPressed(pad);
        return;
    }
    m_pendingPad = m_pendingPad == pad ? -1 : pad; // pressed again before it launched: cancelled
    refresh();
}

void LivePadPanel::audioBeat(double bpm, double confidence)
{
    (void)confidence;
    if (m_adoptHeard && bpm > 0) {
        m_adoptSteady = std::abs(bpm - m_adoptLast) < 0.5 ? m_adoptSteady + 1 : 0;
        m_adoptLast = bpm;
        if (m_adoptSteady >= 6) { // (the same tempo for 7 beats)
            m_adoptHeard = false;
            m_bpm->setValue(bpm); // (saved, the clock and Link follow)
            emit message(tr("Tempo found: %1 BPM").arg(bpm, 0, 'f', 2));
        }
    }
    if (m_auto->currentIndex() == AutoTempo && !linkOn() && bpm > 0 && m_lastBeatMs >= 0) {
        // Tempo keeps its BPM but stays in time with the music: a beat heard near one of its own moves the
        // beats onto it. Only when the music is at that tempo (or half or double it): another song leaves it be.
        const double ratio = bpm / std::max(30.0, m_bpm->value());
        if (std::abs(ratio - 1) < 0.04 || std::abs(ratio - 2) < 0.08 || std::abs(ratio - 0.5) < 0.02) {
            const double period = 60000.0 / std::max(30.0, m_bpm->value());
            const double since = double(m_clock.elapsed() - m_lastBeatMs);
            if (since < 0.25 * period) {
                restartBeatGrid(); // ours came a little early: the next ones a period from the heard one
            } else if (since > 0.75 * period && since < 1.25 * period) {
                restartBeatGrid(); // the heard one is a little before ours: it is the beat
                beat();
            }
        }
    }
    if (m_auto->currentIndex() == AutoTempo || m_auto->currentIndex() == AutoMidi || linkOn())
        return; // the typed / tapped tempo, the MIDI clock or Ableton Link drives the clock
    m_lastAudioBeatMs = m_clock.elapsed();
    if (bpm > 0 && m_auto->currentIndex() == AutoAudio) {
        QSignalBlocker block(m_bpm);
        m_bpm->setValue(bpm);
    }
    beat();
}

void LivePadPanel::setBeatTracker(BeatTracker* tracker)
{
    m_tracker = tracker;
    if (m_tracker) {
        m_tracker->setSettings(BeatTuner::loadSettings());
        if (m_lockBpm->isChecked())
            m_tracker->setTempoLocked(true, m_bpm->value());
    }
    if (m_tuner)
        m_tuner->setTracker(tracker);
}

void LivePadPanel::setTempoLocked(bool locked)
{
    if (m_lockBpm->isChecked() != locked) {
        QSignalBlocker block(m_lockBpm);
        m_lockBpm->setChecked(locked);
    }
    double bpm = m_bpm->value();
    if (m_tracker) {
        // Lock the tempo heard; if none has been heard yet, the one shown (typed or tapped).
        const double heard = m_tracker->bpm();
        m_tracker->setTempoLocked(locked, locked && heard <= 0 ? bpm : 0.0);
        if (locked && heard > 0) {
            bpm = heard;
            QSignalBlocker block(m_bpm);
            m_bpm->setValue(heard);
        }
    }
    m_bpm->setReadOnly((m_auto->currentIndex() == AutoAudio || m_auto->currentIndex() == AutoMidi) && !locked);
    emit message(locked ? tr("Tempo locked at %1 BPM").arg(bpm, 0, 'f', 2) : tr("Tempo unlocked: listening for it again"));
}

void LivePadPanel::findTempoAgain()
{
    if (m_tracker)
        m_tracker->forget(); // the tempo, the lock and an octave set with x2 / /2
    setTempoLocked(false);
    m_beatIndex = 15;
    // On Tempo (or Auto off) the heard tempo doesn't drive the clock: put it in the BPM box once it is steady.
    m_adoptHeard = m_auto->currentIndex() == AutoTempo || m_auto->currentIndex() == AutoOff;
    m_adoptSteady = 0;
    m_adoptLast = 0;
    emit message(m_adoptHeard ? tr("Listening for the tempo: the BPM follows once it is steady (Audio input)")
                              : tr("Listening for the tempo again"));
}

void LivePadPanel::setCuedPad(int pad)
{
    if (pad == m_cued)
        return;
    m_cued = pad;
    refresh();
}

void LivePadPanel::setSongTempo(double bpm, bool lock)
{
    if (bpm <= 0) {
        findTempoAgain();
        return;
    }
    if (m_tracker)
        m_tracker->forget(); // the last song's tempo, beat grid and octave
    {
        QSignalBlocker b1(m_lockBpm), b2(m_bpm);
        m_lockBpm->setChecked(lock);
        m_bpm->setValue(bpm);
    }
    if (m_tracker && lock)
        m_tracker->setTempoLocked(true, bpm);
    pushTempo();
    m_bpm->setReadOnly((m_auto->currentIndex() == AutoAudio || m_auto->currentIndex() == AutoMidi) && !lock);
    m_beatIndex = 15;
    updateAutoTimer();
    emit message(lock ? tr("Tempo locked at %1 BPM").arg(bpm, 0, 'f', 2) : tr("Tempo %1 BPM, listening from there").arg(bpm, 0, 'f', 2));
}

void LivePadPanel::showBeatTuner()
{
    if (!m_tuner) {
        m_tuner = new BeatTuner(this);
        m_tuner->setTracker(m_tracker);
        connect(m_tuner, &BeatTuner::tempoForgotten, this, [this] { findTempoAgain(); });
    }
    m_tuner->show();
    m_tuner->raise();
    m_tuner->activateWindow();
}

void LivePadPanel::beat()
{
    m_lastBeatMs = m_clock.elapsed();
    m_beatIndex = (m_beatIndex + 1) % 16; // beats since the 1, over four bars of four
    flash(m_beatIndex % 4 == 0);
    emit beatTicked(currentBpm(), m_beatIndex % 4);
    // A pad pressed early launches now (on the beat, or on the 1).
    if (m_pendingPad >= 0 && (m_launch->currentIndex() == LaunchBeat || m_beatIndex % 4 == 0)) {
        const int p = m_pendingPad;
        m_pendingPad = -1;
        emit padPressed(p);
        refresh();
        return; // (instead of an auto switch on this beat)
    }
    if (m_running && !m_sequencer->playing() && m_auto->currentIndex() != AutoOff && m_beatIndex % std::max(1, m_every->currentData().toInt()) == 0)
        step();
}

void LivePadPanel::flash(bool downbeat)
{
    theme::style(m_led, downbeat ? QStringLiteral("background: @key; border-radius: 7px;") : QStringLiteral("background: @good; border-radius: 7px;"));
    m_ledTimer.start(90);
}

int LivePadPanel::barBeat() const
{
    return m_beatIndex % 4 + 1;
}

double LivePadPanel::currentBpm() const
{
    if (linkOn())
        return m_bpm->value(); // (the session's)
    const bool audioBeats = m_auto->currentIndex() == AutoAudio || (m_auto->currentIndex() == AutoOff && m_clock.elapsed() - m_lastAudioBeatMs < 2000);
    if (audioBeats && m_tracker && m_tracker->bpm() > 0)
        return m_tracker->bpm();
    return m_bpm->value();
}

void LivePadPanel::scaleTempo(double factor)
{
    if (m_auto->currentIndex() == AutoAudio && m_tracker && m_tracker->bpm() > 0) {
        const double bpm = m_tracker->scaleTempo(factor); // locked or not: the octave stays put
        {
            QSignalBlocker block(m_bpm);
            m_bpm->setValue(bpm);
        }
        pushTempo();
        emit message(tr("Tempo %1 BPM").arg(bpm, 0, 'f', 2));
    } else {
        m_bpm->setValue(std::clamp(m_bpm->value() * factor, 30.0, 300.0));
    }
}

void LivePadPanel::setDownbeat()
{
    if (linkOn()) {
        emit message(tr("Ableton Link sets where the bars are (the session's 1)"));
        return;
    }
    const double period = 60000.0 / std::max(20.0, currentBpm());
    const qint64 now = m_clock.elapsed();
    // Beats well off where the 1 was pressed (on the off-beats, say) move to it.
    bool moved = false;
    if (m_auto->currentIndex() == AutoAudio && m_tracker) {
        moved = m_tracker->beatIsNow();
    } else if (m_auto->currentIndex() == AutoTempo && m_lastBeatMs >= 0) {
        const double since = std::fmod(double(now - m_lastBeatMs), period);
        if (since > 0.2 * period && since < 0.8 * period) {
            restartBeatGrid(); // the tempo's beats from here
            moved = true;
        }
    }
    const int every = std::max(1, m_every->currentData().toInt());
    const bool lastWasSwitch = m_beatIndex % every == 0;
    if (moved || (m_lastBeatMs >= 0 && double(now - m_lastBeatMs) < period / 2)) {
        // The beat just heard (or now) is the 1: switch now if that beat didn't.
        m_beatIndex = 0;
        m_lastBeatMs = now;
        flash(true);
        if (m_running && (moved || !lastWasSwitch))
            step();
    } else {
        m_beatIndex = 15; // the next beat is the 1
    }
    emit message(moved ? tr("The beats moved to where you pressed: that was the 1") : tr("Downbeat set: bars count from that 1"));
}

void LivePadPanel::step()
{
    std::vector<int> candidates;
    for (int i = 0; i < kLivePads; ++i)
        if (i != m_active && !m_project->timeline().livePads[size_t(i)].effects.empty() && !isMomentary(i)) // held ones aren't stepped to
            candidates.push_back(i);
    if (candidates.empty())
        return;
    int next = candidates.front();
    if (m_random->isChecked()) {
        next = candidates[size_t(QRandomGenerator::global()->bounded(int(candidates.size())))];
    } else {
        for (int c : candidates)
            if (c > m_active) {
                next = c;
                break;
            }
    }
    m_stepping = true; // (the device chain doesn't follow auto switch)
    emit padPressed(next);
    m_stepping = false;
}

bool LivePadPanel::followsActivePad() const
{
    return m_follow->isChecked();
}
