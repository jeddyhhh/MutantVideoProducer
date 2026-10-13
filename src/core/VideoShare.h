#pragma once

// Sending pictures to other apps on this computer (Windows):
//
//   Spout - a shared DirectX 11 texture that other apps receive by name:
//           OBS (with the Spout2 plugin), Resolume, TouchDesigner, MadMapper,
//           SpoutCam (which turns it into a webcam)... The sender registers
//           itself the way the Spout SDK does (the "SpoutSenderNames" list,
//           its texture info, the access mutex and frame counter).
//
//   Virtual camera - the "OBS Virtual Camera" webcam that OBS Studio installs:
//           Mutant Video Producer writes into the shared memory queue it reads, so Zoom,
//           Discord, Teams, a browser... see Mutant Video Producer's picture as a camera,
//           without OBS running. It can't be used while OBS's own virtual
//           camera is on.
//
// Elsewhere these report that they aren't available.

#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>
#include <memory>

class SpoutSender {
public:
    SpoutSender();
    ~SpoutSender();
    SpoutSender(const SpoutSender&) = delete;
    SpoutSender& operator=(const SpoutSender&) = delete;

    // Windows with Direct3D 11.
    static bool available();
    // Registers the sender ("Mutant Video Producer", or "Mutant Video Producer_1"... when that name is taken).
    bool open(const QString& name, QString* error = nullptr);
    // Shares a picture (any size; the texture follows it).
    bool send(const QImage& picture);
    void close();
    bool isOpen() const;
    QString name() const; // as registered
    QSize size() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};

// Receiving (tests, and listing what is out there).
namespace spout {
QStringList senders();
// The sender's current picture, read through its own D3D11 device as another app would.
QImage receive(const QString& name, QString* error = nullptr);
} // namespace spout

class VirtualCamera {
public:
    VirtualCamera();
    ~VirtualCamera();
    VirtualCamera(const VirtualCamera&) = delete;
    VirtualCamera& operator=(const VirtualCamera&) = delete;

    // The OBS Virtual Camera is installed (registered as a camera).
    static bool installed();
    static QString cameraName() { return QStringLiteral("OBS Virtual Camera"); }
    // Starts feeding it pictures of `size` (even) at `fps`.
    bool open(QSize size, double fps, QString* error = nullptr);
    // Sends a picture (scaled to the size it was opened with).
    bool send(const QImage& picture);
    void close();
    bool isOpen() const;
    QSize size() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
