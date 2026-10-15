#pragma once

#include "core/KeyframeOps.h"
#include "ui/FxTarget.h"

#include <QIcon>
#include <QWidget>
#include <functional>
#include <map>
#include <optional>
#include <set>

class QDoubleSpinBox;
class QLabel;
class QToolButton;
class KeyframeCanvas;

// The Keyframe Editor (the Keyframes tab beside Effects and Properties): every keyframed setting
// of the selected event (its FX with racks, Pan/Crop, generated media, transitions), of a track's
// FX, the video output or the audio master, laid out over time.
// - Dope Sheet: one row per setting, keyframes as marks. Click, Shift/Ctrl+click or drag a box to
//   pick them; drag to move (Alt: copies), drag the ends of the bar over the ruler to stretch;
//   a group's row moves all of its settings' keyframes at once.
// - Graph: the curves, normalised to each setting's range. Drag keyframes in time and value
//   (Shift: one direction) and the handles of Bezier curves (kept in line; Alt: separately).
// Curves (1-9), Smooth handles, copy and paste (into another event's matching settings too),
// reverse, nudging and typed times and values work on the picked keyframes in both. Each drag
// or command is one undo step.
class KeyframeEditor : public QWidget {
    Q_OBJECT
public:
    KeyframeEditor(Project* project, std::function<TimeUs()> cursor, QWidget* parent = nullptr);
    ~KeyframeEditor() override;
    // Clip, Track, Output or AudioMaster (the panel follows the Effects tab); anything else clears it.
    void setTarget(const FxTarget& target);
    const FxTarget& target() const { return m_target; }
    // Shows one setting (from a control's right-click menu), keyframes or not, and picks its keyframes.
    void focusSetting(const keyops::Address& address);
    void onCursorMoved();
    // Developer check (MutantVideoProducer --devices-test): drives the editor with synthetic mouse and
    // key events on an event of the project; adds PASS/FAIL lines.
    int runSelfTest(QStringList& log);

signals:
    void seekRequested(TimeUs t);

protected:
    void changeEvent(QEvent* e) override;

private:
    friend class KeyframeCanvas;
    using Times = keyops::Times;
    struct Row {
        bool header = false;
        QString group;
        int channel = -1; // index into m_channels
    };
    struct Hit {
        enum Kind { None, Key, Summary, HandleOut, HandleIn, ScaleLeft, ScaleRight, ListName, ListPrev, ListKey, ListNext, Header };
        Kind kind = None;
        int row = -1;
        int channel = -1;
        TimeUs time = 0; // Key, Handle*: the keyframe's (channel) time; Summary: timeline time
    };
    struct Drag {
        enum Kind { None, Seek, Marquee, Move, Handle, Scale, Pan };
        Kind kind = None;
        QPoint start, last;
        std::optional<Timeline> before;
        std::map<QString, Times> picked; // as the drag began
        TimeUs grab = 0;                 // Move: the grabbed keyframe's timeline time
        double grabValue = 0;            // Move (graph): its normalised value
        bool duplicate = false, changed = false;
        int axis = 0;                    // Graph Move with Shift: 0 not decided yet, 1 time, 2 value
        int channel = -1;                // Handle: which keyframe and side
        TimeUs key = 0;
        bool out = true, aligned = true;
        TimeUs pivot = 0, end = 0;       // Scale
        TimeUs t0 = 0;                   // Pan
        double v0 = 0, v1 = 0;
        int scroll = 0;
        bool additive = false;           // Marquee
    };

    // ---- Model ----
    void refresh();                       // channels, rows, picks after any change
    void rebuildRows();
    const keyops::Channel* channel(int index) const;
    int channelIndex(const QString& key) const;
    const ParamValue* values(int channel) const;
    std::vector<int> graphChannels() const;
    std::pair<TimeUs, TimeUs> span() const; // the target's timeline span (event, or the project)
    TimeUs snapFrame(TimeUs t) const;
    TimeUs channelSnap(const keyops::Channel& ch, TimeUs key) const;
    double normalize(const keyops::Channel& ch, double v) const;
    double denormalize(const keyops::Channel& ch, double n) const;
    QString formatValue(const keyops::Channel& ch, double v) const;
    QColor channelColor(int channel) const;
    int pickedCount() const;
    void prunePicked();

    // ---- Edits ----
    void change(const QString& label, const std::function<void(Timeline&)>& fn); // one undo step
    void forPicked(Timeline& tl, const std::function<void(const keyops::Channel&, ParamValue&, const Times&)>& fn);
    void setInterp(Keyframe::Interp interp);
    void smoothHandles();
    void deletePicked();
    void reversePicked();
    void nudgeTime(int frames);
    void nudgeValue(double fraction);
    void setPickedTime(double seconds);
    void setPickedValue(double value);
    void toggleKeyAtCursor(int channel);
    void addKeyAt(int channel, TimeUs timelineTime);
    void jumpKey(int channel, int direction);
    void copyPicked(bool cut);
    void paste(TimeUs at, int fallbackChannel);
    void selectAll();
    void fit();
    void updateFields();

    // ---- Canvas (KeyframeCanvas forwards its events here) ----
    QRect lanes() const;  // keyframe area right of the list, under the ruler
    int rowTop(int row) const;
    int rowAt(int y) const;
    double timeToX(TimeUs t) const;
    TimeUs xToTime(double x) const;
    double valueToY(double n) const;
    double yToValue(double y) const;
    QPointF keyPoint(int channel, const Keyframe& k) const; // graph
    std::pair<QPointF, QPointF> handlePoints(int channel, size_t index) const; // out of key index, in of key index+1
    bool scaleBar(double& x0, double& x1) const;
    Hit hitTest(const QPoint& p) const;
    void paint(QPainter& p);
    void paintList(QPainter& p);
    void paintRuler(QPainter& p);
    void paintDope(QPainter& p);
    void paintGraph(QPainter& p);
    void drawKey(QPainter& p, QPointF c, Keyframe::Interp interp, bool picked, double size) const;
    void mousePress(QMouseEvent* e);
    void mouseMove(QMouseEvent* e);
    void mouseRelease(QMouseEvent* e);
    void mouseDouble(QMouseEvent* e);
    void wheel(QWheelEvent* e);
    void keyPress(QKeyEvent* e);
    void contextMenu(const QPoint& pos, const QPoint& global);
    void applyDrag(const QPoint& pos, Qt::KeyboardModifiers mods);
    void finishDrag();
    void selectInRect(const QRect& r, bool additive);
    QIcon curveIcon(Keyframe::Interp interp) const;
    void updateIcons();

    Project* m_project;
    std::function<TimeUs()> m_cursor;
    FxTarget m_target;
    std::vector<keyops::Channel> m_channels;
    std::vector<Row> m_rows;
    std::set<QString> m_collapsed;      // groups
    std::set<QString> m_shown;          // settings shown although they have no keyframes
    std::set<QString> m_focus;          // graph: settings picked in the list (none: all animated)
    std::map<QString, Times> m_picked;  // channel key -> keyframe (channel) times
    bool m_graph = false, m_animatedOnly = true, m_snap = true, m_applying = false;
    // View: timeline time at the left edge of the lanes and per pixel; graph values shown; list scroll.
    TimeUs m_t0 = 0;
    double m_usPerPx = 10000;
    double m_v0 = -0.05, m_v1 = 1.05;
    int m_scroll = 0;
    bool m_viewSet = false;
    Drag m_drag;
    QPoint m_menuPos;

    KeyframeCanvas* m_canvas;
    QLabel* m_title;
    QToolButton* m_dopeButton;
    QToolButton* m_graphButton;
    QToolButton* m_animatedButton;
    QToolButton* m_snapButton;
    QToolButton* m_smoothButton;
    std::vector<QToolButton*> m_curveButtons;
    QDoubleSpinBox* m_timeSpin;
    QDoubleSpinBox* m_valueSpin;
};
