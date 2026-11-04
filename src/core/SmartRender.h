#pragma once

// Smart render: when the picture in a render's range is nothing but straight cuts of files that already are what the
// render asks for (the same codec, size and frame rate, nothing changed in them), their video is copied into the file
// instead of decoded and encoded again: much faster, and no quality lost. The sound is mixed and encoded as usual.
//
// A copy has to start each cut on a keyframe of its file and end it cleanly (where nothing shown before the cut needs a
// frame after it). The first cut may start anywhere: the frames before it are kept and hidden by the MP4's edit list.
// The render's end may come anywhere: its last picture or two (those needing one past it) hold the one before.
// Codecs where every frame is a keyframe (ProRes, DNxHR, MJPEG...) can be cut anywhere.

#include "core/Exporter.h"
#include "core/Project.h"

#include <QString>
#include <functional>
#include <vector>

struct AVCodecParameters;
struct AVFormatContext;
struct AVRational;
struct AVStream;

namespace smartrender {

struct Piece {
    QString path;
    int stream = -1;      // the video stream in the file
    TimeUs sourceIn = 0;  // where in the file it starts
    TimeUs length = 0;
    TimeUs start = 0;     // where it lands in the render (from the range's start)
};

struct Plan {
    bool ok = false;
    QString why; // when not: what stops it (for the render's status and the dialog)
    std::vector<Piece> pieces;
    int heldFrames = 0; // the end's last pictures that hold the one before (the end isn't on a keyframe)
};

// Whether the render s of snap can copy its video, and what from. Opens the files to check their codec and keyframes.
Plan plan(const ProjectSnapshot& snap, const ExportSettings& s);
// The copied stream's parameters (from the first piece) and the time base its packets come in.
int copyParameters(const Plan& plan, AVCodecParameters* out, AVRational* timeBase);
// Writes the pieces' video packets into vst of oc, one after another. progress gets each packet's time in the render
// (and -1 between pieces) and the bytes written; false stops it. Returns 0 or an FFmpeg error.
int copy(const Plan& plan, AVFormatContext* oc, AVStream* vst, const std::function<bool(TimeUs, qint64)>& progress);

} // namespace smartrender
