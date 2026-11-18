#include "core/Osc.h"

#include <QtEndian>
#include <cstring>

namespace {

// A string padded with zeros to a multiple of 4; false past the end.
bool readString(const char* data, int size, int& pos, QByteArray* out)
{
    const int start = pos;
    while (pos < size && data[pos] != 0)
        ++pos;
    if (pos >= size)
        return false;
    if (out)
        *out = QByteArray(data + start, pos - start);
    pos = (pos + 4) & ~3; // past the zero, padded
    return pos <= size;
}

void parseElement(const char* data, int size, std::vector<OscMessage>& out, int depth)
{
    if (size <= 0 || depth > 8)
        return;
    int pos = 0;
    QByteArray address;
    if (!readString(data, size, pos, &address))
        return;
    if (address == "#bundle") {
        pos += 8; // the time tag (handled at once)
        while (pos + 4 <= size) {
            const qint32 n = qFromBigEndian<qint32>(data + pos);
            pos += 4;
            if (n < 0 || n > size - pos)
                return;
            parseElement(data + pos, n, out, depth + 1);
            pos += n;
        }
        return;
    }
    if (!address.startsWith('/'))
        return;
    OscMessage m;
    m.address = QString::fromUtf8(address);
    QByteArray tags;
    if (pos < size && data[pos] == ',' && !readString(data, size, pos, &tags))
        return;
    for (int i = 1; i < tags.size(); ++i) {
        const auto need = [&](int bytes) { return pos + bytes <= size; };
        switch (tags[i]) {
        case 'i':
            if (!need(4))
                return;
            m.args.push_back(qFromBigEndian<qint32>(data + pos));
            pos += 4;
            break;
        case 'f': {
            if (!need(4))
                return;
            const quint32 bits = qFromBigEndian<quint32>(data + pos);
            float f;
            std::memcpy(&f, &bits, 4);
            m.args.push_back(f);
            pos += 4;
            break;
        }
        case 'h':
        case 'd':
        case 't': {
            if (!need(8))
                return;
            const quint64 bits = qFromBigEndian<quint64>(data + pos);
            double v;
            if (tags[i] == 'd')
                std::memcpy(&v, &bits, 8);
            else
                v = double(qint64(bits));
            if (tags[i] != 't')
                m.args.push_back(v);
            pos += 8;
            break;
        }
        case 'T':
            m.args.push_back(1);
            break;
        case 'F':
            m.args.push_back(0);
            break;
        case 'N':
        case 'I':
            break;
        case 's':
        case 'S':
            if (!readString(data, size, pos, nullptr))
                return;
            break;
        case 'b': {
            if (!need(4))
                return;
            const qint32 n = qFromBigEndian<qint32>(data + pos);
            if (n < 0 || n > size - pos - 4)
                return;
            pos += 4 + ((n + 3) & ~3);
            break;
        }
        default:
            return; // a type we can't step over
        }
    }
    out.push_back(std::move(m));
}

void appendPadded(QByteArray& out, const QByteArray& s)
{
    out += s;
    out += QByteArray(4 - s.size() % 4, '\0');
}

} // namespace

std::vector<OscMessage> parseOsc(const QByteArray& packet)
{
    std::vector<OscMessage> out;
    parseElement(packet.constData(), int(packet.size()), out, 0);
    return out;
}

QByteArray oscPacket(const QString& address, const std::vector<float>& args)
{
    QByteArray out;
    appendPadded(out, address.toUtf8());
    appendPadded(out, ',' + QByteArray(int(args.size()), 'f'));
    for (float f : args) {
        quint32 bits;
        std::memcpy(&bits, &f, 4);
        char be[4];
        qToBigEndian(bits, be);
        out.append(be, 4);
    }
    return out;
}
