#include "core/FilterGraph.h"

#include "core/FFmpeg.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <cmath>
#include <cstring>

FilterGraphState::~FilterGraphState()
{
    reset();
}

void FilterGraphState::reset()
{
    avfilter_graph_free(&m_graph);
    m_src = m_sink = nullptr;
}

bool FilterGraphState::build(QSize size, const QString& spec, double fps, QString* error)
{
    reset();
    m_spec = spec;
    m_size = size;
    m_graph = avfilter_graph_alloc();
    if (!m_graph)
        return false;

    const int fpsNum = int(std::lround(fps * 1000)), fpsDen = 1000;
    const QByteArray args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=1/1000000:pixel_aspect=1/1:frame_rate=%4/%5")
                                .arg(size.width())
                                .arg(size.height())
                                .arg(int(AV_PIX_FMT_BGRA))
                                .arg(fpsNum)
                                .arg(fpsDen)
                                .toLatin1();
    int ret = avfilter_graph_create_filter(&m_src, avfilter_get_by_name("buffer"), "in", args.constData(), nullptr, m_graph);
    if (ret >= 0)
        ret = avfilter_graph_create_filter(&m_sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_graph);
    if (ret < 0) {
        m_error = QStringLiteral("filter setup failed: %1").arg(ff::errorString(ret));
        reset();
        if (error)
            *error = m_error;
        return false;
    }

    AVFilterInOut* outputs = avfilter_inout_alloc();
    AVFilterInOut* inputs = avfilter_inout_alloc();
    outputs->name = av_strdup("in");
    outputs->filter_ctx = m_src;
    outputs->pad_idx = 0;
    outputs->next = nullptr;
    inputs->name = av_strdup("out");
    inputs->filter_ctx = m_sink;
    inputs->pad_idx = 0;
    inputs->next = nullptr;
    // Always hand back BGRA so the result drops straight into the layer.
    const QByteArray desc = (spec.trimmed() + QStringLiteral(",format=bgra")).toUtf8();
    ret = avfilter_graph_parse_ptr(m_graph, desc.constData(), &inputs, &outputs, nullptr);
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    if (ret >= 0)
        ret = avfilter_graph_config(m_graph, nullptr);
    if (ret < 0) {
        m_error = QStringLiteral("invalid filter \"%1\": %2").arg(spec, ff::errorString(ret));
        reset();
        if (error)
            *error = m_error;
        return false;
    }
    m_error.clear();
    return true;
}

QImage FilterGraphState::process(const QImage& in, const QString& spec, TimeUs t, double fps, QString* error)
{
    if (spec.trimmed().isEmpty())
        return in;
    const TimeUs frameDur = TimeUs(kUsPerSecond / std::max(1.0, fps));
    const bool jumped = m_lastT >= 0 && (t <= m_lastT || t - m_lastT > 3 * frameDur);
    if (!m_graph || spec != m_spec || in.size() != m_size || jumped) {
        // Don't retry a known-bad description on every frame.
        if (!m_graph && spec == m_spec && in.size() == m_size && !m_error.isEmpty()) {
            if (error)
                *error = m_error;
            return in;
        }
        if (!build(in.size(), spec, fps, error))
            return in;
    }
    m_lastT = t;

    ff::FramePtr frame = ff::makeFrame();
    frame->format = AV_PIX_FMT_BGRA;
    frame->width = in.width();
    frame->height = in.height();
    frame->pts = t;
    if (av_frame_get_buffer(frame.get(), 0) < 0)
        return in;
    for (int y = 0; y < in.height(); ++y)
        std::memcpy(frame->data[0] + qsizetype(frame->linesize[0]) * y, in.constScanLine(y), size_t(in.width()) * 4);
    if (av_buffersrc_add_frame_flags(m_src, frame.get(), 0) < 0)
        return in;

    ff::FramePtr out = ff::makeFrame();
    if (av_buffersink_get_frame(m_sink, out.get()) < 0)
        return in; // buffering (e.g. temporal filters warming up)
    // Drain anything else so latency doesn't build up.
    ff::FramePtr extra = ff::makeFrame();
    while (av_buffersink_get_frame(m_sink, extra.get()) >= 0) {
        av_frame_unref(out.get());
        av_frame_move_ref(out.get(), extra.get());
    }

    QImage img(out->width, out->height, QImage::Format_ARGB32_Premultiplied);
    for (int y = 0; y < out->height; ++y)
        std::memcpy(img.scanLine(y), out->data[0] + qsizetype(out->linesize[0]) * y, size_t(out->width) * 4);
    if (img.size() != in.size())
        img = img.scaled(in.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return img;
}
