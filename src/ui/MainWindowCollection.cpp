#include "ui/MainWindow.h"
#include "core/Project.h"
#include "core/Profiles.h"
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>

collection::Result MainWindow::collectProjectTo(const QString& destination,const collection::Options& options)
{
    if(m_collecting) {collection::Result result;result.error=tr("A project collection is already running.");return result;}
    m_collecting=true;
    struct Reset {bool& value;~Reset(){value=false;}} reset{m_collecting};
    const auto document=m_project->collectionDocument();
    const QString source=m_project->filePath().isEmpty()?QDir::currentPath():QFileInfo(m_project->filePath()).absolutePath();
    const QString name=m_project->filePath().isEmpty()?QStringLiteral("Project.mvp"):QFileInfo(m_project->filePath()).fileName();
    QProgressDialog dialog(tr("Preparing project collection..."),tr("Cancel"),0,1000,this);
    dialog.setWindowTitle(tr("Collect Project and Media"));dialog.setWindowModality(Qt::ApplicationModal);
    dialog.setMinimumDuration(0);dialog.setAutoClose(false);dialog.setAutoReset(false);dialog.resize(550,130);
    dialog.setAttribute(Qt::WA_DontShowOnScreen,testAttribute(Qt::WA_DontShowOnScreen));
    std::atomic<bool> cancel=false,done=false;std::mutex mutex;qint64 copied=0,total=0;QString file;
    connect(&dialog,&QProgressDialog::canceled,&dialog,[&]{cancel=true;});
    collection::Result result;
    std::thread worker([&] {
        try {
            result=collection::collect(document,source,destination,name,options,[&](qint64 count,qint64 size,const QString& current) {
                {std::lock_guard lock(mutex);copied=count;total=size;file=current;}return !cancel.load();
            });
        } catch(const std::exception& e) {result.error=QString::fromUtf8(e.what());}
        done=true;
    });
    QTimer timer;
    connect(&timer,&QTimer::timeout,&dialog,[&] {
        if(done.load()){dialog.accept();return;}
        qint64 currentCount,currentTotal;QString currentFile;
        {std::lock_guard lock(mutex);currentCount=copied;currentTotal=total;currentFile=file;}
        dialog.setValue(currentTotal?int(std::min(999.,double(currentCount)/double(currentTotal)*1000)):0);
        dialog.setLabelText(tr("Copying %1\n%2 MB of %3 MB").arg(QFileInfo(currentFile).fileName()).arg(currentCount/1048576.).arg(currentTotal/1048576.));
    });
    timer.start(50);dialog.exec();
    // Cancel hides the dialog; wait responsively until the copy has cleaned up its staging folder.
    if(!done.load()) {cancel=true;QEventLoop loop;connect(&timer,&QTimer::timeout,&loop,[&]{if(done.load())loop.quit();});loop.exec();}
    worker.join();return result;
}

void MainWindow::collectProjectDialog()
{
    QDialog dialog(this);dialog.setWindowTitle(tr("Collect Project and Media"));auto* layout=new QVBoxLayout(&dialog);
    auto* explanation=new QLabel(tr("Create a portable copy of the current edit and its assets in a new folder.\nThe collection includes changes you have not saved yet."));explanation->setWordWrap(true);layout->addWidget(explanation);
    auto* folder=new QLineEdit;folder->setPlaceholderText(tr("New collection folder"));folder->setObjectName("collectionDestination");
    QString base=m_project->filePath().isEmpty()?QStringLiteral("Project"):QFileInfo(m_project->filePath()).completeBaseName();
    auto unique=[&](const QString& parent){QString path=QDir(parent).filePath(base+"-collected");for(int i=2;QFileInfo::exists(path);++i)path=QDir(parent).filePath(base+QString("-collected-%1").arg(i));return path;};
    folder->setText(unique(Profiles::instance().folder("Projects")));layout->addWidget(folder);
    auto* browse=new QPushButton(tr("Choose Parent Folder..."));layout->addWidget(browse);
    connect(browse,&QPushButton::clicked,&dialog,[&]{const QString parent=QFileDialog::getExistingDirectory(&dialog,tr("Choose Parent Folder"),QFileInfo(folder->text()).absolutePath());if(!parent.isEmpty())folder->setText(unique(parent));});
    auto* unused=new QCheckBox(tr("Include unused items from Project Media"));unused->setChecked(true);unused->setObjectName("collectionUnusedMedia");layout->addWidget(unused);
    auto* models=new QCheckBox(tr("Include saved RVC voice models (for regenerating narration)"));models->setChecked(true);models->setObjectName("collectionVoiceModels");layout->addWidget(models);
    auto* detail=new QLabel(tr("Media, narration takes, graphics images, LUTs and waveform sources are copied. A report lists missing files and required fonts or VST plugins. Plugins, installed fonts and regenerable caches are not copied."));detail->setWordWrap(true);layout->addWidget(detail);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel);buttons->button(QDialogButtonBox::Ok)->setText(tr("Collect"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,&dialog,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    dialog.resize(570,330);if(dialog.exec()!=QDialog::Accepted)return;
    const auto result=collectProjectTo(folder->text().trimmed(),{unused->isChecked(),models->isChecked()});
    if(result.cancelled)return;
    if(!result.ok){QMessageBox::critical(this,tr("Collect Project and Media"),result.error);return;}
    QMessageBox message(this);message.setWindowTitle(tr("Project Collection Complete"));
    message.setIcon(result.missing.isEmpty()?QMessageBox::Information:QMessageBox::Warning);
    message.setText(tr("Copied %1 files (%2 MB).\n\n%3").arg(result.copiedFiles).arg(result.copiedBytes/1048576.,0,'f',1).arg(result.projectPath));
    if(!result.warnings.isEmpty())message.setInformativeText(result.warnings.join('\n'));
    auto* show=message.addButton(tr("Open Folder"),QMessageBox::ActionRole);message.addButton(QMessageBox::Ok);message.exec();
    if(message.clickedButton()==show)QDesktopServices::openUrl(QUrl::fromLocalFile(folder->text().trimmed()));
}
