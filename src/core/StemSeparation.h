#pragma once

// Stem separation (Tools > AI Components > Stem Separation: Demucs, data/stems/worker.py): an audio event's sound split
// into voice and background, or vocals, drums, bass and other, each on its own track under the event.

#include "core/Project.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace stems {

// The installed engine's Python ("" when it isn't installed) and the worker's arguments for it.
QString workerPath();
QStringList workerArguments(const QString& program);
// Writes the sound event `clip` plays (its file's span, as it is heard: 44.1 kHz stereo float) to `path`. Refused (with
// why) for events at another speed, reversed or ramped, and for events that aren't from a file.
bool writeEventWav(const ProjectSnapshot& snap, const Clip& clip, const QString& path, QString* why);
// Puts the stems (mediaIds, named names: "voice", "background"...) on new audio tracks right under the event's track,
// each where the event is and as long, grouped together; the event itself is muted (kept, to compare). Returns the new
// events, or none (and why) when the event isn't there.
std::vector<int> place(Timeline& tl, const MediaMap& media, int clipId, const std::vector<int>& mediaIds, const QStringList& names,
                       QString* why = nullptr);

} // namespace stems
