#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// Queries what the linked FFmpeg build can read and write.
namespace formats {

struct MuxerInfo {
    QString name;        // e.g. "mp4"
    QString longName;    // e.g. "MP4 (MPEG-4 Part 14)"
    QStringList extensions;
    QString defaultVideoEncoder;
    QString defaultAudioEncoder;
};

struct EncoderInfo {
    QString name;        // e.g. "libx264"
    QString longName;
    bool isDefault = false;
    bool experimental = false;
};

// Every file-producing muxer, sorted by long name.
const std::vector<MuxerInfo>& muxers();
const MuxerInfo* findMuxer(const QString& name);

// Encoders whose output the muxer accepts; the muxer's default comes first.
std::vector<EncoderInfo> encodersFor(const QString& muxer, bool video);

// File-dialog filter covering every extension a demuxer advertises.
QString importFileFilter();

// Counts for the "supported formats" overview.
struct Overview {
    QStringList demuxers;      // "name - long name"
    QStringList videoDecoders;
    QStringList audioDecoders;
    QStringList videoEncoders;
    QStringList audioEncoders;
};
Overview overview();

} // namespace formats
