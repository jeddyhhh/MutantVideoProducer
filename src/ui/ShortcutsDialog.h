#pragma once

#include <QDialog>
#include <QList>

class QAction;
class QTableWidget;

// Options > Keyboard Shortcuts: every command of the menus (and the window's
// own keys) with its shortcut, changed by double-clicking it. Changes are kept
// in the settings ("shortcuts/<command>") and applied at start-up.
class ShortcutsDialog : public QDialog {
public:
    ShortcutsDialog(const QList<QAction*>& actions, QWidget* parent = nullptr);
    // Remembers each action's own shortcut, then applies the saved ones.
    static void applySaved(const QList<QAction*>& actions);
    // The commands of a window: its menus' actions (submenus too) and its own.
    static QList<QAction*> commands(class QMainWindow* window);

private:
    void save();
    QList<QAction*> m_actions;
    QTableWidget* m_table;
};
