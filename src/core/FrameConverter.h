#pragma once

#include "core/FFmpeg.h"

#include <QImage>
#include <QSize>

// Scales and converts decoded frames to premultiplied ARGB32 QImages,
// honouring the source colour matrix (BT.601/709/2020) and range.
class FrameConverter {
public:
    FrameConverter() = default;
    ~FrameConverter();
    FrameConverter(const FrameConverter&) = delete;
    FrameConverter& operator=(const FrameConverter&) = delete;

    QImage convert(const AVFrame* frame, QSize size, bool highQuality);

private:
    SwsContext* m_sws = nullptr;
    int m_srcW = 0, m_srcH = 0, m_srcFmt = -1;
    int m_dstW = 0, m_dstH = 0, m_flags = 0;
    int m_colorspace = -1, m_range = -1;
};
