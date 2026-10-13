#include "core/FormatRegistry.h"

#include "core/FFmpeg.h"

#include <QSet>
#include <algorithm>
#include <cstring>

namespace formats {

namespace {

// Muxers that don't produce a normal media file (network, test/hash output).
bool isExcludedMuxer(const char* name)
{
    static const char* const excluded[] = {"null", "rtp", "rtp_mpegts", "rtsp", "sap", "tee", "fifo", "fifo_test",
                                           "whip", "uncodedframecrc", "framecrc", "framemd5", "crc", "md5",
                                           "streamhash", "hash", "framehash", "chromaprint"};
    for (const char* e : excluded)
        if (std::strcmp(name, e) == 0)
            return true;
    return false;
}

QString encoderNameFor(AVCodecID id)
{
    if (id == AV_CODEC_ID_NONE)
        return {};
    const AVCodec* c = avcodec_find_encoder(id);
    return c ? QString::fromUtf8(c->name) : QString();
}

} // namespace

const std::vector<MuxerInfo>& muxers()
{
    static const std::vector<MuxerInfo> list = [] {
        std::vector<MuxerInfo> out;
        void* it = nullptr;
        while (const AVOutputFormat* of = av_muxer_iterate(&it)) {
            if (isExcludedMuxer(of->name))
                continue;
            if (of->video_codec == AV_CODEC_ID_NONE && of->audio_codec == AV_CODEC_ID_NONE)
                continue;
            MuxerInfo m;
            m.name = QString::fromUtf8(of->name);
            m.longName = QString::fromUtf8(of->long_name ? of->long_name : of->name);
            if (of->extensions)
                m.extensions = QString::fromUtf8(of->extensions).split(QLatin1Char(','), Qt::SkipEmptyParts);
            m.defaultVideoEncoder = encoderNameFor(of->video_codec);
            m.defaultAudioEncoder = encoderNameFor(of->audio_codec);
            out.push_back(std::move(m));
        }
        std::sort(out.begin(), out.end(), [](const MuxerInfo& a, const MuxerInfo& b) {
            return a.longName.compare(b.longName, Qt::CaseInsensitive) < 0;
        });
        return out;
    }();
    return list;
}

const MuxerInfo* findMuxer(const QString& name)
{
    for (const auto& m : muxers())
        if (m.name == name)
            return &m;
    return nullptr;
}

std::vector<EncoderInfo> encodersFor(const QString& muxer, bool video)
{
    std::vector<EncoderInfo> out;
    const QByteArray name = muxer.toUtf8();
    const AVOutputFormat* of = av_guess_format(name.constData(), nullptr, nullptr);
    if (!of)
        return out;
    const AVMediaType type = video ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO;
    const AVCodecID defaultId = video ? of->video_codec : of->audio_codec;

    void* it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it)) {
        if (!av_codec_is_encoder(c) || c->type != type)
            continue;
        if (avformat_query_codec(of, c->id, FF_COMPLIANCE_EXPERIMENTAL) == 0)
            continue;
        EncoderInfo e;
        e.name = QString::fromUtf8(c->name);
        e.longName = QString::fromUtf8(c->long_name ? c->long_name : c->name);
        e.experimental = c->capabilities & AV_CODEC_CAP_EXPERIMENTAL;
        e.isDefault = c->id == defaultId && avcodec_find_encoder(defaultId) == c;
        out.push_back(std::move(e));
    }
    std::sort(out.begin(), out.end(), [](const EncoderInfo& a, const EncoderInfo& b) {
        if (a.isDefault != b.isDefault)
            return a.isDefault;
        return a.name < b.name;
    });
    return out;
}

QString importFileFilter()
{
    QSet<QString> exts;
    void* it = nullptr;
    while (const AVInputFormat* f = av_demuxer_iterate(&it)) {
        if (!f->extensions)
            continue;
        for (const QString& e : QString::fromUtf8(f->extensions).split(QLatin1Char(','), Qt::SkipEmptyParts))
            exts.insert(QStringLiteral("*.") + e.trimmed());
    }
    QStringList sorted(exts.begin(), exts.end());
    sorted.sort();
    return QStringLiteral("All files (*);;Known media files (%1)").arg(sorted.join(QLatin1Char(' ')));
}

Overview overview()
{
    Overview o;
    void* it = nullptr;
    while (const AVInputFormat* f = av_demuxer_iterate(&it))
        o.demuxers << QStringLiteral("%1 - %2").arg(QString::fromUtf8(f->name),
                                                    QString::fromUtf8(f->long_name ? f->long_name : ""));
    it = nullptr;
    while (const AVCodec* c = av_codec_iterate(&it)) {
        const QString line = QStringLiteral("%1 - %2").arg(QString::fromUtf8(c->name),
                                                           QString::fromUtf8(c->long_name ? c->long_name : ""));
        const bool enc = av_codec_is_encoder(c);
        if (c->type == AVMEDIA_TYPE_VIDEO)
            (enc ? o.videoEncoders : o.videoDecoders) << line;
        else if (c->type == AVMEDIA_TYPE_AUDIO)
            (enc ? o.audioEncoders : o.audioDecoders) << line;
    }
    for (QStringList* l : {&o.demuxers, &o.videoDecoders, &o.audioDecoders, &o.videoEncoders, &o.audioEncoders})
        l->sort(Qt::CaseInsensitive);
    return o;
}

} // namespace formats
