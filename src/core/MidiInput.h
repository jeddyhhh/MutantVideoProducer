#pragma once

// MIDI controllers (pad grids, keyboards, knob boxes) for the Live tab: lists
// the MIDI inputs and reads one. Windows: the multimedia MIDI API (winmm).
// Linux: the kernel's raw MIDI devices (/dev/snd/midiC*D*), no library needed.
// Game controllers are listed and opened the same way, as "Xbox controller N"
// (Windows: XInput; Linux: /dev/input/js*): their buttons and D-pad send notes,
// the triggers and each direction of the sticks send controllers (0 at rest,
// 127 all the way), on channel 16. Messages arrive on a MIDI thread.

#include <QString>
#include <QStringList>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

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
    // MIDI Machine Control (a SysEx the transport buttons of many controllers send): status 0xF0, data1 the command
    // (1 stop, 2 play, 3 deferred play, 4 fast forward, 5 rewind, 6 record, 9 pause).
    bool isMmc() const { return status == 0xF0; }
};

// The MMC command in a SysEx message (F0 7F <device> 06 <command> F7), if it is one.
std::optional<MidiMessage> mmcFromSysex(const uint8_t* data, size_t size, double time);

// What a controller nobody has mapped does on the Live tab, so a pad controller plugged in plays straight away:
// - notes 36-51 (C1 to D#2, the drum pads of Akai MPD / MPK / LPD, Arturia, Novation and most others) play the
//   pads, laid out as on the controller: 36 bottom-left, 39 bottom-right, 48 top-left. On a keyboard (MPK,
//   Launchkey, Oxygen...) only on channel 10, where their pads are, so the keys don't play pads.
// - an Akai MPD's six faders (CC 1-6 in its Live and generic presets): All pads, the opacity of layers 1-4, and the
//   output level (a master fade). Not on other controllers, whose knobs may send these (an LPD8's do).
// - transport buttons: Akai's controller mode (CC 115 rewind, 116 fast forward, 117 stop, 118 play, 119 record)
//   and MMC: play starts capture, stop stops it, record records, rewind / fast forward step the pad sets.
// An action (LiveView::runAction), or empty. Game controllers have none.
QString defaultMidiAction(const MidiMessage& m, const QString& deviceName);

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

// A game controller's buttons and axes (see kGamePadButtons / kGamePadAxes in MidiInput.cpp).
struct GamePadState {
    uint32_t buttons = 0;  // bit i: button i held
    uint8_t axes[10] = {}; // 0..127
    bool operator==(const GamePadState&) const = default;
};
// The MIDI messages that take a controller from `before` to `now`.
std::vector<MidiMessage> gamePadChanges(const GamePadState& before, const GamePadState& now, double time);
// "A button", "Right trigger", "Left stick up"... for a game controller's message (empty for any other).
QString gamePadControlName(const MidiMessage& m);
bool isGamePad(const QString& deviceName);

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
