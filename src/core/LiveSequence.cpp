#include "core/FileNames.h"
#include "core/LiveSequence.h"

#include "core/LivePadSets.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>

int LiveSequence::snapTicks() const
{
    if (snap == 0)
        return beatTicks();
    if (snap < 0) // triplets: three in the time of two
        return std::max(1, kTicksPerWhole * 2 / (-snap * 3));
    return std::max(1, kTicksPerWhole / snap);
}

int LiveSequence::padAt(int tick) const
{
    const int loop = loopTicks();
    if (loop <= 0)
        return -1;
    tick = ((tick % loop) + loop) % loop;
    const LiveSequenceBlock* best = nullptr;
    for (const LiveSequenceBlock& b : blocks)
        if (b.length > 0 && tick >= b.start && tick < b.end()
            && (!best || b.start > best->start || (b.start == best->start && b.pad < best->pad)))
            best = &b;
    return best ? best->pad : -1;
}

QJsonObject liveSequenceToJson(const LiveSequence& s)
{
    QJsonArray blocks;
    for (const LiveSequenceBlock& b : s.blocks)
        blocks.append(QJsonArray{b.pad, b.start, b.length});
    return {{QStringLiteral("on"), s.on},
            {QStringLiteral("bars"), s.bars},
            {QStringLiteral("beatsPerBar"), s.beatsPerBar},
            {QStringLiteral("beatUnit"), s.beatUnit},
            {QStringLiteral("snap"), s.snap},
            {QStringLiteral("blocks"), blocks}};
}

LiveSequence liveSequenceFromJson(const QJsonObject& o)
{
    LiveSequence s;
    s.on = o[QStringLiteral("on")].toBool();
    s.bars = std::clamp(o[QStringLiteral("bars")].toInt(4), 1, 64);
    s.beatsPerBar = std::clamp(o[QStringLiteral("beatsPerBar")].toInt(4), 1, 16);
    const int unit = o[QStringLiteral("beatUnit")].toInt(4);
    s.beatUnit = unit == 2 || unit == 8 || unit == 16 ? unit : 4;
    const int snap = o[QStringLiteral("snap")].toInt(16);
    s.snap = snap == 0 || snap == 1 || snap == 2 || snap == 4 || snap == 8 || snap == 16 || snap == 32 || snap == -8 || snap == -16 ? snap : 16;
    for (const QJsonValue v : o[QStringLiteral("blocks")].toArray()) {
        const QJsonArray a = v.toArray();
        LiveSequenceBlock b{a.at(0).toInt(), a.at(1).toInt(), a.at(2).toInt()};
        constexpr int kMost = 1 << 20; // (the longest loop is 196608 ticks: past this a file is nonsense, and start + length overflows)
        if (b.pad >= 0 && b.pad < 16 && b.start >= 0 && b.start < kMost && b.length > 0 && b.length <= kMost)
            s.blocks.push_back(b);
    }
    return s;
}

namespace sequences {
namespace {
QString fileFor(const QString& name)
{
    QString base;
    for (const QChar ch : name.trimmed())
        base += ch.isLetterOrNumber() || ch == QLatin1Char(' ') || ch == QLatin1Char('-') || ch == QLatin1Char('_') ? ch : QLatin1Char('_');
    return QDir(directory()).filePath(safeFileName(base, QStringLiteral("sequence")) + QStringLiteral(".json"));
}

// {name, sequence} from a file, if it is one.
std::optional<std::pair<QString, LiveSequence>> read(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (!o[QStringLiteral("sequence")].isObject())
        return std::nullopt;
    QString name = o[QStringLiteral("name")].toString();
    if (name.isEmpty())
        name = QFileInfo(path).completeBaseName();
    return std::make_pair(name, liveSequenceFromJson(o[QStringLiteral("sequence")].toObject()));
}

// The file holding `name` (by its file name, or one renamed by hand: the name inside); empty: none.
QString find(const QString& name)
{
    if (const auto s = read(fileFor(name)); s && s->first == name)
        return fileFor(name);
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (const auto s = read(dir.filePath(file)); s && s->first == name)
            return dir.filePath(file);
    return {};
}

// Where `name` is saved: its file already, else its own file (numbered when that holds another name: "a/b" and
// "a_b", or "Beat" and "beat" on Windows, share a file name).
QString target(const QString& name)
{
    if (const QString path = find(name); !path.isEmpty())
        return path;
    QString path = fileFor(name);
    for (int n = 2; QFile::exists(path); ++n)
        path = fileFor(name + QStringLiteral(" %1").arg(n));
    return path;
}
} // namespace

QString directory()
{
    const QString dir = QDir(padsets::directory()).filePath(QStringLiteral("Sequences"));
    QDir().mkpath(dir);
    return dir;
}

QStringList names()
{
    QStringList out;
    const QDir dir(directory());
    for (const QString& file : dir.entryList({QStringLiteral("*.json")}, QDir::Files))
        if (const auto s = read(dir.filePath(file)))
            out << s->first;
    std::sort(out.begin(), out.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    out.removeDuplicates();
    return out;
}

std::optional<LiveSequence> load(const QString& name)
{
    const QString path = find(name);
    if (path.isEmpty())
        return std::nullopt;
    return read(path)->second;
}

bool save(const QString& name, const LiveSequence& s, QString* error)
{
    if (name.trimmed().isEmpty()) {
        if (error)
            *error = QStringLiteral("the sequence needs a name");
        return false;
    }
    LiveSequence saved = s;
    saved.on = false; // (whether it plays is the project's)
    const QJsonObject o{{QStringLiteral("name"), name.trimmed()}, {QStringLiteral("sequence"), liveSequenceToJson(saved)}};
    QSaveFile f(target(name.trimmed()));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(o).toJson()) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

bool remove(const QString& name)
{
    const QString path = find(name);
    return !path.isEmpty() && QFile::remove(path);
}
} // namespace sequences
