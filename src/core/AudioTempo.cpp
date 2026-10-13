#include "core/AudioTempo.h"

#include "core/FFmpeg.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <QString>
#include <QStringList>
#include <cstring>

AudioTempo::~AudioTempo()
{
    release();
}

void AudioTempo::release()
{
    avfilter_graph_free(&m_graph);
    m_src = m_sink = nullptr;
}

bool AudioTempo::reset(double speed, int sampleRate)
{
    release();
    m_rate = sampleRate;
    m_pts = 0;
    m_graph = avfilter_graph_alloc();
    if (!m_graph)
        return false;
    const QByteArray args =
        QStringLiteral("sample_rate=%1:sample_fmt=flt:channel_layout=stereo:time_base=1/%1").arg(sampleRate).toLatin1();
    if (avfilter_graph_create_filter(&m_src, avfilter_get_by_name("abuffer"), "in", args.constData(), nullptr, m_graph) < 0
        || avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, m_graph) < 0) {
        release();
        return false;
    }
    // atempo accepts 0.5..100 per instance; chain instances below 0.5.
    QStringList chain;
    double remaining = speed;
    while (remaining < 0.5) {
        chain << QStringLiteral("atempo=0.5");
        remaining /= 0.5;
    }
    chain << QStringLiteral("atempo=%1").arg(remaining, 0, 'f', 6);
    chain << QStringLiteral("aformat=sample_fmts=flt:channel_layouts=stereo");
    const QByteArray desc = chain.join(QLatin1Char(',')).toLatin1();

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_src;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_sink;
    int ret = avfilter_graph_parse_ptr(m_graph, desc.constData(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret >= 0)
        ret = avfilter_graph_config(m_graph, nullptr);
    if (ret < 0) {
        release();
        return false;
    }
    return true;
}

void AudioTempo::push(const float* stereo, int frames)
{
    if (!m_graph || frames <= 0)
        return;
    ff::FramePtr f = ff::makeFrame();
    f->format = AV_SAMPLE_FMT_FLT;
    f->sample_rate = m_rate;
    f->nb_samples = frames;
    const AVChannelLayout stereoLayout = AV_CHANNEL_LAYOUT_STEREO;
    av_channel_layout_copy(&f->ch_layout, &stereoLayout);
    if (av_frame_get_buffer(f.get(), 0) < 0)
        return;
    std::memcpy(f->data[0], stereo, sizeof(float) * 2 * size_t(frames));
    f->pts = m_pts;
    m_pts += frames;
    if (av_buffersrc_add_frame_flags(m_src, f.get(), 0) < 0)
        return;
}

void AudioTempo::pull(std::vector<float>& out)
{
    if (!m_graph)
        return;
    ff::FramePtr f = ff::makeFrame();
    while (av_buffersink_get_frame(m_sink, f.get()) >= 0) {
        const size_t n = size_t(f->nb_samples) * 2;
        const float* data = reinterpret_cast<const float*>(f->data[0]);
        out.insert(out.end(), data, data + n);
        av_frame_unref(f.get());
    }
}
