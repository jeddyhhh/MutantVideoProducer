#include "core/LiveAudio.h"

#include "core/FFmpeg.h"
#include "core/LiveAudioWasapi.h"

extern "C" {
#include <libavdevice/avdevice.h>
}

#include <QStringList>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

namespace {

void registerDevices()
{
    static std::once_flag once;
    std::call_once(once, [] {
        ff::initialize();
        avdevice_register_all();
    });
}

// Audio capture devices of an FFmpeg input device (dshow, pulse, alsa).
void listDevices(const char* format, std::vector<LiveAudioSource>& out)
{
    const AVInputFormat* fmt = av_find_input_format(format);
    if (!fmt)
        return;
    AVDeviceInfoList* list = nullptr;
    if (avdevice_list_input_sources(fmt, nullptr, nullptr, &list) < 0 || !list) {
        avdevice_free_list_devices(&list);
        return;
    }
    for (int i = 0; i < list->nb_devices; ++i) {
        const AVDeviceInfo* d = list->devices[i];
        bool audio = d->nb_media_types == 0 && strcmp(format, "dshow") != 0; // pulse / alsa only list audio
        for (int k = 0; k < d->nb_media_types; ++k)
            audio |= d->media_types[k] == AVMEDIA_TYPE_AUDIO;
        if (!audio || !d->device_name || strcmp(d->device_name, "null") == 0) // ALSA's "discard" device
            continue;
        LiveAudioSource s;
        s.format = QString::fromLatin1(format);
        s.id = QString::fromUtf8(d->device_name);
        s.name = d->device_description && *d->device_description ? QString::fromUtf8(d->device_description) : s.id;
        out.push_back(s);
    }
    avdevice_free_list_devices(&list);
}

} // namespace

std::vector<LiveAudioSource> listLiveAudioSources()
{
    if (qEnvironmentVariableIsSet("MVP_NO_DEVICES")) // tests: no microphones or desktop audio, a 120 BPM click instead
        return {{QStringLiteral("Test beat (120 BPM)"), QStringLiteral("lavfi"),
                 QStringLiteral("aevalsrc=exprs='0.8*sin(2*PI*80*t)*exp(-25*mod(t,0.5))':s=48000,arealtime")}};
    registerDevices();
    std::vector<LiveAudioSource> out;
#ifdef Q_OS_WIN
    // DirectShow listing initialises and uninitialises COM on the calling thread.
    std::thread([&out] { listDevices("dshow", out); }).join();
    // What the playback devices play (WASAPI loopback).
    const std::vector<wasapi::Output> outputs = wasapi::listOutputs();
    if (!outputs.empty())
        out.push_back({QStringLiteral("Desktop audio (default output)"), QStringLiteral("wasapi"), QString()});
    for (const auto& o : outputs)
        out.push_back({QStringLiteral("Desktop audio: %1").arg(QString::fromStdWString(o.name)), QStringLiteral("wasapi"),
                       QString::fromStdWString(o.id)});
#else
    listDevices("pulse", out); // inputs, and "Monitor of ..." for what each output plays
    if (out.empty()) {
        listDevices("alsa", out);
        if (out.empty() && av_find_input_format("alsa"))
            out.push_back({QStringLiteral("Default input (ALSA)"), QStringLiteral("alsa"), QStringLiteral("default")});
    } else {
        out.insert(out.begin(), {QStringLiteral("Default input"), QStringLiteral("pulse"), QStringLiteral("default")});
        out.insert(out.begin() + 1, {QStringLiteral("Desktop audio (default output)"), QStringLiteral("pulse"), QStringLiteral("@DEFAULT_MONITOR@")});
    }
#endif
    return out;
}

double liveClock()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---- Capture ---------------------------------------------------------------------------

struct LiveAudioCapture::Impl {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* dec = nullptr;
    SwrContext* swr = nullptr;
    int swrFormat = -1, swrRate = 0;
    AVChannelLayout swrLayout{};
    int stream = -1;
    std::thread thread;
    std::atomic<bool> quit{false};
    mutable std::mutex mutex;
    std::condition_variable doneCv;
    bool done = false, started = false;
    std::mutex sinkMutex;
    Sink sink;
    QString failure, description;
    float peak[2] = {0, 0};
    double nextTime = -1; // expected start of the next chunk (keeps the stamps continuous)
    std::vector<float> buf;
    AVFrame* raw = nullptr; // Windows loopback: wraps its samples for deliver() (does not own them)

    static int interrupt(void* p) { return static_cast<Impl*>(p)->quit.load() ? 1 : 0; }

    bool open(const LiveAudioSource& src, QString* error)
    {
        const QByteArray format = src.format.toLatin1();
        const AVInputFormat* f = av_find_input_format(format.constData());
        if (!f) {
            *error = QStringLiteral("FFmpeg has no %1 capture support").arg(src.format);
            return false;
        }
        QString url = src.id;
        AVDictionary* opts = nullptr;
        if (src.format == QLatin1String("dshow")) {
            url = QStringLiteral("audio=") + src.id;
            av_dict_set(&opts, "audio_buffer_size", "20", 0); // ms: small chunks keep the stamps accurate
        } else if (src.format == QLatin1String("pulse")) {
            av_dict_set(&opts, "sample_rate", "48000", 0);
            av_dict_set(&opts, "fragment_size", "3840", 0); // 20 ms of 48 kHz stereo s16
        }
        fmt = avformat_alloc_context();
        fmt->interrupt_callback = {&Impl::interrupt, this};
        const int r = avformat_open_input(&fmt, url.toUtf8().constData(), f, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            fmt = nullptr;
            *error = ff::errorString(r);
            return false;
        }
        // Capture devices say what they send when opened; analysing the stream
        // as well would read (and wait for) a second or more of sound.
        bool known = fmt->nb_streams > 0;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVCodecParameters* p = fmt->streams[i]->codecpar;
            known &= p->codec_type != AVMEDIA_TYPE_AUDIO || (p->sample_rate > 0 && p->ch_layout.nb_channels > 0 && p->codec_id != AV_CODEC_ID_NONE);
        }
        if ((!known && avformat_find_stream_info(fmt, nullptr) < 0) || (stream = av_find_best_stream(fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0)) < 0) {
            *error = QStringLiteral("no audio from this source");
            close();
            return false;
        }
        const AVCodecParameters* par = fmt->streams[stream]->codecpar;
        const AVCodec* codec = avcodec_find_decoder(par->codec_id);
        if (!codec || !(dec = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(dec, par) < 0
            || avcodec_open2(dec, codec, nullptr) < 0) {
            *error = QStringLiteral("cannot decode %1").arg(QString::fromLatin1(avcodec_get_name(par->codec_id)));
            close();
            return false;
        }
        char layout[64] = {};
        av_channel_layout_describe(&par->ch_layout, layout, sizeof(layout));
        description = QStringLiteral("%1 Hz %2").arg(par->sample_rate).arg(QString::fromLatin1(layout));
        return true;
    }

    void close()
    {
        swr_free(&swr);
        av_channel_layout_uninit(&swrLayout);
        avcodec_free_context(&dec);
        if (fmt)
            avformat_close_input(&fmt);
        stream = -1;
    }

    ~Impl() { av_frame_free(&raw); }

    // Converts a decoded frame to 48 kHz stereo float and hands it to the sink.
    // `time`: when its first sample was captured (liveClock(); < 0 = it ends now).
    void deliver(const AVFrame* frame, double time = -1)
    {
        AVChannelLayout in{};
        if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)
            av_channel_layout_default(&in, std::max(1, frame->ch_layout.nb_channels));
        else
            av_channel_layout_copy(&in, &frame->ch_layout);
        if (!swr || swrFormat != frame->format || swrRate != frame->sample_rate || av_channel_layout_compare(&swrLayout, &in) != 0) {
            swr_free(&swr);
            AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
            const bool ok = swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, kSampleRate, &in, AVSampleFormat(frame->format),
                                                frame->sample_rate, 0, nullptr) >= 0;
            if (ok && in.nb_channels == 1) { // a mono microphone: the same full-level sound on both sides
                const double both[2] = {1.0, 1.0};
                swr_set_matrix(swr, both, 1);
            }
            if (!ok || swr_init(swr) < 0) {
                swr_free(&swr);
                av_channel_layout_uninit(&in);
                return;
            }
            swrFormat = frame->format;
            swrRate = frame->sample_rate;
            av_channel_layout_uninit(&swrLayout);
            av_channel_layout_copy(&swrLayout, &in);
        }
        av_channel_layout_uninit(&in);
        const int cap = swr_get_out_samples(swr, frame->nb_samples);
        if (cap <= 0)
            return;
        buf.resize(size_t(cap) * 2);
        uint8_t* outPtr = reinterpret_cast<uint8_t*>(buf.data());
        const int n = swr_convert(swr, &outPtr, cap, const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
        if (n <= 0)
            return;
        float pl = 0, pr = 0;
        for (int i = 0; i < n; ++i) {
            pl = std::max(pl, std::abs(buf[size_t(i) * 2]));
            pr = std::max(pr, std::abs(buf[size_t(i) * 2 + 1]));
        }
        // The chunk ends now; keep consecutive chunks back to back unless the
        // source really skipped (or stalled) more than 0.1 s.
        const double duration = double(n) / kSampleRate;
        double t = time >= 0 ? time : liveClock() - duration;
        if (nextTime >= 0 && std::abs(t - nextTime) < 0.1)
            t = nextTime;
        nextTime = t + duration;
        {
            std::lock_guard lock(mutex);
            peak[0] = std::max(peak[0], pl);
            peak[1] = std::max(peak[1], pr);
        }
        std::lock_guard lock(sinkMutex);
        if (sink)
            sink(buf.data(), n, t);
    }

#ifdef Q_OS_WIN
    std::vector<uint8_t> silence;

    void deliverLoopback(const void* samples, int frames, const wasapi::Format& f, double age)
    {
        if (frames <= 0 || f.channels <= 0)
            return;
        const int bytes = f.sample == wasapi::Format::Sample::S16 ? 2 : 4;
        const size_t size = size_t(frames) * size_t(f.channels) * size_t(bytes);
        if (!samples) {
            silence.assign(size, 0);
            samples = silence.data();
        }
        if (!raw && !(raw = av_frame_alloc()))
            return;
        raw->format = f.sample == wasapi::Format::Sample::Float ? AV_SAMPLE_FMT_FLT
                      : f.sample == wasapi::Format::Sample::S16 ? AV_SAMPLE_FMT_S16
                                                                : AV_SAMPLE_FMT_S32;
        raw->sample_rate = f.rate;
        raw->nb_samples = frames;
        av_channel_layout_uninit(&raw->ch_layout);
        if (!f.channelMask || av_channel_layout_from_mask(&raw->ch_layout, f.channelMask) < 0 || raw->ch_layout.nb_channels != f.channels) {
            av_channel_layout_uninit(&raw->ch_layout);
            av_channel_layout_default(&raw->ch_layout, f.channels);
        }
        raw->data[0] = static_cast<uint8_t*>(const_cast<void*>(samples));
        raw->extended_data = raw->data;
        raw->linesize[0] = int(size);
        deliver(raw, liveClock() - age);
        raw->data[0] = nullptr;
    }

    // Loopback capture on this thread; `opened` gets "" once the device is
    // open, or the reason it could not be opened.
    void runLoopback(const std::wstring& id, std::promise<QString>& opened)
    {
        bool isOpen = false;
        std::wstring err;
        const bool ok = wasapi::loopback(
            id, quit,
            [&](const wasapi::Format& f) {
                description = QStringLiteral("Windows loopback %1 Hz %2 ch").arg(f.rate).arg(f.channels);
                isOpen = true;
                opened.set_value(QString());
            },
            [&](const void* samples, int frames, const wasapi::Format& f, double age) { deliverLoopback(samples, frames, f, age); },
            &err);
        if (!isOpen) {
            opened.set_value(err.empty() ? QStringLiteral("cannot open the output device") : QString::fromStdWString(err));
        } else if (!ok && !quit) {
            std::lock_guard lock(mutex);
            failure = QString::fromStdWString(err);
        }
        swr_free(&swr);
        std::lock_guard lock(mutex);
        done = true;
        doneCv.notify_all();
    }
#endif

    void run()
    {
        ff::PacketPtr pkt = ff::makePacket();
        ff::FramePtr frame = ff::makeFrame();
        while (!quit) {
            const int r = av_read_frame(fmt, pkt.get());
            if (r == AVERROR(EAGAIN)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            if (r < 0) {
                if (!quit) {
                    std::lock_guard lock(mutex);
                    failure = r == AVERROR_EOF ? QStringLiteral("the audio source stopped") : ff::errorString(r);
                }
                break;
            }
            if (pkt->stream_index == stream && avcodec_send_packet(dec, pkt.get()) >= 0)
                while (avcodec_receive_frame(dec, frame.get()) == 0) {
                    deliver(frame.get());
                    av_frame_unref(frame.get());
                }
            av_packet_unref(pkt.get());
        }
        close();
        std::lock_guard lock(mutex);
        done = true;
        doneCv.notify_all();
    }
};

LiveAudioCapture::LiveAudioCapture() : d(std::make_shared<Impl>()) {}

LiveAudioCapture::~LiveAudioCapture()
{
    stop();
}

bool LiveAudioCapture::start(const LiveAudioSource& source, Sink sink, QString* error)
{
    stop();
    registerDevices();
    d = std::make_shared<Impl>(); // a previous reader may still be winding down
#ifdef Q_OS_WIN
    if (source.format == QLatin1String("wasapi")) {
        d->sink = std::move(sink);
        d->started = true;
        std::promise<QString> opened;
        std::future<QString> result = opened.get_future();
        d->thread = std::thread([self = d, id = source.id.toStdWString(), &opened] { self->runLoopback(id, opened); });
        const QString e = result.get();
        if (e.isEmpty())
            return true;
        d->thread.join(); // it has finished
        d->started = false;
        if (error)
            *error = e;
        return false;
    }
#endif
    QString e;
    if (source.isNull() || !d->open(source, &e)) {
        if (error)
            *error = source.isNull() ? QStringLiteral("no audio source") : e;
        return false;
    }
    const QString via = source.format == QLatin1String("dshow")   ? QStringLiteral("DirectShow")
                        : source.format == QLatin1String("pulse") ? QStringLiteral("PulseAudio")
                        : source.format == QLatin1String("alsa")  ? QStringLiteral("ALSA")
                                                                  : source.format;
    d->description = QStringLiteral("%1 %2").arg(via, d->description);
    d->sink = std::move(sink);
    d->started = true;
    d->thread = std::thread([self = d] { self->run(); });
    return true;
}

void LiveAudioCapture::stop()
{
    {
        std::lock_guard lock(d->sinkMutex); // no samples are delivered after stop() returns
        d->sink = nullptr;
    }
    d->quit = true;
    if (!d->thread.joinable()) {
        if (!d->started)
            d->close();
        return;
    }
    bool finished;
    {
        std::unique_lock lock(d->mutex);
        finished = d->doneCv.wait_for(lock, std::chrono::milliseconds(500), [&] { return d->done; });
    }
    if (finished)
        d->thread.join();
    else
        d->thread.detach(); // blocked in the device: it closes the source when it returns
}

bool LiveAudioCapture::isRunning() const
{
    std::lock_guard lock(d->mutex);
    return d->started && !d->done && d->failure.isEmpty();
}

std::pair<float, float> LiveAudioCapture::takePeaks()
{
    std::lock_guard lock(d->mutex);
    const std::pair<float, float> p{d->peak[0], d->peak[1]};
    d->peak[0] = d->peak[1] = 0;
    return p;
}

QString LiveAudioCapture::failure() const
{
    std::lock_guard lock(d->mutex);
    return d->failure;
}

QString LiveAudioCapture::description() const
{
    return d->description;
}
