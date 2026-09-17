#include "resources.hpp"
#include <psapi.h>
#include <sstream>
namespace lm {
namespace {
uint64_t ticks(FILETIME t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
uint64_t megabytes(uint64_t bytes) {
    return (bytes + 512 * 1024) / (1024 * 1024);
}
} // namespace
ResourceSample ResourceMeter::sample(IDXGIAdapter *adapter) {
    ResourceSample s;
    const auto now = Clock::now();
    const auto seconds = std::chrono::duration<double>(now - time_).count();
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) {
        const uint64_t k = ticks(kernel), u = ticks(user);
        if (primed_ && seconds > 0) {
            const double capacity = 1e7 * seconds * GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
            s.cpuKernelPercent = (k - kernel_) / capacity * 100;
            s.cpuUserPercent = (u - user_) / capacity * 100;
            s.cpuPercent = *s.cpuKernelPercent + *s.cpuUserPercent;
        }
        kernel_ = k;
        user_ = u;
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof memory;
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                             sizeof memory)) {
        s.workingSetBytes = memory.WorkingSetSize;
        s.peakWorkingSetBytes = memory.PeakWorkingSetSize;
        s.privateBytes = memory.PrivateUsage;
        s.pagefileBytes = memory.PagefileUsage;
        s.pageFaults = memory.PageFaultCount;
        if (primed_ && memory.PageFaultCount >= faults_)
            s.pageFaultsDelta = memory.PageFaultCount - faults_;
        faults_ = memory.PageFaultCount;
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
        if (primed_) {
            s.readBytes = io.ReadTransferCount >= read_ ? io.ReadTransferCount - read_ : 0;
            s.writtenBytes = io.WriteTransferCount >= written_ ? io.WriteTransferCount - written_ : 0;
        }
        read_ = io.ReadTransferCount;
        written_ = io.WriteTransferCount;
    }
    DWORD handles = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &handles))
        s.handles = handles;
    // Only meaningful in the GUI process; a leak here is the classic cause of a slow, then unresponsive, window.
    s.gdiObjects = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    s.userObjects = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (adapter) {
        ComPtr<IDXGIAdapter3> adapter3;
        if (SUCCEEDED(adapter->QueryInterface(IID_PPV_ARGS(&adapter3)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                s.gpuLocalUsedBytes = info.CurrentUsage;
                s.gpuLocalBudgetBytes = info.Budget;
            }
            if (SUCCEEDED(adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info))) {
                s.gpuSharedUsedBytes = info.CurrentUsage;
                s.gpuSharedBudgetBytes = info.Budget;
            }
        }
    }
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        s.onBattery = power.ACLineStatus == 0;
        s.batterySaver = power.SystemStatusFlag != 0;
        s.batteryPercent = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
    }
    primed_ = true;
    time_ = now;
    return s;
}
void describe(nlohmann::json &record, const ResourceSample &s) {
    auto optional = [](const std::optional<double> &v) {
        return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
    };
    record["cpu_percent"] = optional(s.cpuPercent);
    record["cpu_kernel_percent"] = optional(s.cpuKernelPercent);
    record["cpu_user_percent"] = optional(s.cpuUserPercent);
    record["working_set_mb"] = megabytes(s.workingSetBytes);
    record["peak_working_set_mb"] = megabytes(s.peakWorkingSetBytes);
    record["private_mb"] = megabytes(s.privateBytes);
    record["page_faults_delta"] = s.pageFaultsDelta;
    record["io_read_kb"] = s.readBytes / 1024;
    record["io_write_kb"] = s.writtenBytes / 1024;
    record["handles"] = s.handles;
    record["gdi_objects"] = s.gdiObjects;
    record["user_objects"] = s.userObjects;
    if (s.gpuLocalUsedBytes) {
        record["gpu_local_used_mb"] = megabytes(*s.gpuLocalUsedBytes);
        record["gpu_local_budget_mb"] = megabytes(s.gpuLocalBudgetBytes.value_or(0));
    }
    if (s.gpuSharedUsedBytes) {
        record["gpu_shared_used_mb"] = megabytes(*s.gpuSharedUsedBytes);
        record["gpu_shared_budget_mb"] = megabytes(s.gpuSharedBudgetBytes.value_or(0));
    }
    record["on_battery"] = s.onBattery;
    record["battery_saver"] = s.batterySaver;
    if (s.batteryPercent >= 0)
        record["battery_percent"] = s.batteryPercent;
}
std::string summarize(const ResourceSample &s) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(1);
    if (s.cpuPercent)
        out << "cpu " << *s.cpuPercent << "% (kernel " << s.cpuKernelPercent.value_or(0) << "%)";
    else
        out << "cpu -";
    out << " | ram " << megabytes(s.workingSetBytes) << " MB (private " << megabytes(s.privateBytes) << " MB)";
    if (s.gpuLocalUsedBytes || s.gpuSharedUsedBytes)
        out << " | gpu mem " << megabytes(s.gpuLocalUsedBytes.value_or(0) + s.gpuSharedUsedBytes.value_or(0))
            << " MB";
    out << " | handles " << s.handles;
    out << " | " << (s.onBattery ? "battery" : "AC");
    if (s.onBattery && s.batteryPercent >= 0)
        out << ' ' << s.batteryPercent << '%';
    if (s.batterySaver)
        out << " | battery saver on";
    return out.str();
}
std::string machineProfile() {
    std::ostringstream out;
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    GlobalMemoryStatusEx(&memory);
    out << "Machine: " << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) << " logical processors";
    if (memory.ullTotalPhys)
        out << " | " << megabytes(memory.ullTotalPhys) / 1024 << " GB RAM (" << memory.dwMemoryLoad << "% in use)";
    // The registry is the only place that still reports the real build after Windows' version lies.
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        wchar_t text[128]{};
        DWORD size = sizeof text, type = 0;
        if (RegQueryValueExW(key, L"DisplayVersion", nullptr, &type, reinterpret_cast<BYTE *>(text), &size) ==
            ERROR_SUCCESS)
            out << " | Windows " << utf8(text);
        size = sizeof text;
        if (RegQueryValueExW(key, L"CurrentBuildNumber", nullptr, &type, reinterpret_cast<BYTE *>(text), &size) ==
            ERROR_SUCCESS)
            out << " build " << utf8(text);
        RegCloseKey(key);
    }
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        out << " | " << (power.ACLineStatus == 0 ? "on battery" : "on AC");
        if (power.SystemStatusFlag)
            out << " | battery saver on";
    }
    return out.str();
}
} // namespace lm
