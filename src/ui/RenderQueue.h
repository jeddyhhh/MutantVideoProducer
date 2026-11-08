#pragma once
// File > Render Queue: renders waiting one after another in the background, each of a project as it
// was when it was added (the open project, or another project file).

#include "core/Exporter.h"

#include <QJsonObject>
#include <QTimer>
#include <QWidget>

#include <memory>
#include <thread>
#include <vector>

class QOffscreenSurface;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
struct ProjectSnapshot;
struct RenderMonitor;

class RenderQueue : public QWidget {
    Q_OBJECT
public:
    explicit RenderQueue(QWidget* parent = nullptr);
    ~RenderQueue() override; // cancels a running render and waits for it

    // False (with `error`) when another waiting job already writes `settings.path`. `projectFile`: the project it is
    // of (where its render is remembered, whatever is open when it is done).
    bool add(const QString& name, std::shared_ptr<const ProjectSnapshot> snapshot, const ExportSettings& settings, const QString& projectFile,
             QString* error = nullptr);
    bool busy() const { return m_current != nullptr; }
    int waiting() const;
    void start();
    void stop(); // cancels the running render; the rest wait
    QJsonObject status() const; // the jobs: name, file, state, progress, error

signals:
    void addProjectRequested(); // "Add Project File..."
    void rendered(const QString& path, const QString& projectFile);

private:
    struct Job;
    void startNext();
    void poll();
    void refresh(Job& job);
    Job* selectedJob() const;

    std::vector<std::unique_ptr<Job>> m_jobs;
    Job* m_current = nullptr;
    bool m_running = false;
    std::thread m_worker;
    std::unique_ptr<QOffscreenSurface> m_surface;
    QTimer m_tick;
    QTreeWidget* m_list;
    QPushButton *m_start, *m_stop, *m_remove;
};
