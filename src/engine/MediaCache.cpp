#include "engine/MediaCache.h"

#include "core/AudioDecoder.h"
#include "core/FrameConverter.h"
#include "core/Proxy.h"
#include "core/VideoDecoder.h"

#include <QTimer>
#include <QTransform>
#include <algorithm>
#include <cmath>
#include <list>

namespace {
constexpr size_t kMaxThumbs = 3000;
constexpr size_t kMaxQueuedThumbs = 400;
constexpr int kWaveRate = 16000;
}

MediaCache::MediaCache(QObject* parent) : QObject(parent)
{
    m_thumbThread = std::thread(&MediaCache::thumbThreadMain, this);
    m_waveThread = std::thread(&MediaCache::waveThreadMain, this);
}

MediaCache::~MediaCache()
{
    {
        std::lock_guard lock(m_mutex);
        m_quit = true;
    }
    m_thumbCv.notify_all();
    m_waveCv.notify_all();
    m_thumbThread.join();
    m_waveThread.join();
}

void MediaCache::notifyUpdated()
{
    // Called from worker threads with m_mutex held; coalesce into one
    // signal per ~60 ms on the UI thread.
    if (m_updatePending)
        return;
    m_updatePending = true;
    QMetaObject::invokeMethod(
        this,
        [this] {
            QTimer::singleShot(60, this, [this] {
                {
                    std::lock_guard lock(m_mutex);
                    m_updatePending = false;
                }
                emit updated();
            });
        },
        Qt::QueuedConnection);
}

std::shared_ptr<const Waveform> MediaCache::waveform(const std::shared_ptr<const MediaItem>& media, int stream)
{
    if (!media)
        return nullptr;
    const auto key = std::make_pair(media->id, stream);
    std::lock_guard lock(m_mutex);
    follow(*media);
    if (m_waveRequested.insert(key).second) {
        m_waveQueue.push_back({media, stream});
        m_waveCv.notify_one();
    }
    auto it = m_waves.find(key);
    return it == m_waves.end() ? nullptr : it->second;
}

void MediaCache::forget(int mediaId, int revision)
{
    std::lock_guard lock(m_mutex);
    m_revisions[mediaId] = revision;
    drop(mediaId);
}

void MediaCache::follow(const MediaItem& m)
{
    auto [it, added] = m_paths.try_emplace(m.id, m.path);
    if (added || it->second == m.path)
        return;
    it->second = m.path;
    m_revisions.erase(m.id); // (revisions count from 0 again in the other project)
    drop(m.id);
}

void MediaCache::drop(int mediaId)
{
    std::erase_if(m_thumbQueue, [&](const ThumbRequest& r) { return r.media->id == mediaId; });
    std::erase_if(m_thumbPending, [&](const ThumbKey& k) { return std::get<0>(k) == mediaId; });
    std::erase_if(m_waveQueue, [&](const WaveRequest& r) { return r.media->id == mediaId; });
    std::erase_if(m_thumbs, [&](const auto& kv) { return std::get<0>(kv.first) == mediaId; });
    std::erase_if(m_waves, [&](const auto& kv) { return kv.first.first == mediaId; });
    std::erase_if(m_waveRequested, [&](const auto& k) { return k.first == mediaId; });
}

QImage MediaCache::thumbnail(const std::shared_ptr<const MediaItem>& media, int stream, TimeUs t)
{
    if (!media)
        return {};
    if (media->info.isStill)
        t = 0;
    const ThumbKey key{media->id, stream, t};
    std::lock_guard lock(m_mutex);
    follow(*media);
    auto it = m_thumbs.find(key);
    if (it != m_thumbs.end()) {
        it->second.lastUsed = ++m_useCounter;
        return it->second.image;
    }
    if (m_thumbPending.insert(key).second) {
        m_thumbQueue.push_back({media, stream, t});
        if (m_thumbQueue.size() > kMaxQueuedThumbs) {
            m_thumbPending.erase(ThumbKey{m_thumbQueue.front().media->id, m_thumbQueue.front().stream,
                                          m_thumbQueue.front().t});
            m_thumbQueue.erase(m_thumbQueue.begin());
        }
        m_thumbCv.notify_one();
    }
    // Meanwhile show the nearest thumbnail of the same stream.
    auto lo = m_thumbs.lower_bound(key);
    const QImage* best = nullptr;
    TimeUs bestDist = 0;
    auto consider = [&](std::map<ThumbKey, Thumb>::iterator i) {
        if (i == m_thumbs.end() || std::get<0>(i->first) != media->id || std::get<1>(i->first) != stream)
            return;
        const TimeUs d = std::llabs(std::get<2>(i->first) - t);
        if (!best || d < bestDist) {
            best = &i->second.image;
            bestDist = d;
        }
    };
    consider(lo);
    if (lo != m_thumbs.begin())
        consider(std::prev(lo));
    return best ? *best : QImage();
}

QImage MediaCache::decodeThumbnail(VideoDecoder& decoder, FrameConverter& converter, const VideoStreamInfo& vs, TimeUs t)
{
    const AVFrame* f = decoder.frameAt(t, false);
    if (!f)
        return {};
    const QSize disp = vs.displaySize();
    const int h = kThumbHeight;
    const int w = std::max(8, int(std::lround(double(h) * disp.width() / std::max(1, disp.height()))));
    const bool sideways = vs.rotation == 90 || vs.rotation == 270;
    QImage img = converter.convert(f, sideways ? QSize(h, w) : QSize(w, h), false);
    if (!img.isNull() && vs.rotation)
        img = img.transformed(QTransform().rotate(vs.rotation));
    return img;
}

void MediaCache::thumbThreadMain()
{
    struct Dec {
        QString path; // (not the media id: ids repeat across projects)
        int revision = 0;
        int stream = -1;
        std::unique_ptr<VideoDecoder> decoder;
        FrameConverter converter;
    };
    std::list<Dec> decoders; // most recently used first

    std::unique_lock lock(m_mutex);
    for (;;) {
        m_thumbCv.wait(lock, [&] { return m_quit || !m_thumbQueue.empty(); });
        if (m_quit)
            return;
        ThumbRequest req = std::move(m_thumbQueue.back());
        m_thumbQueue.pop_back();
        const ThumbKey key{req.media->id, req.stream, req.t};
        lock.unlock();

        auto it = std::find_if(decoders.begin(), decoders.end(),
                               [&](const Dec& d) { return d.path == req.media->path && d.revision == req.media->revision && d.stream == req.stream; });
        if (it == decoders.end()) {
            Dec& d = decoders.emplace_front();
            d.path = req.media->path;
            d.revision = req.media->revision;
            d.stream = req.stream;
            d.decoder = std::make_unique<VideoDecoder>();
            // Thumbnails from the proxy when there is one: much quicker for heavy files.
            const QString path = proxy::playbackPath(req.media->path, req.stream);
            if (!d.decoder->open(path, path == req.media->path ? req.stream : 0, nullptr, false))
                d.decoder.reset();
            it = decoders.begin();
            if (decoders.size() > 6)
                decoders.pop_back();
        } else if (it != decoders.begin()) {
            decoders.splice(decoders.begin(), decoders, it);
            it = decoders.begin();
        }

        QImage img;
        const VideoStreamInfo* vs = req.media->info.videoStream(req.stream);
        if (it->decoder && vs)
            img = decodeThumbnail(*it->decoder, it->converter, *vs, req.t);
        if (img.isNull()) {
            img = QImage(16, 9, QImage::Format_ARGB32_Premultiplied);
            img.fill(Qt::black);
        }

        lock.lock();
        if (stale(*req.media))
            continue;
        m_thumbPending.erase(key);
        m_thumbs[key] = Thumb{img, ++m_useCounter};
        if (m_thumbs.size() > kMaxThumbs) {
            // Evict the least recently used quarter.
            std::vector<std::pair<uint64_t, ThumbKey>> order;
            order.reserve(m_thumbs.size());
            for (const auto& [k, v] : m_thumbs)
                order.emplace_back(v.lastUsed, k);
            std::nth_element(order.begin(), order.begin() + order.size() / 4, order.end());
            for (size_t i = 0; i < order.size() / 4; ++i)
                m_thumbs.erase(order[i].second);
        }
        notifyUpdated();
    }
}

void MediaCache::waveThreadMain()
{
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_waveCv.wait(lock, [&] { return m_quit || !m_waveQueue.empty(); });
        if (m_quit)
            return;
        WaveRequest req = std::move(m_waveQueue.front());
        m_waveQueue.erase(m_waveQueue.begin());
        lock.unlock();

        AudioDecoder dec;
        auto wave = std::make_shared<Waveform>();
        const auto key = std::make_pair(req.media->id, req.stream);
        if (dec.open(req.media->path, req.stream, kWaveRate)) {
            const int perBucket = kWaveRate / wave->bucketsPerSecond;
            const int64_t total = usToSamples(req.media->info.duration, kWaveRate);
            std::vector<float> buf(size_t(kWaveRate) * 2);
            int64_t pos = 0;
            int64_t lastPublish = 0;
            bool quit = false;
            while (pos < total && !quit) {
                const int n = int(std::min<int64_t>(kWaveRate, total - pos));
                dec.read(pos, n, buf.data());
                for (int b = 0; b * perBucket < n; ++b) {
                    float lo = 0, hi = 0;
                    const int end = std::min(n, (b + 1) * perBucket);
                    for (int i = b * perBucket; i < end; ++i) {
                        lo = std::min({lo, buf[size_t(i) * 2], buf[size_t(i) * 2 + 1]});
                        hi = std::max({hi, buf[size_t(i) * 2], buf[size_t(i) * 2 + 1]});
                    }
                    wave->peaks.push_back(int8_t(std::clamp(lo, -1.0f, 1.0f) * 127));
                    wave->peaks.push_back(int8_t(std::clamp(hi, -1.0f, 1.0f) * 127));
                }
                pos += n;
                // Publish progress every ~30 s of audio so long files fill in.
                if (pos - lastPublish >= int64_t(kWaveRate) * 30 || pos >= total) {
                    lastPublish = pos;
                    auto copy = std::make_shared<Waveform>(*wave);
                    copy->complete = pos >= total;
                    std::lock_guard g(m_mutex);
                    quit = m_quit || stale(*req.media);
                    if (!stale(*req.media))
                        m_waves[key] = std::move(copy);
                    notifyUpdated();
                }
            }
        } else {
            wave->complete = true;
            std::lock_guard g(m_mutex);
            if (!stale(*req.media))
                m_waves[key] = wave;
        }
        lock.lock();
    }
}
