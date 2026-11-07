#include "core/LiveSequence.h"

#include <QJsonArray>
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
        if (b.pad >= 0 && b.pad < 16 && b.start >= 0 && b.length > 0)
            s.blocks.push_back(b);
    }
    return s;
}
