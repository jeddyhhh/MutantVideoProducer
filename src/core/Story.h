#pragma once
#include "core/Project.h"
#include <set>
namespace story {
// Chapter data and all affected timeline edits commit together through Project::applyEdit.
int addChapter(Timeline& timeline, const QString& name, const QString& script, TimeUs narration, TimeUs listening,
               const MediaItem* source, TimeUs sourceIn, const FrameRate& fps, QString* error = nullptr);
bool updateChapter(Timeline& timeline, int id, const QString& name, const QString& script, TimeUs narration,
                   TimeUs listening, QString* error = nullptr);
bool moveChapter(Timeline& timeline, int id, int direction, QString* error = nullptr);
bool removeChapter(Timeline& timeline, int id, QString* error = nullptr);
bool bindChapter(Timeline& timeline, int id, const std::set<int>& clips, QString* error = nullptr);
int adoptChapter(Timeline& timeline,const QString& name,const QString& script,TimeUs from,TimeUs to,TimeUs narration,QString* error=nullptr);
// Editable embedded sequences, with grouped picture and sound containers.
std::vector<int> compound(Timeline& timeline, const std::set<int>& selection, const QString& name, QString* error = nullptr);
bool unpack(Timeline& timeline, int clipId, QString* error = nullptr);
bool replaceCompound(Timeline& timeline,int clipId,const Timeline& content,bool ripple,QString* error=nullptr);
// Seeded visual variations; never alter sound, script, clip placement or source sync.
int directorsDice(Timeline& timeline, const std::set<int>& selection, unsigned seed, int style, double intensity);
}
