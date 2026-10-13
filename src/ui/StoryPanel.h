#pragma once
#include "core/Captions.h"
#include "core/Project.h"
#include <QWidget>
#include <functional>
class TimelineWidget;
class QListWidget;
class QLineEdit;
class QPlainTextEdit;
class QDoubleSpinBox;
class QComboBox;
class StoryPanel : public QWidget {
    Q_OBJECT
public:
    using Speak = std::function<void(const std::vector<Caption>&,const QJsonObject&)>;
    StoryPanel(Project* project, TimelineWidget* timeline, Speak speak, QWidget* parent = nullptr);
private:
    void refresh();
    void loadChapter();
    bool saveChapter();
    void report(const QString& error);
    int currentId() const;
    Project* m_project; TimelineWidget* m_timeline; Speak m_speak;
    QListWidget* m_chapters; QLineEdit* m_name; QPlainTextEdit* m_script;
    QDoubleSpinBox *m_narration,*m_listening,*m_sourceIn;
    QComboBox* m_media;
    std::shared_ptr<const int> m_session;
    bool m_loading=false;
    int m_displayed=0;
    QJsonObject m_loadedChapter;
    std::map<int,QJsonObject> m_drafts;
};
