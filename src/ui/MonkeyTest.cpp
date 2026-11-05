#include "ui/MonkeyTest.h"

#include "core/Project.h"
#include "ui/CrashReport.h"
#include "ui/LiveView.h"
#include "ui/MainWindow.h"
#include "ui/TimelineWidget.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QPushButton>
#include <QProgressDialog>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QHash>
#include <QTabBar>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QFile>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <atomic>
#include <chrono>
#include <memory>
#include <cstdlib>
#include <random>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// What the monkey must not do: leave the test (quit, open a browser or
// Explorer, download, go online), touch the user's things (cameras, the
// Documents folder's examples), or start long jobs that only wait (export,
// transcribing). Matched against an action's or button's text, name and tip.
const QRegularExpression& offLimits()
{
    static const QRegularExpression re(
        QStringLiteral("quit|exit|youtube|update|github|report|download|install|stream|go live|record|camera|capture|webcam|"
                       "mcp|ai control|example|tutorial|welcome|render|export|explorer|containing|reveal|show in|website|"
                       "online|transcrib|caption|restart|live"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

// On the Live tab: its own commands and recording (into the test's folder) are
// fair game; going on air and the virtual camera (other apps see it) are not.
const QRegularExpression& liveOffLimits()
{
    static const QRegularExpression re(
        QStringLiteral("quit|exit|youtube|update|github|report|download|install|stream|go live|on air|rtmp|srt|camera|webcam|obs|ndi|"
                       "mcp|ai control|example|tutorial|welcome|render|export|explorer|containing|reveal|show in|website|"
                       "online|transcrib|caption|restart"),
        QRegularExpression::CaseInsensitiveOption);
    return re;
}

bool g_liveMonkey = false; // the Live tab, not the editor

bool isOffLimits(const QString& text, const QString& name, const QString& tip)
{
    const QRegularExpression& re = g_liveMonkey ? liveOffLimits() : offLimits();
    const QString t = QString(text).remove(QLatin1Char('&'));
    return re.match(t).hasMatch() || re.match(name).hasMatch() || re.match(tip).hasMatch();
}

bool insideLive(const QObject* o)
{
    for (; o; o = o->parent())
        if (qobject_cast<const LiveView*>(o))
            return true;
    return false;
}

// Every window that appears stays off the screen (the user is working).
class OffScreen : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() == QEvent::Polish || e->type() == QEvent::Show)
            if (auto* w = qobject_cast<QWidget*>(o); w && w->isWindow() && !w->testAttribute(Qt::WA_DontShowOnScreen))
                w->setAttribute(Qt::WA_DontShowOnScreen, true);
        return false;
    }
};

class Monkey : public QObject {
    Q_OBJECT
public:
    Monkey(MainWindow& window, const QString& logPath, int steps, unsigned seed)
        : QObject(&window), m_window(window), m_steps(steps), m_rng(seed), m_log(logPath)
    {
        m_log.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate);
        write(QStringLiteral("seed %1, %2 steps").arg(seed).arg(steps));
        m_project = window.findChild<Project*>();
        m_timeline = window.findChild<TimelineWidget*>();
        qApp->installEventFilter(new OffScreen(this));
        // Links and files the app would open go nowhere.
        for (const char* scheme : {"http", "https", "file", "mailto"})
            QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), this, "openUrl");
        // A hang: no step for a minute. (The watch has its own state: it outlives this.)
        m_watch->lastStep = now();
        m_watch->logPath = logPath;
#ifdef Q_OS_WIN
        m_watch->mainThread = GetCurrentThreadId();
#endif
        std::thread([watch = m_watch] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (watch->done)
                    return;
                if (now() - watch->lastStep > 60000) {
                    QFile log(watch->logPath);
                    log.open(QIODevice::Append | QIODevice::Text);
                    log.write("FAIL  HANG: nothing happened for a minute after the step above\n");
#ifdef Q_OS_WIN
                    log.write(crash::reportHang(watch->mainThread).toUtf8());
#endif
                    log.close();
                    std::_Exit(3);
                }
            }
        }).detach();
        // Dialogs and menus get an answer.
        auto* answer = new QTimer(this);
        connect(answer, &QTimer::timeout, this, &Monkey::answerDialogs);
        answer->start(40);
        QTimer::singleShot(1500, this, [this] { // once the project given has opened
            if (m_project)
                m_projectPath = m_project->filePath();
            step();
        });
    }

    ~Monkey() override
    {
        m_watch->done = true;
        for (const char* scheme : {"http", "https", "file", "mailto"})
            QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
    }

private:
    static int64_t now()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void write(const QString& line)
    {
        m_log.write(line.toUtf8() + '\n');
        m_log.flush();
    }

    void fail(const QString& what)
    {
        ++m_failures;
        write(QStringLiteral("FAIL  ") + what);
    }

    int pick(int n) { return n > 0 ? int(m_rng() % unsigned(n)) : 0; }

    // Widgets the monkey may poke (shown, enabled, on its tab).
    QList<QWidget*> pokeable()
    {
        QList<QWidget*> out;
        for (QWidget* w : m_window.findChildren<QWidget*>())
            if (w->isVisible() && w->isEnabled() && w->width() > 2 && w->height() > 2 && insideLive(w) == g_liveMonkey)
                out << w;
        return out;
    }

    // Drags only where the widget follows the mouse itself: elsewhere a drag
    // starts a real drag and drop, which waits for the real mouse.
    static bool draggable(const QWidget* w)
    {
        return qobject_cast<const QAbstractSlider*>(w) || w->inherits("Knob") || w->inherits("PanCropView") || w->inherits("PreviewWidget")
               || w->inherits("QOpenGLWidget") || w->inherits("AudioMeter");
    }

    QList<QAction*> actions()
    {
        QList<QAction*> out;
        for (QAction* a : m_window.findChildren<QAction*>())
            if (a->isEnabled() && !a->isSeparator() && !a->menu() && (g_liveMonkey || !insideLive(a))
                && !isOffLimits(a->text(), a->objectName(), a->toolTip()))
                out << a;
        return out;
    }

    void mouse(QWidget* w, bool drag)
    {
        auto point = [&] { return QPointF(pick(w->width()), pick(w->height())); };
        const Qt::KeyboardModifiers mods = std::array<Qt::KeyboardModifiers, 5>{Qt::NoModifier, Qt::NoModifier, Qt::ShiftModifier, Qt::ControlModifier,
                                                      Qt::AltModifier}[size_t(pick(5))];
        const Qt::MouseButton button = pick(10) == 0 ? Qt::RightButton : Qt::LeftButton;
        QPointF p = point();
        QStringList what; // for the log: what was done, where
        what << QStringLiteral("%1%2").arg(button == Qt::RightButton ? QStringLiteral("right") : QStringLiteral("left"),
                                            mods == Qt::ShiftModifier ? QStringLiteral("+shift") : mods == Qt::ControlModifier ? QStringLiteral("+ctrl")
                                            : mods == Qt::AltModifier ? QStringLiteral("+alt") : QString());
        // (a press can open a dialog, and closing it can rebuild the panel the widget was on: the rest of the gesture
        // then goes nowhere, as the real mouse's would)
        const QPointer<QWidget> alive(w);
        auto send = [&](QEvent::Type type, QPointF at, Qt::MouseButton b, Qt::MouseButtons held) {
            if (!alive)
                return;
            QMouseEvent e(type, at, w->mapToGlobal(at), b, held, mods);
            QApplication::sendEvent(w, &e);
        };
        if (pick(6) == 0) {
            send(QEvent::MouseButtonPress, p, button, button);
            send(QEvent::MouseButtonRelease, p, button, Qt::NoButton);
            send(QEvent::MouseButtonDblClick, p, button, button);
            send(QEvent::MouseButtonRelease, p, button, Qt::NoButton);
            write(QStringLiteral("    %1 double-click at %2,%3").arg(what.join(QString())).arg(p.x()).arg(p.y()));
            return;
        }
        send(QEvent::MouseButtonPress, p, button, button);
        what << QStringLiteral(" at %1,%2").arg(p.x()).arg(p.y());
        if (drag && button == Qt::LeftButton)
            for (int i = 0, n = 1 + pick(5); i < n && alive; ++i) {
                p = point();
                send(QEvent::MouseMove, p, Qt::NoButton, button);
                what << QStringLiteral(" > %1,%2").arg(p.x()).arg(p.y());
            }
        send(QEvent::MouseButtonRelease, p, button, Qt::NoButton);
        write(QStringLiteral("    ") + what.join(QString()));
    }

    void wheel(QWidget* w)
    {
        const QPointF p(pick(w->width()), pick(w->height()));
        const Qt::KeyboardModifiers mods = std::array<Qt::KeyboardModifiers, 4>{Qt::NoModifier, Qt::ControlModifier, Qt::ShiftModifier, Qt::AltModifier}[size_t(pick(4))];
        QWheelEvent e(p, w->mapToGlobal(p), {}, QPoint(0, pick(2) ? 120 : -120), Qt::NoButton, mods, Qt::NoScrollPhase, false);
        QApplication::sendEvent(w, &e);
    }

    void key(QWidget* w)
    {
        static const std::array keys = {Qt::Key_Space, Qt::Key_S, Qt::Key_Delete, Qt::Key_Backspace, Qt::Key_Left, Qt::Key_Right,
                                        Qt::Key_Up, Qt::Key_Down, Qt::Key_Home, Qt::Key_End, Qt::Key_J, Qt::Key_K, Qt::Key_L,
                                        Qt::Key_M, Qt::Key_R, Qt::Key_C, Qt::Key_V, Qt::Key_X, Qt::Key_A, Qt::Key_Z, Qt::Key_BracketLeft,
                                        Qt::Key_BracketRight, Qt::Key_Plus, Qt::Key_Minus, Qt::Key_Return, Qt::Key_Escape, Qt::Key_Tab,
                                        Qt::Key_PageUp, Qt::Key_PageDown, Qt::Key_1, Qt::Key_0, Qt::Key_I, Qt::Key_O, Qt::Key_G};
        const Qt::Key k = keys[size_t(pick(int(keys.size())))];
        const Qt::KeyboardModifiers mods = std::array<Qt::KeyboardModifiers, 6>{Qt::NoModifier, Qt::NoModifier, Qt::ShiftModifier, Qt::ControlModifier,
                                                      Qt::ControlModifier | Qt::ShiftModifier, Qt::AltModifier}[size_t(pick(6))];
        const QString text = (mods & ~Qt::ShiftModifier) || k >= 0x01000000 ? QString() : QString(QChar(k)).toLower();
        QKeyEvent press(QEvent::KeyPress, k, mods, text);
        QKeyEvent release(QEvent::KeyRelease, k, mods, text);
        QApplication::sendEvent(w, &press);
        QApplication::sendEvent(w, &release);
    }

    void step()
    {
        m_watch->lastStep = now();
        if (m_index >= m_steps) {
            finish();
            return;
        }
        ++m_index;
        // Its tab, with the project back now and then (New and Close empty it).
        if (auto* modes = m_window.findChild<QTabWidget*>(QStringLiteral("modeTabs")); modes && modes->currentIndex() != int(g_liveMonkey))
            modes->setCurrentIndex(int(g_liveMonkey));
        if (m_index % 400 == 0 && !m_projectPath.isEmpty()) {
            write(QStringLiteral("%1 reopen the project").arg(m_index));
            m_window.openProject(m_projectPath);
        }
        // On the Live tab, the picture runs (Start) most of the time.
        if (g_liveMonkey && (m_index == 1 || m_index % 150 == 0))
            if (auto* start = m_window.findChild<QPushButton*>(QStringLiteral("liveStart"));
                start && start->isEnabled() && start->text().contains(QLatin1String("Start"), Qt::CaseInsensitive)) {
                write(QStringLiteral("%1 Start").arg(m_index));
                start->click();
            }
        const int what = g_liveMonkey && pick(3) ? 80 + pick(20) : pick(100); // Live: mostly its own widgets
        if (what < 6 && !g_liveMonkey) {
            openEditor();
        } else if (what < 40) {
            const QList<QAction*> list = actions();
            if (!list.isEmpty()) {
                QAction* a = list[pick(int(list.size()))];
                write(QStringLiteral("%1 action \"%2\"").arg(m_index).arg(QString(a->text()).remove(QLatin1Char('&'))));
                a->trigger();
            }
        } else if (what < 55 && g_liveMonkey) { // keys on the Live tab (pads, scenes, Space...)
            if (auto* live = m_window.findChild<LiveView*>()) {
                write(QStringLiteral("%1 live key").arg(m_index));
                key(live);
            }
        } else if (what < 65 && m_timeline && !g_liveMonkey) {
            QWidget* v = m_timeline->viewport();
            write(QStringLiteral("%1 timeline mouse").arg(m_index));
            mouse(v, true);
        } else if (what < 75 && m_timeline && !g_liveMonkey) {
            write(QStringLiteral("%1 timeline key").arg(m_index));
            key(m_timeline);
        } else if (what < 80 && m_timeline && !g_liveMonkey) {
            write(QStringLiteral("%1 timeline wheel").arg(m_index));
            wheel(m_timeline->viewport());
        } else {
            const QList<QWidget*> list = pokeable();
            if (!list.isEmpty()) {
                QWidget* w = list[pick(int(list.size()))];
                if (auto* b = qobject_cast<QAbstractButton*>(w)) {
                    const auto* tb = qobject_cast<QToolButton*>(w);
                    const QAction* d = tb ? tb->defaultAction() : nullptr;
                    if (isOffLimits(b->text(), b->objectName(), b->toolTip())
                        || (d && isOffLimits(d->text(), d->objectName(), d->toolTip()))) {
                        QTimer::singleShot(1, this, &Monkey::step);
                        return;
                    }
                }
                write(QStringLiteral("%1 poke %2 \"%3\"").arg(m_index).arg(QString::fromLatin1(w->metaObject()->className()), w->objectName()));
                const int how = pick(10);
                if (how < 6)
                    mouse(w, draggable(w));
                else if (how < 8)
                    wheel(w);
                else
                    key(w);
            }
        }
        if (m_project) { // (each new problem once)
            const QString p = timelineProblem(m_project->timeline());
            if (!p.isEmpty() && p != m_problem)
                fail(QStringLiteral("after step %1 the timeline is broken: %2").arg(m_index).arg(p));
            m_problem = p;
        }
        checkScreen();
        QTimer::singleShot(pick(3) == 0 ? 30 : 1, this, &Monkey::step);
    }

    // Answers what pops up: a menu runs one of its commands or closes, a
    // message box gets a random button, other dialogs are accepted or cancelled.
    void answerDialogs()
    {
        checkScreen();
        QWidget* popup = QApplication::activePopupWidget();
        if (!popup) // Qt 6.8+ never makes an off-screen (WA_DontShowOnScreen) popup the active one
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->windowType() == Qt::Popup)
                    popup = w;
        if (auto* menu = qobject_cast<QMenu*>(popup)) {
            QList<QAction*> usable;
            for (QAction* a : menu->actions())
                if (a->isEnabled() && !a->isSeparator() && !a->menu() && !isOffLimits(a->text(), a->objectName(), a->toolTip()))
                    usable << a;
            if (!usable.isEmpty() && pick(2)) {
                QAction* a = usable[pick(int(usable.size()))];
                write(QStringLiteral("    menu \"%1\"").arg(QString(a->text()).remove(QLatin1Char('&'))));
                menu->close();
                // From a fresh event: a dialog it opens is then answered (this timer
                // doesn't fire again while it's still running).
                QTimer::singleShot(0, a, &QAction::trigger);
            } else {
                menu->close();
            }
            return;
        }
        if (popup) {
            popup->close();
            return;
        }
        QWidget* modal = QApplication::activeModalWidget();
        if (!modal)
            return;
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            const QList<QAbstractButton*> buttons = box->buttons();
            QAbstractButton* b = buttons.isEmpty() ? nullptr : buttons[pick(int(buttons.size()))];
            write(QStringLiteral("    message \"%1\": %2").arg(box->text().left(80), b ? b->text() : QStringLiteral("(closed)")));
            if (b)
                b->click();
            else
                box->close();
            return;
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            // Used for a while first: before, a dialog was answered at once, so nothing inside one (Compose Graphics, the
            // effect editors, Narration Studio...) was ever clicked at random, and #7 went unseen.
            // (Qt's own colour and font pickers start a real drag and drop on a press and a move, which waits for the real
            // mouse: they are only answered.)
            if (!dialog->inherits("QFileDialog") && !dialog->inherits("QProgressDialog") && !dialog->inherits("QColorDialog")
                && !dialog->inherits("QFontDialog")) {
                if (!m_dialogMoves.contains(dialog)) {
                    m_dialogMoves[dialog] = 4 + pick(40);
                    write(QStringLiteral("    dialog \"%1\" open: %2 moves").arg(dialog->windowTitle()).arg(m_dialogMoves[dialog]));
                }
                if (m_dialogMoves[dialog]-- > 0) {
                    m_watch->lastStep = now();
                    useDialog(dialog);
                    return;
                }
            }
            m_dialogMoves.remove(dialog);
            const bool accept = pick(3) == 0 && !isOffLimits(dialog->windowTitle(), dialog->objectName(), {})
                                && !dialog->inherits("QFileDialog");
            write(QStringLiteral("    dialog \"%1\": %2").arg(dialog->windowTitle(), accept ? QStringLiteral("OK") : QStringLiteral("Cancel")));
            if (accept)
                dialog->accept();
            else if (auto* progress = qobject_cast<QProgressDialog*>(dialog))
                progress->cancel(); // (as its Cancel button or Esc: reject() only hid it, the work going on unseen)
            else
                dialog->reject();
            return;
        }
        modal->close();
    }

    // One move inside a dialog: a dropdown choice (as the user's, with activated), list rows (several, with Ctrl and Shift),
    // a number, a slider, a tick, a tab, text (empty, long, odd), a button, or a click or drag anywhere (canvases).
    void useDialog(QDialog* dialog)
    {
        QList<QWidget*> widgets;
        for (QWidget* w : dialog->findChildren<QWidget*>()) {
            if (!w->isVisible() || !w->isEnabled() || w->width() <= 2 || w->height() <= 2 || w->window() != dialog)
                continue;
            bool inBox = false; // (its OK and Cancel close it: that comes after the moves)
            for (QObject* o = w; o && o != dialog; o = o->parent())
                inBox = inBox || qobject_cast<QDialogButtonBox*>(o);
            if (inBox)
                continue;
            if (auto* b = qobject_cast<QAbstractButton*>(w); b && isOffLimits(b->text(), b->objectName(), b->toolTip()))
                continue;
            widgets << w;
        }
        if (widgets.isEmpty())
            return;
        QWidget* w = widgets[pick(int(widgets.size()))];
        const QString name = QStringLiteral("%1 \"%2\"").arg(QString::fromLatin1(w->metaObject()->className()), w->objectName());
        static const QStringList texts = {QString(), QStringLiteral("0"), QStringLiteral("-1"), QStringLiteral("1e9"), QStringLiteral("hello"),
                                          QStringLiteral("#ff00ff"), QStringLiteral("{\"a\": [1, 2]}"), QString(300, QLatin1Char('x')),
                                          QString::fromUtf8("\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"), QStringLiteral("../..\\x")};
        if (auto* combo = qobject_cast<QComboBox*>(w); combo && combo->count() > 0) {
            const int k = pick(combo->count());
            write(QStringLiteral("      choose %1 in %2").arg(k).arg(name));
            combo->setCurrentIndex(k);
            emit combo->activated(k);
        } else if (auto* view = qobject_cast<QAbstractItemView*>(w->parentWidget()); view && w == view->viewport() && view->model()) {
            const int rows = view->model()->rowCount(view->rootIndex());
            if (rows == 0)
                return mouse(w, false);
            const QModelIndex index = view->model()->index(pick(rows), 0, view->rootIndex());
            const QRect r = view->visualRect(index);
            const Qt::KeyboardModifiers mods = std::array<Qt::KeyboardModifiers, 4>{Qt::NoModifier, Qt::ControlModifier, Qt::ShiftModifier, Qt::NoModifier}[size_t(pick(4))];
            write(QStringLiteral("      click row %1 of %2 in %3%4").arg(index.row()).arg(rows).arg(name, mods == Qt::ControlModifier ? QStringLiteral(" +ctrl")
                                                                                                       : mods == Qt::ShiftModifier ? QStringLiteral(" +shift") : QString()));
            if (r.isValid() && w->rect().intersects(r)) {
                const QPointF at = r.center();
                QMouseEvent press(QEvent::MouseButtonPress, at, w->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, mods);
                QMouseEvent release(QEvent::MouseButtonRelease, at, w->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, mods);
                QApplication::sendEvent(w, &press);
                QApplication::sendEvent(w, &release);
            } else {
                view->setCurrentIndex(index);
            }
        } else if (auto* spin = qobject_cast<QSpinBox*>(w)) {
            const int v = spin->minimum() + int(pick(10001) / 10000.0 * (double(spin->maximum()) - spin->minimum()));
            write(QStringLiteral("      set %1 to %2").arg(name).arg(v));
            spin->setValue(v);
        } else if (auto* dspin = qobject_cast<QDoubleSpinBox*>(w)) {
            const double v = dspin->minimum() + pick(10001) / 10000.0 * (dspin->maximum() - dspin->minimum());
            write(QStringLiteral("      set %1 to %2").arg(name).arg(v));
            dspin->setValue(v);
        } else if (auto* slider = qobject_cast<QAbstractSlider*>(w)) {
            const int v = slider->minimum() + int(pick(10001) / 10000.0 * (double(slider->maximum()) - slider->minimum()));
            write(QStringLiteral("      slide %1 to %2").arg(name).arg(v));
            slider->setValue(v);
        } else if (auto* line = qobject_cast<QLineEdit*>(w); line && !line->isReadOnly()) {
            const QString t = texts[pick(int(texts.size()))];
            write(QStringLiteral("      type %1 chars in %2").arg(t.size()).arg(name));
            line->setText(t);
            emit line->editingFinished();
        } else if (auto* edit = qobject_cast<QPlainTextEdit*>(w); edit && !edit->isReadOnly()) {
            const QString t = texts[pick(int(texts.size()))];
            write(QStringLiteral("      type %1 chars in %2").arg(t.size()).arg(name));
            edit->setPlainText(t);
        } else if (auto* tabs = qobject_cast<QTabBar*>(w); tabs && tabs->count() > 0) {
            const int k = pick(tabs->count());
            write(QStringLiteral("      tab %1 of %2").arg(k).arg(name));
            tabs->setCurrentIndex(k);
        } else if (auto* button = qobject_cast<QAbstractButton*>(w)) {
            write(QStringLiteral("      press %1 \"%2\"").arg(name, QString(button->text()).remove(QLatin1Char('&'))));
            QTimer::singleShot(0, button, &QAbstractButton::click); // (a dialog it opens is answered in turn)
        } else {
            write(QStringLiteral("      mouse in %1").arg(name));
            // Drags on our own canvases and editors; on Qt's widgets and ours that drag and drop, a click only.
            const bool ours = !QString::fromLatin1(w->metaObject()->className()).startsWith(QLatin1Char('Q')) && !w->inherits("DeviceChainView")
                              && !w->inherits("LivePadPanel") && !w->inherits("MediaBin") && !w->inherits("TimelineWidget");
            // Out of this timer's call, as a button press is: Return on the dialog presses its default button, and a
            // dialog that opens (a folder picker) is answered by this timer, which can't run again inside its own call.
            const int how = pick(10);
            const bool drag = ours || draggable(w);
            QTimer::singleShot(0, w, [this, w, how, drag] {
                if (how < 7)
                    mouse(w, drag);
                else if (how < 9)
                    wheel(w);
                else
                    key(w);
            });
        }
    }

    // An editor opened on purpose (not only by luck): an event selected, sometimes a new graphics overlay first, then one of
    // the window's "..." buttons, such as Compose Graphics... in Properties. Its dialog is then used as above.
    void openEditor()
    {
        if (!m_project || !m_timeline)
            return;
        if (pick(3) == 0)
            for (QAction* a : m_window.findChildren<QAction*>())
                if (QString(a->text()).remove(QLatin1Char('&')) == QLatin1String("Graphics Overlay") && a->isEnabled()) {
                    write(QStringLiteral("%1 insert a graphics overlay").arg(m_index));
                    a->trigger();
                    break;
                }
        std::vector<int> ids;
        for (const auto& t : m_project->timeline().tracks)
            for (const auto& c : t.clips)
                ids.push_back(c.id);
        if (ids.empty())
            return;
        const int id = ids[size_t(pick(int(ids.size())))];
        m_timeline->selectClips({id});
        QList<QPushButton*> buttons;
        for (QPushButton* b : m_window.findChildren<QPushButton*>())
            if (b->isVisible() && b->isEnabled() && (b->text().endsWith(QChar(0x2026)) || b->text().endsWith(QLatin1String("...")))
                && !isOffLimits(b->text(), b->objectName(), b->toolTip()) && insideLive(b) == g_liveMonkey)
                buttons << b;
        if (buttons.isEmpty())
            return;
        QPushButton* b = buttons[pick(int(buttons.size()))];
        write(QStringLiteral("%1 open \"%2\" on event %3").arg(m_index).arg(b->text()).arg(id));
        b->click();
    }

    // A window of ours on the user's screen is a failure (and is hidden).
    void checkScreen()
    {
#ifdef Q_OS_WIN
        struct Found { DWORD pid; QStringList titles; } found{GetCurrentProcessId(), {}};
        EnumWindows([](HWND h, LPARAM p) -> BOOL {
            auto* f = reinterpret_cast<Found*>(p);
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            if (pid == f->pid && IsWindowVisible(h)) {
                wchar_t title[200] = {};
                GetWindowTextW(h, title, 199);
                f->titles << QString::fromWCharArray(title);
                ShowWindow(h, SW_HIDE);
            }
            return TRUE;
        }, LPARAM(&found));
        if (!found.titles.isEmpty())
            fail(QStringLiteral("a window showed on the screen: %1").arg(found.titles.join(QStringLiteral(", "))));
#endif
    }

    void finish()
    {
        m_watch->done = true;
        if (m_project)
            if (const QString p = timelineProblem(m_project->timeline()); !p.isEmpty())
                fail(QStringLiteral("at the end the timeline is broken: %1").arg(p));
        write(QStringLiteral("%1  monkey: %2 steps, %3 failure(s)").arg(m_failures ? QStringLiteral("FAIL") : QStringLiteral("PASS"))
                  .arg(m_index).arg(m_failures));
        // Unsaved changes would ask on closing: leave without asking.
        if (m_project)
            m_project->undoStack()->setClean();
        qApp->exit(m_failures);
    }

    Q_INVOKABLE void openUrl(const QUrl& url) { write(QStringLiteral("    (not opened: %1)").arg(url.toString())); }

    MainWindow& m_window;
    Project* m_project = nullptr;
    TimelineWidget* m_timeline = nullptr;
    QString m_projectPath;
    QString m_problem;
    int m_steps;
    int m_index = 0;
    int m_failures = 0;
    std::mt19937 m_rng;
    QFile m_log;
    QHash<const QObject*, int> m_dialogMoves; // moves left in each open dialog
    struct Watch {
        std::atomic<int64_t> lastStep{0};
        std::atomic<bool> done{false};
        QString logPath;
        unsigned long mainThread = 0;
    };
    std::shared_ptr<Watch> m_watch = std::make_shared<Watch>();
};

} // namespace

void startMonkeyTest(MainWindow& window, const QString& logPath, int steps, unsigned seed, bool liveTab)
{
    g_liveMonkey = liveTab;
    new Monkey(window, logPath, steps, seed);
}

#include "MonkeyTest.moc"
