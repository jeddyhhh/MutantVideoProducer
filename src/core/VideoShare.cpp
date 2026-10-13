#include "core/VideoShare.h"

#include "core/FFmpeg.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#endif

#include <QDateTime>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#ifdef Q_OS_WIN
using Microsoft::WRL::ComPtr;

namespace {

// ---- Spout's shared memory: a named file mapping with a "<name>_mutex" ----

struct SharedMemory {
    HANDLE map = nullptr;
    HANDLE mutex = nullptr;
    char* view = nullptr;
    size_t size = 0;

    ~SharedMemory() { close(); }
    // Creates it, or opens it when another app already did (its size then wins).
    bool create(const std::string& name, size_t bytes)
    {
        close();
        map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(bytes), name.c_str());
        if (!map)
            return false;
        return attach(name);
    }
    bool open(const std::string& name)
    {
        close();
        map = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (!map)
            return false;
        return attach(name);
    }
    bool attach(const std::string& name)
    {
        view = static_cast<char*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!view) {
            close();
            return false;
        }
        MEMORY_BASIC_INFORMATION info{};
        size = VirtualQuery(view, &info, sizeof(info)) ? info.RegionSize : 0;
        mutex = CreateMutexA(nullptr, FALSE, (name + "_mutex").c_str());
        return true;
    }
    bool lock()
    {
        if (!mutex)
            return true;
        const DWORD r = WaitForSingleObject(mutex, 67);
        return r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
    }
    void unlock()
    {
        if (mutex)
            ReleaseMutex(mutex);
    }
    void close()
    {
        if (view)
            UnmapViewOfFile(view);
        if (map)
            CloseHandle(map);
        if (mutex)
            CloseHandle(mutex);
        view = nullptr;
        map = mutex = nullptr;
        size = 0;
    }
};

constexpr int kNameLen = 256;           // SpoutMaxSenderNameLen
constexpr DWORD kFormatBgra = 87;       // DXGI_FORMAT_B8G8R8A8_UNORM

#pragma pack(push, 4)
struct SharedTextureInfo { // 280 bytes, as the Spout SDK lays it out
    uint32_t shareHandle;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t usage;
    uint8_t description[256];
    uint32_t partnerId;
};
#pragma pack(pop)
static_assert(sizeof(SharedTextureInfo) == 280);

int maxSenders()
{
    DWORD value = 64, size = sizeof(value);
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Leading Edge\\Spout", "MaxSenders", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        value = 64;
    return int(std::clamp<DWORD>(value, 1, 4096));
}

std::set<std::string> readNames(const SharedMemory& m)
{
    std::set<std::string> names;
    const int slotCount = int(std::min<size_t>(m.size / kNameLen, size_t(maxSenders())));
    for (int i = 0; i < slotCount; ++i) {
        const char* slot = m.view + size_t(i) * kNameLen;
        if (!slot[0])
            break;
        names.insert(std::string(slot, strnlen(slot, kNameLen - 1)));
    }
    return names;
}

void writeNames(SharedMemory& m, const std::set<std::string>& names)
{
    const int slotCount = int(std::min<size_t>(m.size / kNameLen, size_t(maxSenders())));
    int i = 0;
    for (const std::string& n : names) {
        if (i >= slotCount)
            break;
        char* slot = m.view + size_t(i) * kNameLen;
        std::memset(slot, 0, kNameLen);
        std::memcpy(slot, n.data(), std::min<size_t>(n.size(), kNameLen - 1));
        ++i;
    }
    if (i < slotCount)
        m.view[size_t(i) * kNameLen] = 0;
}

ComPtr<ID3D11Device> createDevice(ComPtr<ID3D11DeviceContext>* context)
{
    ComPtr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3,
                                   D3D11_SDK_VERSION, &device, nullptr, context ? context->GetAddressOf() : nullptr);
    if (FAILED(hr)) // 11.1 unknown on old runtimes
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels + 1, 2,
                               D3D11_SDK_VERSION, &device, nullptr, context ? context->GetAddressOf() : nullptr);
    return SUCCEEDED(hr) ? device : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Spout sender
// ---------------------------------------------------------------------------

struct SpoutSender::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    HANDLE shareHandle = nullptr;
    QSize size;
    std::string name;
    SharedMemory names, info;
    HANDLE access = nullptr;    // "<name>_SpoutAccessMutex"
    HANDLE frameCount = nullptr; // "<name>_Count_Semaphore"
    bool registered = false;

    bool makeTexture(QSize s)
    {
        texture.Reset();
        shareHandle = nullptr;
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(s.width());
        desc.Height = UINT(s.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED; // the legacy share handle every Spout receiver opens
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
            return false;
        ComPtr<IDXGIResource> res;
        if (FAILED(texture.As(&res)) || FAILED(res->GetSharedHandle(&shareHandle)) || !shareHandle) {
            texture.Reset();
            return false;
        }
        size = s;
        writeInfo();
        return true;
    }

    void writeInfo()
    {
        SharedTextureInfo ti{};
        ti.shareHandle = uint32_t(uintptr_t(shareHandle));
        ti.width = uint32_t(size.width());
        ti.height = uint32_t(size.height());
        ti.format = kFormatBgra;
        ti.usage = 0;
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::memcpy(ti.description, exe, std::min<size_t>(sizeof(ti.description) - 1, strnlen(exe, MAX_PATH)));
        ti.partnerId = 0; // shares a texture (not CPU memory)
        if (info.view && info.lock()) {
            std::memcpy(info.view, &ti, sizeof(ti));
            info.unlock();
        }
    }

    void unregister()
    {
        if (!registered)
            return;
        registered = false;
        if (names.view && names.lock()) {
            std::set<std::string> list = readNames(names);
            list.erase(name);
            writeNames(names, list);
            names.unlock();
            // The active sender becomes another one, if any.
            SharedMemory active;
            if (active.open("ActiveSenderName") && active.size >= kNameLen && active.lock()) {
                if (std::string(active.view, strnlen(active.view, kNameLen - 1)) == name) {
                    std::memset(active.view, 0, kNameLen);
                    if (!list.empty())
                        std::memcpy(active.view, list.begin()->data(), std::min<size_t>(list.begin()->size(), kNameLen - 1));
                }
                active.unlock();
            }
        }
    }
};

SpoutSender::SpoutSender() : d(std::make_unique<Impl>()) {}
SpoutSender::~SpoutSender()
{
    close();
}

bool SpoutSender::available()
{
    return createDevice(nullptr) != nullptr;
}

bool SpoutSender::open(const QString& wanted, QString* error)
{
    close();
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        close();
        return false;
    };
    d->device = createDevice(&d->context);
    if (!d->device)
        return fail(QStringLiteral("Direct3D 11 isn't available"));
    if (!d->names.create("SpoutSenderNames", size_t(maxSenders()) * kNameLen) || d->names.size < kNameLen)
        return fail(QStringLiteral("can't open Spout's list of senders"));
    if (!d->names.lock())
        return fail(QStringLiteral("Spout's list of senders is busy"));
    std::set<std::string> list = readNames(d->names);
    std::string base = wanted.trimmed().toUtf8().toStdString();
    if (base.empty())
        base = "Mutant Video Producer";
    base.resize(std::min<size_t>(base.size(), kNameLen - 8));
    std::string name = base;
    for (int i = 1; list.count(name); ++i) // taken (another Mutant Video Producer): Mutant Video Producer_1, Mutant Video Producer_2...
        name = base + "_" + std::to_string(i);
    list.insert(name);
    writeNames(d->names, list);
    d->names.unlock();
    d->name = name;
    d->registered = true;
    if (!d->info.create(name, sizeof(SharedTextureInfo)))
        return fail(QStringLiteral("can't create the sender's information"));
    d->access = CreateMutexA(nullptr, FALSE, (name + "_SpoutAccessMutex").c_str());
    d->frameCount = CreateSemaphoreA(nullptr, 1, LONG_MAX, (name + "_Count_Semaphore").c_str());
    // The newest sender is the active one, as with the Spout SDK.
    SharedMemory active;
    if (active.create("ActiveSenderName", kNameLen) && active.lock()) {
        std::memset(active.view, 0, kNameLen);
        std::memcpy(active.view, name.data(), name.size());
        active.unlock();
    }
    if (!d->makeTexture(QSize(1280, 720)))
        return fail(QStringLiteral("can't create a shared texture"));
    return true;
}

bool SpoutSender::send(const QImage& picture)
{
    if (!d->device || picture.isNull())
        return false;
    if (picture.size() != d->size && !d->makeTexture(picture.size()))
        return false;
    // Opaque BGRA, as receivers expect.
    const QImage bgra = picture.format() == QImage::Format_RGB32 ? picture : picture.convertToFormat(QImage::Format_RGB32);
    const DWORD r = d->access ? WaitForSingleObject(d->access, 67) : WAIT_OBJECT_0;
    if (r != WAIT_OBJECT_0 && r != WAIT_ABANDONED)
        return false; // a receiver is reading: skip this one
    d->context->UpdateSubresource(d->texture.Get(), 0, nullptr, bgra.constBits(), UINT(bgra.bytesPerLine()), 0);
    d->context->Flush();
    if (d->access)
        ReleaseMutex(d->access);
    // A new frame for receivers that count them.
    if (d->frameCount && WaitForSingleObject(d->frameCount, 0) == WAIT_OBJECT_0)
        ReleaseSemaphore(d->frameCount, 2, nullptr);
    return true;
}

void SpoutSender::close()
{
    d->unregister();
    d->info.close();
    d->names.close();
    if (d->access)
        CloseHandle(d->access);
    if (d->frameCount)
        CloseHandle(d->frameCount);
    d->access = d->frameCount = nullptr;
    d->texture.Reset();
    d->context.Reset();
    d->device.Reset();
    d->shareHandle = nullptr;
    d->size = {};
}

bool SpoutSender::isOpen() const
{
    return d->device != nullptr;
}

QString SpoutSender::name() const
{
    return QString::fromStdString(d->name);
}

QSize SpoutSender::size() const
{
    return d->size;
}

QStringList spout::senders()
{
    QStringList out;
    SharedMemory m;
    if (!m.open("SpoutSenderNames") || !m.lock())
        return out;
    for (const std::string& n : readNames(m))
        out << QString::fromStdString(n);
    m.unlock();
    return out;
}

QImage spout::receive(const QString& name, QString* error)
{
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return QImage();
    };
    SharedMemory info;
    if (!info.open(name.toStdString()) || info.size < sizeof(SharedTextureInfo))
        return fail(QStringLiteral("no sender called %1").arg(name));
    SharedTextureInfo ti{};
    if (!info.lock())
        return fail(QStringLiteral("busy"));
    std::memcpy(&ti, info.view, sizeof(ti));
    info.unlock();
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device> device = createDevice(&context);
    if (!device)
        return fail(QStringLiteral("no Direct3D 11"));
    ComPtr<ID3D11Texture2D> shared;
    if (FAILED(device->OpenSharedResource(reinterpret_cast<HANDLE>(uintptr_t(ti.shareHandle)), IID_PPV_ARGS(&shared))))
        return fail(QStringLiteral("can't open the shared texture"));
    D3D11_TEXTURE2D_DESC desc{};
    shared->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &staging)))
        return fail(QStringLiteral("can't make a staging texture"));
    HANDLE access = OpenMutexA(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name.toStdString() + "_SpoutAccessMutex").c_str());
    if (access)
        WaitForSingleObject(access, 67);
    context->CopyResource(staging.Get(), shared.Get());
    context->Flush();
    if (access) {
        ReleaseMutex(access);
        CloseHandle(access);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return fail(QStringLiteral("can't read the texture"));
    QImage img(int(desc.Width), int(desc.Height), QImage::Format_RGB32);
    for (int y = 0; y < img.height(); ++y)
        std::memcpy(img.scanLine(y), static_cast<const uchar*>(mapped.pData) + size_t(y) * mapped.RowPitch, size_t(img.width()) * 4);
    context->Unmap(staging.Get(), 0);
    if (ti.width != desc.Width || ti.height != desc.Height)
        return fail(QStringLiteral("the information doesn't match the texture"));
    return img;
}

// ---------------------------------------------------------------------------
// OBS Virtual Camera: the shared memory queue its DirectShow filter reads
// (obs-studio shared/obs-shared-memory-queue): a header, then three NV12
// frames each after a 32-byte header holding its timestamp.
// ---------------------------------------------------------------------------

namespace {

struct QueueHeader {
    volatile uint32_t writeIdx;
    volatile uint32_t readIdx;
    volatile uint32_t state;
    uint32_t offsets[3];
    uint32_t type;
    uint32_t cx;
    uint32_t cy;
    uint64_t interval; // 100 ns units
    uint32_t reserved[8];
};
enum QueueState : uint32_t { Invalid, Starting, Ready, Stopping };
constexpr wchar_t kQueueName[] = L"OBSVirtualCamVideo";
constexpr uint32_t kFrameHeader = 32;

uint32_t align32(uint32_t v)
{
    return (v + 31) & ~31u;
}

} // namespace

struct VirtualCamera::Impl {
    HANDLE map = nullptr;
    QueueHeader* header = nullptr;
    uint8_t* frames[3] = {};
    uint64_t* stamps[3] = {};
    QSize size;
    ff::SwsPtr sws;
    QSize swsFrom;
};

VirtualCamera::VirtualCamera() : d(std::make_unique<Impl>()) {}
VirtualCamera::~VirtualCamera()
{
    close();
}

bool VirtualCamera::installed()
{
    // The filter's class, as OBS registers it.
    HKEY key = nullptr;
    const bool ok = RegOpenKeyExW(HKEY_CLASSES_ROOT, L"CLSID\\{A3FCE0F5-3493-419F-958A-ABA1250EC20B}\\InprocServer32", 0, KEY_READ, &key)
                    == ERROR_SUCCESS;
    if (key)
        RegCloseKey(key);
    return ok;
}

bool VirtualCamera::open(QSize size, double fps, QString* error)
{
    close();
    auto fail = [&](const QString& why) {
        if (error)
            *error = why;
        return false;
    };
    const uint32_t cx = uint32_t(std::max(2, size.width() & ~1)), cy = uint32_t(std::max(2, size.height() & ~1));
    const uint32_t frameSize = cx * cy * 3 / 2;
    uint32_t total = align32(sizeof(QueueHeader));
    uint32_t offsets[3];
    for (uint32_t& o : offsets) {
        o = total;
        total = align32(total + frameSize + kFrameHeader);
    }
    // Someone else (OBS's own virtual camera, or a camera app still reading
    // the last session) holds the queue.
    if (HANDLE other = OpenFileMappingW(FILE_MAP_READ, FALSE, kQueueName)) {
        CloseHandle(other);
        return fail(QStringLiteral("the virtual camera is in use (OBS's own virtual camera is on?)"));
    }
    d->map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, total, kQueueName);
    if (!d->map)
        return fail(QStringLiteral("can't create the camera's memory"));
    d->header = static_cast<QueueHeader*>(MapViewOfFile(d->map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!d->header) {
        CloseHandle(d->map);
        d->map = nullptr;
        return fail(QStringLiteral("can't map the camera's memory"));
    }
    QueueHeader h{};
    h.state = Starting;
    h.cx = cx;
    h.cy = cy;
    h.interval = uint64_t(std::llround(1e7 / std::max(1.0, fps)));
    for (int i = 0; i < 3; ++i)
        h.offsets[i] = offsets[i];
    std::memcpy(d->header, &h, sizeof(h));
    for (int i = 0; i < 3; ++i) {
        auto* base = reinterpret_cast<uint8_t*>(d->header) + offsets[i];
        d->stamps[i] = reinterpret_cast<uint64_t*>(base);
        d->frames[i] = base + kFrameHeader;
    }
    d->size = QSize(int(cx), int(cy));
    return true;
}

bool VirtualCamera::send(const QImage& picture)
{
    if (!d->header || picture.isNull())
        return false;
    const QImage bgra = picture.format() == QImage::Format_RGB32 || picture.format() == QImage::Format_ARGB32_Premultiplied
                            ? picture
                            : picture.convertToFormat(QImage::Format_RGB32);
    if (!d->sws || d->swsFrom != bgra.size()) {
        d->sws.reset(sws_getContext(bgra.width(), bgra.height(), AV_PIX_FMT_BGRA, d->size.width(), d->size.height(), AV_PIX_FMT_NV12,
                                    SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!d->sws)
            return false;
        // Rec. 709, limited range: what OBS sends and cameras assume for HD.
        const int* coeffs = sws_getCoefficients(SWS_CS_ITU709);
        sws_setColorspaceDetails(d->sws.get(), coeffs, 1, coeffs, 0, 0, 1 << 16, 1 << 16);
        d->swsFrom = bgra.size();
    }
    QueueHeader* h = d->header;
    const uint32_t inc = h->writeIdx + 1;
    h->writeIdx = inc;
    const int idx = int(inc % 3);
    *d->stamps[idx] = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    uint8_t* dst[4] = {d->frames[idx], d->frames[idx] + size_t(d->size.width()) * d->size.height(), nullptr, nullptr};
    const int dstStride[4] = {d->size.width(), d->size.width(), 0, 0};
    const uint8_t* src[4] = {bgra.constBits(), nullptr, nullptr, nullptr};
    const int srcStride[4] = {int(bgra.bytesPerLine()), 0, 0, 0};
    sws_scale(d->sws.get(), src, srcStride, 0, bgra.height(), dst, dstStride);
    h->readIdx = inc;
    h->state = Ready;
    return true;
}

void VirtualCamera::close()
{
    if (d->header) {
        d->header->state = Stopping;
        UnmapViewOfFile(d->header);
    }
    if (d->map)
        CloseHandle(d->map);
    d->header = nullptr;
    d->map = nullptr;
    d->sws.reset();
    d->size = {};
}

bool VirtualCamera::isOpen() const
{
    return d->header != nullptr;
}

QSize VirtualCamera::size() const
{
    return d->size;
}

#else // ---- not Windows ----

struct SpoutSender::Impl {};
SpoutSender::SpoutSender() : d(std::make_unique<Impl>()) {}
SpoutSender::~SpoutSender() = default;
bool SpoutSender::available() { return false; }
bool SpoutSender::open(const QString&, QString* error)
{
    if (error)
        *error = QStringLiteral("Spout is Windows only");
    return false;
}
bool SpoutSender::send(const QImage&) { return false; }
void SpoutSender::close() {}
bool SpoutSender::isOpen() const { return false; }
QString SpoutSender::name() const { return {}; }
QSize SpoutSender::size() const { return {}; }
QStringList spout::senders() { return {}; }
QImage spout::receive(const QString&, QString* error)
{
    if (error)
        *error = QStringLiteral("Spout is Windows only");
    return {};
}

struct VirtualCamera::Impl {};
VirtualCamera::VirtualCamera() : d(std::make_unique<Impl>()) {}
VirtualCamera::~VirtualCamera() = default;
bool VirtualCamera::installed() { return false; }
bool VirtualCamera::open(QSize, double, QString* error)
{
    if (error)
        *error = QStringLiteral("the OBS virtual camera is Windows only here");
    return false;
}
bool VirtualCamera::send(const QImage&) { return false; }
void VirtualCamera::close() {}
bool VirtualCamera::isOpen() const { return false; }
QSize VirtualCamera::size() const { return {}; }

#endif
