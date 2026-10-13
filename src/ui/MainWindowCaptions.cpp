#include "core/Brand.h"
// Tools > Captions: auto captions with Whisper, SRT / WebVTT import and export.

#include "ui/MainWindow.h"

#include "ui/Help.h"

#include "core/Captions.h"
#include "core/Generators.h"
#include "core/Project.h"
#include "core/TimelineOps.h"
#include "ui/TimelineWidget.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QStatusBar>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include "core/Profiles.h"
#include <QThread>
#include <QTimer>
#include <atomic>

namespace {

struct WhisperModel {
    const char* file;
    const char* label;
    const char* repo; // on huggingface.co
};
const WhisperModel kModels[] = {
    {"ggml-tiny.bin", "Tiny (78 MB, fastest)", "ggerganov/whisper.cpp"},
    {"ggml-base.bin", "Base (148 MB, good)", "ggerganov/whisper.cpp"},
    {"ggml-small.bin", "Small (488 MB, best, slower)", "ggerganov/whisper.cpp"},
    {"ggml-small.en-tdrz.bin", "Small English, marks who is speaking (488 MB)", "akashmjn/tinydiarize-whisper.cpp"}};

QString modelDir()
{
    const QString profile = Profiles::instance().folder(QStringLiteral("Speech Models"));
    if (!profile.isEmpty()) return profile;
    return QDir(brand::storageLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("whisper"));
}

// A model's file: bundled next to the program (the offline release), else downloaded.
QString modelPath(const QString& file)
{
    const QString bundled = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("models/whisper/") + file);
    return QFileInfo::exists(bundled) ? bundled : QDir(modelDir()).filePath(file);
}

} // namespace

void MainWindow::addCaptions(const std::vector<Caption>& captions, TimeUs offset, bool kinetic)
{
    const GeneratorPreset* preset = findGeneratorPreset(kinetic ? QStringLiteral("kinetic_caption") : QStringLiteral("subtitle"));
    if (!preset || captions.empty())
        return;
    Timeline tl = m_project->timeline();
    const int track = ops::addTrack(tl, TrackType::Video, 0); // on top of everything
    tl.tracks[size_t(track)].name = tr("Captions");
    const FrameRate fps = m_project->settings().fps;
    for (const Caption& cap : captions) {
        Clip c;
        c.id = tl.allocId();
        c.start = fps.snap(cap.start + offset);
        c.length = std::max(fps.snap(cap.end + offset) - c.start, fps.frameDuration());
        c.generator = makeGenerator(*preset);
        c.generator.params[QStringLiteral("text")].text = cap.text;
        c.generator.params[QStringLiteral("caption_speaker")].value = cap.speaker;
        if (cap.speaker == 1) // the second voice in another colour
            c.generator.params[QStringLiteral("color")].value = double(0xffd400);
        if (kinetic) {
            // The words come on over the first 80% of the caption.
            const int words = int(cap.text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts).size());
            c.generator.params[QStringLiteral("rate")].value = std::clamp(words / std::max(0.2, 0.8 * double(c.length) / kUsPerSecond), 0.5, 12.0);
        }
        c.name = generatorLabel(c.generator);
        tl.tracks[size_t(track)].clips.push_back(c);
    }
    tl.sortClips();
    m_project->applyEdit(tl, tr("Add Captions"));
}

void MainWindow::autoCaptions()
{
    stopPlayback(false);
    const Timeline& tl = m_project->timeline();
    TimeUs from = 0, to = tl.duration();
    if (m_timeline->hasRegion()) {
        from = m_timeline->regionStart();
        to = m_timeline->regionEnd();
    }
    if (!whisperAvailable()) {
        QMessageBox::information(this, tr("Auto Captions"), tr("This build of Mutant Video Producer has no speech recognition (Whisper)."));
        return;
    }
    if (to - from < kUsPerSecond) {
        QMessageBox::information(this, tr("Auto Captions"), tr("There is no sound on the timeline to caption."));
        return;
    }
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Auto Captions"));
    auto* form = new QFormLayout(&dlg);
    QSettings settings;
    auto* model = new QComboBox;
    for (const WhisperModel& m : kModels) {
        const bool have = QFileInfo::exists(modelPath(QString::fromLatin1(m.file)));
        model->addItem(tr(m.label) + (have ? QString() : tr(" - downloaded first")), QString::fromLatin1(m.file));
        model->setItemData(model->count() - 1, QString::fromLatin1(m.repo), Qt::UserRole + 1);
    }
    model->setCurrentIndex(std::max(0, model->findData(settings.value(QStringLiteral("captions/model"), QStringLiteral("ggml-base.bin")))));
    auto* language = new QComboBox;
    const std::pair<const char*, const char*> languages[] = {
        {"auto", "Detect it"}, {"en", "English"}, {"es", "Spanish"}, {"fr", "French"}, {"de", "German"}, {"it", "Italian"},
        {"pt", "Portuguese"}, {"nl", "Dutch"}, {"pl", "Polish"}, {"sv", "Swedish"}, {"ru", "Russian"}, {"uk", "Ukrainian"},
        {"tr", "Turkish"}, {"ar", "Arabic"}, {"hi", "Hindi"}, {"ja", "Japanese"}, {"ko", "Korean"}, {"zh", "Chinese"}};
    for (const auto& [code, name] : languages)
        language->addItem(tr(name), QString::fromLatin1(code));
    language->setCurrentIndex(std::max(0, language->findData(settings.value(QStringLiteral("captions/language"), QStringLiteral("auto")))));
    auto* style = new QComboBox;
    style->addItems({tr("Subtitles (a line at the bottom)"), tr("Kinetic Text (words pop on)")});
    style->setCurrentIndex(settings.value(QStringLiteral("captions/style"), 0).toInt());
    auto* length = new QComboBox;
    for (int chars : {32, 42, 60, 80})
        length->addItem(tr("Up to %1 characters").arg(chars), chars);
    length->setCurrentIndex(std::max(0, length->findData(settings.value(QStringLiteral("captions/length"), 42))));
    form->addRow(tr("Speech model"), model);
    form->addRow(tr("Language"), language);
    form->addRow(tr("Style"), style);
    form->addRow(tr("Caption length"), length);
    auto* translate = new QCheckBox(tr("Translate into English"));
    translate->setChecked(settings.value(QStringLiteral("captions/translate"), false).toBool());
    form->addRow(QString(), translate);
    auto* note = new QLabel(tr("Listens to %1 (%2 s) on this computer and adds the captions on a new Captions track. "
                               "Models are downloaded once from huggingface.co.")
                                .arg(m_timeline->hasRegion() ? tr("the time selection") : tr("the whole timeline"))
                                .arg(double(to - from) / kUsPerSecond, 0, 'f', 1));
    note->setWordWrap(true);
    form->addRow(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    help::addButton(buttons, QStringLiteral("captions.md#auto-captions"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    settings.setValue(QStringLiteral("captions/model"), model->currentData());
    settings.setValue(QStringLiteral("captions/language"), language->currentData());
    settings.setValue(QStringLiteral("captions/style"), style->currentIndex());
    settings.setValue(QStringLiteral("captions/length"), length->currentData());
    settings.setValue(QStringLiteral("captions/translate"), translate->isChecked());

    const QString file = model->currentData().toString();
    const QString path = modelPath(file);
    QString error;
    if (!QFileInfo::exists(path)) {
        if (QMessageBox::question(this, tr("Auto Captions"), tr("Download the speech model %1 from huggingface.co now?").arg(model->currentText()))
            != QMessageBox::Yes)
            return;
        if (!downloadFile(this, QUrl(QStringLiteral("https://huggingface.co/%1/resolve/main/%2").arg(model->currentData(Qt::UserRole + 1).toString(), file)),
                          QDir(modelDir()).filePath(file), tr("the speech model %1").arg(file), &error)) {
            QMessageBox::warning(this, tr("Auto Captions"), tr("The model could not be downloaded: %1").arg(error));
            return;
        }
    }
    TranscribeOptions options;
    options.language = language->currentData().toString();
    options.maxChars = length->currentData().toInt();
    options.translate = translate->isChecked();
    QProgressDialog progress(tr("Listening..."), tr("Cancel"), 0, 1000, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    int added = 0;
    if (!captionTimeline(path, options, from, to, style->currentIndex() == 1, &progress, &added, &error)) {
        progress.close();
        if (!progress.wasCanceled())
            QMessageBox::warning(this, tr("Auto Captions"), error);
        return;
    }
    statusBar()->showMessage(tr("Added %n caption(s)", nullptr, added), 5000);
}

bool MainWindow::captionTimeline(const QString& modelFile, TranscribeOptions options, TimeUs from, TimeUs to, bool kinetic,
                                 QProgressDialog* progress, int* added, QString* error)
{
    // Listened to on a worker thread while the window goes on; `progress` shows it and can cancel.
    options.speakerTurns = modelFile.contains(QLatin1String("tdrz")); // (that model marks them)
    const std::vector<float> audio = timelineSpeechAudio(*m_project->snapshot(), from, to);
    std::atomic<int> permille{0};
    std::atomic<bool> cancel{false};
    std::vector<Caption> captions;
    bool ok = false;
    QThread* worker = QThread::create([&] {
        ok = transcribe(modelFile, audio, options, [&](double p) {
            permille = int(p * 1000);
            return !cancel.load();
        }, captions, error);
    });
    QTimer tick;
    if (progress) {
        connect(progress, &QProgressDialog::canceled, this, [&] { cancel = true; });
        connect(&tick, &QTimer::timeout, this, [&] { progress->setValue(std::max(1, permille.load())); });
        tick.start(100);
    }
    QEventLoop loop;
    connect(worker, &QThread::finished, &loop, &QEventLoop::quit);
    worker->start();
    loop.exec();
    worker->wait();
    delete worker;
    if (!ok || captions.empty()) {
        if (ok)
            *error = tr("No speech was heard.");
        return false;
    }
    addCaptions(captions, from, kinetic);
    if (added)
        *added = int(captions.size());
    return true;
}

QString MainWindow::whisperModelPath(const QString& file)
{
    return modelPath(file);
}

void MainWindow::importSubtitles()
{
    QSettings settings;
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Subtitles"), settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/subtitles")), Profiles::instance().folder(QStringLiteral("Media"))).toString(),
                                                      tr("Subtitles (*.srt *.vtt);;All files (*)"));
    if (path.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/subtitles")), QFileInfo(path).absolutePath());
    QFile f(path);
    const std::vector<Caption> captions = f.open(QIODevice::ReadOnly) ? parseSubtitles(QString::fromUtf8(f.readAll())) : std::vector<Caption>();
    if (captions.empty()) {
        QMessageBox::warning(this, tr("Import Subtitles"), tr("No subtitles were found in %1.").arg(QFileInfo(path).fileName()));
        return;
    }
    addCaptions(captions, 0, false);
}

void MainWindow::exportSubtitles()
{
    const int selected = m_timeline->selection().empty() ? 0 : *m_timeline->selection().begin();
    const std::vector<Caption> captions = timelineCaptions(m_project->timeline(), selected);
    if (captions.empty()) {
        QMessageBox::information(this, tr("Export Subtitles"), tr("There are no text events to export (select one on the captions track)."));
        return;
    }
    QSettings settings;
    QString path = QFileDialog::getSaveFileName(this, tr("Export Subtitles"),
                                                QDir(settings.value(Profiles::instance().settingsKey(QStringLiteral("dirs/subtitles")), Profiles::instance().folder(QStringLiteral("Media"))).toString()).filePath(QStringLiteral("captions.srt")),
                                                tr("SubRip (*.srt);;WebVTT (*.vtt)"));
    if (path.isEmpty())
        return;
    settings.setValue(Profiles::instance().settingsKey(QStringLiteral("dirs/subtitles")), QFileInfo(path).absolutePath());
    QSaveFile f(path);
    const QString text = path.endsWith(QLatin1String(".vtt"), Qt::CaseInsensitive) ? writeVtt(captions) : writeSrt(captions);
    if (!f.open(QIODevice::WriteOnly) || f.write(text.toUtf8()) < 0 || !f.commit())
        QMessageBox::warning(this, tr("Export Subtitles"), tr("Could not write %1.").arg(path));
    else
        statusBar()->showMessage(tr("Exported %n subtitle(s)", nullptr, int(captions.size())), 5000);
}
