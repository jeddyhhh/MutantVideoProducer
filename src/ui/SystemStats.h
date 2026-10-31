#pragma once

#include <QString>
#include <memory>

// The computer's load and temperatures for the status bar. Windows: CPU from GetSystemTimes, memory from
// GlobalMemoryStatusEx, an NVIDIA card through its driver's NVML (load, temperature, memory, power), another card's
// load from Windows' GPU Engine counters. Elsewhere nothing is read. Not thread-safe: one reader, the status bar's timer.
class SystemStats {
public:
    struct Reading {
        double cpu = -1;                     // percent; -1: not known (the first reading)
        double ramUsed = 0, ramTotal = 0;    // GB; 0: not known
        double gpu = -1, gpuTemp = -1;       // percent, °C; -1: not known
        double vramUsed = 0, vramTotal = 0;  // GB
        double gpuWatts = -1;
        QString gpuName;
    };
    SystemStats();
    ~SystemStats();
    Reading read(); // the load since the last read

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
