#include "core/AudioMixer.h"
#include "core/Captions.h"
#include "core/Project.h"
#include "core/Voice.h"
#include "ui/VoiceJob.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <cmath>

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("MvpVoiceTest"));
    QTemporaryDir temp(QCoreApplication::applicationDirPath() + QStringLiteral("/voice-test-XXXXXX"));
    if (!temp.isValid())
        return 1;
    if (qEnvironmentVariableIsEmpty("MVP_VOICE_DIR"))
        qputenv("MVP_VOICE_DIR", (temp.path() + QStringLiteral("/assets")).toUtf8());
    int failures = 0;
    auto check = [&](bool ok, const QString& what) {
        QTextStream(stdout) << (ok ? "PASS  " : "FAIL  ") << what << Qt::endl;
        if (!ok)
            ++failures;
    };
    const std::vector<Caption> cues{
        {1000000, 2500000, QStringLiteral("Mutant Video Producer can now"), 0},
        {2500000, 6000000, QStringLiteral("turn your captions into local speech."), 0},
        {8000000, 8100000, QStringLiteral("This longer sentence deliberately needs much more time than this tiny caption allows."), 0},
        {9000000, 12000000, QStringLiteral("Every word is kept."), 0}};
    const auto grouped = voiceSentences(cues, true);
    check(grouped.size() == 3 && grouped.front().start == 1000000 && grouped.front().end == 6000000
              && grouped.front().text == QStringLiteral("Mutant Video Producer can now turn your captions into local speech."),
          QStringLiteral("sentence grouping preserves the caption span and words"));
    check(voiceSentences(cues, false).size() == 4, QStringLiteral("sentence grouping can be disabled"));
    check(
        voiceSentences({{0, 1000000, QStringLiteral("Speaker one"), 0}, {1000000, 2000000, QStringLiteral("speaker two"), 1}}, true).size()
            == 2,
        QStringLiteral("grouping never bridges speaker changes"));
    check(voiceSentences({{0, 1000000, QStringLiteral("First."), 0}, {1000000, 2000000, QStringLiteral("Second"), 0}}, true).size() == 2,
          QStringLiteral("sentence punctuation ends a group"));
    check(voiceSentences({{0, 1000000, QStringLiteral("First"), 0}, {900000, 2000000, QStringLiteral("Overlap"), 0}}, true).size() == 2,
          QStringLiteral("grouping never bridges overlaps"));
    check(voiceSentences({{0, 1000000, QStringLiteral("First"), 0}, {1500000, 2000000, QStringLiteral("Gap"), 0}}, true).size() == 2,
          QStringLiteral("grouping preserves long pauses"));
    check(voiceSentences({{0, 1000000, QStringLiteral("  "), 0}, {-1, 1000000, QStringLiteral("Bad"), 0}}, true).empty(),
          QStringLiteral("empty and invalid captions are ignored"));
    {
        const auto lines = dialogueLines(QStringLiteral("# scene one\nGus: Hi there.\n  still Gus.\n\nMo: Hello: friend.\nGus: Bye."));
        check(lines.size() == 3 && lines[0].speaker == "Gus" && lines[0].text == "Hi there. still Gus." && lines[1].speaker == "Mo"
                  && lines[1].text == "Hello: friend." && dialogueSpeakers(lines) == QStringList({"Gus", "Mo"}),
              QStringLiteral("a dialogue script reads as Name: line, continuations join the line, comments are skipped"));
        const auto gus = dialogueCaptions(lines, "Gus");
        check(gus.size() == 2 && gus[0].end <= gus[1].start && gus[1].text == "Bye.",
              QStringLiteral("each character speaks only their lines, never overlapping"));
        Timeline tl;
        Track a, b;
        a.id = tl.allocId();
        b.id = tl.allocId();
        a.type = b.type = TrackType::Audio;
        for (Track* t : {&a, &a, &b}) {
            Clip c;
            c.id = tl.allocId();
            c.start = TimeUs(t->clips.size()) * 10 * kUsPerSecond;
            c.length = kUsPerSecond;
            t->clips.push_back(c);
        }
        tl.tracks = {a, b};
        const FrameRate fps{25, 1};
        check(arrangeDialogue(tl, lines, {{"Gus", a.id, 0}, {"Mo", b.id, -4}}, 2 * kUsPerSecond, 200000, fps, nullptr)
                  && tl.tracks[0].name == "Gus" && tl.tracks[1].name == "Mo" && tl.tracks[0].effects.empty()
                  && tl.tracks[1].effects.size() == 1 && tl.tracks[1].effects[0].type == "a_pitch"
                  && tl.tracks[0].clips[0].start == 2 * kUsPerSecond && tl.tracks[1].clips[0].start == 3200000
                  && tl.tracks[0].clips[1].start == 4400000 && tl.tracks[0].clips[1].name == "Bye.",
              QStringLiteral("dialogue lines are placed in script order on each character's track, with their pitch"));
        const Timeline before = tl;
        QString arrangeError;
        check(!arrangeDialogue(tl, lines, {{"Gus", b.id, 0}, {"Mo", a.id, 0}}, 0, 0, fps, &arrangeError) && tl == before
                  && !arrangeError.isEmpty(),
              QStringLiteral("a track with the wrong number of lines leaves the timeline alone"));
    }
    {
        // Pauses and treatments in the script: not spoken, but heard.
        const auto lines = dialogueLines(QStringLiteral("(pause 1.2)\nGus: [radio] Over.\n(pause)\n(Pause 2s)\nMo: [echo, backwards][hall] Hello.\nGus: Bye."));
        check(lines.size() == 3 && lines[0].pause == 1.2 && lines[0].text == "Over." && lines[0].treatments == QStringList({"radio"})
                  && lines[1].pause == 3 && lines[1].text == "Hello." && lines[1].treatments == QStringList({"echo", "backwards", "hall"})
                  && lines[2].pause == 0 && unknownTreatment(lines).isEmpty()
                  && unknownTreatment(dialogueLines(QStringLiteral("Gus: [laughs] Ha."))) == "laughs",
              QStringLiteral("a script's pauses and [treatments] are read, not spoken"));
        Timeline tl;
        Track a, b;
        a.id = tl.allocId();
        b.id = tl.allocId();
        a.type = b.type = TrackType::Audio;
        for (Track* t : {&a, &a, &b}) {
            Clip c;
            c.id = tl.allocId();
            c.start = TimeUs(t->clips.size()) * 10 * kUsPerSecond;
            c.length = kUsPerSecond;
            t->clips.push_back(c);
        }
        tl.tracks = {a, b};
        const FrameRate fps{25, 1};
        const bool arranged = arrangeDialogue(tl, lines, {{"Gus", a.id, 0}, {"Mo", b.id, 0}}, 0, 0, fps, nullptr);
        const Clip &over = tl.tracks[0].clips[0], &hello = tl.tracks[1].clips[0];
        check(arranged && over.start == 1200000 && over.effects.size() == 1 && over.effects[0].type == "a_ffmpeg" && hello.start == 5200000
                  && hello.reverse && hello.effects.size() == 2 && hello.effects[1].type == "a_reverb" && !tl.tracks[0].clips[1].reverse,
              QStringLiteral("a pause adds its silence before the line; a treatment adds its sound (backwards plays it reversed)"));
    }
    const auto voices = localVoiceCatalog();
    check(voices.size() == 28, QStringLiteral("all 28 curated English voices have metadata"));
    if (voices.empty())
        return failures + 1;
    const auto voice = *std::find_if(voices.begin(), voices.end(), [](const LocalVoice& v) { return v.id == QLatin1String("af_heart"); });
    check(verifyVoiceAsset(voiceAssetPath(voice.asset), voice.asset)
              && verifyVoiceAsset(voiceAssetPath(kokoroModelAsset()), kokoroModelAsset()),
          QStringLiteral("bundled model and voice pass their pinned SHA-256 checks"));
    QFile corrupt(temp.path() + QStringLiteral("/corrupt.bin"));
    corrupt.open(QIODevice::WriteOnly);
    corrupt.write("wrong");
    corrupt.close();
    check(!verifyVoiceAsset(corrupt.fileName(), voice.asset), QStringLiteral("corrupt assets are rejected"));
    Project project;
    const Timeline initial = project.timeline();
    VoiceJob job(&project);
    auto wait = [&] {
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
            job.cancel();
            loop.quit();
        });
        const auto connection = QObject::connect(&job, &VoiceJob::completed, &loop, &QEventLoop::quit);
        timeout.start(qEnvironmentVariableIsEmpty("MVP_RVC_TEST_MODEL") ? 90000 : 240000);
        loop.exec();
        QObject::disconnect(connection);
        if (job.busy()) {
            job.cancel();
            check(false, QStringLiteral("voice worker timed out"));
        }
    };
    QJsonObject options{{"output_dir", temp.path() + QStringLiteral("/takes")}, {"group", true}, {"timing", "fit"}};
    QString error;
    bool started = job.start(cues, voice, options, false, &error);
    check(started, QStringLiteral("real packaged Kokoro worker starts: %1").arg(error));
    if (!started)
        return failures;
    wait();
    const auto result = job.status();
    check(result.value("ok").toBool(), QStringLiteral("real local AI speech completes: %1").arg(result.value("error").toString()));
    if (!result.value("ok").toBool())
        return failures;
    check(project.timeline().tracks.size() == initial.tracks.size() + 1 && project.timeline().tracks.back().type == TrackType::Audio
              && project.timeline().tracks.back().clips.size() == 3,
          QStringLiteral("phrases are inserted on their own Voice audio track"));
    bool timings = true;
    TimeUs previous = 0;
    for (const auto& clip : project.timeline().tracks.back().clips) {
        timings &= clip.start >= previous && clip.speed >= 1 && clip.speed <= 1.35 && clip.preservePitch;
        const auto info = probeMedia(project.mediaItem(clip.mediaId)->path);
        timings &= double(clip.length) * clip.speed + 1 >= info.duration;
        previous = clip.end();
    }
    check(timings && project.timeline().tracks.back().clips.front().start == 1000000 && result.value("timing_warnings").toInt() > 0
              && !project.timeline().markers.empty(),
          QStringLiteral("timing uses caption positions, bounded pitch-preserving fit, and visible overrun markers"));
    const Timeline after = project.timeline();
    project.undoStack()->undo();
    check(project.timeline() == initial, QStringLiteral("one undo removes the generated track and timing markers"));
    project.undoStack()->redo();
    check(project.timeline() == after, QStringLiteral("redo restores all speech events and timing"));
    const auto audio = timelineSpeechAudio(*project.snapshot(), 0, project.timeline().duration());
    double sum = 0;
    bool finite = true;
    for (float sample : audio) {
        sum += sample * sample;
        finite &= std::isfinite(sample);
    }
    check(finite && sum > 1 && !audio.empty(),
          QStringLiteral("the editor mixes and stretches generated speech with finite, audible samples"));
    check(project.save(temp.path() + QStringLiteral("/voice-project.mvp"), &error), QStringLiteral("generated speech project saves"));
    Project reopened;
    QStringList missing;
    check(reopened.load(project.filePath(), &error, &missing) && missing.empty() && reopened.timeline() == project.timeline(),
          QStringLiteral("save/load preserves voice events and their audio paths"));
    started = job.start({cues.front()}, voice, options, false, &error);
    if (started) {
        Timeline changed = project.timeline();
        changed.tracks.back().name = QStringLiteral("Edited during generation");
        project.applyEdit(changed, QStringLiteral("Concurrent edit"));
        wait();
    }
    check(started && !job.status().value("ok").toBool() && project.timeline().tracks.size() == initial.tracks.size() + 1
              && job.status().value("error").toString().contains(QStringLiteral("project changed"), Qt::CaseInsensitive),
          QStringLiteral("changing the project during generation prevents stale speech insertion"));
    started = job.start(cues, voice, options, false, &error);
    if (started) {
        QTimer::singleShot(20, &job, &VoiceJob::cancel);
        wait();
    }
    check(started && !job.status().value("ok").toBool() && job.status().value("error").toString() == QStringLiteral("Cancelled")
              && project.timeline().tracks.size() == initial.tracks.size() + 1,
          QStringLiteral("cancellation stops the worker without adding events"));
    check(rvcSharedAssets().size() == 4, QStringLiteral("bundled RVC catalog includes all four pinned support assets"));
    QJsonObject rvcOptions = options;
    rvcOptions["rvc"] = QJsonObject{{"model", temp.path() + QStringLiteral("/missing.pth")}};
    check(!job.start({cues.front()}, voice, rvcOptions, false, &error) && error.contains(QStringLiteral("RVC")),
          QStringLiteral("missing RVC model is rejected before editing"));
    const QString rvcModel = qEnvironmentVariable("MVP_RVC_TEST_MODEL");
    if (!rvcModel.isEmpty()) {
        const Timeline before = project.timeline();
        rvcOptions["rvc"] = QJsonObject{{"model", rvcModel}, {"f0_method", "rmvpe"}, {"pitch", 0}, {"index_rate", 0}};
        started = job.start(
            {{0, 8000000, QStringLiteral("Mutant Video Producer converts caption speech locally with its bundled RVC engine."), 0}}, voice,
            rvcOptions, false, &error);
        check(started, QStringLiteral("bundled RVC generation starts: %1").arg(error));
        if (started)
            wait();
        check(started && job.status().value("ok").toBool(),
              QStringLiteral("caption speech and real bundled RVC conversion finish: %1").arg(job.status().value("error").toString()));
        const bool inserted = project.timeline().tracks.size() == before.tracks.size() + 1;
        check(inserted && project.timeline().tracks.back().clips.size() == 1,
              QStringLiteral("RVC converted audio is inserted on its own voice track"));
        if (inserted) {
            const auto& clip = project.timeline().tracks.back().clips.front();
            const auto info = probeMedia(project.mediaItem(clip.mediaId)->path);
            check(info.hasAudio() && info.audio.front().sampleRate == 40000,
                  QStringLiteral("RVC output uses the imported model's sample rate"));
            project.undoStack()->undo();
            check(project.timeline() == before, QStringLiteral("one undo removes RVC generated speech"));
        }
    }
    {
        const Timeline before = project.timeline();
        const int event = before.tracks.back().clips.front().id;
        QJsonObject sentence = options;
        sentence["replace_event"] = event;
        sentence["group"] = false;
        sentence["leading_pause"] = .3;
        sentence["trailing_pause"] = .2;
        sentence["pronunciations"] = QJsonObject{{"Mutant Video Producer", "myoo tunt video producer"}};
        const QString model = qEnvironmentVariable("MVP_ALIGNMENT_TEST_MODEL");
        if (!model.isEmpty()) {
            sentence["align_captions"] = true;
            sentence["alignment_model"] = model;
        }
        started = job.start({{1000000, 6000000, "Mutant Video Producer edits the music. [pause=0.4] Listen closely.", 0}}, voice, sentence,
                            false, &error);
        check(started, "sentence regeneration starts");
        if (started)
            wait();
        check(started && job.status()["ok"].toBool(),
              QStringLiteral("sentence regeneration completes: %1").arg(job.status()["error"].toString()));
        if (started && job.status()["ok"].toBool()) {
            const auto* clip = project.timeline().findClip(event);
            check(clip && clip->narration["takes"].toArray().size() == 2, "regeneration retains earlier alternate take and event ID");
            check(!project.removeMedia(before.findClip(event)->mediaId), "earlier alternate take stays protected in the media pool");
            bool aligned = false, hasPauseText = false;
            for (const auto& t : project.timeline().tracks)
                for (const auto& c : t.clips)
                    if (c.narration["caption_owner"].toInt() == event) {
                        aligned = true;
                        hasPauseText |= c.name.contains("pause", Qt::CaseInsensitive) || c.name.contains("equals", Qt::CaseInsensitive);
                        check(c.start >= clip->start && c.end() <= clip->end(), "aligned word lies within spoken take");
                    }
            if (!model.isEmpty()) {
                check(!hasPauseText, "pause directives create silence and are never spoken");
                QTextStream(stdout) << "ALIGNMENT "
                                    << QJsonDocument(clip->narration["takes"].toArray().last().toObject()).toJson(QJsonDocument::Compact)
                                    << Qt::endl;
                check(aligned, "local Whisper creates captions owned by the sentence");
                check(clip->narration["takes"].toArray().last().toObject()["words"].toArray().size() > 3,
                      "alternate take stores actual word timestamps");
            }
            project.undoStack()->undo();
            check(project.timeline() == before, "one undo restores sentence and its captions");
            project.undoStack()->redo();
            check(project.timeline().findClip(event)->narration["takes"].toArray().size() == 2, "redo restores alternate takes");
        }
        sentence["replace_event"] = initial.tracks.front().id;
        check(!job.start({cues.front()}, voice, sentence, false, &error), "invalid replacement fails safely");
    }
    {
        // Two characters with the same installed voice, the second a pitch lower: one track each, lines in script order.
        DialogueJob dialogue(&project, &job);
        const int tracksBefore = int(project.timeline().tracks.size()), undoBefore = project.undoStack()->index();
        started = dialogue.start(QStringLiteral("Gus: Hello there.\nMo: Hi Gus.\nGus: Bye now."),
                                 {{"Gus", voice, 1, 0}, {"Mo", voice, 1.1, -4}}, 3 * kUsPerSecond, 250000, &error);
        check(started, QStringLiteral("dialogue starts: %1").arg(error));
        if (started) {
            QEventLoop loop;
            QTimer::singleShot(180000, &loop, [&] { dialogue.cancel(); });
            QObject::connect(&dialogue, &DialogueJob::completed, &loop, &QEventLoop::quit);
            loop.exec();
            const auto& tracks = project.timeline().tracks;
            const bool shape = dialogue.status().value("ok").toBool() && int(tracks.size()) == tracksBefore + 2;
            check(shape && tracks[tracksBefore].name == "Gus" && tracks[tracksBefore + 1].name == "Mo"
                      && tracks[tracksBefore].clips.size() == 2 && tracks[tracksBefore + 1].clips.size() == 1
                      && tracks[tracksBefore + 1].effects.size() == 1 && tracks[tracksBefore].clips[0].start == 3 * kUsPerSecond
                      && tracks[tracksBefore + 1].clips[0].start > tracks[tracksBefore].clips[0].end()
                      && tracks[tracksBefore].clips[1].start > tracks[tracksBefore + 1].clips[0].end(),
                  QStringLiteral("dialogue: a track per character, lines in script order: %1").arg(dialogue.status().value("error").toString()));
            check(project.undoStack()->index() == undoBefore + 1, QStringLiteral("dialogue is one undo step"));
            project.undoStack()->undo();
            check(int(project.timeline().tracks.size()) == tracksBefore, QStringLiteral("undo removes the whole dialogue"));
        }
        check(!dialogue.start(QStringLiteral("Zed: who am I"), {{"Gus", voice, 1, 0}}, 0, 0, &error) && error.contains("Zed"),
              QStringLiteral("a character without a voice is refused before anything runs"));
    }
    qputenv("MVP_VOICE_WORKER", (temp.path() + QStringLiteral("/missing.exe")).toUtf8());
    check(!job.start(cues, voice, options, false, &error), QStringLiteral("missing runtime gives a useful error before editing"));
    return failures;
}
