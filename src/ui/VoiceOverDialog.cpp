#include "ui/VoiceOverDialog.h"

#include "ui/AudioMeter.h"

#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr int kRate = LiveAudioCapture::kSampleRate;

// A 32-bit float stereo WAV header for `frames` frames.
QByteArray wavHeader(qint64 frames)
{
    const quint32 data = quint32(std::min<qint64>(frames * 8, 0xFFFFFFFFll - 36));
    QByteArray h(44, 0);
    auto put32 = [&](int at, quint32 v) { qToLittleEndian(v, h.data() + at); };
    auto put16 = [&](int at, quint16 v) { qToLittleEndian(v, h.data() + at); };
    memcpy(h.data(), "RIFF", 4);
    put32(4, 36 + data);
    memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(16, 16);
    put16(20, 3); // IEEE float
    put16(22, 2);
    put32(24, kRate);
    put32(28, kRate * 8);
    put16(32, 8);
    put16(34, 32);
    memcpy(h.data() + 36, "data", 4);
    put32(40, data);
    return h;
}
} // namespace

VoiceOverDialog::VoiceOverDialog(const QString& folder, QWidget* parent) : QDialog(parent), m_folder(folder)
{
    setWindowTitle(tr("Record Voice-Over"));
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("Talk over your edit: recording starts the edit playing from the cursor, and each take goes onto a "
                                "Voice-over track where it started. Use headphones, or the speakers will be heard in the take."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto* form = new QFormLayout;
    m_device = new QComboBox;
    m_device->setObjectName(QStringLiteral("voiceOverDevice"));
    for (const auto& s : listLiveAudioSources())
        m_device->addItem(s.name, QVariant::fromValue(QStringList{s.format, s.id}));
    form->addRow(tr("Microphone"), m_device);
    m_meter = new AudioMeter;
    form->addRow(tr("Level"), m_meter);
    m_playEdit = new QCheckBox(tr("Play the edit while recording"));
    m_playEdit->setChecked(QSettings().value(QStringLiteral("voiceover/playEdit"), true).toBool());
    form->addRow(m_playEdit);
    layout->addLayout(form);
    m_status = new QLabel;
    layout->addWidget(m_status);
    auto* buttons = new QHBoxLayout;
    m_record = new QPushButton;
    m_record->setObjectName(QStringLiteral("voiceOverRecord"));
    m_record->setDefault(true);
    auto* close = new QPushButton(tr("Close"));
    buttons->addStretch();
    buttons->addWidget(m_record);
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(m_record, &QPushButton::clicked, this, [this] { isRecording() ? stopTake() : startTake(); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_playEdit, &QCheckBox::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("voiceover/playEdit"), on); });
    connect(m_device, &QComboBox::currentIndexChanged, this, [this] {
        const QStringList d = m_device->currentData().toStringList();
        if (d.size() == 2) {
            QSettings().setValue(QStringLiteral("voiceover/device"), m_device->currentText());
            setSource({m_device->currentText(), d[0], d[1]});
        }
    });
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, [this] {
        const auto [l, r] = m_capture.takePeaks();
        m_meter->setPeaks(l, r);
        if (!m_capture.failure().isEmpty() && isRecording())
            stopTake();
        updateState();
    });
    m_timer->start(33);

    const int remembered = m_device->findText(QSettings().value(QStringLiteral("voiceover/device")).toString());
    if (m_device->count() > 0) {
        m_device->setCurrentIndex(std::max(0, remembered));
        if (!m_capture.isRunning()) // (index 0 was already current: no change signal)
            emit m_device->currentIndexChanged(m_device->currentIndex());
    }
    updateState();
}

VoiceOverDialog::~VoiceOverDialog()
{
    // (closing with the main window: finish the file, but don't call back into a window being destroyed)
    onStart = {};
    onStop = {};
    blockSignals(true);
    stopTake();
    m_capture.stop();
}

void VoiceOverDialog::setSource(const LiveAudioSource& source)
{
    stopTake();
    m_capture.stop();
    QString error;
    if (!m_capture.start(source, [this](const float* samples, int frames, double time) { write(samples, frames, time); }, &error))
        m_status->setText(tr("Could not open %1: %2").arg(source.name, error));
    else
        m_status->clear();
    updateState();
}

bool VoiceOverDialog::playsEdit() const
{
    return m_playEdit->isChecked();
}

void VoiceOverDialog::startTake()
{
    if (isRecording() || !m_capture.isRunning())
        return;
    if (!QDir().mkpath(m_folder)) {
        m_status->setText(tr("Could not create the folder %1").arg(QDir::toNativeSeparators(m_folder)));
        return;
    }
    const QString base = QDir(m_folder).filePath(QStringLiteral("Voice-over %1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd hh.mm.ss"))));
    QString path = base + QStringLiteral(".wav");
    for (int n = 2; QFile::exists(path); ++n)
        path = QStringLiteral("%1 (%2).wav").arg(base).arg(n);
    auto* file = new QFile(path);
    if (!file->open(QIODevice::WriteOnly) || file->write(wavHeader(0)) != 44) {
        m_status->setText(tr("Could not write %1: %2").arg(QDir::toNativeSeparators(path), file->errorString()));
        delete file;
        return;
    }
    if (onStart)
        onStart(playsEdit());
    {
        std::lock_guard lock(m_lock);
        m_frames = 0;
        m_startTime = liveClock();
        m_file = file;
    }
    m_device->setEnabled(false);
    updateState();
}

void VoiceOverDialog::stopTake()
{
    QFile* file;
    qint64 frames;
    {
        std::lock_guard lock(m_lock);
        file = m_file;
        frames = m_frames;
        m_file = nullptr;
    }
    if (!file)
        return;
    if (onStop)
        onStop();
    const bool ok = file->seek(0) && file->write(wavHeader(frames)) == 44;
    file->close();
    const QString path = file->fileName();
    delete file;
    m_device->setEnabled(true);
    if (!ok)
        m_status->setText(tr("Could not finish %1").arg(QDir::toNativeSeparators(path)));
    else if (frames < kRate / 10) {
        QFile::remove(path);
        m_status->setText(tr("The take was too short to keep."));
    } else {
        m_status->setText(tr("Kept %1 (%2 s).").arg(QFileInfo(path).fileName()).arg(double(frames) / kRate, 0, 'f', 1));
        emit recorded(path);
    }
    updateState();
}

void VoiceOverDialog::write(const float* samples, int frames, double time)
{
    std::lock_guard lock(m_lock);
    if (!m_file)
        return;
    // (a block that started before the take: only its part after the click)
    const int skip = std::clamp(int(std::lround((m_startTime - time) * kRate)), 0, frames);
    if (skip < frames) {
        m_file->write(reinterpret_cast<const char*>(samples + size_t(skip) * 2), qint64(frames - skip) * 8);
        m_frames += frames - skip;
    }
}

void VoiceOverDialog::updateState()
{
    const bool recording = isRecording();
    m_record->setText(recording ? tr("Stop") : tr("Record"));
    m_record->setEnabled(m_capture.isRunning() || recording);
    m_playEdit->setEnabled(!recording);
    if (recording) {
        qint64 frames;
        {
            std::lock_guard lock(m_lock);
            frames = m_frames;
        }
        m_status->setText(tr("Recording... %1 s").arg(double(frames) / kRate, 0, 'f', 1));
    } else if (m_device->count() == 0 && !m_capture.isRunning())
        m_status->setText(tr("No microphone was found. Plug one in and open Record Voice-Over again."));
    else if (!m_capture.failure().isEmpty())
        m_status->setText(m_capture.failure());
}

void VoiceOverDialog::closeEvent(QCloseEvent* e)
{
    stopTake();
    QDialog::closeEvent(e);
}

void VoiceOverDialog::reject()
{
    stopTake();
    QDialog::reject();
}
