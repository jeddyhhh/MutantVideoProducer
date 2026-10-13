#include "ui/StoryPanel.h"
#include "ui/TimelineWidget.h"
#include "core/Story.h"
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

StoryPanel::StoryPanel(Project* project,TimelineWidget* timeline,Speak speak,QWidget* parent)
    : QWidget(parent),m_project(project),m_timeline(timeline),m_speak(std::move(speak))
{
    setObjectName("storyPanel");auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);auto* scroll=new QScrollArea;scroll->setWidgetResizable(true);outer->addWidget(scroll);auto* body=new QWidget;auto* layout=new QVBoxLayout(body);scroll->setWidget(body);
    auto* intro=new QLabel(tr("Plan chapters, assign footage and reserve time to let the music speak."));intro->setWordWrap(true);layout->addWidget(intro);
    m_chapters=new QListWidget; m_chapters->setObjectName("storyChapters");m_chapters->setMaximumHeight(150);layout->addWidget(m_chapters);
    connect(m_chapters,&QListWidget::currentRowChanged,this,[this]{loadChapter();});
    auto* form=new QFormLayout;layout->addLayout(form);
    m_name=new QLineEdit;m_name->setObjectName("chapterName");m_name->setPlaceholderText(tr("Chapter name"));form->addRow(tr("Chapter"),m_name);
    auto seconds=[&](const QString& label,double value){auto* spin=new QDoubleSpinBox;spin->setRange(0,3600);spin->setDecimals(2);spin->setValue(value);spin->setSuffix(tr(" s"));form->addRow(label,spin);return spin;};
    m_narration=seconds(tr("Narration window"),15);m_narration->setObjectName("chapterNarration");m_listening=seconds(tr("Listening only"),10);m_listening->setObjectName("chapterListening");
    m_media=new QComboBox;form->addRow(tr("Footage / music for a new chapter"),m_media);m_sourceIn=seconds(tr("Source starts at"),0);
    m_script=new QPlainTextEdit;m_script->setObjectName("chapterScript");m_script->setPlaceholderText(tr("Write the commentary here. Listening time remains free of narration."));layout->addWidget(m_script,1);
    auto* row=new QHBoxLayout;layout->addLayout(row);
    auto button=[&](const QString& text,std::function<void()> action){auto* b=new QPushButton(text);row->addWidget(b);connect(b,&QPushButton::clicked,this,std::move(action));return b;};
    button(tr("Add chapter"),[this]{Timeline tl=m_project->timeline();QString error;const auto source=m_project->mediaItem(m_media->currentData().toInt());const int id=story::addChapter(tl,m_name->text().isEmpty()?tr("New chapter"):m_name->text(),m_script->toPlainText(),TimeUs(m_narration->value()*kUsPerSecond),TimeUs(m_listening->value()*kUsPerSecond),source.get(),TimeUs(m_sourceIn->value()*kUsPerSecond),m_project->settings().fps,&error);if(!id){report(error);return;}m_project->applyEdit(tl,tr("Add Story Chapter"));refresh();for(int i=0;i<m_chapters->count();++i)if(m_chapters->item(i)->data(Qt::UserRole).toInt()==id)m_chapters->setCurrentRow(i);});
    button(tr("Chapter from region"),[this]{if(!m_timeline->hasRegion()){report(tr("Select a timeline region first."));return;}Timeline tl=m_project->timeline();QString error;int id=story::adoptChapter(tl,m_name->text().isEmpty()?tr("Chapter"):m_name->text(),m_script->toPlainText(),m_timeline->regionStart(),m_timeline->regionEnd(),std::min(m_timeline->regionEnd()-m_timeline->regionStart(),m_project->settings().fps.snap(TimeUs(m_narration->value()*kUsPerSecond))),&error);if(!id){report(error);return;}m_project->applyEdit(tl,tr("Chapter from Timeline Region"));for(int i=0;i<m_chapters->count();++i)if(m_chapters->item(i)->data(Qt::UserRole).toInt()==id)m_chapters->setCurrentRow(i);});
    button(tr("Save changes"),[this]{saveChapter();});button(tr("Go to"),[this]{for(auto v:m_project->timeline().chapters){auto c=v.toObject();if(c["id"].toInt()==currentId()){const auto at=c["start"].toString().toLongLong();m_timeline->setCursorTime(at);m_timeline->setRegion(at,at+c["narration"].toString().toLongLong()+c["listening"].toString().toLongLong());}}});
    row=new QHBoxLayout;layout->addLayout(row);
    button(tr("Assign selected clips"),[this]{Timeline tl=m_project->timeline();QString error;if(!story::bindChapter(tl,currentId(),m_timeline->selection(),&error)){report(error);return;}m_project->applyEdit(tl,tr("Assign Chapter Footage"));});
    button(tr("Move up"),[this]{Timeline tl=m_project->timeline();QString error;if(story::moveChapter(tl,currentId(),-1,&error))m_project->applyEdit(tl,tr("Move Story Chapter"));else report(error);});
    button(tr("Move down"),[this]{Timeline tl=m_project->timeline();QString error;if(story::moveChapter(tl,currentId(),1,&error))m_project->applyEdit(tl,tr("Move Story Chapter"));else report(error);});
    button(tr("Delete"),[this]{Timeline tl=m_project->timeline();QString error;if(story::removeChapter(tl,currentId(),&error))m_project->applyEdit(tl,tr("Delete Story Chapter"));else report(error);});
    row=new QHBoxLayout;layout->addLayout(row);
    button(tr("Generate chapter voice..."),[this]{if(!saveChapter())return;for(auto value:m_project->timeline().chapters){auto c=value.toObject();if(c["id"].toInt()!=currentId())continue;const auto at=c["start"].toString().toLongLong(),length=c["narration"].toString().toLongLong();if(length<=0||c["script"].toString().trimmed().isEmpty()){report(tr("Write a script and give narration a positive duration."));return;}m_speak({Caption{at,at+length,c["script"].toString()}},QJsonObject{{"chapter_id",currentId()}});}});
    auto* note=new QLabel(tr("Duration changes ripple following chapters and assigned material together. Drafts are retained when switching chapters; Save changes commits them to the project."));note->setWordWrap(true);layout->addWidget(note);
    connect(project,&Project::changed,this,[this]{refresh();});connect(project,&Project::mediaChanged,this,[this]{refresh();});refresh();
}
int StoryPanel::currentId() const {return m_chapters->currentItem()?m_chapters->currentItem()->data(Qt::UserRole).toInt():0;}
void StoryPanel::report(const QString& error){if(!error.isEmpty())QMessageBox::information(this,tr("Story"),error);}
void StoryPanel::refresh()
{
    if(m_loading)return;
    m_loading=true;
    const auto session=m_project->snapshot()->session;
    if(m_session!=session) {
        m_session=session; m_drafts.clear(); m_loadedChapter={}; m_displayed=0;
        m_chapters->clear(); m_name->clear(); m_script->clear();
        m_narration->setValue(15); m_listening->setValue(10); m_sourceIn->setValue(0);
        m_media->setCurrentIndex(0);
    }
    const int selected=currentId();m_chapters->clear();
    for(auto value:m_project->timeline().chapters){auto c=value.toObject();auto* item=new QListWidgetItem(QStringLiteral("%1   (%2 s speech + %3 s listening)").arg(c["name"].toString()).arg(c["narration"].toString().toLongLong()/double(kUsPerSecond)).arg(c["listening"].toString().toLongLong()/double(kUsPerSecond)),m_chapters);item->setData(Qt::UserRole,c["id"].toInt());if(c["id"].toInt()==selected)m_chapters->setCurrentItem(item);}
    const int source=m_media->currentData().toInt();m_media->clear();m_media->addItem(tr("No source yet — chapter title placeholder"),0);for(const auto& [id,media]:m_project->media())m_media->addItem(media->displayName(),id);m_media->setCurrentIndex(std::max(0,m_media->findData(source)));m_loading=false;
    QJsonObject current;for(auto value:m_project->timeline().chapters)if(value.toObject()["id"].toInt()==selected)current=value.toObject();
    bool fieldsChanged=current["id"]!=m_loadedChapter["id"];
    for(const auto& key:{"name","script","narration","listening"})fieldsChanged|=current[key]!=m_loadedChapter[key];
    if(fieldsChanged){m_drafts.erase(selected);m_displayed=0;loadChapter();}else m_loadedChapter=current;
}
void StoryPanel::loadChapter()
{
    if(m_loading)return;
    if(m_displayed)m_drafts[m_displayed]=QJsonObject{{"name",m_name->text()},{"script",m_script->toPlainText()},{"narration",QString::number(TimeUs(m_narration->value()*kUsPerSecond))},{"listening",QString::number(TimeUs(m_listening->value()*kUsPerSecond))}};
    m_loading=true;m_loadedChapter={};m_displayed=currentId();
    for(auto value:m_project->timeline().chapters) {
        auto saved=value.toObject();if(saved["id"].toInt()!=m_displayed)continue;m_loadedChapter=saved;
        auto c=m_drafts.count(m_displayed)?m_drafts[m_displayed]:saved;
        m_name->setText(c["name"].toString());m_script->setPlainText(c["script"].toString());m_narration->setValue(c["narration"].toString().toLongLong()/double(kUsPerSecond));m_listening->setValue(c["listening"].toString().toLongLong()/double(kUsPerSecond));
    }
    m_loading=false;
}
bool StoryPanel::saveChapter()
{
    Timeline tl=m_project->timeline();QString error;const auto fps=m_project->settings().fps;
    if(story::updateChapter(tl,currentId(),m_name->text(),m_script->toPlainText(),fps.snap(TimeUs(m_narration->value()*kUsPerSecond)),fps.snap(TimeUs(m_listening->value()*kUsPerSecond)),&error)){m_project->applyEdit(tl,tr("Edit Story Chapter"));refresh();return true;}else {report(error);return false;}
}
