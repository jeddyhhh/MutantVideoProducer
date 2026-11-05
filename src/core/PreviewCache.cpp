#include "core/PreviewCache.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QStandardPaths>
#include <algorithm>

namespace precache {

std::vector<Stretch> stretches(const ProjectSnapshot& snap, QSize size, bool proxies)
{
    std::vector<TimeUs> cuts{0};
    for (const Track& t : snap.timeline.tracks)
        for (const Clip& c : t.clips) {
            cuts.push_back(c.start);
            cuts.push_back(c.end());
        }
    std::sort(cuts.begin(), cuts.end());
    cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
    // What every stretch shares: the settings, the size, the files (as they were when probed).
    QCryptographicHash common(QCryptographicHash::Sha1);
    const ProjectSettings& ps = snap.settings;
    common.addData(QStringLiteral("%1x%2@%3/%4 %5x%6 %7").arg(ps.width).arg(ps.height).arg(ps.fps.num).arg(ps.fps.den)
                       .arg(size.width()).arg(size.height()).arg(proxies).toUtf8());
    for (const auto& [id, m] : snap.media)
        if (m)
            common.addData(QStringLiteral("|%1:%2:%3").arg(id).arg(m->path).arg(m->info.duration).toUtf8());
    const QByteArray base = common.result();
    std::vector<Stretch> out;
    for (size_t i = 0; i + 1 < cuts.size(); ++i) {
        const TimeUs a = cuts[i], b = cuts[i + 1];
        QCryptographicHash h(QCryptographicHash::Sha1);
        h.addData(base);
        h.addData(QByteArray::number(a) + ':' + QByteArray::number(b));
        h.addData(renderKeyData(snap.timeline, a, b));
        out.push_back({a, b, h.result().toHex()});
    }
    return out;
}

const Stretch* stretchAt(const std::vector<Stretch>& list, TimeUs t)
{
    auto it = std::upper_bound(list.begin(), list.end(), t, [](TimeUs v, const Stretch& s) { return v < s.end; });
    return it != list.end() && t >= it->start ? &*it : nullptr;
}

Store::Store(QString dir) : m_dir(std::move(dir))
{
    if (m_dir.isEmpty())
        m_dir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("prerender"));
}

QString Store::path(const QByteArray& key, int64_t frame) const
{
    return QDir(m_dir).filePath(QString::fromLatin1(key) + QLatin1Char('/') + QString::number(frame) + QStringLiteral(".jpg"));
}

const std::set<int64_t>& Store::framesOf(const QByteArray& key)
{
    auto it = m_frames.find(key);
    if (it == m_frames.end()) {
        std::set<int64_t> frames;
        for (const QFileInfo& f : QDir(QDir(m_dir).filePath(QString::fromLatin1(key))).entryInfoList({QStringLiteral("*.jpg")}, QDir::Files))
            frames.insert(f.completeBaseName().toLongLong());
        it = m_frames.emplace(key, std::move(frames)).first;
    }
    return it->second;
}

bool Store::has(const QByteArray& key, int64_t frame)
{
    std::lock_guard lock(m_mutex);
    return framesOf(key).count(frame) > 0;
}

QImage Store::load(const QByteArray& key, int64_t frame)
{
    {
        std::lock_guard lock(m_mutex);
        if (!framesOf(key).count(frame))
            return {};
    }
    QImage img(path(key, frame));
    if (img.isNull()) { // (gone: cleared by another MVP sharing the folder) made again
        std::lock_guard lock(m_mutex);
        const_cast<std::set<int64_t>&>(framesOf(key)).erase(frame);
        return img;
    }
    return img.convertToFormat(QImage::Format_ARGB32_Premultiplied);
}

void Store::save(const QByteArray& key, int64_t frame, const QImage& image)
{
    QDir().mkpath(QDir(m_dir).filePath(QString::fromLatin1(key)));
    const QString file = path(key, frame);
    if (!image.convertToFormat(QImage::Format_RGB32).save(file, "JPG", 92))
        return;
    std::lock_guard lock(m_mutex);
    if (QFileInfo::exists(file)) // (not when cleared while it was being saved)
        const_cast<std::set<int64_t>&>(framesOf(key)).insert(frame);
}

void Store::trim(qint64 bytes)
{
    struct Entry {
        QString dir;
        QDateTime when;
        qint64 size = 0;
    };
    std::vector<Entry> entries;
    qint64 total = 0;
    for (const QFileInfo& d : QDir(m_dir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        Entry e{d.absoluteFilePath(), d.lastModified()};
        for (QDirIterator it(e.dir, {QStringLiteral("*.jpg")}, QDir::Files); it.hasNext();) {
            it.next();
            e.size += it.fileInfo().size();
        }
        total += e.size;
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.when < b.when; });
    for (const Entry& e : entries) {
        if (total <= bytes)
            break;
        QDir(e.dir).removeRecursively();
        total -= e.size;
        std::lock_guard lock(m_mutex);
        m_frames.erase(QFileInfo(e.dir).fileName().toLatin1());
    }
}

void Store::clear()
{
    std::lock_guard lock(m_mutex); // (held throughout, so a frame saved meanwhile isn't counted kept)
    QDir(m_dir).removeRecursively();
    m_frames.clear();
}

} // namespace precache
