#pragma once

// The making-of replay: the project's undo history played back as a video, each step a moment (its name, the timeline's
// events sliding, popping in and fading out, effects stacking up as dots, the picture where it changed above).

#include <QString>
#include <functional>

class Project;

namespace makingof {

// Writes an MP4 (1280 x 720, 30 fps, no sound) to `path`; `progress` (0..1) returns false to stop. False on an error
// (said in `error`) or when stopped.
bool render(const Project& project, const QString& path, const std::function<bool(double)>& progress, QString* error);

} // namespace makingof
