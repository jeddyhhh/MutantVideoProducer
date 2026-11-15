#include "core/Deinterlacer.h"

extern "C" {
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <QString>

Deinterlacer::~Deinterlacer()
{
    reset();
}

void Deinterlacer::reset()
{
    avfilter_graph_free(&m_graph);
    m_in = m_out = nullptr;
    m_flushed = false;
}

bool Deinterlacer::start(const AVFrame* f)
{
    reset();
    m_graph = avfilter_graph_alloc();
    const AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{1, 1};
    // (the times aren't used: the caller keeps its own)
    const QByteArray args = QStringLiteral("video_size=%1x%2:pix_fmt=%3:time_base=1/1000000:pixel_aspect=%4/%5")
                                .arg(f->width).arg(f->height).arg(f->format).arg(sar.num).arg(sar.den)
                                .toLatin1();
    const QByteArray colour = QStringLiteral(":colorspace=%1:range=%2").arg(int(f->colorspace)).arg(int(f->color_range)).toLatin1();
    const AVFilter* buffer = avfilter_get_by_name("buffer");
    AVFilterContext* bwdif = nullptr;
    // Older FFmpeg (Ubuntu 24.04's 6.1) has no colorspace / range options on the buffer source: made without them there
    const bool ok = m_graph
                    && (avfilter_graph_create_filter(&m_in, buffer, "in", (args + colour).constData(), nullptr, m_graph) >= 0
                        || avfilter_graph_create_filter(&m_in, buffer, "in", args.constData(), nullptr, m_graph) >= 0)
                    && avfilter_graph_create_filter(&bwdif, avfilter_get_by_name("bwdif"), "deint",
                                                    "mode=send_frame:parity=auto:deint=interlaced", nullptr, m_graph) >= 0
                    && avfilter_graph_create_filter(&m_out, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, m_graph) >= 0
                    && avfilter_link(m_in, 0, bwdif, 0) >= 0 && avfilter_link(bwdif, 0, m_out, 0) >= 0
                    && avfilter_graph_config(m_graph, nullptr) >= 0;
    if (!ok) {
        reset();
        m_failed = true;
        return false;
    }
    m_w = f->width;
    m_h = f->height;
    m_fmt = f->format;
    return true;
}

bool Deinterlacer::push(const AVFrame* f)
{
    if (m_failed)
        return false;
    if (m_graph && (m_flushed || f->width != m_w || f->height != m_h || f->format != m_fmt))
        reset();
    if (!m_graph && !start(f))
        return false;
    m_trc = f->color_trc;
    m_space = f->colorspace;
    m_primaries = f->color_primaries;
    m_range = f->color_range;
    return av_buffersrc_add_frame_flags(m_in, const_cast<AVFrame*>(f), AV_BUFFERSRC_FLAG_KEEP_REF) >= 0;
}

bool Deinterlacer::pull(AVFrame* out)
{
    if (!m_graph)
        return false;
    av_frame_unref(out); // (the sink moves its frame in without letting go of what was there)
    if (av_buffersink_get_frame(m_out, out) < 0)
        return false;
    // (a format conversion on the way may drop them)
    out->color_trc = m_trc;
    out->colorspace = m_space;
    out->color_primaries = m_primaries;
    out->color_range = m_range;
    return true;
}

void Deinterlacer::flush()
{
    if (m_graph && !m_flushed) {
        av_buffersrc_add_frame(m_in, nullptr);
        m_flushed = true;
    }
}
