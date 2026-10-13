#include "ui/AudioFinishingWindow.h"
#include "core/Project.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <atomic>
#include <chrono>
#include <future>
#include <cmath>

finishing::Report analyzeAudioMix(QWidget* parent, const ProjectSnapshot& snapshot) {
    QProgressDialog progress("Measuring the final mix, including track and master effects…", "Cancel", 0, 1000, parent);
    if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen)) progress.setAttribute(Qt::WA_DontShowOnScreen);
    progress.setObjectName("audioFinishingProgress");
    progress.setWindowTitle("Audio finishing / EBU R128");
    progress.setWindowModality(Qt::WindowModal); progress.setMinimumDuration(0);
    std::atomic<bool> cancel{false}; std::atomic<int> value{0};
    auto future = std::async(std::launch::async, [snapshot, &cancel, &value] {
        return finishing::analyze(snapshot, [&](double p) { value.store(int(p*1000)); return !cancel.load(); });
    });
    while (future.wait_for(std::chrono::milliseconds(20)) != std::future_status::ready) {
        progress.setValue(value.load()); QApplication::processEvents();
        if (progress.wasCanceled()) cancel.store(true);
    }
    auto report = future.get(); progress.close(); return report;
}

void showAudioFinishing(QWidget* parent, Project* project) {
    QDialog dialog(parent);
    if (parent && parent->testAttribute(Qt::WA_DontShowOnScreen)) dialog.setAttribute(Qt::WA_DontShowOnScreen);
    dialog.setWindowTitle("Audio Finishing"); dialog.setObjectName("audioFinishingWindow"); dialog.resize(760, 630);
    auto* layout = new QVBoxLayout(&dialog);
    auto* hint = new QLabel("Analyze the entire final stereo mix. Measurements include fades, ducking, envelopes, track FX and Master FX.");
    hint->setWordWrap(true); layout->addWidget(hint);
    auto* preset = new QComboBox; preset->setObjectName("audioFinishingPreset");
    for (const auto& target : finishing::targets()) preset->addItem(target.name);
    preset->addItem("Custom");
    auto* lufs = new QDoubleSpinBox; lufs->setRange(-36,-5); lufs->setDecimals(1); lufs->setSuffix(" LUFS"); lufs->setValue(-16);
    lufs->setObjectName("audioFinishingTarget");
    auto* ceiling = new QDoubleSpinBox; ceiling->setRange(-12,0); ceiling->setDecimals(1); ceiling->setSuffix(" dBTP"); ceiling->setValue(-1.5);
    ceiling->setObjectName("audioFinishingCeiling");
    auto* form = new QFormLayout; form->addRow("Loudness target",preset); form->addRow("Integrated loudness",lufs); form->addRow("True-peak ceiling",ceiling); layout->addLayout(form);
    auto* values = new QLabel("No measurements yet."); values->setObjectName("audioFinishingMeasurements"); values->setTextFormat(Qt::PlainText); values->setWordWrap(true); layout->addWidget(values);
    auto* peak = new QProgressBar; peak->setRange(0,600); peak->setValue(0); peak->setFormat("True peak: not measured"); layout->addWidget(peak);
    auto* notes = new QPlainTextEdit; notes->setReadOnly(true); notes->setObjectName("audioFinishingWarnings"); layout->addWidget(notes,1);
    auto* status = new QLabel("Press Analyze mix. Targets are editable starting points; check the destination's delivery requirements."); status->setWordWrap(true); layout->addWidget(status);
    auto* row = new QHBoxLayout;
    auto* scan = new QPushButton("Analyze mix"); scan->setObjectName("audioFinishingAnalyze");
    auto* apply = new QPushButton("Apply safe gain"); apply->setObjectName("audioFinishingApply"); apply->setEnabled(false);
    apply->setToolTip("Adds an undoable gain effect at the end of Master FX. Preserves dynamics and respects measured true-peak headroom. Reanalyze afterward.");
    auto* save = new QPushButton("Save report…"); save->setEnabled(false);
    auto* close = new QPushButton("Close");
    for (auto* b : {scan,apply,save,close}) row->addWidget(b); layout->addLayout(row);
    std::shared_ptr<const ProjectSnapshot> measured;
    finishing::Report report;
    auto target = [&] { return finishing::Target{preset->currentText(),lufs->value(),ceiling->value()}; };
    auto refresh = [&] {
        bool valid = measured && measured == project->snapshot() && report.error.isEmpty() && !report.cancelled;
        double gain = finishing::safeGain(report,target());
        apply->setEnabled(valid && std::isfinite(gain)); save->setEnabled(bool(measured));
        if (!measured) return;
        QStringList warnings = finishing::warnings(report,target());
        if (!valid) warnings.prepend("Measurements are stale or incomplete. Analyze the current project before applying gain.");
        notes->setPlainText(warnings.isEmpty() ? "No mix-check warnings for the selected target." : warnings.join("\n\n"));
        values->setText(QString("Integrated: %1 LUFS     Range: %2 LU\nTrue peak: %3 dBTP     Sample peak: %4 dBFS\nMaximum momentary: %5 LUFS     Maximum short-term: %6 LUFS\nStereo correlation: %7     Duration: %8 s")
                       .arg(report.integrated,0,'f',1).arg(report.range,0,'f',1).arg(report.truePeak,0,'f',2).arg(report.samplePeak,0,'f',2)
                       .arg(report.momentaryMax,0,'f',1).arg(report.shortTermMax,0,'f',1).arg(report.correlation,0,'f',2).arg(report.seconds,0,'f',1));
        peak->setValue(int(std::clamp(report.truePeak+60.,0.,60.)*10)); peak->setFormat(QString("True peak: %1 dBTP / ceiling %2 dBTP").arg(report.truePeak,0,'f',2).arg(ceiling->value(),0,'f',1));
        peak->setStyleSheet(report.truePeak > ceiling->value() ? "QProgressBar::chunk { background: #c94654; }" : "");
        status->setText(std::isfinite(gain) ? QString("Safe gain: %1 dB. Reanalyze after changes. Encoders can introduce additional peaks; check the delivered file too.").arg(gain,0,'f',2) : "No safe gain adjustment is available for these measurements.");
    };
    QObject::connect(project,&Project::changed,&dialog,refresh);
    QObject::connect(project,&Project::mediaChanged,&dialog,refresh);
    QObject::connect(preset,qOverload<int>(&QComboBox::activated),&dialog,[&](int index) {
        auto targets = finishing::targets(); if (index < int(targets.size())) { lufs->setValue(targets[index].lufs); ceiling->setValue(targets[index].truePeak); } refresh();
    });
    for (auto* spin : {lufs,ceiling}) QObject::connect(spin,qOverload<double>(&QDoubleSpinBox::valueChanged),&dialog,[&] { refresh(); });
    QObject::connect(scan,&QPushButton::clicked,&dialog,[&] {
        scan->setEnabled(false); apply->setEnabled(false);
        measured = project->snapshot(); report = analyzeAudioMix(&dialog,*measured);
        scan->setEnabled(true); refresh();
    });
    QObject::connect(apply,&QPushButton::clicked,&dialog,[&] {
        if (!measured || measured != project->snapshot()) { refresh(); return; }
        auto tl = project->timeline(); QString error;
        if (!finishing::applyGain(tl,report,target(),&error)) { QMessageBox::warning(&dialog,"Audio finishing",error); return; }
        project->applyEdit(tl,"Finish Audio / Safe Master Gain"); refresh();
    });
    QObject::connect(save,&QPushButton::clicked,&dialog,[&] {
        auto path = QFileDialog::getSaveFileName(&dialog,"Save audio finishing report",{},"JSON report (*.json)"); if(path.isEmpty())return;
        auto json = report.json(target()); json["stale"] = measured != project->snapshot();
        QSaveFile file(path); auto bytes = QJsonDocument(json).toJson();
        if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit()) QMessageBox::warning(&dialog,"Audio finishing","Could not save the report.");
    });
    QObject::connect(close,&QPushButton::clicked,&dialog,&QDialog::accept);
    dialog.exec();
}
