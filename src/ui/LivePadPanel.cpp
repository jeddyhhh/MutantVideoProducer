#include "ui/LivePadPanel.h"

#include "ui/Theme.h"

#include "core/BeatTracker.h"
#include "ui/BeatTuner.h"

#include "core/LivePadSets.h"
#include "core/RandomChain.h"
#include "ui/EffectsBrowser.h"

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

LivePadPanel::LivePadPanel(Project* project, QWidget* parent) : QWidget(parent), m_project(project)
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

    // ---- Auto switch ----
    form->addWidget(new QLabel(tr("Auto")), 2, 0);
    m_auto = new QComboBox;
    m_auto->addItems({tr("Off"), tr("Tempo"), tr("Audio beats"), tr("MIDI clock")});
    m_auto->setToolTip(tr("Switch pads by themselves while capturing: on a tempo (type the BPM or tap it), or on beats\n"
                          "heard on the Audio input. Uses the transition above; empty pads are skipped."));
    form->addWidget(m_auto, 2, 1);
    m_every = new QComboBox;
    for (int n : {1, 2, 4, 8, 16})
        m_every->addItem(n == 1 ? tr("every beat") : tr("every %1").arg(n), n);
    m_every->setToolTip(tr("Beats between switches"));
    form->addWidget(m_every, 2, 2);
    auto* tempoRow = new QHBoxLayout;
    tempoRow->setSpacing(4);
    m_bpm = new QDoubleSpinBox;
    m_bpm->setRange(30, 300);
    m_bpm->setDecimals(2);
    m_bpm->setSingleStep(0.01);
    m_bpm->setSuffix(tr(" BPM"));
    m_bpm->setToolTip(tr("Tempo (with Audio beats: the tempo heard). The arrows step 0.01, Page Up / Down 0.1."));
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
    form->addLayout(tempoRow, 3, 1, 1, 2);
    form->addLayout(adjustRow, 4, 1, 1, 2);
    auto* beatRow = new QHBoxLayout;
    beatRow->setSpacing(4);
    beatRow->addWidget(m_random);
    m_tune = new QToolButton;
    m_tune->setText(tr("Tune..."));
    m_tune->setToolTip(tr("Beat detection settings for Audio beats: what to listen to, sensitivity, tempo range, "
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
    tempoRow->addStretch(1);
    tempoRow->addWidget(m_led);
    beatRow->addStretch(1);
    form->addLayout(beatRow, 5, 1, 1, 2);
    form->addWidget(new QLabel(tr("Launch")), 6, 0);
    m_launch = new QComboBox;
    m_launch->addItems({tr("At once"), tr("On the next beat"), tr("On the next bar (the 1)")});
    m_launch->setToolTip(tr("When a pressed pad switches: straight away, or on the next beat or bar of the beat clock,\n"
                            "so switches land in time even if you press a little early (momentary pads are always at once)"));
    form->addWidget(m_launch, 6, 1, 1, 2);
    form->setColumnStretch(1, 1);
    layout->addLayout(form);
    layout->addStretch(1);

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
        save();
        updateAutoTimer();
        if (m_tracker && m_lockBpm->isChecked()) // typed while locked: the new tempo
            m_tracker->setTempoLocked(true, m_bpm->value());
    });
    connect(m_auto, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_beatIndex = 15;
        m_bpm->setReadOnly((m_auto->currentIndex() == AutoAudio || m_auto->currentIndex() == AutoMidi) && !m_lockBpm->isChecked());
        updateAutoTimer();
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
    connect(&m_beatTimer, &QTimer::timeout, this, [this] {
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
    m_ledTimer.setSingleShot(true);
    connect(&m_ledTimer, &QTimer::timeout, this, [this] { theme::style(m_led, QStringLiteral("background: @raised; border-radius: 7px;")); });
    theme::style(m_led, QStringLiteral("background: @raised; border-radius: 7px;"));
    m_clock.start();
    refreshSets(); // starts as "(not saved)": the project's pads may come from anywhere
    refresh();
    updateAutoTimer(); // the beat clock runs from the start (modulation follows it)
}

void LivePadPanel::setActivePad(int pad)
{
    m_active = pad;
    refresh();
}

void LivePadPanel::setEditing(int target)
{
    m_editing = std::clamp(target, 0, kLayerTarget);
    refresh();
}

void LivePadPanel::setHeldPads(const std::vector<int>& pads)
{
    m_heldPads = pads;
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
        m_edit->addItem(tr("Layer FX (the second source)"));
        m_edit->setCurrentIndex(m_editing);
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

int LivePadPanel::randomizeEmptyPads(int tier)
{
    Timeline tl = m_project->timeline();
    int filled = 0;
    for (int pad = 0; pad < kLivePads; ++pad) {
        LivePad& lp = tl.livePads[size_t(pad)];
        if (lp.locked || !lp.isEmpty())
            continue;
        const RandomChain chain = makeRandomChain(RandomTier(std::clamp(tier, 0, 3)), QRandomGenerator::global()->generate(),
                                                  [&tl] { return tl.allocId(); });
        lp.effects = chain.effects;
        lp.name = chain.name;
        ++filled;
    }
    if (filled)
        m_project->applyEdit(tl, tr("Random Effects Chains"));
    emit message(filled ? tr("Filled %1 empty pad(s) with %2 random chains").arg(filled).arg(randomTierName(RandomTier(tier)))
                        : tr("There are no empty pads to fill"));
    return filled;
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
    if (m_auto->currentIndex() == AutoOff || m_auto->currentIndex() == AutoTempo)
        m_beatTimer.start(int(std::lround(60000.0 / std::max(30.0, m_bpm->value()))));
    else
        m_beatTimer.stop();
    emit tempoChanged(currentBpm());
}

void LivePadPanel::tap()
{
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
    }
    // The tap is the beat: restart the tempo from here.
    m_beatIndex = 0;
    if (m_auto->currentIndex() == AutoTempo) {
        updateAutoTimer();
        beat();
    }
}

void LivePadPanel::clockBeat(double bpm, int barBeat)
{
    if (m_auto->currentIndex() != AutoMidi)
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
    if (m_launch->currentIndex() == LaunchNow || !m_running || isMomentary(pad)) {
        emit padPressed(pad);
        return;
    }
    m_pendingPad = m_pendingPad == pad ? -1 : pad; // pressed again before it launched: cancelled
    refresh();
}

void LivePadPanel::audioBeat(double bpm, double confidence)
{
    (void)confidence;
    if (m_auto->currentIndex() == AutoTempo || m_auto->currentIndex() == AutoMidi)
        return; // the typed / tapped tempo or the MIDI clock drives the clock
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
    emit message(tr("Listening for the tempo again"));
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
    if (m_running && m_auto->currentIndex() != AutoOff && m_beatIndex % std::max(1, m_every->currentData().toInt()) == 0)
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
    const bool audioBeats = m_auto->currentIndex() == AutoAudio || (m_auto->currentIndex() == AutoOff && m_clock.elapsed() - m_lastAudioBeatMs < 2000);
    if (audioBeats && m_tracker && m_tracker->bpm() > 0)
        return m_tracker->bpm();
    return m_bpm->value();
}

void LivePadPanel::scaleTempo(double factor)
{
    if (m_auto->currentIndex() == AutoAudio && m_tracker && m_tracker->bpm() > 0) {
        const double bpm = m_tracker->scaleTempo(factor); // locked or not: the octave stays put
        QSignalBlocker block(m_bpm);
        m_bpm->setValue(bpm);
        emit message(tr("Tempo %1 BPM").arg(bpm, 0, 'f', 2));
    } else {
        m_bpm->setValue(std::clamp(m_bpm->value() * factor, 30.0, 300.0));
    }
}

void LivePadPanel::setDownbeat()
{
    const double period = 60000.0 / std::max(20.0, currentBpm());
    const qint64 now = m_clock.elapsed();
    // Beats well off where the 1 was pressed (on the off-beats, say) move to it.
    bool moved = false;
    if (m_auto->currentIndex() == AutoAudio && m_tracker) {
        moved = m_tracker->beatIsNow();
    } else if (m_auto->currentIndex() == AutoTempo && m_lastBeatMs >= 0) {
        const double since = std::fmod(double(now - m_lastBeatMs), period);
        if (since > 0.2 * period && since < 0.8 * period) {
            m_beatTimer.start(); // the tempo's beats from here
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
