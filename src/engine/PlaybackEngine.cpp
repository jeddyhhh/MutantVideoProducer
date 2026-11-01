#include "engine/PlaybackEngine.h"

#include "core/AudioMixer.h"
#include "core/FrameRenderer.h"
#include "core/Proxy.h"

#include <QAudioDevice>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>
#include <QOffscreenSurface>
#include <algorithm>
#include <cstring>

namespace {
constexpr size_t kVideoQueueMax = 4;
constexpr int kAudioChunk = 1024;            // frames mixed per step
constexpr double kRingSeconds = 0.25;        // audio buffered ahead of the device
constexpr double kScrubSeconds = 0.08;       // length of an audio scrub snippet
}

// Pull-mode device the audio sink reads from. Underruns play silence so the
// clock keeps running even when mixing can't keep up.
class AudioRingDevice : public QIODevice {
public:
    explicit AudioRingDevice(PlaybackEngine* engine) : m_engine(engine) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 16; }

protected:
    qint64 writeData(const char*, qint64) override { return -1; }
    qint64 readData(char* data, qint64 maxlen) override
    {
        const bool asFloat = m_engine->m_format.sampleFormat() == QAudioFormat::Float;
        const int bytesPerFrame = asFloat ? 8 : 4;
        const size_t frames = size_t(maxlen / bytesPerFrame);
        if (frames == 0)
            return 0;
        std::memset(data, 0, frames * bytesPerFrame);
        std::lock_guard lock(m_engine->m_ringMutex);
        auto& ring = m_engine->m_ring;
        const size_t ringFrames = ring.size() / 2;
        const size_t n = std::min(frames, m_engine->m_ringFill);
        for (size_t i = 0; i < n; ++i) {
            const size_t idx = ((m_engine->m_ringRead + i) % ringFrames) * 2;
            float l = std::clamp(ring[idx], -1.0f, 1.0f);
            float r = std::clamp(ring[idx + 1], -1.0f, 1.0f);
            m_engine->m_peakL = std::max(m_engine->m_peakL, std::abs(ring[idx]));
            m_engine->m_peakR = std::max(m_engine->m_peakR, std::abs(ring[idx + 1]));
            if (asFloat) {
                reinterpret_cast<float*>(data)[i * 2] = l;
                reinterpret_cast<float*>(data)[i * 2 + 1] = r;
            } else {
                reinterpret_cast<int16_t*>(data)[i * 2] = int16_t(l * 32767.0f);
                reinterpret_cast<int16_t*>(data)[i * 2 + 1] = int16_t(r * 32767.0f);
            }
        }
        if (ringFrames)
            m_engine->m_ringRead = (m_engine->m_ringRead + n) % ringFrames;
        m_engine->m_ringFill -= n;
        m_engine->m_ringReadPos += int64_t(n);
        m_engine->m_audioCv.notify_one();
        return qint64(frames * bytesPerFrame);
    }

private:
    PlaybackEngine* m_engine;
};

PlaybackEngine::PlaybackEngine(QObject* parent) : QObject(parent)
{
    m_gpuSurface = createGpuSurface(); // may be null: CPU rendering then
    m_preSurface = createGpuSurface();
    setupAudio();
    m_scrubStop.setSingleShot(true);
    m_scrubStop.setInterval(500);
    connect(&m_scrubStop, &QTimer::timeout, this, [this] {
        if (m_scrubSink && !m_playing && m_sink)
            m_sink->stop();
        m_scrubSink = false;
    });
    m_tick.setTimerType(Qt::PreciseTimer);
    m_tick.setInterval(4);
    connect(&m_tick, &QTimer::timeout, this, &PlaybackEngine::onTick);
    m_videoThread = std::thread(&PlaybackEngine::videoThreadMain, this);
    m_audioThread = std::thread(&PlaybackEngine::audioThreadMain, this);
    m_preThread = std::thread(&PlaybackEngine::preRenderThreadMain, this);
}

PlaybackEngine::~PlaybackEngine()
{
    stop();
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    {
        std::lock_guard lock(m_ringMutex);
    }
    m_videoCv.notify_all();
    m_audioCv.notify_all();
    m_preCv.notify_all();
    m_videoThread.join();
    m_audioThread.join();
    m_preThread.join();
    // The sink pulls from the ring device: stop and delete it first.
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    delete m_device;
    m_device = nullptr;
}

std::pair<float, float> PlaybackEngine::takePeaks()
{
    std::lock_guard lock(m_ringMutex);
    const std::pair<float, float> peaks{m_peakL, m_peakR};
    m_peakL = m_peakR = 0;
    return peaks;
}

std::map<int, std::pair<float, float>> PlaybackEngine::takeTrackPeaks()
{
    std::map<int, std::pair<float, float>> out;
    std::lock_guard lock(m_ringMutex);
    while (!m_meterQueue.empty() && m_meterQueue.front().first <= m_ringReadPos) {
        for (const auto& [id, pk] : m_meterQueue.front().second) {
            auto& o = out[id];
            o.first = std::max(o.first, pk.first);
            o.second = std::max(o.second, pk.second);
        }
        m_meterQueue.pop_front();
    }
    return out;
}

void PlaybackEngine::scrubAudio(TimeUs t)
{
    if (m_playing || !m_sink)
        return;
    {
        std::lock_guard lock(m_ringMutex);
        m_scrubAudio = usToSamples(std::max<TimeUs>(0, t), m_sampleRate);
        ++m_audioGeneration; // drop a snippet being mixed
    }
    m_audioCv.notify_all();
    if (!m_scrubSink) {
        m_scrubSink = true;
        m_sink->start(m_device);
    }
    m_scrubStop.start(); // the sink stops a moment after the last scrub
}

void PlaybackEngine::resetRenderer()
{
    ++m_rendererGeneration;
}

void PlaybackEngine::setupAudio()
{
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    m_format.setChannelCount(2);
    m_format.setSampleRate(48000);
    m_format.setSampleFormat(QAudioFormat::Float);
    if (!dev.isNull() && !dev.isFormatSupported(m_format)) {
        m_format.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(m_format))
            m_format.setSampleRate(dev.preferredFormat().sampleRate());
    }
    m_sampleRate = m_format.sampleRate();
    m_ring.assign(size_t(m_sampleRate * kRingSeconds) * 2, 0.0f);
    m_device = new AudioRingDevice(this);
    m_device->open(QIODevice::ReadOnly);
    followDefaultOutput();
    connect(new QMediaDevices(this), &QMediaDevices::audioOutputsChanged, this, &PlaybackEngine::followDefaultOutput);
}

void PlaybackEngine::followDefaultOutput()
{
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.id() == m_outputId && (m_sink || dev.isNull()))
        return;
    reopenAudioOutput();
}

void PlaybackEngine::reopenAudioOutput()
{
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (m_sink) {
        m_sink->stop();
        delete m_sink;
        m_sink = nullptr;
    }
    m_scrubSink = false;
    m_scrubStop.stop();
    m_outputId = dev.id();
    // The same rate (the mixing thread runs at it), in a sample format the device takes.
    QAudioFormat format = m_format;
    format.setSampleFormat(QAudioFormat::Float);
    if (!dev.isNull() && !dev.isFormatSupported(format))
        format.setSampleFormat(QAudioFormat::Int16);
    if (dev.isNull() || !dev.isFormatSupported(format))
        return; // silent until a device that takes this rate (the picture plays on)
    m_format = format;
    m_sink = new QAudioSink(dev, m_format, this);
    m_sink->setBufferSize(m_format.bytesForDuration(80000)); // 80 ms device buffer
    if (m_playing) {
        // Carry on from the sound already played: the new device counts from 0.
        {
            std::lock_guard lock(m_ringMutex);
            m_startPos = samplesToUs(m_ringReadPos, m_sampleRate);
        }
        m_clockAdjust = 0;
        m_wallClock.start();
        m_sink->start(m_device);
    }
}

void PlaybackEngine::setSnapshot(std::shared_ptr<const ProjectSnapshot> snap)
{
    bool projectChanged;
    {
        std::lock_guard lock(m_mutex);
        projectChanged = m_snap && snap && m_snap->session != snap->session;
        m_snap = std::move(snap);
        m_snapChanged = std::chrono::steady_clock::now();
        if (projectChanged) {
            ++m_generation;
            m_queue.clear();
        }
    }
    if (projectChanged && m_scrubSink) {
        m_scrubStop.stop();
        if (m_sink) m_sink->stop();
        m_scrubSink = false;
    }
    std::lock_guard lock(m_ringMutex);
    if (projectChanged) {
        ++m_audioGeneration; // discard a block still mixing the previous project
        m_ringFill = 0;
        m_scrubAudio.reset();
        m_meterQueue.clear();
        m_peakL = m_peakR = 0;
    }
    m_audioCv.notify_all();
}

void PlaybackEngine::setPreviewDivisor(int divisor)
{
    std::lock_guard lock(m_mutex);
    m_divisor = std::max(1, divisor);
}

QSize PlaybackEngine::previewSize(const ProjectSnapshot& s) const
{
    return QSize(std::max(2, s.settings.width / m_divisor), std::max(2, s.settings.height / m_divisor));
}

void PlaybackEngine::requestFrame(TimeUs t)
{
    if (m_playing)
        return; // playback renders on its own
    m_cursorHint = t;
    {
        std::lock_guard lock(m_mutex);
        m_scrubRequest = t;
    }
    m_videoCv.notify_one();
}

void PlaybackEngine::play(TimeUs from)
{
    stop();
    if (m_scrubSink && m_sink) {
        m_scrubStop.stop();
        m_sink->stop();
        m_scrubSink = false;
    }
    std::shared_ptr<const ProjectSnapshot> snap;
    {
        std::lock_guard lock(m_mutex);
        snap = m_snap;
        if (!snap)
            return;
        ++m_generation;
        m_queue.clear();
        m_nextFrame = snap->settings.fps.frameAt(from);
        m_scrubRequest.reset();
    }
    {
        std::lock_guard lock(m_ringMutex);
        ++m_audioGeneration;
        m_ringRead = 0;
        m_ringFill = 0;
        m_audioPos = usToSamples(from, m_sampleRate);
        m_ringReadPos = m_audioPos;
        m_meterQueue.clear();
        m_scrubAudio.reset();
    }
    m_startPos = from;
    m_clock = from;
    m_cursorHint = from;
    m_clockAdjust = 0;
    m_playing = true;
    // Take each lock once so the worker threads can't miss the wake-up.
    { std::lock_guard lock(m_mutex); }
    { std::lock_guard lock(m_ringMutex); }
    m_videoCv.notify_all();
    m_audioCv.notify_all();
    if (m_sink)
        m_sink->start(m_device);
    m_wallClock.start();
    m_tick.start();
}

void PlaybackEngine::stop()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_tick.stop();
    if (m_sink)
        m_sink->stop();
    {
        std::lock_guard lock(m_mutex);
        ++m_generation;
        m_queue.clear();
    }
    {
        std::lock_guard lock(m_ringMutex);
        ++m_audioGeneration;
        m_ringFill = 0;
    }
}

TimeUs PlaybackEngine::heardPosition()
{
    // The sound handed to the device, less what it hasn't played yet. (Not the device's processed time: that counts the
    // silence played while the mix catches up, such as the first use of a pitch shift measuring its delay, and the
    // picture then ran ahead of the sound for the rest of the playback.)
    int64_t handed;
    {
        std::lock_guard lock(m_ringMutex);
        handed = m_ringReadPos;
    }
    const qint64 buffered = m_sink ? m_format.durationForBytes(std::max<qint64>(0, m_sink->bufferSize() - m_sink->bytesFree())) : 0;
    return samplesToUs(handed, m_sampleRate) - buffered;
}

TimeUs PlaybackEngine::computeClock()
{
    TimeUs wall = m_startPos + m_wallClock.nsecsElapsed() / 1000 + m_clockAdjust;
    if (m_sink && m_sink->state() != QAudio::StoppedState) {
        if (m_sink->processedUSecs() > 0) {
            const TimeUs audioClock = heardPosition();
            // The device reports in coarse steps; follow it smoothly.
            const TimeUs diff = audioClock - wall;
            if (std::abs(diff) > 60000)
                m_clockAdjust += diff;
            else
                m_clockAdjust += diff / 16;
            wall = m_startPos + m_wallClock.nsecsElapsed() / 1000 + m_clockAdjust;
        }
    }
    // (the device reports what it has taken, not what has been heard)
    return std::max(m_startPos, wall - m_outputLatency);
}

void PlaybackEngine::onTick()
{
    if (!m_playing)
        return;
    const TimeUs clock = computeClock();
    m_clock = clock;
    std::optional<std::pair<TimeUs, QImage>> due;
    {
        std::lock_guard lock(m_mutex);
        while (!m_queue.empty() && m_queue.front().first <= clock) {
            due = std::move(m_queue.front());
            m_queue.pop_front();
        }
    }
    if (due) {
        m_videoCv.notify_one();
        emit frameReady(due->second, due->first);
    }
    emit positionChanged(clock);
}

void PlaybackEngine::videoThreadMain()
{
    std::unique_ptr<FrameRenderer> renderer;
    int rendererGeneration = -1;
    // Called without the lock held: creating a GPU context takes a moment.
    auto ensureRenderer = [&] {
        if (renderer && rendererGeneration == m_rendererGeneration)
            return;
        rendererGeneration = m_rendererGeneration;
        renderer.reset();
        QString error;
        renderer = createFrameRenderer(false, m_gpuSurface.get(), &error);
        renderer->setUseProxies(true); // the preview: proxies where there are any (and they are on)
        QString name = renderer->name();
        if (!renderer->isGpu() && !error.isEmpty())
            name += QStringLiteral(" (GPU unavailable: %1)").arg(error);
        QMetaObject::invokeMethod(this, [this, name] { emit rendererChanged(name); }, Qt::QueuedConnection);
    };
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_videoCv.wait(lock, [&] {
            return m_quit || (m_scrubRequest && !m_playing) || (m_playing && m_snap && m_queue.size() < kVideoQueueMax);
        });
        if (m_quit)
            return;
        auto snap = m_snap;
        if (!snap) {
            m_scrubRequest.reset();
            continue;
        }
        const QSize size = previewSize(*snap);
        if (m_playing && m_queue.size() < kVideoQueueMax) {
            const FrameRate fps = snap->settings.fps;
            // If rendering fell behind the clock, skip to the next due frame.
            const int64_t clockFrame = fps.frameAt(m_clock.load());
            if (m_nextFrame <= clockFrame)
                m_nextFrame = clockFrame + 1;
            const int64_t frame = m_nextFrame++;
            const uint64_t gen = m_generation;
            const TimeUs t = fps.timeOf(frame);
            lock.unlock();
            QImage img = cachedFrame(snap, t, size);
            if (img.isNull()) {
                ensureRenderer();
                img = renderer->render(*snap, t, size);
            }
            lock.lock();
            if (gen == m_generation && m_playing)
                m_queue.emplace_back(t, std::move(img));
        } else if (m_scrubRequest && !m_playing) {
            const TimeUs t = *m_scrubRequest;
            const uint64_t gen = m_generation;
            m_scrubRequest.reset();
            lock.unlock();
            QImage img = cachedFrame(snap, t, size);
            if (img.isNull()) {
                ensureRenderer();
                img = renderer->render(*snap, t, size);
            }
            QMetaObject::invokeMethod(
                this, [this, img = std::move(img), t, gen] {
                    { std::lock_guard guard(m_mutex); if (gen != m_generation) return; }
                    emit frameReady(img, t);
                }, Qt::QueuedConnection);
            lock.lock();
        }
    }
}

QImage PlaybackEngine::cachedFrame(const std::shared_ptr<const ProjectSnapshot>& snap, TimeUs t, QSize size)
{
    if (!m_preRender)
        return {};
    QByteArray key;
    {
        std::lock_guard lock(m_preMutex);
        if (m_preSnap != snap || m_preSize != size || m_preProxies != proxy::enabled()) // (not worked out for this project yet)
            return {};
        const precache::Stretch* s = precache::stretchAt(m_stretches, t);
        if (!s)
            return {};
        key = s->key;
    }
    QImage img = m_store.load(key, snap->settings.fps.frameAt(t));
    return img.size() == size ? img : QImage();
}

void PlaybackEngine::setPreRender(bool on)
{
    m_preRender = on;
    m_preCv.notify_all();
    if (!on)
        QMetaObject::invokeMethod(this, [this] { emit preRenderChanged({}); }, Qt::QueuedConnection);
}

void PlaybackEngine::clearPreRender()
{
    m_store.clear();
    ++m_preCleared;
    {
        std::lock_guard lock(m_mutex);
        m_snapChanged = std::chrono::steady_clock::now(); // (looked at again, from the start)
    }
    m_preCv.notify_all();
}

void PlaybackEngine::preRenderThreadMain()
{
    using namespace std::chrono;
    constexpr TimeUs kBehind = 5 * kUsPerSecond, kAhead = 90 * kUsPerSecond; // round the cursor
    constexpr qint64 kCacheBytes = qint64(4) << 30;                           // 4 GB on disk at most
    std::unique_ptr<FrameRenderer> renderer;
    int rendererGeneration = -1;
    std::map<QByteArray, bool> heavy; // by stretch key: measured slower than the frame rate allows
    std::vector<PreRenderSpan> shown;
    int rounds = 0, cleared = m_preCleared;
    for (;;) {
        if (cleared != m_preCleared) { // (cleared: measured again)
            cleared = m_preCleared;
            heavy.clear();
        }
        std::shared_ptr<const ProjectSnapshot> snap;
        QSize size;
        {
            std::unique_lock lock(m_mutex);
            m_preCv.wait_for(lock, milliseconds(400), [&] { return m_quit; });
            if (m_quit)
                return;
            // Not while playing, and not until an edit has settled (dragging a knob).
            if (m_playing || !m_preRender || !m_snap || steady_clock::now() - m_snapChanged < milliseconds(900))
                continue;
            snap = m_snap;
            size = previewSize(*snap);
        }
        const bool proxies = proxy::enabled();
        std::vector<precache::Stretch> list;
        {
            std::unique_lock lock(m_preMutex);
            if (m_preSnap != snap || m_preSize != size || m_preProxies != proxies) {
                lock.unlock();
                list = precache::stretches(*snap, size, proxies);
                lock.lock();
                m_stretches = list;
                m_preSnap = snap;
                m_preSize = size;
                m_preProxies = proxies;
            } else {
                list = m_stretches;
            }
        }
        const FrameRate fps = snap->settings.fps;
        const TimeUs fd = fps.frameDuration();
        // (Tests: MVP_PRERENDER_ALL=<seconds> makes every stretch heavy and looks that far ahead only.)
        const double testAhead = qEnvironmentVariable("MVP_PRERENDER_ALL").toDouble();
        const TimeUs cursor = m_cursorHint, from = std::max<TimeUs>(0, cursor - (testAhead > 0 ? 0 : kBehind)),
                     to = cursor + (testAhead > 0 ? TimeUs(testAhead * kUsPerSecond) : kAhead);
        auto stale = [&] {
            std::lock_guard lock(m_mutex);
            return m_quit || m_playing || !m_preRender || m_snap != snap || m_cursorHint < from || m_cursorHint > std::max(from, to - kBehind);
        };
        auto spans = [&] { // the heavy stretches round the cursor: runs of frames done and not done
            std::vector<PreRenderSpan> out;
            for (const precache::Stretch& s : list) {
                const auto h = heavy.find(s.key);
                if (h == heavy.end() || !h->second || s.end <= from || s.start >= to)
                    continue;
                const TimeUs a = std::max(s.start, from), b = std::min(s.end, to);
                for (int64_t f = fps.frameAt(a); fps.timeOf(f) < b; ++f) {
                    const bool done = m_store.has(s.key, f);
                    const TimeUs t0 = std::max(a, fps.timeOf(f)), t1 = std::min(b, fps.timeOf(f + 1));
                    if (!out.empty() && out.back().done == done && out.back().end == t0)
                        out.back().end = t1;
                    else
                        out.push_back({t0, t1, done});
                }
            }
            return out;
        };
        auto publish = [&] {
            std::vector<PreRenderSpan> now = spans();
            if (now == shown)
                return;
            shown = now;
            QMetaObject::invokeMethod(this, [this, now] { emit preRenderChanged(now); }, Qt::QueuedConnection);
        };
        auto ensureRenderer = [&] {
            if (renderer && rendererGeneration == m_rendererGeneration)
                return;
            rendererGeneration = m_rendererGeneration;
            renderer = createFrameRenderer(false, m_preSurface.get(), nullptr);
            renderer->setUseProxies(true); // (as the preview plays)
        };
        // Nearest first: from the cursor on, then what is just before it.
        std::vector<const precache::Stretch*> order;
        for (const precache::Stretch& s : list)
            if (s.end > from && s.start < to)
                order.push_back(&s);
        std::stable_sort(order.begin(), order.end(), [&](const precache::Stretch* x, const precache::Stretch* y) {
            return (x->end <= cursor) < (y->end <= cursor);
        });
        bool worked = false;
        for (const precache::Stretch* s : order) {
            if (stale())
                break;
            const TimeUs a = std::max(s->start, from), b = std::min(s->end, to);
            if (!heavy.count(s->key)) {
                // Heavy: a frame at its start or middle takes longer than a frame lasts (well over: playing has other work).
                // Timed on the frame after one rendered first, so opening and seeking the files isn't counted.
                ensureRenderer();
                double worst = 0;
                for (const TimeUs t : {a, a + (b - a) / 2}) {
                    const int64_t f = fps.frameAt(t);
                    const QImage first = renderer->render(*snap, fps.timeOf(f), size);
                    if (fps.timeOf(f + 1) >= b)
                        continue;
                    const auto t0 = steady_clock::now();
                    const QImage next = renderer->render(*snap, fps.timeOf(f + 1), size);
                    const double took = duration<double, std::micro>(steady_clock::now() - t0).count();
                    worst = std::max(worst, took);
                    if (took > std::max(25000.0, 0.7 * double(fd))) {
                        m_store.save(s->key, f, first);
                        m_store.save(s->key, f + 1, next);
                    }
                }
                heavy[s->key] = testAhead > 0 || worst > std::max(25000.0, 0.7 * double(fd));
                worked = true;
            }
            if (!heavy[s->key])
                continue;
            int n = 0;
            for (int64_t f = fps.frameAt(a); fps.timeOf(f) < b; ++f) {
                if (m_store.has(s->key, f))
                    continue;
                if (stale())
                    break;
                ensureRenderer();
                m_store.save(s->key, f, renderer->render(*snap, fps.timeOf(f), size));
                worked = true;
                if (++n % 12 == 0)
                    publish();
            }
        }
        publish();
        if (worked && ++rounds % 8 == 0)
            m_store.trim(kCacheBytes);
    }
}

void PlaybackEngine::audioThreadMain()
{
    AudioMixer mixer(m_sampleRate);
    std::vector<float> chunk(size_t(kAudioChunk) * 2);
    std::unique_lock lock(m_ringMutex);
    for (;;) {
        const size_t ringFrames = m_ring.size() / 2;
        m_audioCv.wait(lock, [&] {
            bool quit;
            {
                // m_quit is guarded by m_mutex; reading a bool racily here is
                // avoided by checking under that lock.
                std::lock_guard g(m_mutex);
                quit = m_quit;
            }
            return quit || (m_playing && ringFrames - m_ringFill >= size_t(kAudioChunk)) || (!m_playing && m_scrubAudio);
        });
        std::shared_ptr<const ProjectSnapshot> snap;
        {
            std::lock_guard g(m_mutex);
            if (m_quit)
                return;
            snap = m_snap;
        }
        if (!snap) {
            m_scrubAudio.reset();
            continue;
        }
        if (!m_playing && m_scrubAudio) {
            // A scrub snippet replaces whatever is still queued, with short
            // fades so it doesn't click.
            const int64_t from = *m_scrubAudio;
            m_scrubAudio.reset();
            const uint64_t gen = m_audioGeneration;
            const int n = std::min(int(m_sampleRate * kScrubSeconds), int(ringFrames));
            std::vector<float> snippet(size_t(n) * 2);
            lock.unlock();
            for (int done = 0; done < n; done += kAudioChunk)
                mixer.mix(*snap, from + done, std::min(kAudioChunk, n - done), snippet.data() + size_t(done) * 2);
            mixer.takeTrackPeaks();
            const int ramp = std::max(1, m_sampleRate / 250); // 4 ms
            for (int i = 0; i < n; ++i) {
                const float g = std::min({1.0f, float(i) / float(ramp), float(n - 1 - i) / float(ramp)});
                snippet[size_t(i) * 2] *= g;
                snippet[size_t(i) * 2 + 1] *= g;
            }
            lock.lock();
            if (gen != m_audioGeneration || m_playing)
                continue;
            for (int i = 0; i < n; ++i) {
                m_ring[size_t(i) * 2] = snippet[size_t(i) * 2];
                m_ring[size_t(i) * 2 + 1] = snippet[size_t(i) * 2 + 1];
            }
            m_ringRead = 0;
            m_ringFill = size_t(n);
            continue;
        }
        const int64_t pos = m_audioPos;
        const uint64_t gen = m_audioGeneration;
        lock.unlock();
        mixer.mix(*snap, pos, kAudioChunk, chunk.data());
        auto trackPeaks = mixer.takeTrackPeaks();
        lock.lock();
        if (gen != m_audioGeneration || !m_playing)
            continue;
        m_meterQueue.emplace_back(pos + kAudioChunk, std::move(trackPeaks));
        const size_t writeStart = (m_ringRead + m_ringFill) % ringFrames;
        for (size_t i = 0; i < size_t(kAudioChunk); ++i) {
            const size_t idx = ((writeStart + i) % ringFrames) * 2;
            m_ring[idx] = chunk[i * 2];
            m_ring[idx + 1] = chunk[i * 2 + 1];
        }
        m_ringFill += kAudioChunk;
        m_audioPos += kAudioChunk;
    }
}
