#pragma once

#include <QDialog>
#include <QString>

class QComboBox;
class QLineEdit;
class QSpinBox;
class QLabel;

// Live tab > Stream Settings: where to stream (Twitch, YouTube, Facebook or
// any RTMP / RTMPS / SRT server), the stream key and the bitrate. The key is
// kept encrypted for this Windows account (DPAPI), never shown in full.
class StreamDialog : public QDialog {
    Q_OBJECT
public:
    explicit StreamDialog(QWidget* parent = nullptr);

    // The saved settings: the address to stream to (server + key), empty if
    // there is no key or server yet; the bitrate; a name for the status line.
    static QString streamUrl();
    static int bitrateKbps();
    static QString serviceName();

private:
    void save();
    void updateServer();

    QComboBox* m_service;
    QLineEdit* m_server;
    QLineEdit* m_key;
    QSpinBox* m_kbps;
    QLabel* m_hint;
};
