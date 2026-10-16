#pragma once

#include "core/Project.h"

#include <QImage>
#include <QObject>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <vector>

// Audio peaks for drawing waveforms: min/max pairs per bucket, scaled to
// -127..127, mono (max over channels).
struct Waveform {
    int bucketsPerSecond = 100;
    std::vector<int8_t> peaks; // 2 per bucket: min, max
    bool complete = false;
    size_t buckets() const { return peaks.size() / 2; }
};

// Background-generated timeline thumbnails and waveforms.
class MediaCache : public QObject {
    Q_OBJECT
public:
    static constexpr int kThumbHeight = 72;

    explicit MediaCache(QObject* parent = nullptr);
    ~MediaCache() override;

    // Waveform for an audio stream; null until the first part is ready.
    std::shared_ptr<const Waveform> waveform(const std::shared_ptr<const MediaItem>& media, int stream);

    // Best thumbnail available near source time t (may be null); queues the
    // exact one. Pass already-quantised times to get good cache reuse.
    QImage thumbnail(const std::shared_ptr<const MediaItem>& media, int stream, TimeUs t);

    // A frame as a thumbnail kThumbHeight high, turned upright; null if it can't be decoded.
    static QImage decodeThumbnail(class VideoDecoder& decoder, class FrameConverter& converter, const VideoStreamInfo& stream, TimeUs t);

    // Drops what was made for a media item (it was relinked to another file,
    // now at `revision`); work still running for older revisions is discarded.
    void forget(int mediaId, int revision);

signals:
    void updated();

private:
    using ThumbKey = std::tuple<int, int, TimeUs>; // media, stream, time
    struct ThumbRequest {
        std::shared_ptr<const MediaItem> media;
        int stream;
        TimeUs t;
    };
    struct WaveRequest {
        std::shared_ptr<const MediaItem> media;
        int stream;
    };

    void thumbThreadMain();
    void waveThreadMain();
    void notifyUpdated();

    std::mutex m_mutex;
    std::condition_variable m_thumbCv;
    std::condition_variable m_waveCv;
    bool m_quit = false;
    bool m_updatePending = false;

    struct Thumb {
        QImage image;
        uint64_t lastUsed = 0;
    };
    std::map<ThumbKey, Thumb> m_thumbs;
    std::vector<ThumbRequest> m_thumbQueue; // LIFO: newest (visible) first
    std::set<ThumbKey> m_thumbPending;
    uint64_t m_useCounter = 0;

    std::map<int, int> m_revisions; // media id -> oldest revision still wanted
    // Media id -> the file its pictures are of: ids start again at 1 in every
    // project, so the same id can be another file after one opens.
    std::map<int, QString> m_paths;
    bool stale(const MediaItem& m) const
    {
        auto it = m_revisions.find(m.id);
        auto p = m_paths.find(m.id);
        return (it != m_revisions.end() && m.revision < it->second) || (p != m_paths.end() && p->second != m.path);
    }
    // (m_mutex held) Notes the file behind the media's id; another file drops what was made for the id.
    void follow(const MediaItem& m);
    void drop(int mediaId); // (m_mutex held)

    std::map<std::pair<int, int>, std::shared_ptr<const Waveform>> m_waves;
    std::vector<WaveRequest> m_waveQueue;
    std::set<std::pair<int, int>> m_waveRequested;

    std::thread m_thumbThread;
    std::thread m_waveThread;
};
