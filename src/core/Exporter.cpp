#include "core/Exporter.h"

#include "core/AudioMixer.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/FFmpeg.h"

#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstring>

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------

bool presetAvailable(const ExportPreset& p)
{
    return (p.videoEncoder.isEmpty() || hw::encoderUsable(p.videoEncoder))
           && (p.audioEncoder.isEmpty() || hw::encoderUsable(p.audioEncoder));
}

ExportSettings settingsFromPreset(const ExportPreset& p, const ProjectSettings& project)
{
    ExportSettings s;
    s.muxer = p.muxer;
    s.videoEncoder = p.videoEncoder;
    s.audioEncoder = p.audioEncoder;
    s.width = project.width;
    s.height = project.height;
    if (p.width > 0 && p.height > 0) {
        s.width = p.width;
        s.height = p.height;
    } else if (p.width > 0) {
        s.width = p.width;
        s.height = std::max(2, int(std::lround(double(p.width) * project.height / project.width / 2.0)) * 2);
    }
    s.fps = p.fps.valid() ? p.fps : project.fps;
    s.quality = p.quality;
    s.videoBitrate = p.videoBitrate;
    s.encoderPreset = p.encoderPreset;
    s.videoOptions = p.videoOptions;
    s.audioBitrate = p.audioBitrate;
    s.sampleRate = p.sampleRate;
    s.channels = p.channels;
    s.pixelFormat = p.pixelFormat;
    s.targetSizeBytes = int64_t(p.targetSizeMB * 1000.0 * 1000.0);
    s.framing = p.framing;
    s.muxerOptions = p.muxerOptions;
    s.gopSeconds = p.gopSeconds;
    return s;
}

ExportSettings withGpuEncoder(const ExportSettings& in, QString* used)
{
    ExportSettings s = in;
    const struct {
        const char* software;
        std::vector<const char*> hardware;
    } map[] = {{"libx264", {"h264_nvenc", "h264_qsv", "h264_amf"}},
               {"libx265", {"hevc_nvenc", "hevc_qsv", "hevc_amf"}},
               {"libsvtav1", {"av1_nvenc", "av1_qsv", "av1_amf"}},
               {"libaom-av1", {"av1_nvenc", "av1_qsv", "av1_amf"}}};
    for (const auto& m : map) {
        if (s.videoEncoder != QLatin1String(m.software))
            continue;
        for (const char* hwName : m.hardware) {
            const QString name = QString::fromLatin1(hwName);
            if (!hw::encoderUsable(name))
                continue;
            // 4:2:2, 4:4:4 and RGB (all-intra, editing templates) stay in software: NVENC's H.264 can't take
            // yuv422p and the others vary by card.
            const QString pf = in.pixelFormat;
            if (pf.contains(QLatin1String("422")) || pf.contains(QLatin1String("444")) || pf.contains(QLatin1String("rgb"))
                || pf.contains(QLatin1String("gbr")))
                break;
            // 10-bit: the GPU encoders take it as P010 (H.264 ones not at all, so those stay in software).
            const bool tenBit = in.pixelFormat.contains(QLatin1String("10"));
            if (tenBit && name.startsWith(QLatin1String("h264")))
                continue;
            if (tenBit)
                s.pixelFormat = QStringLiteral("p010le");
            s.videoEncoder = name;
            // Keep the generic options (profile, bitrate limits); drop the
            // software encoder's own ones.
            QStringList keep;
            for (const QString& o : s.videoOptions.split(QLatin1Char(':'), Qt::SkipEmptyParts)) {
                const QString key = o.section(QLatin1Char('='), 0, 0);
                if (key == QLatin1String("maxrate") || key == QLatin1String("bufsize") || key == QLatin1String("g")
                    || key == QLatin1String("bf") || (key == QLatin1String("profile") && !name.contains(QLatin1String("av1"))))
                    keep << o;
            }
            const bool nvenc = name.endsWith(QLatin1String("nvenc"));
            if (nvenc) {
                s.encoderPreset = QStringLiteral("p5");
                keep << QStringLiteral("rc=vbr") << QStringLiteral("tune=hq");
                if (s.quality >= 0)
                    s.quality = std::clamp(s.quality + (name.startsWith(QLatin1String("av1")) ? 0 : 1), 0, 51);
            } else {
                s.encoderPreset.clear();
                if (s.videoBitrate <= 0 && s.targetSizeBytes <= 0) // no constant-quality mode in common: pick a generous bitrate
                    s.videoBitrate = int64_t(double(s.width) * s.height * s.fps.toDouble() * 0.12);
                s.quality = -1;
            }
            s.videoOptions = keep.join(QLatin1Char(':'));
            if (used)
                *used = name;
            return s;
        }
    }
    return s;
}

QString imageSequencePattern(const QString& path)
{
    if (path.contains(QLatin1Char('%')))
        return path;
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    const int slash = std::max(path.lastIndexOf(QLatin1Char('/')), path.lastIndexOf(QLatin1Char('\\')));
    if (dot <= slash)
        return path + QStringLiteral("_%05d");
    return path.left(dot) + QStringLiteral("_%05d") + path.mid(dot);
}

// ---------------------------------------------------------------------------
// Encoding
// ---------------------------------------------------------------------------

namespace {

AVPixelFormat choosePixelFormat(const AVCodec* codec)
{
    std::vector<AVPixelFormat> fmts;
    for (AVPixelFormat f : ff::encoderPixelFormats(codec))
        if (sws_isSupportedOutput(f))
            fmts.push_back(f);
    if (fmts.empty())
        return AV_PIX_FMT_YUV420P;
    // Prefer the most widely playable formats when the encoder offers them.
    for (AVPixelFormat pref : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P10LE, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV422P})
        if (std::find(fmts.begin(), fmts.end(), pref) != fmts.end())
            return pref;
    fmts.push_back(AV_PIX_FMT_NONE);
    const AVPixelFormat best = avcodec_find_best_pix_fmt_of_list(fmts.data(), AV_PIX_FMT_BGRA, 0, nullptr);
    return best != AV_PIX_FMT_NONE ? best : fmts.front();
}

AVSampleFormat chooseSampleFormat(const AVCodec* codec)
{
    const auto fmts = ff::encoderSampleFormats(codec);
    if (fmts.empty())
        return AV_SAMPLE_FMT_FLTP;
    for (AVSampleFormat pref : {AV_SAMPLE_FMT_FLTP, AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_S16P, AV_SAMPLE_FMT_S16,
                                AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_S32})
        if (std::find(fmts.begin(), fmts.end(), pref) != fmts.end())
            return pref;
    return fmts.front();
}

int chooseSampleRate(const AVCodec* codec, int wanted)
{
    const auto rates = ff::encoderSampleRates(codec);
    if (rates.empty() || std::find(rates.begin(), rates.end(), wanted) != rates.end())
        return wanted;
    return *std::min_element(rates.begin(), rates.end(),
                             [&](int a, int b) { return std::abs(a - wanted) < std::abs(b - wanted); });
}

void chooseChannelLayout(const AVCodec* codec, int channels, AVChannelLayout* out)
{
    AVChannelLayout want{};
    av_channel_layout_default(&want, channels);
    const auto layouts = ff::encoderChannelLayouts(codec);
    if (layouts.empty()) {
        av_channel_layout_copy(out, &want);
        return;
    }
    for (const auto& l : layouts)
        if (av_channel_layout_compare(&l, &want) == 0) {
            av_channel_layout_copy(out, &want);
            return;
        }
    // Prefer the closest channel count the encoder supports.
    const AVChannelLayout* best = &layouts.front();
    for (const auto& l : layouts)
        if (std::abs(l.nb_channels - channels) < std::abs(best->nb_channels - channels))
            best = &l;
    av_channel_layout_copy(out, best);
}

bool hasOption(void* obj, const char* name)
{
    return obj && av_opt_find(obj, name, nullptr, 0, 0) != nullptr;
}

void applyQuality(AVCodecContext* ctx, int quality, bool hasBitrate)
{
    const QByteArray q = QByteArray::number(quality);
    if (hasOption(ctx->priv_data, "crf")) {
        av_opt_set(ctx->priv_data, "crf", q.constData(), 0);
        if (!hasBitrate)
            ctx->bit_rate = 0; // libvpx/libaom: constant-quality mode
    } else if (hasOption(ctx->priv_data, "cq")) {
        av_opt_set(ctx->priv_data, "cq", q.constData(), 0); // NVENC constant quality
        if (!hasBitrate)
            ctx->bit_rate = 0;
    } else if (hasOption(ctx->priv_data, "global_quality") || hasOption(ctx, "global_quality")) {
        ctx->flags |= AV_CODEC_FLAG_QSCALE;
        ctx->global_quality = FF_QP2LAMBDA * std::clamp(quality, 1, 31);
    }
}

AVDictionary* parseOptions(const QString& text)
{
    AVDictionary* dict = nullptr;
    const QByteArray utf8 = text.trimmed().toUtf8();
    if (!utf8.isEmpty())
        av_dict_parse_string(&dict, utf8.constData(), "=", ":", 0);
    return dict;
}

struct OutputGuard {
    AVFormatContext* oc = nullptr;
    bool ioOpen = false;
    ~OutputGuard()
    {
        if (!oc)
            return;
        if (ioOpen)
            avio_closep(&oc->pb);
        avformat_free_context(oc);
    }
};

} // namespace

bool Exporter::run(const ProjectSnapshot& snap, const ExportSettings& s, const Progress& progress, QOffscreenSurface* gpuSurface, const std::function<void(const ExportStatus&)>& status)
{
    m_error.clear();
    m_rendererName.clear();
    ExportStatus info;
    QElapsedTimer clock, publishClock;
    clock.start(); publishClock.start();
    auto publish = [&](bool force = false) {
        if (!status || (!force && publishClock.elapsed() < 200)) return;
        info.activeClips.clear();
        if (info.stage == QLatin1String("Rendering")) {
            const TimeUs at = TimeUs(info.position * kUsPerSecond);
            bool soloAudio = false, soloVideo = false;
            for (const auto& track : snap.timeline.tracks)
                if (track.solo && !track.muted) (track.type == TrackType::Audio ? soloAudio : soloVideo) = true;
            for (const auto& track : snap.timeline.tracks) {
                if (track.muted || ((track.type == TrackType::Audio ? soloAudio : soloVideo) && !track.solo)) continue;
                for (const auto& clip : track.clips) {
                    if (clip.muted || at < clip.start || at >= clip.start + clip.length) continue;
                    const auto media = snap.media.find(clip.mediaId);
                    const QString source = media != snap.media.end() ? media->second->path : QStringLiteral("Generated media");
                    QStringList effects;
                    for (const auto& effect : clip.effects) if (effect.enabled) effects.append(effect.type);
                    for (const auto& effect : track.effects) if (effect.enabled) effects.append(effect.type);
                    info.activeClips.append(QStringLiteral("%1 / %2\n  Source: %3\n  Effects: %4")
                        .arg(track.name, clip.name, source, effects.isEmpty() ? QStringLiteral("None") : effects.join(", ")));
                }
            }
        }
        info.elapsed = clock.nsecsElapsed() / 1e9;
        info.fileBytes = QFileInfo(s.path).size();
        status(info); publishClock.restart();
    };
    auto stage = [&](const QString& name, const QString& detail) {
        info.stage = name; info.detail = detail; publish(true);
    };
    stage(QStringLiteral("Preparing"), QStringLiteral("Validating timeline range and output settings"));
    auto fail = [&](QString msg, int err = 0) {
        if (err < 0)
            msg += QStringLiteral(": ") + ff::errorString(err);
        m_error = msg;
        stage(msg == QLatin1String("Cancelled") ? QStringLiteral("Cancelled") : QStringLiteral("Failed"), msg);
        return false;
    };
    if (s.rangeEnd <= s.rangeStart)
        return fail(QStringLiteral("Nothing to render: the time range is empty"));
    if (s.videoEncoder.isEmpty() && s.audioEncoder.isEmpty())
        return fail(QStringLiteral("Choose a video and/or an audio codec"));
    if (s.path.isEmpty())
        return fail(QStringLiteral("No output file given"));
    if (QFileInfo(s.path).isDir())
        return fail(QStringLiteral("%1 is a folder: give a file name").arg(QDir::toNativeSeparators(s.path)));
    // Writing over a file the project reads would destroy it while it is read.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    constexpr Qt::CaseSensitivity names = Qt::CaseInsensitive;
#else
    constexpr Qt::CaseSensitivity names = Qt::CaseSensitive;
#endif
    const QString target = QFileInfo(s.path).absoluteFilePath();
    for (const auto& [id, m] : snap.media)
        if (QFileInfo::exists(m->path) && QFileInfo(m->path).absoluteFilePath().compare(target, names) == 0)
            return fail(QStringLiteral("Can't export over %1: the project uses that file. Choose another name.").arg(QFileInfo(m->path).fileName()));
    if (!s.videoEncoder.isEmpty() && (!s.fps.valid() || s.width < 1 || s.height < 1))
        return fail(QStringLiteral("The picture size and frame rate must be above 0 (%1x%2, %3/%4)").arg(s.width).arg(s.height).arg(s.fps.num).arg(s.fps.den));

    const bool sequence = s.muxer == QLatin1String("image2");
    const QString outPath = sequence ? imageSequencePattern(s.path) : s.path;
    if (sequence && !s.audioEncoder.isEmpty())
        return fail(QStringLiteral("Image sequences have no audio: turn audio off"));
    const QByteArray pathUtf8 = outPath.toUtf8();
    const QByteArray muxerName = s.muxer.toUtf8();
    OutputGuard out;
    int ret = avformat_alloc_output_context2(&out.oc, nullptr, muxerName.isEmpty() ? nullptr : muxerName.constData(),
                                             pathUtf8.constData());
    if (ret < 0 || !out.oc)
        return fail(QStringLiteral("Cannot create output format \"%1\"").arg(s.muxer), ret);
    AVFormatContext* oc = out.oc;
    const bool globalHeader = oc->oformat->flags & AVFMT_GLOBALHEADER;

    stage(QStringLiteral("Opening encoders"), QStringLiteral("Configuring video codec, pixel format and rate control"));
    // ---- Video stream ------------------------------------------------------
    ff::CodecContextPtr venc;
    AVStream* vst = nullptr;
    ff::FramePtr vframe;
    ff::SwsPtr sws;
    int width = s.width, height = s.height;
    if (!s.videoEncoder.isEmpty()) {
        const QByteArray name = s.videoEncoder.toUtf8();
        const AVCodec* codec = avcodec_find_encoder_by_name(name.constData());
        if (!codec)
            return fail(QStringLiteral("Video encoder \"%1\" is not available").arg(s.videoEncoder));
        venc.reset(avcodec_alloc_context3(codec));
        AVPixelFormat pixFmt = choosePixelFormat(codec);
        if (!s.pixelFormat.isEmpty()) {
            const AVPixelFormat wanted = av_get_pix_fmt(s.pixelFormat.toUtf8().constData());
            const auto supported = ff::encoderPixelFormats(codec);
            if (wanted == AV_PIX_FMT_NONE || !sws_isSupportedOutput(wanted)
                || (!supported.empty() && std::find(supported.begin(), supported.end(), wanted) == supported.end()))
                return fail(QStringLiteral("%1 cannot encode pixel format %2").arg(s.videoEncoder, s.pixelFormat));
            pixFmt = wanted;
        }
        const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixFmt);
        if (desc && desc->log2_chroma_w)
            width &= ~1;
        if (desc && desc->log2_chroma_h)
            height &= ~1;
        width = std::max(width, 2);
        height = std::max(height, 2);

        venc->width = width;
        venc->height = height;
        venc->pix_fmt = pixFmt;
        venc->time_base = AVRational{s.fps.den, s.fps.num};
        venc->framerate = AVRational{s.fps.num, s.fps.den};
        venc->sample_aspect_ratio = AVRational{1, 1};
        venc->gop_size = std::max(1, int(std::lround(s.fps.toDouble() * (s.gopSeconds > 0 ? s.gopSeconds : 2.0))));
        venc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        venc->thread_count = 0;
        int64_t videoBitrate = s.videoBitrate;
        int quality = s.quality;
        if (s.targetSizeBytes > 0) {
            // Fit the file: total bits over the duration, minus audio and ~6% container / rate-control margin.
            const double secs = std::max(0.1, double(s.rangeEnd - s.rangeStart) / double(kUsPerSecond));
            const double audio = s.audioEncoder.isEmpty() ? 0.0 : double(std::max<int64_t>(s.audioBitrate, 64000));
            const double videoBits = double(s.targetSizeBytes) * 8.0 * 0.94 - audio * secs;
            // With a constant-quality template the size is a ceiling (short or simple clips stay small);
            // otherwise the bitrate itself is chosen to fill the size. A ceiling has to leave room for
            // the rate-control buffer (one second of the max rate), which the encoder may spend on top
            // of the max rate: on short clips that alone would overshoot the size.
            // ...and no more than about a bit per pixel per frame: a big size over a short render asked for
            // gigabits a second, which encoders (NVENC) refuse and nothing needs.
            const double perSecond = std::min(videoBits / (quality >= 0 ? secs + 1.0 : secs),
                                              std::max(2e6, double(venc->width) * venc->height * std::max(1.0, s.fps.toDouble())));
            const int64_t cap = std::max<int64_t>(50000, int64_t(perSecond));
            videoBitrate = quality >= 0 ? 0 : cap;
            venc->rc_max_rate = cap;
            venc->rc_buffer_size = int(std::min<int64_t>(INT32_MAX, cap));
        }
        if (videoBitrate > 0)
            venc->bit_rate = videoBitrate;
        if (quality >= 0)
            applyQuality(venc.get(), quality, videoBitrate > 0);
        if (!s.encoderPreset.isEmpty() && hasOption(venc->priv_data, "preset"))
            av_opt_set(venc->priv_data, "preset", s.encoderPreset.toUtf8().constData(), 0);

        const bool isYuv = desc && !(desc->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)) && desc->nb_components >= 3;
        bool fullRange = false;
        if (isYuv) {
            const auto ranges = ff::encoderColorRanges(codec);
            fullRange = codec->id == AV_CODEC_ID_MJPEG
                        || (!ranges.empty() && std::find(ranges.begin(), ranges.end(), AVCOL_RANGE_MPEG) == ranges.end());
            const bool hd = height >= 720;
            venc->colorspace = hd ? AVCOL_SPC_BT709 : AVCOL_SPC_SMPTE170M;
            venc->color_primaries = hd ? AVCOL_PRI_BT709 : AVCOL_PRI_SMPTE170M;
            venc->color_trc = hd ? AVCOL_TRC_BT709 : AVCOL_TRC_SMPTE170M;
            venc->color_range = fullRange ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
        }
        if (globalHeader)
            venc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

        AVDictionary* opts = parseOptions(s.videoOptions);
        // "tag=hvc1": the container's codec tag (Apple needs hvc1 for HEVC, DivX players XVID).
        uint32_t codecTag = 0;
        if (const AVDictionaryEntry* t = av_dict_get(opts, "tag", nullptr, 0); t && std::strlen(t->value) == 4) {
            codecTag = MKTAG(t->value[0], t->value[1], t->value[2], t->value[3]);
            av_dict_set(&opts, "tag", nullptr, 0);
        }
        ret = avcodec_open2(venc.get(), codec, &opts);
        av_dict_free(&opts);
        if (ret < 0)
            return fail(QStringLiteral("Could not open video encoder %1 (%2x%3, %4)")
                            .arg(s.videoEncoder)
                            .arg(width)
                            .arg(height)
                            .arg(QString::fromUtf8(av_get_pix_fmt_name(pixFmt))),
                        ret);

        info.videoEncoder = QString::fromUtf8(codec->name);
        info.pixelFormat = QString::fromUtf8(av_get_pix_fmt_name(pixFmt));
        info.width = width; info.height = height;
        publish(true);
        vst = avformat_new_stream(oc, nullptr);
        vst->time_base = venc->time_base;
        vst->avg_frame_rate = venc->framerate;
        avcodec_parameters_from_context(vst->codecpar, venc.get());
        if (codecTag)
            vst->codecpar->codec_tag = codecTag;

        vframe = ff::makeFrame();
        vframe->format = pixFmt;
        vframe->width = width;
        vframe->height = height;
        vframe->color_range = venc->color_range;
        vframe->colorspace = venc->colorspace;
        vframe->color_primaries = venc->color_primaries;
        vframe->color_trc = venc->color_trc;
        if ((ret = av_frame_get_buffer(vframe.get(), 0)) < 0)
            return fail(QStringLiteral("Out of memory"), ret);

        sws.reset(sws_getContext(width, height, AV_PIX_FMT_BGRA, width, height, pixFmt,
                                 SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP, nullptr, nullptr, nullptr));
        if (!sws)
            return fail(QStringLiteral("Unsupported pixel format conversion to %1")
                            .arg(QString::fromUtf8(av_get_pix_fmt_name(pixFmt))));
        if (isYuv)
            sws_setColorspaceDetails(sws.get(), sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                     sws_getCoefficients(height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601),
                                     fullRange ? 1 : 0, 0, 1 << 16, 1 << 16);
    }

    stage(QStringLiteral("Opening audio encoder"), QStringLiteral("Configuring sample format, channel layout and resampler"));
    // ---- Audio stream ------------------------------------------------------
    ff::CodecContextPtr aenc;
    AVStream* ast = nullptr;
    ff::SwrPtr swr;
    std::unique_ptr<AVAudioFifo, void (*)(AVAudioFifo*)> fifo(nullptr, av_audio_fifo_free);
    int rate = s.sampleRate;
    int frameSize = 1024;
    bool smallLastFrame = false;
    if (!s.audioEncoder.isEmpty()) {
        const QByteArray name = s.audioEncoder.toUtf8();
        const AVCodec* codec = avcodec_find_encoder_by_name(name.constData());
        if (!codec)
            return fail(QStringLiteral("Audio encoder \"%1\" is not available").arg(s.audioEncoder));
        aenc.reset(avcodec_alloc_context3(codec));
        rate = chooseSampleRate(codec, s.sampleRate);
        aenc->sample_fmt = chooseSampleFormat(codec);
        aenc->sample_rate = rate;
        chooseChannelLayout(codec, std::clamp(s.channels, 1, 2), &aenc->ch_layout);
        aenc->time_base = AVRational{1, rate};
        aenc->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
        if (s.audioBitrate > 0)
            aenc->bit_rate = s.audioBitrate;
        if (globalHeader)
            aenc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        AVDictionary* opts = parseOptions(s.audioOptions);
        ret = avcodec_open2(aenc.get(), codec, &opts);
        av_dict_free(&opts);
        if (ret < 0)
            return fail(QStringLiteral("Could not open audio encoder %1 (%2 Hz, %3 ch)")
                            .arg(s.audioEncoder)
                            .arg(rate)
                            .arg(aenc->ch_layout.nb_channels),
                        ret);
        const bool variable = codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE;
        frameSize = (aenc->frame_size > 0 && !variable) ? aenc->frame_size : 1024;
        smallLastFrame = variable || (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME);

        info.audioEncoder = QString::fromUtf8(codec->name);
        info.sampleFormat = QString::fromUtf8(av_get_sample_fmt_name(aenc->sample_fmt));
        info.sampleRate = rate; info.channels = aenc->ch_layout.nb_channels;
        publish(true);
        ast = avformat_new_stream(oc, nullptr);
        ast->time_base = aenc->time_base;
        avcodec_parameters_from_context(ast->codecpar, aenc.get());

        SwrContext* raw = nullptr;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&raw, &aenc->ch_layout, aenc->sample_fmt, rate, &stereo, AV_SAMPLE_FMT_FLT, rate, 0,
                                nullptr) < 0
            || swr_init(raw) < 0) {
            swr_free(&raw);
            return fail(QStringLiteral("Could not set up audio conversion"));
        }
        swr.reset(raw);
        fifo.reset(av_audio_fifo_alloc(aenc->sample_fmt, aenc->ch_layout.nb_channels, frameSize * 4));
    }

    // ---- Subtitle track: the captions as text (MP4 / MOV timed text, SRT in Matroska, WebVTT in WebM) ----
    AVStream* sst = nullptr;
    if (!s.captions.empty() && !sequence) {
        const QString muxer = QString::fromLatin1(oc->oformat->name);
        const AVCodecID id = muxer.contains(QLatin1String("webm")) ? AV_CODEC_ID_WEBVTT
                             : muxer.contains(QLatin1String("matroska")) ? AV_CODEC_ID_SUBRIP
                                                                         : AV_CODEC_ID_MOV_TEXT;
        if (avformat_query_codec(oc->oformat, id, FF_COMPLIANCE_NORMAL) == 1) {
            sst = avformat_new_stream(oc, nullptr);
            sst->time_base = AVRational{1, 1000};
            sst->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
            sst->codecpar->codec_id = id;
        }
    }

    stage(QStringLiteral("Opening output"), QStringLiteral("Creating output file and writing container header"));
    // ---- Open file, write header -------------------------------------------
    QDir().mkpath(QFileInfo(s.path).absolutePath()); // a folder given that isn't there yet (image sequences too)
    if (!(oc->oformat->flags & AVFMT_NOFILE)) {
        if ((ret = avio_open(&oc->pb, pathUtf8.constData(), AVIO_FLAG_WRITE)) < 0)
            return fail(QStringLiteral("Cannot write to %1").arg(s.path), ret);
        out.ioOpen = true;
    }
    AVDictionary* muxOpts = parseOptions(s.muxerOptions);
    if (hasOption(oc->priv_data, "movflags") && !av_dict_get(muxOpts, "movflags", nullptr, 0))
        av_dict_set(&muxOpts, "movflags", "+faststart", 0);
    ret = avformat_write_header(oc, &muxOpts);
    av_dict_free(&muxOpts);
    if (ret < 0)
        return fail(QStringLiteral("Could not write file header"), ret);

    ff::PacketPtr pkt = ff::makePacket();
    stage(QStringLiteral("Writing subtitles"), QStringLiteral("Interleaving caption packets into the container"));
    if (sst) // written up front: the muxer interleaves them with the pictures and sound
        for (const Caption& c : s.captions) {
            const TimeUs a = std::max(c.start, s.rangeStart), b = std::min(c.end, s.rangeEnd);
            if (b <= a)
                continue;
            QByteArray text = c.text.toUtf8();
            if (sst->codecpar->codec_id == AV_CODEC_ID_MOV_TEXT) // (a 16-bit length, then the text)
                text.prepend(char(text.size() & 0xff)).prepend(char((text.size() >> 8) & 0xff));
            if (av_new_packet(pkt.get(), int(text.size())) < 0)
                return fail(QStringLiteral("Out of memory"));
            memcpy(pkt->data, text.constData(), size_t(text.size()));
            pkt->pts = pkt->dts = av_rescale_q(a - s.rangeStart, AVRational{1, int(kUsPerSecond)}, sst->time_base);
            pkt->duration = av_rescale_q(b - a, AVRational{1, int(kUsPerSecond)}, sst->time_base);
            pkt->stream_index = sst->index;
            pkt->flags |= AV_PKT_FLAG_KEY;
            ++info.subtitlePackets; info.packetBytes += pkt->size;
            if ((ret = av_interleaved_write_frame(oc, pkt.get())) < 0)
                return fail(QStringLiteral("Could not write the subtitles"), ret);
        }
    auto encode = [&](AVCodecContext* ctx, AVStream* st, AVFrame* f) -> int {
        QElapsedTimer encodingTimer; encodingTimer.start();
        int r = avcodec_send_frame(ctx, f);
        if (r < 0 && r != AVERROR_EOF)
            return r;
        for (;;) {
            r = avcodec_receive_packet(ctx, pkt.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                info.encodingSeconds += encodingTimer.nsecsElapsed() / 1e9;
                return 0;
            }
            if (r < 0)
                return r;
            av_packet_rescale_ts(pkt.get(), ctx->time_base, st->time_base);
            pkt->stream_index = st->index;
            if (st == vst) ++info.videoPackets; else ++info.audioPackets;
            info.packetBytes += pkt->size;
            info.encodingSeconds += encodingTimer.nsecsElapsed() / 1e9;
            QElapsedTimer muxTimer; muxTimer.start();
            r = av_interleaved_write_frame(oc, pkt.get());
            info.muxingSeconds += muxTimer.nsecsElapsed() / 1e9;
            encodingTimer.restart();
            if (r < 0)
                return r;
        }
    };

    auto failAndRemove = [&](const QString& msg, int err = 0) {
        if (out.ioOpen) {
            avio_closep(&oc->pb);
            out.ioOpen = false;
        }
        if (!sequence)
            QFile::remove(outPath);
        return fail(msg, err);
    };

    // ---- Main loop -----------------------------------------------------------
    const TimeUs duration = s.rangeEnd - s.rangeStart;
    const int64_t totalFrames = venc ? s.fps.frameAt(duration - 1) + 1 : 0;
    const int64_t totalSamples = aenc ? usToSamples(duration, rate) : 0;
    const int64_t rangeStartSample = usToSamples(s.rangeStart, rate);
    info.totalFrames = totalFrames; info.totalSamples = totalSamples;
    stage(QStringLiteral("Rendering"), QStringLiteral("Decoding sources, applying effects, compositing pictures and mixing audio"));
    int64_t samplesMixed = 0;
    int64_t audioPts = 0;
    std::unique_ptr<AudioMixer> mixer = aenc ? std::make_unique<AudioMixer>(rate) : nullptr;
    std::vector<float> mixBuf;

    auto encodeAudioFrame = [&](int nb) -> int {
        ff::FramePtr f = ff::makeFrame();
        f->nb_samples = nb;
        f->format = aenc->sample_fmt;
        f->sample_rate = rate;
        av_channel_layout_copy(&f->ch_layout, &aenc->ch_layout);
        int r = av_frame_get_buffer(f.get(), 0);
        if (r < 0)
            return r;
        av_audio_fifo_read(fifo.get(), reinterpret_cast<void**>(f->data), nb);
        f->pts = audioPts;
        audioPts += nb;
        return encode(aenc.get(), ast, f.get());
    };

    auto pushToFifo = [&](const float* in, int n) -> int {
        const int cap = swr_get_out_samples(swr.get(), n);
        if (cap <= 0)
            return 0;
        ff::FramePtr tmp = ff::makeFrame();
        tmp->nb_samples = cap;
        tmp->format = aenc->sample_fmt;
        av_channel_layout_copy(&tmp->ch_layout, &aenc->ch_layout);
        int r = av_frame_get_buffer(tmp.get(), 0);
        if (r < 0)
            return r;
        const uint8_t* inPtr = reinterpret_cast<const uint8_t*>(in);
        const int got = swr_convert(swr.get(), tmp->data, cap, in ? &inPtr : nullptr, in ? n : 0);
        if (got > 0)
            av_audio_fifo_write(fifo.get(), reinterpret_cast<void**>(tmp->data), got);
        return 0;
    };

    // Mixes and encodes audio until `upTo` samples have been produced.
    auto pumpAudio = [&](int64_t upTo) -> int {
        while (samplesMixed < upTo) {
            const int n = int(std::min<int64_t>(4096, upTo - samplesMixed));
            mixBuf.resize(size_t(n) * 2);
            QElapsedTimer mixTimer; mixTimer.start();
            mixer->mix(snap, rangeStartSample + samplesMixed, n, mixBuf.data());
            info.audioSeconds += mixTimer.nsecsElapsed() / 1e9;
            samplesMixed += n;
            info.samples = samplesMixed;
            if (int r = pushToFifo(mixBuf.data(), n); r < 0)
                return r;
            while (av_audio_fifo_size(fifo.get()) >= frameSize)
                if (int r = encodeAudioFrame(frameSize); r < 0)
                    return r;
        }
        return 0;
    };

    if (venc) {
        std::unique_ptr<FrameRenderer> renderer = createFrameRenderer(true, gpuSurface);
        m_rendererName = renderer->name();
        info.renderer = m_rendererName; publish(true);
        const TimeUs frameDur = s.fps.frameDuration();
        const QSize outSize(width, height);
        const QSize project(std::max(1, snap.settings.width), std::max(1, snap.settings.height));
        const bool reframe = std::abs((double(project.width()) / project.height()) / (double(width) / height) - 1.0) > 0.01;
        auto even = [](QSize z) { return QSize(std::max(2, z.width() & ~1), std::max(2, z.height() & ~1)); };
        const QSize fitSize = even(project.scaled(outSize, Qt::KeepAspectRatio));
        const QSize fillSize = even(project.scaled(outSize, Qt::KeepAspectRatioByExpanding));
        auto renderFrame = [&](TimeUs t) -> QImage {
            if (!reframe)
                return renderer->render(snap, t, outSize);
            QImage canvas(outSize, QImage::Format_ARGB32_Premultiplied);
            canvas.fill(Qt::black);
            QPainter p(&canvas);
            p.setRenderHint(QPainter::SmoothPixmapTransform);
            switch (s.framing) {
            case Framing::Fill: {
                const QImage img = renderer->render(snap, t, fillSize);
                p.drawImage(QPoint((width - img.width()) / 2, (height - img.height()) / 2), img);
                break;
            }
            case Framing::Stretch:
                p.drawImage(QRect(QPoint(0, 0), outSize), renderer->render(snap, t, fitSize));
                break;
            case Framing::BlurFit: {
                const QImage img = renderer->render(snap, t, fitSize);
                // Background: the same picture filling the frame, heavily blurred and dimmed.
                const QImage tiny = img.scaled(std::max(4, img.width() / 24), std::max(4, img.height() / 24), Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
                const QImage mid = tiny.scaled(fillSize / 4, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                p.drawImage(QRect(QPoint((width - fillSize.width()) / 2, (height - fillSize.height()) / 2), fillSize), mid);
                p.fillRect(canvas.rect(), QColor(0, 0, 0, 90));
                p.drawImage(QPoint((width - img.width()) / 2, (height - img.height()) / 2), img);
                break;
            }
            case Framing::Fit:
                p.drawImage(QPoint((width - fitSize.width()) / 2, (height - fitSize.height()) / 2), renderer->render(snap, t, fitSize));
                break;
            }
            p.end();
            return canvas;
        };
        for (int64_t n = 0; n < totalFrames; ++n) {
            const TimeUs tv = s.fps.timeOf(n);
            if (aenc && (ret = pumpAudio(std::min(totalSamples, usToSamples(tv + frameDur, rate)))) < 0)
                return failAndRemove(QStringLiteral("Audio encoding failed"), ret);

            QElapsedTimer frameTimer; frameTimer.start();
            if ((ret = av_frame_make_writable(vframe.get())) < 0)
                return failAndRemove(QStringLiteral("Out of memory"), ret);
            if (!reframe && renderer->renderYuv(snap, s.rangeStart + tv, vframe.get())) {
                info.colorConversion = QStringLiteral("GPU shader (includes composition; packed YUV readback)");
                info.renderingSeconds += frameTimer.nsecsElapsed() / 1e9;
            } else {
                const QImage img = renderFrame(s.rangeStart + tv);
                info.renderingSeconds += frameTimer.nsecsElapsed() / 1e9;
                frameTimer.restart();
                const uint8_t* src[4] = {img.constBits(), nullptr, nullptr, nullptr};
                const int srcStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
                sws_scale(sws.get(), src, srcStride, 0, height, vframe->data, vframe->linesize);
                info.conversionSeconds += frameTimer.nsecsElapsed() / 1e9;
            }
            vframe->pts = n;
            if ((ret = encode(venc.get(), vst, vframe.get())) < 0)
                return failAndRemove(QStringLiteral("Video encoding failed"), ret);

            info.frames = n + 1; info.position = double(s.rangeStart + tv) / kUsPerSecond;
            info.progress = double(n + 1) / double(totalFrames); publish();
            if (progress && !progress(info.progress))
                return failAndRemove(QStringLiteral("Cancelled"));
        }
    }

    if (aenc) {
        while (samplesMixed < totalSamples) {
            if ((ret = pumpAudio(std::min(totalSamples, samplesMixed + rate))) < 0)
                return failAndRemove(QStringLiteral("Audio encoding failed"), ret);
            if (!venc) {
                info.progress = double(samplesMixed) / double(totalSamples);
                info.position = double(s.rangeStart) / kUsPerSecond + double(samplesMixed) / rate;
                publish();
            }
            if (!venc && progress && !progress(info.progress))
                return failAndRemove(QStringLiteral("Cancelled"));
        }
        stage(QStringLiteral("Flushing audio"), QStringLiteral("Draining resampler, audio FIFO and delayed encoder packets"));
        pushToFifo(nullptr, 0); // flush resampler
        while (av_audio_fifo_size(fifo.get()) >= frameSize)
            if ((ret = encodeAudioFrame(frameSize)) < 0)
                return failAndRemove(QStringLiteral("Audio encoding failed"), ret);
        const int remaining = av_audio_fifo_size(fifo.get());
        if (remaining > 0) {
            if (!smallLastFrame) {
                // Pad the last frame with silence for fixed-frame-size codecs.
                std::vector<float> silence(size_t(frameSize - remaining) * 2, 0.0f);
                pushToFifo(silence.data(), frameSize - remaining);
            }
            if ((ret = encodeAudioFrame(std::min(frameSize, av_audio_fifo_size(fifo.get())))) < 0)
                return failAndRemove(QStringLiteral("Audio encoding failed"), ret);
        }
        if ((ret = encode(aenc.get(), ast, nullptr)) < 0)
            return failAndRemove(QStringLiteral("Audio encoder flush failed"), ret);
    }
    stage(QStringLiteral("Flushing video"), QStringLiteral("Writing delayed video frames and encoder packets"));
    if (venc && (ret = encode(venc.get(), vst, nullptr)) < 0)
        return failAndRemove(QStringLiteral("Video encoder flush failed"), ret);

    stage(QStringLiteral("Finalizing container"), QStringLiteral("Writing index and trailer; MP4 fast-start may relocate metadata. Completion time cannot be estimated."));
    if ((ret = av_write_trailer(oc)) < 0)
        return failAndRemove(QStringLiteral("Could not finish the file"), ret);
    stage(QStringLiteral("Closing output"), QStringLiteral("Flushing file buffers and checking the final write"));
    if (out.ioOpen) {
        ret = avio_closep(&oc->pb); out.ioOpen = false;
        if (ret < 0) return failAndRemove(QStringLiteral("Could not close output file"), ret);
    }
    info.progress = 1;
    stage(QStringLiteral("Complete"), QStringLiteral("Output file finalized and closed successfully"));
    if (progress)
        progress(1.0);
    return true;
}
