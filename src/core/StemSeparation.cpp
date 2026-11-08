#include "core/StemSeparation.h"

#include "core/AiRuntime.h"
#include "core/AudioDecoder.h"
#include "core/TimelineOps.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QSaveFile>

namespace stems {

QString workerPath()
{
    return aiRuntimeWorkerPath(QStringLiteral("stems"), QString());
}

QStringList workerArguments(const QString& program)
{
    return aiRuntimeWorkerArguments(QStringLiteral("stems"), program);
}

bool writeEventWav(const ProjectSnapshot& snap, const Clip& clip, const QString& path, QString* why)
{
    auto fail = [&](const QString& text) {
        if (why)
            *why = text;
        return false;
    };
    const auto it = snap.media.find(clip.mediaId);
    const MediaItem* media = it != snap.media.end() ? it->second.get() : nullptr;
    if (clip.isGenerated() || clip.isSequence() || !media || !media->info.audioStream(clip.streamIndex))
        return fail(QStringLiteral("Choose a sound event from a file."));
    if (clip.speed != 1.0 || clip.reverse || clip.hasRamp() || clip.loops())
        return fail(QStringLiteral("Put the event's speed back to 100% first (or render it to a file and separate that)."));
    constexpr int rate = 44100;
    AudioDecoder dec;
    QString error;
    if (!dec.open(media->path, media->info.audioStream(clip.streamIndex)->index, rate, &error))
        return fail(error);
    const int64_t first = clip.sourceIn * rate / kUsPerSecond, frames = clip.length * rate / kUsPerSecond;
    if (frames <= 0)
        return fail(QStringLiteral("The event is empty."));
    if (frames * 8 > 0xFFFFFFFFll - 36) // (a WAV file's sizes are 32-bit: about 3 hours 20 minutes of this)
        return fail(QStringLiteral("The event is too long to separate at once: split it into parts of under three hours."));
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return fail(QStringLiteral("Can't write %1").arg(path));
    QDataStream st(&f);
    st.setByteOrder(QDataStream::LittleEndian);
    st.setFloatingPointPrecision(QDataStream::SinglePrecision);
    const quint32 bytes = quint32(frames * 8);
    st.writeRawData("RIFF", 4);
    st << quint32(36 + bytes);
    st.writeRawData("WAVEfmt ", 8);
    st << quint32(16) << quint16(3) << quint16(2) << quint32(rate) << quint32(rate * 8) << quint16(8) << quint16(32);
    st.writeRawData("data", 4);
    st << bytes;
    std::vector<float> buf(size_t(rate) * 2);
    for (int64_t done = 0; done < frames;) {
        const int n = int(std::min<int64_t>(rate, frames - done));
        dec.read(first + done, n, buf.data());
        for (int i = 0; i < n * 2; ++i)
            st << buf[size_t(i)];
        done += n;
    }
    return f.commit() || fail(QStringLiteral("Can't write %1").arg(path));
}

std::vector<int> place(Timeline& tl, const MediaMap& media, int clipId, const std::vector<int>& mediaIds, const QStringList& names, QString* why)
{
    int ti = -1;
    Clip* original = tl.findClip(clipId, &ti);
    if (!original || ti < 0 || mediaIds.empty()) {
        if (why)
            *why = QStringLiteral("The event isn't on the timeline any more.");
        return {};
    }
    const Clip source = *original;
    const QString base = tl.tracks[size_t(ti)].name;
    const int group = tl.allocId();
    std::vector<int> made;
    for (size_t k = 0; k < mediaIds.size(); ++k) {
        const int index = ops::addTrack(tl, TrackType::Audio, ti + 1 + int(k));
        Track& track = tl.tracks[size_t(index)];
        track.name = QStringLiteral("%1 - %2").arg(base, k < size_t(names.size()) ? names[int(k)] : QString::number(k + 1));
        Clip c;
        c.id = tl.allocId();
        c.mediaId = mediaIds[k];
        const auto m = media.find(mediaIds[k]);
        c.streamIndex = m != media.end() && m->second && m->second->info.hasAudio() ? m->second->info.audio.front().index : 0;
        c.start = source.start;
        c.length = source.length;
        c.gain = source.gain;
        c.fadeIn = source.fadeIn;
        c.fadeOut = source.fadeOut;
        c.name = track.name;
        c.groupId = group;
        track.clips.push_back(c);
        made.push_back(c.id);
    }
    if (Clip* again = tl.findClip(clipId))
        again->muted = true;
    return made;
}

} // namespace stems
