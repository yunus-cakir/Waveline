#pragma once
#include "common.h"

#include <pdh.h>

namespace wl {

// Resource usage of this process, sampled once per second:
// CPU, RAM, GPU memory, and GPU engine utilisation (NVENC = "VideoEncode", "3D").
// Also reports total system NVENC usage so other encoders (OBS, Sunshine...) are visible.
class Metrics {
public:
    struct Sample {
        double cpuPercent = 0;       // of all logical cores
        double nvencPercent = 0;     // this process
        double gpu3dPercent = 0;     // this process (video processor/copies)
        double nvencSystemPercent = 0;
        double workingSetMB = 0;
        double privateMB = 0;
        double gpuDedicatedMB = 0;
    };

    Metrics();
    ~Metrics();
    void Tick();
    const Sample& Last() const { return last_; }
    Sample Average() const;
    Sample Peak() const { return peak_; }

private:
    double SumCounter(PDH_HCOUNTER counter);

    PDH_HQUERY query_ = nullptr;
    PDH_HCOUNTER nvenc_ = nullptr, gpu3d_ = nullptr, nvencSystem_ = nullptr, gpuMem_ = nullptr;
    uint64_t lastCpu_ = 0;
    int64_t lastWall_ = 0;
    unsigned cores_ = 1;
    Sample last_, sum_, peak_;
    uint64_t samples_ = 0;
};

}  // namespace wl
