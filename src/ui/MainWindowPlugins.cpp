// Tools > Plugins: the script plugins' commands (Plugins.h), filled each time the menu opens.
#include "ui/MainWindow.h"

#include "core/Project.h"
#include "ui/McpServer.h"
#include "ui/Plugins.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QTimer>
#include <QUndoStack>
#include <QUrl>

void MainWindow::fillPluginsMenu(QMenu* menu)
{
    menu->clear();
    QStringList problems;
    const std::vector<plugins::Plugin> list = plugins::load(&problems);
    for (const plugins::Plugin& p : list)
        for (int i = 0; i < int(p.commands.size()); ++i) {
            QAction* a = menu->addAction(p.commands[size_t(i)].label);
            a->setObjectName(QStringLiteral("plugin:%1:%2").arg(p.id).arg(i));
            a->setToolTip(QStringLiteral("%1 %2%3").arg(p.name, p.version, p.author.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(p.author)));
            // (after the menu has closed: a command refuses to edit while a menu is open)
            connect(a, &QAction::triggered, this, [this, p, i] { QTimer::singleShot(0, this, [this, p, i] { runPluginCommand(p, i); }); });
        }
    if (list.empty())
        menu->addAction(tr("(no plugins)"))->setEnabled(false);
    if (!problems.isEmpty())
        menu->addAction(tr("Plugins That Didn't Load (%1)...").arg(problems.size()), this, [this, problems] {
            QMessageBox::warning(this, tr("Plugins"), problems.join(QLatin1Char('\n')));
        });
    menu->addSeparator();
    QAction* folder = menu->addAction(tr("Open Plugins Folder"), this, [] {
        QDir().mkpath(plugins::userFolder());
        QDesktopServices::openUrl(QUrl::fromLocalFile(plugins::userFolder()));
    });
    folder->setObjectName(QStringLiteral("pluginsFolder"));
    menu->addAction(tr("Writing Plugins..."), this, [this] { showHelp(QStringLiteral("plugins.md")); });
}

void MainWindow::runPluginCommand(const plugins::Plugin& plugin, int command)
{
    if (!plugin.bundled) {
        // A plugin is code from someone else: run only once the user says so, and again after it changes.
        const QString key = QStringLiteral("plugins/trusted/%1").arg(plugin.id), print = plugins::fingerprint(plugin);
        if (QSettings().value(key).toString() != print) {
            const QString changed = QSettings().contains(key) ? tr("\n\nIt has changed since you last ran it.") : QString();
            if (QMessageBox::question(this, tr("Run Plugin"),
                                      tr("Run \"%1\" %2%3?\n\n%4\n\nA plugin can change the open project, save and render files and do "
                                         "anything else AI Control can. Only run plugins from people you trust.%5")
                                          .arg(plugin.name, plugin.version,
                                               plugin.author.isEmpty() ? QString() : tr(" by %1").arg(plugin.author),
                                               QDir::toNativeSeparators(plugin.folder), changed))
                != QMessageBox::Yes)
                return;
            QSettings().setValue(key, print);
        }
    }
    QJsonObject args;
    QStringList files;
    if (!plugins::ask(this, plugin, command, &args, &files))
        return;
    QStringList log;
    const QString error = runPlugin(plugin, command, args, files, &log);
    if (!error.isEmpty())
        QMessageBox::warning(this, plugin.name,
                             tr("%1\n\n%2Anything it changed before that is one step in Edit > Undo.")
                                 .arg(error, log.isEmpty() ? QString() : log.join(QLatin1Char('\n')) + QStringLiteral("\n\n")));
    else
        statusBar()->showMessage(log.isEmpty() ? tr("%1: done").arg(plugin.commands[size_t(command)].label) : log.last(), 6000);
}

QString MainWindow::runPlugin(const plugins::Plugin& plugin, int command, const QJsonObject& args, const QStringList& files,
                              QStringList* log)
{
    if (editorActive())
        stopPlayback(false);
    statusBar()->showMessage(tr("Plugin: %1...").arg(plugin.commands[size_t(command)].label));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    // Everything it does is one undo step.
    m_project->undoStack()->beginMacro(tr("Plugin: %1").arg(QString(plugin.commands[size_t(command)].label).remove(QStringLiteral("..."))));
    const QString error = plugins::run(*m_mcp, plugin, command, args, files, log);
    m_project->undoStack()->endMacro();
    QApplication::restoreOverrideCursor();
    statusBar()->clearMessage();
    return error;
}

QString MainWindow::runPluginForTests(const QString& id, int command, const QJsonObject& args, const QStringList& files)
{
    for (const plugins::Plugin& p : plugins::load())
        if (p.id == id && command < int(p.commands.size()))
            return runPlugin(p, command, args, files, nullptr);
    return QStringLiteral("no plugin %1").arg(id);
}
