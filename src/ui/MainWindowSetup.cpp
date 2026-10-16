// MainWindow: building the window: layout, menus and actions, signal wiring.
#include "core/Brand.h"
#include "core/ProjectFormat.h"
#include "core/Profiles.h"
#include "core/Voice.h"
#include "core/LiveRecorder.h"
#include <QUuid>
#include "ui/MainWindow.h"
#include "core/Compositor.h"
#include "core/Exporter.h"
#include "core/FFmpeg.h"
#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/FormatRegistry.h"
#include "core/Project.h"
#include "engine/MediaCache.h"
#include "engine/PlaybackEngine.h"
#include "ui/AudioMeter.h"
#include "ui/AudioFinishingWindow.h"
#include "ui/DeviceChainView.h"
#include "ui/EffectEditor.h"
#include "ui/KeyframeEditor.h"
#include "ui/EventPropertiesDialog.h"
#include "ui/EffectsBrowser.h"
#include "ui/CrashReport.h"
#include "ui/ExportDialog.h"
#include "ui/StoryPanel.h"
#include "ui/Vst3Window.h"
#include "../../tests/transport.h"
#include "../../tests/collection-ui.h"
#include "core/Story.h"
#include "ui/RenderStatusDialog.h"
#include "ui/Help.h"
#include "ui/LiveClipsBrowser.h"
#include "ui/LiveView.h"
#include "engine/LiveEngine.h"
#include "ui/MediaBin.h"
#include "ui/MixerWindow.h"
#include "ui/PreviewWidget.h"
#include "ui/ProjectSettingsDialog.h"
#include "ui/RelinkDialog.h"
#include "ui/ShortcutsDialog.h"
#include "ui/Theme.h"
#include "ui/TimelineWidget.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QSlider>
#include <QCloseEvent>
#include <QComboBox>
#include <QDate>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QLockFile>
#include <QPushButton>
#include <QToolButton>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFileDialog>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QOffscreenSurface>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QListWidget>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QProgressDialog>
#include "core/BeatMarkers.h"
#include "core/Captions.h"
#include "core/Vst3Host.h"
#include "core/Proxy.h"
#include "core/Scopes.h"
#include "ui/ProxyQueue.h"
#include "ui/McpServer.h"
#include "core/AudioEffects.h"
#include "core/MediaInfo.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QProcess>
#include <QTcpSocket>
#include <QRegularExpression>
#include <random>
#include <QGuiApplication>
#include <QSpinBox>
#include <QTabWidget>
#include <QUndoView>
#include <QDoubleSpinBox>
#include <QWheelEvent>
#include "core/Generators.h"
#include "core/TimelineOps.h"
#include "ui/ScopesWidget.h"
#include "core/Stabilizer.h"
#include <atomic>
#include <thread>
#include <windows.h>
#include <QProgressDialog>
#include <QSettings>
#include <QSplitter>
#include <QStandardPaths>
#include <QScrollBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QDoubleSpinBox>
#include <QTextBrowser>
#include <QTimer>
#include <QTreeWidget>
#include <QToolBar>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <atomic>
#include <thread>

void MainWindow::createLayout()
{
    // Media/FX tabs and the preview on top, the transport bar and timeline
    // below them, and the effect device chain docked along the bottom.
    m_timeline = new TimelineWidget(m_project, m_cache);

    auto* timelinePane = new QWidget;
    auto* tl = new QVBoxLayout(timelinePane);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(0);
    auto* transport = new QToolBar;
    transport->setObjectName(QStringLiteral("transport"));
    transport->setIconSize(QSize(18, 18));
    tl->addWidget(transport);
    tl->addWidget(m_timeline, 1);

    m_bin = new MediaBin(m_project, m_cache);
    auto* tabs = new QTabWidget;
    tabs->setObjectName(QStringLiteral("dockTabs"));
    {
        auto* media = new QWidget; // the filter bar over the files
        auto* v = new QVBoxLayout(media);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);
        v->addWidget(m_bin->createFilterBar());
        v->addWidget(m_bin, 1);
        tabs->addTab(media, tr("Project Media"));
    }
    {
        auto* page = new QWidget; auto* layout = new QVBoxLayout(page); layout->setContentsMargins(0, 0, 0, 0);
        auto* shared = new MediaBin(m_project, m_cache); shared->setObjectName(QStringLiteral("profileMedia")); shared->setSharedProfile(true);
        auto* manage = new QPushButton(tr("Profiles & Home Folders..."));
        connect(manage, &QPushButton::clicked, this, &MainWindow::profilesDialog);
        layout->addWidget(manage); layout->addWidget(shared->createFilterBar()); layout->addWidget(shared, 1);
        tabs->addTab(page, tr("Profile Media"));
        connect(shared, &MediaBin::importRequested, this, [this](const QStringList& paths) { importFiles(paths); });
        connect(shared, &MediaBin::addToTimelineRequested, this, [this](int id) { m_timeline->insertMediaAtCursor(id); });
        connect(shared, &MediaBin::sharedToTimelineRequested, this, [this](const QString& path) { importFiles({path}, m_timeline->cursorTime(), -1, true); });
    }
    auto* fxBrowser = new EffectsBrowser(EffectsBrowser::Kind::Effects);
    fxBrowser->setObjectName(QStringLiteral("fxBrowser"));
    tabs->addTab(fxBrowser, tr("Video FX"));
    auto* audioFxBrowser = new EffectsBrowser(EffectsBrowser::Kind::AudioEffects);
    audioFxBrowser->setObjectName(QStringLiteral("audioFxBrowser"));
    tabs->addTab(audioFxBrowser, tr("Audio FX"));
    auto* transitionBrowser = new EffectsBrowser(EffectsBrowser::Kind::Transitions);
    transitionBrowser->setObjectName(QStringLiteral("transitionBrowser"));
    tabs->addTab(transitionBrowser, tr("Transitions"));
    auto* generatorBrowser = new EffectsBrowser(EffectsBrowser::Kind::Generators);
    generatorBrowser->setObjectName(QStringLiteral("generatorBrowser"));
    tabs->addTab(generatorBrowser, tr("Generators"));
    tabs->setTabToolTip(tabs->count() - 1, tr("Media generators: text, titles, credits, solid colours"));
    m_liveClips = new LiveClipsBrowser;
    m_liveClips->setObjectName(QStringLiteral("liveClips"));
    tabs->addTab(m_liveClips, tr("From Live"));
    tabs->setTabToolTip(tabs->count() - 1, tr("Clips recorded on the Live tab: drag onto the timeline"));
    {
        auto* youtube = new QWidget;
        auto* v = new QVBoxLayout(youtube); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
        auto* download = new QPushButton(tr("Download from YouTube..."));
        download->setObjectName(QStringLiteral("downloadFromYouTube"));
        connect(download, &QPushButton::clicked, this, &MainWindow::createClipFromYouTube);
        m_youtubeBin = new MediaBin(m_project, m_cache); m_youtubeBin->setObjectName(QStringLiteral("youtubeMedia"));
        m_youtubeBin->setOriginFilter(QStringLiteral("youtube"));
        // Only this project's downloads by default; the profile library holds every project's.
        auto* others = new QCheckBox(tr("Show downloads from other projects"));
        others->setObjectName(QStringLiteral("youtubeOtherProjects"));
        connect(others, &QCheckBox::toggled, m_youtubeBin, &MediaBin::setSharedProfile);
        auto* row = new QHBoxLayout; row->addWidget(download, 1); row->addWidget(others);
        v->addLayout(row); v->addWidget(m_youtubeBin->createFilterBar()); v->addWidget(m_youtubeBin, 1);
        tabs->addTab(youtube, tr("From YouTube"));
        tabs->setTabToolTip(tabs->count() - 1, tr("Clips imported through the YouTube downloader"));
    }
    {
        auto* history = new QUndoView(m_project->undoStack()); // click a step to go back (or forward) to it
        history->setObjectName(QStringLiteral("history"));
        history->setEmptyLabel(tr("(the project as opened)"));
        tabs->addTab(history, tr("History"));
    }
    tabs->addTab(new StoryPanel(m_project,m_timeline,[this](const std::vector<Caption>& caps,const QJsonObject& options){narrationStudio(caps,options);}),tr("Story"));
    connect(m_timeline,&TimelineWidget::sequenceRequested,this,[this](int id){editCompound(id);});
    m_fxEditor = new EffectEditor(m_project, [this] { return m_timeline->cursorTime(); }, this);

    auto* previewPane = new QWidget;
    auto* pl = new QVBoxLayout(previewPane);
    pl->setContentsMargins(0, 0, 0, 0);
    pl->setSpacing(2);
    auto* previewBar = new QToolBar;
    m_quality = new QComboBox;
    m_quality->addItem(tr("Best (Full)"), 1);
    m_quality->addItem(tr("Good (Half)"), 2);
    m_quality->addItem(tr("Draft (Quarter)"), 4);
    m_quality->setCurrentIndex(1);
    m_quality->setToolTip(tr("Preview resolution"));
    previewBar->addWidget(new QLabel(tr(" Video Preview  ")));
    previewBar->addWidget(m_quality);
    previewBar->addSeparator();
    QAction* outputFx = previewBar->addAction(tr("Output FX"));
    outputFx->setToolTip(tr("Video output FX: effects applied to the whole final picture"));
    connect(outputFx, &QAction::triggered, this, [this] { m_devices->setTarget({FxTarget::Kind::Output, 0}); });
    previewBar->addSeparator();
    m_proxyAction = previewBar->addAction(tr("Proxies"));
    m_proxyAction->setObjectName(QStringLiteral("proxyToggle"));
    m_proxyAction->setCheckable(true);
    m_proxyAction->setChecked(QSettings().value(QStringLiteral("proxy/enabled"), true).toBool());
    proxy::setEnabled(m_proxyAction->isChecked());
    m_proxyAction->setToolTip(tr("Play proxies (light copies of heavy files) in the preview where there are any.\n"
                                 "Make them: right-click files in Project Media > Create Proxy, or Tools > Create Proxies.\n"
                                 "Renders always use the original files."));
    connect(m_proxyAction, &QAction::toggled, this, [this](bool on) {
        proxy::setEnabled(on);
        QSettings().setValue(QStringLiteral("proxy/enabled"), on);
        refreshPreview();
    });
    m_scopesAction = previewBar->addAction(tr("Scopes"));
    m_scopesAction->setObjectName(QStringLiteral("scopesToggle"));
    m_scopesAction->setCheckable(true);
    m_scopesAction->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
    m_scopesAction->setToolTip(tr("Video scopes beside the preview: waveform, RGB parade, vectorscope, histogram (Ctrl+Alt+S)"));
    pl->addWidget(previewBar);
    m_preview = new PreviewWidget;
    pl->addWidget(m_preview, 1);
    m_previewInfo = new QLabel;
    m_previewInfo->setContentsMargins(6, 2, 6, 2);
    theme::style(m_previewInfo, QStringLiteral("color: @dim;"));
    pl->addWidget(m_previewInfo);

    m_scopes = new ScopesWidget;
    m_scopes->setVisible(false);
    m_topSplit = new QSplitter(Qt::Horizontal);
    m_topSplit->setObjectName(QStringLiteral("editorTopSplit"));
    m_topSplit->addWidget(tabs);
    m_topSplit->addWidget(previewPane);
    m_topSplit->addWidget(m_scopes);
    m_topSplit->setStretchFactor(0, 1);
    m_topSplit->setStretchFactor(1, 2);
    m_topSplit->setStretchFactor(2, 1);
    connect(m_scopesAction, &QAction::toggled, this, [this](bool on) {
        m_scopes->setVisible(on);
        QSettings().setValue(QStringLiteral("scopes/visible"), on);
        if (on) {
            m_scopes->setPicture(m_preview->image());
            const QList<int> sizes = m_topSplit->sizes();
            if (sizes.size() == 3 && sizes[2] < 120) { // room for it, from the preview
                const int w = std::max(260, sizes[1] / 3);
                m_topSplit->setSizes({sizes[0], std::max(200, sizes[1] - w), w});
            }
        }
    });
    m_scopesAction->setChecked(QSettings().value(QStringLiteral("scopes/visible"), false).toBool());

    m_devices = new DeviceChainView(m_project, [this] { return m_timeline->cursorTime(); });
    m_devices->setObjectName(QStringLiteral("editorDevices"));
    m_devices->setPreview(m_preview); // effect masks are drawn and dragged on the preview
    m_keyframes = new KeyframeEditor(m_project, [this] { return m_timeline->cursorTime(); });

    m_mainSplit = new QSplitter(Qt::Vertical);
    m_mainSplit->setObjectName(QStringLiteral("editorMainSplit"));
    m_mainSplit->addWidget(m_topSplit);
    m_mainSplit->addWidget(timelinePane);
    // The bottom section: the device chain, and the properties of a transition,
    // generated media or Pan/Crop (no separate window).
    m_bottomTabs = new QTabWidget;
    m_bottomTabs->setObjectName(QStringLiteral("editorBottomTabs"));
    m_bottomTabs->setDocumentMode(true);
    m_fxEditor->setEmbedded(true);
    m_fxEditor->setPreview(m_preview);
    m_bottomTabs->addTab(m_devices, tr("Effects"));
    m_bottomTabs->setTabToolTip(0, tr("The effect chain of the selected event or track, the video output or the audio master"));
    m_bottomTabs->addTab(m_fxEditor, tr("Properties"));
    m_bottomTabs->setTabToolTip(1, tr("A transition, generated media (text, titles, colours) or an event's Pan/Crop: "
                                      "click a transition's label or select a text event"));
    m_bottomTabs->addTab(m_keyframes, tr("Keyframes"));
    m_bottomTabs->setTabToolTip(2, tr("Every keyframe of the event, track or output shown in Effects: a dope sheet "
                                      "and a graph of the curves"));
    m_mainSplit->addWidget(m_bottomTabs);
    m_mainSplit->setStretchFactor(0, 4);
    m_mainSplit->setStretchFactor(1, 5);
    m_mainSplit->setStretchFactor(2, 0);
    m_mainSplit->setCollapsible(1, false);

    // Editor (everything above) and Live (capture + effects) modes.
    m_live = new LiveView(m_project);
    connect(m_live->engine(), &LiveEngine::recordingStarted, m_liveClips, [this](const QString& path) { m_liveClips->setRecordingPath(path); });
    connect(m_live->engine(), &LiveEngine::recordingFinished, m_liveClips, [this] { m_liveClips->setRecordingPath(QString()); });
    connect(m_liveClips, &LiveClipsBrowser::addToTimelineRequested, this,
            [this](const QString& path) { importFiles({path}, m_timeline->cursorTime()); });
    connect(m_liveClips, &LiveClipsBrowser::addToMediaRequested, this, [this](const QString& path) { importFiles({path}); });
    m_modes = new QTabWidget;
    m_modes->setObjectName(QStringLiteral("modeTabs"));
    m_modes->setDocumentMode(true);
    m_modes->addTab(m_mainSplit, tr("Editor"));
    m_modes->addTab(m_live, tr("Live"));
    m_modes->setTabToolTip(1, tr("Capture a camera, capture card, screen or window and play with effects in real time"));
    connect(m_modes, &QTabWidget::currentChanged, this, [this](int index) {
        const bool live = index == 1;
        if (live && m_engine->isPlaying())
            stopPlayback(false);
        m_live->setActive(live);
    });
    setCentralWidget(m_modes);
    statusBar()->showMessage(tr("Ready. Import media with Ctrl+I or drag files in."), 5000);
    m_rendererLabel = new QLabel;
    theme::style(m_rendererLabel, QStringLiteral("color: @dim; padding: 0 6px;"));
    // Proxies being made: which, how far, a button to stop.
    m_proxies = new ProxyQueue(this);
    m_proxyStatus = new QLabel;
    m_proxyStatus->setObjectName(QStringLiteral("proxyStatus"));
    theme::style(m_proxyStatus, QStringLiteral("color: @info; padding: 0 6px;"));
    m_proxyStatus->setVisible(false);
    m_proxyCancel = new QToolButton;
    m_proxyCancel->setText(QStringLiteral("✕"));
    m_proxyCancel->setToolTip(tr("Stop making proxies"));
    m_proxyCancel->setAutoRaise(true);
    m_proxyCancel->setVisible(false);
    m_mcpLabel = new QLabel;
    m_mcpLabel->setObjectName(QStringLiteral("aiControlStatus"));
    theme::style(m_mcpLabel, QStringLiteral("color: @cue; padding: 0 6px;"));
    m_mcpLabel->setVisible(false);
    statusBar()->addPermanentWidget(m_mcpLabel);
    statusBar()->addPermanentWidget(m_proxyStatus);
    statusBar()->addPermanentWidget(m_proxyCancel);
    statusBar()->addPermanentWidget(m_rendererLabel);
    connect(m_proxyCancel, &QToolButton::clicked, m_proxies, &ProxyQueue::cancelAll);
    connect(m_proxies, &ProxyQueue::progress, this, [this](const QString& name, double f, int left) {
        m_proxyStatus->setText(left > 0 ? tr("Proxy: %1 %2% (%3 more)").arg(name).arg(int(f * 100)).arg(left)
                                        : tr("Proxy: %1 %2%").arg(name).arg(int(f * 100)));
        m_proxyStatus->setVisible(true);
        m_proxyCancel->setVisible(true);
    });
    connect(m_proxies, &ProxyQueue::finished, this, [this](int mediaId, bool ok, const QString& error) {
        auto m = m_project->mediaItem(mediaId);
        const QString name = m ? m->displayName() : QString();
        if (!ok && error != QLatin1String("cancelled"))
            statusBar()->showMessage(tr("No proxy for %1: %2").arg(name, error), 8000);
        else if (ok)
            statusBar()->showMessage(tr("Proxy ready: %1").arg(name), 4000);
        m_bin->refreshIcons();
        refreshPreview();
    });
    connect(m_proxies, &ProxyQueue::idle, this, [this] {
        m_proxyStatus->setVisible(false);
        m_proxyCancel->setVisible(false);
    });
}

void MainWindow::createActions()
{
    auto* undo = m_project->undoStack();
    QToolBar* transport = findChild<QToolBar*>(QStringLiteral("transport"));

    auto act = [this](const QString& text, const QKeySequence& key, auto slot) {
        auto* a = new QAction(text, this);
        if (!key.isEmpty())
            a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        addAction(a); // shortcuts work wherever focus is in the window
        return a;
    };

    // File
    QMenu* file = menuBar()->addMenu(tr("&File"));
    QAction* newAction = act(tr("&New..."), QKeySequence::New, [this] { newProjectDialog(); });
    newAction->setObjectName(QStringLiteral("newProject"));
    file->addAction(newAction);
    file->addAction(act(tr("&Open..."), QKeySequence::Open, [this] { openProjectDialog(); }));
    m_recentMenu = file->addMenu(tr("Open &Recent"));
    m_recentMenu->setToolTipsVisible(true);
    connect(m_recentMenu, &QMenu::aboutToShow, this, &MainWindow::rebuildRecentMenu);
    rebuildRecentMenu();
    file->addAction(act(tr("&Save"), QKeySequence::Save, [this] { save(); }));
    file->addAction(act(tr("Save &As..."), QKeySequence(QStringLiteral("Ctrl+Shift+S")), [this] { saveAs(); }));
    file->addAction(act(tr("Collect Project and Media..."), {}, [this] { collectProjectDialog(); }));
    file->addSeparator();
    file->addAction(act(tr("&Import Media..."), QKeySequence(QStringLiteral("Ctrl+I")), [this] { importDialog(); }));
    file->addAction(act(tr("Create Clip from &YouTube..."), QKeySequence(QStringLiteral("Ctrl+Shift+Y")), [this] {
        if (editorActive())
            createClipFromYouTube();
    }));
    file->addAction(act(tr("Relink &Missing Media..."), QKeySequence(), [this] { relinkMissing(); }));
    file->addAction(act(tr("Export &Frame as Image..."), QKeySequence(QStringLiteral("Ctrl+Shift+F")), [this] { exportFrame(false); }));
    file->addAction(act(tr("Copy Frame to Clipboard"), QKeySequence(QStringLiteral("Ctrl+Shift+C")), [this] { exportFrame(true); }));
    file->addAction(act(tr("&Render As..."), QKeySequence(QStringLiteral("Ctrl+R")), [this] { renderAs(); }));
    file->addSeparator();
    file->addAction(act(tr("&Properties..."), QKeySequence(QStringLiteral("Alt+Return")), [this] { projectProperties(); }));
    auto* profilesAction = act(tr("Profiles & Home Folders..."), QKeySequence(), [this] { profilesDialog(); });
    profilesAction->setObjectName(QStringLiteral("profiles")); file->addAction(profilesAction);
    file->addSeparator();
    file->addAction(act(tr("E&xit"), QKeySequence(QStringLiteral("Alt+F4")), [this] { close(); }));
    // Edit
    QMenu* edit = menuBar()->addMenu(tr("&Edit"));
    QAction* undoAct = undo->createUndoAction(this, tr("&Undo"));
    undoAct->setShortcut(QKeySequence::Undo);
    QAction* redoAct = undo->createRedoAction(this, tr("&Redo"));
    redoAct->setShortcuts({QKeySequence(QStringLiteral("Ctrl+Y")), QKeySequence(QStringLiteral("Ctrl+Shift+Z"))});
    addAction(undoAct);
    addAction(redoAct);
    edit->addAction(undoAct);
    edit->addAction(redoAct);
    edit->addSeparator();
    edit->addAction(act(tr("Cu&t"), QKeySequence::Cut, [this] { m_timeline->cutSelection(); }));
    edit->addAction(act(tr("&Copy"), QKeySequence::Copy, [this] { m_timeline->copySelection(); }));
    edit->addAction(act(tr("&Paste"), QKeySequence::Paste, [this] { m_timeline->paste(); }));
    edit->addAction(act(tr("&Delete"), QKeySequence::Delete, [this] { m_timeline->deleteSelection(false); }));
    edit->addAction(act(tr("Ripple De&lete"), QKeySequence(QStringLiteral("Ctrl+Del")), [this] { m_timeline->deleteSelection(true); }));
    edit->addSeparator();
    edit->addAction(act(tr("&Split"), QKeySequence(QStringLiteral("S")), [this] { m_timeline->splitAtCursor(); }));
    edit->addAction(act(tr("Select &All"), QKeySequence::SelectAll, [this] { m_timeline->selectAll(); }));
    edit->addAction(act(tr("&Group Selected Events"), QKeySequence(QStringLiteral("G")), [this] { m_timeline->groupSelection(); }));
    edit->addAction(act(tr("&Ungroup Selected Events"), QKeySequence(QStringLiteral("U")), [this] { m_timeline->ungroupSelection(); }));
    edit->addAction(act(tr("&Mute / Unmute Event"), QKeySequence(), [this] { m_timeline->toggleMuteSelection(); }));
    edit->addSeparator();
    edit->addAction(act(tr("Nudge Left (1 frame)"), QKeySequence(QStringLiteral("Alt+Left")), [this] { m_timeline->nudgeSelection(-1); }));
    edit->addAction(act(tr("Nudge Right (1 frame)"), QKeySequence(QStringLiteral("Alt+Right")), [this] { m_timeline->nudgeSelection(1); }));
    edit->addAction(act(tr("Nudge Left (10 frames)"), QKeySequence(QStringLiteral("Shift+Alt+Left")), [this] { m_timeline->nudgeSelection(-10); }));
    edit->addAction(act(tr("Nudge Right (10 frames)"), QKeySequence(QStringLiteral("Shift+Alt+Right")), [this] { m_timeline->nudgeSelection(10); }));
    edit->addAction(act(tr("Trim Start to Cursor"), QKeySequence(QStringLiteral("Alt+[")), [this] { m_timeline->trimSelectionToCursor(true); }));
    edit->addAction(act(tr("Trim End to Cursor"), QKeySequence(QStringLiteral("Alt+]")), [this] { m_timeline->trimSelectionToCursor(false); }));
    edit->addSeparator();
    edit->addAction(act(tr("Set &In Point"), QKeySequence(Qt::Key_I), [this] {
        if (editorActive())
            m_timeline->setInPoint(currentTime());
    }));
    edit->addAction(act(tr("Set &Out Point"), QKeySequence(Qt::Key_O), [this] {
        if (editorActive())
            m_timeline->setOutPoint(currentTime());
    }));
    edit->addSeparator();
    edit->addAction(act(tr("&Event FX..."), QKeySequence(QStringLiteral("Ctrl+E")), [this] {
        if (const int id = m_timeline->firstSelectedClip())
            m_devices->setTarget({FxTarget::Kind::Clip, id});
        else
            statusBar()->showMessage(tr("Select an event first."), 4000);
    }));
    edit->addAction(act(tr("Audio &Master FX..."), QKeySequence(), [this] { m_devices->setTarget({FxTarget::Kind::AudioMaster, 0}); }));
    edit->addAction(act(tr("&Normalize Audio Events"), QKeySequence(), [this] { m_timeline->normalizeSelection(); }));
    edit->addAction(act(tr("Video &Output FX..."), QKeySequence(), [this] { m_devices->setTarget({FxTarget::Kind::Output, 0}); }));
    edit->addAction(act(tr("Event &Pan/Crop..."), QKeySequence(QStringLiteral("Ctrl+Shift+P")), [this] {
        if (const int id = m_timeline->firstSelectedVideoClip())
            m_fxEditor->setTarget({FxTarget::Kind::PanCrop, id});
        else
            statusBar()->showMessage(tr("Select a video event first."), 4000);
    }));
    edit->addAction(act(tr("Event P&roperties..."), QKeySequence(QStringLiteral("Ctrl+Shift+E")), [this] {
        const auto& sel = m_timeline->selection();
        if (sel.empty()) {
            statusBar()->showMessage(tr("Select an event first."), 4000);
            return;
        }
        EventPropertiesDialog dlg(m_project, sel, this);
        if (dlg.exec() == QDialog::Accepted)
            dlg.apply();
    }));

    // View
    QMenu* view = menuBar()->addMenu(tr("&View"));
    view->addAction(act(tr("Zoom &In"), QKeySequence(Qt::Key_Up), [this] { m_timeline->zoomIn(); }));
    view->addAction(act(tr("Zoom &Out"), QKeySequence(Qt::Key_Down), [this] { m_timeline->zoomOut(); }));
    view->addAction(act(tr("Zoom to &Fit"), QKeySequence(QStringLiteral("Ctrl+0")), [this] { m_timeline->zoomToFit(); }));
    // The look: every theme switches at once and is remembered.
    QMenu* themes = view->addMenu(tr("&Theme"));
    themes->setObjectName(QStringLiteral("themeMenu"));
    auto* themeGroup = new QActionGroup(themes);
    for (const theme::Colors& t : theme::themes()) {
        QAction* a = themes->addAction(tr(t.name));
        a->setCheckable(true);
        a->setChecked(&t == &theme::c());
        a->setData(QString::fromLatin1(t.id));
        themeGroup->addAction(a);
        connect(a, &QAction::triggered, this, [id = QString::fromLatin1(t.id)] { theme::apply(*qApp, id); });
    }
    view->addSeparator();
    view->addAction(act(tr("Audio &Mixer..."), QKeySequence(QStringLiteral("Ctrl+Shift+M")), [this] { showMixer(); }));
    view->addAction(m_scopesAction);
    view->addSeparator();
    view->addAction(act(tr("Previous Marker"), QKeySequence(Qt::Key_BracketLeft), [this] {
        if (editorActive()) {
            stopPlayback(false);
            m_timeline->jumpToMarker(-1);
        }
    }));
    view->addAction(act(tr("Next Marker"), QKeySequence(Qt::Key_BracketRight), [this] {
        if (editorActive()) {
            stopPlayback(false);
            m_timeline->jumpToMarker(1);
        }
    }));
    view->addSeparator();
    view->addAction(act(tr("Shuttle Backward (press again: faster)"), QKeySequence(Qt::Key_J), [this] { shuttle(-1); }));
    view->addAction(act(tr("Pause"), QKeySequence(Qt::Key_K), [this] { shuttle(0); }));
    view->addAction(act(tr("Shuttle Forward (press again: faster)"), QKeySequence(Qt::Key_L), [this] { shuttle(1); }));

    // Insert
    QMenu* insert = menuBar()->addMenu(tr("&Insert"));
    insert->addAction(act(tr("&Video Track"), QKeySequence(QStringLiteral("Ctrl+Shift+Q")),
                          [this] { m_timeline->insertTrack(TrackType::Video); }));
    insert->addAction(act(tr("&Audio Track"), QKeySequence(QStringLiteral("Ctrl+Q")),
                          [this] { m_timeline->insertTrack(TrackType::Audio); }));
    insert->addSeparator();
    insert->addAction(act(tr("&Volume Envelope (or Composite Level for video)"), QKeySequence(QStringLiteral("V")),
                          [this] { m_timeline->toggleEnvelope(EnvelopeKind::Volume); }));
    insert->addAction(act(tr("&Pan Envelope"), QKeySequence(QStringLiteral("P")), [this] { m_timeline->toggleEnvelope(EnvelopeKind::Pan); }));
    insert->addAction(act(tr("Velocity Envelope (&Speed Ramp)"), QKeySequence(QStringLiteral("Shift+V")),
                          [this] { m_timeline->toggleVelocityEnvelope(); }));
    insert->addSeparator();
    insert->addAction(act(tr("&Marker (at the cursor, or where playback is)"), QKeySequence(Qt::Key_M), [this] {
        if (!editorActive())
            return;
        const TimeUs t = currentTime();
        m_timeline->addMarker(t);
        statusBar()->showMessage(tr("Marker at %1").arg(formatTimecode(t, m_project->settings().fps)), 2500);
    }));
    insert->addAction(act(tr("&Region (from the time selection)"), QKeySequence(Qt::Key_R), [this] {
        if (editorActive())
            m_timeline->addRegionFromSelection();
    }));
    insert->addAction(act(tr("&Freeze Frame (2 s at the cursor)"), QKeySequence(QStringLiteral("Shift+F")), [this] {
        if (!editorActive())
            return;
        stopPlayback(false);
        if (m_timeline->insertFreezeFrame(2 * kUsPerSecond))
            statusBar()->showMessage(tr("Freeze frame added: trim its end to change how long it holds."), 5000);
    }));
    insert->addSeparator();
    auto insertGenerated = [this](const char* preset) {
        stopPlayback(false);
        if (const int id = m_timeline->insertGeneratorAtCursor(QString::fromLatin1(preset)))
            m_fxEditor->setTarget({FxTarget::Kind::Generator, id});
    };
    insert->addAction(act(tr("&Text / Title"), QKeySequence(QStringLiteral("Ctrl+T")), [insertGenerated] { insertGenerated("title"); }));
    insert->addAction(act(tr("&Lower Third"), QKeySequence(), [insertGenerated] { insertGenerated("lower_third"); }));
    insert->addAction(act(tr("&Credits Roll"), QKeySequence(), [insertGenerated] { insertGenerated("credits"); }));
    insert->addAction(act(tr("&Solid Color"), QKeySequence(), [insertGenerated] { insertGenerated("solid_black"); }));
    insert->addSeparator();
    insert->addAction(act(tr("Graphics Overlay"), QKeySequence(), [insertGenerated] { insertGenerated("graphics_broadcast"); }));
    insert->addAction(act(tr("Audio Waveform Graphic"), QKeySequence(), [insertGenerated] { insertGenerated("audio_waveform"); }));

    // Tools
    QMenu* tools = menuBar()->addMenu(tr("&Tools"));
#ifdef _WIN32
    auto* aiAction = act(tr("AI Components..."), QKeySequence(), [this] { aiComponents(); });
    aiAction->setObjectName(QStringLiteral("aiComponents"));
    tools->addAction(aiAction);
#endif
    tools->addAction(act(tr("Detect &Beats (Markers)..."), QKeySequence(QStringLiteral("Ctrl+B")), [this] {
        if (editorActive())
            detectBeats();
    }));
    tools->addAction(act(tr("&Split Selected Events at Markers"), QKeySequence(), [this] {
        if (editorActive())
            splitAtMarkers();
    }));
    tools->addAction(act(tr("Split Selected Events at Scene &Changes..."), QKeySequence(), [this] {
        if (editorActive())
            splitAtSceneChanges();
    }));
    tools->addAction(act(tr("Make Beat &Montage..."), QKeySequence(), [this] {
        if (editorActive())
            beatMontage();
    }));
    tools->addAction(act(tr("Narration Studio..."),QKeySequence(),[this]{if(editorActive())narrationStudio();}));
    QMenu* youtube = tools->addMenu(tr("&YouTube"));
    youtube->addAction(act(tr("&Chapters for the Description..."), QKeySequence(), [this] { if (editorActive()) youtubeChapters(); }));
    youtube->addAction(act(tr("&Thumbnail from the Cursor Frame..."), QKeySequence(), [this] { if (editorActive()) youtubeThumbnail(); }));
    tools->addAction(act(tr("Director's Dice..."),QKeySequence(),[this]{if(editorActive())directorsDice();}));
    tools->addAction(act(tr("Create Compound Sequence..."),QKeySequence("Ctrl+Shift+G"),[this]{if(editorActive())createCompound();}));
    tools->addAction(act(tr("Edit Compound Sequence..."),QKeySequence(),[this]{if(editorActive())editCompound();}));
    tools->addAction(act(tr("Unpack Compound Sequence"),QKeySequence(),[this]{if(editorActive())unpackCompound();}));
    QMenu* captionsMenu = tools->addMenu(tr("&Captions"));
    captionsMenu->addAction(act(tr("&Auto Captions (Speech to Text)..."), QKeySequence(QStringLiteral("Ctrl+Shift+T")), [this] {
        if (editorActive())
            autoCaptions();
    }));
    captionsMenu->addAction(act(tr("&Import Subtitles (SRT / VTT)..."), QKeySequence(), [this] { importSubtitles(); }));
    captionsMenu->addAction(act(tr("&Export Subtitles (SRT / VTT)..."), QKeySequence(), [this] { exportSubtitles(); }));
    auto* voiceAction = act(tr("Generate &Voice from Captions..."), QKeySequence(), [this] { if (editorActive()) generateVoice(); });
    voiceAction->setObjectName(QStringLiteral("generateVoice"));
    captionsMenu->addAction(voiceAction);
    tools->addSeparator();
    auto* finishingAction = act(tr("Audio &Finishing…"), QKeySequence(), [this] {
        if (editorActive()) { stopPlayback(false); showAudioFinishing(this, m_project); }
    });
    finishingAction->setObjectName(QStringLiteral("audioFinishing"));
    tools->addAction(finishingAction);
    tools->addAction(act(tr("Create &Proxies for Heavy Files"), QKeySequence(), [this] {
        QList<int> all;
        for (const auto& [id, m] : m_project->media())
            all << id;
        createProxies(all, true);
    }));
    tools->addAction(act(tr("Create Proxies for Selected Media"), QKeySequence(), [this] { createProxies(m_bin->selectedMediaIds(), false); }));
    QMenu* proxySize = tools->addMenu(tr("Proxy Size"));
    auto* sizes = new QActionGroup(proxySize);
    for (int lines : {360, 540, 720}) {
        QAction* a = proxySize->addAction(tr("%1p").arg(lines));
        a->setCheckable(true);
        a->setChecked(ProxyQueue::height() == lines);
        sizes->addAction(a);
        connect(a, &QAction::triggered, this, [lines] { ProxyQueue::setHeight(lines); });
    }
    QAction* autoProxy = tools->addAction(tr("Make Proxies on Import (heavy files)"));
    autoProxy->setCheckable(true);
    autoProxy->setChecked(QSettings().value(QStringLiteral("proxy/auto"), true).toBool());
    autoProxy->setToolTip(tr("4K, HEVC, AV1, ProRes and 10-bit files get a proxy in the background when imported"));
    connect(autoProxy, &QAction::toggled, this, [](bool on) { QSettings().setValue(QStringLiteral("proxy/auto"), on); });
    tools->addAction(act(tr("Delete All Proxies..."), QKeySequence(), [this] {
        const qint64 bytes = proxy::totalBytes();
        if (QMessageBox::question(this, tr("Delete All Proxies"),
                                  tr("Delete every proxy in the cache (%1 MB)? They can be made again.").arg(bytes / (1024 * 1024)))
            != QMessageBox::Yes)
            return;
        m_proxies->cancelAll();
        const int n = proxy::removeAll();
        statusBar()->showMessage(tr("Deleted %1 proxies").arg(n), 4000);
        m_bin->refreshIcons();
        refreshPreview();
    }));

    // Options
    QMenu* options = menuBar()->addMenu(tr("&Options"));
    m_mcp = new McpServer(this, this);
    QAction* aiControl = options->addAction(tr("Allow AI Control (MCP)"));
    aiControl->setObjectName(QStringLiteral("aiControlToggle"));
    aiControl->setCheckable(true);
    aiControl->setToolTip(tr("Let an AI assistant (Claude Code, Claude Desktop, any MCP client) on this computer edit the project and play the Live tab"));
    connect(aiControl, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue(QStringLiteral("mcp/enabled"), on);
        setAiControl(on);
    });
    options->addAction(tr("AI Control Setup..."), this, &MainWindow::showAiControlSetup);
    QMenu* liveLayout = options->addMenu(tr("Live Tab Layout"));
    auto* liveLayouts = new QActionGroup(liveLayout);
    for (int i = 0; i < LiveView::kLayoutCount; ++i) {
        QAction* a = liveLayout->addAction(LiveView::layoutName(i));
        a->setCheckable(true);
        a->setChecked(m_live && m_live->layoutMode() == i);
        liveLayouts->addAction(a);
        connect(a, &QAction::triggered, this, [this, i] { m_live->setLayoutMode(i); });
        connect(m_live, &LiveView::layoutChanged, a, [a, i](int layout) { a->setChecked(layout == i); });
    }
    connect(m_mcp, &McpServer::activity, this, [this](const QString& tool) {
        statusBar()->showMessage(tr("AI: %1").arg(tool), 3000);
    });
    aiControl->setChecked(QSettings().value(QStringLiteral("mcp/enabled"), false).toBool());
    options->addSeparator();
    m_snapAction = act(tr("Enable &Snapping"), QKeySequence(Qt::Key_F8), [this](bool on) { m_timeline->setSnapping(on); });
    m_snapAction->setCheckable(true);
    m_snapAction->setChecked(true);
    options->addAction(m_snapAction);
    options->addAction(tr("&Keyboard Shortcuts..."), this, [this] {
        ShortcutsDialog dlg(ShortcutsDialog::commands(this), this);
        dlg.exec();
    });
    // Automatic crossfades on overlaps (off: an edit cuts what it lands on).
    QAction* crossfades = options->addAction(tr("Automatic &Crossfades"));
    crossfades->setObjectName(QStringLiteral("autoCrossfades"));
    crossfades->setCheckable(true);
    crossfades->setToolTip(tr("Events overlapping on a track crossfade; off, an event moved, trimmed or placed over another cuts it"));
    connect(crossfades, &QAction::toggled, this, [this](bool on) {
        m_timeline->setAutoCrossfade(on);
        QSettings().setValue(QStringLiteral("edit/autoCrossfade"), on);
    });
    crossfades->setChecked(QSettings().value(QStringLiteral("edit/autoCrossfade"), true).toBool());
    m_timeline->setAutoCrossfade(crossfades->isChecked());
    // The grid events snap to.
    QMenu* gridMenu = options->addMenu(tr("&Grid"));
    auto* grids = new QActionGroup(gridMenu);
    const std::pair<TimelineWidget::Grid, QString> gridKinds[] = {
        {TimelineWidget::Grid::Off, tr("Off")}, {TimelineWidget::Grid::Frames, tr("Frames")},
        {TimelineWidget::Grid::HalfSeconds, tr("Half Seconds")}, {TimelineWidget::Grid::Seconds, tr("Seconds")},
        {TimelineWidget::Grid::Beats, tr("Beats (bars brighter)")}};
    const int savedGrid = QSettings().value(QStringLiteral("edit/grid"), 0).toInt();
    for (const auto& [kind, label] : gridKinds) {
        QAction* a = gridMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(int(kind) == savedGrid);
        grids->addAction(a);
        connect(a, &QAction::triggered, this, [this, kind] {
            m_timeline->setGrid(kind);
            QSettings().setValue(QStringLiteral("edit/grid"), int(kind));
        });
    }
    gridMenu->addSeparator();
    gridMenu->addAction(tr("Beat Grid Tempo..."), this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Beat Grid"));
        auto* form = new QFormLayout(&dlg);
        auto* bpm = new QDoubleSpinBox;
        bpm->setRange(20, 400);
        bpm->setDecimals(2);
        bpm->setValue(m_timeline->gridBpm());
        auto* first = new QDoubleSpinBox;
        first->setRange(0, 3600);
        first->setDecimals(3);
        first->setSuffix(tr(" s"));
        first->setValue(double(m_timeline->gridOffset()) / kUsPerSecond);
        form->addRow(tr("Tempo (BPM)"), bpm);
        form->addRow(tr("A beat at"), first);
        auto* note = new QLabel(tr("Tools > Detect Beats sets these from the music on the timeline."));
        theme::style(note, QStringLiteral("color: @dim;"));
        form->addRow(note);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        help::addButton(buttons, QStringLiteral("editing.md#snapping-and-the-grid"));
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted)
            return;
        m_timeline->setGridTempo(bpm->value(), TimeUs(first->value() * kUsPerSecond));
        QSettings().setValue(QStringLiteral("edit/gridBpm"), m_timeline->gridBpm());
        QSettings().setValue(QStringLiteral("edit/gridOffset"), double(m_timeline->gridOffset()));
    });
    m_timeline->setGridTempo(QSettings().value(QStringLiteral("edit/gridBpm"), 120.0).toDouble(),
                             TimeUs(QSettings().value(QStringLiteral("edit/gridOffset"), 0.0).toDouble()));
    m_timeline->setGrid(TimelineWidget::Grid(std::clamp(savedGrid, 0, 4)));
    m_loopAction = act(tr("&Loop Playback"), QKeySequence(QStringLiteral("Q")), [] {});
    m_loopAction->setCheckable(true);
    options->addAction(m_loopAction);
    m_clickJumpAction = options->addAction(tr("Click While Playing &Jumps There"), this,
                                           [](bool on) { QSettings().setValue(QStringLiteral("play/clickJumps"), on); });
    m_clickJumpAction->setCheckable(true);
    m_clickJumpAction->setChecked(QSettings().value(QStringLiteral("play/clickJumps"), true).toBool());
    m_clickJumpAction->setToolTip(tr("Off: playback carries on and the click sets where Play / Space starts next"));
    QMenu* rippleMenu = options->addMenu(tr("Auto &Ripple"));
    {
        auto* group = new QActionGroup(this);
        const QString names[] = {tr("Off"), tr("Affected Tracks"), tr("All Tracks, Markers and Regions")};
        for (int i = 0; i < 3; ++i) {
            QAction* a = rippleMenu->addAction(names[i], this, [this, i] { setRippleMode(i); });
            a->setCheckable(true);
            group->addAction(a);
            m_rippleActions << a;
        }
        rippleMenu->addSeparator();
        rippleMenu->addAction(act(tr("Toggle Auto Ripple"), QKeySequence(QStringLiteral("Ctrl+L")), [this] {
            setRippleMode(m_timeline->rippleMode() == TimelineWidget::RippleMode::Off ? m_lastRipple : 0);
        }));
    }
    QMenu* autosaveMenu = options->addMenu(tr("&Autosave"));
    {
        auto* group = new QActionGroup(this);
        const int current = QSettings().value(QStringLiteral("autosave/minutes"), 2).toInt();
        for (int minutes : {0, 1, 2, 5, 10}) {
            QAction* a = autosaveMenu->addAction(minutes == 0 ? tr("Off") : tr("Every %n minute(s)", nullptr, minutes), this, [this, minutes] {
                QSettings().setValue(QStringLiteral("autosave/minutes"), minutes);
                if (minutes > 0)
                    m_autosaveTimer.start(minutes * 60000);
                else
                    m_autosaveTimer.stop();
            });
            a->setCheckable(true);
            a->setChecked(minutes == current);
            group->addAction(a);
        }
    }
    QAction* scrubAct = options->addAction(tr("Audio &Scrubbing"), this,
                                           [](bool on) { QSettings().setValue(QStringLiteral("audio/scrub"), on); });
    scrubAct->setCheckable(true);
    scrubAct->setChecked(QSettings().value(QStringLiteral("audio/scrub"), true).toBool());
    scrubAct->setToolTip(tr("Hear the sound under the cursor while dragging it on the ruler or stepping frames"));
    QAction* reopen = options->addAction(tr("Reopen Last Project on Start"), this,
                                         [](bool on) { QSettings().setValue(QStringLiteral("startup/reopenLast"), on); });
    reopen->setCheckable(true);
    reopen->setChecked(QSettings().value(QStringLiteral("startup/reopenLast"), true).toBool());
    QAction* updates = options->addAction(tr("Check for &Updates on Start"), this,
                                          [](bool on) { QSettings().setValue(QStringLiteral("updates/check"), on); });
    updates->setCheckable(true);
    updates->setChecked(QSettings().value(QStringLiteral("updates/check"), true).toBool());
    updates->setToolTip(tr("Once a day, ask GitHub whether a newer Mutant Video Producer is out (the request carries only the version number)"));
    options->addSeparator();
    QAction* gpuAct = act(tr("&GPU Acceleration (OpenGL rendering)"), QKeySequence(), [this](bool on) {
        hw::settings().gpuCompositing = on;
        QSettings().setValue(QStringLiteral("gpu/compositing"), on);
        m_engine->resetRenderer();
        if (m_live)
            m_live->engine()->resetRenderer();
        refreshPreview();
    });
    gpuAct->setCheckable(true);
    gpuAct->setChecked(hw::settings().gpuCompositing);
    options->addAction(gpuAct);
    QAction* hwDecodeAct = act(tr("&Hardware Video Decoding"), QKeySequence(), [this](bool on) {
        hw::settings().hardwareDecoding = on;
        QSettings().setValue(QStringLiteral("gpu/hardwareDecoding"), on);
        m_engine->resetRenderer(); // reopens decoders
        refreshPreview();
    });
    hwDecodeAct->setCheckable(true);
    hwDecodeAct->setChecked(hw::settings().hardwareDecoding);
    options->addAction(hwDecodeAct);

    // Transport (not in a menu: these live on the transport bar)
    auto* playStart = act(tr("Play From Start"), QKeySequence(QStringLiteral("Shift+Space")), [this] {
        stopPlayback(false);
        m_timeline->setCursorTime(0);
        play(false);
    });
    theme::setIcon(playStart, theme::Icon::PlayFromStart);
    m_playAction = act(tr("Play / Stop (return to start)"), QKeySequence(Qt::Key_Space), [this] { togglePlay(); });
    theme::setIcon(m_playAction, theme::Icon::Play);
    auto* pause = act(tr("Play / Pause"), QKeySequence(Qt::Key_Return), [this] { pausePlay(); });
    pause->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    theme::setIcon(pause, theme::Icon::Pause);
    auto* stop = act(tr("Stop"), QKeySequence(), [this] { stopPlayback(true); });
    theme::setIcon(stop, theme::Icon::Stop);
    auto* home = act(tr("Go to Start"), QKeySequence(Qt::Key_Home), [this] {
        stopPlayback(false);
        m_timeline->goToStart();
    });
    theme::setIcon(home, theme::Icon::GoToStart);
    auto* end = act(tr("Go to End"), QKeySequence(Qt::Key_End), [this] {
        stopPlayback(false);
        m_timeline->goToEnd();
    });
    theme::setIcon(end, theme::Icon::GoToEnd);
    auto* prev = act(tr("Previous Frame"), QKeySequence(Qt::Key_Left), [this] {
        stopPlayback(false);
        m_timeline->stepFrames(-1);
    });
    theme::setIcon(prev, theme::Icon::PrevFrame);
    auto* next = act(tr("Next Frame"), QKeySequence(Qt::Key_Right), [this] {
        stopPlayback(false);
        m_timeline->stepFrames(1);
    });
    theme::setIcon(next, theme::Icon::NextFrame);
    act(tr("Previous Edit Point"), QKeySequence(QStringLiteral("Ctrl+Left")), [this] { m_timeline->jumpToEdit(-1); });
    act(tr("Next Edit Point"), QKeySequence(QStringLiteral("Ctrl+Right")), [this] { m_timeline->jumpToEdit(1); });
    theme::setIcon(m_loopAction, theme::Icon::Loop);

    transport->addAction(m_loopAction);
    transport->addAction(playStart);
    transport->addAction(m_playAction);
    transport->addAction(pause);
    transport->addAction(stop);
    transport->addSeparator();
    transport->addAction(home);
    transport->addAction(prev);
    transport->addAction(next);
    transport->addAction(end);
    transport->addSeparator();
    transport->addAction(m_snapAction);
    m_rippleButton = new QToolButton;
    m_rippleButton->setMenu(rippleMenu);
    m_rippleButton->setPopupMode(QToolButton::InstantPopup);
    m_rippleButton->setCheckable(true);
    m_rippleButton->setToolTip(tr("Auto ripple (Ctrl+L): moving, trimming, deleting and pasting push or pull what comes after"));
    transport->addWidget(m_rippleButton);
    transport->addSeparator();
    QAction* masterFx = transport->addAction(tr("Master FX"));
    masterFx->setToolTip(tr("Audio master FX: effects on the final mix (e.g. limiter, EQ)"));
    connect(masterFx, &QAction::triggered, this, [this] { m_devices->setTarget({FxTarget::Kind::AudioMaster, 0}); });
    m_meter = new AudioMeter;
    m_meter->setFixedWidth(240);
    transport->addWidget(m_meter);
    QAction* mixer = transport->addAction(tr("Mixer"));
    mixer->setToolTip(tr("Audio Mixer (Ctrl+Shift+M): levels, faders, pan, solo / mute and auto ducking per track"));
    connect(mixer, &QAction::triggered, this, &MainWindow::showMixer);
    auto* meterTimer = new QTimer(this);
    connect(meterTimer, &QTimer::timeout, this, [this] {
        const bool playing = m_engine->isPlaying();
        const auto [l, r] = playing ? m_engine->takePeaks() : std::pair<float, float>{0, 0};
        m_meter->setPeaks(l, r);
        const auto tracks = playing ? m_engine->takeTrackPeaks() : std::map<int, std::pair<float, float>>{};
        m_timeline->setTrackMeters(tracks);
        if (m_mixer)
            m_mixer->setPeaks(tracks, {l, r});
    });
    meterTimer->start(33);

    // Help
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    QAction* guide = act(tr("&User Guide"), QKeySequence(), [this] { showHelp(QStringLiteral("README.md")); });
    guide->setObjectName(QStringLiteral("userGuide"));
    help->addAction(guide);
    QMenu* tutorials = help->addMenu(tr("&Tutorials"));
    tutorials->addAction(tr("All Tutorials"), this, [this] { showHelp(QStringLiteral("tutorials/README.md")); });
    tutorials->addSeparator();
    for (const QFileInfo& f : QDir(docsPath(QStringLiteral("tutorials"))).entryInfoList({QStringLiteral("*.md")}, QDir::Files, QDir::Name)) {
        QFile page(f.filePath());
        if (f.fileName() == QLatin1String("README.md") || !page.open(QIODevice::ReadOnly))
            continue;
        QString title = QString::fromUtf8(page.readLine()).trimmed(); // "# Title"
        while (title.startsWith(QLatin1Char('#')))
            title.remove(0, 1);
        tutorials->addAction(title.trimmed(), this, [this, name = f.fileName()] { showHelp(QStringLiteral("tutorials/") + name); });
    }
    help->addAction(act(tr("&Find a Command..."), QKeySequence(QStringLiteral("Ctrl+K")), [this] { findCommand(); }));
    QAction* here = act(tr("Help on &This"), QKeySequence(QStringLiteral("Shift+F1")),
                        [] { help::open(help::pageFor(QApplication::focusWidget())); });
    here->setToolTip(tr("The guide's page for the part of the window that has the focus"));
    help->addAction(here);
    QAction* welcome = act(tr("&Welcome Window"), QKeySequence(), [this] { showWelcome(); });
    welcome->setObjectName(QStringLiteral("welcomeWindow"));
    help->addAction(welcome);
    help->addSeparator();
    help->addAction(act(tr("System &Check..."), QKeySequence(), [this] { showSystemCheck(); }));
    help->addAction(act(tr("Check for &Updates..."), QKeySequence(), [this] { checkForUpdates(true); }));
    help->addAction(act(tr("Supported &Formats..."), QKeySequence(), [this] { showFormats(); }));
    help->addAction(act(tr("&About Mutant Video Producer"), QKeySequence(), [this] { about(); }));
}

void MainWindow::connectSignals()
{
    // Shift+F1 (Help on This): the guide's page for the part of the window with the focus.
    for (const auto& [w, page] : std::initializer_list<std::pair<QWidget*, const char*>>{
             {m_timeline, "editing.md"},
             {m_bin, "media.md#project-media"},
             {m_devices, "effects.md#the-device-chain"},
             {m_fxEditor, "effects.md#the-properties-tab-and-handles-on-the-preview"},
             {m_keyframes, "effects.md#the-keyframe-editor"},
             {m_preview, "getting-started.md#the-window"},
             {m_liveClips, "media.md#from-live"},
             {m_youtubeBin, "media.md#create-clip-from-youtube"},
             {m_live, "live.md"},
             {findChild<QWidget*>(QStringLiteral("fxBrowser")), "video-effects.md"},
             {findChild<QWidget*>(QStringLiteral("audioFxBrowser")), "audio.md"},
             {findChild<QWidget*>(QStringLiteral("transitionBrowser")), "transitions.md"},
             {findChild<QWidget*>(QStringLiteral("generatorBrowser")), "titles-and-generators.md"},
             {findChild<QWidget*>(QStringLiteral("history")), "editing.md#undo-and-history"}})
        if (w)
            help::setPage(w, QString::fromLatin1(page));
    connect(m_project, &Project::changed, this, [this] {
        m_engine->setSnapshot(m_project->snapshot());
        if (!m_engine->isPlaying())
            m_engine->requestFrame(m_timeline->cursorTime());
        updateTitle();
    });
    connect(m_project, &Project::settingsChanged, this, &MainWindow::updatePreviewInfo);
    connect(m_project, &Project::mediaChanged, this, [this] { m_engine->setSnapshot(m_project->snapshot()); });
    for (auto signal : {&Project::changed, &Project::mediaChanged, &Project::settingsChanged})
        connect(m_project, signal, this, [this] { ++m_editCount; });
    connect(m_project, &Project::mediaRelinked, this, [this](int id) {
        if (auto m = m_project->mediaItem(id))
            m_cache->forget(id, m->revision);
        refreshPreview();
    });
    connect(m_timeline, &TimelineWidget::scrubbed, this, [this](TimeUs t) {
        if (QSettings().value(QStringLiteral("audio/scrub"), true).toBool())
            m_engine->scrubAudio(t);
    });
    connect(m_timeline, &TimelineWidget::message, this, [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(m_timeline, &TimelineWidget::relinkRequested, this, [this](int id) { relinkMissing(id); });
    connect(m_timeline, &TimelineWidget::beatsRequested, this, &MainWindow::detectBeats);
    connect(m_timeline, &TimelineWidget::stabilizeRequested, this, &MainWindow::stabilizeClip);
    connect(m_bin, &MediaBin::relinkRequested, this, [this](int id) { relinkMissing(id); });
    connect(m_bin, &MediaBin::proxyRequested, this, [this](const QList<int>& ids) { createProxies(ids, false, true); });
    connect(m_bin, &MediaBin::proxyDeleteRequested, this, &MainWindow::deleteProxies);
    connect(m_project->undoStack(), &QUndoStack::cleanChanged, this, &MainWindow::updateTitle);

    // Moving the cursor while playing carries on from there, or (Click While Playing
    // Jumps There off) only sets where Space stops and starts next; a shuttle stops.
    connect(m_timeline, &TimelineWidget::cursorMoved, this, [this](TimeUs t) {
        if (m_shuttleRate == 0 && m_engine->isPlaying()) {
            m_playStart = t;
            if (m_clickJumpAction->isChecked()) {
                m_engine->play(t);
                m_timeline->setPlayhead(t);
            }
            return;
        }
        stopPlayback(false);
        m_engine->requestFrame(t);
    });
    connect(m_timeline, &TimelineWidget::filesDropped, this,
            [this](const QStringList& paths, TimeUs at, int track) { importFiles(paths, at, track, true); });
    connect(m_bin, &MediaBin::importRequested, this, [this](const QStringList& paths) {
        if (paths.isEmpty())
            importDialog();
        else
            importFiles(paths);
    });
    connect(m_bin, &MediaBin::addToTimelineRequested, this, [this](int id) { m_timeline->insertMediaAtCursor(id); });
    connect(m_youtubeBin, &MediaBin::sharedToTimelineRequested, this, [this](const QString& path) { importFiles({path}, m_timeline->cursorTime(), -1, true); });
    connect(m_youtubeBin, &MediaBin::importRequested, this, [this](const QStringList& paths) { importFiles(paths); });
    connect(m_project, &Project::mediaChanged, this, [this] {
        QJsonArray records;
        for (const auto& [id, m] : m_project->media()) if (m && m->info.ok())
            records.append(QJsonObject{{"path", m->path}, {"folder", m->folder}, {"rating", m->rating}, {"in", double(m->markIn)}, {"out", double(m->markOut)}, {"origin", m->origin}});
        QString error;
        if (!Profiles::instance().remember(records, &error)) statusBar()->showMessage(error, 8000);
    });
    connect(m_youtubeBin, &MediaBin::addToTimelineRequested, this, [this](int id) { m_timeline->insertMediaAtCursor(id); });
    connect(m_youtubeBin, &MediaBin::relinkRequested, this, [this](int id) { relinkMissing(id); });
    connect(m_youtubeBin, &MediaBin::proxyRequested, this, [this](const QList<int>& ids) { createProxies(ids, false, true); });
    connect(m_youtubeBin, &MediaBin::proxyDeleteRequested, this, &MainWindow::deleteProxies);

    // Video FX and transitions.
    connect(m_timeline, &TimelineWidget::clipEffectsRequested, this,
            [this](int id) { m_devices->setTarget({FxTarget::Kind::Clip, id}); });
    connect(m_timeline, &TimelineWidget::trackEffectsRequested, this,
            [this](int id) { m_devices->setTarget({FxTarget::Kind::Track, id}); });
    // The device chain follows the selected event;
    // generated media (text, titles, colours) shows its properties.
    connect(m_timeline, &TimelineWidget::selectionChanged, this, [this] {
        const int id = m_timeline->firstSelectedClip();
        if (!id)
            return;
        const Clip* c = m_project->timeline().findClip(id);
        if (c && c->isGenerated() && !m_holdProperties) {
            const bool sameEditor = m_fxEditor->target().id == id && m_bottomTabs->currentWidget() == m_fxEditor;
            m_holdProperties = true;
            m_devices->setTarget({FxTarget::Kind::Clip, id}); // (its Event FX are a tab away)
            if (!sameEditor || m_fxEditor->target().kind != FxTarget::Kind::Generator)
                m_fxEditor->setTarget({FxTarget::Kind::Generator, id});
            m_bottomTabs->setCurrentWidget(m_fxEditor);
            QTimer::singleShot(0, this, [this] { m_holdProperties = false; });
            return;
        }
        m_devices->setTarget({FxTarget::Kind::Clip, id});
    });
    connect(m_timeline, &TimelineWidget::cursorMoved, m_devices, &DeviceChainView::onCursorMoved);
    // The Keyframes tab shows what Effects shows (an event: all of its settings).
    connect(m_devices, &DeviceChainView::targetChanged, m_keyframes, [this](const FxTarget& t) {
        if (t.kind != FxTarget::Kind::Live)
            m_keyframes->setTarget(t);
    });
    connect(m_timeline, &TimelineWidget::cursorMoved, m_keyframes, &KeyframeEditor::onCursorMoved);
    connect(m_keyframes, &KeyframeEditor::seekRequested, this, [this](TimeUs t) {
        seekTo(t);
        m_keyframes->onCursorMoved();
    });
    auto showKeyframes = [this](const keyops::Address& a) {
        m_keyframes->focusSetting(a);
        m_bottomTabs->setCurrentWidget(m_keyframes);
    };
    connect(m_devices, &DeviceChainView::keyframeEditorRequested, this, showKeyframes);
    connect(m_fxEditor, &EffectEditor::keyframeEditorRequested, this, showKeyframes);
    // Clicking a track header shows that track's FX.
    connect(m_timeline, &TimelineWidget::trackSelected, this, [this](int id) { m_devices->setTarget({FxTarget::Kind::Track, id}); });
    connect(m_devices, &DeviceChainView::seekRequested, this, [this](TimeUs t) {
        seekTo(t);
        m_devices->onCursorMoved();
    });
    connect(m_timeline, &TimelineWidget::transitionRequested, this, [this](int id, bool in) {
        m_keyframes->setTarget({FxTarget::Kind::Clip, id});
        m_fxEditor->setTarget({in ? FxTarget::Kind::TransitionIn : FxTarget::Kind::TransitionOut, id});
    });
    connect(m_timeline, &TimelineWidget::cursorMoved, m_fxEditor, &EffectEditor::onCursorMoved);
    connect(m_timeline, &TimelineWidget::generatorRequested, this,
            [this](int id) { m_fxEditor->setTarget({FxTarget::Kind::Generator, id}); });
    connect(m_timeline, &TimelineWidget::panCropRequested, this,
            [this](int id) { m_fxEditor->setTarget({FxTarget::Kind::PanCrop, id}); });
    connect(m_timeline, &TimelineWidget::propertiesRequested, this, [this](int id) {
        std::set<int> ids = m_timeline->selection();
        if (!ids.count(id))
            ids = {id};
        EventPropertiesDialog dlg(m_project, ids, this);
        if (dlg.exec() == QDialog::Accepted)
            dlg.apply();
    });
    auto* generatorBrowser = findChild<EffectsBrowser*>(QStringLiteral("generatorBrowser"));
    connect(generatorBrowser, &EffectsBrowser::activated, this, [this](const QString& id) {
        stopPlayback(false);
        if (const int clip = m_timeline->insertGeneratorAtCursor(id))
            m_fxEditor->setTarget({FxTarget::Kind::Generator, clip});
    });
    connect(m_fxEditor, &EffectEditor::seekRequested, this, [this](TimeUs t) { seekTo(t); m_fxEditor->onCursorMoved(); });
    connect(m_fxEditor, &EffectEditor::opened, this, [this] {
        m_bottomTabs->setCurrentWidget(m_fxEditor);
        m_holdProperties = true; // the selection that comes with a click on a transition keeps it here
        QTimer::singleShot(0, this, [this] { m_holdProperties = false; });
    });
    connect(m_fxEditor, &EffectEditor::closed, this, [this] { m_bottomTabs->setCurrentWidget(m_devices); });
    connect(m_devices, &DeviceChainView::targetChanged, this, [this](const FxTarget& t) {
        if (!m_holdProperties && t.kind != FxTarget::Kind::None)
            m_bottomTabs->setCurrentWidget(m_devices);
    });
    auto* fxBrowser = findChild<EffectsBrowser*>(QStringLiteral("fxBrowser"));
    auto addFx = [this](const QString& id) {
        // Add to the chain shown in the device view, else to the selected events.
        if (m_devices->acceptsEffect(id)) {
            m_devices->addEffect(id);
        } else if (const int clip = m_timeline->addEffectToSelection(id)) {
            m_devices->setTarget({FxTarget::Kind::Clip, clip});
        } else {
            statusBar()->showMessage(tr("Select events of the right kind first, or drag the effect onto an event or track header."), 5000);
        }
    };
    connect(fxBrowser, &EffectsBrowser::activated, this, addFx);
    auto addPreset = [this](const QString& id) {
        if (m_devices->acceptsPreset(id)) {
            m_devices->addPreset(id);
        } else if (const int clip = m_timeline->addPresetToSelection(id)) {
            m_devices->setTarget({FxTarget::Kind::Clip, clip});
        } else {
            statusBar()->showMessage(tr("Select events of the right kind first, or drag the preset onto an event, track or the device chain."), 5000);
        }
    };
    connect(fxBrowser, &EffectsBrowser::presetActivated, this, addPreset);
    connect(findChild<EffectsBrowser*>(QStringLiteral("audioFxBrowser")), &EffectsBrowser::presetActivated, this, addPreset);
    connect(findChild<EffectsBrowser*>(QStringLiteral("audioFxBrowser")), &EffectsBrowser::activated, this, addFx);
    auto* transitionBrowser = findChild<EffectsBrowser*>(QStringLiteral("transitionBrowser"));
    connect(transitionBrowser, &EffectsBrowser::activated, this, [this](const QString& id) {
        if (const int clip = m_timeline->setTransitionOnSelection(id))
            m_fxEditor->setTarget({FxTarget::Kind::TransitionIn, clip});
        else
            statusBar()->showMessage(tr("Select video events first, or drag the transition onto the start or end of an event."), 5000);
    });

    connect(m_engine, &PlaybackEngine::frameReady, m_preview, [this](const QImage& img, TimeUs) {
        m_preview->setImage(img);
        if (m_scopes->isVisible())
            m_scopes->setPicture(img);
    });
    connect(m_engine, &PlaybackEngine::positionChanged, this, &MainWindow::onPlaybackPosition);
    connect(m_engine, &PlaybackEngine::rendererChanged, this, [this](const QString& name) {
        const QString decode = hw::settings().hardwareDecoding && !hw::decodeDeviceName().isEmpty()
                                   ? hw::decodeDeviceName()
                                   : tr("CPU");
        m_rendererLabel->setText(tr("Render: %1   Decode: %2").arg(name, decode));
        m_rendererName = name;
    });
    connect(m_quality, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] {
        m_engine->setPreviewDivisor(m_quality->currentData().toInt());
        updatePreviewInfo();
        refreshPreview();
    });
}
