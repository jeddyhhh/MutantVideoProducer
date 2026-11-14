#include "core/Brand.h"
#include "core/FFmpeg.h"
#include "core/Project.h"
#include "core/Hardware.h"
#include "core/Proxy.h"
#include "ui/DeviceChainView.h"
#include "ui/EffectEditor.h"
#include "ui/KeyframeEditor.h"
#include "ui/CrashReport.h"
#include "ui/ExportDialog.h"
#include "ui/LiveView.h"
#include "ui/MainWindow.h"
#include "ui/MonkeyTest.h"
#include "ui/NodeEffectEditor.h"
#include <QGraphicsView>
#include "ui/McpServer.h"
#include "ui/TimelineWidget.h"
#include "ui/Theme.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QIcon>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeWidget>
#include <QTimer>
#include <memory>
#include <optional>
#include <QLineEdit>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char** argv)
{
    // An MCP client started us as its server: pass messages to the running Mutant Video Producer (no window).
    for (int i = 1; i < argc; ++i)
        if (QByteArray(argv[i]) == "--mcp-stdio")
            return runMcpStdioBridge(argc, argv);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Mutant Video Producer"));
    QApplication::setOrganizationName(QStringLiteral("Mutant Video Producer"));
    QApplication::setApplicationVersion(QStringLiteral(MVP_VERSION));
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/data/mvp-icon.png")));
    // Test and screenshot sessions must never import real user settings.
    bool isolated = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray argument(argv[i]);
        isolated |= argument == "--screenshot" || argument.endsWith("-test");
    }
    if (!isolated) brand::initializeCompatibility();
    QApplication::setApplicationDisplayName(QString::fromLatin1(brand::name));
    ff::initialize();
    theme::apply(app);

    // Command line: a project file opens it, anything else is imported.
    // Developer option: --screenshot <file.png> renders the window off-screen
    // (never shown on the desktop), saves it after loading and quits.
    QStringList args = app.arguments().mid(1);
    QString screenshot;
    if (const int i = args.indexOf(QStringLiteral("--screenshot")); i >= 0 && i + 1 < args.size()) {
        screenshot = args[i + 1];
        args.remove(i, 2);
    }
    // Options repeated on the command line (--live-action pad:0 --live-action pad:3).
    auto takeAll = [&](const QString& option) {
        QStringList values;
        for (int i = args.indexOf(option); i >= 0 && i + 1 < args.size(); i = args.indexOf(option)) {
            values << args[i + 1];
            args.remove(i, 2);
        }
        return values;
    };
    // Screenshot extras: --live-video <file> starts the Live tab on a video,
    // --live-action <action> then plays a pad / scene..., --trigger <name>
    // triggers an action or clicks a button by object name (e.g. scopesToggle),
    // --grab <name> also saves one widget (object or class name, e.g.
    // PreviewWidget) as <screenshot>_<name>.png, --delay <ms> waits before grabbing.
    const QStringList liveVideo = takeAll(QStringLiteral("--live-video"));
    const QStringList liveActions = takeAll(QStringLiteral("--live-action"));
    const QStringList triggers = takeAll(QStringLiteral("--trigger"));
    // --effect-file <.mvpfx>: with --trigger effectBuilder, the Effect Builder opens that effect (--grab NodeEffectEditor)
    const QStringList effectFile = takeAll(QStringLiteral("--effect-file"));
    const QStringList graphZoom = takeAll(QStringLiteral("--graph-zoom")); // (and its graph at this zoom, centred)
    const QStringList grabs = takeAll(QStringLiteral("--grab"));
    const QStringList delays = takeAll(QStringLiteral("--delay"));
    const QStringList themeId = takeAll(QStringLiteral("--theme")); // a theme by id, e.g. for screenshots of it
    const QStringList modeTab = takeAll(QStringLiteral("--mode"));   // --mode <n>: the workspace tab (0 Editor, 1 Live, 2 Deliver)
    const QStringList eventId = takeAll(QStringLiteral("--event"));  // --event <id>: the event --fx-editor shows (default: the first)
    if (!screenshot.isEmpty()) {
        // Its own fresh settings: not the user's sources (a screen), layouts or recent files.
        QApplication::setApplicationName(QStringLiteral("MvpScreenshot"));
        QSettings().clear();
        if (!liveVideo.isEmpty()) {
            qputenv("MVP_NO_DEVICES", "1"); // only the video: never this computer's screens or cameras in a screenshot
            const QString path = QFileInfo(liveVideo.front()).absoluteFilePath();
            QSettings().setValue(QStringLiteral("live/playlists"), QVariantList{QStringList{path}});
            QSettings().setValue(QStringLiteral("live/source"), QStringLiteral("Video: %1").arg(QFileInfo(path).fileName()));
        }
    }
    if (!themeId.isEmpty())
        theme::apply(app, themeId.front());
    const bool play = args.removeAll(QStringLiteral("--play")) > 0;
    QString devicesTest; // --devices-test <log file>: scripted device chain UI check
    if (const int i = args.indexOf(QStringLiteral("--devices-test")); i >= 0 && i + 1 < args.size()) {
        devicesTest = args[i + 1];
        args.remove(i, 2);
    }
    QString liveTest; // --live-test <log file>: opens a screen on the Live tab and checks live effects
    if (const int i = args.indexOf(QStringLiteral("--live-test")); i >= 0 && i + 1 < args.size()) {
        liveTest = args[i + 1];
        args.remove(i, 2);
        // Test recordings go next to the log, not into the user's Videos folder.
        qputenv("MVP_LIVE_DIR", QDir(QFileInfo(liveTest).absolutePath()).filePath(QStringLiteral("live-recordings")).toUtf8());
        // Its own settings and pad sets: the user's saved Audio input and Beat Detection
        // settings (e.g. a Max BPM under 120) would change what the test hears.
        QApplication::setApplicationName(QStringLiteral("MvpLiveTest"));
        QSettings().clear();
    }
    QString editorTest; // --editor-test <log file>: markers, ripple, tracks, J/K/L, autosave, recent, relink
    if (const int i = args.indexOf(QStringLiteral("--editor-test")); i >= 0 && i + 1 < args.size()) {
        editorTest = args[i + 1];
        args.remove(i, 2);
        // Its own settings, autosaves and recent list, not the user's.
        QApplication::setApplicationName(QStringLiteral("MvpEditorTest"));
        const QString dir = QDir(QFileInfo(editorTest).absolutePath()).filePath(QStringLiteral("editor-test"));
        QDir(dir).removeRecursively();
        qputenv("MVP_AUTOSAVE_DIR", QDir(dir).filePath(QStringLiteral("autosave")).toUtf8());
    }
    QString monkeyTest; // --monkey-test <log file> [--steps n] [--seed n] [--live]: random commands, keys and clicks (see MonkeyTest.h)
    int monkeySteps = 3000;
    unsigned monkeySeed = 1;
    bool monkeyLive = false;
    if (const int i = args.indexOf(QStringLiteral("--monkey-test")); i >= 0 && i + 1 < args.size()) {
        monkeyTest = args[i + 1];
        args.remove(i, 2);
        if (const int k = args.indexOf(QStringLiteral("--steps")); k >= 0 && k + 1 < args.size()) {
            monkeySteps = args[k + 1].toInt();
            args.remove(k, 2);
        }
        if (const int k = args.indexOf(QStringLiteral("--seed")); k >= 0 && k + 1 < args.size()) {
            monkeySeed = args[k + 1].toUInt();
            args.remove(k, 2);
        }
        monkeyLive = args.contains(QStringLiteral("--live")); // the Live tab (the option is taken below)
        // Its own settings and autosaves; Qt's own file dialogs (they can be closed from here).
        QApplication::setApplicationName(QStringLiteral("MvpMonkeyTest"));
        QSettings().clear();
        // Never the computer's cameras, microphones, screens or windows: on the Live
        // tab, the project's videos (in its media folder) and a test beat.
        qputenv("MVP_NO_DEVICES", "1");
        QVariantList playlists;
        for (const QString& a : args)
            if (a.endsWith(QLatin1String(".mvp"), Qt::CaseInsensitive))
                for (const QFileInfo& f : QDir(QFileInfo(a).absoluteDir().filePath(QStringLiteral("media"))).entryInfoList({QStringLiteral("*.mp4")}, QDir::Files))
                    playlists << QStringList{f.absoluteFilePath()};
        QSettings().setValue(QStringLiteral("live/playlists"), playlists);
        if (!playlists.isEmpty()) // chosen, so Start has something to show
            QSettings().setValue(QStringLiteral("live/source"),
                                 QStringLiteral("Video: %1").arg(QFileInfo(playlists.front().toStringList().front()).fileName()));
        const QString dir = QDir(QFileInfo(monkeyTest).absolutePath()).filePath(QStringLiteral("monkey-test"));
        QDir(dir).removeRecursively();
        qputenv("MVP_AUTOSAVE_DIR", QDir(dir).filePath(QStringLiteral("autosave")).toUtf8());
        qputenv("MVP_LIVE_DIR", QDir(dir).filePath(QStringLiteral("live-recordings")).toUtf8());
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    }
    const bool liveTab = args.removeAll(QStringLiteral("--live")) > 0; // developer option: start on the Live tab
    // Developer option: --export-dialog[=search] opens the Render As dialog (for screenshots).
    std::optional<QString> exportDialog;
    for (int i = 0; i < args.size(); ++i)
        if (args[i].startsWith(QLatin1String("--export-dialog"))) {
            exportDialog = args[i].section(QLatin1Char('='), 1);
            args.removeAt(i);
            break;
        }
    const bool fxEditor = args.removeAll(QStringLiteral("--fx-editor")) > 0;
    // Developer options: the Keyframes tab on the first event with keyframes (its Dope Sheet, or Graph).
    const bool keyframesGraph = args.removeAll(QStringLiteral("--keyframes-graph")) > 0;
    const bool keyframesTab = args.removeAll(QStringLiteral("--keyframes")) > 0 || keyframesGraph;
    const bool mixer = args.removeAll(QStringLiteral("--mixer")) > 0; // developer option: open the Audio Mixer
    int dockTab = -1; // --tab <n>: developer option, picks a tab of the media / FX browser
    if (const int i = args.indexOf(QStringLiteral("--tab")); i >= 0 && i + 1 < args.size()) {
        dockTab = args[i + 1].toInt();
        args.remove(i, 2);
    }
    QString browse; // --browse <text>: developer option, opens and scrolls to that FX browser folder
    if (const int i = args.indexOf(QStringLiteral("--browse")); i >= 0 && i + 1 < args.size()) {
        browse = args[i + 1];
        args.remove(i, 2);
    }
    const bool panCropEditor = args.removeAll(QStringLiteral("--pancrop-editor")) > 0;
    double seekSeconds = -1;
    if (const int i = args.indexOf(QStringLiteral("--seek")); i >= 0 && i + 1 < args.size()) {
        seekSeconds = args[i + 1].toDouble();
        args.remove(i, 2);
    }
    // A crash leaves a report (the next start offers it). Developer option:
    // --crash-test segv|abort|fatal crashes on purpose, with its own reports folder.
    const QStringList crashTest = takeAll(QStringLiteral("--crash-test"));
    if (!crashTest.isEmpty())
        QApplication::setApplicationName(QStringLiteral("MvpCrashTest"));
    crash::install();
    if (!crashTest.isEmpty()) {
        qWarning("crash test: %s", qPrintable(crashTest.front()));
        if (crashTest.front() == QLatin1String("abort"))
            std::abort();
        if (crashTest.front() == QLatin1String("fatal"))
            qFatal("crash test: a fatal error");
        volatile int* nowhere = nullptr;
        *nowhere = 1;
    }

    // One Mutant Video Producer per user: files opened while it runs (a double-click on a .mvp) go to it.
    const bool tests = !devicesTest.isEmpty() || !liveTest.isEmpty() || !editorTest.isEmpty() || !monkeyTest.isEmpty();
    const QString instanceKey =
        QStringLiteral("%1-%2").arg(QApplication::applicationName(), qEnvironmentVariable("USERNAME", qEnvironmentVariable("USER")));
    QStringList given;
    for (const QString& a : args)
        if (QFileInfo::exists(a))
            given << QFileInfo(a).absoluteFilePath();
    if (!tests && !given.isEmpty()) {
        QLocalSocket running;
        running.connectToServer(instanceKey);
        if (running.waitForConnected(500)) {
#ifdef Q_OS_WIN
            AllowSetForegroundWindow(ASFW_ANY); // (so it may come to the front)
#endif
            running.write(given.join(QLatin1Char('\n')).toUtf8() + '\n');
            running.waitForBytesWritten(2000);
            running.disconnectFromServer();
            return 0;
        }
    }

    // Proxies deleted while the preview still had them open go once the window
    // (and its decoders) have closed: this is destroyed after it.
    struct PurgeProxies {
        ~PurgeProxies() { proxy::purge(); }
    } purgeProxies;
    MainWindow window;
    QLocalServer instance;
    if (!tests) {
        QLocalServer::removeServer(instanceKey); // (a crashed one's leftover socket, on Linux)
        instance.listen(instanceKey);
        QObject::connect(&instance, &QLocalServer::newConnection, &window, [&] {
            while (QLocalSocket* s = instance.nextPendingConnection()) {
                auto data = std::make_shared<QByteArray>(s->readAll()); // (it may all be here, and the sender gone, already)
                auto finish = [s, data, &window] {
                    *data += s->readAll();
                    s->deleteLater();
                    window.openFromElsewhere(QString::fromUtf8(*data).split(QLatin1Char('\n'), Qt::SkipEmptyParts));
                };
                if (s->state() == QLocalSocket::UnconnectedState) {
                    finish();
                    continue;
                }
                QObject::connect(s, &QLocalSocket::readyRead, s, [s, data] {
                    *data += s->readAll();
                    if (data->size() > 256 * 1024) { // file names, not a flood
                        data->clear();
                        s->abort();
                    }
                });
                QObject::connect(s, &QLocalSocket::disconnected, &window, finish);
            }
        });
    }
    const bool scripted = !screenshot.isEmpty() || !devicesTest.isEmpty() || !liveTest.isEmpty() || !editorTest.isEmpty() || !monkeyTest.isEmpty();
    if (scripted) {
        window.setAttribute(Qt::WA_DontShowOnScreen, true);
        for (QWidget* w : window.findChildren<EffectEditor*>())
            w->setAttribute(Qt::WA_DontShowOnScreen, true);
    }
    if (!monkeyTest.isEmpty()) // before anything opens: its dialogs are answered and kept off the screen
        startMonkeyTest(window, monkeyTest, monkeySteps, monkeySeed, monkeyLive);
    window.show();

    QStringList media;
    bool projectGiven = false;
    for (const QString& a : args) {
        if (QFileInfo(a).suffix().compare(QStringLiteral("mvp"), Qt::CaseInsensitive) == 0) {
            projectGiven = true;
            window.openProject(a);
        } else if (QFileInfo::exists(a)) {
            // Keep command-line imports valid when saving to another folder.
            media << QFileInfo(a).absoluteFilePath();
        }
    }
    if (!scripted) // recover a crashed session's work, or reopen the last project
        window.startup(projectGiven || !media.isEmpty());
    if (!media.isEmpty())
        window.importFiles(media, TimeUs(0));
    if (seekSeconds >= 0)
        window.seekTo(TimeUs(seekSeconds * kUsPerSecond));
    if (fxEditor || panCropEditor) {
        // Developer option: show the first event's effects (or title / pan-crop editor).
        if (auto* project = window.findChild<Project*>())
            for (const Track& t : project->timeline().tracks)
                if (!t.clips.empty() && (eventId.isEmpty() || project->timeline().findClip(eventId.front().toInt()))) {
                    const Clip& c = eventId.isEmpty() ? t.clips.front() : *project->timeline().findClip(eventId.front().toInt());
                    if (panCropEditor || c.isGenerated())
                        window.findChild<EffectEditor*>()->setTarget(
                            {panCropEditor ? FxTarget::Kind::PanCrop : FxTarget::Kind::Generator, c.id});
                    else
                        window.findChild<DeviceChainView*>(QStringLiteral("editorDevices"))->setTarget({FxTarget::Kind::Clip, c.id});
                    break;
                }
    }
    if (keyframesTab) {
        auto* editor = window.findChild<KeyframeEditor*>();
        if (auto* project = window.findChild<Project*>()) {
            int found = 0;
            for (const Track& t : project->timeline().tracks)
                for (const Clip& c : t.clips)
                    for (const keyops::Channel& ch : keyops::channels(project->timeline(), keyops::Scope::Clip, c.id))
                        if (const ParamValue* p = keyops::find(project->timeline(), ch); p && !p->keys.empty() && !found)
                            found = c.id;
            if (found)
                editor->setTarget({FxTarget::Kind::Clip, found});
        }
        if (auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("editorBottomTabs")))
            tabs->setCurrentWidget(editor);
        if (keyframesGraph)
            if (auto* graph = editor->findChild<QToolButton*>(QStringLiteral("keyGraphMode")))
                graph->click();
    }
    if (mixer)
        QMetaObject::invokeMethod(&window, "showMixerForTests", Qt::DirectConnection);
    if (dockTab >= 0)
        if (auto* tabs = window.findChild<QTabWidget*>(QStringLiteral("dockTabs")))
            tabs->setCurrentIndex(dockTab);
    if (!browse.isEmpty())
        for (QTreeWidget* tree : window.findChildren<QTreeWidget*>()) {
            const auto hits = tree->findItems(browse, Qt::MatchContains | Qt::MatchRecursive);
            if (hits.isEmpty())
                continue;
            for (QTreeWidgetItem* it = hits.front(); it; it = it->parent())
                it->setExpanded(true);
            tree->scrollToItem(hits.front(), QAbstractItemView::PositionAtTop);
        }
    if (play)
        QTimer::singleShot(500, &window, [&] { window.togglePlay(); });
    if (!devicesTest.isEmpty()) {
        QTimer::singleShot(1500, &window, [&] {
            auto* view = window.findChild<DeviceChainView*>(QStringLiteral("editorDevices"));
            if (auto* project = window.findChild<Project*>())
                for (const Track& t : project->timeline().tracks)
                    if (t.type == TrackType::Video && !t.clips.empty()) {
                        view->setTarget({FxTarget::Kind::Clip, t.clips.front().id});
                        break;
                    }
            QStringList log;
            int failures = view->runSelfTest(log);
            failures += window.findChild<TimelineWidget*>()->runDropTest(log);
            failures += window.findChild<KeyframeEditor*>()->runSelfTest(log);
            // The header click above should have switched the device panel to Track FX.
            const bool trackShown = view->target().kind == FxTarget::Kind::Track;
            log << QStringLiteral("%1  the device panel shows Track FX after a header click").arg(trackShown ? QStringLiteral("PASS") : QStringLiteral("FAIL"));
            failures += trackShown ? 0 : 1;
            QFile f(devicesTest);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                f.write(log.join(QLatin1Char('\n')).toUtf8() + "\n");
            app.exit(failures);
        });
    }
    if (!editorTest.isEmpty()) {
        QTimer::singleShot(1500, &window, [&] {
            QStringList log;
            const int failures = window.runEditorTest(log);
            QFile f(editorTest);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                f.write(log.join(QLatin1Char('\n')).toUtf8() + "\n");
            app.exit(failures);
        });
    }
    if (liveTab || !liveTest.isEmpty() || !modeTab.isEmpty())
        if (auto* modes = window.findChild<QTabWidget*>(QStringLiteral("modeTabs")))
            modes->setCurrentIndex(modeTab.isEmpty() ? 1 : modeTab.front().toInt());
    if (!liveTest.isEmpty()) {
        QTimer::singleShot(1000, &window, [&] {
            QStringList log;
            const int failures = window.findChild<LiveView*>()->runSelfTest(log);
            QFile f(liveTest);
            if (f.open(QIODevice::WriteOnly | QIODevice::Text))
                f.write(log.join(QLatin1Char('\n')).toUtf8() + "\n");
            app.exit(failures);
        });
    }
    ExportDialog* exportDlg = nullptr;
    if (exportDialog) {
        const Project* project = window.findChild<Project*>();
        exportDlg = new ExportDialog(project->settings(), 30 * kUsPerSecond, false, 0, 0, QDir::home().filePath(QStringLiteral("render.mp4")), &window);
        exportDlg->setAttribute(Qt::WA_DontShowOnScreen, !screenshot.isEmpty());
        exportDlg->show();
        if (!exportDialog->isEmpty())
            if (auto* search = exportDlg->findChild<QLineEdit*>(QStringLiteral("templateSearch")))
                search->setText(*exportDialog);
    }
    for (const QString& name : triggers)
        QTimer::singleShot(800, &window, [&window, name] {
            // (by object name, or a menu command by its words: "Record Voice-Over")
            auto words = [](QString t) { return t.remove(QLatin1Char('&')).remove(QStringLiteral("...")).remove(QChar(0x2026)).trimmed(); };
            QAction* byText = nullptr;
            for (QAction* a : window.findChildren<QAction*>())
                if (!byText && words(a->text()).compare(name, Qt::CaseInsensitive) == 0)
                    byText = a;
            if (auto* action = window.findChild<QAction*>(name))
                action->trigger();
            else if (auto* button = window.findChild<QAbstractButton*>(name))
                button->click();
            else if (byText)
                byText->trigger();
        });
    if (!effectFile.isEmpty())
        QTimer::singleShot(1200, &window, [&window, path = effectFile.front(), offScreen = !screenshot.isEmpty()] {
            if (auto* editor = window.findChild<NodeEffectEditor*>()) {
                editor->hide();
                editor->setAttribute(Qt::WA_DontShowOnScreen, offScreen);
                editor->resize(1600, 1000);
                editor->show();
                editor->openFile(path);
            }
        });
    if (!graphZoom.isEmpty())
        QTimer::singleShot(2000, &window, [&window, zoom = graphZoom.front().toDouble()] {
            if (auto* editor = window.findChild<NodeEffectEditor*>())
                if (auto* view = editor->findChild<QGraphicsView*>()) {
                    view->resetTransform();
                    view->scale(zoom, zoom);
                    view->centerOn(view->scene()->itemsBoundingRect().center());
                }
        });
    if (!screenshot.isEmpty() && !liveVideo.isEmpty()) {
        if (auto* modes = window.findChild<QTabWidget*>(QStringLiteral("modeTabs")))
            modes->setCurrentIndex(1);
        QTimer::singleShot(500, &window, [&window] { window.findChild<LiveView*>()->startStop(); });
        for (const QString& action : liveActions)
            QTimer::singleShot(1500, &window, [&window, action] { window.findChild<LiveView*>()->runAction(action, true); });
    }
    if (!screenshot.isEmpty()) {
        window.resize(1600, 1000);
        // Keep the picture legible and all demo tracks visible in documentation captures (once the window has its
        // size: earlier, the splitters share out their old sizes again).
        // (the effects section only when it is the subject)
        QTimer::singleShot(300, &window, [&window, bottom = fxEditor || panCropEditor || keyframesTab] {
            if (auto* split = window.findChild<QSplitter*>(QStringLiteral("editorMainSplit")))
                split->setSizes(bottom ? QList<int>{420, 250, 330} : QList<int>{440, 560, 0});
            if (auto* split = window.findChild<QSplitter*>(QStringLiteral("editorTopSplit")))
                split->setSizes({360, 1240, 0});
        });
        QTimer::singleShot(delays.isEmpty() ? 4000 : delays.front().toInt(), &window, [&] {
            window.grab().save(screenshot);
            for (const QString& name : grabs)
                for (QWidget* w : window.findChildren<QWidget*>())
                    if (w->isVisible() && (w->objectName() == name || QLatin1String(w->metaObject()->className()) == name)) {
                        w->grab().save(QFileInfo(screenshot).dir().filePath(QFileInfo(screenshot).completeBaseName() + QLatin1Char('_') + name
                                                                             + QStringLiteral(".png")));
                        break;
                    }
            if (exportDlg)
                exportDlg->grab().save(QFileInfo(screenshot).dir().filePath(QFileInfo(screenshot).completeBaseName() + QStringLiteral("_export.png")));
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w->isVisible() && w->windowTitle() == QStringLiteral("Audio Mixer"))
                    w->grab().save(QFileInfo(screenshot).dir().filePath(QFileInfo(screenshot).completeBaseName() + QStringLiteral("_mixer.png")));
            if (auto* editor = window.findChild<EffectEditor*>(); editor && editor->isVisible())
                editor->grab().save(QFileInfo(screenshot).dir().filePath(QFileInfo(screenshot).completeBaseName() + QStringLiteral("_fx.png")));
            app.exit(0); // not quit(): that would ask to save the project
        });
    }
    return app.exec();
}
