#include "core/MidiInput.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QRegularExpression>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace {
double steadyNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

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
#else
    for (const auto& [path, name] : Impl::list())
        out << name;
#endif
    return out;
}

bool MidiInput::open(const QString& name, std::function<void(const MidiMessage&)> onMessage, QString* error)
{
    close();
    d->onMessage = std::move(onMessage);
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
