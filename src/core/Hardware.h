#pragma once

#include <QString>
#include <atomic>

struct AVBufferRef;

// GPU / hardware acceleration switches and shared devices.
namespace hw {

struct Settings {
    std::atomic<bool> hardwareDecoding{true};  // D3D11VA / VAAPI / ... video decoding
    std::atomic<bool> gpuCompositing{true};    // OpenGL renderer for preview and export
};
Settings& settings();

// Shared hardware device used by all video decoders (created on first use).
// Returns null when no hardware decoder device is available.
AVBufferRef* decodeDevice(int* deviceType = nullptr);
// e.g. "D3D11VA", or empty when none.
QString decodeDeviceName();

// True if the encoder can actually be opened on this machine (hardware
// encoders such as NVENC, AMF or Quick Sync need the matching GPU/driver).
bool encoderUsable(const QString& name);
bool isHardwareEncoder(const QString& name);

} // namespace hw
