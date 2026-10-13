#include "ui/MainWindow.h"
#include "engine/PlaybackEngine.h"
#include <QLineEdit>
#include <QScopedValueRollback>
#include "ui/TimelineWidget.h"
#include "ui/SequenceEditor.h"
#include "ui/VoiceJob.h"
#include "ui/PreviewWidget.h"
#include "core/Story.h"
#include "core/TimelineOps.h"
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QInputDialog>
#include <QLabel>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSpinBox>
#include <QVBoxLayout>
void MainWindow::createCompound()
{
    if(voiceJob()->busy()){QMessageBox::information(this,tr("Sequence"),tr("Wait for voice generation to finish."));return;}
    bool ok=false;const auto name=QInputDialog::getText(this,tr("Compound Sequence"),tr("Name"),QLineEdit::Normal,tr("Sequence"),&ok);if(!ok||name.trimmed().isEmpty())return;
    Timeline tl=m_project->timeline();auto selected=m_timeline->selection();
    if(selected.empty()&&m_timeline->hasRegion()) {
        ops::splitClips(tl,{},m_timeline->regionStart());ops::splitClips(tl,{},m_timeline->regionEnd());
        for(const auto& t:tl.tracks)for(const auto& c:t.clips)if(c.start>=m_timeline->regionStart()&&c.end()<=m_timeline->regionEnd())selected.insert(c.id);
    }
    QString error;const auto created=story::compound(tl,selected,name,&error);if(created.empty()){QMessageBox::information(this,tr("Sequence"),error);return;}
    m_project->applyEdit(tl,tr("Create Compound Sequence"));m_timeline->selectClips(std::set<int>(created.begin(),created.end()));
}
void MainWindow::editCompound(int id)
{
    if(voiceJob()->busy()){QMessageBox::information(this,tr("Sequence"),tr("Wait for voice generation to finish."));return;}
    QScopedValueRollback<bool> editing(m_editorTransaction,true);stopPlayback(false);if(!id)id=m_timeline->firstSelectedClip();if(!editEmbeddedSequence(m_project,id,this)&&(!m_project->timeline().findClip(id)||!m_project->timeline().findClip(id)->isSequence()))QMessageBox::information(this,tr("Sequence"),tr("Select a compound sequence first. Double-clicking a compound opens its tracks."));
}
void MainWindow::unpackCompound()
{
    Timeline tl=m_project->timeline();QString error;if(story::unpack(tl,m_timeline->firstSelectedClip(),&error))m_project->applyEdit(tl,tr("Unpack Compound Sequence"));else QMessageBox::information(this,tr("Sequence"),error);
}
void MainWindow::directorsDice()
{
    if(voiceJob()->busy()){QMessageBox::information(this,tr("Director's Dice"),tr("Wait for voice generation to finish."));return;}
    QScopedValueRollback<bool> editing(m_editorTransaction,true);stopPlayback(false);const Timeline before=m_project->timeline();auto selection=m_timeline->selection();if(selection.empty()){QMessageBox::information(this,tr("Director's Dice"),tr("Select footage or image clips to vary. Their sound and timing remain intact."));return;}
    const TimeUs originalCursor=m_timeline->cursorTime();TimeUs previewTime=originalCursor;const Clip* first=nullptr;bool within=false;
    for(const auto& track:before.tracks)if(isVisualTrack(track.type))for(const auto& clip:track.clips)if(selection.count(clip.id)&&!clip.isGenerated()){if(!first)first=&clip;if(originalCursor>=clip.start&&originalCursor<clip.end())within=true;}
    if(!within&&first)previewTime=first->start+std::min<TimeUs>(100000,first->length/2);
    QDialog dlg(this);dlg.setObjectName("directorsDice");dlg.setWindowTitle(tr("Director's Dice"));dlg.resize(520,340);auto* layout=new QVBoxLayout(&dlg);
    auto* intro=new QLabel(tr("Roll a new visual treatment for the selected footage. Keep the rhythm and sound; vary motion and accents. Preview, reroll, or cancel to restore the original."));intro->setWordWrap(true);layout->addWidget(intro);
    auto* form=new QFormLayout;layout->addLayout(form);auto* style=new QComboBox;style->addItems({tr("Measured — gentle camera motion"),tr("Restless — motion and brief colour splits"),tr("Riot — stronger motion and glitch entrances")});form->addRow(tr("Direction"),style);
    auto* strength=new QDoubleSpinBox;strength->setRange(0,1);strength->setSingleStep(.1);strength->setValue(.6);form->addRow(tr("Intensity"),strength);
    auto* seed=new QSpinBox;seed->setRange(0,1000000000);seed->setValue(int(QRandomGenerator::global()->bounded(1000000000u)));form->addRow(tr("Seed — reuse it for the same result"),seed);
    auto* preview=new PreviewWidget;preview->setObjectName("dicePreview");preview->setImage(m_preview->image());preview->setMinimumHeight(150);layout->addWidget(preview,1);
    auto* roll=new QPushButton(tr("Roll / Preview"));layout->addWidget(roll);Timeline variation=before;
    connect(roll,&QPushButton::clicked,&dlg,[&]{variation=before;const int count=story::directorsDice(variation,selection,unsigned(seed->value()),style->currentIndex(),strength->value());if(!count){QMessageBox::information(&dlg,tr("Director's Dice"),tr("Choose footage or images; captions and generated titles are kept intact."));return;}m_project->setTimelineLive(variation);seekTo(previewTime);});
    auto* reroll=new QPushButton(tr("Surprise me again"));layout->addWidget(reroll);connect(reroll,&QPushButton::clicked,&dlg,[&]{seed->setValue(int(QRandomGenerator::global()->bounded(1000000000u)));roll->click();});
    const auto image=connect(m_engine,&PlaybackEngine::frameReady,&dlg,[=](const QImage& frame,TimeUs){preview->setImage(frame);});
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel);layout->addWidget(buttons);connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&dlg,&QDialog::accept);connect(buttons,&QDialogButtonBox::rejected,&dlg,&QDialog::reject);
    seekTo(previewTime);
    const bool applied=dlg.exec()==QDialog::Accepted;disconnect(image);m_project->setTimelineLive(before);if(applied)m_project->applyEdit(variation,tr("Director's Dice — seed %1").arg(seed->value()));seekTo(originalCursor);
}
