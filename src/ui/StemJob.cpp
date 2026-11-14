#include "ui/StemJob.h"

#include "core/AiRuntime.h"
#include "core/FileNames.h"
#include "core/MediaInfo.h"
#include "core/Profiles.h"
#include "core/StemSeparation.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QUuid>

StemJob::StemJob(Project* project, QObject* parent) : QObject(parent), m_project(project)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &StemJob::readOutput);
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        readOutput();
        const QString detail = QString::fromUtf8(m_process.readAllStandardError()).right(1500);
        finish(m_cancelled ? tr("Cancelled")
               : exit != QProcess::NormalExit || code != 0 ? (m_error.isEmpty() ? tr("Stem separation failed. %1").arg(detail) : m_error)
                                                           : QString());
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(tr("Could not start the stem separation engine: %1").arg(m_process.errorString()));
    });
}

StemJob::~StemJob()
{
    m_process.disconnect(this);
    m_process.kill();
    m_process.waitForFinished(3000);
}

bool StemJob::start(int clipId, int stems, bool best, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    if (m_busy)
        return fail(tr("A separation is already running."));
    const QString program = stems::workerPath();
    if (program.isEmpty())
        return fail(tr("Install the Stem Separation engine first (Tools > AI Components)."));
    const auto snap = m_project->snapshot();
    const Clip* clip = snap->timeline.findClip(clipId);
    if (!clip)
        return fail(tr("There is no event %1.").arg(clipId));
    // Next to the project (its Stems folder), else in the profile's media folder.
    const QString home = m_project->filePath().isEmpty() ? Profiles::instance().folder(QStringLiteral("Media"))
                                                         : QFileInfo(m_project->filePath()).absolutePath();
    const auto media = m_project->mediaItem(clip->mediaId);
    const QString name = safeFileName(media ? QFileInfo(media->path).completeBaseName() : QString(), QStringLiteral("event"));
    m_folder = QDir(home).filePath(QStringLiteral("Stems/%1 %2").arg(name, QUuid::createUuid().toString(QUuid::Id128).left(6)));
    if (!QDir().mkpath(m_folder))
        return fail(tr("Can't make the folder %1").arg(QDir::toNativeSeparators(m_folder)));
    m_input = QDir(m_folder).filePath(name + QStringLiteral(".wav"));
    QString why;
    if (!stems::writeEventWav(*snap, *clip, m_input, &why))
        return fail(why);
    const QJsonObject request{{"input", m_input},
                              {"output", m_folder},
                              {"stems", stems == 4 ? 4 : 2},
                              {"model", best ? "htdemucs_ft" : "htdemucs"},
                              {"device", "cuda"}, // (the worker uses the CPU when there is no CUDA)
                              {"models", QDir(aiRuntimeRoot()).filePath(QStringLiteral("models/demucs"))}};
    QSaveFile file(QDir(m_folder).filePath(QStringLiteral("request.json")));
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(request).toJson()) < 0 || !file.commit())
        return fail(tr("Can't write the request in %1").arg(QDir::toNativeSeparators(m_folder)));
    m_clipId = clipId;
    m_session = m_project->session();
    m_count = stems == 4 ? 4 : 2;
    m_busy = true;
    m_done = m_ok = m_cancelled = false;
    m_error.clear();
    m_pending.clear();
    m_stems = {};
    m_made = {};
    m_progress = 0;
    m_message = tr("Starting the stem separation engine");
    QStringList arguments = stems::workerArguments(program);
    arguments << QDir(m_folder).filePath(QStringLiteral("request.json"));
    m_process.start(program, arguments);
    return true;
}

void StemJob::cancel()
{
    if (!m_busy)
        return;
    m_cancelled = true;
    m_process.kill();
}

QJsonObject StemJob::status() const
{
    QJsonObject o{{"busy", m_busy}, {"progress", m_progress}, {"message", m_message}, {"done", m_done}};
    if (m_done) {
        o["ok"] = m_ok;
        if (!m_error.isEmpty())
            o["error"] = m_error;
        o["event_ids"] = m_made;
        o["folder"] = m_folder;
    }
    return o;
}

void StemJob::readOutput()
{
    m_pending += m_process.readAllStandardOutput();
    while (m_pending.contains('\n')) {
        const int end = m_pending.indexOf('\n');
        const QJsonObject o = QJsonDocument::fromJson(m_pending.left(end)).object();
        m_pending.remove(0, end + 1);
        if (o.contains("progress")) {
            m_progress = std::clamp(o["progress"].toDouble(), 0.0, 1.0);
            m_message = o["message"].toString();
            emit progress(m_progress, m_message);
        }
        if (o.contains("error"))
            m_error = o["error"].toString();
        if (o["done"].toBool())
            m_stems = o["stems"].toObject();
    }
    if (m_pending.size() > 65536)
        m_pending.clear();
}

void StemJob::finish(const QString& failure)
{
    if (!m_busy)
        return;
    QString error = failure;
    const QStringList order = m_count == 4 ? QStringList{"vocals", "drums", "bass", "other"} : QStringList{"voice", "background"};
    if (error.isEmpty() && m_project->session() != m_session)
        error = tr("The project was closed before the stems were ready. They are in %1").arg(QDir::toNativeSeparators(m_folder));
    if (error.isEmpty() && !m_project->timeline().findClip(m_clipId))
        error = tr("The event was deleted before the stems were ready. They are in %1").arg(QDir::toNativeSeparators(m_folder));
    if (error.isEmpty()) {
        std::vector<MediaInfo> infos; // (all read before any is added: none left in Project Media when one is missing)
        QStringList names;
        for (const QString& key : order) {
            const QString path = m_stems.value(key).toString();
            const MediaInfo info = path.isEmpty() ? MediaInfo() : probeMedia(path);
            if (!info.ok() || !info.hasAudio()) {
                error = tr("The engine didn't write the %1 stem.").arg(key);
                break;
            }
            infos.push_back(info);
            names << key;
        }
        if (error.isEmpty()) {
            std::vector<int> ids;
            for (const MediaInfo& info : infos)
                ids.push_back(m_project->addMedia(info));
            Timeline tl = m_project->timeline();
            QString why;
            const std::vector<int> made = stems::place(tl, m_project->media(), m_clipId, ids, names, &why);
            if (made.empty())
                error = why;
            else {
                m_project->applyEdit(tl, tr("Separate Stems"));
                for (int id : made)
                    m_made.append(id);
            }
        }
    }
    QFile::remove(m_input); // (the event's sound, written for the engine)
    m_busy = false;
    m_done = true;
    m_ok = error.isEmpty();
    m_error = error;
    m_progress = m_ok ? 1.0 : m_progress;
    emit completed(m_ok, error);
}
