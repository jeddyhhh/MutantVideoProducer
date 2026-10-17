#pragma once
// The Deliver tab: everything for getting a finished edit out, in one place. A header with how ready
// the edit is, an overview strip of the edit (frames, chapters, gaps; click a frame for the thumbnail),
// checks with fixes and a loudness meter, outputs rendered together through the render queue (with
// size estimates, packs and progress), the publish pack (title, thumbnail, description, tags,
// captions) and every render made before.

#include "core/AudioFinishing.h"
#include "core/Time.h"

#include <QImage>
#include <QWidget>

#include <functional>
#include <map>
#include <memory>
#include <optional>

class Project;
class RenderQueue;
class QComboBox;
class QElapsedTimer;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QVBoxLayout;
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
    ~DeliverView() override;

    void refresh(); // the checks, outputs, strip and history, for the project as it is now

    // A render to list in History (any render: the queue, Render As, AI control).
    static void remember(const QString& projectPath, const QString& file, const QString& templateName = {}, double seconds = 0);
    // The template names listed for a project (saved per project file), ticked or not.
    QStringList outputs() const;
    void setOutputs(const QStringList& templateNames);
    QString outputPath(const ExportPreset& preset) const;
    int renderOutputs(QString* error = nullptr); // queues the ticked outputs and starts the queue; returns how many
    bool savePack(QString* error = nullptr);      // description, thumbnail and captions into the output folder
    int problems() const { return m_problems; }   // checks worth a look
    // The description with {title}, {chapters}, {tracklist}, {length} and {date} filled in.
    QString expandedDescription() const;
    // A file size estimate for a template over `length`, in bytes; `exact` false when it depends on the picture.
    static double estimateBytes(const ExportPreset& p, int width, int height, double fps, TimeUs length, bool* exact = nullptr);

protected:
    void showEvent(class QShowEvent* e) override;

private:
    struct OutputRow;
    void runChecks();
    void fillOutputs();
    void fillHistory();
    void updateHeader();
    void updateTotals();
    void pickThumbnail(TimeUs t);
    void pollQueue();
    void rebuildStrip();
    QString baseName() const;
    QString settingsKey(const QString& what) const;
    TimeUs limitFor(const ExportPreset& p) const; // the trim a check set for a template, or 0

    Project* m_project;
    Hooks m_hooks;
    std::optional<finishing::Report> m_loudness;
    std::map<QString, TimeUs> m_trim; // template name -> render only the first part
    std::vector<std::unique_ptr<OutputRow>> m_rows;
    int m_problems = 0;

    // Header
    QLabel *m_projectName, *m_projectInfo, *m_readiness, *m_renderStatus;
    QPushButton *m_render, *m_openWhenDone;
    QProgressBar* m_overall;
    // Strip
    class DeliverStrip* m_strip;
    // Checks
    QVBoxLayout* m_checkList;
    QComboBox* m_target;
    class LoudnessMeter* m_meter;
    // Outputs
    QVBoxLayout* m_outputList;
    QComboBox* m_addTemplate;
    QLineEdit* m_folder;
    QLabel* m_totals;
    // Publish pack
    QLineEdit *m_title, *m_tags;
    QPlainTextEdit* m_description;
    QComboBox* m_listingTrack;
    QLabel *m_thumbLabel, *m_descCount, *m_tagCount, *m_packStatus;
    QImage m_thumbFrame, m_thumb; // the chosen frame, and it as a 1280x720 thumbnail with the title
    TimeUs m_thumbTime = -1;
    // History
    QTreeWidget* m_history;

    std::unique_ptr<class Compositor> m_stripRenderer; // the strip's frames (its own decoders)
    int m_stripGeneration = 0;                         // a newer strip stops the frames of an older one
    class QTimer *m_poll, *m_again;
    QElapsedTimer* m_renderClock;
    std::map<QString, std::pair<QString, double>> m_pending; // queued file -> template name, seconds
    int m_batch = 0;                                          // outputs in the batch being rendered
    bool m_queueConnected = false, m_filling = false;
};
