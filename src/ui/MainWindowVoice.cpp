#include "ui/MainWindow.h"
#include "ui/VoiceJob.h"
#include "ui/TimelineWidget.h"
#include "core/Voice.h"
#include <QAudioOutput>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMediaPlayer>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

VoiceJob* MainWindow::voiceJob()
{
    if (!m_voiceJob) m_voiceJob = new VoiceJob(m_project, this);
    return m_voiceJob;
}

void MainWindow::generateVoice()
{
    if (voiceJob()->busy()) {
        QMessageBox::information(this, tr("Generate Voice"), tr("Voice generation is already running. Wait for it to finish.")); return;
    }
    const auto voices = localVoiceCatalog();
    if (voices.empty()) {
        QMessageBox::warning(this, tr("Generate Voice"), tr("The voice catalog is missing from this build.")); return;
    }
    QSettings settings;
    QDialog dlg(this); dlg.setWindowTitle(tr("Generate Voice from Captions")); dlg.setObjectName(QStringLiteral("generateVoiceDialog"));
    dlg.resize(680, 600);
    auto* layout = new QVBoxLayout(&dlg);
    auto* intro = new QLabel(tr("Turn captions into local AI speech on a new audio track. Download voices once, then work offline."), &dlg);
    intro->setWordWrap(true); layout->addWidget(intro);
    auto* tabs = new QTabWidget(&dlg); layout->addWidget(tabs);
    auto* speechPage = new QWidget(tabs); auto* form = new QFormLayout(speechPage); tabs->addTab(speechPage, tr("Speech & Voices"));
    auto* source = new QComboBox(&dlg);
    bool hasSelected = false;
    for (int id : m_timeline->selection()) {
        const auto* c = m_project->timeline().findClip(id);
        if (c && (c->generator.type == QLatin1String("text") || c->generator.type == QLatin1String("kinetic_text"))) hasSelected = true;
    }
    if (hasSelected) source->addItem(tr("Selected captions only"), -1);
    int preferredTrack = -1;
    for (const auto& t : m_project->timeline().tracks) {
        const auto text = std::find_if(t.clips.begin(), t.clips.end(), [](const Clip& c) {
            return c.generator.type == QLatin1String("text") || c.generator.type == QLatin1String("kinetic_text");
        });
        if (text == t.clips.end()) continue;
        source->addItem(tr("Track: %1").arg(t.name), text->id);
        for (const auto& c : t.clips)
            if (m_timeline->selection().count(c.id)) preferredTrack = source->count() - 1;
    }
    if (preferredTrack >= 0) source->setCurrentIndex(preferredTrack);
    form->addRow(tr("Captions"), source);
    auto* region = new QCheckBox(tr("Only captions starting inside the time selection"), &dlg);
    region->setEnabled(m_timeline->hasRegion()); form->addRow(QString(), region);
    auto* voice = new QComboBox(&dlg);
    auto refresh = [&] {
        const int current = std::max(0, voice->currentIndex()); voice->clear();
        for (const auto& v : voices) {
            const QString accent = v.language == QLatin1String("en-gb") ? tr("UK") : tr("US");
            const QString gender = v.id.at(1) == QLatin1Char('f') ? tr("female") : tr("male");
            voice->addItem(tr("%1 · %2, %3 · %4").arg(v.name, accent, gender, voiceAssetInstalled(v.asset) ? tr("Installed") : tr("Download")), v.id);
        }
        voice->setCurrentIndex(std::min(current, voice->count() - 1));
    };
    refresh(); voice->setCurrentIndex(std::max(0, voice->findData(settings.value(QStringLiteral("voice/id"), QStringLiteral("af_heart")))));
    form->addRow(tr("Voice library"), voice);
    auto* credit = new QLabel(tr("Kokoro · hexgrad · Apache-2.0 · English US / UK\nEach voice: about 510 KB. CPU model: 92 MB. GPU model: 326 MB.\nCUDA inference is automatic when supported, with CPU fallback."), &dlg);
    credit->setWordWrap(true); form->addRow(QString(), credit);
    auto* libraryRow = new QHBoxLayout;
    auto* download = new QPushButton(tr("Download Selected Voice + Model"), &dlg);
    auto* preview = new QPushButton(tr("Preview Voice"), &dlg);
    libraryRow->addWidget(download); libraryRow->addWidget(preview); form->addRow(QString(), libraryRow);
    auto* rate = new QDoubleSpinBox(&dlg); rate->setRange(0.5, 2); rate->setSingleStep(0.05); rate->setDecimals(2);
    rate->setValue(settings.value(QStringLiteral("voice/rate"), 1.0).toDouble()); rate->setSuffix(tr(" ×")); form->addRow(tr("Speech rate"), rate);
    auto* timing = new QComboBox(&dlg); timing->addItem(tr("Fit caption timing (up to 1.35× faster)"), QStringLiteral("fit"));
    timing->addItem(tr("Natural duration"), QStringLiteral("natural"));
    timing->setCurrentIndex(std::max(0, timing->findData(settings.value(QStringLiteral("voice/timing"), QStringLiteral("fit")))));
    form->addRow(tr("Timing"), timing);
    auto* group = new QCheckBox(tr("Join adjacent short captions into sentences"), &dlg);
    group->setChecked(settings.value(QStringLiteral("voice/group"), true).toBool()); form->addRow(QString(), group);
    auto* timingNote = new QLabel(tr("Speech keeps its pitch. Long or overlapping speech is flagged with a timing marker; later phrases move forward to keep every word audible."), &dlg);
    timingNote->setWordWrap(true); form->addRow(QString(), timingNote);

    auto* rvcBox = new QGroupBox(tr("RVC voice conversion (optional)"), &dlg); rvcBox->setCheckable(true); rvcBox->setChecked(false);
    tabs->addTab(rvcBox, tr("RVC Conversion")); auto* rvcForm = new QFormLayout(rvcBox);
    auto* rvcIntro = new QLabel(tr("Convert speech locally with Mutant Video Producer's bundled RVC engine. Import an exported .pth voice and optional matching .index. CUDA acceleration is automatic on supported NVIDIA GPUs, with CPU fallback. No separate RVC, Python or CUDA Toolkit installation is needed."), rvcBox);
    rvcIntro->setWordWrap(true); rvcForm->addRow(rvcIntro);
    auto* rvcModel = new QComboBox(rvcBox);
    auto refreshRvc = [&] {
        const QString previous = rvcModel->currentData().toString(); rvcModel->clear();
        QDir dir(voiceDataDir() + QStringLiteral("/rvc"));
        for (const auto& sub : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            QDir models(dir.filePath(sub));
            for (const auto& name : models.entryList({QStringLiteral("*.pth")}, QDir::Files)) rvcModel->addItem(name, models.filePath(name));
        }
        rvcModel->setCurrentIndex(std::max(0, rvcModel->findData(previous)));
    };
    refreshRvc(); rvcModel->setCurrentIndex(std::max(0, rvcModel->findData(settings.value(QStringLiteral("voice/rvcModel")))));
    auto* importModel = new QPushButton(tr("Import RVC Voice..."), rvcBox);
    auto* modelsRow = new QHBoxLayout; modelsRow->addWidget(rvcModel, 1); modelsRow->addWidget(importModel); rvcForm->addRow(tr("Converted voice"), modelsRow);
    auto* support = new QLabel(rvcBox); support->setWordWrap(true); rvcForm->addRow(support);
    auto refreshSupport = [&] {
        const auto assets = rvcSharedAssets();
        const bool installed = assets.size() == 4 && std::all_of(assets.begin(), assets.end(), voiceAssetInstalled);
        support->setText(installed ? tr("RVC support models installed. Ready for offline conversion.")
                                  : tr("Shared RVC support models: 370 MB download, once for all voices."));
    };
    refreshSupport();
    auto* downloadRvc = new QPushButton(tr("Download RVC Support Models"), rvcBox); rvcForm->addRow(downloadRvc);
    auto ensureRvc = [&] {
        const auto assets = rvcSharedAssets();
        if (assets.size() != 4) { QMessageBox::warning(&dlg, tr("RVC"), tr("The RVC model catalog is missing from this build.")); return false; }
        for (const auto& asset : assets) {
            if (verifyVoiceAsset(voiceAssetPath(asset), asset)) continue;
            QString error; const QString destination = QDir(voiceDataDir()).filePath(asset.file);
            if (!downloadFile(&dlg, QUrl(asset.url), destination, tr("RVC: %1").arg(QFileInfo(asset.file).fileName()), &error)) {
                if (!error.isEmpty()) QMessageBox::warning(&dlg, tr("RVC Download"), error); return false;
            }
            if (!verifyVoiceAsset(destination, asset)) {
                QFile::remove(destination); QMessageBox::warning(&dlg, tr("RVC Download"), tr("The download failed its SHA-256 check. Please try again.")); return false;
            }
        }
        refreshSupport(); return true;
    };
    connect(downloadRvc, &QPushButton::clicked, &dlg, [&] { ensureRvc(); });
    auto* pitch = new QSpinBox(rvcBox); pitch->setRange(-24, 24); pitch->setValue(settings.value(QStringLiteral("voice/pitch"), 0).toInt());
    pitch->setSuffix(tr(" semitones")); rvcForm->addRow(tr("Pitch"), pitch);
    auto* retrieval = new QDoubleSpinBox(rvcBox); retrieval->setRange(0, 1); retrieval->setSingleStep(0.05); retrieval->setValue(settings.value(QStringLiteral("voice/retrieval"), 0.0).toDouble());
    rvcForm->addRow(tr("Index retrieval"), retrieval);
    auto* protect = new QDoubleSpinBox(rvcBox); protect->setRange(0, 0.5); protect->setSingleStep(0.01); protect->setValue(settings.value(QStringLiteral("voice/protect"), 0.33).toDouble());
    rvcForm->addRow(tr("Consonant protection"), protect);
    auto* speaker = new QSpinBox(rvcBox); speaker->setRange(0, 9999); rvcForm->addRow(tr("RVC speaker ID"), speaker);
    auto* f0 = new QComboBox(rvcBox); f0->addItem(QStringLiteral("RMVPE"), QStringLiteral("rmvpe")); f0->addItem(QStringLiteral("Praat / PM"), QStringLiteral("pm"));
    rvcForm->addRow(tr("Pitch detection"), f0);
    connect(importModel, &QPushButton::clicked, &dlg, [&] {
        const QString model = QFileDialog::getOpenFileName(&dlg, tr("Import RVC Voice from a Trusted Source"), {}, tr("RVC model (*.pth)"));
        if (model.isEmpty()) return;
        const QString index = QFileDialog::getOpenFileName(&dlg, tr("Matching RVC Index (Cancel if None)"), QFileInfo(model).absolutePath(), tr("RVC index (*.index)"));
        const QString folder = voiceDataDir() + QStringLiteral("/rvc/") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString destination = QDir(folder).filePath(QFileInfo(model).fileName());
        if (!QDir().mkpath(folder) || !QFile::copy(model, destination) || (!index.isEmpty() && !QFile::copy(index, QDir(folder).filePath(QStringLiteral("voice.index"))))) {
            QDir(folder).removeRecursively(); QMessageBox::warning(&dlg, tr("RVC Voice"), tr("Could not copy the voice model.")); return;
        }
        refreshRvc(); rvcModel->setCurrentIndex(rvcModel->findData(destination)); retrieval->setValue(index.isEmpty() ? 0 : 0.75);
        rvcBox->setChecked(true);
    });
    auto ensureVoice = [&] {
        if (voiceJob()->busy()) return false;
        const auto& v = voices[size_t(voice->currentIndex())];
        for (const VoiceAsset& asset : {kokoroModelAsset(), kokoroGpuModelAsset(), v.asset}) {
            if (verifyVoiceAsset(voiceAssetPath(asset), asset)) continue;
            QString error; const QString destination = QDir(voiceDataDir()).filePath(asset.file);
            if (!downloadFile(&dlg, QUrl(asset.url), destination, tr("Kokoro: %1").arg(QFileInfo(asset.file).fileName()), &error)) {
                if (!error.isEmpty()) QMessageBox::warning(&dlg, tr("Voice Download"), error); return false;
            }
            if (!verifyVoiceAsset(destination, asset)) {
                QFile::remove(destination); QMessageBox::warning(&dlg, tr("Voice Download"), tr("The download failed its SHA-256 check. Please download it again.")); return false;
            }
        }
        refresh(); return true;
    };
    connect(download, &QPushButton::clicked, &dlg, [&] { ensureVoice(); });
    QMediaPlayer player; QAudioOutput audio; player.setAudioOutput(&audio);
    QTemporaryDir previews;
    auto options = [&] {
        QJsonObject o{{"rate", rate->value()}, {"timing", timing->currentData().toString()}, {"group", group->isChecked()}};
        if (rvcBox->isChecked()) {
            const QString model = rvcModel->currentData().toString();
            const QString index = QFileInfo(model).dir().filePath(QStringLiteral("voice.index"));
            o["rvc"] = QJsonObject{{"engine", "bundled"}, {"model", model}, {"index", QFileInfo::exists(index) ? index : QString()},
                                  {"pitch", pitch->value()}, {"index_rate", retrieval->value()}, {"protect", protect->value()}, {"speaker_id", speaker->value()}, {"f0_method", f0->currentData().toString()}};
        }
        return o;
    };
    auto run = [&](const std::vector<Caption>& captions, bool isPreview) {
        player.stop();
        if (rvcBox->isChecked() && (rvcModel->currentData().toString().isEmpty() || !QFileInfo::exists(rvcWorkerPath()))) {
            QMessageBox::warning(&dlg, tr("RVC"), rvcModel->currentData().toString().isEmpty() ? tr("Import an RVC .pth voice first.") : tr("The bundled RVC engine is missing from this build.")); return false;
        }
        if (!ensureVoice()) return false;
        if (rvcBox->isChecked() && !ensureRvc()) return false;
        QJsonObject o = options();
        if (isPreview) o["output_dir"] = previews.path();
        QString error;
        if (!voiceJob()->start(captions, voices[size_t(voice->currentIndex())], o, isPreview, &error)) {
            QMessageBox::warning(&dlg, tr("Generate Voice"), error); return false;
        }
        QProgressDialog progress(tr("Starting local speech engine..."), tr("Cancel"), 0, 1000, &dlg);
        progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0); progress.setAutoClose(false); progress.setAutoReset(false);
        QEventLoop loop;
        const auto p = connect(voiceJob(), &VoiceJob::progress, &progress, [&](double value, const QString& message) { progress.setLabelText(message); progress.setValue(int(value * 1000)); });
        const auto done = connect(voiceJob(), &VoiceJob::completed, &loop, &QEventLoop::quit);
        connect(&progress, &QProgressDialog::canceled, voiceJob(), &VoiceJob::cancel);
        loop.exec(); disconnect(p); disconnect(done); progress.close();
        const auto result = voiceJob()->status();
        if (!result.value("ok").toBool()) {
            if (result.value("error").toString() != tr("Cancelled")) QMessageBox::warning(&dlg, tr("Generate Voice"), result.value("error").toString()); return false;
        }
        if (isPreview) {
            std::vector<VoiceTake> takes;
            if (readVoiceTakes(voiceJob()->outputFolder(), takes, &error)) { player.setSource(QUrl::fromLocalFile(takes.front().path)); player.play(); }
        } else {
            const int warnings = result.value("timing_warnings").toInt();
            statusBar()->showMessage(tr("Generated %1 voice phrases on a new audio track.").arg(result.value("events").toInt()), 8000);
            if (warnings) QMessageBox::information(&dlg, tr("Voice Timing"), tr("%1 phrases need timing review. They are marked [Timing] and have timeline markers. All words were kept; later phrases were moved forward when needed.").arg(warnings));
        }
        return true;
    };
    connect(preview, &QPushButton::clicked, &dlg, [&] { run({Caption{0, 8 * kUsPerSecond, tr("This is a preview of your local AI voice in Mutant Video Producer."), 0}}, true); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Generate Voice")); buttons->button(QDialogButtonBox::Ok)->setEnabled(source->count() > 0);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        std::vector<Caption> caps;
        if (source->currentData().toInt() == -1) {
            for (const auto& track : m_project->timeline().tracks)
                for (const auto& c : track.clips)
                    if (m_timeline->selection().count(c.id) && (c.generator.type == QLatin1String("text") || c.generator.type == QLatin1String("kinetic_text"))) {
                        const auto text = c.generator.params.find(QStringLiteral("text"));
                        if (text != c.generator.params.end()) caps.push_back({c.start, c.end(), text->second.text,
                            c.generator.params.contains(QStringLiteral("caption_speaker")) ? int(c.generator.params.at(QStringLiteral("caption_speaker")).value) : 0});
                    }
        } else caps = timelineCaptions(m_project->timeline(), source->currentData().toInt());
        if (region->isChecked()) std::erase_if(caps, [&](const Caption& c) { return c.start < m_timeline->regionStart() || c.start >= m_timeline->regionEnd(); });
        if (caps.empty()) { QMessageBox::information(&dlg, tr("Generate Voice"), tr("There are no captions in this selection.")); return; }
        if (!run(caps, false)) return;
        settings.setValue(QStringLiteral("voice/id"), voice->currentData()); settings.setValue(QStringLiteral("voice/rate"), rate->value());
        settings.setValue(QStringLiteral("voice/timing"), timing->currentData()); settings.setValue(QStringLiteral("voice/group"), group->isChecked());
        settings.setValue(QStringLiteral("voice/rvcModel"), rvcModel->currentData()); settings.setValue(QStringLiteral("voice/pitch"), pitch->value());
        settings.setValue(QStringLiteral("voice/retrieval"), retrieval->value()); settings.setValue(QStringLiteral("voice/protect"), protect->value());
        dlg.accept();
    });
    dlg.exec();
}
