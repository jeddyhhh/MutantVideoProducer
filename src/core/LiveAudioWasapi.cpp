#include "core/LiveAudioWasapi.h"

#ifdef _WIN32

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace wasapi {
namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com()
    {
        if (p)
            p->Release();
    }
    T* operator->() const { return p; }
    void** put() { return reinterpret_cast<void**>(&p); }
};

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit()
    {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};

// PKEY_Device_FriendlyName (spelled out: the header's definition needs a GUID library).
const PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

std::wstring describe(const wchar_t* what, HRESULT hr)
{
    wchar_t code[16];
    swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return std::wstring(what) + L" (" + code + L")";
}

std::wstring deviceId(IMMDevice* dev)
{
    LPWSTR id = nullptr;
    std::wstring s;
    if (SUCCEEDED(dev->GetId(&id)) && id) {
        s = id;
        CoTaskMemFree(id);
    }
    return s;
}

std::wstring friendlyName(IMMDevice* dev)
{
    Com<IPropertyStore> props;
    std::wstring name;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, reinterpret_cast<IPropertyStore**>(props.put())))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
            name = v.pwszVal;
        PropVariantClear(&v);
    }
    return name;
}

bool createEnumerator(Com<IMMDeviceEnumerator>& en, HRESULT* hr)
{
    *hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), en.put());
    return SUCCEEDED(*hr);
}

double now100ns()
{
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * 1e7 / freq;
}

} // namespace

std::vector<Output> listOutputs()
{
    std::vector<Output> out;
    // On a fresh thread: the caller's thread may be in another COM apartment.
    std::thread([&out] {
        ComInit com;
        Com<IMMDeviceEnumerator> en;
        HRESULT hr;
        if (!createEnumerator(en, &hr))
            return;
        std::wstring defaultId;
        {
            Com<IMMDevice> dev;
            if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, reinterpret_cast<IMMDevice**>(dev.put()))))
                defaultId = deviceId(dev.p);
        }
        Com<IMMDeviceCollection> devices;
        if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, reinterpret_cast<IMMDeviceCollection**>(devices.put()))))
            return;
        UINT count = 0;
        devices->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            Com<IMMDevice> dev;
            if (FAILED(devices->Item(i, reinterpret_cast<IMMDevice**>(dev.put()))))
                continue;
            Output o;
            o.id = deviceId(dev.p);
            o.name = friendlyName(dev.p);
            o.isDefault = !defaultId.empty() && o.id == defaultId;
            if (o.name.empty())
                o.name = o.id;
            if (o.isDefault)
                out.insert(out.begin(), o);
            else
                out.push_back(o);
        }
    }).join();
    return out;
}

bool loopback(const std::wstring& id, const std::atomic<bool>& quit, const std::function<void(const Format&)>& opened,
              const Chunk& chunk, std::wstring* error)
{
    ComInit com;
    auto fail = [&](const wchar_t* what, HRESULT hr) {
        if (error)
            *error = describe(what, hr);
        return false;
    };
    HRESULT hr;
    Com<IMMDeviceEnumerator> en;
    if (!createEnumerator(en, &hr))
        return fail(L"no audio devices", hr);
    Com<IMMDevice> dev;
    hr = id.empty() ? en->GetDefaultAudioEndpoint(eRender, eConsole, reinterpret_cast<IMMDevice**>(dev.put()))
                    : en->GetDevice(id.c_str(), reinterpret_cast<IMMDevice**>(dev.put()));
    if (FAILED(hr))
        return fail(L"output device not found", hr);
    Com<IAudioClient> client;
    if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put())))
        return fail(L"cannot open the output device", hr);
    WAVEFORMATEX* mix = nullptr;
    if (FAILED(hr = client->GetMixFormat(&mix)) || !mix)
        return fail(L"no mix format", hr);
    Format format;
    format.rate = int(mix->nSamplesPerSec);
    format.channels = mix->nChannels;
    WORD tag = mix->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix);
        format.channelMask = ext->dwChannelMask;
        tag = WORD(ext->SubFormat.Data1); // KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT carry the format tag here
    }
    bool supported = true;
    if (tag == WAVE_FORMAT_IEEE_FLOAT && mix->wBitsPerSample == 32)
        format.sample = Format::Sample::Float;
    else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 16)
        format.sample = Format::Sample::S16;
    else if (tag == WAVE_FORMAT_PCM && mix->wBitsPerSample == 32)
        format.sample = Format::Sample::S32;
    else
        supported = false;
    if (supported)
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000 /* 100 ms buffer */, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (!supported)
        return fail(L"unsupported output format", E_FAIL);
    if (FAILED(hr))
        return fail(L"cannot record the output device", hr);
    Com<IAudioCaptureClient> capture;
    if (FAILED(hr = client->GetService(__uuidof(IAudioCaptureClient), capture.put())))
        return fail(L"cannot record the output device", hr);
    if (FAILED(hr = client->Start()))
        return fail(L"cannot start recording the output device", hr);
    opened(format);

    const double frame100ns = 1e7 / format.rate;
    double filledUntil = now100ns(); // end of the sound delivered so far (100 ns)
    bool ok = true;
    while (!quit) {
        UINT32 packet = 0;
        if (FAILED(hr = capture->GetNextPacketSize(&packet))) {
            ok = fail(L"the output device stopped", hr); // unplugged, format changed...
            break;
        }
        if (packet == 0) {
            // Nothing is playing (Windows then sends no packets): fill with
            // silence up to 20 ms ago, in steps of 30 ms or more.
            const double now = now100ns();
            const double gap = now - 200000.0 - filledUntil;
            if (gap >= 300000.0) {
                const int frames = int(gap / frame100ns);
                chunk(nullptr, frames, format, (now - filledUntil) / 1e7);
                filledUntil += frames * frame100ns;
            }
            Sleep(5);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devicePos = 0, qpcPos = 0;
        if (FAILED(hr = capture->GetBuffer(&data, &frames, &flags, &devicePos, &qpcPos))) {
            ok = fail(L"the output device stopped", hr);
            break;
        }
        const double now = now100ns();
        const double start = (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpcPos == 0 ? now - frames * frame100ns : double(qpcPos);
        if (frames > 0)
            chunk((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, int(frames), format, (now - start) / 1e7);
        filledUntil = std::max(filledUntil, start + frames * frame100ns);
        capture->ReleaseBuffer(frames);
    }
    client->Stop();
    return ok;
}

} // namespace wasapi

#endif
