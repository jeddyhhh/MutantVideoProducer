#pragma once

#include "core/FFmpeg.h"
#include "core/Time.h"

#include <QString>
#include <vector>

// Decodes one audio stream to interleaved stereo float at a fixed output
// rate, addressed by sample position (at that rate) in media time.
class AudioDecoder {
public:
    AudioDecoder();
    ~AudioDecoder();
    AudioDecoder(const AudioDecoder&) = delete;
    AudioDecoder& operator=(const AudioDecoder&) = delete;

    bool open(const QString& path, int streamIndex, int outputRate, QString* error = nullptr);
    bool isOpen() const { return m_dec != nullptr; }

    // Writes `frames` stereo frames (2 floats each) starting at sample `pos`.
    // Positions outside the stream produce silence.
    void read(int64_t pos, int frames, float* out);

    // Sample position just after the last read; sequential reads continue here.
    int64_t position() const { return m_readPos; }
    const QString& path() const { return m_path; }
    int streamIndex() const { return m_streamIndex; }

private:
    bool decodeMore();
    void appendFrame(const AVFrame* frame);
    void seek(int64_t pos);
    bool reopen();

    QString m_path;
    int m_streamIndex = -1;
    int m_outRate = 48000;
    ff::InputFormatPtr m_fmt;
    ff::CodecContextPtr m_dec;
    ff::PacketPtr m_pkt;
    ff::FramePtr m_frame;
    ff::SwrPtr m_swr;
    AVStream* m_stream = nullptr;
    TimeUs m_startOffset = 0;

    // Input parameters the resampler was configured for.
    int m_swrFormat = -1;
    int m_swrRate = 0;
    AVChannelLayout m_swrLayout{};

    std::vector<float> m_buf;   // interleaved stereo
    int64_t m_bufStart = 0;     // sample position of m_buf[0]
    int64_t m_readPos = 0;
    bool m_needTimestamp = true;
    bool m_draining = false;
    bool m_eof = false;
};
