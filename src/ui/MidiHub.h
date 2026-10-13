#pragma once

#include "core/MidiInput.h"

#include <QObject>

// The MIDI controller the Live tab listens to (one at a time), shared by the
// Live tab's mappings and the Modulate dialog's MIDI learn. Messages arrive on
// the GUI thread.
class MidiHub : public QObject {
    Q_OBJECT
public:
    static MidiHub* instance();

    QStringList devices() const { return MidiInput::devices(); }
    bool open(const QString& name, QString* error);
    void close();
    QString current() const { return m_input.name(); }

signals:
    void message(const MidiMessage& m);

private:
    explicit MidiHub(QObject* parent = nullptr) : QObject(parent) {}
    MidiInput m_input;
};
