#pragma once
class QWidget;
class Project;
// Modal composition; Apply commits one undoable edit, Cancel preserves the
// event.
void composeGraphics(QWidget *parent, Project *project, int eventId);
void chooseWaveformAudio(QWidget *parent, Project *project, int eventId);
