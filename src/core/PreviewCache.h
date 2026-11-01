#pragma once

// Pre-rendering for the preview: the timeline cut into stretches at every event's edges, each with a key made from
// everything that changes its pictures (renderKeyData, the project's settings, the files, the preview's size), so an
// edit in a stretch makes it a new one and its old frames are simply not found again. Heavy stretches' frames are kept
// as JPEG files in the cache folder (pre-rendered in the background), and the preview plays them from there.

#include "core/Project.h"

#include <QImage>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace precache {

struct Stretch {
    TimeUs start = 0, end = 0;
    QByteArray key; // hex
};
// The stretches of the project for a preview of `size` (proxies: whether the preview plays them).
std::vector<Stretch> stretches(const ProjectSnapshot& snap, QSize size, bool proxies);
// The stretch at t, or null.
const Stretch* stretchAt(const std::vector<Stretch>& list, TimeUs t);

// The frames on disk, by stretch key and frame number. Thread-safe.
class Store {
public:
    explicit Store(QString dir = {}); // default: the app's cache folder, "prerender"
    bool has(const QByteArray& key, int64_t frame);
    QImage load(const QByteArray& key, int64_t frame);
    void save(const QByteArray& key, int64_t frame, const QImage& image);
    // Oldest stretches deleted until the folder is under `bytes`.
    void trim(qint64 bytes);
    // Every frame thrown away.
    void clear();
    QString dir() const { return m_dir; }

private:
    QString path(const QByteArray& key, int64_t frame) const;
    const std::set<int64_t>& framesOf(const QByteArray& key); // (m_mutex held)
    std::mutex m_mutex;
    QString m_dir;
    std::map<QByteArray, std::set<int64_t>> m_frames; // the stretches looked at so far
};

} // namespace precache
