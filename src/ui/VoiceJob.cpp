#include "core/Brand.h"
#include "ui/VoiceJob.h"
#include "core/AiRuntime.h"
#include "core/Profiles.h"
#include "core/MediaInfo.h"
#include "core/AudioDecoder.h"
#include "core/TimelineOps.h"
#include "core/Generators.h"
#include <QRegularExpression>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#endif
namespace {
QJsonObject assetJson(const VoiceAsset& a)
{
    return {{"file", a.file}, {"path", voiceAssetPath(a)}, {"sha256", a.sha256}, {"size", double(a.size)}};
}
} // namespace
VoiceJob::VoiceJob(Project* project, QObject* parent) : QObject(parent), m_project(project)
{
    connect(&m_process, &QProcess::readyReadStandardOutput, this, &VoiceJob::readOutput);
    connect(&m_process, &QProcess::started, this, [this] {
#ifdef _WIN32
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        HANDLE process = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, DWORD(m_process.processId()));
        if (job && process && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))
            && AssignProcessToJobObject(job, process))
            m_processTree = job;
        else if (job)
            CloseHandle(job);
        if (process)
            CloseHandle(process);
#endif
    });
    connect(&m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this, [this](int code, QProcess::ExitStatus exit) {
        readOutput();
        const QString detail = QString::fromUtf8(m_process.readAllStandardError()).right(2000);
        finish(m_cancelled                                     ? tr("Cancelled")
                   : exit != QProcess::NormalExit || code != 0 ? (m_error.isEmpty() ? tr("Voice worker failed. %1").arg(detail) : m_error)
                                                               : QString());
    });
    connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart)
            finish(tr("Could not start the voice worker: %1").arg(m_process.errorString()));
    });
}
VoiceJob::~VoiceJob()
{
    cancel();
    m_alignmentCancel = true;
    if (m_alignment.joinable())
        m_alignment.join();
    m_process.disconnect(this);
    m_process.kill();
    m_process.waitForFinished(3000);
#ifdef _WIN32
    if (m_processTree)
        CloseHandle(HANDLE(m_processTree));
#endif
}
QString VoiceJob::workerPath()
{
    const QString overridePath = qEnvironmentVariable("MVP_VOICE_WORKER");
    if (!overridePath.isEmpty())
        return overridePath;
    return aiRuntimeWorkerPath(QStringLiteral("speech"),
                               QCoreApplication::applicationDirPath() + QStringLiteral("/voice/worker/mvp-voice-worker")
#ifdef _WIN32
                                   + QStringLiteral(".exe")
#endif
    );
}
bool VoiceJob::start(const std::vector<Caption>& captions, const LocalVoice& voice, const QJsonObject& options, bool preview,
                     QString* error)
{
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_busy)
        return fail(tr("Voice generation is already running."));
    if (!QFileInfo::exists(workerPath()))
        return fail(tr("Install the Speech Generation component in Tools > AI Components first."));
    const VoiceAsset model = kokoroModelAsset();
    if (!verifyVoiceAsset(voiceAssetPath(model), model) || !verifyVoiceAsset(voiceAssetPath(voice.asset), voice.asset))
        return fail(tr("Download the Kokoro model and this voice in Tools > Captions > Generate Voice first."));
    const auto segments = voiceSentences(captions, options.value("group").toBool(true));
    if (segments.empty() || segments.size() > 10000)
        return fail(tr("There are no captions to speak (or too many captions)."));
    const double rate = options.value("rate").toDouble(1);
    const QString timing = options.value("timing").toString(QStringLiteral("fit"));
    if (!std::isfinite(rate) || rate < 0.5 || rate > 2 || (timing != QLatin1String("fit") && timing != QLatin1String("natural")))
        return fail(tr("Use a speech rate from 0.5 to 2 and fit or natural timing."));
    QJsonObject rvc = options.value("rvc").toObject();
    if (!rvc.empty() && rvc.value("engine").toString() != QLatin1String("external")) {
        if (!QFileInfo::exists(rvcWorkerPath()))
            return fail(tr("Install the RVC Conversion component in Tools > AI Components first."));
        const QString path = rvc.value("model").toString();
        if (!QFileInfo::exists(path) || QFileInfo(path).suffix().compare(QStringLiteral("pth"), Qt::CaseInsensitive))
            return fail(tr("Import an RVC .pth voice in Tools > Captions > Generate Voice."));
        const auto assets = rvcSharedAssets();
        if (assets.size() != 4)
            return fail(tr("The RVC support model catalog is missing from this build."));
        QJsonArray list;
        for (const auto& a : assets) {
            if (!verifyVoiceAsset(voiceAssetPath(a), a))
                return fail(tr("Download the RVC support models in Tools > Captions > Generate Voice > RVC Conversion."));
            list.append(assetJson(a));
        }
        rvc["engine"] = "bundled";
        rvc["worker"] = rvcWorkerPath();
        rvc["assets"] = list;
        rvc["worker_args"] = QJsonArray::fromStringList(aiRuntimeWorkerArguments(QStringLiteral("rvc"), rvcWorkerPath()));
    }
    const int replacement = options.value("replace_event").toInt();
    if (replacement) {
        int ti = 0;
        const auto* clip = m_project->timeline().findClip(replacement, &ti);
        if (!clip || segments.size() != 1 || m_project->timeline().tracks[size_t(ti)].type != TrackType::Audio || clip->isSequence()
            || clip->isGenerated() || !m_project->mediaItem(clip->mediaId))
            return fail(tr("Choose one existing audio narration event to regenerate."));
    }
    if (options.value("align_captions").toBool() && !QFileInfo::exists(options.value("alignment_model").toString()))
        return fail(tr("Install a Whisper speech model before aligning generated captions."));
    m_options = options;
    m_backends = {};
    if (replacement)
        if (const auto* c = m_project->timeline().findClip(replacement); c && c->narration.contains("chapter_id"))
            m_options["chapter_id"] = c->narration["chapter_id"];
    if (m_options.contains("chapter_id"))
        m_options["ripple"] = false; // the fixed chapter narration/listening boundary owns the time
    m_voiceId = voice.id;
    m_createdIds.clear();
    m_wordCaptions.clear();
    m_alignmentReady = m_alignmentRunning = false;
    m_alignmentCancel = false;
    m_before = m_project->timeline();
    m_settings = m_project->settings();
    m_projectFile = m_project->filePath();
    QString base = options.value("output_dir").toString();
    if (base.isEmpty())
        base = Profiles::instance().folder(QStringLiteral("Voice"));
    if (base.isEmpty()) {
        base = m_projectFile.isEmpty()
            ? brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Voice"))
            : QFileInfo(m_projectFile).dir().filePath(QFileInfo(m_projectFile).completeBaseName() + QStringLiteral("-voice"));
    }
    m_folder = QDir(base).absoluteFilePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    if (!QDir().mkpath(m_folder))
        return fail(tr("Could not create the generated voice folder."));
    QJsonArray list;
    for (const auto& c : segments)
        list.append(QJsonObject{{"start", double(c.start) / kUsPerSecond}, {"end", double(c.end) / kUsPerSecond}, {"text", c.text}});
    QJsonObject v = assetJson(voice.asset);
    v["id"] = voice.id;
    v["language"] = voice.language;
    QJsonObject request{{"version", 1}, {"model", assetJson(model)}, {"voice", v}, {"segments", list}, {"rate", rate}, {"timing", timing}};
    const auto gpuModel = kokoroGpuModelAsset();
    if (verifyVoiceAsset(voiceAssetPath(gpuModel), gpuModel))
        request["gpu_model"] = assetJson(gpuModel);
    for (const QString& key : {QStringLiteral("device"), QStringLiteral("pronunciations"), QStringLiteral("leading_pause"),
                               QStringLiteral("trailing_pause"), QStringLiteral("sentence_pause")})
        if (options.contains(key))
            request[key] = options[key];
    if (!rvc.empty())
        request["rvc"] = rvc;
    QSaveFile file(QDir(m_folder).filePath(QStringLiteral("request.json")));
    const QByteArray bytes = QJsonDocument(request).toJson();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return fail(tr("Could not write the voice request."));
    m_busy = true;
    m_done = m_ok = m_cancelled = false;
    m_preview = preview;
    m_count = m_warnings = m_trackId = 0;
    m_error.clear();
    m_pending.clear();
    m_progress = 0;
    m_message = tr("Starting local speech engine");
    m_voiceName = voice.name;
    if (!rvc.empty())
        m_voiceName += tr(" → %1").arg(QFileInfo(rvc.value("model").toString()).completeBaseName());
    auto environment = QProcessEnvironment::systemEnvironment();
    QString cuda = aiRuntimeCudaLibraries();
    if (cuda.isEmpty())
        cuda = QFileInfo(rvcWorkerPath()).dir().filePath(QStringLiteral("_internal/torch/lib"));
    if (!environment.contains(QStringLiteral("MVP_CUDA_LIBRARY_DIR")) && QFileInfo::exists(cuda))
        environment.insert(QStringLiteral("MVP_CUDA_LIBRARY_DIR"), cuda);
    m_process.setProcessEnvironment(environment);
    const QString program = workerPath();
    QStringList arguments =
        qEnvironmentVariableIsSet("MVP_VOICE_WORKER") ? QStringList{} : aiRuntimeWorkerArguments(QStringLiteral("speech"), program);
    arguments.append(file.fileName());
    m_process.start(program, arguments);
    return true;
}
void VoiceJob::readOutput()
{
    m_pending += m_process.readAllStandardOutput();
    while (m_pending.contains('\n')) {
        const int end = m_pending.indexOf('\n');
        const auto o = QJsonDocument::fromJson(m_pending.left(end)).object();
        m_pending.remove(0, end + 1);
        if (o.contains("backend"))
            m_backends[o.value("engine").toString("speech")] = o["backend"];
        if (o.contains("progress")) {
            m_progress = std::clamp(o.value("progress").toDouble(), 0.0, 1.0);
            m_message = o.value("message").toString();
            emit progress(m_progress, m_message);
        }
        if (o.contains("message") && !o.contains("progress")) {
            m_message = o["message"].toString();
            emit progress(m_progress, m_message);
        }
        if (o.contains("error"))
            m_error = o.value("error").toString();
    }
    if (m_pending.size() > 65536)
        m_pending.clear();
}
void VoiceJob::finish(const QString& failure)
{
    if (!m_busy)
        return;
    QString error = failure;
    std::vector<VoiceTake> takes;
    std::vector<MediaInfo> media;
    if (error.isEmpty() && readVoiceTakes(m_folder, takes, &error)) {
        for (const auto& take : takes) {
            const auto info = probeMedia(take.path);
            if (!info.ok() || !info.hasAudio() || info.hasVideo() || info.duration <= 0
                || std::abs(double(take.length) * take.speed - double(info.duration)) > 30000) {
                error = tr("A generated voice file is missing, invalid, or has incorrect timing.");
                break;
            }
            media.push_back(info);
        }
    }
    if (error.isEmpty() && !m_preview) {
        const auto fps = m_settings.fps;
        for (const auto& take : takes) {
            const TimeUs length = fps.timeOf(fps.frameAt(take.length) + (fps.timeOf(fps.frameAt(take.length)) < take.length ? 1 : 0));
            for (auto value : m_before.chapters) {
                auto c = value.toObject();
                if (c["id"].toInt() != m_options["chapter_id"].toInt())
                    continue;
                const auto at = c["start"].toString().toLongLong();
                if (take.start < at || take.start + length > at + c["narration"].toString().toLongLong()) {
                    error = tr("This take would enter the chapter's listening section. Expand the narration window or shorten the script. "
                               "Generated audio is kept in %1.")
                                .arg(m_folder);
                    break;
                }
            }
            const int replacement = m_options["replace_event"].toInt();
            int ti = 0;
            if (const auto* old = m_before.findClip(replacement, &ti); old && !m_options["ripple"].toBool())
                for (const auto& next : m_before.tracks[size_t(ti)].clips)
                    if (next.id != replacement && next.start >= old->end() && next.start < old->start + length) {
                        error =
                            tr("This take overlaps following narration. Expand the window or enable ripple. Generated audio is kept in %1.")
                                .arg(m_folder);
                        break;
                    }
        }
    }
    if (error.isEmpty() && !m_preview && m_options.value("align_captions").toBool() && !m_alignmentReady) {
        if (m_alignmentRunning)
            return;
        m_alignmentRunning = true;
        m_message = tr("Aligning captions to generated speech");
        const QString model = m_options.value("alignment_model").toString();
        m_alignment = std::thread([this, takes, media, model] {
            std::vector<Caption> aligned;
            QString failure;
            for (size_t i = 0; i < takes.size() && !m_alignmentCancel; ++i) {
                const auto& take = takes[i];
                AudioDecoder decoder;
                if (!decoder.open(take.path, media[i].audio.front().index, 16000, &failure))
                    break;
                const int frames = int(std::min<TimeUs>(usToSamples(media[i].duration, 16000), 2 * 3600 * 16000));
                std::vector<float> stereo(size_t(frames) * 2), mono(size_t(frames), 0);
                decoder.read(0, frames, stereo.data());
                for (int j = 0; j < frames; ++j)
                    mono[size_t(j)] = (stereo[size_t(j) * 2] + stereo[size_t(j) * 2 + 1]) * .5f;
                TranscribeOptions options;
                options.wordTimestamps = true;
                options.language = "en";
                std::vector<Caption> words;
                if (!transcribe(
                        model, mono, options,
                        [&](double value) {
                            QMetaObject::invokeMethod(
                                this,
                                [this, value, i, size = takes.size()] {
                                    m_message = tr("Aligning take %1 of %2").arg(i + 1).arg(size);
                                    m_progress = (i + value) / size;
                                    emit progress(m_progress, m_message);
                                },
                                Qt::QueuedConnection);
                            return !m_alignmentCancel.load();
                        },
                        words, &failure))
                    break;
                if (words.empty()) {
                    failure = QStringLiteral("Speech alignment produced no words. Try another speech model or disable alignment.");
                    break;
                }
                for (auto word : words) {
                    word.start = take.start + TimeUs(word.start / take.speed);
                    word.end = take.start + TimeUs(word.end / take.speed);
                    word.speaker = int(i);
                    aligned.push_back(word);
                }
            }
            if (m_alignmentCancel)
                failure = QStringLiteral("Cancelled");
            QMetaObject::invokeMethod(
                this,
                [this, aligned = std::move(aligned), failure] {
                    if (m_alignment.joinable())
                        m_alignment.join();
                    m_wordCaptions = aligned;
                    m_alignmentReady = true;
                    m_alignmentRunning = false;
                    finish(failure);
                },
                Qt::QueuedConnection);
        });
        return;
    }
    if (error.isEmpty() && !m_preview) {
        if (m_before != m_project->timeline() || m_settings != m_project->settings() || m_projectFile != m_project->filePath()) {
            error = tr("The project changed during generation. Speech was kept in %1, but was not inserted. Generate again from the "
                       "current project.")
                        .arg(m_folder);
        } else {
            Timeline tl = m_project->timeline();
            Track track;
            track.id = tl.allocId();
            track.type = TrackType::Audio;
            track.name = tr("Voice — %1").arg(m_voiceName);
            const auto fps = m_settings.fps;
            TimeUs lastEnd = 0;
            for (size_t i = 0; i < takes.size(); ++i) {
                const auto& take = takes[i];
                Clip clip;
                clip.id = tl.allocId();
                clip.mediaId = m_project->addMedia(media[i]);
                clip.streamIndex = media[i].audio.front().index;
                clip.start = std::max(fps.snap(take.start), lastEnd);
                const int64_t frames = fps.frameAt(take.length);
                // Round up: rounding down would trim the last fraction of a word.
                clip.length = std::max(fps.frameDuration(), fps.timeOf(frames + (fps.timeOf(frames) < take.length ? 1 : 0)));
                clip.speed = take.speed;
                clip.preservePitch = true;
                clip.name = take.text;
                if (take.overrun || take.shifted) {
                    ++m_warnings;
                    clip.name = tr("[Timing] %1").arg(take.text);
                    Marker marker;
                    marker.id = tl.allocId();
                    marker.time = clip.start;
                    marker.name = tr("Voice timing: %1").arg(take.text.left(80));
                    marker.color = 3;
                    tl.markers.push_back(marker);
                }
                lastEnd = clip.end();
                track.clips.push_back(clip);
            }
            const int replacement = m_options.value("replace_event").toInt();
            for (size_t i = 0; i < track.clips.size(); ++i) {
                auto& clip = track.clips[i];
                QJsonArray alternatives;
                if (replacement)
                    if (const auto* previous = tl.findClip(replacement)) {
                        alternatives = previous->narration.value("takes").toArray();
                        if (alternatives.isEmpty())
                            alternatives.append(QJsonObject{{"media", previous->mediaId},
                                                            {"path", m_project->mediaItem(previous->mediaId)->path},
                                                            {"length", QString::number(previous->length)},
                                                            {"speed", previous->speed},
                                                            {"text", previous->name}});
                    }
                QJsonArray words;
                for (const auto& word : m_wordCaptions)
                    if (word.speaker == int(i))
                        words.append(QJsonObject{{"start", QString::number(std::max<TimeUs>(0, word.start - takes[i].start))},
                                                 {"end", QString::number(std::min(clip.length, word.end - takes[i].start))},
                                                 {"text", word.text}});
                alternatives.append(QJsonObject{{"media", clip.mediaId},
                                                {"path", takes[i].path},
                                                {"length", QString::number(clip.length)},
                                                {"speed", clip.speed},
                                                {"text", takes[i].text},
                                                {"voice", m_voiceId},
                                                {"rate", m_options["rate"]},
                                                {"words", words},
                                                {"rvc", m_options["rvc"]}});
                clip.narration = {{"text", takes[i].text},
                                  {"voice", m_voiceId},
                                  {"rate", m_options.value("rate").toDouble(1)},
                                  {"takes", alternatives},
                                  {"active_take", alternatives.size() - 1}};
                if (m_options.contains("chapter_id"))
                    clip.narration["chapter_id"] = m_options["chapter_id"];
                else if (replacement)
                    if (const auto* previous = tl.findClip(replacement))
                        if (previous->narration.contains("chapter_id"))
                            clip.narration["chapter_id"] = previous->narration["chapter_id"];
                clip.narration["rvc"] = m_options["rvc"];
                if (replacement) {
                    auto* old = tl.findClip(replacement);
                    const auto oldLength = old->length;
                    if (m_options.value("ripple").toBool()) {
                        const TimeUs end = old->end(), delta = clip.length - oldLength;
                        ops::rippleShift(tl, end, delta, {}, ops::expandToGroups(tl, {replacement}), true);
                        for (int ci = 0; ci < tl.chapters.size(); ++ci) {
                            auto chapter = tl.chapters[ci].toObject();
                            if (chapter["start"].toString().toLongLong() >= end) {
                                chapter["start"] = QString::number(chapter["start"].toString().toLongLong() + delta);
                                tl.chapters[ci] = chapter;
                            }
                        }
                    }
                    old = tl.findClip(replacement);
                    const auto oldEffects = old->effects;
                    const auto oldGain = old->gain;
                    const auto oldGroup = old->groupId;
                    clip.id = replacement;
                    clip.start = old->start;
                    clip.effects = oldEffects;
                    clip.gain = oldGain;
                    clip.groupId = oldGroup;
                    *old = clip;
                    tl.findClip(replacement, &m_trackId);
                    m_trackId = tl.tracks[size_t(m_trackId)].id;
                }
                m_createdIds.push_back(clip.id);
            }
            if (!replacement) {
                m_trackId = track.id;
                tl.tracks.push_back(track);
            }
            if (m_alignmentReady) {
                std::erase_if(tl.tracks, [&](Track& t) {
                    std::erase_if(t.clips, [&](const Clip& c) {
                        return c.narration.value("caption_owner").toInt() == replacement && replacement != 0;
                    });
                    return false;
                });
                const int sourceCaption = m_options.value("source_caption").toInt();
                if (sourceCaption)
                    ops::removeClips(tl, {sourceCaption});
                Track captions;
                captions.id = tl.allocId();
                captions.name = tr("Captions — aligned narration");
                const auto* preset = findGeneratorPreset("kinetic_caption");
                for (const auto& word : m_wordCaptions) {
                    if (word.speaker < 0 || size_t(word.speaker) >= m_createdIds.size())
                        continue;
                    const auto* owner = tl.findClip(m_createdIds[size_t(word.speaker)]);
                    if (!owner)
                        continue;
                    const TimeUs offset = owner->start - takes[size_t(word.speaker)].start;
                    Clip c;
                    c.id = tl.allocId();
                    c.start = std::clamp(word.start + offset, owner->start, owner->end());
                    c.length = std::min(word.end + offset, owner->end()) - c.start;
                    if (c.length <= 0)
                        continue;
                    c.name = word.text;
                    if (preset)
                        c.generator = makeGenerator(*preset);
                    else
                        c.generator.type = "text";
                    c.generator.params["text"].text = word.text;
                    c.generator.params["animIn"].value = 0;
                    c.generator.params["animOut"].value = 0;
                    c.narration["caption_owner"] = m_createdIds[size_t(word.speaker)];
                    captions.clips.push_back(c);
                }
                tl.tracks.insert(tl.tracks.begin(), std::move(captions));
            }
            if (m_options.contains("chapter_id"))
                for (int ci = 0; ci < tl.chapters.size(); ++ci) {
                    auto chapter = tl.chapters[ci].toObject();
                    if (chapter["id"].toInt() != m_options["chapter_id"].toInt())
                        continue;
                    QJsonArray ids;
                    std::set<int> owned;
                    for (auto value : chapter["clips"].toArray())
                        if (tl.findClip(value.toInt()))
                            owned.insert(value.toInt());
                    for (int id : m_createdIds)
                        owned.insert(id);
                    for (int id : owned)
                        ids.append(id);
                    for (const auto& t : tl.tracks)
                        for (const auto& c : t.clips)
                            if (c.narration.contains("caption_owner")
                                && std::find(m_createdIds.begin(), m_createdIds.end(), c.narration["caption_owner"].toInt())
                                    != m_createdIds.end())
                                ids.append(c.id);
                    chapter["clips"] = ids;
                    tl.chapters[ci] = chapter;
                }
            tl.sortClips();
            tl.sortMarkers();
            m_project->applyEdit(tl, tr("Generate Voice from Captions"));
        }
    }
    m_count = int(takes.size());
    m_error = error;
    m_ok = error.isEmpty();
    m_busy = false;
    m_done = true;
    m_message = m_ok ? tr("Speech ready") : error;
#ifdef _WIN32
    if (m_processTree) {
        CloseHandle(HANDLE(m_processTree));
        m_processTree = nullptr;
    }
#endif
    if (!m_ok && m_cancelled)
        QDir(m_folder).removeRecursively();
    emit completed(m_ok, error);
}
void VoiceJob::cancel()
{
    if (!m_busy)
        return;
    m_cancelled = true;
    m_alignmentCancel = true;
    QFile file(QDir(m_folder).filePath(QStringLiteral("cancel")));
    if (file.open(QIODevice::WriteOnly))
        file.close();
#ifdef _WIN32
    if (m_processTree)
        TerminateJobObject(HANDLE(m_processTree), 1);
    else
        m_process.kill();
#else
    m_process.terminate();
#endif
}
QJsonObject VoiceJob::status() const
{
    QJsonArray ids;
    for (int id : m_createdIds)
        ids.append(id);
    return {{"backends", m_backends},        {"event_ids", ids},     {"busy", m_busy},   {"done", m_done},        {"ok", m_ok},
            {"progress", m_progress},        {"message", m_message}, {"error", m_error}, {"track_id", m_trackId}, {"events", m_count},
            {"timing_warnings", m_warnings}, {"folder", m_folder}};
}
