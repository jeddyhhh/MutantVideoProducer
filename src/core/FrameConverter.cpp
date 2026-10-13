#include "core/FrameConverter.h"

FrameConverter::~FrameConverter()
{
    sws_freeContext(m_sws);
}

namespace {

bool isFullRangeFormat(AVPixelFormat f)
{
    switch (f) {
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
    case AV_PIX_FMT_YUVJ440P:
    case AV_PIX_FMT_YUVJ411P:
        return true;
    default:
        return false;
    }
}

} // namespace

QImage FrameConverter::convert(const AVFrame* frame, QSize size, bool highQuality)
{
    if (!frame || size.width() < 1 || size.height() < 1 || frame->width < 1 || frame->height < 1)
        return {};
    const auto srcFmt = static_cast<AVPixelFormat>(frame->format);
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(srcFmt);
    if (!desc)
        return {};
    const bool hasAlpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;
    const int flags = highQuality ? (SWS_BICUBIC | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INT) : SWS_BILINEAR;

    if (!m_sws || m_srcW != frame->width || m_srcH != frame->height || m_srcFmt != srcFmt
        || m_dstW != size.width() || m_dstH != size.height() || m_flags != flags) {
        sws_freeContext(m_sws);
        m_sws = sws_getContext(frame->width, frame->height, srcFmt, size.width(), size.height(), AV_PIX_FMT_BGRA,
                               flags, nullptr, nullptr, nullptr);
        if (!m_sws)
            return {};
        m_srcW = frame->width;
        m_srcH = frame->height;
        m_srcFmt = srcFmt;
        m_dstW = size.width();
        m_dstH = size.height();
        m_flags = flags;
        m_colorspace = m_range = -1;
    }

    if (!(desc->flags & AV_PIX_FMT_FLAG_RGB)) {
        int cs = frame->colorspace;
        if (cs == AVCOL_SPC_UNSPECIFIED || cs == AVCOL_SPC_RGB || cs == AVCOL_SPC_RESERVED)
            cs = frame->height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
        const int range = (frame->color_range == AVCOL_RANGE_JPEG || isFullRangeFormat(srcFmt)) ? 1 : 0;
        if (cs != m_colorspace || range != m_range) {
            sws_setColorspaceDetails(m_sws, sws_getCoefficients(cs), range, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                     1 << 16, 1 << 16);
            m_colorspace = cs;
            m_range = range;
        }
    }

    QImage img(size, hasAlpha ? QImage::Format_ARGB32 : QImage::Format_ARGB32_Premultiplied);
    if (img.isNull())
        return {};
    uint8_t* dst[4] = {img.bits(), nullptr, nullptr, nullptr};
    int dstStride[4] = {int(img.bytesPerLine()), 0, 0, 0};
    sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
    if (hasAlpha)
        img.convertTo(QImage::Format_ARGB32_Premultiplied);
    return img;
}
