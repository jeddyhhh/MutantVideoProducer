#pragma once

// Downloading a video (or a part of it) from YouTube and the other sites
// yt-dlp knows, for Create Clip from YouTube. yt-dlp is a separate program
// (the app fetches it once); FFmpeg's ffmpeg.exe joins the picture and sound.

#include "core/Time.h"

#include <QString>
#include <QUrl>
#include <QStringList>
#include <functional>

// (The whole video is fetched: fetching only a part goes through ffmpeg and
// is many times slower. Create Clip marks the part to use instead.)
struct YouTubeOptions {
    QString url;
    int maxHeight = 1080; // the tallest picture to fetch; 0 = the sound only
    QString folder;       // where the file goes
    QString cookiesBrowser; // optional, explicitly selected by the user
    QString cookiesFile;    // optional Netscape cookies file; never copied into the project
    QString jsRuntime;      // "deno:/path/deno" or "node:/path/node"; auto-detected when empty
};

// Where yt-dlp is: on the PATH (Linux packages), else the copy Mutant Video Producer downloads (which may not
// be there yet), and where that copy comes from.
QString ytdlpPath();
QUrl ytdlpDownloadUrl();
// The folder with ffmpeg in it, for yt-dlp to join picture and sound: next to Mutant Video Producer (the
// Windows release), else on the PATH; empty when there is none.
QString ffmpegFolder();
QString youtubeJsRuntime();
QStringList youtubeArguments(const QString& ffmpegDir, const YouTubeOptions& options);
enum class YouTubeFailure { Authentication, Cookies, RateLimit, JavaScript, Token, Unavailable, Other };
YouTubeFailure youtubeFailure(const QString& error);
QString youtubeFailureHelp(const QString& error);

// Runs `ytdlp` (with ffmpeg in `ffmpegDir`, or yt-dlp's own search when it is empty). `progress` gets 0..1 (or -1 while
// it can't tell) and returns false to cancel. The file's path, or empty with `error`.
QString youtubeDownload(const QString& ytdlp, const QString& ffmpegDir, const YouTubeOptions& options,
                        const std::function<bool(double)>& progress, QString* error);

// "1:30", "1:02:03", "90" or "90.5" (seconds); -1 when empty or not a time.
TimeUs parseClock(const QString& text);
