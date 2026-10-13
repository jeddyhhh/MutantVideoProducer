#pragma once

// Single include point for FFmpeg plus small RAII helpers and
// version-portable capability queries.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/display.h>
#include <libavutil/imgutils.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <QString>
#include <functional>
#include <memory>
#include <vector>

#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
#define MVP_FF_HAS_SUPPORTED_CONFIG 1
#endif

namespace ff {

void initialize();
QString errorString(int errnum);

struct InputFormatDeleter {
    void operator()(AVFormatContext* c) const noexcept { avformat_close_input(&c); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext* c) const noexcept { avcodec_free_context(&c); }
};
struct FrameDeleter {
    void operator()(AVFrame* f) const noexcept { av_frame_free(&f); }
};
struct PacketDeleter {
    void operator()(AVPacket* p) const noexcept { av_packet_free(&p); }
};
struct SwsDeleter {
    void operator()(SwsContext* s) const noexcept { sws_freeContext(s); }
};
struct SwrDeleter {
    void operator()(SwrContext* s) const noexcept { swr_free(&s); }
};

using InputFormatPtr = std::unique_ptr<AVFormatContext, InputFormatDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

inline FramePtr makeFrame() { return FramePtr(av_frame_alloc()); }
inline PacketPtr makePacket() { return PacketPtr(av_packet_alloc()); }

// Opens a media file with generous probing so odd and legacy formats are
// detected, and reads stream info. Returns 0 or a negative AVERROR.
int openInput(const QString& path, InputFormatPtr& out);

// Opens a decoder for the given stream with multithreading enabled.
// `configure` can adjust the context (e.g. hardware decoding) before opening.
int openDecoder(AVStream* stream, CodecContextPtr& out,
                const std::function<void(AVCodecContext*, const AVCodec*)>& configure = {},
                const AVCodec* codecOverride = nullptr);

// True for demuxers that deliver single still images (png_pipe, image2, ...).
bool isImageDemuxer(const AVInputFormat* fmt);

// Clockwise display rotation in degrees (0/90/180/270) from the display matrix.
int streamRotation(const AVStream* st);

// Media time (microseconds) of the first sample in the file.
int64_t formatStartTime(const AVFormatContext* fc);

// Encoder capability queries. An empty result means "anything / unknown".
std::vector<AVPixelFormat> encoderPixelFormats(const AVCodec* c);
std::vector<AVSampleFormat> encoderSampleFormats(const AVCodec* c);
std::vector<int> encoderSampleRates(const AVCodec* c);
std::vector<AVChannelLayout> encoderChannelLayouts(const AVCodec* c);
std::vector<AVColorRange> encoderColorRanges(const AVCodec* c);

} // namespace ff
