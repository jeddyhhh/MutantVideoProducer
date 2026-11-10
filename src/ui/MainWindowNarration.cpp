#include "ui/MainWindow.h"

#include "ui/TimelineWidget.h"

#include "ui/VoiceJob.h"
#include "ui/VoiceOverDialog.h"
#include "core/Brand.h"
#include "core/Profiles.h"

#include "core/Voice.h"

#include "core/Generators.h"

#include "core/TimelineOps.h"

#include <QAudioOutput>

#include <QCheckBox>

#include <QComboBox>

#include <QDialogButtonBox>

#include <QDoubleSpinBox>

#include <QFileInfo>

#include <QFormLayout>

#include <QHBoxLayout>

#include <QGroupBox>

#include <QJsonArray>

#include <QLabel>

#include <QMediaPlayer>

#include <QMessageBox>

#include <QPlainTextEdit>

#include <QPushButton>

#include <QRegularExpression>

#include <QSettings>

#include <QDir>

#include <QTabWidget>

#include <QVBoxLayout>

void MainWindow::narrationStudio(const std::vector<Caption>& supplied, const QJsonObject& suppliedOptions)

{

    stopPlayback(false);

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Narration Studio"));
    dialog.setObjectName("narrationStudio");
    dialog.resize(820, 700);

    auto* layout = new QVBoxLayout(&dialog);
    auto* tabs = new QTabWidget;
    layout->addWidget(tabs);

    auto* sentence = new QWidget;
    auto* form = new QFormLayout(sentence);
    tabs->addTab(sentence, tr("Sentence and takes"));

    int current = 0, sourceCaption = 0;
    QString original;
    TimeUs start = m_timeline->cursorTime(), duration = 15 * kUsPerSecond;

    for (int id : m_timeline->selection())
        if (const auto* c = m_project->timeline().findClip(id)) {

            if (c->narration.contains("takes")) {
                current = id;
                original = c->narration["text"].toString(c->name);
                start = c->start;
                duration = c->length;
                break;
            }

            if (c->generator.type == "text" || c->generator.type == "kinetic_text") {
                sourceCaption = id;
                auto text = c->generator.params.find("text");
                original = text == c->generator.params.end() ? c->name : text->second.text;
                start = c->start;
                duration = c->length;
                break;
            }
        }

    if (!supplied.empty()) {
        original = supplied.front().text;
        start = supplied.front().start;
        duration = supplied.front().end - start;
        current = sourceCaption = 0;
        for (const auto& t : m_project->timeline().tracks)
            for (const auto& c : t.clips)
                if (c.narration["chapter_id"].toInt(-1) == suppliedOptions["chapter_id"].toInt(-2))
                    current = c.id;
    }

    auto* script = new QPlainTextEdit(original);
    script->setObjectName("narrationScript");
    script->setMinimumHeight(170);
    script->setPlaceholderText(tr("Write one sentence or paragraph. Add [pause=0.5] for an intentional pause."));
    form->addRow(tr("Script"), script);

    auto* at = new QDoubleSpinBox;
    at->setRange(0, 1e6);
    at->setDecimals(3);
    at->setValue(start / double(kUsPerSecond));
    form->addRow(tr("Start (seconds)"), at);

    auto* window = new QDoubleSpinBox;
    window->setRange(.1, 3600);
    window->setDecimals(3);
    window->setValue(duration / double(kUsPerSecond));
    form->addRow(tr("Narration window (seconds)"), window);

    at->setEnabled(!current);
    window->setEnabled(true);

    const auto settings = m_project->timeline().narrationSettings;

    auto* voice = new QComboBox;
    for (const auto& v : localVoiceCatalog())
        voice->addItem(v.name + (voiceAssetInstalled(v.asset) ? tr(" — installed") : tr(" — download in Generate Voice")), v.id);

    voice->setCurrentIndex(std::max(0, voice->findData(settings.value("voice").toString("bm_george"))));
    form->addRow(tr("Voice"), voice);

    auto* rate = new QDoubleSpinBox;
    rate->setRange(.5, 2);
    rate->setSingleStep(.05);
    rate->setValue(settings.value("rate").toDouble(1));
    form->addRow(tr("Speech rate"), rate);

    QJsonObject previousRvc = settings["rvc"].toObject();
    if (const auto* c = m_project->timeline().findClip(current); c && c->narration["rvc"].isObject())
        previousRvc = c->narration["rvc"].toObject();

    if (previousRvc.isEmpty()) {
        QSettings saved;
        const QString rvcModel = saved.value("voice/rvcModel").toString();
        if (QFileInfo::exists(rvcModel)) {
            const auto index = QFileInfo(rvcModel).dir().filePath("voice.index");
            previousRvc = {{"engine", "bundled"},
                           {"model", rvcModel},
                           {"index", QFileInfo::exists(index) ? index : QString()},
                           {"pitch", saved.value("voice/pitch", 0).toInt()},
                           {"index_rate", saved.value("voice/retrieval", 0).toDouble()},
                           {"protect", saved.value("voice/protect", .33).toDouble()},
                           {"f0_method", "rmvpe"}};
        }
    }

    auto* rvc = new QCheckBox(tr("Use RVC conversion: %1")
                                  .arg(previousRvc.isEmpty() ? tr("import a voice in Generate Voice first")
                                                             : QFileInfo(previousRvc["model"].toString()).completeBaseName()));
    rvc->setEnabled(!previousRvc.isEmpty());
    rvc->setChecked(!previousRvc.isEmpty() && settings["rvc"].isObject());
    form->addRow(rvc);

    auto* align = new QCheckBox(tr("Align captions to the actual generated speech (local Whisper)"));
    QString model = whisperModelPath("ggml-base.bin");
    for (const auto& candidate : {"ggml-base.bin", "ggml-small.bin", "ggml-tiny.bin", "ggml-medium.bin"})
        if (QFileInfo::exists(whisperModelPath(candidate))) {
            model = whisperModelPath(candidate);
            break;
        }
    align->setChecked(QFileInfo::exists(model));
    align->setEnabled(QFileInfo::exists(model));
    form->addRow(align);

    auto* ripple = new QCheckBox(tr("Ripple following material if the replacement's length changes"));
    form->addRow(ripple);

    auto* take = new QComboBox;
    form->addRow(tr("Alternate takes"), take);

    auto* audio = new QAudioOutput(&dialog);
    auto* player = new QMediaPlayer(&dialog);
    player->setAudioOutput(audio);

    auto* play = new QPushButton(tr("Audition selected take"));
    auto* use = new QPushButton(tr("Use selected take"));
    auto* audition = new QHBoxLayout;
    audition->addWidget(play);
    audition->addWidget(use);
    form->addRow(audition);

    auto reloadTakes = [&] {
        take->clear();
        if (const auto* c = m_project->timeline().findClip(current)) {
            const auto values = c->narration["takes"].toArray();
            for (int i = 0; i < values.size(); ++i) {
                auto t = values[i].toObject();
                take->addItem(tr("Take %1 — %2").arg(i + 1).arg(t["text"].toString().left(65)), i);
            }
            take->setCurrentIndex(c->narration["active_take"].toInt(values.size() - 1));
        }
    };
    reloadTakes();

    connect(play, &QPushButton::clicked, &dialog, [&] {
        if (const auto* c = m_project->timeline().findClip(current)) {
            auto values = c->narration["takes"].toArray();
            if (take->currentIndex() < 0 || take->currentIndex() >= values.size())
                return;
            auto t = values[take->currentIndex()].toObject();
            const auto media = m_project->mediaItem(t["media"].toInt());
            player->setSource(QUrl::fromLocalFile(media ? media->path : t["path"].toString()));
            player->setPlaybackRate(t["speed"].toDouble(1));
            player->play();
        }
    });

    connect(use, &QPushButton::clicked, &dialog, [&] {
        if (voiceJob()->busy())
            return;
        Timeline tl = m_project->timeline();
        auto* c = tl.findClip(current);
        if (!c)
            return;
        auto values = c->narration["takes"].toArray();
        if (take->currentIndex() < 0 || take->currentIndex() >= values.size())
            return;
        const auto selected = values[take->currentIndex()].toObject();
        const auto length = selected["length"].toString().toLongLong();
        if (!m_project->mediaItem(selected["media"].toInt())) {
            QMessageBox::information(&dialog, tr("Alternate Take"), tr("This take is offline. Relink its media first."));
            return;
        }

        for (auto value : tl.chapters) {
            auto chapter = value.toObject();
            if (chapter["id"].toInt() == c->narration["chapter_id"].toInt()
                && c->start + length > chapter["start"].toString().toLongLong() + chapter["narration"].toString().toLongLong()) {
                QMessageBox::information(&dialog, tr("Alternate Take"),
                                         tr("Expand this chapter's narration window first; this take enters the listening section."));
                return;
            }
        }

        if (!ripple->isChecked() && length > c->length) {
            QMessageBox::information(&dialog, tr("Alternate Take"),
                                     tr("This take is longer. Enable ripple or use a larger narration window."));
            return;
        }

        if (ripple->isChecked()) {
            const TimeUs end = c->end(), delta = length - c->length;
            ops::rippleShift(tl, end, delta, {}, ops::expandToGroups(tl, {current}), true, false);
            for (int ci = 0; ci < tl.chapters.size(); ++ci) {
                auto chapter = tl.chapters[ci].toObject();
                if (chapter["start"].toString().toLongLong() >= end) {
                    chapter["start"] = QString::number(chapter["start"].toString().toLongLong() + delta);
                    tl.chapters[ci] = chapter;
                }
            }
        }
        c = tl.findClip(current);

        c->mediaId = selected["media"].toInt();
        if (const auto media = m_project->mediaItem(c->mediaId); media && !media->info.audioStream(c->streamIndex) && media->info.hasAudio())
            c->streamIndex = media->info.audio.front().index; // (the take's own file)
        c->sourceIn = 0;
        c->length = length;
        ops::clampFades(*c); // (a shorter take)
        c->speed = selected["speed"].toDouble(1);
        c->name = selected["text"].toString();
        c->narration["text"] = c->name;
        c->narration["active_take"] = take->currentIndex();

        for (auto& track : tl.tracks)
            std::erase_if(track.clips, [&](const Clip& clip) { return clip.narration["caption_owner"].toInt() == current; });

        Track captions;
        captions.id = tl.allocId();
        captions.name = tr("Captions — selected take");
        const TimeUs origin = c->start;
        const int chapterId = c->narration["chapter_id"].toInt();
        const auto* preset = findGeneratorPreset("kinetic_caption");

        auto words = selected["words"].toArray();
        if (words.isEmpty())
            words.append(QJsonObject{{"start", "0"}, {"end", QString::number(length)}, {"text", c->name}});

        for (auto value : words) {
            auto word = value.toObject();
            Clip cap;
            cap.id = tl.allocId();
            cap.start = origin + word["start"].toString().toLongLong();
            cap.length = word["end"].toString().toLongLong() - word["start"].toString().toLongLong();
            if (cap.length <= 0 || cap.start < 0) // (a damaged take's word times)
                continue;
            cap.generator = preset ? makeGenerator(*preset) : GeneratorSpec{};
            if (!preset)
                cap.generator.type = "text";
            cap.generator.params["text"].text = word["text"].toString();
            cap.name = word["text"].toString();
            cap.narration["caption_owner"] = current;
            captions.clips.push_back(cap);
        }
        tl.tracks.insert(tl.tracks.begin(), std::move(captions));
        for (int i = 0; i < tl.chapters.size(); ++i) {
            auto chapter = tl.chapters[i].toObject();
            if (chapter["id"].toInt() != chapterId)
                continue;
            std::set<int> owned;
            for (auto value : chapter["clips"].toArray())
                if (tl.findClip(value.toInt()))
                    owned.insert(value.toInt());
            for (const auto& t : tl.tracks)
                for (const auto& cap : t.clips)
                    if (cap.narration["caption_owner"].toInt() == current)
                        owned.insert(cap.id);
            QJsonArray values;
            for (int id : owned)
                values.append(id);
            chapter["clips"] = values;
            tl.chapters[i] = chapter;
        }
        tl.sortClips();
        m_project->applyEdit(tl, tr("Use Alternate Narration Take"));
        script->setPlainText(selected["text"].toString());
        reloadTakes();
    });

    auto* dictionary = new QWidget;
    auto* dictionaryLayout = new QVBoxLayout(dictionary);
    tabs->addTab(dictionary, tr("Pronunciation and pauses"));

    auto* help = new QLabel(tr("Keep the spelling in your script. Enter one pronunciation per line: Necrophagist = neck roff a "
                               "gist\nInline pauses: [pause=0.5]. These create real silence in the generated audio."));
    help->setWordWrap(true);
    dictionaryLayout->addWidget(help);

    auto* glossary = new QPlainTextEdit;
    QStringList entries;
    const auto dict = settings["pronunciations"].toObject();
    for (auto it = dict.begin(); it != dict.end(); ++it)
        entries.append(it.key() + " = " + it.value().toString());
    glossary->setPlainText(entries.join('\n'));
    dictionaryLayout->addWidget(glossary, 1);

    auto* pauseForm = new QFormLayout;
    dictionaryLayout->addLayout(pauseForm);

    auto pause = [&](const QString& label, const char* key) {
        auto* s = new QDoubleSpinBox;
        s->setRange(0, 10);
        s->setSingleStep(.1);
        s->setValue(settings[key].toDouble());
        pauseForm->addRow(label, s);
        return s;
    };

    auto* leading = pause(tr("Pause before speaking"), "leading_pause");
    auto* trailing = pause(tr("Pause after speaking"), "trailing_pause");
    auto* between = pause(tr("Pause between sentences"), "sentence_pause");

    auto* status = new QLabel;
    status->setWordWrap(true);
    layout->addWidget(status);

    auto* generate = new QPushButton(current ? tr("Generate another take of this sentence") : tr("Generate narration"));
    layout->addWidget(generate);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    connect(generate, &QPushButton::clicked, &dialog, [&] {
        if (voiceJob()->busy())
            return;
        QJsonObject pronunciations;

        for (const auto& line : glossary->toPlainText().split('\n')) {
            if (line.trimmed().isEmpty())
                continue;
            const int eq = line.indexOf('=');
            if (eq <= 0 || line.mid(eq + 1).trimmed().isEmpty()) {
                status->setText(tr("Use word = pronunciation on each dictionary line."));
                return;
            }
            pronunciations[line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
        }

        const auto catalog = localVoiceCatalog();
        auto selected =
            std::find_if(catalog.begin(), catalog.end(), [&](const LocalVoice& v) { return v.id == voice->currentData().toString(); });
        if (selected == catalog.end())
            return;

        QJsonObject options = suppliedOptions;
        options["rate"] = rate->value();
        options["timing"] = "fit";
        options["group"] = false;
        options["pronunciations"] = pronunciations;
        options["leading_pause"] = leading->value();
        options["trailing_pause"] = trailing->value();
        options["sentence_pause"] = between->value();
        options["align_captions"] = align->isChecked();
        options["alignment_model"] = model;
        options["ripple"] = ripple->isChecked();
        if (rvc->isChecked())
            options["rvc"] = previousRvc;
        if (current) {
            options["replace_event"] = current;
            if (const auto* c = m_project->timeline().findClip(current); c && c->narration.contains("chapter_id"))
                options["chapter_id"] = c->narration["chapter_id"];
        }
        if (sourceCaption)
            options["source_caption"] = sourceCaption;

        Timeline tl = m_project->timeline();
        tl.narrationSettings = options;
        tl.narrationSettings["voice"] = selected->id;

        // Per-run event ids are never persisted as defaults.

        for (const auto& key : {"replace_event", "source_caption", "chapter_id"})
            tl.narrationSettings.remove(key);

        m_project->applyEdit(tl, tr("Narration Preferences"));

        QString error;
        const TimeUs a = TimeUs(at->value() * kUsPerSecond), b = a + TimeUs(window->value() * kUsPerSecond);

        if (!voiceJob()->start({Caption{a, b, script->toPlainText()}}, *selected, options, false, &error)) {
            status->setText(error);
            return;
        }
        generate->setEnabled(false);
        status->setText(tr("Generating locally..."));
    });

    connect(voiceJob(), &VoiceJob::progress, &dialog, [&](double value, const QString& message) {
        status->setText(QStringLiteral("%1% — %2").arg(value * 100, 0, 'f', 0).arg(message));
    });

    connect(voiceJob(), &VoiceJob::completed, &dialog, [&](bool ok, const QString& error) {
        generate->setEnabled(true);
        status->setText(ok ? tr("Ready. Earlier takes are retained; audition them before choosing.") : error);
        if (ok) {
            const auto result = voiceJob()->status();
            const auto ids = result["event_ids"].toArray();
            if (!ids.isEmpty()) {
                current = ids.first().toInt();
                sourceCaption = 0;
                m_timeline->selectClips({current});
                at->setEnabled(false);
                window->setEnabled(false);
                if (const auto* c = m_project->timeline().findClip(current)) {
                    at->setValue(c->start / double(kUsPerSecond));
                    window->setValue(c->length / double(kUsPerSecond));
                }
                reloadTakes();
            }
        }
    });

    dialog.exec();
    player->stop();
}

VoiceOverDialog* MainWindow::recordVoiceOver()
{
    // Takes go beside the project (or into the profile's Voice folder until it is saved).
    const QString file = m_project->filePath();
    QString folder = !file.isEmpty() ? QFileInfo(file).dir().filePath(QFileInfo(file).completeBaseName() + QStringLiteral("-voiceover"))
                                     : Profiles::instance().folder(QStringLiteral("Voice"));
    if (folder.isEmpty())
        folder = brand::userFolder(QStandardPaths::DocumentsLocation, QStringLiteral("Voice"));
    auto* dialog = new VoiceOverDialog(folder, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    auto start = std::make_shared<TimeUs>(0);
    dialog->onStart = [this, start](bool playEdit) {
        *start = m_timeline->cursorTime();
        if (playEdit)
            play(false);
    };
    dialog->onStop = [this] { stopPlayback(true); };
    connect(dialog, &VoiceOverDialog::recorded, this, [this, start](const QString& path) {
        const MediaInfo info = probeMedia(path);
        if (!info.ok()) {
            QMessageBox::warning(this, tr("Record Voice-Over"), tr("The take could not be read: %1").arg(info.error));
            return;
        }
        Timeline tl = m_project->timeline();
        const int id = m_project->addMedia(info);
        if (const auto media = m_project->mediaItem(id); media && ops::placeVoiceOver(tl, *media, *start, m_project->settings().fps))
            m_project->applyEdit(tl, tr("Record Voice-Over"));
    });
    dialog->show();
    return dialog;
}
