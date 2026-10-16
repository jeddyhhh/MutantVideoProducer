#include "core/YouTubeUpload.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>

#include <algorithm>
#include <map>

namespace upload {

QString youtubeTime(TimeUs t, bool hours)
{
    const qint64 s = std::max<TimeUs>(0, t) / kUsPerSecond; // whole seconds, as YouTube reads them
    const QLatin1Char zero('0');
    return hours ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, zero).arg(s % 60, 2, 10, zero)
                 : QStringLiteral("%1:%2").arg(s / 60, 2, 10, zero).arg(s % 60, 2, 10, zero);
}

QString trackListing(const Project& project, int track)
{
    const Timeline& tl = project.timeline();
    if (track < 0 || track >= int(tl.tracks.size()))
        return {};
    std::vector<const Clip*> clips;
    for (const Clip& c : tl.tracks[size_t(track)].clips)
        clips.push_back(&c);
    std::sort(clips.begin(), clips.end(), [](const Clip* a, const Clip* b) { return a->start < b->start; });
    const bool hours = !clips.empty() && clips.back()->start >= 3600 * kUsPerSecond;
    const int digits = clips.size() > 99 ? 3 : 2;
    static const QRegularExpression number(QStringLiteral(R"(^\s*\d{1,3}(\s*[-._)]+\s*|\s+)(?=\S))"));
    QStringList lines;
    for (size_t i = 0; i < clips.size(); ++i) {
        const Clip& c = *clips[i];
        QString name = c.name;
        if (const auto media = project.mediaItem(c.mediaId); media && (name.isEmpty() || name == media->displayName()))
            name = QFileInfo(media->path).completeBaseName().remove(number);
        lines << QStringLiteral("%1. %2 - %3").arg(int(i + 1), digits, 10, QLatin1Char('0')).arg(name.trimmed(), youtubeTime(c.start, hours));
    }
    return lines.join(QLatin1Char('\n'));
}

QString youtubeChapters(const Timeline& tl, QStringList* warnings)
{
    std::map<qint64, QString> chapters; // by whole second: two in the same second would be one to YouTube
    for (const QJsonValue& v : tl.chapters) {
        const QJsonObject c = v.toObject();
        chapters.emplace(c.value(QStringLiteral("start")).toString().toLongLong() / kUsPerSecond, c.value(QStringLiteral("name")).toString());
    }
    if (chapters.empty()) {
        int n = 0;
        for (const Marker& m : tl.markers) {
            ++n;
            const QString name = m.name.trimmed().isEmpty() ? QStringLiteral("Chapter %1").arg(n) : m.name.trimmed();
            chapters.emplace(std::max<TimeUs>(0, m.time) / kUsPerSecond, name);
        }
    }
    if (chapters.empty())
        return {};
    if (chapters.begin()->first > 0)
        chapters.emplace(0, QStringLiteral("Intro"));
    const qint64 end = std::max<qint64>(tl.duration() / kUsPerSecond, chapters.rbegin()->first);
    const bool hours = end >= 3600;
    QStringList lines, shortOnes;
    for (auto it = chapters.begin(); it != chapters.end(); ++it) {
        const QString name = it->second.trimmed().isEmpty() ? QStringLiteral("Chapter") : it->second.trimmed();
        lines << youtubeTime(it->first * kUsPerSecond, hours) + QLatin1Char(' ') + name;
        const auto next = std::next(it);
        if ((next == chapters.end() ? end : next->first) - it->first < 10)
            shortOnes << name;
    }
    if (warnings) {
        warnings->clear();
        if (lines.size() < 3)
            *warnings << QStringLiteral("YouTube shows chapters only when there are at least 3; this list has %1.").arg(lines.size());
        if (!shortOnes.isEmpty())
            *warnings << QStringLiteral("YouTube needs every chapter to be at least 10 seconds long: %1.").arg(shortOnes.join(QStringLiteral(", ")));
    }
    return lines.join(QLatin1Char('\n'));
}

QImage thumbnail(const QImage& frame, const QString& title)
{
    const QSize size(1280, 720);
    QImage out(size, QImage::Format_RGB32);
    out.fill(Qt::black);
    QPainter p(&out);
    p.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform | QPainter::TextAntialiasing);
    if (!frame.isNull()) {
        const QImage filled = frame.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawImage(QPoint((size.width() - filled.width()) / 2, (size.height() - filled.height()) / 2), filled);
    }
    const QString text = title.simplified();
    if (!text.isEmpty()) {
        // Darken the bottom so the words read on any picture.
        QLinearGradient shade(0, size.height() * 0.45, 0, size.height());
        shade.setColorAt(0, QColor(0, 0, 0, 0));
        shade.setColorAt(1, QColor(0, 0, 0, 190));
        p.fillRect(out.rect(), shade);
        const QRectF box(48, size.height() * 0.42, size.width() - 96, size.height() * 0.58 - 40);
        QFont font(QStringLiteral("Arial Black"));
        font.setStyleHint(QFont::SansSerif);
        font.setWeight(QFont::Black);
        // The largest size up to 130 px whose (word-wrapped) lines fit the bottom part.
        QRectF needed;
        for (int px = 130; px >= 36; px -= 4) {
            font.setPixelSize(px);
            p.setFont(font);
            needed = p.boundingRect(box, Qt::AlignLeft | Qt::AlignBottom | Qt::TextWordWrap, text);
            if (needed.height() <= box.height() && needed.width() <= box.width())
                break;
        }
        // Outlined, line by line, so the stroke follows the glyphs.
        const QFontMetricsF fm(font);
        QStringList rows;
        for (const QString& word : text.split(QLatin1Char(' '))) {
            if (!rows.isEmpty() && fm.horizontalAdvance(rows.last() + QLatin1Char(' ') + word) <= box.width())
                rows.last() += QLatin1Char(' ') + word;
            else
                rows << word;
        }
        QPainterPath path;
        double y = box.bottom() - fm.descent() - (rows.size() - 1) * fm.lineSpacing();
        for (const QString& row : rows) {
            path.addText(QPointF(box.left(), y), font, row);
            y += fm.lineSpacing();
        }
        p.setPen(QPen(Qt::black, std::max(6.0, font.pixelSize() / 9.0), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(Qt::white);
        p.drawPath(path);
    }
    p.end();
    return out;
}

} // namespace upload
