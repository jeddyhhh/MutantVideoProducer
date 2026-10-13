#include "ui/MainWindow.h"
#include "ui/VoiceJob.h"
#include "ui/LiveView.h"
#include "engine/LiveEngine.h"
#include "core/Profiles.h"
#include "ui/Help.h"
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLockFile>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QCoreApplication>

void MainWindow::profilesDialog()
{
    QDialog dlg(this); dlg.setWindowTitle(tr("Profiles & Home Folders")); dlg.setObjectName(QStringLiteral("profilesDialog")); dlg.resize(660, 320);
    auto* layout = new QVBoxLayout(&dlg);
    auto* explanation = new QLabel(tr("Choose a home for this profile's projects, downloads, live recordings and generated voices. Its media library is available in every project. Imported files are shared by reference."));
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* form = new QFormLayout; layout->addLayout(form);
    auto* choices = new QComboBox; choices->setObjectName(QStringLiteral("profileChoice"));
    choices->addItem(tr("Default (existing folders)"), QString());
    const auto profiles = Profiles::instance().list();
    for (const auto& p : profiles) choices->addItem(p.name, p.id);
    choices->addItem(tr("Create a new profile..."), QStringLiteral("new"));
    form->addRow(tr("Profile"), choices);
    auto* name = new QLineEdit; name->setObjectName(QStringLiteral("profileName")); form->addRow(tr("Name"), name);
    auto* home = new QLineEdit; home->setObjectName(QStringLiteral("profileHome"));
    auto* browse = new QPushButton(tr("Choose Folder...")); auto* row = new QHBoxLayout; row->addWidget(home, 1); row->addWidget(browse);
    form->addRow(tr("Home folder"), row);
    auto show = [&] {
        const QString id = choices->currentData().toString(); name->clear(); home->clear();
        for (const auto& p : profiles) if (p.id == id) { name->setText(p.name); home->setText(QDir::toNativeSeparators(p.home)); }
        name->setEnabled(!id.isEmpty()); home->setEnabled(!id.isEmpty()); browse->setEnabled(!id.isEmpty());
    };
    connect(choices, qOverload<int>(&QComboBox::currentIndexChanged), &dlg, [&](int) { show(); });
    choices->setCurrentIndex(std::max(0, choices->findData(Profiles::instance().active().id))); show();
    connect(browse, &QPushButton::clicked, &dlg, [&] {
        const QString path = QFileDialog::getExistingDirectory(&dlg, tr("Profile Home Folder"), home->text());
        if (!path.isEmpty()) home->setText(QDir::toNativeSeparators(path));
    });
    auto* note = new QLabel(tr("Changing profiles closes the current project after resolving unsaved changes. Changing a home folder leaves existing files in their current location; choose the old home again to access them."));
    note->setWordWrap(true); layout->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(tr("Save & Use Profile")); layout->addWidget(buttons);
    help::addButton(buttons, QStringLiteral("profiles.md"));
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, [&] {
        if (voiceJob()->busy() || m_live->engine()->isRecording()) {
            QMessageBox::information(&dlg, tr("Profiles"), tr("Finish voice generation or stop the live recording before changing profiles.")); return;
        }
        const QString id = choices->currentData().toString();
        if (!id.isEmpty() && (name->text().trimmed().isEmpty() || !QDir::isAbsolutePath(QDir::fromNativeSeparators(home->text())))) {
            QMessageBox::warning(&dlg, tr("Profiles"), tr("Enter a profile name and choose an absolute home folder.")); return;
        }
        const auto previous = Profiles::instance().active();
        const bool switching = id != previous.id || (!id.isEmpty() && Profiles::cleanPath(previous.home) != Profiles::cleanPath(QDir::fromNativeSeparators(home->text())));
        if (switching && !maybeSave()) return;
        QString error;
        const bool ok = id.isEmpty() ? Profiles::instance().useLegacy(&error)
            : Profiles::instance().saveAndActivate({id == QLatin1String("new") ? QString() : id, name->text(), QDir::fromNativeSeparators(home->text())}, &error);
        if (!ok) { QMessageBox::warning(&dlg, tr("Profiles"), error); return; }
        if (switching) {
            newProject();
            m_autosaveLock.reset();
            const QString base = QDir(autosaveDir()).filePath(QStringLiteral("session-%1-%2").arg(QCoreApplication::applicationPid()).arg(QDateTime::currentMSecsSinceEpoch()));
            m_autosavePath = base + QStringLiteral(".mvp");
            m_autosaveLock = std::make_unique<QLockFile>(base + QStringLiteral(".lock")); m_autosaveLock->setStaleLockTime(0); m_autosaveLock->tryLock(0);
        }
        updateTitle(); dlg.accept();
    });
    dlg.exec();
}
