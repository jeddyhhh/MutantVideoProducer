#include "engine/LiveEngine.h"

#include "core/Compositor.h"
#include "core/Fft.h"
#include "core/FrameRenderer.h"
#include "core/LiveRecorder.h"
#include "core/VideoShare.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFont>
#include <QOffscreenSurface>
#include <QPainter>
#include <QPainterPath>
#include <chrono>
#include <cmath>
#include <map>

namespace {

using Clock = std::chrono::steady_clock;

// Where the layer goes in a picture of `out` size.
QRect layerRect(QSize out, QSize layer, const LiveLayerSettings& s)
{
    if (s.mode == LiveLayerSettings::Mode::Full || layer.isEmpty())
        return QRect(QPoint(0, 0), out);
    const int w = std::max(16, int(std::lround(out.width() * std::clamp(s.size, 0.05, 1.0))));
    const int h = std::max(9, int(std::lround(double(w) * layer.height() / layer.width())));
    const int m = int(std::lround(out.width() * 0.03));
    switch (s.corner) {
    case 0: return QRect(m, m, w, h);
    case 1: return QRect(out.width() - m - w, m, w, h);
    case 2: return QRect(m, out.height() - m - h, w, h);
    case 4: return QRect((out.width() - w) / 2, (out.height() - h) / 2, w, h);
    default: return QRect(out.width() - m - w, out.height() - m - h, w, h);
    }
}

// What the visualiser overlays draw: the spectrum (64 bands, 0..1, smoothed)
// and the waveform of the last moment (-1..1, auto-scaled).
struct AudioView {
    std::vector<float> bands = std::vector<float>(64, 0.0f);
    std::vector<float> wave;

    // From the newest `samples` (mono, 48 kHz).
    void update(const std::vector<float>& samples)
    {
        const size_t n = 2048;
        if (samples.size() < n)
            return;
        std::vector<std::complex<float>> f(n);
        const size_t from = samples.size() - n;
        for (size_t i = 0; i < n; ++i)
            f[i] = samples[from + i] * float(0.5 - 0.5 * std::cos(2 * 3.14159265 * double(i) / double(n - 1)));
        fftInPlace(f);
        const double rate = 48000, binHz = rate / double(n);
        for (size_t b = 0; b < bands.size(); ++b) {
            const double lo = 40 * std::pow(400.0, double(b) / bands.size()), hi = 40 * std::pow(400.0, double(b + 1) / bands.size());
            const size_t k0 = std::max<size_t>(1, size_t(lo / binHz)), k1 = std::max(k0 + 1, size_t(hi / binHz));
            double e = 0;
            for (size_t k = k0; k < k1 && k < n / 2; ++k)
                e = std::max(e, double(std::abs(f[k])));
            const double db = 20 * std::log10(e * 4.0 / double(n) + 1e-9);
            const float v = float(std::clamp((db + 66) / 54, 0.0, 1.0)); // -66 dB .. -12 dB
            bands[b] = std::max(v, bands[b] * 0.86f);                     // falls back gently
        }
        // The waveform: the last ~40 ms, scaled to its loudest so quiet sound still shows.
        const size_t w = 1920;
        wave.assign(samples.end() - std::ptrdiff_t(w), samples.end());
        float peak = 0.05f;
        for (float s : wave)
            peak = std::max(peak, std::abs(s));
        for (float& s : wave)
            s /= peak;
    }
};

// Text, pictures, the tempo, the time or the audio over the output.
void drawOverlays(QImage& out, const std::vector<LiveOverlay>& overlays, double bpm, double beatPhase, std::map<QString, QImage>& pictures,
                  const AudioView& audio)
{
    if (overlays.empty())
        return;
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double margin = out.height() * 0.03;
    const double pulse = std::exp(-5.0 * std::clamp(beatPhase, 0.0, 1.0));
    for (const LiveOverlay& ov : overlays) {
        if (!ov.enabled)
            continue;
        const double grow = ov.pulse ? 1.0 + 0.15 * pulse : 1.0;
        const double height = out.height() * std::clamp(ov.size, 1.0, 100.0) / 100.0 * grow;
        QRectF box;
        QPainterPath text;
        QImage picture;
        const bool visual = ov.kind == LiveOverlay::Kind::Spectrum || ov.kind == LiveOverlay::Kind::Waveform || ov.kind == LiveOverlay::Kind::Circle;
        if (visual) {
            if (ov.kind == LiveOverlay::Kind::Circle)
                box = QRectF(0, 0, height * 1.6, height * 1.6);
            else
                box = QRectF(0, 0, out.width() * std::clamp(ov.width, 5.0, 100.0) / 100.0 * grow, height);
        } else if (ov.kind == LiveOverlay::Kind::Image) {
            auto it = pictures.find(ov.image);
            if (it == pictures.end())
                it = pictures.emplace(ov.image, QImage(ov.image).convertToFormat(QImage::Format_ARGB32_Premultiplied)).first;
            picture = it->second;
            if (picture.isNull())
                continue;
            box = QRectF(0, 0, height * picture.width() / std::max(1, picture.height()), height);
        } else {
            QString s = ov.text;
            if (ov.kind == LiveOverlay::Kind::Bpm)
                s = bpm > 0 ? QStringLiteral("%1 BPM").arg(bpm, 0, 'f', 1) : QStringLiteral("--- BPM");
            else if (ov.kind == LiveOverlay::Kind::Clock)
                s = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
            if (s.isEmpty())
                continue;
            QFont f(QStringLiteral("Segoe UI"));
            f.setBold(true);
            f.setPixelSize(std::max(4, int(height)));
            int line = 0;
            for (const QString& l : s.split(QLatin1Char('\n'))) {
                text.addText(0, f.pixelSize() * 0.8 + line * f.pixelSize() * 1.15, f, l);
                ++line;
            }
            box = text.boundingRect();
        }
        const int col = ov.anchor % 3, row = ov.anchor / 3;
        const double x = col == 0 ? margin : col == 1 ? (out.width() - box.width()) / 2 : out.width() - margin - box.width();
        const double y = row == 0 ? margin : row == 1 ? (out.height() - box.height()) / 2 : out.height() - margin - box.height();
        p.save();
        p.setOpacity(std::clamp(ov.opacity, 0.0, 1.0));
        p.translate(x - box.left(), y - box.top());
        const QColor color(QRgb(0xff000000u | uint32_t(ov.color)));
        if (visual) {
            const std::vector<float>& bands = audio.bands;
            if (ov.kind == LiveOverlay::Kind::Spectrum) { // bars from the bottom of the box
                const int count = 32;
                const double slot = box.width() / count;
                for (int i = 0; i < count; ++i) {
                    const float v = std::max(bands[size_t(i * 2)], bands[size_t(i * 2 + 1)]);
                    const double h = std::max(1.0, v * box.height());
                    p.fillRect(QRectF(box.left() + i * slot + slot * 0.12, box.bottom() - h, slot * 0.76, h), color);
                }
            } else if (ov.kind == LiveOverlay::Kind::Waveform && !audio.wave.empty()) {
                QPainterPath line;
                const size_t n = audio.wave.size(), steps = 400;
                for (size_t i = 0; i <= steps; ++i) {
                    const double x = box.left() + box.width() * double(i) / steps;
                    const double y = box.center().y() - audio.wave[std::min(n - 1, i * n / steps)] * box.height() / 2;
                    i ? line.lineTo(x, y) : line.moveTo(x, y);
                }
                p.setPen(QPen(color, std::max(1.5, height * 0.025), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.setBrush(Qt::NoBrush);
                p.drawPath(line);
            } else if (ov.kind == LiveOverlay::Kind::Circle) { // bars around a ring
                const QPointF c = box.center();
                const double r = box.width() / 3.2;
                p.setPen(QPen(color, std::max(1.0, r * 0.03)));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(c, r, r);
                const int count = 64;
                p.setPen(QPen(color, std::max(1.5, r * 2 * 3.14159 / count * 0.55), Qt::SolidLine, Qt::FlatCap));
                for (int i = 0; i < count; ++i) {
                    const double a = 2 * 3.14159265 * i / count - 3.14159265 / 2;
                    const float v = bands[size_t((i < count / 2 ? i : count - 1 - i) * 2)]; // mirrored: lows at the top and bottom
                    const QPointF dir(std::cos(a), std::sin(a));
                    p.drawLine(c + dir * (r * 1.05), c + dir * (r * (1.08 + 0.55 * v)));
                }
            }
        } else if (!picture.isNull()) {
            p.drawImage(box, picture);
        } else {
            p.setPen(QPen(QColor(0, 0, 0, 200), std::max(1.0, height * 0.08), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            p.setBrush(Qt::NoBrush);
            p.drawPath(text); // an outline, readable over anything
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawPath(text);
        }
        p.restore();
    }
}

// The audio input's level, bass, mids and highs, scaled to the loudest of
// the last few seconds (0..1).
struct LevelFollower {
    BandFollower bands{LiveAudioCapture::kSampleRate};
    double peak[4] = {1e-4, 1e-4, 1e-4, 1e-4};

    void process(const float* stereo, int frames, float out[4])
    {
        if (frames <= 0)
            return;
        bands.process(stereo, frames);
        const double forget = std::exp(-frames / bands.rate / 6.0);
        for (int k = 0; k < 4; ++k) {
            peak[k] = std::max({bands.env[k], peak[k] * forget, 1e-4});
            out[k] = float(std::clamp(bands.env[k] / peak[k], 0.0, 1.0));
        }
    }
};

} // namespace

LiveEngine::LiveEngine(QObject* parent) : QObject(parent)
{
    m_gpuSurface = createGpuSurface(); // may be null: CPU rendering then
}

LiveEngine::~LiveEngine()
{
    stop();
}

void LiveEngine::setSnapshot(std::shared_ptr<const ProjectSnapshot> snap)
{
    std::lock_guard lock(m_mutex);
    if (m_snap && snap && m_snap->session != snap->session)
        ++m_rendererGeneration;
    m_snap = std::move(snap);
}

void LiveEngine::start(const Options& options)
{
    stop();
    m_quit = false;
    m_running = true;
    m_mirror = options.mirror;
    m_fps = std::max(1.0, options.fps);
    {
        std::lock_guard lock(m_mutex);
        m_maxSize = options.maxSize;
        m_maxSizeChanged = false;
        m_canvas = options.canvas;
        m_framing = options.framing;
        m_layerSettings = options.layerSettings;
        m_shareChange = true; // sharing chosen before Start
    }
    m_thread = std::thread(&LiveEngine::threadMain, this, options);
}

void LiveEngine::stop()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_cv.notify_all();
    if (m_thread.joinable())
        m_thread.join();
    if (m_saveThread.joinable())
        m_saveThread.join();
    m_quit = false;
    m_running = false;
}

void LiveEngine::setMaxSize(QSize size)
{
    std::lock_guard lock(m_mutex);
    m_maxSize = size;
    m_maxSizeChanged = true;
}

void LiveEngine::setPanZoom(const LivePanZoom& pz)
{
    std::lock_guard lock(m_mutex);
    m_panZoom = pz;
}

LivePanZoom LiveEngine::panZoom() const
{
    std::lock_guard lock(m_mutex);
    return m_panZoom;
}

void LiveEngine::setCanvas(QSize canvas, LiveFraming framing)
{
    std::lock_guard lock(m_mutex);
    m_canvas = canvas;
    m_framing = framing;
    m_maxSizeChanged = true;
}

QImage LiveEngine::cleanFrame() const
{
    if (!m_running)
        return {};
    std::lock_guard lock(m_mutex);
    return m_clean;
}

void LiveEngine::startRecording(const QString& path, bool withEffects)
{
    {
        std::lock_guard lock(m_mutex);
        m_recordPath = path;
        m_recordEffects = withEffects;
        m_recordStart = true;
        m_recordStop = false;
    }
    m_recording = true;
    m_cv.notify_all();
}

void LiveEngine::stopRecording()
{
    {
        std::lock_guard lock(m_mutex);
        m_recordStart = false;
        m_recordStop = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setAudioSource(const LiveAudioSource& source)
{
    {
        std::lock_guard lock(m_mutex);
        m_audioRequest = source;
        m_audioChange = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setPad(int pad, const TransitionSpec& transition, double seconds)
{
    {
        std::lock_guard lock(m_mutex);
        m_padRequest = pad >= 0 && pad < kLivePads ? pad : -1;
        m_padTransition = transition;
        m_padSeconds = std::max(0.0, seconds);
        m_padChange = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setHeldPads(const std::vector<int>& pads)
{
    std::lock_guard lock(m_mutex);
    m_held.clear();
    for (int p : pads)
        if (p >= 0 && p < kLivePads)
            m_held.push_back(p);
}

void LiveEngine::setLayer(const LiveSource& source)
{
    {
        std::lock_guard lock(m_mutex);
        m_layerRequest = source;
        m_layerChange = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setLayerSettings(const LiveLayerSettings& settings)
{
    std::lock_guard lock(m_mutex);
    m_layerSettings = settings;
}

void LiveEngine::nextFile(bool layer)
{
    std::lock_guard lock(m_mutex);
    (layer ? m_nextLayerFile : m_nextFile) = true;
}

void LiveEngine::noteBeat(double bpm, int barBeat)
{
    std::lock_guard lock(m_clockMutex);
    const auto now = Clock::now();
    const double period = 60.0 / std::max(20.0, m_clockBpm);
    // The beat this is: the one the clock expected nearest now, and always a new one.
    double count = std::round(m_beatCount + std::chrono::duration<double>(now - m_lastBeat).count() / period);
    if (count <= std::floor(m_beatCount))
        count = std::floor(m_beatCount) + 1;
    m_beatCount = count;
    m_lastBeat = now;
    m_barBeat = ((barBeat % 4) + 4) % 4;
    if (bpm > 0)
        m_clockBpm = std::clamp(bpm, 20.0, 400.0);
}

void LiveEngine::setTempo(double bpm)
{
    std::lock_guard lock(m_clockMutex);
    if (bpm <= 0)
        return;
    // Carry on from where the clock is now, at the new tempo.
    const auto now = Clock::now();
    const double before = m_beatCount;
    m_beatCount += std::chrono::duration<double>(now - m_lastBeat).count() / (60.0 / m_clockBpm);
    m_barBeat = int((m_barBeat + int64_t(std::floor(m_beatCount)) - int64_t(std::floor(before))) % 4);
    m_lastBeat = now;
    m_clockBpm = std::clamp(bpm, 20.0, 400.0);
}

void LiveEngine::setMidiController(int channel, int controller, int value)
{
    if (channel < 0 || channel > 15 || controller < 0 || controller > 127)
        return;
    m_midi[size_t(channel)][size_t(controller)] = uint8_t(std::clamp(value, 0, 127));
}

ModInputs LiveEngine::modInputs() const
{
    ModInputs in;
    {
        std::lock_guard lock(m_clockMutex);
        const double since = std::chrono::duration<double>(Clock::now() - m_lastBeat).count() / (60.0 / m_clockBpm);
        in.beats = m_beatCount + since;
        in.barBeat = int((m_barBeat + int64_t(std::floor(in.beats)) - int64_t(std::floor(m_beatCount))) % 4);
    }
    in.level = m_level;
    in.bass = m_bass;
    in.mids = m_mids;
    in.highs = m_highs;
    for (size_t c = 0; c < 16; ++c)
        for (size_t k = 0; k < 128; ++k)
            in.midi[c][k] = m_midi[c][k];
    return in;
}

void LiveEngine::setReplay(bool on, double seconds, bool withEffects)
{
    {
        std::lock_guard lock(m_mutex);
        m_replayWanted = on;
        m_replaySeconds = seconds;
        m_replayEffects = withEffects;
        m_replayChange = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setSharing(bool spout, const QString& spoutName, bool virtualCamera)
{
    {
        std::lock_guard lock(m_mutex);
        m_spoutWanted = spout;
        m_spoutName = spoutName;
        m_cameraWanted = virtualCamera;
        m_shareChange = true;
        m_sharing = spout || virtualCamera;
    }
    m_cv.notify_all();
}

void LiveEngine::setCue(int kind, int index)
{
    m_cueIndex = index;
    m_cueKind = kind;
}

void LiveEngine::startStream(const QString& url, int kbps)
{
    {
        std::lock_guard lock(m_mutex);
        m_streamUrl = url;
        m_streamKbps = kbps;
        m_streamStart = true;
        m_streamStop = false;
        m_streaming = true;
    }
    m_cv.notify_all();
}

void LiveEngine::stopStream()
{
    {
        std::lock_guard lock(m_mutex);
        m_streamStart = false;
        m_streamStop = true;
    }
    m_cv.notify_all();
}

void LiveEngine::saveReplay(const QString& path)
{
    {
        std::lock_guard lock(m_mutex);
        m_replaySavePath = path;
    }
    m_cv.notify_all();
}

void LiveEngine::setInputs(const std::vector<LiveSource>& inputs)
{
    {
        std::lock_guard lock(m_mutex);
        m_inputsRequest = inputs;
        m_inputsChange = true;
    }
    m_cv.notify_all();
}

void LiveEngine::setProgram(int input, const TransitionSpec& transition, double seconds)
{
    {
        std::lock_guard lock(m_mutex);
        m_programRequest = std::clamp(input, 0, kMaxInputs);
        m_programTransition = transition;
        m_programSeconds = seconds;
        m_programChange = true;
    }
    m_program = std::clamp(input, 0, kMaxInputs);
    m_cv.notify_all();
}

void LiveEngine::setPaused(bool paused)
{
    m_paused = paused;
    m_cv.notify_all();
}

void LiveEngine::threadMain(Options o)
{
    LiveCapture capture;
    QString error;
    if (!capture.start(o.source, o.format, o.maxSize, &error)) {
        m_running = false;
        QMetaObject::invokeMethod(this, [this, error] { emit failed(error); }, Qt::QueuedConnection);
        return;
    }
    QMetaObject::invokeMethod(this, [this, d = capture.description()] { emit started(d); }, Qt::QueuedConnection);

    // The second source, laid over the first.
    LiveCapture layer;
    bool layerOpen = false;
    auto openLayer = [&](const LiveSource& src) {
        layer.stop();
        layerOpen = false;
        QString desc, err;
        if (!src.name.isEmpty()) {
            QSize box;
            {
                std::lock_guard lock(m_mutex);
                box = m_canvas.isValid() ? m_canvas : m_maxSize;
            }
            layerOpen = layer.start(src, LiveFormat(), box, &err);
            if (layerOpen)
                desc = layer.description();
        }
        QMetaObject::invokeMethod(this, [this, desc, err] { emit layerStatus(desc, err); }, Qt::QueuedConnection);
    };
    {
        std::lock_guard lock(m_mutex);
        m_layerChange = false;
        m_nextFile = m_nextLayerFile = false;
    }
    openLayer(o.layer);

    // The switcher's other inputs, and the one on air (blended from the one before while switching).
    std::vector<std::unique_ptr<LiveCapture>> inputs;
    auto openInputs = [&](const std::vector<LiveSource>& list) {
        inputs.clear();
        QSize box;
        {
            std::lock_guard lock(m_mutex);
            box = m_canvas.isValid() ? m_canvas : m_maxSize;
        }
        for (int k = 0; k < std::min(int(list.size()), kMaxInputs); ++k) {
            auto c = std::make_unique<LiveCapture>();
            QString err;
            if (list[size_t(k)].name.isEmpty() || !c->start(list[size_t(k)], LiveFormat(), box, &err))
                c.reset();
            inputs.push_back(std::move(c));
        }
        if (std::none_of(inputs.begin(), inputs.end(), [](const auto& c) { return c != nullptr; }))
            inputs.clear(); // all off: no switcher work per frame
    };
    {
        std::lock_guard lock(m_mutex);
        m_inputsChange = m_programChange = false;
    }
    openInputs(o.inputs);
    int program = std::clamp(o.program, 0, kMaxInputs), fromProgram = program;
    m_program = program;
    bool switching = false;
    Clock::time_point switchStart;
    double switchSeconds = 0;
    TransitionSpec switchSpec;
    std::vector<LiveSource> inputsChange;
    bool changeInputs = false;

    // Sound: read on its own thread and handed straight to the recordings.
    LiveAudioCapture audio;
    bool audioFailureShown = false;
    auto openAudio = [&](const LiveAudioSource& src) {
        audio.stop();
        m_beats->reset(); // a new input: find its tempo afresh
        m_level = m_bass = m_mids = m_highs = 0;
        audioFailureShown = false;
        QString desc, err;
        if (!src.isNull()) {
            const bool ok = audio.start(
                src,
                [this, beats = m_beats, levels = std::make_shared<LevelFollower>()](const float* samples, int frames, double time) {
                    {
                        std::lock_guard lock(m_audioMutex);
                        if (m_audioTarget)
                            m_audioTarget->addAudio(samples, frames, time);
                        if (m_replayAudioTarget)
                            m_replayAudioTarget->addAudio(samples, frames, time);
                        if (m_streamAudioTarget)
                            m_streamAudioTarget->addAudio(samples, frames, time);
                    }
                    float values[4] = {0, 0, 0, 0};
                    levels->process(samples, frames, values);
                    {
                        std::lock_guard lock(m_scopeMutex);
                        for (int i = 0; i < frames; ++i) {
                            m_scope[m_scopePos] = 0.5f * (samples[i * 2] + samples[i * 2 + 1]);
                            m_scopePos = (m_scopePos + 1) % m_scope.size();
                        }
                    }
                    m_level = values[0];
                    m_bass = values[1];
                    m_mids = values[2];
                    m_highs = values[3];
                    // Beats are sent straight from the audio thread: the picture loop is too coarse.
                    beats->process(samples, frames, time);
                    const std::vector<double> found = beats->takeBeats();
                    if (!found.empty())
                        QMetaObject::invokeMethod(this, [this, bpm = beats->bpm(), conf = beats->confidence()] { emit beat(bpm, conf); },
                                                  Qt::QueuedConnection);
                },
                &err);
            if (ok)
                desc = audio.description();
        }
        QMetaObject::invokeMethod(this, [this, desc, err] { emit audioStatus(desc, err); }, Qt::QueuedConnection);
    };
    {
        std::lock_guard lock(m_mutex);
        m_audioChange = false;
    }
    openAudio(o.audio);
    QElapsedTimer levelTimer;
    levelTimer.start();

    std::unique_ptr<FrameRenderer> renderer;
    int rendererGeneration = -1;
    QString rendererName;
    auto next = Clock::now();
    int64_t frame = 0;
    QElapsedTimer statsTimer;
    statsTimer.start();
    int rendered = 0;
    double renderMs = 0;
    QString failure;
    std::unique_ptr<LiveRecorder> recorder;
    QString recordPath;
    bool recordEffects = true, wantRecording = false, restartRecording = false, changeAudio = false;
    // Replay buffer (shared with a thread saving it).
    std::shared_ptr<LiveRecorder> replay;
    bool replayWanted = false, replayEffects = true, restartReplay = false;
    double replaySeconds = 30;
    QString replaySave;
    // Pads: the active one, and the one being blended away from; held momentary ones.
    int pad = o.pad >= 0 && o.pad < kLivePads ? o.pad : -1, fromPad = -1;
    std::vector<int> held;
    bool fading = false;
    Clock::time_point fadeStart;
    double fadeSeconds = 0;
    TransitionSpec fadeSpec;
    LiveLayerSettings layerSettings = o.layerSettings;
    LiveSource layerChange;
    bool changeLayer = false;
    std::map<QString, QImage> overlayPictures;
    AudioView audioView;
    // A fixed output shape (portrait...), the size limit otherwise, the pan and zoom.
    QSize canvas = o.canvas, captureBox(-1, -1), maxSize = o.maxSize;
    LiveFraming framing = o.framing;
    LivePanZoom panZoom = m_panZoom;
    // The cue: its own renderer, so effects that remember (trails, feedback) don't run twice a frame.
    std::unique_ptr<FrameRenderer> cueRenderer;
    int cueRendererGeneration = -1;
    // Streaming.
    std::unique_ptr<LiveRecorder> streamer;
    QString streamUrl;
    int streamKbps = 4500;
    bool wantStream = false, streamLive = false;
    auto stopStreaming = [&](const QString& why) {
        if (streamer) {
            {
                std::lock_guard lock(m_audioMutex);
                m_streamAudioTarget = nullptr;
            }
            streamer->finish();
            streamer.reset();
        }
        streamLive = false;
        m_streaming = false;
        QMetaObject::invokeMethod(this, [this, why] { emit streamStatus(false, false, QString(), why); }, Qt::QueuedConnection);
    };
    // Sharing the output with other apps.
    SpoutSender spout;
    VirtualCamera camera;
    bool spoutWanted = false, cameraWanted = false, shareChange = true;
    QString spoutName, spoutError, cameraError;
    int cameraRetry = 0; // frames until the virtual camera is tried again
    auto shareStatus = [&] {
        QMetaObject::invokeMethod(this, [this, n = spout.isOpen() ? spout.name() : QString(), se = spoutError,
                                         cs = camera.isOpen() ? camera.size() : QSize(), ce = cameraError] {
            emit sharingStatus(n, se, cs, ce);
        }, Qt::QueuedConnection);
    };
    std::vector<float> scope;
    {
        std::lock_guard lock(m_mutex);
        m_padChange = false;
        // A replay buffer switched on before capture starts runs from the first picture.
        replayWanted = m_replayWanted;
        replaySeconds = m_replaySeconds;
        replayEffects = m_replayEffects;
        m_replayChange = false;
        m_replaySavePath.clear();
    }
    auto finishRecording = [&] {
        if (!recorder)
            return;
        {
            std::lock_guard lock(m_audioMutex);
            m_audioTarget = nullptr;
        }
        QString err;
        const bool ok = recorder->finish(&err) && recorder->frames() > 0;
        const double secs = recorder->seconds();
        if (ok) {
            const QImage thumb = recorder->thumbnail();
            if (!thumb.isNull())
                thumb.save(liveThumbnailPath(recordPath), "JPG", 85);
        } else if (err.isEmpty()) {
            err = QStringLiteral("nothing was recorded");
        }
        recorder.reset();
        m_recording = false;
        QMetaObject::invokeMethod(this, [this, p = recordPath, ok, err, secs] { emit recordingFinished(p, ok, err, secs); },
                                  Qt::QueuedConnection);
    };
    auto stopReplay = [&] {
        if (!replay)
            return;
        {
            std::lock_guard lock(m_audioMutex);
            m_replayAudioTarget = nullptr;
        }
        if (m_saveThread.joinable())
            m_saveThread.join();
        replay->finish();
        replay.reset();
        m_replayRunning = false;
        QMetaObject::invokeMethod(this, [this] { emit replayStatus(false, QString(), QString()); }, Qt::QueuedConnection);
    };
    while (!m_quit) {
        {
            std::unique_lock lock(m_mutex);
            m_cv.wait_until(lock, next, [&] { return m_quit.load(); });
            if (m_quit)
                break;
            if (m_maxSizeChanged) {
                canvas = m_canvas;
                framing = m_framing;
                maxSize = m_maxSize;
                captureBox = QSize(-1, -1); // chosen again below
                layer.setMaxSize(canvas.isValid() ? canvas : m_maxSize);
                m_maxSizeChanged = false;
            }
            panZoom = m_panZoom;
            if (m_recordStop) {
                m_recordStop = false;
                wantRecording = false;
            }
            if (m_recordStart) {
                m_recordStart = false;
                restartRecording = true;
                recordEffects = m_recordEffects;
            }
            if (m_audioChange) {
                m_audioChange = false;
                o.audio = m_audioRequest;
                changeAudio = true;
            }
            if (m_padChange) {
                m_padChange = false;
                if (m_padRequest != pad) {
                    fading = m_padSeconds > 0 && !m_padTransition.type.isEmpty();
                    fromPad = pad; // mid-fade: from the pad being faded to
                    fadeStart = Clock::now();
                    fadeSeconds = m_padSeconds;
                    fadeSpec = m_padTransition;
                    pad = m_padRequest;
                }
            }
            held = m_held;
            layerSettings = m_layerSettings;
            if (m_layerChange) {
                m_layerChange = false;
                layerChange = m_layerRequest;
                changeLayer = true;
            }
            if (m_nextFile) {
                m_nextFile = false;
                capture.nextFile();
            }
            if (m_inputsChange) {
                m_inputsChange = false;
                inputsChange = m_inputsRequest;
                changeInputs = true;
            }
            if (m_programChange) {
                m_programChange = false;
                if (m_programRequest != program) {
                    switching = m_programSeconds > 0 && !m_programTransition.type.isEmpty();
                    fromProgram = program;
                    switchStart = Clock::now();
                    switchSeconds = m_programSeconds;
                    switchSpec = m_programTransition;
                    program = m_programRequest;
                }
            }
            if (m_nextLayerFile) {
                m_nextLayerFile = false;
                if (layerOpen)
                    layer.nextFile();
            }
            if (m_streamStart) {
                m_streamStart = false;
                wantStream = true;
                streamUrl = m_streamUrl;
                streamKbps = m_streamKbps;
            }
            if (m_streamStop) {
                m_streamStop = false;
                wantStream = false;
            }
            if (m_shareChange) {
                m_shareChange = false;
                shareChange = true;
                spoutWanted = m_spoutWanted;
                cameraWanted = m_cameraWanted;
                if (spoutName != m_spoutName && spout.isOpen())
                    spout.close(); // re-registered under the new name below
                spoutName = m_spoutName;
            }
            if (m_replayChange) {
                m_replayChange = false;
                restartReplay = m_replayWanted != replayWanted || m_replaySeconds != replaySeconds || m_replayEffects != replayEffects;
                replayWanted = m_replayWanted;
                replaySeconds = m_replaySeconds;
                replayEffects = m_replayEffects;
            }
            if (!m_replaySavePath.isEmpty()) {
                replaySave = m_replaySavePath;
                m_replaySavePath.clear();
            }
        }
        if (changeAudio) {
            changeAudio = false;
            openAudio(o.audio);
            if (replay) // the sound track appears or goes: start the buffer again
                restartReplay = true;
        }
        if (changeLayer) {
            changeLayer = false;
            openLayer(layerChange);
        }
        if (changeInputs) {
            changeInputs = false;
            openInputs(inputsChange);
            captureBox = QSize(-1, -1); // (sized below)
        }
        for (auto& in : inputs)
            if (in && !in->failure().isEmpty())
                in.reset(); // unplugged: shown as off
        if (!audio.failure().isEmpty() && !audioFailureShown) { // unplugged...
            audioFailureShown = true;
            QMetaObject::invokeMethod(this, [this, err = audio.failure()] { emit audioStatus(QString(), err); }, Qt::QueuedConnection);
        }
        if (layerOpen && !layer.failure().isEmpty()) {
            layerOpen = false;
            QMetaObject::invokeMethod(this, [this, err = layer.failure()] { emit layerStatus(QString(), err); }, Qt::QueuedConnection);
        }
        if (restartRecording) { // a new file: close the current one first
            restartRecording = false;
            finishRecording();
            wantRecording = true;
        }
        if (!wantRecording)
            finishRecording();
        if (restartReplay || (!replayWanted && replay)) {
            restartReplay = false;
            stopReplay();
        }
        const double fps = m_fps;
        const auto now = Clock::now();
        next += std::chrono::microseconds(int64_t(1e6 / fps));
        if (next < now - std::chrono::milliseconds(250)) // fell far behind: don't try to catch up
            next = now;
        failure = capture.failure();
        if (!failure.isEmpty())
            break;
        if (m_paused && !wantRecording && !replay && !m_sharing && !m_streaming) // keep recording (sharing, streaming) while the Editor tab is shown
            continue;
        // The size to capture at: what the output needs (a fixed shape: only a
        // crop or a fit away), times the zoom, so zooming in keeps the source's detail.
        {
            const QSize source = capture.sourceSize();
            QSize want = !canvas.isValid() ? maxSize : framing == LiveFraming::CropFill ? coverSize(source, canvas) : canvas;
            if (panZoom.zoom > 1.0001 && want.isValid())
                want = QSize(int(std::lround(want.width() * panZoom.zoom)) & ~1, int(std::lround(want.height() * panZoom.zoom)) & ~1);
            if (want != captureBox) {
                captureBox = want;
                capture.setMaxSize(want);
                for (auto& in : inputs)
                    if (in)
                        in->setMaxSize(want);
            }
        }
        QImage img = capture.latest();
        if (img.isNull())
            continue;
        // The switcher: an input's picture, fitted to the main source's (so the
        // output keeps one size), and the one on air in place of the main one.
        if (!inputs.empty()) {
            const QImage mainPicture = img;
            auto picture = [&](int k) -> QImage {
                if (k == 0)
                    return mainPicture;
                if (k > int(inputs.size()) || !inputs[size_t(k - 1)])
                    return {};
                const QImage p = inputs[size_t(k - 1)]->latest();
                if (p.isNull())
                    return {};
                return fitLayer(p.format() == QImage::Format_ARGB32_Premultiplied ? p : p.convertToFormat(QImage::Format_ARGB32_Premultiplied),
                                mainPicture.size(), true);
            };
            QImage onAir = picture(program);
            if (onAir.isNull())
                onAir = mainPicture;
            if (switching) {
                const double progress = std::chrono::duration<double>(Clock::now() - switchStart).count() / std::max(0.001, switchSeconds);
                QImage before = picture(fromProgram);
                if (progress >= 1.0 || before.isNull()) {
                    switching = false;
                } else {
                    auto premul = [](const QImage& i) {
                        return i.format() == QImage::Format_ARGB32_Premultiplied ? i : i.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    };
                    onAir = liveTransition(premul(before), premul(onAir), progress, switchSpec, fps);
                }
            }
            img = onAir;
            if (frame % 4 == 0) {
                QList<QImage> thumbs;
                for (int k = 0; k <= int(inputs.size()); ++k) {
                    const QImage p = picture(k);
                    thumbs << (p.isNull() ? QImage() : p.scaled(QSize(192, 108), Qt::KeepAspectRatio, Qt::FastTransformation));
                }
                QMetaObject::invokeMethod(this, [this, thumbs] { emit inputFrames(thumbs); }, Qt::QueuedConnection);
            }
        }
        std::shared_ptr<const ProjectSnapshot> snap;
        int snapshotRendererGeneration;
        {
            std::lock_guard lock(m_mutex);
            snap = m_snap;
            snapshotRendererGeneration = m_rendererGeneration;
        }
        if (!renderer || rendererGeneration != snapshotRendererGeneration) {
            rendererGeneration = snapshotRendererGeneration;
            renderer.reset();
            QString gpuError;
            renderer = createFrameRenderer(false, m_gpuSurface.get(), &gpuError);
            rendererName = renderer->name();
            if (!renderer->isGpu() && !gpuError.isEmpty())
                rendererName += QStringLiteral(" (GPU unavailable)");
        }
        if (m_mirror)
            img = img.mirrored(true, false);
        if (canvas.isValid()) {
            img = frameToCanvas(img, canvas, framing, panZoom);
        } else if (!panZoom.isDefault()) {
            // The output keeps the size it has without zoom; the zoomed-in part fills it.
            QSize source = capture.sourceSize();
            if (!source.isValid())
                source = img.size();
            QSize out = maxSize.isValid() && (source.width() > maxSize.width() || source.height() > maxSize.height())
                            ? source.scaled(maxSize, Qt::KeepAspectRatio)
                            : source;
            out = QSize(std::max(2, out.width() & ~1), std::max(2, out.height() & ~1));
            img = frameToCanvas(img, out, LiveFraming::CropFill, panZoom);
        }
        static const EffectChain kNone;
        QElapsedTimer t;
        t.start();
        const TimeUs liveTime = TimeUs(double(frame) * 1e6 / fps);
        // Modulated settings take their values for this frame.
        const ModInputs inputs = modInputs();
        auto chainNow = [&](const EffectChain& chain) { return hasModulation(chain) ? applyModulation(chain, inputs) : chain; };
        // The frame: the layer with its own FX over the picture, the Live FX, then the active pad's chain (or, while
        // switching, the previous pad's fading into it), then the held (momentary) pads'. In one go: on the GPU,
        // one upload and one read back.
        LiveFrameJob job;
        job.frame = img;
        job.time = liveTime;
        job.fps = fps;
        EffectChain layerFx;
        if (layerOpen) {
            const QImage layerImg = layer.latest();
            if (!layerImg.isNull()) {
                job.layer = layerImg;
                job.layerRect = layerRect(img.size(), layerImg.size(), layerSettings);
                job.layerCover = layerSettings.mode == LiveLayerSettings::Mode::Full;
                job.layerOpacity = layerSettings.opacity;
                job.layerBlend = layerSettings.blend;
                if (snap && hasActiveEffects(snap->timeline.liveLayerEffects)) {
                    layerFx = chainNow(snap->timeline.liveLayerEffects);
                    job.layerFx = &layerFx;
                }
            }
        }
        auto padChain = [&](int p) -> EffectChain {
            if (p < 0 || !snap || !hasActiveEffects(snap->timeline.livePads[size_t(p)].effects))
                return {};
            return chainNow(snap->timeline.livePads[size_t(p)].effects);
        };
        const EffectChain liveFx = snap ? chainNow(snap->timeline.liveEffects) : kNone;
        job.liveFx = &liveFx;
        EffectChain fromFx, toFx;
        if (fading) {
            const double progress = std::chrono::duration<double>(Clock::now() - fadeStart).count() / std::max(0.001, fadeSeconds);
            if (progress >= 1.0) {
                fading = false;
            } else {
                fromFx = padChain(fromPad);
                toFx = padChain(pad);
                job.fadeProgress = progress;
                job.fadeFrom = &fromFx;
                job.fadeTo = &toFx;
                job.fadeSpec = fadeSpec;
            }
        }
        std::vector<EffectChain> after; // (the pads after the Live FX)
        if (!fading)
            after.push_back(padChain(pad));
        for (int h : held) // momentary pads, while held
            if (h != pad)
                after.push_back(padChain(h));
        for (const EffectChain& c : after)
            job.after.push_back(&c);
        // The picture before the effects (with the layer), when a recording or the replay keeps that one.
        const bool wantClean = job.layer.isNull() ? false : (wantRecording && !recordEffects) || (replayWanted && !replayEffects);
        QImage clean;
        QImage out = renderer->renderLiveFrame(job, wantClean ? &clean : nullptr);
        if (wantClean && !clean.isNull())
            img = clean;
        if (snap && !snap->timeline.liveOverlays.empty()) {
            double bpm;
            {
                std::lock_guard lock(m_clockMutex);
                bpm = m_clockBpm;
            }
            if (out.format() != QImage::Format_ARGB32_Premultiplied)
                out = out.convertToFormat(QImage::Format_ARGB32_Premultiplied);
            const bool visual = std::any_of(snap->timeline.liveOverlays.begin(), snap->timeline.liveOverlays.end(), [](const LiveOverlay& o) {
                return o.enabled && (o.kind == LiveOverlay::Kind::Spectrum || o.kind == LiveOverlay::Kind::Waveform || o.kind == LiveOverlay::Kind::Circle);
            });
            if (visual) {
                {
                    std::lock_guard lock(m_scopeMutex); // oldest first
                    scope.assign(m_scope.begin() + std::ptrdiff_t(m_scopePos), m_scope.end());
                    scope.insert(scope.end(), m_scope.begin(), m_scope.begin() + std::ptrdiff_t(m_scopePos));
                }
                audioView.update(scope);
            }
            drawOverlays(out, snap->timeline.liveOverlays, bpm, inputs.beats - std::floor(inputs.beats), overlayPictures, audioView);
        }
        // The cue, every other frame, at most 640 wide.
        if (const int cueKind = m_cueKind; cueKind != CueNone && snap && frame % 2 == 0 && !m_cuePending) {
            const int index = m_cueIndex;
            const EffectChain* fx = &snap->timeline.liveEffects;
            const std::vector<LiveOverlay>* overlays = &snap->timeline.liveOverlays;
            int cuePad = -1;
            bool ok = true;
            if (cueKind == CuePad) {
                ok = index >= 0 && index < kLivePads;
                cuePad = index;
            } else {
                ok = index >= 0 && index < kLiveScenes && snap->timeline.liveScenes[size_t(index)].used;
                if (ok) {
                    const LiveScene& sc = snap->timeline.liveScenes[size_t(index)];
                    fx = &sc.liveEffects;
                    overlays = &sc.overlays;
                    cuePad = sc.pad;
                }
            }
            if (ok) {
                if (!cueRenderer || cueRendererGeneration != snapshotRendererGeneration) {
                    cueRendererGeneration = snapshotRendererGeneration;
                    cueRenderer = createFrameRenderer(false, m_gpuSurface.get());
                }
                QImage base = img.width() > 640 ? img.scaledToWidth(640, Qt::SmoothTransformation) : img;
                LiveFrameJob cueJob;
                cueJob.frame = base;
                cueJob.time = liveTime;
                cueJob.fps = fps;
                const EffectChain cueFx = chainNow(*fx);
                const EffectChain cuePadFx = cuePad >= 0 ? chainNow(snap->timeline.livePads[size_t(cuePad)].effects) : EffectChain{};
                cueJob.liveFx = &cueFx;
                cueJob.after = {&cuePadFx};
                QImage cue = cueRenderer->renderLiveFrame(cueJob);
                if (!overlays->empty()) {
                    double bpm;
                    {
                        std::lock_guard lock(m_clockMutex);
                        bpm = m_clockBpm;
                    }
                    if (cue.format() != QImage::Format_ARGB32_Premultiplied)
                        cue = cue.convertToFormat(QImage::Format_ARGB32_Premultiplied);
                    drawOverlays(cue, *overlays, bpm, inputs.beats - std::floor(inputs.beats), overlayPictures, audioView);
                }
                m_cuePending = true;
                QMetaObject::invokeMethod(this, [this, cue = std::move(cue)] {
                    m_cuePending = false;
                    emit cueFrameReady(cue);
                }, Qt::QueuedConnection);
            }
        }
        renderMs += double(t.nsecsElapsed()) / 1e6;
        ++frame;
        ++rendered;
        // Other apps: Spout, the virtual camera.
        if (shareChange || spoutWanted != spout.isOpen()) {
            if (spoutWanted && !spout.isOpen()) {
                spoutError.clear();
                if (!spout.open(spoutName, &spoutError))
                    spoutWanted = false;
            } else if (!spoutWanted && spout.isOpen()) {
                spout.close();
                spoutError.clear();
            }
        }
        if (spout.isOpen())
            spout.send(out);
        const QSize camSize(out.width() & ~1, out.height() & ~1);
        if (cameraWanted && (!camera.isOpen() || camera.size() != camSize) && --cameraRetry <= 0) {
            const bool wasOpen = camera.isOpen();
            camera.close();
            QString err;
            if (camera.open(camSize, fps, &err)) {
                cameraError.clear();
            } else {
                // A camera app may still hold the old one for a moment: try again in half a second.
                cameraRetry = std::max(1, int(fps / 2));
                if (!wasOpen && err != cameraError) {
                    cameraError = err;
                    shareStatus();
                }
            }
            shareChange = true;
        } else if (!cameraWanted && camera.isOpen()) {
            camera.close();
            cameraError.clear();
            shareChange = true;
        } else if (!cameraWanted && !cameraError.isEmpty()) {
            cameraError.clear();
            shareChange = true;
        }
        if (camera.isOpen())
            camera.send(out);
        // The stream.
        if (wantStream && !streamer) {
            auto s = std::make_unique<LiveRecorder>();
            QString err;
            const bool withAudio = audio.isRunning();
            if (s->startStream(streamUrl, out.size(), fps, streamKbps, &err, withAudio)) {
                streamer = std::move(s);
                streamLive = false;
                if (withAudio) {
                    std::lock_guard lock(m_audioMutex);
                    m_streamAudioTarget = streamer.get();
                }
                QMetaObject::invokeMethod(this, [this, e = streamer->encoderName()] { emit streamStatus(true, false, e, QString()); },
                                          Qt::QueuedConnection);
            } else {
                wantStream = false;
                stopStreaming(err);
            }
        } else if (!wantStream && streamer) {
            stopStreaming(QString());
        }
        if (streamer) {
            streamer->addFrame(out);
            if (!streamer->streamError().isEmpty()) {
                wantStream = false;
                stopStreaming(streamer->streamError());
            } else if (!streamLive && streamer->isConnected()) {
                streamLive = true;
                QMetaObject::invokeMethod(this, [this, e = streamer->encoderName()] { emit streamStatus(false, true, e, QString()); },
                                          Qt::QueuedConnection);
            }
        }
        if (shareChange) {
            shareChange = false;
            shareStatus();
        }
        if (wantRecording && !recorder) {
            {
                std::lock_guard lock(m_mutex);
                recordPath = m_recordPath;
            }
            const QImage& first = recordEffects ? out : img;
            auto rec = std::make_unique<LiveRecorder>();
            QString err;
            if (rec->start(recordPath, first.size(), fps, &err, audio.isRunning())) {
                recorder = std::move(rec);
                m_recording = true;
                if (recorder->hasAudio()) {
                    std::lock_guard lock(m_audioMutex);
                    m_audioTarget = recorder.get();
                }
                QMetaObject::invokeMethod(this, [this, p = recordPath, e = recorder->encoderName()] { emit recordingStarted(p, e); },
                                          Qt::QueuedConnection);
            } else {
                wantRecording = false;
                m_recording = false;
                QMetaObject::invokeMethod(this, [this, p = recordPath, err] { emit recordingFinished(p, false, err, 0); },
                                          Qt::QueuedConnection);
            }
        }
        {
            std::lock_guard lock(m_mutex);
            m_clean = img;
        }
        if (recorder)
            recorder->addFrame(recordEffects ? out : img);
        if (replayWanted && !replay) {
            auto rec = std::make_shared<LiveRecorder>();
            QString err;
            const QImage& first = replayEffects ? out : img;
            if (rec->startBuffer(first.size(), fps, replaySeconds, &err, audio.isRunning())) {
                replay = std::move(rec);
                m_replayRunning = true;
                if (replay->hasAudio()) {
                    std::lock_guard lock(m_audioMutex);
                    m_replayAudioTarget = replay.get();
                }
                QMetaObject::invokeMethod(this, [this, e = replay->encoderName()] { emit replayStatus(true, e, QString()); }, Qt::QueuedConnection);
            } else {
                replayWanted = false;
                QMetaObject::invokeMethod(this, [this, err] { emit replayStatus(false, QString(), err); }, Qt::QueuedConnection);
            }
        }
        if (replay)
            replay->addFrame(replayEffects ? out : img);
        if (!replaySave.isEmpty()) {
            const QString path = replaySave;
            replaySave.clear();
            if (!replay) {
                QMetaObject::invokeMethod(this, [this, path] { emit replaySaved(path, false, QStringLiteral("the replay buffer is off"), 0); },
                                          Qt::QueuedConnection);
            } else {
                if (m_saveThread.joinable())
                    m_saveThread.join();
                // Written on its own thread: the pictures keep coming meanwhile.
                m_saveThread = std::thread([this, buffer = replay, path] {
                    QString err;
                    double secs = 0;
                    const bool ok = buffer->saveReplay(path, &err, &secs);
                    if (ok) {
                        const QImage thumb = buffer->thumbnail();
                        if (!thumb.isNull())
                            thumb.save(liveThumbnailPath(path), "JPG", 85);
                    }
                    QMetaObject::invokeMethod(this, [this, path, ok, err, secs] { emit replaySaved(path, ok, err, secs); }, Qt::QueuedConnection);
                });
            }
        }
        // One frame in flight to the UI at most; newer pictures replace unshown ones.
        if (!out.isNull() && !m_framePending.exchange(true))
            QMetaObject::invokeMethod(
                this,
                [this, out = std::move(out)] {
                    m_framePending = false;
                    emit frameReady(out);
                },
                Qt::QueuedConnection);
        if (audio.isRunning() && levelTimer.elapsed() >= 40) {
            levelTimer.restart();
            const std::pair<float, float> peaks = audio.takePeaks();
            QMetaObject::invokeMethod(this, [this, peaks] { emit audioLevel(peaks.first, peaks.second); }, Qt::QueuedConnection);
        }
        if (statsTimer.elapsed() >= 1000) {
            const double secs = double(statsTimer.restart()) / 1000.0;
            const double renderFps = rendered / secs, ms = rendered ? renderMs / rendered : 0.0;
            rendered = 0;
            renderMs = 0;
            QMetaObject::invokeMethod(
                this,
                [this, cap = capture.captureFps(), renderFps, ms, name = rendererName, src = capture.sourceSize()] {
                    emit statsChanged(cap, renderFps, ms, name, src);
                },
                Qt::QueuedConnection);
        }
        if (streamer && streamLive && frame % std::max<int64_t>(1, int64_t(fps)) == 0)
            QMetaObject::invokeMethod(this, [this, s = streamer->seconds(), dr = streamer->droppedFrames()] { emit streamProgress(s, dr); },
                                      Qt::QueuedConnection);
        if (recorder && frame % std::max<int64_t>(1, int64_t(fps / 4)) == 0)
            QMetaObject::invokeMethod(
                this, [this, s = recorder->seconds(), dr = recorder->droppedFrames()] { emit recordingProgress(s, dr); },
                Qt::QueuedConnection);
    }
    finishRecording();
    stopReplay();
    if (streamer)
        stopStreaming(QString());
    if (spout.isOpen() || camera.isOpen()) {
        spout.close();
        camera.close();
        spoutError.clear();
        cameraError.clear();
        shareStatus();
    }
    audio.stop();
    layer.stop();
    capture.stop();
    cueRenderer.reset();
    renderer.reset();
    m_running = false;
    if (!failure.isEmpty())
        QMetaObject::invokeMethod(this, [this, failure] { emit failed(failure); }, Qt::QueuedConnection);
    else
        QMetaObject::invokeMethod(this, [this] { emit stopped(); }, Qt::QueuedConnection);
}
