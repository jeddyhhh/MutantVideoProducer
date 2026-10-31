#include "SystemStats.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <QHash>
#include <vector>
#endif

struct SystemStats::Impl {
#ifdef Q_OS_WIN
    ULONGLONG idle = 0, busy = 0; // the CPU times at the last read

    // NVML (nvml.dll comes with NVIDIA's driver, in System32). Its calls: 0 is success.
    struct NvUtil {
        unsigned gpu, memory;
    };
    struct NvMemory {
        unsigned long long total, free, used;
    };
    using Device = void*;
    HMODULE nvml = nullptr;
    Device device = nullptr;
    int (*shutdown)() = nullptr;
    int (*utilization)(Device, NvUtil*) = nullptr;
    int (*temperature)(Device, int, unsigned*) = nullptr;
    int (*memory)(Device, NvMemory*) = nullptr;
    int (*power)(Device, unsigned*) = nullptr;
    QString gpuName;

    // Any other card: the 3D engines' load, every process's added up, the busiest adapter.
    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER engines = nullptr;

    Impl()
    {
        // ponytail: the first NVIDIA card only; pick the one MVP renders on if anyone has two.
        nvml = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (nvml) {
            const auto fn = [this](const char* name) { return reinterpret_cast<void*>(GetProcAddress(nvml, name)); };
            const auto init = reinterpret_cast<int (*)()>(fn("nvmlInit_v2"));
            const auto byIndex = reinterpret_cast<int (*)(unsigned, Device*)>(fn("nvmlDeviceGetHandleByIndex_v2"));
            const auto name = reinterpret_cast<int (*)(Device, char*, unsigned)>(fn("nvmlDeviceGetName"));
            shutdown = reinterpret_cast<int (*)()>(fn("nvmlShutdown"));
            utilization = reinterpret_cast<int (*)(Device, NvUtil*)>(fn("nvmlDeviceGetUtilizationRates"));
            temperature = reinterpret_cast<int (*)(Device, int, unsigned*)>(fn("nvmlDeviceGetTemperature"));
            memory = reinterpret_cast<int (*)(Device, NvMemory*)>(fn("nvmlDeviceGetMemoryInfo"));
            power = reinterpret_cast<int (*)(Device, unsigned*)>(fn("nvmlDeviceGetPowerUsage"));
            if (init && byIndex && shutdown && init() == 0) {
                if (byIndex(0, &device) != 0)
                    device = nullptr;
                char text[96] = {};
                if (device && name && name(device, text, sizeof text) == 0)
                    gpuName = QString::fromUtf8(text);
            }
            if (!device) {
                FreeLibrary(nvml);
                nvml = nullptr;
            }
        }
        if (!device && PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS) {
            if (PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &engines) != ERROR_SUCCESS)
                engines = nullptr;
            PdhCollectQueryData(query); // (a rate: the first value comes at the next collect)
        }
    }
    ~Impl()
    {
        if (nvml) {
            shutdown();
            FreeLibrary(nvml);
        }
        if (query)
            PdhCloseQuery(query);
    }

    double engineLoad()
    {
        if (!engines || PdhCollectQueryData(query) != ERROR_SUCCESS)
            return -1;
        DWORD bytes = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(engines, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA)
            return -1;
        std::vector<BYTE> buffer(bytes);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
        if (PdhGetFormattedCounterArrayW(engines, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != ERROR_SUCCESS)
            return -1;
        QHash<QString, double> adapters; // "pid_1_luid_0x0_0xD8C7_phys_0_eng_0_engtype_3D": the adapter is luid..phys
        for (DWORD i = 0; i < count; ++i) {
            const QString instance = QString::fromWCharArray(items[i].szName);
            const int from = instance.indexOf(QLatin1String("luid_")), to = instance.indexOf(QLatin1String("_phys"));
            if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA && from >= 0 && to > from)
                adapters[instance.mid(from, to - from)] += items[i].FmtValue.doubleValue;
        }
        double busiest = adapters.isEmpty() ? -1 : 0;
        for (const double load : adapters)
            busiest = std::max(busiest, std::min(100.0, load));
        return busiest;
    }
#endif
};

SystemStats::SystemStats()
    : d(std::make_unique<Impl>())
{
}

SystemStats::~SystemStats() = default;

SystemStats::Reading SystemStats::read()
{
    Reading r;
#ifdef Q_OS_WIN
    constexpr double kGB = 1024.0 * 1024.0 * 1024.0;
    FILETIME idle, kernel, user;
    if (GetSystemTimes(&idle, &kernel, &user)) {
        const auto value = [](const FILETIME& t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        const ULONGLONG i = value(idle), busy = value(kernel) + value(user) - i; // (kernel time includes idle)
        if (d->idle || d->busy) {
            const double di = double(i - d->idle), db = double(busy - d->busy);
            if (di + db > 0)
                r.cpu = 100.0 * db / (di + db);
        }
        d->idle = i;
        d->busy = busy;
    }
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    if (GlobalMemoryStatusEx(&mem)) {
        r.ramTotal = double(mem.ullTotalPhys) / kGB;
        r.ramUsed = double(mem.ullTotalPhys - mem.ullAvailPhys) / kGB;
    }
    if (d->device) {
        r.gpuName = d->gpuName;
        Impl::NvUtil u{};
        if (d->utilization && d->utilization(d->device, &u) == 0)
            r.gpu = u.gpu;
        unsigned value = 0;
        if (d->temperature && d->temperature(d->device, 0 /* NVML_TEMPERATURE_GPU */, &value) == 0)
            r.gpuTemp = value;
        Impl::NvMemory m{};
        if (d->memory && d->memory(d->device, &m) == 0) {
            r.vramUsed = double(m.used) / kGB;
            r.vramTotal = double(m.total) / kGB;
        }
        if (d->power && d->power(d->device, &value) == 0)
            r.gpuWatts = value / 1000.0; // (milliwatts)
    } else {
        r.gpu = d->engineLoad();
    }
#endif
    return r;
}
