#pragma once

#include "core/Project.h"
#include "core/Captions.h"
#include "core/Time.h"
#include "core/ProjectCollection.h"

#include <QElapsedTimer>
#include <QMainWindow>
#include <QUrl>
#include <QTimer>
#include <memory>
#include <optional>
#include <vector>

class DeviceChainView;
class EffectEditor;
class AudioMeter;
class MediaBin;
class MediaCache;
class PlaybackEngine;
class PreviewWidget;
class Project;
class QAction;
class QComboBox;
class QLabel;
class QLockFile;
class QMenu;
class QSplitter;
class QTextBrowser;
class QToolButton;
class TimelineWidget;
class VoiceJob;
class DialogueJob;
struct VoiceAsset;

class MainWindow : public QMainWindow {
    Q_OBJECT
    friend class McpServer; // AI control edits through the window
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

    bool openProject(const QString& path);
    // Probes and adds files to the media pool; optionally places them on the timeline.
    void importFiles(const QStringList& paths, std::optional<TimeUs> at = {}, int trackIndex = -1, bool newVideoTracks = false);
    void seekTo(TimeUs t);
    void togglePlay();      // Space: play / stop and return to start

    // After the window is shown: offers to recover autosaved work left by a
    // crash, then (unless a project was opened from the command line)
    // reopens the last project if that option is on; with nothing open, the
    // welcome window. Checks for a newer release once a day.
    void startup(bool projectGiven);
    // Files another start of Mutant Video Producer passed on (a double-click on a .mvp while it runs): a project
    // opens (after asking to save), anything else is imported; the window comes to the front.
    void openFromElsewhere(const QStringList& paths);
    // Where autosaves go (MVP_AUTOSAVE_DIR overrides it).
    static QString autosaveDir();
    // Developer check (MutantVideoProducer --editor-test): markers, ripple, tracks,
    // J/K/L, I/O, autosave and recovery, recent projects, relinking.
    int runEditorTest(QStringList& log);
    Q_INVOKABLE void showMixerForTests() { showMixer(); }

    // Help > User Guide / Tutorials, and context help (Help.h): a page of docs/
    // ("README.md", "effects.md#masks"); from a dialog, in a window of the dialog's own.
    void showHelp(const QString& page);
    static QString docsPath(const QString& page);
    // Opens a copy of an example project (data/examples) from Documents/Mutant Video Producer Examples (or `into`).
    void openExample(const QString& name, const QString& into = {});

    // Help > System Check (MainWindowStart.cpp): what this computer can do, and how to fix what it can't.
    struct CheckItem {
        enum State { Ready, Note, Problem };
        QString what, status, fix;
        State state = Ready;
    };
    std::vector<CheckItem> systemCheck();
    // Whether release `latest` ("v0.1.5") is newer than `current` ("0.1.4"; a "dev-..." build never is).
    static bool isNewerVersion(const QString& latest, const QString& current);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void generateVoice();
    void aiComponents();
    bool installAiComponent(const QString& id, const QString& device, QString* error);
    bool m_aiInstallBusy = false;
    void narrationStudio(const std::vector<Caption>& supplied = {}, const QJsonObject& suppliedOptions = {});
    void syncByAudio();          // Tools > Sync Selected Events by Audio
    void makeMulticam();         // Tools > Multicam > Make Multicam from Selected Events
    void multicamAngle(int angle); // 1-9: cut (playing) or switch (stopped) to that angle
    void showMulticam();         // the angles window
    class VoiceOverDialog* recordVoiceOver(); // Tools > Record Voice-Over (returned for the tests)
    void createCompound();
    void editCompound(int id = 0);
    void unpackCompound();
    void directorsDice();
    void profilesDialog();
    VoiceJob* voiceJob();
    VoiceJob* m_voiceJob = nullptr;
    void generateDialogue();
    // The built-in sound effects library: sfx next to the program.
    static QString soundLibraryDir();
    QWidget* createSoundsTab();
    DialogueJob* dialogueJob();
    DialogueJob* m_dialogueJob = nullptr;
    // Downloads (and SHA-256 checks) whichever of these voice files are missing; false when one fails or is cancelled.
    bool ensureVoiceAssets(QWidget* parent, const std::vector<VoiceAsset>& assets);
    void createActions();
    void createLayout();
    void connectSignals();

    // An empty project: the size and rate of `settings`, or of the first video added.
    void newProject(const std::optional<ProjectSettings>& settings = std::nullopt);
    void newProjectDialog(); // File > New: the shape and frame rate first
    void openProjectDialog();
    void importDialog();
    bool save();
    bool saveAs();
    void collectProjectDialog();
    collection::Result collectProjectTo(const QString& destination, const collection::Options& options);
    bool m_collecting = false;
    bool maybeSave();
    void renderAs();
    void projectProperties();
    void showFormats();
    void about();
    // Starting out (MainWindowStart.cpp): the welcome window, the system check,
    // and a newer release on GitHub (asked by Help > Check for Updates, or once a day).
    void showWelcome();
    void showSystemCheck();
    void checkForUpdates(bool manual);
    // Mutant Video Producer crashed last time: the report, to look at or to report on GitHub (CrashReport.h).
    void offerCrashReport();
    // Help > Find a Command (Ctrl+K): every menu command, found by typing part of its name.
    void findCommand();

    // Autosave / recovery.
    void autosave();
    void removeAutosave();
    bool checkRecovery();
    void reopenLast(); // Options > Reopen Last Project on Start
    QStringList orphanedAutosaves() const; // left by sessions that crashed, newest first
    bool recoverFrom(const QString& path);
    // Recent projects.
    void addRecent(const QString& path);
    void rebuildRecentMenu();
    void relinkMissing(int focusMediaId = 0);
    void setRippleMode(int mode); // TimelineWidget::RippleMode
    void showMixer();
    // The frame at the cursor at full project size, to a file or the clipboard.
    void exportFrame(bool toClipboard);
    // Tools: beat markers from the music, splitting on markers, a montage cut to them.
    void detectBeats();
    void splitAtMarkers();
    void splitAtSceneChanges(); // the selected video events, where the picture cuts to a new shot
    void autoReframe();         // Pan/Crop that follows the subject, for a tall (9:16) frame
    void beatMontage();
    // Tools > Captions (MainWindowCaptions.cpp): speech to captions with Whisper,
    // SRT / WebVTT in and out; captions go on a new Captions track.
    void autoCaptions();
    void importSubtitles();
    void exportSubtitles();
    void addCaptions(const std::vector<struct Caption>& captions, TimeUs offset, bool kinetic);
    // Auto captions of the timeline from..to with Whisper model file `modelFile` (a "tdrz"
    // model marks speaker turns); `progress` (may be null) shows it and can cancel.
    bool captionTimeline(const QString& modelFile, struct TranscribeOptions options, TimeUs from, TimeUs to, bool kinetic,
                         class QProgressDialog* progress, int* added, QString* error);
    // File > Create Clip from YouTube (MainWindowYouTube.cpp): a link (or a part of it) downloaded with
    // yt-dlp next to the project and placed at the cursor.
    void createClipFromYouTube();
    void youtubeChapters();   // chapters for the video description, from story chapters or markers
    void youtubeThumbnail();  // a 1280x720 thumbnail from the frame at the cursor, with a title
    // The downloaded file into Project Media's YouTube folder, `from`..`to` marked (-1: its start / end),
    // and on the timeline at the cursor; its media id (0: it could not be imported).
    int placeDownloadedClip(const QString& path, TimeUs from, TimeUs to);
    // Downloads `url` to `dest` with a progress dialog that can cancel; false with `error`.
    static bool downloadFile(QWidget* parent, const QUrl& url, const QString& dest, const QString& what, QString* error, qint64 maximumBytes = 0);
    // A Whisper model's file: bundled with the program (the offline release), else in the downloads folder.
    static QString whisperModelPath(const QString& file);
    // Whisper on the timeline's mix from `from` to `to` (caption times from `from`), on a worker thread.
    bool transcribeTimeline(const QString& modelFile, const struct TranscribeOptions& options, TimeUs from, TimeUs to,
                            class QProgressDialog* progress, std::vector<struct Caption>& captions, QString* error);
    void removeSilences();      // Tools: cut the quiet stretches out of the edit
    void insertTime();          // Insert: empty time at the cursor on every unlocked track (the opposite of a cut)
    void editByTranscript();    // Tools: strike words in a transcript to cut them out
    // Right-click a video event > Stabilize: how much, analysing it first if needed.
    void stabilizeClip(int clipId);
    // Proxies: queue the files (only the heavy ones if `heavyOnly`), delete theirs.
    void createProxies(const QList<int>& mediaIds, bool heavyOnly, bool replace = false);
    void deleteProxies(const QList<int>& mediaIds);

    // J / K / L shuttle (and the Live tab check for editor-only keys).
    bool editorActive() const;
    TimeUs currentTime() const;
    void shuttle(int direction);
    void stopShuttle();
    void onShuttleTick();

    void play(bool fromRegionStart);
    void pausePlay();       // Enter: play / pause in place
    void stopPlayback(bool returnToStart);
    void onPlaybackPosition(TimeUs t);
    void refreshPreview();
    void updateTitle();
    void updatePreviewInfo();

    Project* m_project;
    MediaCache* m_cache;
    PlaybackEngine* m_engine;
    TimelineWidget* m_timeline;
    PreviewWidget* m_preview;
    MediaBin* m_bin;
    MediaBin* m_youtubeBin = nullptr;
    class RenderQueue* m_renderQueue = nullptr; // made when first used
    class RenderQueue* renderQueue();
    void queueProjectFile(); // Render Queue > Add Project File
    EffectEditor* m_fxEditor;       // transitions, titles, pan/crop
    DeviceChainView* m_devices;     // effect chains
    class KeyframeEditor* m_keyframes; // every keyframe of the event, track or output shown in Effects
    class QTabWidget* m_bottomTabs = nullptr; // Effects (the device chain) / Properties (transition, generator, pan/crop)
    bool m_editorTransaction = false; // modal sequence or live variation preview owns the timeline
    bool m_holdProperties = false;  // the Properties tab just opened: the selection that came with it doesn't switch away
    class QTabWidget* m_modes = nullptr; // Editor / Live
    class LiveView* m_live = nullptr;
    class DeliverView* m_deliver = nullptr;
    class LiveClipsBrowser* m_liveClips = nullptr; // "From Live" tab
    QLabel* m_previewInfo;
    QComboBox* m_quality;
    QSplitter* m_mainSplit;
    QSplitter* m_topSplit;

    QAction* m_loopAction = nullptr;
    QAction* m_clickJumpAction = nullptr; // a click while playing: jump there (on) or set the start point (off)
    QTextBrowser* m_helpBrowser = nullptr; // in the Help window, made when first shown
    QAction* m_snapAction = nullptr;
    QAction* m_playAction = nullptr;
    QLabel* m_rendererLabel = nullptr;
    QString m_rendererName;            // what renders the preview ("GPU (...)" or "CPU ...")
    QString m_updateUrl;               // a newer release's page, once one was found
    QToolButton* m_updateButton = nullptr; // status bar: the newer release
    class ProxyQueue* m_proxies = nullptr;
    QLabel* m_proxyStatus = nullptr;
    QToolButton* m_proxyCancel = nullptr;
    QAction* m_proxyAction = nullptr;  // preview bar: play proxies
    class ScopesWidget* m_scopes = nullptr;
    QAction* m_scopesAction = nullptr; // preview bar / View: show the scopes
    class McpServer* m_mcp = nullptr;  // AI control (Options > Allow AI Control)
    QLabel* m_mcpLabel = nullptr;
    void setAiControl(bool on);
    void showAiControlSetup();
    AudioMeter* m_meter = nullptr;
    class MixerWindow* m_mixer = nullptr;

    TimeUs m_playStart = 0;
    bool m_settingsMatched = false;

    QTimer m_autosaveTimer;
    QString m_autosavePath;              // this session's autosave file
    std::unique_ptr<QLockFile> m_autosaveLock; // held while running: a leftover file without it is a crash
    quint64 m_editCount = 0, m_autosavedCount = 0;
    QMenu* m_recentMenu = nullptr;
    QToolButton* m_rippleButton = nullptr;
    QList<QAction*> m_rippleActions;
    int m_lastRipple = 1;

    QTimer m_shuttleTimer;
    QElapsedTimer m_shuttleClock;
    double m_shuttleRate = 0; // 0 = not shuttling; 1x forward uses normal playback
    TimeUs m_shuttleFrom = 0;
};
