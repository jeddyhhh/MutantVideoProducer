#include "ui/ModulationDialog.h"

#include "ui/Theme.h"

#include "ui/Help.h"
#include "core/LiveModulation.h"
#include "ui/MidiHub.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

namespace {
const double kLengths[] = {0.0625, 0.125, 0.25, 0.5, 1, 2, 4, 8, 16};

QString lengthName(double beats)
{
    if (beats < 1)
        return ModulationDialog::tr("1/%1 beat").arg(int(std::lround(1 / beats)));
    if (beats == 1)
        return ModulationDialog::tr("1 beat");
    return ModulationDialog::tr("%1 beats (%2)").arg(beats).arg(beats == 4 ? ModulationDialog::tr("a bar") : ModulationDialog::tr("%1 bars").arg(beats / 4));
}

QWidget* row(QWidget* a, QWidget* b = nullptr)
{
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(a);
    if (b)
        l->addWidget(b);
    l->addStretch(1);
    return w;
}
} // namespace

ModulationDialog::ModulationDialog(const ParamDef& def, const std::optional<Modulation>& current, QWidget* parent)
    : QDialog(parent), m_def(def)
{
    setWindowTitle(tr("Modulate %1").arg(def.name));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    m_source = new QComboBox;
    for (int s = 0; s <= int(Modulation::Source::Mids); ++s)
        m_source->addItem(modulationSourceName(Modulation::Source(s)));
    form->addRow(tr("Moved by"), m_source);

    m_shape = new QComboBox;
    for (int s = 0; s <= int(Modulation::Shape::Random); ++s)
        m_shape->addItem(modulationShapeName(Modulation::Shape(s)));
    m_shapeRow = row(m_shape);
    form->addRow(tr("Shape"), m_shapeRow);

    m_length = new QComboBox;
    for (double b : kLengths)
        m_length->addItem(lengthName(b), b);
    m_lengthRow = row(m_length);
    form->addRow(tr("Length"), m_lengthRow);

    m_amount = new QDoubleSpinBox;
    m_amount->setRange(-100, 100);
    m_amount->setDecimals(0);
    m_amount->setSuffix(QStringLiteral(" %"));
    m_amount->setToolTip(tr("How far the setting moves from where its knob is, as a share of its whole range (negative: down)"));
    m_amountRow = row(m_amount);
    form->addRow(tr("Amount"), m_amountRow);

    m_channel = new QSpinBox;
    m_channel->setRange(1, 16);
    m_channel->setPrefix(tr("channel "));
    m_cc = new QSpinBox;
    m_cc->setRange(0, 127);
    m_cc->setPrefix(tr("CC "));
    m_learn = new QToolButton;
    m_learn->setText(tr("Learn"));
    m_learn->setCheckable(true);
    m_learn->setToolTip(tr("Move a knob or fader on your controller to pick it (choose the controller in the Live tab's MIDI list)"));
    auto* midi = new QWidget;
    auto* ml = new QHBoxLayout(midi);
    ml->setContentsMargins(0, 0, 0, 0);
    ml->addWidget(m_channel);
    ml->addWidget(m_cc);
    ml->addWidget(m_learn);
    ml->addStretch(1);
    m_midiRow = midi;
    form->addRow(tr("Controller"), m_midiRow);
    layout->addLayout(form);

    m_help = new QLabel;
    m_help->setWordWrap(true);
    theme::style(m_help, QStringLiteral("color: @dim;"));
    m_help->setMinimumWidth(360);
    layout->addWidget(m_help);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("live.md#modulation"));
    QPushButton* remove = buttons->addButton(tr("No Modulation"), QDialogButtonBox::ResetRole);
    remove->setEnabled(current.has_value());
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        m_result = this->current();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(remove, &QPushButton::clicked, this, [this] {
        m_result.reset();
        accept();
    });

    const Modulation m = current.value_or(Modulation());
    m_source->setCurrentIndex(int(m.source));
    m_shape->setCurrentIndex(int(m.shape));
    int length = 4;
    for (int i = 0; i < int(std::size(kLengths)); ++i)
        if (std::abs(kLengths[i] - m.beats) < 1e-6)
            length = i;
    m_length->setCurrentIndex(length);
    m_amount->setValue(m.amount * 100);
    m_channel->setValue(m.channel + 1);
    m_cc->setValue(std::max(0, m.cc));
    m_result = current;
    connect(m_source, qOverload<int>(&QComboBox::currentIndexChanged), this, &ModulationDialog::updateRows);
    connect(MidiHub::instance(), &MidiHub::message, this, [this](const MidiMessage& msg) {
        if (!m_learn->isChecked() || !msg.isController())
            return;
        m_channel->setValue(msg.channel() + 1);
        m_cc->setValue(msg.data1);
        m_learn->setChecked(false);
        if (isGamePad(MidiHub::instance()->current()))
            m_help->setText(tr("The %1 sets the whole range of %2 (0 at rest).").arg(gamePadControlName(msg).toLower(), m_def.name));
    });
    connect(m_learn, &QToolButton::toggled, this, [this](bool on) {
        if (on && MidiHub::instance()->current().isEmpty())
            m_help->setText(tr("Choose your MIDI controller in the Live tab's MIDI list first."));
        else
            updateRows();
    });
    updateRows();
}

Modulation ModulationDialog::current() const
{
    Modulation m;
    m.source = Modulation::Source(m_source->currentIndex());
    m.shape = Modulation::Shape(m_shape->currentIndex());
    m.beats = m_length->currentData().toDouble();
    m.amount = m_amount->value() / 100.0;
    m.channel = m_channel->value() - 1;
    m.cc = m_cc->value();
    return m;
}

void ModulationDialog::updateRows()
{
    using S = Modulation::Source;
    const S s = S(m_source->currentIndex());
    m_shapeRow->setEnabled(s == S::Lfo);
    m_lengthRow->setEnabled(s == S::Lfo || s == S::Beat || s == S::Bar);
    m_amountRow->setEnabled(s != S::Midi);
    m_midiRow->setEnabled(s == S::Midi);
    switch (s) {
    case S::Beat: m_help->setText(tr("Jumps on every beat and falls back over the length (a beat at most).")); break;
    case S::Bar: m_help->setText(tr("Jumps on the 1 of every bar (set it with Set 1) and falls back over the length (a bar at most).")); break;
    case S::Lfo: m_help->setText(tr("A wave in time with the tempo: one cycle per length, starting on a beat.")); break;
    case S::Level: m_help->setText(tr("Follows how loud the Audio input is (scaled to the loudest of the last few seconds).")); break;
    case S::Bass: m_help->setText(tr("Follows the bass (kick drums, bass lines) of the Audio input.")); break;
    case S::Highs: m_help->setText(tr("Follows the highs (hi-hats, cymbals, \"s\" sounds) of the Audio input.")); break;
    case S::Mids: m_help->setText(tr("Follows the mids (snares, claps, voices, guitars) of the Audio input.")); break;
    case S::Midi:
        m_help->setText(tr("A knob or fader on your MIDI controller (or a trigger or stick direction on an Xbox controller) sets the whole range "
                           "of %1.").arg(m_def.name));
        break;
    }
}
