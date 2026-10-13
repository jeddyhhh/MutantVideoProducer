#pragma once

#include "core/Project.h"
#include "ui/FxTarget.h"

#include <QDialog>
#include <QTimer>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class FrameConverter;
class PanCropView;
class VideoDecoder;
class QFontComboBox;
class QPlainTextEdit;
class QLabel;
class QMenu;
class QLineEdit;
class QPushButton;
class QScrollArea;
class QSlider;
class QToolButton;
class QVBoxLayout;
class PreviewWidget;
class PropertyOverlay;

// Non-modal editor window for an event's transition, generated media (titles)
// or pan/crop. Effect chains are edited in the DeviceChainView. Changes
// preview live; each burst of edits becomes one undo step.
class EffectEditor : public QDialog {
    Q_OBJECT
public:
    EffectEditor(Project* project, std::function<TimeUs()> cursor, QWidget* parent = nullptr);
    ~EffectEditor() override;

    // TransitionIn/Out, Generator or PanCrop.
    void setTarget(const FxTarget& target);
    const FxTarget& target() const { return m_target; }

    void onCursorMoved();
    // In a panel of the main window instead of its own window: setTarget asks
    // to be shown (opened), Remove Transition to be put away (closed).
    void setEmbedded(bool embedded);
    // The editor's preview: positions, angles and sizes are dragged and
    // scrolled right on the picture while this shows.
    void setPreview(PreviewWidget* preview);
    // What the preview handles move (tests): position ids, angle id, size id.
    QStringList handles() const;

signals:
    void seekRequested(TimeUs t);
    void opened();
    void closed();

protected:
    void hideEvent(QHideEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void reject() override;

private:
    struct Row {
        QString id;
        ParamType type = ParamType::Number;
        double min = 0, max = 1;
        QSlider* slider = nullptr;
        QDoubleSpinBox* spin = nullptr;
        QComboBox* combo = nullptr;
        QCheckBox* check = nullptr;
        QPushButton* colorButton = nullptr;
        QLineEdit* text = nullptr;
        QPlainTextEdit* multiText = nullptr;
        QFontComboBox* font = nullptr;
        QToolButton* keyButton = nullptr;
        QToolButton* prevKey = nullptr;
        QToolButton* nextKey = nullptr;
    };

    friend class PropertyOverlay;
    void updateOverlay();
    double paramNow(const QString& id) const;
    QSizeF panCropScale() const;
    const ParamDef* paramDef(const QString& id) const;
    bool isTransition() const;
    bool isGenerator() const { return m_target.kind == FxTarget::Kind::Generator; }
    bool isPanCrop() const { return m_target.kind == FxTarget::Kind::PanCrop; }
    bool isEventLocal() const { return isGenerator() || isPanCrop(); }
    void updatePanCropView();
    bool targetExists() const;
    TransitionSpec* transition(Timeline& tl) const;
    const TransitionSpec* transition(const Timeline& tl) const;
    const std::vector<ParamDef>* currentDefs() const;
    const ParamMap* currentParams() const;
    ParamMap* currentParams(Timeline& tl) const;
    TimeUs keyTime() const;      // keyframe time at the cursor (event-local for events)
    TimeUs keyToTimeline(TimeUs key) const;
    TimeUs keyTolerance() const;

    void rebuild();              // title, transition type + parameter form
    void rebuildParams();
    void refreshValues();        // values/keyframe markers at the cursor
    void setParam(const QString& id, double value);
    void setParamText(const QString& id, const QString& text);
    void toggleKey(const QString& id);
    void jumpKey(const QString& id, int direction);
    void setKeyInterp(const QString& id, Keyframe::Interp interp);

    // Live edit; committed as one undo step after a short pause.
    void edit(const std::function<void(Timeline&)>& fn, const QString& label);
    // Structural change (e.g. transition type): committed immediately.
    void editNow(const std::function<void(Timeline&)>& fn, const QString& label);
    void flushEdit();

    Project* m_project;
    std::function<TimeUs()> m_cursor;
    FxTarget m_target;

    QLabel* m_title;
    QComboBox* m_transitionType;
    QLabel* m_description;
    QScrollArea* m_scroll;
    QWidget* m_form = nullptr;
    std::vector<Row> m_rows;

    PanCropView* m_panCropView = nullptr;
    QPushButton* m_resetButton = nullptr;
    QPushButton* m_removeTransition = nullptr;
    std::unique_ptr<VideoDecoder> m_pcDecoder;
    std::unique_ptr<FrameConverter> m_pcConverter;
    QString m_pcPath;
    int m_pcStream = -1;

    QTimer m_commitTimer;
    std::optional<Timeline> m_editBefore;
    QString m_editLabel;
    bool m_applying = false;
    bool m_updatingUi = false;
    bool m_embedded = false;
    PreviewWidget* m_preview = nullptr;
    std::unique_ptr<PropertyOverlay> m_overlay;
};
