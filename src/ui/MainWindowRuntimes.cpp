#include "ui/MainWindow.h"
#include "ui/VoiceJob.h"
#include "core/AiRuntime.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QVBoxLayout>
#include <algorithm>

bool MainWindow::installAiComponent(const QString& id, const QString& device, QString* error)
{
    auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (m_aiInstallBusy || voiceJob()->busy())
        return fail(tr("Wait for the current AI installation or voice generation to finish."));
    QString problem;
    const auto runtimes = parseAiRuntimes(aiRuntimeCatalog(), &problem);
    const auto found = std::find_if(runtimes.begin(), runtimes.end(), [&](const AiRuntime& r) { return r.id == id; });
    if (found == runtimes.end())
        return fail(problem.isEmpty() ? tr("This AI component is not available in this build.") : problem);
    QScopedValueRollback<bool> busy(m_aiInstallBusy, true);
    QProgressDialog progress(tr("Preparing %1").arg(found->name), tr("Cancel"), 0, 1000, this);
    progress.setWindowTitle(tr("Install AI Component"));
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setAutoClose(false);
    progress.setAutoReset(false);
    progress.resize(700, 300);
    return installAiRuntime(
        *found, device.isEmpty() ? QStringLiteral("cpu") : device,
        [&](double value, const QString& message) {
            progress.setLabelText(message);
            progress.setValue(int(value * 1000));
            QApplication::processEvents();
            return !progress.wasCanceled();
        },
        error);
}
void MainWindow::aiComponents()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("AI Components"));
    dialog.setObjectName(QStringLiteral("aiComponentsDialog"));
    dialog.resize(700, 440);
    auto* layout = new QVBoxLayout(&dialog);
    auto* intro = new QLabel(
        tr("Optional AI engines download directly from their upstream sources: Astral Python, PyPI and PyTorch. The app handles setup in a "
           "private folder; no separate Python installation is needed. Installed engines work offline and are shared by your profiles."));
    intro->setWordWrap(true);
    layout->addWidget(intro);
    QString problem;
    const auto runtimes = parseAiRuntimes(aiRuntimeCatalog(), &problem);
    if (runtimes.empty()) {
        auto* label = new QLabel(problem);
        label->setWordWrap(true);
        layout->addWidget(label);
    }
    for (const auto& runtime : runtimes) {
        layout->addWidget(new QLabel(QStringLiteral("<b>%1</b>").arg(runtime.name.toHtmlEscaped())));
        auto* status = new QLabel;
        status->setWordWrap(true);
        layout->addWidget(status);
        auto refresh = [=] {
            const QString path = runtime.id == "speech" ? VoiceJob::workerPath() : rvcWorkerPath();
            status->setText(QFileInfo::exists(path) ? tr("Installed — ready to use. Install again to repair or change acceleration.")
                                                    : tr("Not installed."));
        };
        refresh();
        auto* mode = new QComboBox;
        mode->addItem(tr("CPU — smaller download, works on any supported PC"), "cpu");
        mode->addItem(tr("NVIDIA CUDA — larger download, requires a compatible GPU driver"), "cuda");
        layout->addWidget(mode);
        auto* detail = new QLabel(
            runtime.id == "speech"
                ? tr("Speech: roughly 100–500 MB of engine downloads. CUDA speech shares CUDA/cuDNN libraries with the CUDA RVC engine; "
                     "install that too for NVIDIA acceleration. CPU fallback remains available.")
                : tr("RVC: roughly 500 MB for CPU, or 3 GB for CUDA. Allow 5 GB (CPU) or 12 GB (CUDA) of free space during setup."));
        detail->setWordWrap(true);
        layout->addWidget(detail);
        auto* install = new QPushButton(tr("Download & Install / Repair"));
        layout->addWidget(install);
        connect(install, &QPushButton::clicked, &dialog, [&, runtime, refresh, mode, install] {
            install->setEnabled(false);
            mode->setEnabled(false);
            QString error;
            if (!installAiComponent(runtime.id, mode->currentData().toString(), &error))
                QMessageBox::warning(&dialog, tr("AI Component"), error);
            refresh();
            install->setEnabled(true);
            mode->setEnabled(true);
        });
    }
    layout->addStretch();
    auto* note = new QLabel(
        tr("Voice and model downloads are separate: choose them in Generate Voice, Auto Captions, or Remove Background. Downloads use "
           "pinned versions and verified checksums. Cancelling or a failed repair keeps the existing working engine."));
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}
