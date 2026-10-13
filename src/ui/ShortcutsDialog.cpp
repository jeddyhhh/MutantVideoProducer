#include "ui/ShortcutsDialog.h"

#include "ui/Help.h"

#include <QAction>
#include <functional>
#include <QDialogButtonBox>
#include <QHeaderView>
#include <QKeySequenceEdit>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// A command's name in the settings: its menu text without '&' and "...".
QString commandName(const QAction* a)
{
    return a->text().remove(QLatin1Char('&')).remove(QStringLiteral("...")).trimmed();
}

QKeySequence defaultShortcut(const QAction* a)
{
    return a->property("defaultShortcut").value<QKeySequence>();
}

} // namespace

QList<QAction*> ShortcutsDialog::commands(QMainWindow* window)
{
    QList<QAction*> list;
    std::function<void(const QList<QAction*>&)> walk = [&](const QList<QAction*>& actions) {
        for (QAction* a : actions) {
            if (a->menu())
                walk(a->menu()->actions());
            else if (!a->isSeparator() && !a->text().isEmpty() && !list.contains(a))
                list << a;
        }
    };
    walk(window->menuBar()->actions());
    walk(window->actions());
    return list;
}

void ShortcutsDialog::applySaved(const QList<QAction*>& actions)
{
    QSettings settings;
    for (QAction* a : actions) {
        if (!a->property("defaultShortcut").isValid())
            a->setProperty("defaultShortcut", a->shortcut());
        const QString key = QStringLiteral("shortcuts/") + commandName(a);
        a->setShortcut(settings.contains(key) ? QKeySequence(settings.value(key).toString()) : defaultShortcut(a));
    }
}

ShortcutsDialog::ShortcutsDialog(const QList<QAction*>& actions, QWidget* parent) : QDialog(parent), m_actions(actions)
{
    setWindowTitle(tr("Keyboard Shortcuts"));
    resize(560, 640);
    auto* v = new QVBoxLayout(this);
    auto* search = new QLineEdit;
    search->setPlaceholderText(tr("Search commands or keys"));
    search->setClearButtonEnabled(true);
    m_table = new QTableWidget(int(actions.size()), 2);
    m_table->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut (double-click to change)")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    for (int i = 0; i < int(actions.size()); ++i) {
        m_table->setItem(i, 0, new QTableWidgetItem(commandName(actions[i])));
        m_table->setItem(i, 1, new QTableWidgetItem(actions[i]->shortcut().toString(QKeySequence::NativeText)));
        m_table->item(i, 1)->setData(Qt::UserRole, actions[i]->shortcut().toString());
    }
    v->addWidget(search);
    v->addWidget(m_table, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("shortcuts.md"));
    QPushButton* reset = buttons->addButton(tr("Reset All"), QDialogButtonBox::ResetRole);
    v->addWidget(buttons);
    auto setKey = [this](int row, const QKeySequence& key) {
        m_table->item(row, 1)->setText(key.toString(QKeySequence::NativeText));
        m_table->item(row, 1)->setData(Qt::UserRole, key.toString());
    };
    connect(search, &QLineEdit::textChanged, this, [this](const QString& text) {
        for (int i = 0; i < m_table->rowCount(); ++i)
            m_table->setRowHidden(i, !text.isEmpty() && !m_table->item(i, 0)->text().contains(text, Qt::CaseInsensitive)
                                         && !m_table->item(i, 1)->text().contains(text, Qt::CaseInsensitive));
    });
    connect(m_table, &QTableWidget::cellDoubleClicked, this, [this, setKey](int row) {
        QDialog ask(this);
        ask.setWindowTitle(m_table->item(row, 0)->text());
        auto* l = new QVBoxLayout(&ask);
        auto* edit = new QKeySequenceEdit(QKeySequence(m_table->item(row, 1)->data(Qt::UserRole).toString()));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        edit->setMaximumSequenceLength(1);
#else // (one key: the first of what was typed)
        connect(edit, &QKeySequenceEdit::keySequenceChanged, edit, [edit](const QKeySequence& s) {
            if (s.count() > 1)
                edit->setKeySequence(QKeySequence(s[0]));
        });
#endif
        auto* b = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        QPushButton* none = b->addButton(tr("None"), QDialogButtonBox::ResetRole);
        l->addWidget(edit);
        l->addWidget(b);
        connect(b, &QDialogButtonBox::accepted, &ask, &QDialog::accept);
        connect(b, &QDialogButtonBox::rejected, &ask, &QDialog::reject);
        connect(none, &QPushButton::clicked, edit, &QKeySequenceEdit::clear);
        edit->setFocus();
        if (ask.exec() != QDialog::Accepted)
            return;
        const QKeySequence key = edit->keySequence();
        for (int i = 0; i < m_table->rowCount() && !key.isEmpty(); ++i) // a key does one thing
            if (i != row && QKeySequence(m_table->item(i, 1)->data(Qt::UserRole).toString()) == key) {
                if (QMessageBox::question(this, tr("Keyboard Shortcuts"),
                                          tr("%1 is used by \"%2\". Move it here?").arg(key.toString(QKeySequence::NativeText), m_table->item(i, 0)->text()))
                    != QMessageBox::Yes)
                    return;
                setKey(i, QKeySequence());
            }
        setKey(row, key);
    });
    connect(reset, &QPushButton::clicked, this, [this, setKey] {
        for (int i = 0; i < int(m_actions.size()); ++i)
            setKey(i, defaultShortcut(m_actions[i]));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ShortcutsDialog::save()
{
    QSettings settings;
    for (int i = 0; i < int(m_actions.size()); ++i) {
        QAction* a = m_actions[i];
        const QKeySequence key(m_table->item(i, 1)->data(Qt::UserRole).toString());
        const QString name = QStringLiteral("shortcuts/") + commandName(a);
        if (key == defaultShortcut(a))
            settings.remove(name);
        else
            settings.setValue(name, key.toString());
        a->setShortcut(key);
    }
}
