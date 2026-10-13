#pragma once

// Windows desktop audio for the Live tab: WASAPI loopback capture of what a
// playback device plays (FFmpeg has no capture device for it). Plain Win32,
// used by LiveAudio.

#ifdef _WIN32

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wasapi {

struct Output {
    std::wstring id; // endpoint id
    std::wstring name;
    bool isDefault = false;
};

// Active playback devices, the default one first.
std::vector<Output> listOutputs();

struct Format {
    enum class Sample { Float, S16, S32 };
    int rate = 0;
    int channels = 0;
    uint64_t channelMask = 0; // speaker positions (0 = unknown)
    Sample sample = Sample::Float;
};

// `samples` are interleaved in `format` (null = silence); `age` is how many
// seconds ago the first of them was played.
using Chunk = std::function<void(const void* samples, int frames, const Format& format, double age)>;

// Captures what an output device plays (empty id = the default device) on
// the calling thread until `quit` is set. `opened` is called once the device
// is open, before any chunk. While nothing plays, Windows sends nothing:
// silence is delivered instead so the sound keeps its timing. Returns false
// with `error` set when the device cannot be opened or capture fails (e.g.
// it was unplugged).
bool loopback(const std::wstring& id, const std::atomic<bool>& quit, const std::function<void(const Format&)>& opened,
              const Chunk& chunk, std::wstring* error);

} // namespace wasapi

#endif
