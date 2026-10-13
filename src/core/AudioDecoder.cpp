#include "core/AudioDecoder.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr int kMaxReadErrors = 64;
}

AudioDecoder::AudioDecoder() = default;

AudioDecoder::~AudioDecoder()
{
    av_channel_layout_uninit(&m_swrLayout);
}

bool AudioDecoder::open(const QString& path, int streamIndex, int outputRate, QString* error)
{
    m_dec.reset();
    m_fmt.reset();
    m_swr.reset();
    m_path = path;
    m_streamIndex = streamIndex;
    m_outRate = outputRate;

    auto fail = [&](const QString& msg) {
        if (error)
            *error = msg;
        m_dec.reset();
        m_fmt.reset();
        return false;
    };

    int ret = ff::openInput(path, m_fmt);
    if (ret < 0)
        return fail(QStringLiteral("open failed: %1").arg(ff::errorString(ret)));
    AVFormatContext* fc = m_fmt.get();
    if (streamIndex < 0 || unsigned(streamIndex) >= fc->nb_streams
        || fc->streams[streamIndex]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
        return fail(QStringLiteral("stream %1 is not an audio stream").arg(streamIndex));
    m_stream = fc->streams[streamIndex];
    for (unsigned i = 0; i < fc->nb_streams; ++i)
        if (int(i) != streamIndex)
            fc->streams[i]->discard = AVDISCARD_ALL;

    ret = ff::openDecoder(m_stream, m_dec);
    if (ret < 0)
        return fail(QStringLiteral("decoder: %1").arg(ff::errorString(ret)));

    m_pkt = ff::makePacket();
    m_frame = ff::makeFrame();
    m_startOffset = ff::formatStartTime(fc);
    m_buf.clear();
    m_bufStart = 0;
    m_readPos = 0;
    m_needTimestamp = true;
    m_draining = m_eof = false;
    m_swrFormat = -1;
    return true;
}

bool AudioDecoder::reopen()
{
    const QString path = m_path;
    return open(path, m_streamIndex, m_outRate);
}

void AudioDecoder::seek(int64_t pos)
{
    m_buf.clear();
    m_bufStart = pos;
    m_needTimestamp = true;
    m_draining = m_eof = false;
    m_swr.reset();
    m_swrFormat = -1;

    const TimeUs t = samplesToUs(pos, m_outRate) + m_startOffset;
    int64_t ts = av_rescale_q(t, AV_TIME_BASE_Q, m_stream->time_base);
    if (m_stream->start_time != AV_NOPTS_VALUE)
        ts = std::max(ts, m_stream->start_time);
    int ret = av_seek_frame(m_fmt.get(), m_streamIndex, ts, AVSEEK_FLAG_BACKWARD);
    if (ret < 0)
        ret = avformat_seek_file(m_fmt.get(), -1, INT64_MIN, t, t, 0);
    if (ret < 0) {
        // Unseekable: decode from the start; read() discards what precedes pos.
        if (reopen()) {
            m_bufStart = 0;
            m_needTimestamp = true;
        }
        return;
    }
    avcodec_flush_buffers(m_dec.get());
}

void AudioDecoder::appendFrame(const AVFrame* frame)
{
    AVChannelLayout inLayout{};
    if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || frame->ch_layout.nb_channels == 0)
        av_channel_layout_default(&inLayout, std::max(1, frame->ch_layout.nb_channels));
    else
        av_channel_layout_copy(&inLayout, &frame->ch_layout);

    if (!m_swr || m_swrFormat != frame->format || m_swrRate != frame->sample_rate
        || av_channel_layout_compare(&m_swrLayout, &inLayout) != 0) {
        SwrContext* swr = nullptr;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, m_outRate, &inLayout,
                                static_cast<AVSampleFormat>(frame->format), frame->sample_rate, 0, nullptr) < 0
            || swr_init(swr) < 0) {
            swr_free(&swr);
            av_channel_layout_uninit(&inLayout);
            m_swr.reset();
            return;
        }
        m_swr.reset(swr);
        m_swrFormat = frame->format;
        m_swrRate = frame->sample_rate;
        av_channel_layout_uninit(&m_swrLayout);
        av_channel_layout_copy(&m_swrLayout, &inLayout);
    }
    av_channel_layout_uninit(&inLayout);

    const int maxOut = swr_get_out_samples(m_swr.get(), frame->nb_samples);
    if (maxOut <= 0)
        return;
    const size_t oldSize = m_buf.size();
    m_buf.resize(oldSize + size_t(maxOut) * 2);
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(m_buf.data() + oldSize);
    const int got = swr_convert(m_swr.get(), &outPtr, maxOut,
                                const_cast<const uint8_t**>(frame->extended_data), frame->nb_samples);
    m_buf.resize(oldSize + size_t(std::max(got, 0)) * 2);
}

bool AudioDecoder::decodeMore()
{
    int readErrors = 0;
    for (;;) {
        int ret = avcodec_receive_frame(m_dec.get(), m_frame.get());
        if (ret == 0) {
            if (m_needTimestamp) {
                const int64_t pts = m_frame->best_effort_timestamp;
                if (pts != AV_NOPTS_VALUE) {
                    const TimeUs t = av_rescale_q(pts, m_stream->time_base, AV_TIME_BASE_Q) - m_startOffset;
                    m_bufStart = usToSamples(t, m_outRate);
                }
                m_needTimestamp = false;
            }
            appendFrame(m_frame.get());
            return true;
        }
        if (ret == AVERROR_EOF || m_draining) {
            m_eof = true;
            return false;
        }
        if (ret != AVERROR(EAGAIN) && ++readErrors > kMaxReadErrors)
            return false;

        ret = av_read_frame(m_fmt.get(), m_pkt.get());
        if (ret < 0) {
            const bool atEnd = ret == AVERROR_EOF || (m_fmt->pb && avio_feof(m_fmt->pb));
            if (!atEnd && ++readErrors <= kMaxReadErrors)
                continue;
            avcodec_send_packet(m_dec.get(), nullptr);
            m_draining = true;
            continue;
        }
        if (m_pkt->stream_index == m_streamIndex)
            avcodec_send_packet(m_dec.get(), m_pkt.get());
        av_packet_unref(m_pkt.get());
    }
}

void AudioDecoder::read(int64_t pos, int frames, float* out)
{
    std::memset(out, 0, sizeof(float) * 2 * size_t(frames));
    if (!m_dec || frames <= 0)
        return;

    const int64_t bufEnd = m_bufStart + int64_t(m_buf.size() / 2);
    // Seek when going backwards or jumping > 1 s ahead (but not past the end).
    if (pos < m_bufStart || (pos > bufEnd + m_outRate && !m_eof))
        seek(pos);
    if (!m_dec)
        return;

    while (!m_eof && m_bufStart + int64_t(m_buf.size() / 2) < pos + frames) {
        if (!decodeMore())
            break;
        // Drop data that is already behind the read position to keep the
        // buffer small while decoding through a gap.
        const int64_t behind = pos - m_bufStart;
        if (behind > int64_t(m_outRate) && int64_t(m_buf.size() / 2) > behind) {
            m_buf.erase(m_buf.begin(), m_buf.begin() + behind * 2);
            m_bufStart = pos;
        }
    }

    const int64_t avail0 = std::max(pos, m_bufStart);
    const int64_t avail1 = std::min(pos + frames, m_bufStart + int64_t(m_buf.size() / 2));
    if (avail1 > avail0)
        std::memcpy(out + (avail0 - pos) * 2, m_buf.data() + (avail0 - m_bufStart) * 2,
                    sizeof(float) * 2 * size_t(avail1 - avail0));

    // Discard consumed samples.
    const int64_t consumed = std::min<int64_t>(pos + frames - m_bufStart, int64_t(m_buf.size() / 2));
    if (consumed > 0) {
        m_buf.erase(m_buf.begin(), m_buf.begin() + consumed * 2);
        m_bufStart += consumed;
    }
    m_readPos = pos + frames;
}
