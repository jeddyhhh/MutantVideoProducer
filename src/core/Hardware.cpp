#include "core/Hardware.h"

#include "core/FFmpeg.h"

extern "C" {
#include <libavutil/hwcontext.h>
}

#include <algorithm>
#include <map>
#include <mutex>
#include <thread>

namespace hw {

Settings& settings()
{
    static Settings s;
    return s;
}

namespace {

std::mutex g_mutex;
AVBufferRef* g_device = nullptr;
AVHWDeviceType g_deviceType = AV_HWDEVICE_TYPE_NONE;
bool g_deviceTried = false;

} // namespace

AVBufferRef* decodeDevice(int* deviceType)
{
    std::lock_guard lock(g_mutex);
    if (!g_deviceTried) {
        g_deviceTried = true;
        // Preferred order per platform; the first one that initialises wins.
#ifdef _WIN32
        const AVHWDeviceType order[] = {AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2, AV_HWDEVICE_TYPE_CUDA};
#else
        const AVHWDeviceType order[] = {AV_HWDEVICE_TYPE_VAAPI, AV_HWDEVICE_TYPE_CUDA, AV_HWDEVICE_TYPE_VDPAU};
#endif
        for (AVHWDeviceType type : order) {
            AVBufferRef* dev = nullptr;
            if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) >= 0) {
                g_device = dev; // kept for the lifetime of the process
                g_deviceType = type;
                break;
            }
        }
    }
    if (deviceType)
        *deviceType = int(g_deviceType);
    return g_device;
}

QString decodeDeviceName()
{
    int type = 0;
    if (!decodeDevice(&type))
        return {};
    const char* name = av_hwdevice_get_type_name(AVHWDeviceType(type));
    return name ? QString::fromLatin1(name).toUpper() : QString();
}

bool isHardwareEncoder(const QString& name)
{
    for (const char* tag : {"_nvenc", "_amf", "_qsv", "_vaapi", "_vulkan", "_mf", "_d3d12va"})
        if (name.endsWith(QLatin1String(tag)))
            return true;
    return false;
}

bool encoderUsable(const QString& name)
{
    static std::mutex mutex;
    static std::map<QString, bool> cache;
    std::lock_guard lock(mutex);
    auto it = cache.find(name);
    if (it != cache.end())
        return it->second;

    bool ok = false;
    const QByteArray utf8 = name.toUtf8();
    if (const AVCodec* codec = avcodec_find_encoder_by_name(utf8.constData())) {
        if (!isHardwareEncoder(name)) {
            ok = true;
        } else {
            // Try to open it for real: this fails fast without the hardware.
            // On a thread of its own: Media Foundation encoders initialise and
            // shut down COM / Media Foundation on the calling thread, which must
            // not be the GUI thread (Qt's audio output and WinRT objects live there).
            std::thread([&] {
                ff::CodecContextPtr ctx(avcodec_alloc_context3(codec));
                ctx->width = 640;
                ctx->height = 360;
                ctx->time_base = AVRational{1, 30};
                ctx->framerate = AVRational{30, 1};
                const auto fmts = ff::encoderPixelFormats(codec);
                ctx->pix_fmt = AV_PIX_FMT_NV12;
                if (!fmts.empty() && std::find(fmts.begin(), fmts.end(), AV_PIX_FMT_NV12) == fmts.end())
                    ctx->pix_fmt = std::find(fmts.begin(), fmts.end(), AV_PIX_FMT_YUV420P) != fmts.end() ? AV_PIX_FMT_YUV420P : fmts.front();
                const int prev = av_log_get_level();
                av_log_set_level(AV_LOG_QUIET);
                ok = avcodec_open2(ctx.get(), codec, nullptr) >= 0;
                av_log_set_level(prev);
            }).join();
        }
    }
    cache[name] = ok;
    return ok;
}

} // namespace hw
