#include "core/FFmpeg.h"

#include <cmath>
#include <cstring>

namespace ff {

void initialize()
{
    // Broken/odd files make some decoders very chatty; errors only.
    av_log_set_level(AV_LOG_ERROR);
}

QString errorString(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return QString::fromUtf8(buf);
}

int openInput(const QString& path, InputFormatPtr& out)
{
    out.reset();
    AVFormatContext* ctx = nullptr;
    AVDictionary* opts = nullptr;
    // Larger probe window than the defaults: helps with headerless streams,
    // VBR MP3, MPEG-TS/PS and other containers that need more data to
    // identify every stream.
    av_dict_set(&opts, "probesize", "20000000", 0);
    av_dict_set(&opts, "analyzeduration", "10000000", 0);
    const QByteArray utf8 = path.toUtf8(); // FFmpeg expects UTF-8 on all platforms
    int ret = avformat_open_input(&ctx, utf8.constData(), nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0)
        return ret;
    out.reset(ctx);
    ret = avformat_find_stream_info(ctx, nullptr);
    if (ret < 0 && ctx->nb_streams == 0)
        return ret;
    // A partial failure still leaves usable streams; let callers decide.
    return 0;
}

int openDecoder(AVStream* stream, CodecContextPtr& out, const std::function<void(AVCodecContext*, const AVCodec*)>& configure,
                const AVCodec* codecOverride)
{
    out.reset();
    const AVCodec* codec = codecOverride ? codecOverride : avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return AVERROR_DECODER_NOT_FOUND;
    CodecContextPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx)
        return AVERROR(ENOMEM);
    int ret = avcodec_parameters_to_context(ctx.get(), stream->codecpar);
    if (ret < 0)
        return ret;
    ctx->pkt_timebase = stream->time_base;
    ctx->thread_count = 0; // auto
    ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (configure)
        configure(ctx.get(), codec);
    ret = avcodec_open2(ctx.get(), codec, nullptr);
    if (ret < 0)
        return ret;
    out = std::move(ctx);
    return 0;
}

bool isImageDemuxer(const AVInputFormat* fmt)
{
    if (!fmt || !fmt->name)
        return false;
    const char* n = fmt->name;
    if (std::strcmp(n, "image2") == 0)
        return true;
    const size_t len = std::strlen(n);
    return len > 5 && std::strcmp(n + len - 5, "_pipe") == 0;
}

int streamRotation(const AVStream* st)
{
    const uint8_t* data = nullptr;
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(60, 31, 102)
    const AVPacketSideData* sd = av_packet_side_data_get(st->codecpar->coded_side_data,
                                                         st->codecpar->nb_coded_side_data,
                                                         AV_PKT_DATA_DISPLAYMATRIX);
    if (sd)
        data = sd->data;
#else
    data = av_stream_get_side_data(st, AV_PKT_DATA_DISPLAYMATRIX, nullptr);
#endif
    if (!data)
        return 0;
    const double theta = -av_display_rotation_get(reinterpret_cast<const int32_t*>(data));
    if (std::isnan(theta))
        return 0;
    int deg = int(std::lround(theta / 90.0)) * 90;
    deg %= 360;
    if (deg < 0)
        deg += 360;
    return deg;
}

int64_t formatStartTime(const AVFormatContext* fc)
{
    return (fc->start_time != AV_NOPTS_VALUE && fc->start_time > 0) ? fc->start_time : 0;
}

namespace {

// AV_CODEC_CONFIG_* only exist with avcodec_get_supported_config (FFmpeg 7.1+).
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
#define MVP_CODEC_CONFIG(name) AV_CODEC_CONFIG_##name
#else
#define MVP_CODEC_CONFIG(name) 0
#endif

template <typename T>
std::vector<T> supportedConfig(const AVCodec* c, [[maybe_unused]] int config, const T* legacy, bool (*isEnd)(const T&))
{
    std::vector<T> out;
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    (void)legacy;
    const void* cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, static_cast<AVCodecConfig>(config), 0, &cfg, &n) >= 0 && cfg) {
        const T* p = static_cast<const T*>(cfg);
        out.assign(p, p + n);
    }
#else
    if (legacy)
        for (const T* p = legacy; !isEnd(*p); ++p)
            out.push_back(*p);
#endif
    (void)isEnd;
    return out;
}

} // namespace

std::vector<AVPixelFormat> encoderPixelFormats(const AVCodec* c)
{
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    const AVPixelFormat* legacy = nullptr;
#else
    const AVPixelFormat* legacy = c->pix_fmts;
#endif
    return supportedConfig<AVPixelFormat>(c, MVP_CODEC_CONFIG(PIX_FORMAT), legacy,
                                          [](const AVPixelFormat& f) { return f == AV_PIX_FMT_NONE; });
}

std::vector<AVSampleFormat> encoderSampleFormats(const AVCodec* c)
{
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    const AVSampleFormat* legacy = nullptr;
#else
    const AVSampleFormat* legacy = c->sample_fmts;
#endif
    return supportedConfig<AVSampleFormat>(c, MVP_CODEC_CONFIG(SAMPLE_FORMAT), legacy,
                                           [](const AVSampleFormat& f) { return f == AV_SAMPLE_FMT_NONE; });
}

std::vector<int> encoderSampleRates(const AVCodec* c)
{
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    const int* legacy = nullptr;
#else
    const int* legacy = c->supported_samplerates;
#endif
    return supportedConfig<int>(c, MVP_CODEC_CONFIG(SAMPLE_RATE), legacy, [](const int& r) { return r == 0; });
}

std::vector<AVChannelLayout> encoderChannelLayouts(const AVCodec* c)
{
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    const AVChannelLayout* legacy = nullptr;
#else
    const AVChannelLayout* legacy = c->ch_layouts;
#endif
    return supportedConfig<AVChannelLayout>(c, MVP_CODEC_CONFIG(CHANNEL_LAYOUT), legacy,
                                            [](const AVChannelLayout& l) { return l.nb_channels == 0; });
}

std::vector<AVColorRange> encoderColorRanges(const AVCodec* c)
{
#ifdef MVP_FF_HAS_SUPPORTED_CONFIG
    std::vector<AVColorRange> out;
    const void* cfg = nullptr;
    int n = 0;
    if (avcodec_get_supported_config(nullptr, c, AV_CODEC_CONFIG_COLOR_RANGE, 0, &cfg, &n) >= 0 && cfg) {
        const AVColorRange* p = static_cast<const AVColorRange*>(cfg);
        out.assign(p, p + n);
    }
    return out;
#else
    (void)c;
    return {};
#endif
}

} // namespace ff
