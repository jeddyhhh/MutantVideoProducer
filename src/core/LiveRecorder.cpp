#include "core/Brand.h"
#include "core/LiveRecorder.h"

#include "core/FFmpeg.h"
#include "core/Hardware.h"
#include "core/LiveAudio.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include "core/Profiles.h"
#include <QUrl>
#include <climits>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

namespace {
constexpr size_t kMaxQueued = 16; // frames waiting for the encoder (~0.5 s at 30 fps)
constexpr int kAudioRate = LiveAudioCapture::kSampleRate;
constexpr int64_t kMaxAudioQueued = kAudioRate * 5; // samples waiting for the encoder

struct QueuedFrame {
    QImage image;
    double time;
};
struct QueuedAudio {
    std::vector<float> samples; // interleaved stereo
    double time;
};
}

struct LiveRecorder::Impl {
    AVFormatContext* out = nullptr;
    AVCodecContext* enc = nullptr;
    AVStream* stream = nullptr;
    SwsContext* sws = nullptr;
    QSize swsIn;
    AVFrame* frame = nullptr;
    QSize size;
    double fps = 30;
    QString encoder, error;
    // Streaming.
    bool streaming = false, silent = false; // silent: a sound track of silence (no input)
    int kbps = 0;
    QString url;
    std::atomic<bool> connected{false}, abort{false}, failed{false};
    std::thread thread;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<QueuedFrame> queue;
    bool finishing = false, recording = false;
    std::atomic<int64_t> written{0}, dropped{0};
    int64_t lastSlot = -1; // pts of the last picture written
    double t0 = 0;         // liveClock() at the start: pts 0
    QImage thumb;
    bool thumbTaken = false, thumbLate = false;
    // Sound (when recording with audio).
    AVCodecContext* aenc = nullptr;
    AVStream* astream = nullptr;
    AVAudioFifo* fifo = nullptr;
    AVFrame* aframe = nullptr;
    std::deque<QueuedAudio> audioQueue;
    int64_t audioQueued = 0; // samples in audioQueue
    int64_t audioPos = 0;    // samples placed so far (the next one goes here)
    int64_t audioPts = 0;    // samples encoded so far
    bool audioStarted = false;
    std::vector<float> planes[2];
    // Replay buffer: packets (in their encoder's time base) of the last keepSeconds.
    bool memory = false;
    double keepSeconds = 30;
    struct Stored {
        AVPacket* packet;
        bool audio;
        double time; // seconds since t0
        bool key;
    };
    mutable std::mutex bufferMutex;
    std::deque<Stored> stored;
    QImage latestThumb;

    void storePacket(AVCodecContext* c, AVPacket* pkt)
    {
        const bool audio = c == aenc;
        Stored s{av_packet_clone(pkt), audio, double(pkt->pts) * av_q2d(c->time_base), audio || (pkt->flags & AV_PKT_FLAG_KEY)};
        if (!s.packet)
            return;
        std::lock_guard lock(bufferMutex);
        stored.push_back(s);
        // Keep keepSeconds, from a picture keyframe: drop everything before the
        // last keyframe that is at least that old.
        double newest = 0;
        for (auto it = stored.rbegin(); it != stored.rend(); ++it)
            if (!it->audio) {
                newest = it->time;
                break;
            }
        double cut = -1;
        for (const Stored& p : stored)
            if (!p.audio && p.key && p.time <= newest - keepSeconds)
                cut = p.time;
        while (cut > 0 && !stored.empty() && stored.front().time < cut) {
            av_packet_free(&stored.front().packet);
            stored.pop_front();
        }
    }

    void clearStored()
    {
        std::lock_guard lock(bufferMutex);
        for (Stored& s : stored)
            av_packet_free(&s.packet);
        stored.clear();
    }

    bool openEncoder(const QString& name, QString* why)
    {
        const AVCodec* codec = avcodec_find_encoder_by_name(name.toLatin1().constData());
        if (!codec) {
            *why = QStringLiteral("%1 not available").arg(name);
            return false;
        }
        if (hw::isHardwareEncoder(name) && !hw::encoderUsable(name)) {
            *why = QStringLiteral("%1 not usable on this machine").arg(name);
            return false;
        }
        AVCodecContext* c = avcodec_alloc_context3(codec);
        c->width = size.width();
        c->height = size.height();
        const AVRational rate = av_d2q(fps, 100000);
        c->framerate = rate;
        c->time_base = av_inv_q(rate);
        c->gop_size = std::max(1, int(std::lround(fps))); // a keyframe a second: quick seeking when editing
        c->max_b_frames = 0;
        c->sample_aspect_ratio = {1, 1};
        c->color_range = AVCOL_RANGE_MPEG;
        c->colorspace = AVCOL_SPC_BT709;
        c->color_primaries = AVCOL_PRI_BT709;
        c->color_trc = AVCOL_TRC_BT709;
        const std::vector<AVPixelFormat> formats = ff::encoderPixelFormats(codec);
        AVPixelFormat pix = formats.empty() ? AV_PIX_FMT_YUV420P : formats.front();
        for (AVPixelFormat f : formats)
            if (f == AV_PIX_FMT_YUV420P || f == AV_PIX_FMT_NV12) {
                pix = f;
                break;
            }
        c->pix_fmt = pix;
        const int64_t bitrate = int64_t(double(size.width()) * size.height() * fps * 0.2); // ~5.5 Mb/s at 720p30
        AVDictionary* opts = nullptr;
        if (name == QLatin1String("libx264")) {
            av_dict_set(&opts, "preset", "veryfast", 0);
            av_dict_set(&opts, "crf", "18", 0);
        } else if (name == QLatin1String("h264_nvenc")) {
            av_dict_set(&opts, "preset", "p4", 0);
            av_dict_set(&opts, "rc", "vbr", 0);
            av_dict_set(&opts, "cq", "20", 0);
            c->bit_rate = bitrate;
            c->rc_max_rate = bitrate * 3;
        } else if (name == QLatin1String("h264_amf")) {
            av_dict_set(&opts, "quality", "balanced", 0);
            av_dict_set(&opts, "rc", "cqp", 0);
            av_dict_set(&opts, "qp_i", "20", 0);
            av_dict_set(&opts, "qp_p", "22", 0);
        } else if (name == QLatin1String("mpeg4")) {
            c->flags |= AV_CODEC_FLAG_QSCALE;
            c->global_quality = FF_QP2LAMBDA * 3;
        } else {
            c->bit_rate = bitrate;
        }
        if (streaming) { // what streaming sites ask for: a constant bitrate, a keyframe every 2 s
            const int64_t bps = int64_t(kbps) * 1000;
            c->bit_rate = bps;
            c->rc_max_rate = bps;
            c->rc_min_rate = name == QLatin1String("libx264") ? 0 : bps;
            c->rc_buffer_size = int(std::min<int64_t>(bps * 2, INT_MAX));
            c->gop_size = std::max(1, int(std::lround(fps * 2)));
            c->flags &= ~AV_CODEC_FLAG_QSCALE;
            av_dict_set(&opts, "crf", nullptr, 0);
            av_dict_set(&opts, "cq", nullptr, 0);
            av_dict_set(&opts, "qp_i", nullptr, 0);
            av_dict_set(&opts, "qp_p", nullptr, 0);
            if (name == QLatin1String("libx264")) {
                av_dict_set(&opts, "preset", "veryfast", 0);
                av_dict_set(&opts, "tune", "zerolatency", 0);
                av_dict_set(&opts, "x264-params", "nal-hrd=cbr:force-cfr=1", 0);
            } else if (name == QLatin1String("h264_nvenc") || name == QLatin1String("h264_amf")) {
                av_dict_set(&opts, "rc", "cbr", 0);
            }
        }
        if (!out || (out->oformat->flags & AVFMT_GLOBALHEADER)) // the buffer is saved as MP4
            c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int r = avcodec_open2(c, codec, &opts);
        av_dict_free(&opts);
        if (r < 0) {
            *why = QStringLiteral("%1: %2").arg(name, ff::errorString(r));
            avcodec_free_context(&c);
            return false;
        }
        enc = c;
        encoder = name;
        return true;
    }

    bool write(AVCodecContext* c, AVStream* st, AVFrame* f)
    {
        if (avcodec_send_frame(c, f) < 0)
            return false;
        ff::PacketPtr pkt = ff::makePacket();
        while (avcodec_receive_packet(c, pkt.get()) == 0) {
            if (memory) {
                storePacket(c, pkt.get());
                av_packet_unref(pkt.get());
                continue;
            }
            av_packet_rescale_ts(pkt.get(), c->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (streaming && failed)
                continue;
            if (av_interleaved_write_frame(out, pkt.get()) < 0) {
                if (streaming) {
                    streamFailed();
                    continue;
                }
                return false;
            }
        }
        return true;
    }

    bool openAudio(QString* why)
    {
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (!codec || !(aenc = avcodec_alloc_context3(codec))) {
            *why = QStringLiteral("no AAC encoder");
            return false;
        }
        aenc->sample_fmt = AV_SAMPLE_FMT_FLTP;
        aenc->sample_rate = kAudioRate;
        aenc->ch_layout = AV_CHANNEL_LAYOUT_STEREO;
        aenc->bit_rate = streaming ? 160000 : 192000;
        aenc->time_base = AVRational{1, kAudioRate};
        if (!out || (out->oformat->flags & AVFMT_GLOBALHEADER))
            aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        const int r = avcodec_open2(aenc, codec, nullptr);
        if (r < 0) {
            *why = QStringLiteral("AAC: %1").arg(ff::errorString(r));
            return false;
        }
        if (out) {
            astream = avformat_new_stream(out, nullptr);
            astream->time_base = aenc->time_base;
            avcodec_parameters_from_context(astream->codecpar, aenc);
        }
        fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLTP, 2, kAudioRate);
        aframe = av_frame_alloc();
        aframe->format = AV_SAMPLE_FMT_FLTP;
        aframe->sample_rate = kAudioRate;
        av_channel_layout_copy(&aframe->ch_layout, &aenc->ch_layout);
        aframe->nb_samples = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        if (!fifo || av_frame_get_buffer(aframe, 0) < 0) {
            *why = QStringLiteral("out of memory");
            return false;
        }
        return true;
    }

    // Appends `n` stereo samples (null: silence) to the sound track.
    void pushAudio(const float* interleaved, int64_t n)
    {
        while (n > 0) {
            const int chunk = int(std::min<int64_t>(n, 4096));
            for (auto& p : planes)
                p.assign(size_t(chunk), 0.0f);
            if (interleaved) {
                for (int i = 0; i < chunk; ++i) {
                    planes[0][size_t(i)] = interleaved[size_t(i) * 2];
                    planes[1][size_t(i)] = interleaved[size_t(i) * 2 + 1];
                }
                interleaved += size_t(chunk) * 2;
            }
            void* data[2] = {planes[0].data(), planes[1].data()};
            av_audio_fifo_write(fifo, data, chunk);
            audioPos += chunk;
            n -= chunk;
        }
    }

    // Encodes whole AAC frames from the FIFO (and the rest when flushing).
    void encodeAudio(bool flush)
    {
        const int frameSize = aenc->frame_size > 0 ? aenc->frame_size : 1024;
        while (av_audio_fifo_size(fifo) >= frameSize || (flush && av_audio_fifo_size(fifo) > 0)) {
            const int n = std::min(frameSize, av_audio_fifo_size(fifo));
            if (av_frame_make_writable(aframe) < 0)
                return;
            aframe->nb_samples = n;
            av_audio_fifo_read(fifo, reinterpret_cast<void**>(aframe->data), n);
            aframe->pts = audioPts;
            audioPts += n;
            if (!write(aenc, astream, aframe)) {
                std::lock_guard lock(mutex);
                if (error.isEmpty())
                    error = QStringLiteral("writing the sound failed");
                return;
            }
        }
        if (flush)
            write(aenc, astream, nullptr);
    }

    void placeAudio(const QueuedAudio& a)
    {
        const float* s = a.samples.data();
        int64_t n = int64_t(a.samples.size() / 2);
        // The first sound lands exactly where it was captured; after that the
        // samples follow on, unless the source skipped or stalled (> 0.2 s).
        int64_t diff = std::llround((a.time - t0) * kAudioRate) - audioPos;
        if (audioStarted && std::abs(diff) < kAudioRate / 5)
            diff = 0;
        if (diff < 0) { // captured before the recording started (or overlapping)
            const int64_t skip = std::min(n, -diff);
            s += skip * 2;
            n -= skip;
        } else if (diff > 0) {
            pushAudio(nullptr, std::min<int64_t>(diff, int64_t(kAudioRate) * 60));
        }
        if (n <= 0)
            return;
        pushAudio(s, n);
        audioStarted = true;
        encodeAudio(false);
    }

    void encodeImage(const QImage& img, double time)
    {
        // Slot on the recording's frame grid from the capture time: a picture
        // that comes two or more slots late fills the gap (so the sound stays in
        // sync); one that is two or more slots early is dropped.
        const int64_t target = std::llround((time - t0) * fps);
        const int64_t slot = lastSlot + 1;
        if (target - slot <= -2)
            return;
        const int64_t gap = target - slot >= 2 ? std::min<int64_t>(target - slot, int64_t(fps * 60)) : 0;
        const QImage src = img.format() == QImage::Format_ARGB32_Premultiplied || img.format() == QImage::Format_ARGB32
                                   || img.format() == QImage::Format_RGB32
                               ? img
                               : img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (!sws || swsIn != src.size()) {
            sws_freeContext(sws);
            sws = sws_getContext(src.width(), src.height(), AV_PIX_FMT_BGRA, size.width(), size.height(), enc->pix_fmt,
                                 SWS_BILINEAR, nullptr, nullptr, nullptr);
            // Full-range RGB -> limited-range BT.709, as tagged on the stream.
            sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), 1, sws_getCoefficients(SWS_CS_ITU709), 0, 0,
                                     1 << 16, 1 << 16);
            swsIn = src.size();
        }
        if (av_frame_make_writable(frame) < 0)
            return;
        const uint8_t* in[1] = {src.constBits()};
        const int inStride[1] = {int(src.bytesPerLine())};
        sws_scale(sws, in, inStride, 0, src.height(), frame->data, frame->linesize);
        for (int64_t i = 0; i <= gap; ++i) {
            frame->pts = slot + i;
            if (!write(enc, stream, frame)) {
                std::lock_guard lock(mutex);
                if (error.isEmpty())
                    error = QStringLiteral("writing the recording failed");
                return;
            }
            lastSlot = frame->pts;
            ++written;
        }
        if (silent && aenc) { // the stream's sound track: silence up to the picture
            const int64_t want = std::llround(double(lastSlot + 1) / fps * kAudioRate);
            if (want > audioPos) {
                pushAudio(nullptr, want - audioPos);
                encodeAudio(false);
            }
        }
        if (memory) { // a recent picture for the saved replay's thumbnail
            if (written % std::max<int64_t>(1, int64_t(fps)) == 1) {
                QImage t = src.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                std::lock_guard lock(mutex);
                latestThumb = std::move(t);
            }
            return;
        }
        const bool late = written >= int64_t(fps / 2); // half a second in: past any fade-in from black
        if (!thumbTaken || (late && !thumbLate)) {
            QImage t = src.scaled(320, 180, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            thumbTaken = true;
            thumbLate = late;
            std::lock_guard lock(mutex);
            thumb = std::move(t);
        }
    }

    // Streams: connects and sends the header (on this thread: it can take a while).
    bool connect()
    {
        AVIOInterruptCB cb{[](void* p) { return static_cast<Impl*>(p)->abort.load() ? 1 : 0; }, this};
        AVDictionary* opts = nullptr;
        av_dict_set(&opts, "rw_timeout", "10000000", 0); // 10 s without progress: give up ("timeout" would make RTMP listen)
        int r = avio_open2(&out->pb, url.toUtf8().constData(), AVIO_FLAG_WRITE, &cb, &opts);
        av_dict_free(&opts);
        if (r >= 0) {
            out->interrupt_callback = cb;
            AVDictionary* hopts = nullptr;
            if (QByteArray(out->oformat->name) == "flv")
                av_dict_set(&hopts, "flvflags", "no_duration_filesize", 0);
            r = avformat_write_header(out, &hopts);
            av_dict_free(&hopts);
        }
        if (r < 0) {
            std::lock_guard lock(mutex);
            error = abort ? QStringLiteral("stopped")
                          : QStringLiteral("can't connect to the server (check the address, the stream key and the connection): %1")
                                .arg(ff::errorString(r));
            failed = true;
            return false;
        }
        connected = true;
        return true;
    }

    void run()
    {
        if (streaming && !connect()) {
            // Nothing to send to: let the pictures go until stopped.
            std::unique_lock lock(mutex);
            recording = false;
            queue.clear();
            audioQueue.clear();
            cv.wait(lock, [&] { return finishing; });
            return;
        }
        for (;;) {
            std::optional<QueuedFrame> img;
            std::deque<QueuedAudio> sound;
            {
                std::unique_lock lock(mutex);
                cv.wait(lock, [&] { return finishing || !queue.empty() || !audioQueue.empty(); });
                if (queue.empty() && audioQueue.empty())
                    break; // finishing
                sound.swap(audioQueue);
                audioQueued = 0;
                if (!queue.empty()) {
                    img = std::move(queue.front());
                    queue.pop_front();
                }
            }
            for (const QueuedAudio& a : sound)
                placeAudio(a);
            if (img)
                encodeImage(img->image, img->time);
        }
        write(enc, stream, nullptr); // flush
        if (aenc)
            encodeAudio(true);
        if (out)
            av_write_trailer(out);
    }

    // Packets go out over the network: a failure stops the stream (it doesn't hang the engine).
    void streamFailed()
    {
        std::lock_guard lock(mutex);
        if (!failed) {
            failed = true;
            if (error.isEmpty())
                error = QStringLiteral("the connection was lost");
        }
        recording = false;
    }

    void close()
    {
        sws_freeContext(sws);
        sws = nullptr;
        av_frame_free(&frame);
        avcodec_free_context(&enc);
        av_frame_free(&aframe);
        avcodec_free_context(&aenc);
        if (fifo)
            av_audio_fifo_free(fifo);
        fifo = nullptr;
        astream = nullptr;
        clearStored();
        if (out) {
            if (!(out->oformat->flags & AVFMT_NOFILE))
                avio_closep(&out->pb);
            avformat_free_context(out);
            out = nullptr;
        }
    }
};

LiveRecorder::LiveRecorder() : d(std::make_unique<Impl>()) {}

LiveRecorder::~LiveRecorder()
{
    finish();
}

bool LiveRecorder::start(const QString& path, QSize size, double fps, QString* error, bool withAudio)
{
    finish();
    ff::initialize();
    d->memory = false;
    d->size = QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
    d->fps = std::max(1.0, fps);
    d->written = 0;
    d->dropped = 0;
    d->lastSlot = -1;
    d->audioQueue.clear();
    d->audioQueued = d->audioPos = d->audioPts = 0;
    d->audioStarted = false;
    d->thumb = QImage();
    d->thumbTaken = d->thumbLate = false;
    d->error.clear();
    d->finishing = false;
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        d->close();
        return false;
    };
    if (avformat_alloc_output_context2(&d->out, nullptr, "mp4", path.toUtf8().constData()) < 0 || !d->out)
        return fail(QStringLiteral("cannot create %1").arg(path));
    QStringList tried;
    for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "libx264", "libopenh264", "h264_mf", "mpeg4"}) {
        QString why;
        if (d->openEncoder(QString::fromLatin1(name), &why))
            break;
        tried << why;
    }
    if (!d->enc)
        return fail(QStringLiteral("no video encoder works (%1)").arg(tried.join(QStringLiteral("; "))));
    d->stream = avformat_new_stream(d->out, nullptr);
    d->stream->time_base = d->enc->time_base;
    d->stream->avg_frame_rate = d->enc->framerate;
    avcodec_parameters_from_context(d->stream->codecpar, d->enc);
    d->frame = av_frame_alloc();
    d->frame->format = d->enc->pix_fmt;
    d->frame->width = d->size.width();
    d->frame->height = d->size.height();
    d->frame->color_range = AVCOL_RANGE_MPEG;
    d->frame->colorspace = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(d->frame, 0) < 0)
        return fail(QStringLiteral("out of memory"));
    if (withAudio) {
        QString why;
        if (!d->openAudio(&why))
            return fail(why);
    }
    int r = avio_open(&d->out->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE);
    if (r < 0)
        return fail(QStringLiteral("cannot write %1: %2").arg(path, ff::errorString(r)));
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    r = avformat_write_header(d->out, &opts);
    av_dict_free(&opts);
    if (r < 0)
        return fail(ff::errorString(r));
    d->t0 = liveClock();
    d->recording = true;
    d->thread = std::thread([this] { d->run(); });
    return true;
}

bool LiveRecorder::startStream(const QString& url, QSize size, double fps, int kbps, QString* error, bool withAudio)
{
    finish();
    ff::initialize();
    avformat_network_init();
    d->memory = false;
    d->streaming = true;
    d->silent = !withAudio;
    d->kbps = std::clamp(kbps, 300, 50000);
    d->url = url.trimmed();
    d->connected = false;
    d->abort = false;
    d->failed = false;
    d->size = QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
    d->fps = std::max(1.0, fps);
    d->written = 0;
    d->dropped = 0;
    d->lastSlot = -1;
    d->audioQueue.clear();
    d->audioQueued = d->audioPos = d->audioPts = 0;
    d->audioStarted = false;
    d->thumb = QImage();
    d->thumbTaken = d->thumbLate = false;
    d->error.clear();
    d->finishing = false;
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        d->close();
        d->streaming = false;
        return false;
    };
    const QString scheme = QUrl(d->url).scheme().toLower();
    const char* format = scheme.startsWith(QLatin1String("rtmp")) ? "flv" : (scheme == QLatin1String("srt") || scheme == QLatin1String("udp")) ? "mpegts" : nullptr;
    if (!format)
        return fail(QStringLiteral("the address must start with rtmp://, rtmps://, srt:// or udp://"));
    if (avformat_alloc_output_context2(&d->out, nullptr, format, nullptr) < 0 || !d->out)
        return fail(QStringLiteral("can't create the stream"));
    QStringList tried;
    for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "libx264", "libopenh264", "h264_mf"}) {
        QString why;
        if (d->openEncoder(QString::fromLatin1(name), &why))
            break;
        tried << why;
    }
    if (!d->enc)
        return fail(QStringLiteral("no H.264 encoder works (%1)").arg(tried.join(QStringLiteral("; "))));
    d->stream = avformat_new_stream(d->out, nullptr);
    d->stream->time_base = d->enc->time_base;
    d->stream->avg_frame_rate = d->enc->framerate;
    avcodec_parameters_from_context(d->stream->codecpar, d->enc);
    d->frame = av_frame_alloc();
    d->frame->format = d->enc->pix_fmt;
    d->frame->width = d->size.width();
    d->frame->height = d->size.height();
    d->frame->color_range = AVCOL_RANGE_MPEG;
    d->frame->colorspace = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(d->frame, 0) < 0)
        return fail(QStringLiteral("out of memory"));
    QString why;
    if (!d->openAudio(&why)) // always: sites expect sound
        return fail(why);
    d->t0 = liveClock();
    d->recording = true;
    d->thread = std::thread([this] { d->run(); });
    return true;
}

bool LiveRecorder::isConnected() const
{
    return d->connected && !d->failed;
}

QString LiveRecorder::streamError() const
{
    std::lock_guard lock(d->mutex);
    return d->failed ? d->error : QString();
}

bool LiveRecorder::startBuffer(QSize size, double fps, double seconds, QString* error, bool withAudio)
{
    finish();
    ff::initialize();
    d->memory = true;
    d->keepSeconds = std::clamp(seconds, 2.0, 600.0);
    d->size = QSize(std::max(2, size.width() & ~1), std::max(2, size.height() & ~1));
    d->fps = std::max(1.0, fps);
    d->written = 0;
    d->dropped = 0;
    d->lastSlot = -1;
    d->audioQueue.clear();
    d->audioQueued = d->audioPos = d->audioPts = 0;
    d->audioStarted = false;
    d->error.clear();
    d->finishing = false;
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        d->close();
        return false;
    };
    QStringList tried;
    for (const char* name : {"h264_nvenc", "h264_qsv", "h264_amf", "libx264", "libopenh264", "h264_mf", "mpeg4"}) {
        QString why;
        if (d->openEncoder(QString::fromLatin1(name), &why))
            break;
        tried << why;
    }
    if (!d->enc)
        return fail(QStringLiteral("no video encoder works (%1)").arg(tried.join(QStringLiteral("; "))));
    d->frame = av_frame_alloc();
    d->frame->format = d->enc->pix_fmt;
    d->frame->width = d->size.width();
    d->frame->height = d->size.height();
    d->frame->color_range = AVCOL_RANGE_MPEG;
    d->frame->colorspace = AVCOL_SPC_BT709;
    if (av_frame_get_buffer(d->frame, 0) < 0)
        return fail(QStringLiteral("out of memory"));
    if (withAudio) {
        QString why;
        if (!d->openAudio(&why))
            return fail(why);
    }
    d->t0 = liveClock();
    d->recording = true;
    d->thread = std::thread([this] { d->run(); });
    return true;
}

bool LiveRecorder::saveReplay(const QString& path, QString* error, double* seconds)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (!d->memory || !d->enc)
        return fail(QStringLiteral("no replay buffer"));
    std::vector<Impl::Stored> packets;
    {
        std::lock_guard lock(d->bufferMutex);
        for (const Impl::Stored& s : d->stored)
            packets.push_back({av_packet_clone(s.packet), s.audio, s.time, s.key});
    }
    struct Free {
        std::vector<Impl::Stored>& p;
        ~Free()
        {
            for (auto& s : p)
                av_packet_free(&s.packet);
        }
    } freeLater{packets};
    size_t first = 0;
    while (first < packets.size() && (packets[first].audio || !packets[first].key))
        ++first;
    if (first == packets.size())
        return fail(QStringLiteral("nothing in the replay buffer yet"));
    const double start = packets[first].time;
    double end = start;
    for (size_t i = first; i < packets.size(); ++i)
        if (!packets[i].audio)
            end = std::max(end, packets[i].time + 1.0 / d->fps);
    AVFormatContext* out = nullptr;
    if (avformat_alloc_output_context2(&out, nullptr, "mp4", path.toUtf8().constData()) < 0 || !out)
        return fail(QStringLiteral("cannot create %1").arg(path));
    auto closeOut = [&] {
        if (!(out->oformat->flags & AVFMT_NOFILE) && out->pb)
            avio_closep(&out->pb);
        avformat_free_context(out);
    };
    AVStream* vs = avformat_new_stream(out, nullptr);
    vs->time_base = d->enc->time_base;
    vs->avg_frame_rate = d->enc->framerate;
    avcodec_parameters_from_context(vs->codecpar, d->enc);
    AVStream* as = nullptr;
    if (d->aenc) {
        as = avformat_new_stream(out, nullptr);
        as->time_base = d->aenc->time_base;
        avcodec_parameters_from_context(as->codecpar, d->aenc);
    }
    int r = avio_open(&out->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE);
    if (r < 0) {
        closeOut();
        return fail(QStringLiteral("cannot write %1: %2").arg(path, ff::errorString(r)));
    }
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "movflags", "+faststart", 0);
    r = avformat_write_header(out, &opts);
    av_dict_free(&opts);
    if (r < 0) {
        closeOut();
        return fail(ff::errorString(r));
    }
    const int64_t videoStart = packets[first].packet->pts;
    const int64_t audioStart = std::llround(start * kAudioRate);
    const AVRational encTb = d->enc->time_base;
    bool ok = true;
    for (size_t i = first; i < packets.size() && ok; ++i) {
        AVPacket* p = packets[i].packet;
        if (packets[i].audio) {
            if (!as || p->pts < audioStart)
                continue;
            p->pts -= audioStart;
            p->dts = p->dts == AV_NOPTS_VALUE ? p->pts : p->dts - audioStart;
            av_packet_rescale_ts(p, d->aenc->time_base, as->time_base);
            p->stream_index = as->index;
        } else {
            p->pts -= videoStart;
            p->dts = p->dts == AV_NOPTS_VALUE ? p->pts : p->dts - videoStart;
            av_packet_rescale_ts(p, encTb, vs->time_base);
            p->stream_index = vs->index;
        }
        ok = av_interleaved_write_frame(out, p) >= 0;
    }
    ok = av_write_trailer(out) >= 0 && ok;
    closeOut();
    if (seconds)
        *seconds = end - start;
    return ok ? true : fail(QStringLiteral("writing %1 failed").arg(path));
}

double LiveRecorder::bufferedSeconds() const
{
    std::lock_guard lock(d->bufferMutex);
    double first = -1, last = 0;
    for (const Impl::Stored& s : d->stored)
        if (!s.audio) {
            if (first < 0)
                first = s.time;
            last = s.time;
        }
    return first < 0 ? 0 : last - first;
}

QSize LiveRecorder::size() const
{
    return d->size;
}

bool LiveRecorder::addFrame(const QImage& frame, double time)
{
    if (!d->recording || frame.isNull())
        return false;
    if (time < 0)
        time = liveClock();
    {
        std::lock_guard lock(d->mutex);
        if (d->queue.size() >= kMaxQueued) {
            ++d->dropped;
            return false;
        }
        d->queue.push_back({frame, time});
    }
    d->cv.notify_one();
    return true;
}

void LiveRecorder::addAudio(const float* samples, int frames, double time)
{
    if (!d->recording || !d->aenc || frames <= 0)
        return;
    {
        std::lock_guard lock(d->mutex);
        if (d->audioQueued + frames > kMaxAudioQueued)
            return; // the encoder is stuck: the gap is filled with silence later
        d->audioQueue.push_back({std::vector<float>(samples, samples + size_t(frames) * 2), time});
        d->audioQueued += frames;
    }
    d->cv.notify_one();
}

bool LiveRecorder::finish(QString* error)
{
    if (!d->recording && !d->thread.joinable())
        return true;
    {
        std::lock_guard lock(d->mutex);
        d->finishing = true;
    }
    d->cv.notify_one();
    if (d->streaming && !d->connected)
        d->abort = true; // still connecting: give up now
    if (d->thread.joinable()) {
        if (d->streaming) { // a stuck network write must not hang the engine
            std::thread watchdog([this] {
                for (int i = 0; i < 60 && !d->abort; ++i) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    std::lock_guard lock(d->mutex);
                    if (!d->recording && d->queue.empty())
                        break;
                }
                d->abort = true;
            });
            d->thread.join();
            d->abort = true;
            watchdog.join();
        } else {
            d->thread.join();
        }
    }
    d->recording = false;
    d->close();
    const bool wasStream = d->streaming;
    d->streaming = false;
    d->silent = false;
    if (error)
        *error = d->error;
    return d->error.isEmpty() || (wasStream && d->error == QLatin1String("stopped"));
}

bool LiveRecorder::isRecording() const { return d->recording && !d->failed; }
bool LiveRecorder::hasAudio() const { return d->aenc != nullptr; }
double LiveRecorder::startTime() const { return d->t0; }
int64_t LiveRecorder::frames() const { return d->written; }
int64_t LiveRecorder::droppedFrames() const { return d->dropped; }
double LiveRecorder::seconds() const { return double(d->written) / d->fps; }
QString LiveRecorder::encoderName() const { return d->encoder; }

QImage LiveRecorder::thumbnail() const
{
    std::lock_guard lock(d->mutex);
    return d->memory ? d->latestThumb : d->thumb;
}

QString liveRecordingsFolder()
{
    if (qEnvironmentVariableIsSet("MVP_LIVE_DIR")) { // tests
        const QString dir = qEnvironmentVariable("MVP_LIVE_DIR");
        QDir().mkpath(dir);
        return dir;
    }
    const QString profile = Profiles::instance().folder(QStringLiteral("Live"));
    if (!profile.isEmpty()) { QDir().mkpath(profile); return profile; }
    QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    const QString previous = QDir(base).filePath(QStringLiteral("Montage Live"));
    const QString dir = QDir(previous).exists() ? previous : QDir(base).filePath(QStringLiteral("Mutant Video Producer Live"));
    QDir().mkpath(dir);
    return dir;
}

QString newLiveRecordingPath(const QString& prefix, const QString& extension)
{
    const QDir dir(liveRecordingsFolder());
    const QString stem = QStringLiteral("%1 %2").arg(prefix).arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH-mm-ss")));
    QString path = dir.filePath(QStringLiteral("%1.%2").arg(stem, extension));
    for (int i = 2; QFileInfo::exists(path); ++i)
        path = dir.filePath(QStringLiteral("%1 (%2).%3").arg(stem).arg(i).arg(extension));
    return path;
}

QString liveThumbnailPath(const QString& recording)
{
    const QFileInfo fi(recording);
    const QByteArray key = (fi.absoluteFilePath() + QString::number(fi.size()) + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    QString cache = brand::storageLocation(QStandardPaths::CacheLocation);
    const QString dir = QDir(cache).filePath(QStringLiteral("live-thumbs"));
    QDir().mkpath(dir);
    return QDir(dir).filePath(QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Md5).toHex()) + QStringLiteral(".jpg"));
}
