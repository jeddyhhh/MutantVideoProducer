#pragma once

// Multicam editing: video events of the same moment from several cameras (lined
// up first, e.g. by their sound: AudioSync.h) become the angles of a Multicam
// track on top, and the edit is cut between them. The angles stay on their own
// tracks (their picture hidden: the tracks are muted) and the Multicam track
// holds the cut: one event per stretch, each a piece of one angle, named
// "Angle N". Sound is left alone (keep the best recording playing).

#include "core/Project.h"

#include <QString>
#include <set>
#include <vector>

namespace multicam {

struct Angle {
    int number = 0; // 1..
    int clipId = 0; // the angle's event on its own track
    QString name;   // its event's name, or its file's
};

// The project's multicam: its track (-1: none) and angles.
int trackIndex(const Timeline& tl);
std::vector<Angle> angles(const Timeline& tl, const MediaMap& media);
// Which angle an event of the Multicam track shows (0: not one of them).
int angleOf(const Clip& c);

// Makes the video events `clipIds` (in that order: angle 1, 2...) the angles of a new Multicam track on top, starting
// on angle 1. False (and why) when there are fewer than two, one isn't a video file or a generated picture (an
// animation scene filmed from several cameras), one plays at another speed, or
// there is a multicam already.
bool create(Timeline& tl, const MediaMap& media, const std::vector<int>& clipIds, QString* error);
// From time t on (up to the next cut), the Multicam track shows `angle`: what pressing a number does while playing.
bool cut(Timeline& tl, const MediaMap& media, int angle, TimeUs t, QString* error);
// The stretch of the Multicam track under t shows `angle` instead: what pressing a number does when stopped.
bool switchAngle(Timeline& tl, const MediaMap& media, int angle, TimeUs t, QString* error);
// The events `leaving` are about to be removed: an angle whose own event is one of them passes to another piece of
// that event (it was split: the head of a camera's file cut off, a silence at its start removed), if one stays.
void passOn(Timeline& tl, const std::set<int>& leaving);

} // namespace multicam
