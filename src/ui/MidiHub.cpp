#include "ui/MidiHub.h"

#include <QCoreApplication>
#include <QPointer>

MidiHub* MidiHub::instance()
{
    static QPointer<MidiHub> hub;
    if (!hub)
        hub = new MidiHub(QCoreApplication::instance());
    return hub;
}

bool MidiHub::open(const QString& name, QString* error)
{
    if (name == m_input.name())
        return true;
    QPointer<MidiHub> self(this);
    return m_input.open(
        name,
        [self](const MidiMessage& m) {
            // From the MIDI thread to the GUI thread.
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, m] {
                if (self)
                    emit self->message(m);
            }, Qt::QueuedConnection);
        },
        error);
}

void MidiHub::close()
{
    m_input.close();
}
