#pragma once
#include "core/AudioFinishing.h"
class QWidget;
class Project;
void showAudioFinishing(QWidget* parent, Project* project);
// Responsive, cancellable scan shared by the window and MCP.
finishing::Report analyzeAudioMix(QWidget* parent, const ProjectSnapshot& snapshot);
