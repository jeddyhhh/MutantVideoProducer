#pragma once

#include "core/MidiInput.h"
#include "engine/LiveEngine.h"

#include <QMap>
#include <QPointer>
#include <QSplitter>
#include <QSet>
#include <QWidget>
#include <vector>

struct MidiMessage;
struct OscMessage;
class LiveOverlaysDialog;
class QSlider;
class QToolButton;

class AudioMeter;
class DeviceChainView;
class EffectsBrowser;
class LivePadPanel;
class PreviewWidget;
class Project;
class QCheckBox;
class QComboBox;
class QLabel;
class QSpinBox;
class QPushButton;
class QSplitter;

// The Live tab: pick a camera / capture card, a screen or a window, and play
// with effects on it in real time. A large picture on top; underneath, the
// Video FX browser and the Live FX device chain (saved with the project).
class LiveView : public QWidget {
    Q_OBJECT
    friend class McpServer; // AI control reads and plays the tab
public:
    explicit LiveView(Project* project, QWidget* parent = nullptr);
    ~LiveView() override;

    // Tab shown / hidden: rendering pauses while hidden (the source stays open).
    void setActive(bool active);
    LiveEngine* engine() const { return m_engine; }
    // Capturing with the Editor timeline as the source, a layer or a switcher input.
    bool usesEditor() const;
    DeviceChainView* devices() const { return m_devices; }
    // Start / Stop button (also a controller's Play / Stop).
    void startStop();
    // Rec button: records the live output into the Mutant Video Producer Live folder
    // (starts capture first if needed); pressing again finishes the clip.
    void toggleRecording();
    // Go Live: streams the output to the service in Stream Settings (starts
    // capture first if needed); pressing again ends the stream.
    void toggleStream();
    bool isStreamLive() const { return m_streamLive; }

    // Developer check (MutantVideoProducer --live-test): opens the first screen, renders
    // with an effect, checks pictures arrive; adds PASS/FAIL lines.
    int runSelfTest(QStringList& log);

    // Plays an action from a key, MIDI or OSC: "pad:N" (held down / let go for a
    // momentary pad; `velocity` 0..1 is how hard it was hit), "tap", "setone",
    // "findtempo", "lockbpm", "rec", "replay", "blackout", "start", "stop",
    // "set:next", "set:prev", "scene:N", "song:next", "song:prev", "song:N".
    void runAction(const QString& action, bool down, double velocity = 1.0);
    void midiMessage(const MidiMessage& m);
    // OSC from the network (Setup > MIDI > OSC): learned addresses, and /mvp/pad/1..16,
    // /mvp/scene/1..8, /mvp/padmix 0..1, /mvp/blackout, /mvp/tap... (docs/live.md).
    void oscMessage(const OscMessage& m);
    // Keeps the last seconds in memory while capturing; Save Replay writes them.
    void saveReplay();
    // Saves the picture on the big display (the output, as it is now) as a PNG next to the recordings.
    void saveScreenshot();
    // Scenes (F1-F8): the whole look saved and recalled.
    void saveScene(int scene);
    void recallScene(int scene);
    // Cue (Shift+click a pad or scene): previewed beside the output, not live;
    // Take puts it live. Cueing the same one again closes the cue.
    void cuePad(int pad);
    void cueScene(int scene);
    void takeCue();
    void clearCue();
    // Setlist (Page Down / Page Up): a song's pad set, tempo and scene.
    void recallSong(int index);
    void stepSong(int delta);
    void editSetlist();
    int currentSong() const { return m_currentSong; }
    // How the tab is laid out: 0 Sidebar (pads left, setup right), 1 Stage (the
    // picture across the top, pads and effects below), 2 Compact (tabs beside
    // the picture). The same controls in each; Options > Live Tab Layout.
    static constexpr int kLayoutCount = 3;
    void setLayoutMode(int layout);
    int layoutMode() const { return m_layout; }
    static QString layoutName(int layout);

signals:
    void layoutChanged(int layout);

public:

protected:
    // Pad keys on the Live tab (and its output windows): 1 2 3 4 / Q W E R / A S D F / Z X C V.
    bool eventFilter(QObject* watched, QEvent* event) override;
    void fitSidebar(); // the setup's width: what its sections need, plus the scroll bar

    // The switcher: input 0 = the Source, 1.. = the Switcher section's inputs.
    // Preview picks the next one; cutTo puts one on air (with the pads' transition).
    void setPreviewInput(int input);
    void cutTo(int input, bool withTransition);
    int programInput() const { return m_program; }

private:
    void holdPad(int pad, bool down);
    // Faders (a controller's, or OSC 0..1): "padmix", "master", "opacity:N" (layer N, from 0). False for other actions.
    static bool isFader(const QString& action);
    void setFader(const QString& action, double value);
    std::vector<LiveSource> inputSources() const;
    void updateSwitcher();
    void chooseVideoFiles(QComboBox* combo);
    void addPlaylist(const QStringList& paths);
    LiveSource layerSource(int layer) const;
    void showLayer(); // the controls show layer m_layerIndex's source and settings
    void updateLayerControls();
    void refreshMidi();
    void learnMidi(const QString& action, const QString& label);
    void saveMidiMap() const;
    void refreshScenes();
    void saveLayoutState() const;
    void refreshSetlist();
    // Spout / virtual camera, as ticked.
    void updateSharing();
    void refreshSources();
    void refreshAudioSources();
    // Pads: press = switch that pad on (or off if it is on); edit = show a
    // chain in the device chain panel (0 = Live FX, 1..16 = pad).
    void triggerPad(int pad);
    // Follow: the device chain shows the pad just switched on (or the Live FX when it went off).
    void followPad(int before);
    // Pan and zoom of the source in the output (dragging / scrolling the picture).
    void setPanZoom(LivePanZoom pz);
    void zoomBy(double factor, QPointF atOutputPixel);
    void panZoomFrame(QSizeF* canvas, LiveFraming* framing) const;
    void editChain(int target);
    bool editingLocked() const;
    void updateDeviceLock();
    // The picture in its own window (for OBS) or back in the tab.
    void togglePopOut();
    void updatePaused();
    LiveAudioSource audioSource() const;
    LiveEngine::Options options() const;
    void updateControls();
    void showFullScreenOutput();
    void saveSettings() const;

    Project* m_project;
    LiveEngine* m_engine;
    PreviewWidget* m_display;
    PreviewWidget* m_fullScreen = nullptr; // separate top-level output window
    PreviewWidget* m_popOut = nullptr;     // the picture in its own (normal) window
    LivePadPanel* m_pads;
    QPushButton* m_popOutButton;
    int m_activePad = -1;
    DeviceChainView* m_devices;
    EffectsBrowser* m_browser;
    QSplitter* m_bottomSplit;
    QComboBox* m_source;
    QComboBox* m_format;
    QComboBox* m_size;
    QComboBox* m_framing; // portrait / square sizes: how the picture goes in
    QWidget* m_framingRow = nullptr;
    QWidget* m_formatRow = nullptr; // cameras only
    LivePanZoom m_panZoom, m_panStart;
    int m_dragLayer = -1;     // the layer being dragged on the picture (-1: the drag pans the source)
    QPointF m_dragLayerStart; // its centre when the drag began (shares of the picture)
    int layerAt(QPointF at) const; // the topmost picture-in-picture layer at a point of the picture (output pixels), or -1
    QSize m_sourceSize;
    QLabel* m_panZoomLabel = nullptr;
    // The pieces the layouts arrange, and the frame they are in now.
    QWidget* m_showBar = nullptr;
    QWidget* m_strip = nullptr;  // scenes and the setlist, under the picture
    QWidget* m_center = nullptr; // the picture and the strip
    class QScrollArea* m_sidebar = nullptr; // the setup sections
    class QVBoxLayout* m_mainLayout = nullptr;
    QWidget* m_frame = nullptr;
    QPointer<class QScrollArea> m_padsScroll;
    class QTabWidget* m_frameTabs = nullptr;
    std::vector<std::pair<QString, QPointer<QSplitter>>> m_frameSplitters;
    int m_layout = 0;
    QComboBox* m_fps;
    QCheckBox* m_mirror;
    QComboBox* m_audio;
    AudioMeter* m_audioMeter;
    QPushButton* m_start;
    QPushButton* m_rec;
    QComboBox* m_recMode;
    QLabel* m_recTime;
    QLabel* m_status;
    QToolButton* m_nextFile;
    // Second bar: the layers, the replay buffer, MIDI, overlays.
    QComboBox* m_layerPick; // which layer the controls below show
    QComboBox* m_layer;
    std::array<QComboBox*, LiveEngine::kMaxInputs> m_inputs{};
    std::array<QToolButton*, LiveEngine::kMaxInputs + 1> m_inputTiles{};
    QWidget* m_switcher = nullptr;
    int m_program = 0, m_preview = 1;
    QToolButton* m_nextLayerFile;
    QComboBox* m_layerMode;
    QComboBox* m_layerCorner;
    QSlider* m_layerSize;
    QLabel* m_layerSizeLabel;
    QComboBox* m_layerBlend;
    QSlider* m_layerOpacity;
    QCheckBox* m_layerNoFx;
    int m_layerIndex = 0;
    std::array<QString, kLiveLayers> m_layerNames;          // each layer's source by name (empty: none)
    std::array<LiveLayerSettings, kLiveLayers> m_layerSets; // and how it is laid over
    QCheckBox* m_replay;
    QComboBox* m_replayLength;
    QPushButton* m_saveReplay;
    QPushButton* m_screenshot;
    QString m_lastScreenshot; // the last one saved (tests)
    QComboBox* m_midi;
    QCheckBox* m_velocity; // MIDI velocity sets how much of a pad shows
    QCheckBox* m_oscBox;
    QSpinBox* m_oscPort;
    QLabel* m_oscStatus;
    class QUdpSocket* m_osc = nullptr;
    void updateOsc();
    MidiClock m_midiClock;
    std::array<QToolButton*, kLiveScenes> m_sceneButtons{};
    int m_activeScene = -1;
    // Third bar: the setlist, sending the output to other apps.
    QComboBox* m_song;
    QToolButton* m_songPrev;
    QToolButton* m_songNext;
    QLabel* m_songNotes;
    int m_currentSong = -1;
    QCheckBox* m_spoutBox;
    QCheckBox* m_cameraBox;
    QLabel* m_shareStatus;
    QString m_spoutSender; // as registered now (empty = off)
    QPushButton* m_goLive;
    // The cue panel.
    QWidget* m_cuePanel;
    PreviewWidget* m_cueView;
    QLabel* m_cueLabel;
    int m_cueKind = 0, m_cueIndex = -1; // LiveEngine::CueKind
    int m_cueFrames = 0;
    QLabel* m_streamStatus;
    bool m_streamLive = false;
    QSize m_cameraSize;    // the virtual camera's picture now (empty = off)
    LiveOverlaysDialog* m_overlays = nullptr;
    std::vector<int> m_held; // momentary pads held down, in order
    QMap<QString, QString> m_midiMap; // "n:channel:note" / "c:channel:cc" -> action
    QSet<QString> m_ccDown;           // controllers mapped to actions that are "down"
    QString m_midiLearn, m_midiLearnLabel;
    QString m_lastRecording; // the latest clip recorded (for AI control)
    std::vector<LiveSource> m_sources; // index = combo item data
    std::vector<LiveAudioSource> m_audioSources; // index = m_audio item data (-1 = no audio)
    QString m_audioState; // shown after the capture statistics
    bool m_active = false;
    int m_frames = 0;
};
