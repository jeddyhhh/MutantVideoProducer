#pragma once
// The Deliver tab: everything for getting a finished edit out, in one place. Checks before rendering
// (offline media, gaps, loudness, YouTube chapters, platform length limits), several outputs rendered
// through the render queue, the publish pack (title, thumbnail, description, captions) and what was
// rendered before.

#include "core/AudioFinishing.h"
#include "core/Time.h"

#include <QImage>
#include <QWidget>

#include <functional>
#include <map>
#include <optional>

class Project;
class RenderQueue;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;
struct ExportPreset;

class DeliverView : public QWidget {
    Q_OBJECT
public:
    struct Hooks {
        std::function<RenderQueue*()> queue;
        std::function<TimeUs()> cursor;
        std::function<void(TimeUs)> seek;
        std::function<bool()> save;
        std::function<void()> audioFinishing;
    };
    DeliverView(Project* project, Hooks hooks, QWidget* parent = nullptr);

    void refresh(); // the checks, file names and history, for the project as it is now

    // A render to list in History (any render: the queue, Render As, AI control).
    static void remember(const QString& projectPath, const QString& file, const QString& templateName = {});
    // The template names ticked for a project (saved per project file).
    QStringList outputs() const;
    void setOutputs(const QStringList& templateNames);
    QString outputPath(const ExportPreset& preset) const;
    int renderOutputs(QString* error = nullptr); // queues the ticked outputs and starts the queue; returns how many
    bool savePack(QString* error = nullptr);      // description, thumbnail and captions into the output folder

protected:
    void showEvent(class QShowEvent* e) override;

private:
    void runChecks();
    void fillOutputs();
    void fillHistory();
    void pickThumbnail();
    void pollQueue();
    QString baseName() const;
    QString settingsKey(const QString& what) const;
    TimeUs limitFor(const ExportPreset& p) const; // the trim a check set for a template, or 0

    Project* m_project;
    Hooks m_hooks;
    std::optional<finishing::Report> m_loudness;
    std::map<QString, TimeUs> m_trim; // template name -> render only the first part
    QTreeWidget *m_checks, *m_outputs, *m_history;
    QComboBox *m_addTemplate, *m_listingTrack;
    QLineEdit *m_folder, *m_title;
    QPlainTextEdit* m_description;
    QLabel* m_thumbLabel;
    QImage m_thumbFrame, m_thumb; // the cursor frame, and it as a 1280x720 thumbnail with the title
    QPushButton* m_render;
    class QTimer* m_poll;
    std::map<QString, QString> m_pending; // queued file -> template name, for History
    bool m_queueConnected = false, m_filling = false;
};
