#include "ui/RenderQueue.h"

#include "core/FrameRenderer.h"
#include "core/Hardware.h"
#include "core/Project.h"
#include "ui/Help.h"
#include "ui/RenderStatusDialog.h"

#include <QDialogButtonBox>
#include <QJsonArray>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QOffscreenSurface>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

struct RenderQueue::Job {
    QString name;
    std::shared_ptr<const ProjectSnapshot> snapshot;
    ExportSettings settings;
    enum class State { Waiting, Rendering, Done, Failed, Cancelled } state = State::Waiting;
    QString error;
    bool ok = false;
    std::shared_ptr<RenderMonitor> monitor;
    QTreeWidgetItem* item = nullptr;
};

RenderQueue::RenderQueue(QWidget* parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Render Queue"));
    resize(760, 320);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(tr("Renders run one after another in the background. Each is of its project as it was when "
                                    "it was added, so you can go on editing. Add the open project with File > Render As > "
                                    "Add to Render Queue.")));
    m_list = new QTreeWidget;
    m_list->setObjectName(QStringLiteral("renderQueueList"));
    m_list->setHeaderLabels({tr("Project"), tr("Template"), tr("File"), tr("Status")});
    m_list->setRootIsDecorated(false);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    layout->addWidget(m_list, 1);
    auto* row = new QHBoxLayout;
    auto* addProject = new QPushButton(tr("Add Project File..."));
    m_remove = new QPushButton(tr("Remove"));
    auto* clear = new QPushButton(tr("Clear Finished"));
    m_start = new QPushButton(tr("Start"));
    m_stop = new QPushButton(tr("Stop"));
    m_start->setObjectName(QStringLiteral("renderQueueStart"));
    m_stop->setToolTip(tr("Cancels the render that is running (its half-written file is removed); the rest wait"));
    row->addWidget(addProject);
    row->addWidget(m_remove);
    row->addWidget(clear);
    row->addStretch(1);
    row->addWidget(m_start);
    row->addWidget(m_stop);
    auto* buttons = new QDialogButtonBox;
    help::addButton(buttons, QStringLiteral("rendering.md#render-queue"));
    row->addWidget(buttons);
    layout->addLayout(row);
    connect(addProject, &QPushButton::clicked, this, &RenderQueue::addProjectRequested);
    connect(m_start, &QPushButton::clicked, this, &RenderQueue::start);
    connect(m_stop, &QPushButton::clicked, this, &RenderQueue::stop);
    connect(m_remove, &QPushButton::clicked, this, [this] {
        Job* job = selectedJob();
        if (!job || job == m_current)
            return;
        delete job->item;
        std::erase_if(m_jobs, [job](const auto& j) { return j.get() == job; });
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        std::erase_if(m_jobs, [](const auto& j) {
            const bool finished = j->state != Job::State::Waiting && j->state != Job::State::Rendering;
            if (finished)
                delete j->item;
            return finished;
        });
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        Job* job = selectedJob();
        m_remove->setEnabled(job && job != m_current);
    });
    connect(&m_tick, &QTimer::timeout, this, &RenderQueue::poll);
    m_stop->setEnabled(false);
    m_remove->setEnabled(false);
}

RenderQueue::~RenderQueue()
{
    if (m_current)
        m_current->monitor->cancel = true;
    if (m_worker.joinable())
        m_worker.join();
}

bool RenderQueue::add(const QString& name, std::shared_ptr<const ProjectSnapshot> snapshot, const ExportSettings& settings, QString* error)
{
    const QString path = QFileInfo(settings.path).absoluteFilePath();
    for (const auto& j : m_jobs)
        if ((j->state == Job::State::Waiting || j->state == Job::State::Rendering)
            && QFileInfo(j->settings.path).absoluteFilePath().compare(path, Qt::CaseInsensitive) == 0) {
            if (error)
                *error = tr("%1 is already waiting to render to %2.").arg(j->name, QDir::toNativeSeparators(path));
            return false;
        }
    auto job = std::make_unique<Job>();
    job->name = name;
    job->snapshot = std::move(snapshot);
    job->settings = settings;
    job->item = new QTreeWidgetItem(m_list);
    refresh(*job);
    m_jobs.push_back(std::move(job));
    for (int c = 0; c < 2; ++c)
        m_list->resizeColumnToContents(c);
    return true;
}

int RenderQueue::waiting() const
{
    return int(std::count_if(m_jobs.begin(), m_jobs.end(), [](const auto& j) { return j->state == Job::State::Waiting; }));
}

void RenderQueue::start()
{
    m_running = true;
    if (!m_current)
        startNext();
}

void RenderQueue::stop()
{
    m_running = false;
    if (m_current)
        m_current->monitor->cancel = true;
}

void RenderQueue::startNext()
{
    const auto next = std::find_if(m_jobs.begin(), m_jobs.end(), [](const auto& j) { return j->state == Job::State::Waiting; });
    if (!m_running || next == m_jobs.end()) {
        m_running = false;
        m_start->setEnabled(true);
        m_stop->setEnabled(false);
        return;
    }
    Job* job = next->get();
    m_current = job;
    job->state = Job::State::Rendering;
    job->monitor = std::make_shared<RenderMonitor>();
    // The GPU surface is made here, on the GUI thread; the worker owns the GL context.
    m_surface = hw::settings().gpuCompositing ? createGpuSurface() : nullptr;
    QOffscreenSurface* surface = m_surface.get();
    m_worker = std::thread([job, surface] {
        Exporter exporter;
        job->ok = exporter.run(*job->snapshot, job->settings, [job](double) { return !job->monitor->cancel.load(); }, surface,
                               [job](const ExportStatus& value) { job->monitor->update(value); });
        job->error = job->ok ? QString() : exporter.error();
        job->monitor->done = true;
    });
    m_start->setEnabled(false);
    m_stop->setEnabled(true);
    m_remove->setEnabled(false);
    refresh(*job);
    m_tick.start(250);
}

void RenderQueue::poll()
{
    if (!m_current)
        return;
    Job& job = *m_current;
    if (!job.monitor->done) {
        refresh(job);
        return;
    }
    m_worker.join();
    m_surface.reset();
    m_tick.stop();
    job.state = job.ok ? Job::State::Done : job.monitor->cancel ? Job::State::Cancelled : Job::State::Failed;
    m_current = nullptr;
    refresh(job);
    if (job.ok)
        emit rendered(job.settings.path);
    startNext();
}

void RenderQueue::refresh(Job& job)
{
    QString status;
    switch (job.state) {
    case Job::State::Waiting: status = tr("Waiting"); break;
    case Job::State::Rendering: status = tr("Rendering %1%").arg(int(job.monitor->snapshot().progress * 100)); break;
    case Job::State::Done: status = tr("Done"); break;
    case Job::State::Cancelled: status = tr("Cancelled"); break;
    case Job::State::Failed: status = tr("Failed: %1").arg(job.error); break;
    }
    job.item->setText(0, job.name);
    const QString video = job.settings.videoEncoder.isEmpty() ? tr("audio") : job.settings.videoEncoder;
    job.item->setText(1, QStringLiteral("%1 %2, %3x%4").arg(QFileInfo(job.settings.path).suffix().toUpper(), video)
                             .arg(job.settings.width).arg(job.settings.height));
    job.item->setText(2, QDir::toNativeSeparators(job.settings.path));
    job.item->setText(3, status);
    job.item->setToolTip(3, status);
}

QJsonObject RenderQueue::status() const
{
    static const char* states[] = {"waiting", "rendering", "done", "failed", "cancelled"};
    QJsonArray jobs;
    for (const auto& j : m_jobs) {
        QJsonObject o{{QStringLiteral("name"), j->name},
                      {QStringLiteral("path"), j->settings.path},
                      {QStringLiteral("state"), QString::fromLatin1(states[int(j->state)])}};
        if (j->state == Job::State::Rendering)
            o[QStringLiteral("progress")] = j->monitor->snapshot().progress;
        if (!j->error.isEmpty())
            o[QStringLiteral("error")] = j->error;
        jobs.append(o);
    }
    return {{QStringLiteral("running"), m_running}, {QStringLiteral("jobs"), jobs}};
}

RenderQueue::Job* RenderQueue::selectedJob() const
{
    const auto items = m_list->selectedItems();
    for (const auto& j : m_jobs)
        if (!items.isEmpty() && j->item == items.front())
            return j.get();
    return nullptr;
}
