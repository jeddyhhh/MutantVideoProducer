#include "ui/MediaBin.h"

#include "ui/Theme.h"
#include "ui/Help.h"
#include "core/Project.h"
#include "core/Profiles.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QStandardPaths>
#include "core/Brand.h"
#include <QFileInfo>
#include <QPointer>
#include <QThreadPool>
#include <QDir>
#include <QJsonObject>
#include <QUrl>
#include <QSet>
#include "engine/MediaCache.h"

#include "core/FrameConverter.h"
#include "core/Proxy.h"
#include "core/VideoDecoder.h"

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QVBoxLayout>

namespace {
constexpr int kIconW = 112;
constexpr int kIconH = 63;

QIcon placeholderIcon(bool audio)
{
    QPixmap pm(kIconW, kIconH);
    pm.fill(theme::c().field);
    QPainter p(&pm);
    p.setPen(theme::c().dim);
    p.drawText(pm.rect(), Qt::AlignCenter, audio ? QStringLiteral("♪ audio") : QStringLiteral("…"));
    return QIcon(pm);
}

QIcon thumbIcon(const QImage& img, bool proxyBadge)
{
    QPixmap pm(kIconW, kIconH);
    pm.fill(Qt::black);
    QPainter p(&pm);
    const QImage scaled = img.scaled(kIconW, kIconH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    p.drawImage((kIconW - scaled.width()) / 2, (kIconH - scaled.height()) / 2, scaled);
    if (proxyBadge) { // a "P" badge in the corner
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(40, 140, 220));
        p.drawRoundedRect(QRectF(kIconW - 17, 3, 14, 13), 3, 3);
        QFont f = p.font();
        f.setPixelSize(10);
        f.setBold(true);
        p.setFont(f);
        p.setPen(Qt::white);
        p.drawText(QRectF(kIconW - 17, 3, 14, 13), Qt::AlignCenter, QStringLiteral("P"));
    }
    return QIcon(pm);
}

QString stars(int n)
{
    return n > 0 ? QString(n, QChar(0x2605)) : QString();
}
} // namespace

MediaBin::MediaBin(Project* project, MediaCache* cache, QWidget* parent)
    : QListWidget(parent), m_project(project), m_cache(cache)
{
    setViewMode(QListView::IconMode);
    setIconSize(QSize(kIconW, kIconH));
    setGridSize(QSize(kIconW + 24, kIconH + 40));
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setWordWrap(true);
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setDragEnabled(true);
    setAcceptDrops(true);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::DragDrop);
    setDefaultDropAction(Qt::CopyAction);
    setUniformItemSizes(true);

    connect(m_project, &Project::mediaChanged, this, &MediaBin::rebuild);
    connect(m_cache, &MediaCache::updated, this, &MediaBin::refreshIcons);
    connect(this, &QListWidget::itemDoubleClicked, this,
            [this](QListWidgetItem* item) {
                if (item->data(Qt::UserRole).toInt()) emit addToTimelineRequested(item->data(Qt::UserRole).toInt());
                else emit sharedToTimelineRequested(item->data(Qt::UserRole + 2).toString());
            });
    connect(&Profiles::instance(), &Profiles::libraryChanged, this, [this] { if (m_sharedProfile) rebuild(); });
    rebuild();
}

QList<int> MediaBin::selectedMediaIds() const
{
    QList<int> ids;
    for (auto* item : selectedItems())
        if (item->data(Qt::UserRole).toInt()) ids << item->data(Qt::UserRole).toInt();
    return ids;
}

void MediaBin::rebuild()
{
    const QList<int> selected = selectedMediaIds(); // (kept: ratings and folders rebuild the list)
    QSet<QString> selectedPaths;
    for (auto* it : selectedItems()) if (!it->data(Qt::UserRole).toInt()) selectedPaths.insert(it->data(Qt::UserRole + 2).toString());
    clear();
    // By folder, then name.
    std::vector<std::shared_ptr<const MediaItem>> list;
    for (const auto& [id, media] : m_project->media())
        if (m_origin.isEmpty() || media->origin == m_origin) list.push_back(media);
    std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
        return std::make_pair(a->folder.toLower(), a->displayName().toLower()) < std::make_pair(b->folder.toLower(), b->displayName().toLower());
    });
    for (const auto& media : list) {
        QString text = media->displayName();
        if (media->rating)
            text += QLatin1Char('\n') + stars(media->rating);
        auto* item = new QListWidgetItem(text, this);
        item->setData(Qt::UserRole, media->id);
        QString details = media->info.summary();
        if (!media->folder.isEmpty())
            details += tr("\nFolder: %1").arg(media->folder);
        if (media->markOut > media->markIn)
            details += tr("\nMarked: %1 to %2 s").arg(double(media->markIn) / kUsPerSecond, 0, 'f', 2).arg(double(media->markOut) / kUsPerSecond, 0, 'f', 2);
        item->setToolTip(details);
        item->setData(Qt::UserRole + 1, details);
        if (!media->info.ok()) {
            item->setForeground(theme::c().bad);
            item->setText(tr("%1 (offline)").arg(media->displayName()));
            item->setToolTip(tr("Can't open %1\nRight-click > Relink Media to find it.").arg(media->path));
        }
        item->setSelected(selected.contains(media->id));
    }
    if (m_sharedProfile) {
        QSet<QString> projectPaths;
        for (const auto& m : list) projectPaths.insert(Profiles::libraryKey(m->path));
        for (const auto& value : Profiles::instance().media()) {
            const auto record = value.toObject(); const QString path = record.value("path").toString();
            if (path.isEmpty() || !QDir::isAbsolutePath(path)) continue;
            if (!m_origin.isEmpty() && record.value("origin").toString() != m_origin) continue;
            const bool inProject = projectPaths.contains(Profiles::libraryKey(path));
            if (inProject) continue;
            auto* it = new QListWidgetItem(QFileInfo(path).fileName(), this);
            it->setData(Qt::UserRole, 0); it->setData(Qt::UserRole + 2, path); it->setData(Qt::UserRole + 3, record);
            it->setSelected(selectedPaths.contains(path));
            it->setIcon(placeholderIcon(false)); it->setToolTip(tr("Profile library: %1\nDouble-click or drag to import into this project.").arg(QDir::toNativeSeparators(path)));
            auto known = m_exists.find(path); // (looked up once: the library can hold thousands of files on slow drives)
            if (known == m_exists.end()) known = m_exists.insert(path, QFileInfo::exists(path));
            if (!*known) { it->setForeground(theme::c().bad); it->setText(it->text() + tr(" (offline)")); }
        }
    }
    if (m_folderBox) { // the folders to choose from
        QSignalBlocker block(m_folderBox);
        m_folderBox->clear();
        m_folderBox->addItem(tr("All folders"), QString());
        for (const QString& f : folders())
            m_folderBox->addItem(f, f);
        m_folderBox->setCurrentIndex(std::max(0, m_folderBox->findData(m_folder)));
    }
    applyFilter();
    refreshIcons();
}

QStringList MediaBin::folders() const
{
    QStringList list;
    for (const auto& [id, media] : m_project->media())
        if ((m_origin.isEmpty() || media->origin == m_origin) && !media->folder.isEmpty() && !list.contains(media->folder))
            list << media->folder;
    list.sort(Qt::CaseInsensitive);
    if (m_sharedProfile) for (const auto& value : Profiles::instance().media()) {
        const auto r = value.toObject(); const QString f = r.value("folder").toString();
        if ((m_origin.isEmpty() || r.value("origin").toString() == m_origin) && !f.isEmpty() && !list.contains(f)) list << f;
    }
    return list;
}

void MediaBin::setSharedProfile(bool enabled) { m_sharedProfile = enabled; rebuild(); }

void MediaBin::showEvent(QShowEvent* e)
{
    QListWidget::showEvent(e);
    if (m_sharedProfile && !m_exists.isEmpty()) { // files may have come back or gone while it was hidden
        m_exists.clear();
        rebuild();
    }
}

void MediaBin::setFilter(const QString& search, const QString& folder, int minRating)
{
    m_search = search.trimmed();
    m_folder = folder;
    m_minRating = minRating;
    applyFilter();
}

void MediaBin::setOriginFilter(const QString& origin)
{
    m_origin = origin;
    setAcceptDrops(origin.isEmpty());
    setDragDropMode(origin.isEmpty() ? QAbstractItemView::DragDrop : QAbstractItemView::DragOnly);
    rebuild();
}

void MediaBin::paintEvent(QPaintEvent* e)
{
    QListWidget::paintEvent(e);
    for (int i = 0; i < count(); ++i)
        if (!item(i)->isHidden())
            return;
    QPainter p(viewport());
    p.setPen(theme::c().faint);
    p.drawText(viewport()->rect().adjusted(16, 16, -16, -16), Qt::AlignCenter | Qt::TextWordWrap,
               count() ? tr("Nothing matches the filter.") : m_origin == QLatin1String("youtube")
                       ? tr("Clips downloaded with File > Create Clip from YouTube appear here.\nDrag them onto the timeline, or double-click to add at the cursor.")
                       : tr("Drag files here, or press Ctrl+I to import video, music and pictures.\n"
                            "File > Create Clip from YouTube downloads a video."));
}

void MediaBin::applyFilter()
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* it = item(i);
        const auto media = m_project->mediaItem(it->data(Qt::UserRole).toInt());
        const auto record = it->data(Qt::UserRole + 3).toJsonObject();
        const QString folder = media ? media->folder : record.value("folder").toString();
        const int rating = media ? media->rating : record.value("rating").toInt();
        const QString name = media ? media->displayName() : it->text();
        const bool show = rating >= m_minRating
                          && (m_folder.isEmpty() || folder == m_folder || folder.startsWith(m_folder + QLatin1Char('/')))
                          && (m_search.isEmpty() || name.contains(m_search, Qt::CaseInsensitive) || folder.contains(m_search, Qt::CaseInsensitive));
        it->setHidden(!show);
    }
}

QWidget* MediaBin::createFilterBar()
{
    auto* bar = new QWidget;
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(2, 2, 2, 2);
    auto* search = new QLineEdit;
    search->setObjectName(QStringLiteral("mediaSearch"));
    search->setPlaceholderText(tr("Search"));
    search->setClearButtonEnabled(true);
    m_folderBox = new QComboBox;
    m_folderBox->setObjectName(QStringLiteral("mediaFolder"));
    m_folderBox->setToolTip(tr("Show one folder (right-click files > Move to Folder)"));
    auto* rating = new QComboBox;
    rating->setObjectName(QStringLiteral("mediaRating"));
    rating->setToolTip(tr("Show files rated at least this (select files and press 0-5)"));
    rating->addItem(tr("Any rating"), 0);
    for (int n = 1; n <= 5; ++n)
        rating->addItem(stars(n) + (n < 5 ? tr(" or more") : QString()), n);
    h->addWidget(search, 1);
    h->addWidget(m_folderBox);
    h->addWidget(rating);
    auto update = [this, search, rating] { setFilter(search->text(), m_folderBox->currentData().toString(), rating->currentData().toInt()); };
    connect(search, &QLineEdit::textChanged, this, update);
    connect(m_folderBox, qOverload<int>(&QComboBox::activated), this, update);
    connect(rating, qOverload<int>(&QComboBox::activated), this, update);
    rebuild();
    return bar;
}

void MediaBin::setDetails(const std::function<void(QString&, int&)>& change)
{
    for (int id : selectedMediaIds())
        if (auto m = m_project->mediaItem(id)) {
            QString folder = m->folder;
            int rating = m->rating;
            change(folder, rating);
            m_project->setMediaDetails(id, folder, rating, m->markIn, m->markOut);
        }
}

void MediaBin::keyPressEvent(QKeyEvent* e)
{
    if (e->key() >= Qt::Key_0 && e->key() <= Qt::Key_5 && !(e->modifiers() & (Qt::ControlModifier | Qt::AltModifier))) {
        const int n = e->key() - Qt::Key_0;
        setDetails([n](QString&, int& rating) { rating = n; });
        return;
    }
    QListWidget::keyPressEvent(e);
}

void MediaBin::markInOut(int mediaId)
{
    const auto media = m_project->mediaItem(mediaId);
    if (!media || !media->info.ok() || media->info.isStill || media->info.duration <= 0)
        return;
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Mark In / Out: %1").arg(media->displayName()));
    auto* v = new QVBoxLayout(&dlg);
    auto* picture = new QLabel;
    picture->setFixedSize(480, 270);
    picture->setAlignment(Qt::AlignCenter);
    picture->setStyleSheet(QStringLiteral("background: black;"));
    auto* slider = new QSlider(Qt::Horizontal);
    const int frames = std::max(1, int(media->info.duration / 40000)); // (25 steps a second)
    slider->setRange(0, frames);
    auto* where = new QLabel;
    auto* row = new QHBoxLayout;
    auto* setIn = new QPushButton(tr("Set In (I)"));
    auto* setOut = new QPushButton(tr("Set Out (O)"));
    auto* clear = new QPushButton(tr("Clear"));
    row->addWidget(setIn);
    row->addWidget(setOut);
    row->addWidget(clear);
    row->addStretch(1);
    row->addWidget(where);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("media.md#project-media"));
    v->addWidget(picture);
    v->addWidget(slider);
    v->addLayout(row);
    v->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    TimeUs in = media->markIn, out = media->markOut > media->markIn ? media->markOut : media->info.duration;
    auto timeAt = [&](int v) { return TimeUs(double(v) / frames * double(media->info.duration)); };
    auto show = [&] {
        where->setText(tr("In %1 s  Out %2 s  (%3 s)").arg(double(in) / kUsPerSecond, 0, 'f', 2).arg(double(out) / kUsPerSecond, 0, 'f', 2)
                           .arg(double(out - in) / kUsPerSecond, 0, 'f', 2));
    };
    VideoDecoder decoder;
    FrameConverter converter;
    const bool video = media->info.hasVideo() && decoder.open(media->path, media->info.video.front().index, nullptr, false);
    auto showFrame = [&](int v) {
        if (!video)
            return;
        if (const AVFrame* f = decoder.frameAt(timeAt(v)))
            picture->setPixmap(QPixmap::fromImage(converter.convert(f, QSize(480, 270), false).scaled(480, 270, Qt::KeepAspectRatio)));
    };
    connect(slider, &QSlider::valueChanged, &dlg, showFrame);
    connect(setIn, &QPushButton::clicked, &dlg, [&] {
        in = std::min(timeAt(slider->value()), out - 40000);
        show();
    });
    connect(setOut, &QPushButton::clicked, &dlg, [&] {
        out = std::max(timeAt(slider->value()), in + 40000);
        show();
    });
    connect(clear, &QPushButton::clicked, &dlg, [&] {
        in = 0;
        out = media->info.duration;
        show();
    });
    setIn->setShortcut(QKeySequence(Qt::Key_I));
    setOut->setShortcut(QKeySequence(Qt::Key_O));
    if (!video)
        picture->setText(tr("(sound only)"));
    slider->setValue(int(double(in) / double(media->info.duration) * frames));
    showFrame(slider->value());
    show();
    if (dlg.exec() == QDialog::Accepted)
        m_project->setMediaDetails(mediaId, media->folder, media->rating, in, out >= media->info.duration && in == 0 ? 0 : out);
}

void MediaBin::refreshIcons()
{
    for (int i = 0; i < count(); ++i) {
        QListWidgetItem* it = item(i);
        auto media = m_project->mediaItem(it->data(Qt::UserRole).toInt());
        if (!media) {
            if (const QIcon icon = libraryIcon(it->data(Qt::UserRole + 2).toString()); !icon.isNull())
                it->setIcon(icon);
            continue;
        }
        if (media->info.hasVideo()) {
            const TimeUs t = media->info.isStill ? 0 : std::min<TimeUs>(kUsPerSecond, media->info.duration / 2);
            const int stream = media->info.video.front().index;
            const QImage img = m_cache->thumbnail(media, stream, t);
            const bool hasProxy = !media->info.isStill && proxy::exists(media->path, stream);
            if (media->info.ok())
                it->setToolTip(it->data(Qt::UserRole + 1).toString()
                               + (hasProxy ? tr("\nProxy: ready (the preview plays it when Proxies is on)")
                                  : proxy::recommended(media->info) ? tr("\nA heavy file: right-click > Create Proxy for smooth previews")
                                                                     : QString()));
            it->setIcon(img.isNull() ? placeholderIcon(false) : thumbIcon(img, hasProxy));
        } else {
            it->setIcon(placeholderIcon(true));
        }
    }
}

QIcon MediaBin::libraryIcon(const QString& path)
{
    auto [entry, added] = m_library.try_emplace(path);
    if (!added || !QFileInfo::exists(path))
        return entry->second;
    const QFileInfo fi(path);
    const QByteArray id = (fi.absoluteFilePath() + QString::number(fi.size()) + QString::number(fi.lastModified().toMSecsSinceEpoch())).toUtf8();
    const QString dir = QDir(brand::storageLocation(QStandardPaths::CacheLocation)).filePath(QStringLiteral("thumbnails"));
    const QString saved = QDir(dir).filePath(QString::fromLatin1(QCryptographicHash::hash(id, QCryptographicHash::Md5).toHex()) + QStringLiteral(".jpg"));
    QThreadPool::globalInstance()->start([bin = QPointer<MediaBin>(this), path, dir, saved] {
        QImage img(saved);
        bool audio = false;
        if (img.isNull()) {
            const MediaInfo info = probeMedia(path);
            audio = info.ok() && !info.hasVideo();
            if (info.hasVideo()) {
                const VideoStreamInfo& vs = info.video.front();
                const QString source = proxy::playbackPath(path, vs.index); // quicker for heavy files
                VideoDecoder decoder;
                FrameConverter converter;
                if (decoder.open(source, source == path ? vs.index : 0, nullptr, false))
                    img = MediaCache::decodeThumbnail(decoder, converter, vs, info.isStill ? 0 : std::min<TimeUs>(kUsPerSecond, info.duration / 2));
                if (!img.isNull() && QDir().mkpath(dir))
                    img.save(saved, "JPG", 85);
            }
        }
        QMetaObject::invokeMethod(qApp, [bin, path, img, audio] {
            if (!bin)
                return;
            const QIcon icon = img.isNull() ? placeholderIcon(audio) : thumbIcon(img, false);
            bin->m_library[path] = icon;
            for (int i = 0; i < bin->count(); ++i)
                if (QListWidgetItem* it = bin->item(i); !it->data(Qt::UserRole).toInt() && it->data(Qt::UserRole + 2).toString() == path)
                    it->setIcon(icon);
        });
    });
    return {};
}

QStringList MediaBin::mimeTypes() const
{
    return {QString::fromLatin1(kMimeType), QStringLiteral("text/uri-list")};
}

static QString dragSource(const Project& project)
{
    return QStringLiteral("%1/%2").arg(QCoreApplication::applicationPid()).arg(quintptr(project.session().get()));
}

QMimeData* MediaBin::mimeData(const QList<QListWidgetItem*>& items) const
{
    if (std::any_of(items.begin(), items.end(), [](auto* it) { return it->data(Qt::UserRole).toInt() == 0; })) {
        auto* mime = new QMimeData; QList<QUrl> urls;
        for (auto* it : items) {
            const auto m = m_project->mediaItem(it->data(Qt::UserRole).toInt());
            urls << QUrl::fromLocalFile(m ? m->path : it->data(Qt::UserRole + 2).toString());
        }
        mime->setUrls(urls); return mime;
    }
    std::vector<int> ids;
    for (auto* item : items)
        ids.push_back(item->data(Qt::UserRole).toInt());
    auto* mime = new QMimeData;
    mime->setData(QString::fromLatin1(kMimeType), dragData(*m_project, ids));
    return mime;
}

QByteArray MediaBin::dragData(const Project& project, const std::vector<int>& ids)
{
    QStringList list;
    for (const int id : ids)
        list << QString::number(id);
    return (dragSource(project) + QLatin1Char('|') + list.join(QLatin1Char(','))).toUtf8();
}

std::vector<int> MediaBin::draggedIds(const QMimeData* mime, const Project& project)
{
    std::vector<int> ids;
    const QString data = QString::fromUtf8(mime->data(QString::fromLatin1(kMimeType)));
    if (data.section(QLatin1Char('|'), 0, 0) != dragSource(project))
        return ids;
    for (const QString& s : data.section(QLatin1Char('|'), 1).split(QLatin1Char(','), Qt::SkipEmptyParts))
        ids.push_back(s.toInt());
    return ids;
}

void MediaBin::dragEnterEvent(QDragEnterEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        QListWidget::dragEnterEvent(e);
}

void MediaBin::dragMoveEvent(QDragMoveEvent* e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
    else
        e->ignore();
}

void MediaBin::dropEvent(QDropEvent* e)
{
    if (!e->mimeData()->hasUrls()) {
        e->ignore();
        return;
    }
    QStringList paths;
    for (const QUrl& url : e->mimeData()->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    e->acceptProposedAction();
    if (!paths.isEmpty())
        emit importRequested(paths);
}

void MediaBin::contextMenuEvent(QContextMenuEvent* e)
{
    QListWidgetItem* item = itemAt(e->pos());
    QMenu menu(this);
    if (item) {
        const int id = item->data(Qt::UserRole).toInt();
        if (!id) {
            const QString path = item->data(Qt::UserRole + 2).toString();
            menu.addAction(tr("Add to Project Media"), this, [this, path] { emit importRequested({path}); });
            menu.addAction(tr("Add to Timeline at Cursor"), this, [this, path] { emit sharedToTimelineRequested(path); });
            menu.exec(e->globalPos()); return;
        }
        if (auto m = m_project->mediaItem(id); m && !m->info.ok()) {
            menu.addAction(tr("Relink Media..."), this, [this, id] { emit relinkRequested(id); });
            menu.addSeparator();
        }
        menu.addAction(tr("Add to Timeline at Cursor"), this, [this, id] { emit addToTimelineRequested(id); });
        if (auto m = m_project->mediaItem(id); m && m->info.ok() && !m->info.isStill) {
            menu.addAction(tr("Mark In / Out..."), this, [this, id] { markInOut(id); });
            if (m->markOut > m->markIn)
                menu.addAction(tr("Clear In / Out"), this, [this, id] {
                    if (auto mm = m_project->mediaItem(id))
                        m_project->setMediaDetails(id, mm->folder, mm->rating, 0, 0);
                });
        }
        QMenu* rate = menu.addMenu(tr("Rating"));
        for (int n = 0; n <= 5; ++n)
            rate->addAction(n ? stars(n) : tr("None"), this, [this, n] { setDetails([n](QString&, int& r) { r = n; }); });
        QMenu* move = menu.addMenu(tr("Move to Folder"));
        for (const QString& f : folders())
            move->addAction(f, this, [this, f] { setDetails([f](QString& folder, int&) { folder = f; }); });
        move->addSeparator();
        move->addAction(tr("New Folder..."), this, [this] {
            bool ok = false;
            const QString f = QInputDialog::getText(this, tr("New Folder"), tr("Folder (a / makes one inside another)"), QLineEdit::Normal, QString(), &ok);
            if (ok && !f.trimmed().isEmpty())
                setDetails([f](QString& folder, int&) { folder = f.trimmed(); });
        });
        move->addAction(tr("No Folder"), this, [this] { setDetails([](QString& folder, int&) { folder.clear(); }); });
        menu.addAction(tr("Properties..."), this, [this, id] {
            if (auto m = m_project->mediaItem(id))
                QMessageBox::information(this, tr("Media Properties"), m->info.summary());
        });
        if (auto m = m_project->mediaItem(id); m && m->info.ok() && m->info.hasVideo() && !m->info.isStill) {
            menu.addSeparator();
            const bool has = proxy::exists(m->path, m->info.video.front().index);
            menu.addAction(has ? tr("Create Proxy (again)") : tr("Create Proxy"), this, [this] { emit proxyRequested(selectedMediaIds()); })
                ->setToolTip(tr("A light copy the preview plays instead (renders still use the original)"));
            menu.addAction(tr("Delete Proxy"), this, [this] { emit proxyDeleteRequested(selectedMediaIds()); })->setEnabled(has);
        }
        menu.addSeparator();
        menu.addAction(tr("Remove from Project"), this, [this] {
            QStringList inUse;
            for (int mid : selectedMediaIds()) {
                if (m_project->isMediaUsed(mid)) {
                    if (auto m = m_project->mediaItem(mid))
                        inUse << m->displayName();
                } else {
                    m_project->removeMedia(mid);
                }
            }
            if (!inUse.isEmpty())
                QMessageBox::information(this, tr("Media In Use"),
                                         tr("These files are used on the timeline and were kept:\n%1").arg(inUse.join('\n')));
        });
    } else {
        menu.addAction(tr("Import Media..."), this, [this] { emit importRequested({}); });
    }
    menu.exec(e->globalPos());
}
