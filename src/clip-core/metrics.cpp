#include "metrics.h"

#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <vector>

namespace wl {

namespace {
uint64_t ProcessCpu100ns() {
    FILETIME create, exit, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user);
    auto toU64 = [](const FILETIME& f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return toU64(kernel) + toU64(user);
}

PDH_HCOUNTER AddCounter(PDH_HQUERY query, const std::wstring& path) {
    PDH_HCOUNTER c = nullptr;
    if (PdhAddEnglishCounterW(query, path.c_str(), 0, &c) != ERROR_SUCCESS) return nullptr;
    return c;
}
}  // namespace

Metrics::Metrics() {
    cores_ = std::max<unsigned>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    lastCpu_ = ProcessCpu100ns();
    lastWall_ = QpcNow100ns();

    if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
        query_ = nullptr;
        return;
    }
    const std::wstring pid = std::to_wstring(GetCurrentProcessId());
    nvenc_ = AddCounter(query_, L"\\GPU Engine(pid_" + pid + L"_*engtype_VideoEncode)\\Utilization Percentage");
    gpu3d_ = AddCounter(query_, L"\\GPU Engine(pid_" + pid + L"_*engtype_3D)\\Utilization Percentage");
    nvencSystem_ = AddCounter(query_, L"\\GPU Engine(*engtype_VideoEncode)\\Utilization Percentage");
    gpuMem_ = AddCounter(query_, L"\\GPU Process Memory(pid_" + pid + L"_*)\\Dedicated Usage");
    PdhCollectQueryData(query_);
}

Metrics::~Metrics() {
    if (query_) PdhCloseQuery(query_);
}

double Metrics::SumCounter(PDH_HCOUNTER counter) {
    if (!counter) return 0;
    DWORD size = 0, count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, nullptr);
    if (s != PDH_MORE_DATA || size == 0) return 0;
    std::vector<BYTE> buffer(size);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    s = PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &size, &count, items);
    if (s != ERROR_SUCCESS) return 0;
    double sum = 0;
    for (DWORD i = 0; i < count; ++i)
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            sum += items[i].FmtValue.doubleValue;
    return sum;
}

void Metrics::Tick() {
    Sample s;
    const uint64_t cpu = ProcessCpu100ns();
    const int64_t wall = QpcNow100ns();
    if (wall > lastWall_)
        s.cpuPercent = 100.0 * static_cast<double>(cpu - lastCpu_) / (static_cast<double>(wall - lastWall_) * cores_);
    lastCpu_ = cpu;
    lastWall_ = wall;

    PROCESS_MEMORY_COUNTERS_EX mem{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
        s.workingSetMB = mem.WorkingSetSize / (1024.0 * 1024.0);
        s.privateMB = mem.PrivateUsage / (1024.0 * 1024.0);
    }

    if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
        s.nvencPercent = SumCounter(nvenc_);
        s.gpu3dPercent = SumCounter(gpu3d_);
        s.nvencSystemPercent = SumCounter(nvencSystem_);
        s.gpuDedicatedMB = SumCounter(gpuMem_) / (1024.0 * 1024.0);
    }

    last_ = s;
    sum_.cpuPercent += s.cpuPercent;
    sum_.nvencPercent += s.nvencPercent;
    sum_.gpu3dPercent += s.gpu3dPercent;
    sum_.nvencSystemPercent += s.nvencSystemPercent;
    sum_.workingSetMB += s.workingSetMB;
    sum_.privateMB += s.privateMB;
    sum_.gpuDedicatedMB += s.gpuDedicatedMB;
    peak_.cpuPercent = std::max(peak_.cpuPercent, s.cpuPercent);
    peak_.nvencPercent = std::max(peak_.nvencPercent, s.nvencPercent);
    peak_.gpu3dPercent = std::max(peak_.gpu3dPercent, s.gpu3dPercent);
    peak_.nvencSystemPercent = std::max(peak_.nvencSystemPercent, s.nvencSystemPercent);
    peak_.workingSetMB = std::max(peak_.workingSetMB, s.workingSetMB);
    peak_.privateMB = std::max(peak_.privateMB, s.privateMB);
    peak_.gpuDedicatedMB = std::max(peak_.gpuDedicatedMB, s.gpuDedicatedMB);
    ++samples_;
}

Metrics::Sample Metrics::Average() const {
    Sample a;
    if (!samples_) return a;
    const double n = static_cast<double>(samples_);
    a.cpuPercent = sum_.cpuPercent / n;
    a.nvencPercent = sum_.nvencPercent / n;
    a.gpu3dPercent = sum_.gpu3dPercent / n;
    a.nvencSystemPercent = sum_.nvencSystemPercent / n;
    a.workingSetMB = sum_.workingSetMB / n;
    a.privateMB = sum_.privateMB / n;
    a.gpuDedicatedMB = sum_.gpuDedicatedMB / n;
    return a;
}

}  // namespace wl
