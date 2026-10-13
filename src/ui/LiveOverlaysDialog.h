#pragma once

#include <QDialog>
#include <functional>

class Project;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;
class QToolButton;
struct LiveOverlay;

// The Live tab's overlays: text, a picture (a logo), the tempo or the time
// drawn over the output (and recorded with it). Saved with the project;
// changes show at once and can be undone.
class LiveOverlaysDialog : public QDialog {
    Q_OBJECT
public:
    explicit LiveOverlaysDialog(Project* project, QWidget* parent = nullptr);

private:
    void refresh();
    void showSelected();
    void change(const std::function<void(LiveOverlay&)>& fn, const QString& label);
    void add(int kind);

    Project* m_project;
    QListWidget* m_list;
    QWidget* m_editor;
    QCheckBox* m_enabled;
    QLineEdit* m_text;
    QLineEdit* m_image;
    QToolButton* m_browse;
    QComboBox* m_anchor;
    QDoubleSpinBox* m_size;
    QDoubleSpinBox* m_width;
    QPushButton* m_color;
    QSlider* m_opacity;
    QCheckBox* m_pulse;
    bool m_updating = false;
};
