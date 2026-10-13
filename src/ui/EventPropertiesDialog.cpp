#include "ui/EventPropertiesDialog.h"

#include "ui/Theme.h"

#include "ui/Help.h"
#include "core/TimelineOps.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QVBoxLayout>

EventPropertiesDialog::EventPropertiesDialog(Project* project, std::set<int> clipIds, QWidget* parent)
    : QDialog(parent), m_project(project), m_ids(std::move(clipIds))
{
    setWindowTitle(tr("Event Properties"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    const Clip* first = nullptr;
    bool anyVideo = false, anyAudio = false;
    for (const auto& t : m_project->timeline().tracks)
        for (const auto& c : t.clips)
            if (m_ids.count(c.id)) {
                if (!first)
                    first = &c;
                (isVisualTrack(t.type) ? anyVideo : anyAudio) = true;
            }

    m_rate = new QDoubleSpinBox;
    m_rate->setRange(ops::kMinSpeed * 100, ops::kMaxSpeed * 100);
    m_rate->setDecimals(1);
    m_rate->setSuffix(QStringLiteral(" %"));
    m_rate->setSingleStep(5);
    form->addRow(tr("Playback rate:"), m_rate);
    m_reverse = new QCheckBox(tr("Play in reverse"));
    form->addRow(QString(), m_reverse);
    m_keepSpan = new QCheckBox(tr("Keep the same footage (change event length)"));
    m_keepSpan->setChecked(true);
    m_keepSpan->setToolTip(tr("Off: the event keeps its length and shows more or less of the source."));
    form->addRow(QString(), m_keepSpan);
    m_blend = new QCheckBox(tr("Blend frames (smoother slow motion)"));
    m_blend->setEnabled(anyVideo);
    form->addRow(tr("Video:"), m_blend);
    m_pitch = new QCheckBox(tr("Preserve pitch when the speed changes"));
    m_pitch->setEnabled(anyAudio);
    form->addRow(tr("Audio:"), m_pitch);
    m_info = new QLabel;
    theme::style(m_info, QStringLiteral("color: @dim;"));
    form->addRow(QString(), m_info);
    layout->addLayout(form);

    if (first) {
        m_rate->setValue(first->speed * 100);
        m_reverse->setChecked(first->reverse);
        m_blend->setChecked(first->frameBlend);
        m_pitch->setChecked(first->preservePitch);
        m_length = first->length;
        m_span = first->sourceSpan();
        m_oldSpeed = first->speed;
    }

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("speed.md#playback-speed"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    connect(m_rate, qOverload<double>(&QDoubleSpinBox::valueChanged), this, &EventPropertiesDialog::updateInfo);
    connect(m_keepSpan, &QCheckBox::toggled, this, &EventPropertiesDialog::updateInfo);
    updateInfo();
}

void EventPropertiesDialog::updateInfo()
{
    const double speed = m_rate->value() / 100.0;
    // Same footage: the length scales with the rate (a speed ramp stretches along).
    const TimeUs newLength = m_keepSpan->isChecked() ? TimeUs(double(m_length) * m_oldSpeed / speed) : m_length;
    m_info->setText(tr("Event length: %1 → %2").arg(formatDuration(m_length), formatDuration(newLength)));
}

void EventPropertiesDialog::apply()
{
    Timeline tl = m_project->timeline();
    const FrameRate fps = m_project->settings().fps;
    const double speed = m_rate->value() / 100.0;
    for (auto& t : tl.tracks)
        for (auto& c : t.clips) {
            if (!m_ids.count(c.id))
                continue;
            if (std::abs(c.speed - speed) > 1e-9 && !c.isGenerated()) {
                auto media = m_project->mediaItem(c.mediaId);
                ops::setClipSpeed(c, media.get(), speed, m_keepSpan->isChecked(), fps);
            }
            c.reverse = m_reverse->isChecked();
            if (isVisualTrack(t.type))
                c.frameBlend = m_blend->isChecked();
            else
                c.preservePitch = m_pitch->isChecked();
        }
    tl.sortClips();
    m_project->applyEdit(tl, tr("Event Properties"));
}
