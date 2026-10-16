#include "ui/RenderStatusDialog.h"
#include <QApplication>
#include <QJsonArray>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileInfo>
#include <QLabel>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QScrollBar>
#include <QTextCursor>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <cmath>

namespace {
void updateText(QPlainTextEdit* view, const QString& text)
{
    if (view->toPlainText() == text)
        return;
    const int scroll = view->verticalScrollBar()->value();
    const auto oldCursor = view->textCursor();
    view->setPlainText(text);
    auto cursor = view->textCursor();
    cursor.setPosition(std::min(oldCursor.anchor(), int(text.size())));
    cursor.setPosition(std::min(oldCursor.position(), int(text.size())), QTextCursor::KeepAnchor);
    view->setTextCursor(cursor);
    view->verticalScrollBar()->setValue(scroll);
}
} // namespace

void RenderMonitor::update(const ExportStatus& value)
{
    std::lock_guard<std::mutex> lock(mutex);
    if (status.stage != value.stage)
        activity.append(QStringLiteral("[%1 s] %2: %3").arg(value.elapsed, 0, 'f', 2).arg(value.stage, value.detail));
    status = value;
}
ExportStatus RenderMonitor::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex);
    return status;
}
QJsonObject RenderMonitor::json() const
{
    const auto s = snapshot();
    return {{"active_clips", QJsonArray::fromStringList(s.activeClips)},
            {"stage", s.stage},
            {"detail", s.detail},
            {"frames", double(s.frames)},
            {"total_frames", double(s.totalFrames)},
            {"audio_samples", double(s.samples)},
            {"total_audio_samples", double(s.totalSamples)},
            {"elapsed_seconds", s.elapsed},
            {"timeline_seconds", s.position},
            {"renderer", s.renderer},
            {"video_encoder", s.videoEncoder},
            {"audio_encoder", s.audioEncoder},
            {"pixel_format", s.pixelFormat},
            {"sample_format", s.sampleFormat},
            {"file_bytes", double(s.fileBytes)},
            {"packet_bytes", double(s.packetBytes)},
            {"video_packets", double(s.videoPackets)},
            {"audio_packets", double(s.audioPackets)},
            {"subtitle_packets", double(s.subtitlePackets)},
            {"color_conversion", s.colorConversion},
            {"compositing_seconds", s.renderingSeconds},
            {"conversion_seconds", s.conversionSeconds},
            {"audio_mixing_seconds", s.audioSeconds},
            {"encoding_seconds", s.encodingSeconds},
            {"muxing_seconds", s.muxingSeconds}};
}
RenderStatusDialog::RenderStatusDialog(const ExportSettings& s, const ProjectSnapshot& project, std::shared_ptr<RenderMonitor> monitor,
                                       QWidget* parent)
    : QDialog(parent), m_settings(s), m_monitor(std::move(monitor))
{
    m_clock.start();
    setWindowTitle(tr("Render Status - %1").arg(QFileInfo(s.path).fileName()));
    resize(900, 740);
    setMinimumSize(600, 450);
    auto* layout = new QVBoxLayout(this);
    m_stage = new QLabel(tr("Preparing render"));
    m_stage->setWordWrap(true);
    m_stage->setStyleSheet(QStringLiteral("font-size:18px;font-weight:bold;"));
    layout->addWidget(m_stage);
    auto* path = new QLabel(s.path);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path);
    m_progress = new QProgressBar;
    m_progress->setRange(0, 1000);
    layout->addWidget(m_progress);
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    auto* tabs = new QTabWidget;
    layout->addWidget(tabs, 1);
    auto page = [&](const QString& title) {
        auto* w = new QPlainTextEdit;
        w->setReadOnly(true);
        tabs->addTab(w, title);
        return w;
    };
    m_metrics = page(tr("Live statistics"));
    auto* settings = page(tr("Output and timeline"));
    qint64 clips = 0, effects = 0;
    for (const auto& t : project.timeline.tracks) {
        clips += t.clips.size();
        effects += t.effects.size();
        for (const auto& c : t.clips)
            effects += c.effects.size();
    }
    settings->setPlainText(
        tr("Output: %1\nContainer: %2\nRange: %3 to %4 seconds (%5 seconds)\nVideo: %6, %7 x %8, %9 fps\nVideo bitrate: %10 bits/s; "
           "quality: %11; preset: %12\nPixel format: %13\nVideo options: %14\nAudio: %15, %16 Hz, %17 channels, %18 bits/s\nAudio options: "
           "%19\nMuxer options: %20\nGOP: %21 seconds (0 = default)\nTarget size: %22 bytes (0 = unrestricted)\nTimeline: %23 tracks, %24 "
           "clips, %25 clip/track effects, %26 media files\nEmbedded caption cues requested: %27\n\nCounters report completed work. "
           "Encoding and muxing are measured separately. Compositing includes source decoding and video effects; audio mixing includes "
           "source decoding and audio effects. ETA applies to frame/sample production, not container finalization.")
            .arg(s.path, s.muxer)
            .arg(double(s.rangeStart) / kUsPerSecond)
            .arg(double(s.rangeEnd) / kUsPerSecond)
            .arg(double(s.rangeEnd - s.rangeStart) / kUsPerSecond)
            .arg(s.videoEncoder)
            .arg(s.width)
            .arg(s.height)
            .arg(s.fps.toDouble())
            .arg(s.videoBitrate)
            .arg(s.quality)
            .arg(s.encoderPreset)
            .arg(s.pixelFormat.isEmpty() ? tr("Automatic") : s.pixelFormat, s.videoOptions, s.audioEncoder)
            .arg(s.sampleRate)
            .arg(s.channels)
            .arg(s.audioBitrate)
            .arg(s.audioOptions, s.muxerOptions)
            .arg(s.gopSeconds)
            .arg(s.targetSizeBytes)
            .arg(project.timeline.tracks.size())
            .arg(clips)
            .arg(effects)
            .arg(project.media.size())
            .arg(s.captions.size()));
    m_active = page(tr("Active clips and effects"));
    m_log = page(tr("Stage log"));
    auto* buttons = new QHBoxLayout;
    layout->addLayout(buttons);
    auto* copy = new QPushButton(tr("Copy report"));
    buttons->addWidget(copy);
    connect(copy, &QPushButton::clicked, this, [=] {
        QApplication::clipboard()->setText(m_summary->text() + "\n\n" + m_metrics->toPlainText() + "\n\n" + m_active->toPlainText() + "\n\n"
                                           + settings->toPlainText() + "\n\n" + m_log->toPlainText());
    });
    buttons->addStretch();
    m_folder = new QPushButton(tr("Open output folder"));
    buttons->addWidget(m_folder);
    connect(m_folder, &QPushButton::clicked, this,
            [=] { QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(s.path).absolutePath())); });
    m_cancel = new QPushButton(tr("Cancel render"));
    buttons->addWidget(m_cancel);
    connect(m_cancel, &QPushButton::clicked, this, [=] {
        m_monitor->cancel = true;
        refresh();
    });
    m_close = new QPushButton(tr("Close"));
    buttons->addWidget(m_close);
    connect(m_close, &QPushButton::clicked, this, &QDialog::accept);
    auto* timer = new QTimer(this);
    timer->setObjectName("renderStatusPoll");
    connect(timer, &QTimer::timeout, this, &RenderStatusDialog::refresh);
    timer->start(200);
    refresh();
}
void RenderStatusDialog::reject()
{
    // A running job stays visible until its worker has released output resources.
    if (m_monitor->done)
        QDialog::reject();
}
void RenderStatusDialog::refresh()
{
    auto s = m_monitor->snapshot();
    const bool done = m_monitor->done.load();
    if (!done)
        s.elapsed = std::max(s.elapsed, m_clock.elapsed() / 1000.0);
    if (done)
        findChild<QTimer*>("renderStatusPoll")->stop();
    const bool producing = s.stage == QLatin1String("Rendering");
    m_stage->setText((m_monitor->cancel && !done ? tr("Cancellation requested - waiting for worker") : s.stage) + "\n" + s.detail);
    if (!producing && !done) {
        m_progress->setRange(0, 0);
        m_progress->setFormat(tr("%1").arg(s.stage));
    } else {
        m_progress->setRange(0, 1000);
        m_progress->setValue(int(s.progress * 1000));
        m_progress->setFormat(tr("%1% of frames/audio produced").arg(s.progress * 100, 0, 'f', 1));
    }
    const double fps = s.elapsed > 0 ? s.frames / s.elapsed : 0;
    const double speed = s.elapsed > 0 ? (s.position - double(m_settings.rangeStart) / kUsPerSecond) / s.elapsed : 0;
    QString eta = producing && s.progress > .001 ? tr("%1 s (production only)").arg(s.elapsed * (1 / s.progress - 1), 0, 'f', 1)
                                                 : tr("Unavailable during setup/finalization");
    m_summary->setText(
        tr("Elapsed: %1 s | Remaining estimate: %2\nAverage throughput: %3 fps | Timeline speed: %4x real time | Output: %5 MiB")
            .arg(s.elapsed, 0, 'f', 1)
            .arg(eta)
            .arg(fps, 0, 'f', 2)
            .arg(speed, 0, 'f', 2)
            .arg(s.fileBytes / 1048576.0, 0, 'f', 2));
    updateText(m_active, s.activeClips.isEmpty() ? tr("No clips currently being processed in this stage.") : s.activeClips.join("\n\n"));
    updateText(m_metrics,
               tr("STAGE\n%1\n%2\n\nPICTURE\nFrames produced: %3 / %4\nTimeline position: %5 s\nRenderer: %6\nActual encoder: %7\nActual "
                  "size: %8 x %9\nActual pixel format: %10\n\nAUDIO\nSamples mixed (per channel): %11 / %12\nActual encoder: %13\nActual "
                  "sample format: %14\nActual output: %15 Hz, %16 channels\n\nCONTAINER\nVideo packets: %17\nAudio packets: %18\nSubtitle "
                  "packets: %19\nEncoded packet payload: %20 MiB\nFile size on disk: %21 MiB\n\nMEASURED PIPELINE TIME\nSource decoding / "
                  "effects / compositing: %22 s\nPicture format conversion: %23 s\nAudio decoding / mixing / effects: %24 s\nCodec calls "
                  "(video + audio): %25 s\nPacket mux writes: %26 s\nTotal elapsed (including setup and finalization): %27 s")
                       .arg(s.stage, s.detail)
                       .arg(s.frames)
                       .arg(s.totalFrames)
                       .arg(s.position, 0, 'f', 3)
                       .arg(s.renderer, s.videoEncoder)
                       .arg(s.width)
                       .arg(s.height)
                       .arg(s.pixelFormat)
                       .arg(s.samples)
                       .arg(s.totalSamples)
                       .arg(s.audioEncoder, s.sampleFormat)
                       .arg(s.sampleRate)
                       .arg(s.channels)
                       .arg(s.videoPackets)
                       .arg(s.audioPackets)
                       .arg(s.subtitlePackets)
                       .arg(s.packetBytes / 1048576.0, 0, 'f', 2)
                       .arg(s.fileBytes / 1048576.0, 0, 'f', 2)
                       .arg(s.renderingSeconds, 0, 'f', 3)
                       .arg(s.conversionSeconds, 0, 'f', 3)
                       .arg(s.audioSeconds, 0, 'f', 3)
                       .arg(s.encodingSeconds, 0, 'f', 3)
                       .arg(s.muxingSeconds, 0, 'f', 3)
                       .arg(s.elapsed, 0, 'f', 3)
                   + tr("\n\nColour conversion: %1").arg(s.colorConversion));
    {
        std::lock_guard<std::mutex> lock(m_monitor->mutex);
        if (m_logLines != m_monitor->activity.size()) {
            m_log->setPlainText(m_monitor->activity.join('\n'));
            m_logLines = m_monitor->activity.size();
        }
    }
    m_cancel->setEnabled(!done && !m_monitor->cancel);
    m_close->setEnabled(done);
    m_folder->setEnabled(done && s.stage == QLatin1String("Complete"));
}
