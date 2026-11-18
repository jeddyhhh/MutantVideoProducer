#pragma once

#include <QByteArray>
#include <QString>
#include <vector>

// OSC (Open Sound Control): messages over the network from apps like TouchOSC
// or Lemur on a phone or tablet. A message is an address ("/mvp/pad/3") and
// arguments; here every number (int, float, double, true / false) is read as a
// double and strings and blobs are passed over.
struct OscMessage {
    QString address;
    std::vector<double> args;
    double arg(size_t i, double fallback) const { return i < args.size() ? args[i] : fallback; }
};

// The messages in a UDP packet: one message, or a bundle (nested ones too).
// Anything malformed is left out.
std::vector<OscMessage> parseOsc(const QByteArray& packet);
// A message with float arguments, as an app would send it (tests).
QByteArray oscPacket(const QString& address, const std::vector<float>& args);
