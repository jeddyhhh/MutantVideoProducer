#include "ui/LiveClipsBrowser.h"

#include "ui/Theme.h"
#include "core/FrameConverter.h"
#include "core/LiveRecorder.h"
#include "core/Profiles.h"
#include <QJsonObject>
#include "core/MediaInfo.h"
#include "core/VideoDecoder.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QThreadPool>
#include <QUrl>

namespace {
constexpr int kPathRole = Qt::UserRole;
constexpr int kIconW = 128, kIconH = 72;

QIcon placeholder()
{
    QPixmap pm(kIconW, kIconH);
    pm.fill(theme::c().field);
    QPainter p(&pm);
    p.setPen(theme::c().faint);
    p.drawText(pm.rect(), Qt::AlignCenter, QStringLiteral("LIVE"));
    return QIcon(pm);
}

QString liveDuration(TimeUs us)
{
    const int64_t s = (us + kUsPerSecond / 2) / kUsPerSecond;
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}
} // namespace

LiveClipsBrowser::LiveClipsBrowser(QWidget* parent) : QListWidget(parent)
{
    setIconSize(QSize(kIconW, kIconH));
    setDragEnabled(true);
    setDragDropMode(QAbstractItemView::DragOnly);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setSpacing(2);
    setWordWrap(true);
    setToolTip(tr("Clips recorded and screenshots taken on the Live tab: drag onto the timeline, or double-click to add at the cursor"));
    m_refreshTimer.setSingleShot(true);
    m_refreshTimer.setInterval(400);
    connect(&m_refreshTimer, &QTimer::timeout, this, &LiveClipsBrowser::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_refreshTimer.start(); });
    m_watcher.addPath(liveRecordingsFolder());
    connect(&Profiles::instance(), &Profiles::changed, this, [this] {
        if (!m_watcher.directories().isEmpty()) m_watcher.removePaths(m_watcher.directories());
        refresh();
    });
    connect(this, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) { emit addToTimelineRequested(item->data(kPathRole).toString()); });
    refresh();
}

void LiveClipsBrowser::setRecordingPath(const QString& path)
{
    m_recording = path;
    m_refreshTimer.start();
}

void LiveClipsBrowser::refresh()
{
    const QDir dir(liveRecordingsFolder());
    if (!m_watcher.directories().contains(dir.absolutePath()))
        m_watcher.addPath(dir.absolutePath());
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.mp4"), QStringLiteral("*.mkv"), QStringLiteral("*.mov"), QStringLiteral("*.png")},
                                                  QDir::Files, QDir::Time); // newest first
    QStringList wanted;
    for (const QFileInfo& fi : files)
        if (QDir::cleanPath(fi.absoluteFilePath()) != QDir::cleanPath(m_recording))
            wanted << fi.absoluteFilePath();
    // Keep existing items (and their loaded details); add new ones, drop removed ones.
    QJsonArray records;
    for (const auto& path : wanted)
        if (Profiles::instance().findMedia(path).isEmpty()) records.append(QJsonObject{{"path", path}, {"origin", "live"}, {"folder", "Live"}});
    Profiles::instance().remember(records);
    QStringList have;
    for (int i = count() - 1; i >= 0; --i) {
        const QString p = item(i)->data(kPathRole).toString();
        if (!wanted.contains(p))
            delete takeItem(i);
        else
            have << p;
    }
    for (int i = 0; i < wanted.size(); ++i) {
        const QString& p = wanted[i];
        if (have.contains(p))
            continue;
        auto* it = new QListWidgetItem(placeholder(), QFileInfo(p).completeBaseName());
        it->setData(kPathRole, p);
        it->setToolTip(QDir::toNativeSeparators(p));
        insertItem(i, it);
        loadDetails(p);
    }
    viewport()->update();
}

void LiveClipsBrowser::loadDetails(const QString& path)
{
    QPointer<LiveClipsBrowser> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        const MediaInfo info = probeMedia(path);
        QImage thumb;
        const QString thumbPath = liveThumbnailPath(path);
        if (info.ok() && info.isStill) { // a screenshot: itself
            thumb.load(path);
            thumb = thumb.scaled(QSize(320, 180), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        } else if (QFileInfo::exists(thumbPath)) {
            thumb.load(thumbPath);
        } else if (info.ok() && info.hasVideo()) {
            VideoDecoder dec;
            if (dec.open(path, info.video.front().index, nullptr, false)) {
                if (const AVFrame* f = dec.frameAt(std::min<TimeUs>(kUsPerSecond / 2, info.duration / 2))) {
                    FrameConverter conv;
                    const QSize src(info.video.front().width, info.video.front().height);
                    thumb = conv.convert(f, src.scaled(320, 180, Qt::KeepAspectRatio), true);
                    if (!thumb.isNull())
                        thumb.save(thumbPath, "JPG", 85);
                }
            }
        }
        QMetaObject::invokeMethod(qApp, [self, path, info, thumb] {
            if (!self)
                return;
            for (int i = 0; i < self->count(); ++i) {
                QListWidgetItem* it = self->item(i);
                if (it->data(kPathRole).toString() != path)
                    continue;
                QString detail;
                if (info.ok() && info.isStill)
                    detail = tr("Still  ·  %1x%2").arg(info.video.front().width).arg(info.video.front().height);
                else if (info.ok() && info.hasVideo())
                    detail = QStringLiteral("%1  ·  %2x%3").arg(liveDuration(info.duration)).arg(info.video.front().width).arg(info.video.front().height);
                else
                    detail = tr("unreadable: %1").arg(info.error);
                it->setText(QFileInfo(path).completeBaseName() + QLatin1Char('\n') + detail);
                if (!thumb.isNull()) {
                    QPixmap pm(kIconW, kIconH);
                    pm.fill(Qt::black);
                    QPainter p(&pm);
                    const QSize s = thumb.size().scaled(kIconW, kIconH, Qt::KeepAspectRatio);
                    p.setRenderHint(QPainter::SmoothPixmapTransform);
                    p.drawImage(QRect((kIconW - s.width()) / 2, (kIconH - s.height()) / 2, s.width(), s.height()), thumb);
                    p.end();
                    it->setIcon(QIcon(pm));
                }
            }
        });
    });
}

QStringList LiveClipsBrowser::mimeTypes() const
{
    return {QStringLiteral("text/uri-list")};
}

QMimeData* LiveClipsBrowser::mimeData(const QList<QListWidgetItem*>& items) const
{
    QList<QUrl> urls;
    for (const QListWidgetItem* it : items)
        urls << QUrl::fromLocalFile(it->data(kPathRole).toString());
    if (urls.isEmpty())
        return nullptr;
    auto* data = new QMimeData;
    data->setUrls(urls);
    return data;
}

void LiveClipsBrowser::contextMenuEvent(QContextMenuEvent* e)
{
    QListWidgetItem* it = itemAt(e->pos());
    QMenu menu;
    if (it) {
        const QString path = it->data(kPathRole).toString();
        menu.addAction(tr("Add to Timeline at Cursor"), this, [this, path] { emit addToTimelineRequested(path); });
        menu.addAction(tr("Add to Project Media"), this, [this, path] { emit addToMediaRequested(path); });
        menu.addSeparator();
        menu.addAction(tr("Show in Folder"), this, [path] {
#ifdef Q_OS_WIN
            QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(path)});
#else
            QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
        });
        menu.addAction(tr("Rename..."), this, [this, path] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, tr("Rename Clip"), tr("Name:"), QLineEdit::Normal,
                                                       QFileInfo(path).completeBaseName(), &ok).trimmed();
            if (!ok || name.isEmpty())
                return;
            const QString target = QFileInfo(path).dir().filePath(name + QLatin1Char('.') + QFileInfo(path).suffix());
            if (QFileInfo::exists(target) || !QFile::rename(path, target))
                QMessageBox::warning(this, tr("Rename Clip"), tr("Could not rename the clip to \"%1\".").arg(name));
            refresh();
        });
        menu.addAction(tr("Move to Recycle Bin"), this, [this, path] {
            if (QMessageBox::question(this, tr("Remove Clip"), tr("Move \"%1\" to the recycle bin?\n\nEvents already on the timeline "
                                                                    "will lose their media.").arg(QFileInfo(path).fileName()))
                != QMessageBox::Yes)
                return;
            if (!QFile::moveToTrash(path))
                QMessageBox::warning(this, tr("Remove Clip"), tr("Could not move the clip to the recycle bin."));
            QFile::remove(liveThumbnailPath(path));
            refresh();
        });
        menu.addSeparator();
    }
    menu.addAction(tr("Open Recordings Folder"), this,
                   [] { QDesktopServices::openUrl(QUrl::fromLocalFile(liveRecordingsFolder())); });
    menu.addAction(tr("Refresh"), this, &LiveClipsBrowser::refresh);
    menu.exec(e->globalPos());
}

void LiveClipsBrowser::paintEvent(QPaintEvent* e)
{
    QListWidget::paintEvent(e);
    if (count() > 0)
        return;
    QPainter p(viewport());
    p.setPen(theme::c().faint);
    p.drawText(viewport()->rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
               tr("No live clips yet.\n\nOn the Live tab, press ● Rec to record what you see, or Screenshot for a still; "
                  "they show up here, ready to drag onto the timeline."));
}
