#include "core/Brand.h"
// Help > User Guide and Tutorials: the Markdown pages of docs/ (copied next to
// the program) in a window of their own. A link to an example project (.mtg)
// opens a copy of it in Documents/Mutant Video Producer Examples, so the original stays as it is.

#include "ui/MainWindow.h"

#include "ui/Help.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMessageBox>
#include <QPushButton>
#include <QAbstractTextDocumentLayout>
#include <QStandardPaths>
#include <QTextBrowser>
#include <QScrollBar>
#include <QShortcut>
#include <QTextBlock>
#include <QToolBar>
#include <QVBoxLayout>

namespace {

// Pictures no wider than the page; links to a heading ("#auto-ripple") scroll to it.
class HelpBrowser : public QTextBrowser {
public:
    QVariant loadResource(int type, const QUrl& name) override;

protected:
    void doSetSource(const QUrl& url, QTextDocument::ResourceType type) override
    {
        QTextBrowser::doSetSource(url, type);
        if (!url.hasFragment())
            return;
        // Markdown headings have no anchors of their own: find the one whose GitHub-style
        // anchor (lower case, spaces as hyphens, other punctuation dropped) is the fragment.
        for (QTextBlock b = document()->begin(); b.isValid(); b = b.next()) {
            if (b.blockFormat().headingLevel() == 0)
                continue;
            QString anchor;
            for (const QChar c : b.text().trimmed().toLower())
                if (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'))
                    anchor += c;
                else if (c == QLatin1Char(' '))
                    anchor += QLatin1Char('-');
            if (anchor == url.fragment()) {
                verticalScrollBar()->setValue(int(document()->documentLayout()->blockBoundingRect(b).top()));
                return;
            }
        }
    }
};

QVariant HelpBrowser::loadResource(int type, const QUrl& name)
{
    QVariant v = QTextBrowser::loadResource(type, name);
    if (type != QTextDocument::ImageResource)
        return v;
    QImage img = v.typeId() == QMetaType::QImage ? v.value<QImage>() : QImage::fromData(v.toByteArray());
    const int most = std::max(320, viewport()->width() - 40);
    if (img.width() > most)
        img = img.scaledToWidth(most, Qt::SmoothTransformation);
    return img;
}

} // namespace

QString MainWindow::docsPath(const QString& page)
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("docs/") + page);
}

void MainWindow::showHelp(const QString& page)
{
    // From a dialog (a modal one blocks every other window), the help window is the dialog's own.
    QWidget* owner = QApplication::activeModalWidget();
    if (!owner)
        owner = this;
    auto* window = owner->findChild<QWidget*>(QStringLiteral("helpWindow"), Qt::FindDirectChildrenOnly);
    auto* browser = window ? window->findChild<QTextBrowser*>(QStringLiteral("helpBrowser")) : nullptr;
    if (!browser) {
        window = new QWidget(owner, Qt::Window);
        window->setObjectName(QStringLiteral("helpWindow"));
        window->setAttribute(Qt::WA_DontShowOnScreen, testAttribute(Qt::WA_DontShowOnScreen)); // (off screen like the test runs)
        window->setWindowTitle(tr("Mutant Video Producer Help"));
        window->resize(960, 800);
        browser = new HelpBrowser;
        browser->setObjectName(QStringLiteral("helpBrowser"));
        browser->setOpenLinks(false);
        auto* bar = new QToolBar;
        QAction* back = bar->addAction(tr("Back"), browser, &QTextBrowser::backward);
        QAction* forward = bar->addAction(tr("Forward"), browser, &QTextBrowser::forward);
        bar->addAction(tr("Contents"), browser, [browser] { browser->setSource(QUrl::fromLocalFile(docsPath(QStringLiteral("README.md")))); });
        bar->addAction(tr("Tutorials"), browser, [browser] { browser->setSource(QUrl::fromLocalFile(docsPath(QStringLiteral("tutorials/README.md")))); });
        back->setEnabled(false);
        forward->setEnabled(false);
        connect(browser, &QTextBrowser::backwardAvailable, back, &QAction::setEnabled);
        connect(browser, &QTextBrowser::forwardAvailable, forward, &QAction::setEnabled);
        connect(browser, &QTextBrowser::anchorClicked, this, [this, browser](const QUrl& url) {
            if (url.path().endsWith(QLatin1String(".mtg")))
                openExample(QFileInfo(url.path()).completeBaseName());
            else if (url.scheme().startsWith(QLatin1String("http")))
                QDesktopServices::openUrl(url);
            else
                browser->setSource(url);
        });
        auto* layout = new QVBoxLayout(window);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        layout->addWidget(bar);
        layout->addWidget(browser);
    }
    if (owner == this)
        m_helpBrowser = browser;
    const QString path = docsPath(page.section(QLatin1Char('#'), 0, 0)); // ("editing.md#markers-and-regions")
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(owner, tr("Mutant Video Producer Help"), tr("The help pages are missing (%1).").arg(QDir::toNativeSeparators(path)));
        return;
    }
    QUrl url = QUrl::fromLocalFile(path);
    url.setFragment(page.section(QLatin1Char('#'), 1));
    browser->setSource(url);
    window->show();
    window->raise();
    window->activateWindow();
}

namespace help {

void open(const QString& page)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* main = qobject_cast<MainWindow*>(w)) {
            main->showHelp(page);
            return;
        }
}

void setPage(QWidget* w, const QString& page)
{
    w->setProperty("helpPage", page);
}

QString pageFor(const QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (const QString page = w->property("helpPage").toString(); !page.isEmpty())
            return page;
    return QStringLiteral("README.md");
}

void addButton(QDialogButtonBox* box, const QString& page)
{
    box->addButton(QDialogButtonBox::Help);
    QObject::connect(box, &QDialogButtonBox::helpRequested, box, [page] { open(page); });
    auto* key = new QShortcut(QKeySequence(QStringLiteral("Shift+F1")), box, [page] { open(page); });
    key->setContext(Qt::WindowShortcut);
    setPage(box->parentWidget() ? box->parentWidget() : box, page);
}

} // namespace help

void MainWindow::openExample(const QString& name, const QString& into)
{
    const QDir from(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("examples")));
    const QString original = from.filePath(name + QStringLiteral(".mtg"));
    if (!QFileInfo::exists(original)) {
        QMessageBox::warning(this, tr("Example Projects"), tr("The example %1 is missing.").arg(QDir::toNativeSeparators(original)));
        return;
    }
    const QDir to(into.isEmpty() ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Examples"))
                                 : into);
    QDir().mkpath(to.filePath(QStringLiteral("media")));
    for (const QFileInfo& f : QDir(from.filePath(QStringLiteral("media"))).entryInfoList(QDir::Files))
        QFile::copy(f.filePath(), to.filePath(QStringLiteral("media/") + f.fileName())); // (once: an existing file is kept)
    const QString copy = to.filePath(name + QStringLiteral(".mtg"));
    if (QFileInfo::exists(copy) && into.isEmpty()) {
        // An earlier copy may hold your changes.
        QMessageBox box(QMessageBox::Question, tr("Example Projects"),
                        tr("You have opened this example before. Open your copy, or start again from the original?"), QMessageBox::Cancel, this);
        QPushButton* mine = box.addButton(tr("Open My Copy"), QMessageBox::AcceptRole);
        QPushButton* again = box.addButton(tr("Start Again"), QMessageBox::DestructiveRole);
        box.exec();
        if (box.clickedButton() == again)
            QFile::remove(copy);
        else if (box.clickedButton() != mine)
            return;
    }
    if (!QFileInfo::exists(copy)) {
        QFile::copy(original, copy);
        QFile(copy).setPermissions(QFile(copy).permissions() | QFileDevice::WriteOwner);
    }
    if (maybeSave())
        openProject(copy);
}
