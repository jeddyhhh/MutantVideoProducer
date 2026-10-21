// The Sounds tab: the built-in sound effects library (sfx next to the program, every sound CC0), to preview, drag onto
// the timeline or add at the cursor.
#include "ui/MainWindow.h"
#include "ui/TimelineWidget.h"
#include <QAudioOutput>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileSystemModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QPushButton>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

QString MainWindow::soundLibraryDir()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("sfx"));
}

QWidget* MainWindow::createSoundsTab()
{
    auto* page = new QWidget;
    page->setObjectName(QStringLiteral("soundsTab"));
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search sounds (whoosh, boom, ding...)"));
    search->setClearButtonEnabled(true);
    layout->addWidget(search);
    auto* model = new QFileSystemModel(page);
    model->setReadOnly(true);
    model->setFilter(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    model->setNameFilters({QStringLiteral("*.ogg")});
    model->setNameFilterDisables(false);
    auto* tree = new QTreeView;
    tree->setObjectName(QStringLiteral("soundLibrary"));
    tree->setModel(model);
    tree->setRootIndex(model->setRootPath(soundLibraryDir()));
    for (int c = 1; c < model->columnCount(); ++c)
        tree->hideColumn(c);
    tree->header()->hide();
    tree->setDragEnabled(true); // the model drags file URLs: the timeline imports them where they're dropped
    tree->setDragDropMode(QAbstractItemView::DragOnly);
    tree->setToolTip(tr("Double-click to listen. Drag onto the timeline, or Add at Cursor."));
    layout->addWidget(tree, 1);
    auto* row = new QHBoxLayout;
    auto* play = new QPushButton(tr("Play"));
    auto* add = new QPushButton(tr("Add at Cursor"));
    add->setObjectName(QStringLiteral("soundAddAtCursor"));
    auto* credits = new QPushButton(tr("Credits"));
    row->addWidget(play);
    row->addWidget(add);
    row->addStretch();
    row->addWidget(credits);
    layout->addLayout(row);
    auto* note = new QLabel(tr("Free to use in any video, commercial too: every sound is public domain (CC0)."));
    note->setWordWrap(true);
    layout->addWidget(note);

    auto* player = new QMediaPlayer(page);
    player->setAudioOutput(new QAudioOutput(page));
    auto selected = [=] {
        const QModelIndex i = tree->currentIndex();
        return i.isValid() && !model->isDir(i) ? model->filePath(i) : QString();
    };
    auto listen = [=] {
        const QString path = selected();
        if (path.isEmpty())
            return;
        player->stop();
        player->setSource(QUrl::fromLocalFile(path));
        player->play();
    };
    connect(tree, &QTreeView::activated, page, listen);
    connect(play, &QPushButton::clicked, page, listen);
    connect(add, &QPushButton::clicked, this, [this, selected] {
        const QString path = selected();
        if (!path.isEmpty() && editorActive())
            importFiles({path}, m_timeline->cursorTime());
    });
    connect(credits, &QPushButton::clicked, page,
            [] { QDesktopServices::openUrl(QUrl::fromLocalFile(QDir(soundLibraryDir()).filePath(QStringLiteral("CREDITS.md")))); });
    connect(search, &QLineEdit::textChanged, page, [=](const QString& text) {
        model->setNameFilters({text.trimmed().isEmpty() ? QStringLiteral("*.ogg") : QStringLiteral("*%1*.ogg").arg(text.trimmed())});
        if (!text.trimmed().isEmpty())
            tree->expandAll();
    });
    return page;
}
