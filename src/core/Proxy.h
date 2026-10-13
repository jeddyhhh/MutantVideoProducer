#pragma once

// Proxies: lightweight copies of heavy video (4K, HEVC, 10-bit...) that the
// editor's preview decodes instead, so playback and scrubbing stay smooth.
// Exports (and everything else) always read the original files.
//
// A proxy is the video stream alone, scaled down, in H.264 tuned for fast
// decoding (a keyframe every 12 frames, no B-frames), with exactly the
// source's timestamps, kept in the app's cache and named from the source's
// path, size and date, so it is found again from any project and dropped
// when the file changes.

#include <QString>
#include <functional>

struct MediaInfo;

namespace proxy {

// Where a stream's proxy lives (the app's cache, "proxies"; MVP_PROXY_DIR
// overrides the folder, for tests).
QString file(const QString& path, int stream);
bool exists(const QString& path, int stream);
// Heavy enough to want one: bigger than 1080p, HEVC / AV1 / ProRes / DNxHD,
// or more than 8 bits. Never for stills or pictures with transparency.
bool recommended(const MediaInfo& info);
// Makes the proxy: every frame scaled to `height` lines (never up).
// progress(0..1) returns false to cancel. Pictures with transparency are
// refused (the proxy couldn't keep it).
bool make(const QString& path, int stream, int height, const std::function<bool(double)>& progress, QString* error = nullptr);
// Deleting one the preview has open (Windows keeps open files) takes it out
// of use at once and deletes the file as soon as it is let go of.
bool remove(const QString& path, int stream);
// Deletes those still waiting (at exit).
void purge();

// The preview decodes proxies where they exist (on by default).
void setEnabled(bool on);
bool enabled();
// What the preview decodes for a stream: its proxy when proxies are on and
// it exists, otherwise the file itself.
QString playbackPath(const QString& path, int stream);

// All proxies in the cache: their total size; deleting them all.
qint64 totalBytes();
int removeAll();

} // namespace proxy
