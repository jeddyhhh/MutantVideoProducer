#pragma once

#include <QPointer>
#include <QWidget>
#include <memory>

class Project;
class Vst3Plugin;
class QTimer;

// A VST3 Plugin effect's own window: the plugin's editor, running its own
// instance. What is changed in it is saved into the effect's state (one
// undo step per pause in the editing), which the playing instances follow.
class Vst3Window : public QWidget {
public:
    // Opens (or raises) the window of effect `effectId`.
    static void open(Project* project, int effectId, QWidget* parent);
    ~Vst3Window() override;

protected:
    void closeEvent(QCloseEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    friend class MainWindow; // editor regression checks cover session invalidation
    Vst3Window(Project* project, int effectId, QWidget* parent);
    void save();

    QPointer<Project> m_project;
    std::shared_ptr<const int> m_session;
    int m_effectId;
    std::unique_ptr<Vst3Plugin> m_plugin;
    QTimer* m_timer;
    bool m_resizing = false;
};
