#include "ui/ExportDialog.h"

#include "ui/Theme.h"

#include "ui/Help.h"
#include "core/FormatRegistry.h"
#include "core/Hardware.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSettings>
#include <QStandardItemModel>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <cmath>

FrameRate parseFrameRate(const QString& text, FrameRate fallback)
{
    const QString t = text.trimmed();
    if (t.contains(QLatin1Char('/'))) {
        const auto parts = t.split(QLatin1Char('/'));
        const int n = parts.value(0).toInt(), d = parts.value(1).toInt();
        return (n > 0 && d > 0) ? FrameRate{n, d} : fallback;
    }
    bool ok = false;
    const double v = t.toDouble(&ok);
    if (!ok || v <= 0 || v > 1000)
        return fallback;
    // NTSC-style rates.
    for (int base : {24, 30, 60, 120})
        if (std::abs(v - base * 1000.0 / 1001.0) < 0.01)
            return FrameRate{base * 1000, 1001};
    if (std::abs(v - std::round(v)) < 1e-6)
        return FrameRate{int(std::round(v)), 1};
    return FrameRate{int(std::round(v * 1000)), 1000};
}

QString frameRateText(const FrameRate& fps)
{
    if (fps.den == 1001)
        return QString::number(fps.toDouble(), 'f', 3);
    if (fps.den == 1)
        return QString::number(fps.num);
    return QStringLiteral("%1/%2").arg(fps.num).arg(fps.den);
}

namespace {

void fillEncoders(QComboBox* box, const std::vector<formats::EncoderInfo>& list, const QString& keep)
{
    box->blockSignals(true);
    box->clear();
    for (const auto& e : list) {
        QString label = QStringLiteral("%1 - %2").arg(e.name, e.longName);
        if (e.isDefault)
            label += QStringLiteral("  [default]");
        if (e.experimental)
            label += QStringLiteral("  [experimental]");
        if (hw::isHardwareEncoder(e.name))
            label += hw::encoderUsable(e.name) ? QStringLiteral("  [GPU]") : QStringLiteral("  [GPU - not available on this PC]");
        box->addItem(label, e.name);
    }
    const int idx = box->findData(keep);
    box->setCurrentIndex(idx >= 0 ? idx : 0);
    box->blockSignals(false);
}

} // namespace

ExportDialog::ExportDialog(const ProjectSettings& project, TimeUs projectDuration, bool hasRegion, TimeUs regionStart,
                           TimeUs regionEnd, const QString& suggestedPath, QWidget* parent)
    : QDialog(parent), m_project(project), m_duration(projectDuration), m_regionStart(regionStart), m_regionEnd(regionEnd)
{
    setWindowTitle(tr("Render As"));
    setMinimumWidth(620);
    auto* layout = new QVBoxLayout(this);

    auto* top = new QFormLayout;
    auto* templateRow = new QHBoxLayout;
    m_preset = new QComboBox;
    m_preset->setMaxVisibleItems(30);
    m_preset->setMinimumWidth(380);
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search templates (e.g. tiktok, discord, prores, psp)"));
    m_search->setClearButtonEnabled(true);
    m_search->setObjectName(QStringLiteral("templateSearch"));
    templateRow->addWidget(m_preset, 2);
    templateRow->addWidget(m_search, 1);
    top->addRow(tr("Template:"), templateRow);
    m_notes = new QLabel;
    m_notes->setWordWrap(true);
    theme::style(m_notes, QStringLiteral("color: @dim;"));
    top->addRow(QString(), m_notes);
    m_useGpu = new QCheckBox(tr("Encode on the GPU when possible (much faster for H.264 / HEVC / AV1)"));
    bool anyGpu = false;
    for (const char* e : {"h264_nvenc", "h264_qsv", "h264_amf"})
        anyGpu |= hw::encoderUsable(QString::fromLatin1(e));
    m_useGpu->setEnabled(anyGpu);
    m_useGpu->setChecked(anyGpu && QSettings().value(QStringLiteral("export/useGpu"), true).toBool());
    if (!anyGpu)
        m_useGpu->setToolTip(tr("No hardware video encoder works on this PC"));
    top->addRow(QString(), m_useGpu);
    m_subtitles = new QCheckBox;
    m_subtitles->setToolTip(tr("The text of the Captions track as subtitles viewers can turn on or off (MP4, MOV, MKV and WebM)"));
    m_subtitles->setChecked(QSettings().value(QStringLiteral("export/subtitles"), true).toBool());
    m_subtitles->hide();
    top->addRow(QString(), m_subtitles);

    auto* pathRow = new QHBoxLayout;
    m_path = new QLineEdit(suggestedPath);
    auto* browseBtn = new QPushButton(tr("Browse..."));
    pathRow->addWidget(m_path, 1);
    pathRow->addWidget(browseBtn);
    top->addRow(tr("Output file:"), pathRow);

    m_muxer = new QComboBox;
    m_muxer->setMaxVisibleItems(25);
    for (const auto& m : formats::muxers()) {
        QString label = QStringLiteral("%1 (%2)").arg(m.longName, m.name);
        if (!m.extensions.isEmpty())
            label += QStringLiteral("  *.%1").arg(m.extensions.join(QStringLiteral(", *.")));
        m_muxer->addItem(label, m.name);
    }
    top->addRow(tr("Format:"), m_muxer);
    m_muxerOptions = new QLineEdit;
    m_muxerOptions->setPlaceholderText(tr("FFmpeg format options, e.g. hls_time=6"));
    top->addRow(tr("Format options:"), m_muxerOptions);
    layout->addLayout(top);

    // Video
    m_videoBox = new QGroupBox(tr("Video"));
    m_videoBox->setCheckable(true);
    auto* vf = new QFormLayout(m_videoBox);
    m_videoCodec = new QComboBox;
    m_videoCodec->setMaxVisibleItems(25);
    vf->addRow(tr("Codec:"), m_videoCodec);
    auto* sizeRow = new QHBoxLayout;
    m_width = new QSpinBox;
    m_height = new QSpinBox;
    for (QSpinBox* s : {m_width, m_height}) {
        s->setRange(16, 16384);
        s->setSingleStep(2);
    }
    m_width->setValue(project.width);
    m_height->setValue(project.height);
    sizeRow->addWidget(m_width);
    sizeRow->addWidget(new QLabel(QStringLiteral("x")));
    sizeRow->addWidget(m_height);
    auto* projSize = new QPushButton(tr("Project size"));
    sizeRow->addWidget(projSize);
    sizeRow->addStretch();
    vf->addRow(tr("Frame size:"), sizeRow);
    m_fps = new QComboBox;
    m_fps->setEditable(true);
    for (const char* r : {"23.976", "24", "25", "29.970", "30", "48", "50", "59.940", "60", "120", "15", "20", "12"})
        m_fps->addItem(QString::fromLatin1(r));
    m_fps->setCurrentText(frameRateText(project.fps));
    vf->addRow(tr("Frame rate:"), m_fps);
    m_quality = new QSpinBox;
    m_quality->setRange(-1, 63);
    m_quality->setSpecialValueText(tr("Encoder default"));
    m_quality->setToolTip(tr("Constant quality: CRF for x264/x265/VP9/AV1 (lower = better), "
                             "q-scale 1-31 for MPEG-family codecs"));
    vf->addRow(tr("Quality (CRF / q):"), m_quality);
    m_videoBitrate = new QSpinBox;
    m_videoBitrate->setRange(0, 1000000);
    m_videoBitrate->setSuffix(tr(" kb/s"));
    m_videoBitrate->setSpecialValueText(tr("Auto"));
    vf->addRow(tr("Bitrate:"), m_videoBitrate);
    m_encoderPreset = new QLineEdit;
    m_encoderPreset->setPlaceholderText(tr("e.g. medium, slow (x264/x265) or 0-13 (SVT-AV1)"));
    vf->addRow(tr("Encoder preset:"), m_encoderPreset);
    m_videoOptions = new QLineEdit;
    m_videoOptions->setPlaceholderText(tr("FFmpeg encoder options, e.g. profile=high:tune=film"));
    vf->addRow(tr("Extra options:"), m_videoOptions);
    m_pixFmt = new QComboBox;
    m_pixFmt->setEditable(true);
    m_pixFmt->addItem(tr("Automatic"), QString());
    for (const char* f : {"yuv420p", "yuv420p10le", "yuv422p", "yuv422p10le", "yuv444p", "yuv444p10le", "nv12", "rgb24", "rgb48be",
                          "bgr0", "gbrp", "gbrp10le"})
        m_pixFmt->addItem(QString::fromLatin1(f), QString::fromLatin1(f));
    m_pixFmt->setToolTip(tr("Pixel format of the encoded video (10-bit avoids banding; 4:2:2 / 4:4:4 keep more colour)"));
    vf->addRow(tr("Pixel format:"), m_pixFmt);
    m_framing = new QComboBox;
    m_framing->addItem(tr("Fit (black bars)"), int(Framing::Fit));
    m_framing->addItem(tr("Fit on a blurred copy of the picture"), int(Framing::BlurFit));
    m_framing->addItem(tr("Fill (crop the edges)"), int(Framing::Fill));
    m_framing->addItem(tr("Stretch"), int(Framing::Stretch));
    m_framing->setToolTip(tr("Used when the frame size has another shape than the project, e.g. a vertical 9:16 template "
                             "for a 16:9 project"));
    vf->addRow(tr("Other shapes:"), m_framing);
    m_targetSize = new QDoubleSpinBox;
    m_targetSize->setRange(0, 100000);
    m_targetSize->setDecimals(1);
    m_targetSize->setSuffix(tr(" MB"));
    m_targetSize->setSpecialValueText(tr("Off"));
    m_targetSize->setToolTip(tr("Choose the video bitrate so the whole file fits this size (messaging apps, email)"));
    vf->addRow(tr("Fit file into:"), m_targetSize);
    layout->addWidget(m_videoBox);

    // Audio
    m_audioBox = new QGroupBox(tr("Audio"));
    m_audioBox->setCheckable(true);
    auto* af = new QFormLayout(m_audioBox);
    m_audioCodec = new QComboBox;
    m_audioCodec->setMaxVisibleItems(25);
    af->addRow(tr("Codec:"), m_audioCodec);
    m_sampleRate = new QComboBox;
    for (int r : {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000})
        m_sampleRate->addItem(QStringLiteral("%1 Hz").arg(r), r);
    m_sampleRate->setCurrentIndex(m_sampleRate->findData(48000));
    af->addRow(tr("Sample rate:"), m_sampleRate);
    m_channels = new QComboBox;
    m_channels->addItem(tr("Stereo"), 2);
    m_channels->addItem(tr("Mono"), 1);
    af->addRow(tr("Channels:"), m_channels);
    m_audioBitrate = new QSpinBox;
    m_audioBitrate->setRange(0, 4000);
    m_audioBitrate->setSuffix(tr(" kb/s"));
    m_audioBitrate->setSpecialValueText(tr("Auto"));
    af->addRow(tr("Bitrate:"), m_audioBitrate);
    layout->addWidget(m_audioBox);

    // Range
    auto* rangeBox = new QGroupBox(tr("Render range"));
    auto* rl = new QHBoxLayout(rangeBox);
    m_rangeAll = new QRadioButton(tr("Entire project (%1)").arg(formatDuration(projectDuration)));
    m_rangeRegion = new QRadioButton(hasRegion ? tr("Loop region only (%1)").arg(formatDuration(regionEnd - regionStart))
                                               : tr("Loop region only"));
    m_rangeRegion->setEnabled(hasRegion);
    (hasRegion ? m_rangeRegion : m_rangeAll)->setChecked(true);
    rl->addWidget(m_rangeAll);
    rl->addWidget(m_rangeRegion);
    rl->addStretch();
    layout->addWidget(rangeBox);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("rendering.md"));
    auto* renderBtn = buttons->addButton(tr("Render"), QDialogButtonBox::AcceptRole);
    renderBtn->setDefault(true);
    layout->addWidget(buttons);

    connect(m_preset, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int row) {
        const int index = m_preset->itemData(row).toInt();
        if (index >= -1)
            applyPreset(index);
    });
    connect(m_search, &QLineEdit::textChanged, this, &ExportDialog::fillTemplates);
    connect(m_useGpu, &QCheckBox::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("export/useGpu"), on);
        updateNotes();
    });
    connect(m_subtitles, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("export/subtitles"), on); });
    for (QRadioButton* r : {m_rangeAll, m_rangeRegion})
        connect(r, &QRadioButton::toggled, this, &ExportDialog::updateNotes);
    connect(m_videoCodec, qOverload<int>(&QComboBox::currentIndexChanged), this, &ExportDialog::updateNotes);
    connect(m_muxer, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        onMuxerChanged();
        if (const auto* m = formats::findMuxer(m_muxer->currentData().toString()); m && !m->extensions.isEmpty())
            fixExtension(m->extensions.front());
        m_preset->blockSignals(true);
        m_preset->setCurrentIndex(m_preset->count() - 1); // Custom
        m_preset->blockSignals(false);
    });
    connect(browseBtn, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(projSize, &QPushButton::clicked, this, [this] {
        m_width->setValue(m_project.width);
        m_height->setValue(m_project.height);
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::validateAndAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // Last template used (by name), else the first.
    const QString last = QSettings().value(QStringLiteral("export/template")).toString();
    int start = 0;
    for (size_t i = 0; i < exportPresets().size(); ++i)
        if (exportPresets()[i].name == last && presetAvailable(exportPresets()[i]))
            start = int(i);
    m_presetIndex = start;
    fillTemplates();
    applyPreset(start);
}

ExportDialog::~ExportDialog()
{
    // Children are deleted one by one: a radio button losing its partner toggles the other,
    // which must not reach updateNotes() once the notes label is gone.
    for (QWidget* w : findChildren<QWidget*>())
        w->blockSignals(true);
}

void ExportDialog::fillTemplates()
{
    const QString filter = m_search->text().trimmed();
    QSignalBlocker block(m_preset);
    m_preset->clear();
    auto* model = qobject_cast<QStandardItemModel*>(m_preset->model());
    QString category;
    int select = -1;
    const auto& presets = exportPresets();
    for (size_t i = 0; i < presets.size(); ++i) {
        const ExportPreset& p = presets[i];
        if (!presetAvailable(p))
            continue;
        if (!filter.isEmpty() && !p.name.contains(filter, Qt::CaseInsensitive) && !p.category.contains(filter, Qt::CaseInsensitive)
            && !p.notes.contains(filter, Qt::CaseInsensitive) && !p.videoEncoder.contains(filter, Qt::CaseInsensitive))
            continue;
        if (p.category != category) {
            category = p.category;
            m_preset->addItem(category, -2); // header
            if (model) {
                QStandardItem* item = model->item(m_preset->count() - 1);
                item->setEnabled(false);
                QFont f = item->font();
                f.setBold(true);
                item->setFont(f);
            }
        }
        m_preset->addItem(QStringLiteral("    ") + p.name, int(i));
        if (int(i) == m_presetIndex)
            select = m_preset->count() - 1;
    }
    m_preset->addItem(tr("Custom"), -1);
    if (select < 0 && !filter.isEmpty()) {
        // Searching: take the first match.
        for (int i = 0; i < m_preset->count(); ++i)
            if (m_preset->itemData(i).toInt() >= 0) {
                m_preset->setCurrentIndex(i);
                applyPreset(m_preset->itemData(i).toInt());
                return;
            }
    }
    if (select < 0) // nothing matches / custom settings: keep them
        select = m_preset->count() - 1;
    m_preset->setCurrentIndex(select);
}

TimeUs ExportDialog::renderLength() const
{
    return m_rangeRegion->isChecked() ? m_regionEnd - m_regionStart : m_duration;
}

void ExportDialog::updateNotes()
{
    QStringList lines;
    const auto& presets = exportPresets();
    const ExportPreset* p = m_presetIndex >= 0 && m_presetIndex < int(presets.size()) ? &presets[size_t(m_presetIndex)] : nullptr;
    if (p && !p->notes.isEmpty())
        lines << p->notes;
    if (p && p->maxSeconds > 0 && renderLength() > TimeUs(p->maxSeconds) * kUsPerSecond)
        lines << theme::css(QStringLiteral("<span style='color:@warn'>%1</span>"))
                     .arg(tr("This render is %1 long; %2 accepts up to %3.")
                              .arg(formatDuration(renderLength()), p->name.section(QLatin1Char(' '), 0, 0), formatDuration(TimeUs(p->maxSeconds) * kUsPerSecond)));
    if (m_useGpu->isChecked() && m_videoBox->isChecked()) {
        ExportSettings s;
        s.videoEncoder = m_videoCodec->currentData().toString();
        QString used;
        withGpuEncoder(s, &used);
        if (!used.isEmpty())
            lines << tr("GPU encoding with %1.").arg(used);
    }
    m_notes->setText(lines.join(QStringLiteral("<br>")));
    m_notes->setVisible(!lines.isEmpty());
}

void ExportDialog::onMuxerChanged()
{
    const QString mux = m_muxer->currentData().toString();
    const auto video = formats::encodersFor(mux, true);
    const auto audio = formats::encodersFor(mux, false);
    fillEncoders(m_videoCodec, video, m_videoCodec->currentData().toString());
    fillEncoders(m_audioCodec, audio, m_audioCodec->currentData().toString());
    m_videoBox->setEnabled(!video.empty());
    m_audioBox->setEnabled(!audio.empty());
    if (video.empty())
        m_videoBox->setChecked(false);
    if (audio.empty())
        m_audioBox->setChecked(false);
}

void ExportDialog::applyPreset(int index)
{
    const auto& presets = exportPresets();
    m_presetIndex = index;
    if (index < 0 || index >= int(presets.size())) {
        updateNotes();
        return; // "Custom": leave everything as is
    }
    const ExportPreset& p = presets[size_t(index)];
    const ExportSettings s = settingsFromPreset(p, m_project);
    QSettings().setValue(QStringLiteral("export/template"), p.name);
    m_gopSeconds = p.gopSeconds;
    const int pix = m_pixFmt->findData(p.pixelFormat);
    if (pix >= 0)
        m_pixFmt->setCurrentIndex(pix);
    else
        m_pixFmt->setEditText(p.pixelFormat);
    m_framing->setCurrentIndex(std::max(0, m_framing->findData(int(p.framing))));
    m_targetSize->setValue(p.targetSizeMB);
    m_muxerOptions->setText(p.muxerOptions);

    m_muxer->blockSignals(true);
    m_muxer->setCurrentIndex(std::max(0, m_muxer->findData(p.muxer)));
    m_muxer->blockSignals(false);
    onMuxerChanged();

    m_videoBox->setChecked(!p.videoEncoder.isEmpty());
    if (!p.videoEncoder.isEmpty()) {
        const int i = m_videoCodec->findData(p.videoEncoder);
        if (i >= 0)
            m_videoCodec->setCurrentIndex(i);
    }
    m_audioBox->setChecked(!p.audioEncoder.isEmpty());
    if (!p.audioEncoder.isEmpty()) {
        const int i = m_audioCodec->findData(p.audioEncoder);
        if (i >= 0)
            m_audioCodec->setCurrentIndex(i);
    }
    m_width->setValue(s.width);
    m_height->setValue(s.height);
    m_fps->setCurrentText(frameRateText(s.fps));
    m_quality->setValue(s.quality);
    m_videoBitrate->setValue(int(s.videoBitrate / 1000));
    m_encoderPreset->setText(s.encoderPreset);
    m_videoOptions->setText(s.videoOptions);
    const int sr = m_sampleRate->findData(s.sampleRate);
    if (sr >= 0)
        m_sampleRate->setCurrentIndex(sr);
    m_channels->setCurrentIndex(s.channels == 1 ? 1 : 0);
    m_audioBitrate->setValue(int(s.audioBitrate / 1000));
    fixExtension(p.extension);
    updateNotes();
}

void ExportDialog::fixExtension(const QString& ext)
{
    QString path = m_path->text().trimmed();
    if (path.isEmpty() || ext.isEmpty())
        return;
    const QFileInfo fi(path);
    path = fi.dir().filePath(fi.completeBaseName() + QLatin1Char('.') + ext);
    m_path->setText(QDir::toNativeSeparators(path));
}

void ExportDialog::browse()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Render As"), m_path->text());
    if (!path.isEmpty())
        m_path->setText(QDir::toNativeSeparators(path));
}

void ExportDialog::validateAndAccept()
{
    if (m_path->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Render As"), tr("Choose an output file."));
        return;
    }
    if (!m_videoBox->isChecked() && !m_audioBox->isChecked()) {
        QMessageBox::warning(this, tr("Render As"), tr("Enable video and/or audio."));
        return;
    }
    if (QFileInfo::exists(m_path->text())
        && QMessageBox::question(this, tr("Render As"), tr("%1 already exists. Overwrite it?").arg(m_path->text()))
               != QMessageBox::Yes)
        return;
    accept();
}

void ExportDialog::setCaptions(const std::vector<Caption>& captions)
{
    m_captions = captions;
    m_subtitles->setText(tr("Add the captions as a subtitle track (%n caption(s))", nullptr, int(captions.size())));
    m_subtitles->setVisible(!captions.empty());
}

ExportSettings ExportDialog::settings() const
{
    ExportSettings s;
    s.path = m_path->text().trimmed();
    s.muxer = m_muxer->currentData().toString();
    if (m_videoBox->isChecked() && m_videoBox->isEnabled())
        s.videoEncoder = m_videoCodec->currentData().toString();
    if (m_audioBox->isChecked() && m_audioBox->isEnabled())
        s.audioEncoder = m_audioCodec->currentData().toString();
    s.width = m_width->value();
    s.height = m_height->value();
    s.fps = parseFrameRate(m_fps->currentText(), m_project.fps);
    s.quality = m_quality->value();
    s.videoBitrate = int64_t(m_videoBitrate->value()) * 1000;
    s.encoderPreset = m_encoderPreset->text().trimmed();
    s.videoOptions = m_videoOptions->text().trimmed();
    s.sampleRate = m_sampleRate->currentData().toInt();
    s.channels = m_channels->currentData().toInt();
    s.audioBitrate = int64_t(m_audioBitrate->value()) * 1000;
    QString pix = m_pixFmt->currentText().trimmed();
    if (m_pixFmt->currentIndex() == 0 && pix == m_pixFmt->itemText(0))
        pix.clear(); // "Automatic"
    s.pixelFormat = pix;
    s.framing = Framing(m_framing->currentData().toInt());
    s.targetSizeBytes = int64_t(m_targetSize->value() * 1000.0 * 1000.0);
    s.muxerOptions = m_muxerOptions->text().trimmed();
    s.gopSeconds = m_gopSeconds;
    if (m_rangeRegion->isChecked()) {
        s.rangeStart = m_regionStart;
        s.rangeEnd = m_regionEnd;
    } else {
        s.rangeStart = 0;
        s.rangeEnd = m_duration;
    }
    if (m_useGpu->isChecked() && !s.videoEncoder.isEmpty())
        s = withGpuEncoder(s);
    if (m_subtitles->isVisible() && m_subtitles->isChecked())
        s.captions = m_captions;
    return s;
}
