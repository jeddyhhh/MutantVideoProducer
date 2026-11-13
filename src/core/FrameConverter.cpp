#include "core/FrameConverter.h"

#include "core/HdrToneMap.h"
#include "core/Parallel.h"

#include <vector>

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

// The tone mapping's tables: 12-bit signal to linear, HLG's OOTF gain by luminance, linear (0..1) to the SDR signal.
struct HdrTables {
    std::vector<float> linear, gain, encode;
    explicit HdrTables(hdr::Transfer t) : linear(4096), gain(4096), encode(4096)
    {
        for (int i = 0; i < 4096; ++i) {
            linear[size_t(i)] = float(hdr::toLinear(i / 4095.0, t));
            gain[size_t(i)] = float(hdr::hlgOotfGain(i / 4095.0));
            encode[size_t(i)] = float(hdr::encode(i / 4095.0) * 255.0);
        }
    }
};

const HdrTables& hdrTables(hdr::Transfer t)
{
    static const HdrTables pq(hdr::Transfer::Pq), hlg(hdr::Transfer::Hlg);
    return t == hdr::Transfer::Pq ? pq : hlg;
}

// RGB48 (BT.2020, HDR signal) to opaque BGRA (BT.709, SDR).
void toneMapRows(const uint8_t* src, int srcStride, QImage& img, hdr::Transfer t)
{
    const HdrTables& tab = hdrTables(t);
    const int w = img.width();
    parallelFor(img.height(), [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y) {
            const auto* in = reinterpret_cast<const uint16_t*>(src + size_t(y) * size_t(srcStride));
            auto* out = reinterpret_cast<uint32_t*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                double r = tab.linear[in[x * 3] >> 4], g = tab.linear[in[x * 3 + 1] >> 4], b = tab.linear[in[x * 3 + 2] >> 4];
                if (t == hdr::Transfer::Hlg) {
                    const double k = tab.gain[size_t(std::clamp(int((0.2627 * r + 0.6780 * g + 0.0593 * b) * 4095 + 0.5), 0, 4095))];
                    r *= k, g *= k, b *= k;
                }
                const double* m = hdr::k2020To709;
                double r7 = std::max(0.0, m[0] * r + m[1] * g + m[2] * b), g7 = std::max(0.0, m[3] * r + m[4] * g + m[5] * b),
                       b7 = std::max(0.0, m[6] * r + m[7] * g + m[8] * b);
                const double peak = std::max({r7, g7, b7});
                if (peak > hdr::kKnee) {
                    const double k = hdr::toneMap(peak) / peak;
                    r7 *= k, g7 *= k, b7 *= k;
                }
                auto enc = [&](double v) { return uint32_t(tab.encode[size_t(std::clamp(int(v * 4095 + 0.5), 0, 4095))] + 0.5f); };
                out[x] = 0xff000000u | (enc(r7) << 16) | (enc(g7) << 8) | enc(b7);
            }
        }
    }, 16);
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
    const hdr::Transfer transfer = desc->flags & AV_PIX_FMT_FLAG_RGB ? hdr::Transfer::Sdr : hdr::transferOf(frame->color_trc);
    const AVPixelFormat dstFmt = transfer == hdr::Transfer::Sdr ? AV_PIX_FMT_BGRA : AV_PIX_FMT_RGB48;

    if (!m_sws || m_srcW != frame->width || m_srcH != frame->height || m_srcFmt != srcFmt
        || m_dstW != size.width() || m_dstH != size.height() || m_flags != flags || m_dstFmt != dstFmt) {
        sws_freeContext(m_sws);
        m_sws = sws_getContext(frame->width, frame->height, srcFmt, size.width(), size.height(), dstFmt,
                               flags, nullptr, nullptr, nullptr);
        if (!m_sws)
            return {};
        m_srcW = frame->width;
        m_srcH = frame->height;
        m_srcFmt = srcFmt;
        m_dstW = size.width();
        m_dstH = size.height();
        m_flags = flags;
        m_dstFmt = dstFmt;
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

    if (transfer != hdr::Transfer::Sdr) {
        QImage img(size, QImage::Format_ARGB32_Premultiplied);
        std::vector<uint8_t> rgb(size_t(size.width()) * 6 * size_t(size.height()));
        uint8_t* dst[4] = {rgb.data(), nullptr, nullptr, nullptr};
        int dstStride[4] = {size.width() * 6, 0, 0, 0};
        if (img.isNull())
            return {};
        sws_scale(m_sws, frame->data, frame->linesize, 0, frame->height, dst, dstStride);
        toneMapRows(rgb.data(), dstStride[0], img, transfer);
        return img;
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
