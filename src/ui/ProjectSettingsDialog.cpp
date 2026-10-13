#include "ui/ProjectSettingsDialog.h"

#include "ui/Help.h"
#include "ui/ExportDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

ProjectSettings settingsForMedia(const VideoStreamInfo& v, const ProjectSettings& base)
{
    ProjectSettings s = base;
    const QSize disp = v.displaySize();
    s.width = std::max(16, disp.width() & ~1);
    s.height = std::max(16, disp.height() & ~1);
    // Phones often report rates like 29.87 for variable-rate video; snap to
    // the nearest standard rate when close.
    const double fps = v.frameRate.toDouble();
    const FrameRate standard[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
                                  {50, 1},       {60000, 1001}, {60, 1}, {15, 1}, {20, 1}, {12, 1}, {10, 1}};
    FrameRate best = v.frameRate;
    double bestErr = 0.03;
    for (const FrameRate& r : standard) {
        const double err = std::abs(r.toDouble() - fps) / fps;
        if (err < bestErr) {
            bestErr = err;
            best = r;
        }
    }
    s.fps = best.valid() ? best : base.fps;
    return s;
}

ProjectSettingsDialog::ProjectSettingsDialog(const ProjectSettings& current, const MediaMap& media, QWidget* parent)
    : QDialog(parent), m_base(current)
{
    setWindowTitle(tr("Project Properties"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;

    auto* sizeRow = new QHBoxLayout;
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (QSpinBox* s : {m_width, m_height}) {
        s->setRange(16, 16384);
        s->setSingleStep(2);
    }
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel(QStringLiteral("x")));
    sizeRow->addWidget(m_height);
    sizeRow->addStretch();
    form->addRow(tr("Frame size:"), sizeRow);

    auto* presets = new QComboBox;
    presets->addItem(tr("Presets..."));
    const struct {
        const char* name;
        int w, h;
    } sizes[] = {{"4K UHD 3840x2160", 3840, 2160}, {"1440p 2560x1440", 2560, 1440}, {"1080p 1920x1080", 1920, 1080},
                 {"720p 1280x720", 1280, 720},     {"Vertical 1080x1920", 1080, 1920}, {"Square 1080x1080", 1080, 1080},
                 {"NTSC DV 720x480", 720, 480},   {"PAL 720x576", 720, 576},          {"3DS 400x240 (top screen)", 400, 240}};
    for (const auto& s : sizes)
        presets->addItem(QString::fromLatin1(s.name), QSize(s.w, s.h));
    form->addRow(QString(), presets);
    connect(presets, qOverload<int>(&QComboBox::activated), this, [this, presets](int i) {
        const QSize s = presets->itemData(i).toSize();
        if (s.isValid()) {
            m_width->setValue(s.width());
            m_height->setValue(s.height());
        }
    });

    m_fps = new QComboBox;
    m_fps->setEditable(true);
    for (const char* r : {"23.976", "24", "25", "29.970", "30", "48", "50", "59.940", "60", "120", "15", "20"})
        m_fps->addItem(QString::fromLatin1(r));
    form->addRow(tr("Frame rate:"), m_fps);

    m_sampleRate = new QComboBox;
    for (int r : {32000, 44100, 48000, 96000})
        m_sampleRate->addItem(QStringLiteral("%1 Hz").arg(r), r);
    form->addRow(tr("Audio sample rate:"), m_sampleRate);

    auto* match = new QComboBox;
    match->addItem(tr("Match media settings..."));
    for (const auto& [id, m] : media)
        if (m->info.hasVideo())
            match->addItem(m->displayName(), id);
    match->setEnabled(match->count() > 1);
    form->addRow(QString(), match);
    connect(match, qOverload<int>(&QComboBox::activated), this, [this, match, media](int i) {
        auto it = media.find(match->itemData(i).toInt());
        if (it != media.end() && it->second->info.hasVideo())
            load(settingsForMedia(it->second->info.video.front(), settings()));
    });

    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("options.md#elsewhere"));
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    load(current);
}

void ProjectSettingsDialog::load(const ProjectSettings& s)
{
    m_width->setValue(s.width);
    m_height->setValue(s.height);
    m_fps->setCurrentText(frameRateText(s.fps));
    const int i = m_sampleRate->findData(s.sampleRate);
    m_sampleRate->setCurrentIndex(i >= 0 ? i : m_sampleRate->findData(48000));
}

ProjectSettings ProjectSettingsDialog::settings() const
{
    ProjectSettings s = m_base;
    s.width = m_width->value() & ~1;
    s.height = m_height->value() & ~1;
    s.fps = parseFrameRate(m_fps->currentText(), m_base.fps);
    s.sampleRate = m_sampleRate->currentData().toInt();
    return s;
}
