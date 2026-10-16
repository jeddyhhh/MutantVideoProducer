#pragma once
// Getting a finished video onto YouTube: the description's track list and chapters, and a thumbnail.

#include "core/Project.h"

#include <QImage>
#include <QStringList>

namespace upload {
// A description timestamp: "04:19", or "1:04:19" when the video is an hour or longer.
QString youtubeTime(TimeUs t, bool hours);

// A track's events as a YouTube description list: "01. Name - 04:19" ("1:04:19" from an hour on).
// Names are the event's own name, or its file name without the extension and a leading track number.
QString trackListing(const Project& project, int track);

// YouTube chapters ("00:00 Intro" per line) from the story chapters, or else from the markers and
// regions. An "Intro" at 00:00 is added when the first one starts later. YouTube shows chapters only
// when there are three or more, each at least 10 s long; `warnings` says what breaks that.
QString youtubeChapters(const Timeline& timeline, QStringList* warnings = nullptr);

// A 1280x720 YouTube thumbnail: `frame` filling the picture (centre-cropped) with `title`, if any,
// in large outlined type along the bottom.
QImage thumbnail(const QImage& frame, const QString& title);
} // namespace upload
