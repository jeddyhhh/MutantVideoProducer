#pragma once

#include "core/KeyframeOps.h"
#include "core/Project.h"
#include "ui/FxTarget.h"

#include <QTimer>
#include <QWidget>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

class QLabel;
class QMenu;
class QMimeData;
class QScrollArea;
class QToolButton;
class ParamControl;
class PreviewWidget;
class MaskOverlay;
class PreviewOverlay;

// Device chain, docked under the timeline: the effect chain of
// an event, a track, the video output or the audio master as a row of
// devices, each showing all of its controls. Drag effects from the FX tabs
// (or devices themselves; Ctrl copies) in front of or behind a device, into
// a rack's chain, or onto a rack's chain list for a new parallel chain.
// Ctrl+G groups the selected devices into a rack.
class DeviceChainView : public QWidget {
    Q_OBJECT
public:
    // Devices are as tall as the panel allows (controls stack in columns
    // down that height), so a long device gets narrower as the panel grows.
    int deviceBodyHeight() const;
    static constexpr const char* kDeviceMime = "application/x-mvp-device";

    DeviceChainView(Project* project, std::function<TimeUs()> cursor, QWidget* parent = nullptr);
    ~DeviceChainView() override;

    // Clip, Track, Output or AudioMaster; anything else clears the view.
    void setTarget(const FxTarget& target);
    const FxTarget& target() const { return m_target; }
    // True if the chain shown can take this (video or audio) effect.
    bool acceptsEffect(const QString& type) const;
    // Appends an effect to the top-level chain.
    void addEffect(const QString& type);
    // Presets: true if the chain shown can take it; appends its devices.
    bool acceptsPreset(const QString& presetId) const;
    void addPreset(const QString& presetId);
    void onCursorMoved();
    // The picture an effect's mask is drawn on and dragged in (the editor's preview).
    void setPreview(PreviewWidget* preview);

    // Developer check (MutantVideoProducer --devices-test): drives the view with synthetic
    // drag/drop and mouse events on the current target; adds PASS/FAIL lines.
    int runSelfTest(QStringList& log);

signals:
    void seekRequested(TimeUs t);
    // A chain was put up (an event's, a track's, the output's...).
    void targetChanged(const FxTarget& target);
    // A control's "Edit in Keyframe Editor": show that setting there.
    void keyframeEditorRequested(const keyops::Address& address);
    // An effect made in the Effect Builder was double-clicked: open it there (the instance, by its effect id).
    void userEffectRequested(int effectId);

private:
    friend class ParamControl;
    friend class MaskOverlay;
    friend class DeviceWidget;
    friend class DeviceTitle;
    friend class ChainStrip;
    friend class RackChainList;
    friend class McpServer; // (AI control: Track Motion)

    // A parameter of effect `index` in chain `path`, or of that rack's chain `chain`.
    struct ParamRef {
        ChainPath path;
        int index = 0;
        int chain = -1;
    };

    bool isAudio() const;
    bool targetExists() const;
    // A device dragged: its place, tagged with this view and its chain (a drag from another view, another chain or
    // another copy of the program names a place in a different chain: it's refused).
    QByteArray deviceMimeData(const ChainPath& path, int index) const;
    bool readDeviceMime(const QMimeData* mime, ChainPath* path, int* index) const;
    EffectChain* root(Timeline& tl) const;
    const EffectChain* root(const Timeline& tl) const;
    const EffectInstance* effectAt(const ChainPath& path, int index) const;
    ParamMap* params(Timeline& tl, const ParamRef& ref) const;
    const ParamMap* params(const ParamRef& ref) const;
    TimeUs keyTime() const;  // keyframe time at the cursor (event-local for events)
    TimeUs keyToTimeline(TimeUs key) const;
    TimeUs keyTolerance() const;

    // Live edits collapse into one undo step; structural edits commit at once.
    void edit(const std::function<void(Timeline&)>& fn, const QString& label);
    void editNow(const std::function<void(Timeline&)>& fn, const QString& label);
    void editRoot(const std::function<void(EffectChain& root, Timeline& tl)>& fn, const QString& label);
    void flushEdit();

    // (key: always a keyframe at the cursor, as on an animated setting)
    void setParam(const ParamRef& ref, const QString& id, double value, bool key = false);
    void setParamText(const ParamRef& ref, const QString& id, const QString& text);
    void toggleKey(const ParamRef& ref, const ParamDef& def);
    void jumpKey(const ParamRef& ref, const QString& id, int direction);
    void setKeyInterp(const ParamRef& ref, const QString& id, Keyframe::Interp interp);
    keyops::Address keyAddress(const ParamRef& ref, const QString& id) const;
    void resetParam(const ParamRef& ref, const ParamDef& def);
    // Live tab: what moves the setting by itself (nullopt: nothing).
    void setModulation(const ParamRef& ref, const ParamDef& def, const std::optional<Modulation>& mod);
    // Keyframes that follow the loudness of a band of the timeline's sound.
    void followAudio(const ParamRef& ref, const ParamDef& def);
    // Freehand mask: a point removed, or one added after `index` at `insert` (the mask's own frame).
    void editMaskPoints(const ParamRef& ref, int index, std::optional<QPointF> insert);
    // Masks: on / off (an ellipse), the one shown on the picture, and Track Motion.
    void toggleMask(const ChainPath& path, int index);
    void updateMaskOverlay();
    // Returns what it says when done; quiet: says it only that way, no message boxes (AI control).
    QString trackMask(const ChainPath& path, int index, bool quiet = false);
    // Corner Pin (an event's): its four corners followed on the shot under the event, from the cursor on, as keyframes.
    QString trackCorners(const ChainPath& path, int index, bool quiet = false);
    // White Balance (an event's): Auto Colour from the event at the cursor, or the next click on the preview picks its neutral.
    void autoWhiteBalance(const ChainPath& path, int index);
    void pickNeutral(const ChainPath& path, int index);
    // Sets the neutral from the event at `at` (0..1 of the picture), as it looks before the effect.
    void setNeutralAt(const ChainPath& path, int index, QPointF at);

    // Structure.
    bool dropMime(const QMimeData* mime, const ChainPath& path, int index, bool copy);
    bool dropAsNewChain(const QMimeData* mime, const ChainPath& rackPath, int rackIndex, bool copy);
    bool canDrop(const QMimeData* mime) const;
    void removeDevice(const ChainPath& path, int index);
    void duplicateDevice(const ChainPath& path, int index);
    void setDeviceEnabled(const ChainPath& path, int index, bool on);
    void groupSelection();
    void ungroup(const ChainPath& path, int index);
    void addChain(const ChainPath& path, int rackIndex);
    void removeChain(const ChainPath& path, int rackIndex, int chain);
    void duplicateChain(const ChainPath& path, int rackIndex, int chain);
    void renameChain(const ChainPath& path, int rackIndex, int chain);
    void setChainFlag(const ChainPath& path, int rackIndex, int chain, bool solo, bool on);
    void selectChain(int rackId, int chain);
    void selectDevice(int effectId, bool toggle);
    void toggleFold(int effectId);
    void setDragging(bool on);
    void loadPreset(const ChainPath& path, int index, const QString& presetId);
    void savePreset(const ChainPath& path, int index);     // one device (a rack: as a chain preset)
    void saveChainPreset(const EffectChain& devices);     // asks for a name
    void saveSelectionPreset();

    void scheduleRebuild();
    void rebuild();
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override; // a new theme: the devices are built again in its colours
    void refreshValues();
    QString structureKey() const;

    Project* m_project;
    std::function<TimeUs()> m_cursor;
    FxTarget m_target;

    QLabel* m_title;
    QToolButton* m_addButton;
    QMenu* m_videoMenu = nullptr;
    QMenu* m_audioMenu = nullptr;
    QToolButton* m_groupButton;
    QToolButton* m_clearLiveButton;
    // Scope tabs: the selected event's chain, its track's chain, the output / master.
    QToolButton* m_scopeEvent;
    QToolButton* m_scopeTrack;
    QToolButton* m_scopeMain;
    int m_contextClip = 0;  // event the Event tab returns to
    int m_contextTrack = 0; // track the Track tab shows
    void updateScopeTabs();
    QScrollArea* m_scroll;
    int m_bodyHeight = 140; // height of a (top-level) device's controls
    std::vector<ParamControl*> m_controls;
    std::vector<QWidget*> m_displays; // the devices' pictures of their settings (ParamDisplay), drawn again as values change
    std::map<int, int> m_rackChain; // rack effect id -> chain shown
    std::set<int> m_folded;         // effect ids
    std::set<int> m_selected;       // effect ids
    PreviewWidget* m_preview = nullptr;
    std::unique_ptr<MaskOverlay> m_maskOverlay;
    std::unique_ptr<PreviewOverlay> m_picker; // Pick Neutral: the next click on the preview
    int m_maskEditing = 0;          // effect whose mask is on the picture
    QString m_key;

    QTimer m_commitTimer;
    QTimer m_rebuildTimer;
    std::optional<Timeline> m_editBefore;
    QString m_editLabel;
    bool m_applying = false;
    bool m_dragging = false;
};
