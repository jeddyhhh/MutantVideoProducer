#include "ui/RelinkDialog.h"

#include "ui/Help.h"
#include "core/Project.h"

#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <map>

namespace {

#ifdef Q_OS_WIN
QString nameKey(const QString& name) { return name.toLower(); }
#else
QString nameKey(const QString& name) { return name; }
#endif

// The nearest folder of `path` that still exists (for the file dialog).
QString existingFolder(const QString& path)
{
    QDir dir = QFileInfo(path).dir();
    while (!dir.exists() && dir.cdUp()) {
    }
    return dir.exists() ? dir.absolutePath() : QDir::homePath();
}

} // namespace

RelinkDialog::RelinkDialog(Project* project, int focusMediaId, QWidget* parent) : QDialog(parent), m_project(project)
{
    setWindowTitle(tr("Missing Media"));
    resize(760, 380);
    for (int id : m_project->offlineMedia())
        m_ids.insert(id);
    if (focusMediaId)
        m_ids.insert(focusMediaId);

    auto* layout = new QVBoxLayout(this);
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);
    m_list = new QTreeWidget;
    m_list->setRootIsDecorated(false);
    m_list->setHeaderLabels({tr("File"), tr("Status"), tr("Location")});
    m_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_list, 1);
    connect(m_list, &QTreeWidget::itemDoubleClicked, this, [this] { locateSelected(); });

    auto* buttons = new QDialogButtonBox;
    help::addButton(buttons, QStringLiteral("media.md#missing-media"));
    QPushButton* locate = buttons->addButton(tr("Locate..."), QDialogButtonBox::ActionRole);
    locate->setToolTip(tr("Point the selected file at where it is now; the others are then looked for next to it"));
    QPushButton* search = buttons->addButton(tr("Search Folder..."), QDialogButtonBox::ActionRole);
    search->setToolTip(tr("Look for every missing file, by name, in a folder and all its subfolders"));
    buttons->addButton(QDialogButtonBox::Close);
    connect(locate, &QPushButton::clicked, this, &RelinkDialog::locateSelected);
    connect(search, &QPushButton::clicked, this, &RelinkDialog::searchDialog);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    refresh();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toInt() == focusMediaId || (!focusMediaId && i == 0))
            m_list->setCurrentItem(m_list->topLevelItem(i));
}

int RelinkDialog::missingCount() const
{
    int n = 0;
    for (int id : m_ids)
        if (auto m = m_project->mediaItem(id); m && !m->info.ok())
            ++n;
    return n;
}

void RelinkDialog::refresh()
{
    const int current = m_list->currentItem() ? m_list->currentItem()->data(0, Qt::UserRole).toInt() : 0;
    m_list->clear();
    for (int id : m_ids) {
        auto m = m_project->mediaItem(id);
        if (!m)
            continue;
        const bool ok = m->info.ok();
        auto* item = new QTreeWidgetItem(m_list, {m->displayName(), ok ? tr("Found") : tr("Missing"),
                                                  QDir::toNativeSeparators(QFileInfo(m->path).absolutePath())});
        item->setData(0, Qt::UserRole, id);
        item->setToolTip(2, QDir::toNativeSeparators(m->path));
        item->setForeground(1, ok ? QColor(0x6c, 0xd0, 0x7a) : QColor(0xe0, 0x60, 0x60));
        if (id == current)
            m_list->setCurrentItem(item);
    }
    const int missing = missingCount();
    m_summary->setText(missing == 0
                           ? tr("Every file was found. Its events are back online.")
                           : tr("%n file(s) used by this project can't be found: they were moved, renamed or are on a drive "
                                "that isn't connected. Their events show as \"Media offline\". Locate a file (the others are "
                                "then looked for next to it), or search a folder for all of them.",
                                nullptr, missing));
}

int RelinkDialog::relinkFound(const std::function<QString(const QString&)>& lookup)
{
    int found = 0;
    for (int id : m_ids) {
        auto m = m_project->mediaItem(id);
        if (!m || m->info.ok())
            continue;
        const QString candidate = lookup(m->path);
        if (!candidate.isEmpty() && QFileInfo::exists(candidate) && m_project->relinkMedia(id, candidate))
            ++found;
    }
    return found;
}

int RelinkDialog::relink(int mediaId, const QString& path, QString* error)
{
    auto media = m_project->mediaItem(mediaId);
    if (!media)
        return 0;
    const QString oldDir = QFileInfo(media->path).absolutePath();
    const QString newDir = QFileInfo(path).absolutePath();
    if (!m_project->relinkMedia(mediaId, path, error))
        return 0;
    m_ids.insert(mediaId);
    // The others: in the same folder as this one, or where their old folder
    // went (a whole project folder moved or copied to another drive).
    const int more = relinkFound([&](const QString& oldPath) {
        const QString sameFolder = QDir(newDir).filePath(QFileInfo(oldPath).fileName());
        if (QFileInfo::exists(sameFolder))
            return sameFolder;
        const QString oldBase = QFileInfo(oldDir).absolutePath(); // parent of the old folder
        const QString newBase = QFileInfo(newDir).absolutePath();
        const QString abs = QFileInfo(oldPath).absoluteFilePath();
        if (abs.startsWith(oldDir + QLatin1Char('/')))
            return newDir + abs.mid(oldDir.size());
        if (abs.startsWith(oldBase + QLatin1Char('/')))
            return newBase + abs.mid(oldBase.size());
        return QString();
    });
    refresh();
    return 1 + more;
}

int RelinkDialog::searchFolder(const QString& folder)
{
    std::map<QString, QString> wanted; // file name -> found path
    for (int id : m_ids)
        if (auto m = m_project->mediaItem(id); m && !m->info.ok())
            wanted.emplace(nameKey(QFileInfo(m->path).fileName()), QString());
    if (wanted.empty())
        return 0;
    size_t left = wanted.size();
    int scanned = 0;
    QDirIterator it(folder, QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext() && left > 0 && ++scanned < 300000) {
        it.next();
        auto w = wanted.find(nameKey(it.fileName()));
        if (w != wanted.end() && w->second.isEmpty()) {
            w->second = it.filePath();
            --left;
        }
    }
    const int found = relinkFound([&](const QString& oldPath) {
        auto w = wanted.find(nameKey(QFileInfo(oldPath).fileName()));
        return w == wanted.end() ? QString() : w->second;
    });
    refresh();
    return found;
}

void RelinkDialog::locateSelected()
{
    QTreeWidgetItem* item = m_list->currentItem();
    if (!item)
        return;
    const int id = item->data(0, Qt::UserRole).toInt();
    auto media = m_project->mediaItem(id);
    if (!media)
        return;
    const QString name = QFileInfo(media->path).fileName();
    const QString path = QFileDialog::getOpenFileName(this, tr("Locate %1").arg(name), existingFolder(media->path),
                                                      tr("%1;;All files (*)").arg(name));
    if (path.isEmpty())
        return;
    QString error;
    const int n = relink(id, path, &error);
    if (n == 0)
        QMessageBox::warning(this, tr("Locate"), tr("Could not use %1:\n%2").arg(QDir::toNativeSeparators(path), error));
    else if (n > 1)
        m_summary->setText(m_summary->text() + QLatin1Char('\n')
                           + tr("Also found %n other file(s) in the same place.", nullptr, n - 1));
}

void RelinkDialog::searchDialog()
{
    QSettings settings;
    const QString folder = QFileDialog::getExistingDirectory(this, tr("Search Folder for Missing Media"),
                                                             settings.value(QStringLiteral("dirs/media"), QDir::homePath()).toString());
    if (folder.isEmpty())
        return;
    setCursor(Qt::WaitCursor);
    const int before = missingCount();
    const int found = searchFolder(folder);
    unsetCursor();
    if (found == 0)
        QMessageBox::information(this, tr("Search Folder"),
                                 tr("None of the %n missing file(s) are in %1.", nullptr, before).arg(QDir::toNativeSeparators(folder)));
}
