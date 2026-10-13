#pragma once

// Subtitles: SRT / WebVTT files in and out, and speech to text with Whisper
// (whisper.cpp, built in when deps/whisper is present) for auto captions.

#include "core/Time.h"

#include <QString>
#include <functional>
#include <vector>

struct ProjectSnapshot;

struct Caption {
    TimeUs start = 0, end = 0;
    QString text; // lines separated by '\n'
    int speaker = 0; // auto captions with speaker turns: 0, 1, 0... at each change of speaker
    bool operator==(const Caption&) const = default;
};

struct Timeline;
// The captions of a timeline: the text events on the track of event
// `clipId` (when it is text), else on the topmost track with text events.
std::vector<Caption> timelineCaptions(const Timeline& tl, int clipId = 0);

// Reads SRT or WebVTT (the format is told from the text); tags like <i> are dropped.
std::vector<Caption> parseSubtitles(const QString& text);
QString writeSrt(const std::vector<Caption>& captions);
QString writeVtt(const std::vector<Caption>& captions);

// The timeline's mix from `from` to `to` as mono 16 kHz (what Whisper listens to),
// up to the end of the last audio event and at most 2 hours.
std::vector<float> timelineSpeechAudio(const ProjectSnapshot& snap, TimeUs from, TimeUs to);

// Whether this build has Whisper, and whether it runs on the GPU here (Vulkan).
bool whisperAvailable();
bool whisperOnGpu();
struct TranscribeOptions {
    QString language = QStringLiteral("auto"); // "auto", "en", "de"...
    int maxChars = 42;                          // longest caption
    bool wordTimestamps = false; // one caption per recognised word, timed from the audio
    QString initialPrompt;
    bool translate = false;                     // into English
    bool speakerTurns = false;                  // mark changes of speaker (needs a "tdrz" model)
};
// Speech in `mono16k` to captions (times from the start of the audio).
// `progress` gets 0..1 and returns false to cancel. False (with `error`) on failure.
bool transcribe(const QString& modelPath, const std::vector<float>& mono16k, const TranscribeOptions& options,
                const std::function<bool(double)>& progress, std::vector<Caption>& out, QString* error = nullptr);
