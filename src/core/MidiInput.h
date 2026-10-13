#pragma once

// MIDI controllers (pad grids, keyboards, knob boxes) for the Live tab: lists
// the MIDI inputs and reads one. Windows: the multimedia MIDI API (winmm).
// Linux: the kernel's raw MIDI devices (/dev/snd/midiC*D*), no library needed.
// Messages arrive on a MIDI thread.

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

struct MidiMessage {
    uint8_t status = 0, data1 = 0, data2 = 0;
    double time = 0; // when it arrived (seconds, steady clock): timing for MIDI clock
    int kind() const { return status >> 4; }      // 0x8 note off, 0x9 note on, 0xB controller...
    int channel() const { return status & 0x0f; }
    bool isNoteOn() const { return kind() == 0x9 && data2 > 0; }
    bool isNoteOff() const { return kind() == 0x8 || (kind() == 0x9 && data2 == 0); }
    bool isController() const { return kind() == 0xB; }
    // MIDI clock (from DJ software, a drum machine...): 24 ticks a beat, start /
    // continue / stop, and the song position (in sixteenths).
    bool isClockTick() const { return status == 0xF8; }
    bool isStart() const { return status == 0xFA; }
    bool isContinue() const { return status == 0xFB; }
    bool isStop() const { return status == 0xFC; }
    bool isSongPosition() const { return status == 0xF2; }
    int songPosition() const { return data1 | (data2 << 7); }
};

// Follows a MIDI clock: the tempo from the ticks, a beat every 24 of them, and
// where it is in the bar (from Start and the song position).
class MidiClock {
public:
    struct Beat {
        double bpm;
        int barBeat; // 0 = the 1
    };
    // Returns a beat when this message completes one.
    std::optional<Beat> feed(const MidiMessage& m);
    double bpm() const { return m_bpm; }
    bool running() const { return m_running; }

private:
    double m_lastTick = -1, m_bpm = 0;
    double m_interval = 0; // smoothed seconds per tick
    int m_ticks = 0;       // ticks since the last beat
    int64_t m_beats = 0;   // beats since the start
    bool m_running = false;
};

class MidiInput {
public:
    MidiInput();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    static QStringList devices();
    // Opens the input called `name`; `onMessage` is called on a MIDI thread.
    bool open(const QString& name, std::function<void(const MidiMessage&)> onMessage, QString* error);
    void close();
    bool isOpen() const;
    QString name() const;

    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
