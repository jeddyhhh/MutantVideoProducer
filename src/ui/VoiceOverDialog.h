#pragma once

// Tools > Record Voice-Over: records a microphone (any Live audio input) to a
// WAV file while the edit plays from the cursor, so you can talk over the
// picture. Each take is handed back to be placed where it started.

#include "core/LiveAudio.h"

#include <QDialog>
#include <functional>
#include <mutex>

class AudioMeter;
class QCheckBox;
class QComboBox;
class QFile;
class QLabel;
class QPushButton;
class QTimer;

class VoiceOverDialog : public QDialog {
    Q_OBJECT
public:
    // Takes are saved in `folder` (made when the first take starts).
    explicit VoiceOverDialog(const QString& folder, QWidget* parent = nullptr);
    ~VoiceOverDialog() override;

    // Opens this input (the device list's choice, or a test source) and shows its level.
    void setSource(const LiveAudioSource& source);
    bool isRecording() const { return m_file != nullptr; }
    bool playsEdit() const;

    void startTake();
    void stopTake();

    // Called as a take starts (play the edit if wanted) and ends (stop it).
    std::function<void(bool playEdit)> onStart;
    std::function<void()> onStop;

signals:
    // A finished take: its WAV file.
    void recorded(const QString& path);

protected:
    void closeEvent(QCloseEvent* e) override;
    void reject() override;

private:
    void write(const float* samples, int frames, double time);
    void updateState();

    QString m_folder;
    LiveAudioCapture m_capture;
    QComboBox* m_device;
    QCheckBox* m_playEdit;
    AudioMeter* m_meter;
    QPushButton* m_record;
    QLabel* m_status;
    QTimer* m_timer;

    std::mutex m_lock; // the take's file, written on the capture thread
    QFile* m_file = nullptr;
    double m_startTime = 0; // liveClock() when the take started: earlier samples are left out
    qint64 m_frames = 0;
};
