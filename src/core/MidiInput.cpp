#include "core/MidiInput.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <algorithm>
#include <atomic>
#include <iterator>
#include <chrono>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#include <Xinput.h>
#else
#include <fcntl.h>
#include <linux/joystick.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {
double steadyNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// A game controller: its buttons as notes 60.., its axes as controllers 20.., on channel 16.
constexpr uint8_t kPadChannel = 15, kPadNote = 60, kPadController = 20;
const char* const kGamePadButtons[] = {"A button", "B button", "X button", "Y button", "Left bumper", "Right bumper", "View button",
                                       "Menu button", "Left stick click", "Right stick click", "D-pad up", "D-pad down", "D-pad left",
                                       "D-pad right", "Guide button"};
const char* const kGamePadAxes[] = {"Left trigger", "Right trigger", "Left stick right", "Left stick left", "Left stick up",
                                    "Left stick down", "Right stick right", "Right stick left", "Right stick up", "Right stick down"};
enum PadButton { A, B, X, Y, LB, RB, View, Menu, LS, RS, Up, Down, Left, Right, Guide };
enum PadAxis { LT, RT, LSRight, LSLeft, LSUp, LSDown, RSRight, RSLeft, RSUp, RSDown };
const QString kPadPrefix = QStringLiteral("Xbox controller ");

// A stick's axis (-32768..32767, up / right positive) as its two directions, 0..127 past the dead zone.
void stickAxis(GamePadState& s, int positive, int negative, int v, int deadZone)
{
    auto scale = [&](int a) { return uint8_t(a <= deadZone ? 0 : std::min(127, (a - deadZone) * 127 / (32767 - deadZone))); };
    s.axes[positive] = scale(v);
    s.axes[negative] = scale(-v);
}
uint8_t triggerAxis(int v, int range) // 0..range, a little dead zone at rest
{
    const int dead = range / 10;
    return uint8_t(v <= dead ? 0 : std::min(127, (v - dead) * 127 / (range - dead)));
}
} // namespace

std::vector<MidiMessage> gamePadChanges(const GamePadState& before, const GamePadState& now, double time)
{
    std::vector<MidiMessage> out;
    for (int i = 0; i < int(std::size(kGamePadButtons)); ++i) {
        const bool was = before.buttons >> i & 1, is = now.buttons >> i & 1;
        if (was != is)
            out.push_back({uint8_t((is ? 0x90 : 0x80) | kPadChannel), uint8_t(kPadNote + i), uint8_t(is ? 127 : 0), time});
    }
    for (int i = 0; i < int(std::size(now.axes)); ++i)
        if (before.axes[i] != now.axes[i])
            out.push_back({uint8_t(0xB0 | kPadChannel), uint8_t(kPadController + i), now.axes[i], time});
    return out;
}

QString gamePadControlName(const MidiMessage& m)
{
    if (m.channel() != kPadChannel)
        return {};
    const bool note = m.isNoteOn() || m.isNoteOff();
    const int i = m.data1 - (note ? kPadNote : kPadController);
    if (note)
        return i >= 0 && i < int(std::size(kGamePadButtons)) ? QString::fromLatin1(kGamePadButtons[i]) : QString();
    if (m.isController())
        return i >= 0 && i < int(std::size(kGamePadAxes)) ? QString::fromLatin1(kGamePadAxes[i]) : QString();
    return {};
}

bool isGamePad(const QString& deviceName)
{
    return deviceName.startsWith(kPadPrefix);
}

std::optional<MidiClock::Beat> MidiClock::feed(const MidiMessage& m)
{
    if (m.isStart()) { // from the top: the next tick starts beat 1
        m_running = true;
        m_ticks = 0;
        m_beats = 0;
        m_lastTick = -1;
        return std::nullopt;
    }
    if (m.isContinue()) {
        m_running = true;
        return std::nullopt;
    }
    if (m.isStop()) {
        m_running = false;
        return std::nullopt;
    }
    if (m.isSongPosition()) { // sixteenths from the start
        const int sixteenths = m.songPosition();
        m_beats = sixteenths / 4;
        m_ticks = (sixteenths % 4) * 6;
        return std::nullopt;
    }
    if (!m.isClockTick())
        return std::nullopt;
    // The tempo from the time between ticks, smoothed (ticks jitter a little).
    if (m_lastTick >= 0) {
        const double dt = m.time - m_lastTick;
        if (dt > 0.001 && dt < 0.2) // 12.5 to 2500 BPM: anything else is a gap
            m_interval = m_interval > 0 ? m_interval + (dt - m_interval) * 0.05 : dt;
    }
    m_lastTick = m.time;
    if (m_interval > 0)
        m_bpm = 60.0 / (24.0 * m_interval);
    const bool onBeat = m_ticks == 0;
    m_ticks = (m_ticks + 1) % 24;
    if (!onBeat)
        return std::nullopt;
    const int barBeat = int(m_beats % 4);
    ++m_beats;
    return Beat{m_bpm, barBeat};
}

struct MidiInput::Impl {
    std::function<void(const MidiMessage&)> onMessage;
    QString name;
    // A game controller, read on its own thread.
    std::thread padThread;
    std::atomic<bool> padQuit{false};

    void send(const GamePadState& before, const GamePadState& now)
    {
        for (const MidiMessage& m : gamePadChanges(before, now, steadyNow()))
            if (onMessage)
                onMessage(m);
    }
#ifdef Q_OS_WIN
    static std::vector<int> pads() // XInput's connected users
    {
        std::vector<int> out;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE st{};
            if (XInputGetState(i, &st) == ERROR_SUCCESS)
                out.push_back(int(i));
        }
        return out;
    }
    void runPad(int user)
    {
        GamePadState last;
        DWORD packet = DWORD(-1);
        while (!padQuit) {
            XINPUT_STATE st{};
            if (XInputGetState(DWORD(user), &st) == ERROR_SUCCESS && st.dwPacketNumber != packet) {
                packet = st.dwPacketNumber;
                const XINPUT_GAMEPAD& g = st.Gamepad;
                GamePadState now;
                const std::pair<WORD, int> bits[] = {{XINPUT_GAMEPAD_A, A}, {XINPUT_GAMEPAD_B, B}, {XINPUT_GAMEPAD_X, X}, {XINPUT_GAMEPAD_Y, Y},
                                                     {XINPUT_GAMEPAD_LEFT_SHOULDER, LB}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, RB},
                                                     {XINPUT_GAMEPAD_BACK, View}, {XINPUT_GAMEPAD_START, Menu},
                                                     {XINPUT_GAMEPAD_LEFT_THUMB, LS}, {XINPUT_GAMEPAD_RIGHT_THUMB, RS},
                                                     {XINPUT_GAMEPAD_DPAD_UP, Up}, {XINPUT_GAMEPAD_DPAD_DOWN, Down},
                                                     {XINPUT_GAMEPAD_DPAD_LEFT, Left}, {XINPUT_GAMEPAD_DPAD_RIGHT, Right}};
                for (const auto& [mask, button] : bits)
                    if (g.wButtons & mask)
                        now.buttons |= 1u << button;
                now.axes[LT] = triggerAxis(g.bLeftTrigger, 255);
                now.axes[RT] = triggerAxis(g.bRightTrigger, 255);
                stickAxis(now, LSRight, LSLeft, g.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                stickAxis(now, LSUp, LSDown, g.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE);
                stickAxis(now, RSRight, RSLeft, g.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                stickAxis(now, RSUp, RSDown, g.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE);
                send(last, now);
                last = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(8));
        }
    }
#else
    static QStringList pads() // the kernel's joystick devices
    {
        QStringList out;
        for (const QString& f : QDir(QStringLiteral("/dev/input")).entryList({QStringLiteral("js*")}, QDir::System | QDir::Files, QDir::Name))
            out << QStringLiteral("/dev/input/") + f;
        return out;
    }
    void runPad(int fd)
    {
        // The xpad driver's layout: buttons A B X Y LB RB View Menu Guide LS RS; axes LX LY LT RX RY RT, D-pad X Y.
        const int buttonOf[] = {A, B, X, Y, LB, RB, View, Menu, Guide, LS, RS};
        GamePadState last, now;
        int lx = 0, ly = 0, rx = 0, ry = 0;
        while (!padQuit) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0)
                continue;
            js_event e{};
            if (read(fd, &e, sizeof e) != ssize_t(sizeof e))
                break; // unplugged
            const int type = e.type & ~JS_EVENT_INIT;
            if (type == JS_EVENT_BUTTON && e.number < std::size(buttonOf)) {
                if (e.value)
                    now.buttons |= 1u << buttonOf[e.number];
                else
                    now.buttons &= ~(1u << buttonOf[e.number]);
            } else if (type == JS_EVENT_AXIS) {
                const int v = e.value;
                auto dpad = [&](int negative, int positive) {
                    now.buttons &= ~((1u << negative) | (1u << positive));
                    if (v < -16000)
                        now.buttons |= 1u << negative;
                    else if (v > 16000)
                        now.buttons |= 1u << positive;
                };
                switch (e.number) {
                case 0: lx = v; break;
                case 1: ly = -v; break; // (down is positive here)
                case 3: rx = v; break;
                case 4: ry = -v; break;
                case 2: now.axes[LT] = triggerAxis(v + 32767, 65534); break;
                case 5: now.axes[RT] = triggerAxis(v + 32767, 65534); break;
                case 6: dpad(Left, Right); break;
                case 7: dpad(Up, Down); break;
                default: break;
                }
                stickAxis(now, LSRight, LSLeft, lx, 7849);
                stickAxis(now, LSUp, LSDown, ly, 7849);
                stickAxis(now, RSRight, RSLeft, rx, 8689);
                stickAxis(now, RSUp, RSDown, ry, 8689);
            }
            if (!(e.type & JS_EVENT_INIT) && !(now == last)) {
                send(last, now);
                last = now;
            }
        }
        ::close(fd);
    }
#endif
#ifdef Q_OS_WIN
    HMIDIIN handle = nullptr;

    static void CALLBACK proc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR)
    {
        if (msg != MIM_DATA)
            return;
        auto* self = reinterpret_cast<Impl*>(instance);
        MidiMessage m;
        m.status = uint8_t(param1 & 0xff);
        m.data1 = uint8_t((param1 >> 8) & 0x7f);
        m.data2 = uint8_t((param1 >> 16) & 0x7f);
        m.time = steadyNow();
        // Channel messages, the song position and real time (clock, start, stop).
        if ((m.status < 0xf0 || m.status == 0xf2 || m.status >= 0xf8) && self->onMessage)
            self->onMessage(m);
    }
#else
    int fd = -1;
    std::thread thread;
    std::atomic<bool> quit{false};

    void run()
    {
        uint8_t running = 0, data[2] = {0, 0};
        int have = 0;
        bool sysex = false;
        while (!quit) {
            pollfd p{fd, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0)
                continue;
            uint8_t buf[256];
            const ssize_t n = read(fd, buf, sizeof buf);
            if (n <= 0)
                break; // unplugged
            const double now = steadyNow();
            for (ssize_t i = 0; i < n; ++i) {
                const uint8_t b = buf[i];
                if (b >= 0xf8) { // clock, start / stop...: real time, between any bytes
                    MidiMessage m;
                    m.status = b;
                    m.time = now;
                    if (onMessage)
                        onMessage(m);
                    continue;
                }
                if (b == 0xf0) {
                    sysex = true;
                    continue;
                }
                if (b >= 0x80) {
                    sysex = false;
                    running = b >= 0xf0 && b != 0xf2 ? 0 : b; // the song position has data; other system messages are ignored
                    have = 0;
                    continue;
                }
                if (sysex || !running)
                    continue;
                data[have++] = b;
                const int kind = running >> 4;
                const int need = kind == 0xC || kind == 0xD ? 1 : 2;
                if (have == need) {
                    MidiMessage m;
                    m.status = running;
                    m.data1 = data[0];
                    m.data2 = need == 2 ? data[1] : 0;
                    m.time = now;
                    have = 0; // running status: the next data bytes reuse it
                    if (running == 0xf2)
                        running = 0; // (no running status for system messages)
                    if (onMessage)
                        onMessage(m);
                }
            }
        }
    }

    // "midiC1D0" -> path and a readable name from /proc/asound.
    static std::vector<std::pair<QString, QString>> list()
    {
        std::vector<std::pair<QString, QString>> out;
        QFile cards(QStringLiteral("/proc/asound/cards"));
        QMap<int, QString> names;
        if (cards.open(QIODevice::ReadOnly)) {
            static const QRegularExpression re(QStringLiteral("^\\s*(\\d+)\\s+\\[[^\\]]*\\]:\\s*[^-]*-\\s*(.+)$"));
            for (const QString& line : QString::fromUtf8(cards.readAll()).split(QLatin1Char('\n'))) {
                const auto m = re.match(line);
                if (m.hasMatch())
                    names[m.captured(1).toInt()] = m.captured(2).trimmed();
            }
        }
        const QDir dir(QStringLiteral("/dev/snd"));
        static const QRegularExpression dev(QStringLiteral("^midiC(\\d+)D(\\d+)$"));
        for (const QString& f : dir.entryList({QStringLiteral("midiC*D*")}, QDir::System | QDir::Files, QDir::Name)) {
            const auto m = dev.match(f);
            if (!m.hasMatch())
                continue;
            const int card = m.captured(1).toInt(), device = m.captured(2).toInt();
            const QString base = names.value(card, QStringLiteral("Card %1").arg(card));
            out.emplace_back(dir.filePath(f), device > 0 ? QStringLiteral("%1 (%2)").arg(base).arg(device + 1) : base);
        }
        return out;
    }
#endif
};

MidiInput::MidiInput() : d(std::make_unique<Impl>()) {}

MidiInput::~MidiInput()
{
    close();
}

QStringList MidiInput::devices()
{
    QStringList out;
#ifdef Q_OS_WIN
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR)
            out << QString::fromWCharArray(caps.szPname);
    }
    for (int i = 0; i < int(Impl::pads().size()); ++i)
        out << kPadPrefix + QString::number(i + 1);
#else
    for (const auto& [path, name] : Impl::list())
        out << name;
    for (int i = 0; i < int(Impl::pads().size()); ++i)
        out << kPadPrefix + QString::number(i + 1);
#endif
    return out;
}

bool MidiInput::open(const QString& name, std::function<void(const MidiMessage&)> onMessage, QString* error)
{
    close();
    d->onMessage = std::move(onMessage);
    if (isGamePad(name)) {
        const auto pads = Impl::pads();
        const int i = name.mid(kPadPrefix.size()).toInt() - 1;
        if (i < 0 || i >= int(pads.size())) {
            if (error)
                *error = QStringLiteral("%1 is not connected").arg(name);
            return false;
        }
#ifdef Q_OS_WIN
        const int handle = pads[size_t(i)];
#else
        const int handle = ::open(pads[i].toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (handle < 0) {
            if (error)
                *error = QStringLiteral("could not open %1 (are you in the input group?)").arg(pads[i]);
            return false;
        }
#endif
        d->padQuit = false;
        d->name = name;
        d->padThread = std::thread([this, handle] { d->runPad(handle); });
        return true;
    }
#ifdef Q_OS_WIN
    const UINT n = midiInGetNumDevs();
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR || QString::fromWCharArray(caps.szPname) != name)
            continue;
        const MMRESULT r = midiInOpen(&d->handle, i, DWORD_PTR(&Impl::proc), DWORD_PTR(d.get()), CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            d->handle = nullptr;
            if (error)
                *error = QStringLiteral("could not open %1 (is another program using it?)").arg(name);
            return false;
        }
        midiInStart(d->handle);
        d->name = name;
        return true;
    }
#else
    for (const auto& [path, devName] : Impl::list()) {
        if (devName != name)
            continue;
        d->fd = ::open(path.toLocal8Bit().constData(), O_RDONLY | O_NONBLOCK);
        if (d->fd < 0) {
            if (error)
                *error = QStringLiteral("could not open %1 (are you in the audio group?)").arg(path);
            return false;
        }
        d->quit = false;
        d->name = name;
        d->thread = std::thread([this] { d->run(); });
        return true;
    }
#endif
    if (error)
        *error = QStringLiteral("%1 is not connected").arg(name);
    return false;
}

void MidiInput::close()
{
    d->padQuit = true;
    if (d->padThread.joinable())
        d->padThread.join();
#ifdef Q_OS_WIN
    if (d->handle) {
        midiInStop(d->handle);
        midiInReset(d->handle);
        midiInClose(d->handle);
        d->handle = nullptr;
    }
#else
    d->quit = true;
    if (d->thread.joinable())
        d->thread.join();
    if (d->fd >= 0)
        ::close(d->fd);
    d->fd = -1;
#endif
    d->name.clear();
}

bool MidiInput::isOpen() const
{
    return !d->name.isEmpty();
}

QString MidiInput::name() const
{
    return d->name;
}
