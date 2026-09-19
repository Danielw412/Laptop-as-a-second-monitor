#include "resources.hpp"
#include "logging.hpp"
#include <psapi.h>
#include <sstream>
#include <winternl.h> // NTSTATUS, which the D3DKMT header uses
#include <d3dkmthk.h>
namespace lm {
namespace {
uint64_t ticks(FILETIME t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
uint64_t megabytes(uint64_t bytes) {
    return (bytes + 512 * 1024) / (1024 * 1024);
}
// DXGK_ENGINE_TYPE values. Spelled out because older SDK headers lack the newest one (video codec, 9).
GpuEngine classify(int engineType) {
    switch (engineType) {
    case 1:
        return GpuEngine::ThreeD;
    case 2:
        return GpuEngine::VideoDecode;
    case 3:
        return GpuEngine::VideoEncode;
    case 4:
        return GpuEngine::VideoProcessing;
    case 6:
        return GpuEngine::Copy;
    case 9:
        return GpuEngine::VideoCodec;
    default:
        return GpuEngine::Other; // 0 other (often compute), 5 scene assembly, 7 overlay, 8 crypto
    }
}
std::string registryText(HKEY key, const wchar_t *name) {
    wchar_t text[128]{};
    DWORD size = sizeof text - sizeof(wchar_t), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE *>(text), &size) != ERROR_SUCCESS ||
        type != REG_SZ)
        return {};
    return utf8(text);
}
} // namespace
const char *gpuEngineName(GpuEngine e) {
    static const char *const names[] = {"3d",       "copy",        "video_decode", "video_encode",
                                        "video_processing", "video_codec", "other"};
    return names[size_t(e)];
}
// Per-engine running time from the kernel graphics statistics (D3DKMTQueryStatistics), the same source System
// Informer uses for its per-process GPU columns. Discovery - how many engines, and what each one is - happens once
// per adapter; after that a sample is two small kernel queries per engine, so it is cheap enough for the engine
// thread's once-a-second block. The entry points are looked up at run time: if any is missing, or discovery
// fails, the fields are simply not reported.
struct ResourceMeter::GpuEngines {
    using QueryStatistics = NTSTATUS(APIENTRY *)(const D3DKMT_QUERYSTATISTICS *);
    using OpenAdapterFromLuid = NTSTATUS(APIENTRY *)(D3DKMT_OPENADAPTERFROMLUID *);
    using QueryAdapterInfo = NTSTATUS(APIENTRY *)(const D3DKMT_QUERYADAPTERINFO *);
    using CloseAdapter = NTSTATUS(APIENTRY *)(const D3DKMT_CLOSEADAPTER *);
    QueryStatistics query = nullptr;
    OpenAdapterFromLuid openAdapter = nullptr;
    QueryAdapterInfo adapterInfo = nullptr;
    CloseAdapter closeAdapter = nullptr;
    // The per-process queries need full query rights (a PROCESS_QUERY_LIMITED_INFORMATION handle is refused with
    // STATUS_INVALID_PARAMETER); the pseudo-handle has them for this process and needs no closing.
    HANDLE process = GetCurrentProcess();
    struct Node {
        GpuEngine engine = GpuEngine::Other;
        int64_t process = -1, system = -1; // Previous running time (100 ns), -1 when not read
    };
    std::vector<Node> nodes;
    LUID luid{};
    bool ready = false, failed = false, primed = false;
    Clock::time_point last{};
    GpuEngines() {
        if (HMODULE gdi = GetModuleHandleW(L"gdi32.dll")) {
            query = reinterpret_cast<QueryStatistics>(GetProcAddress(gdi, "D3DKMTQueryStatistics"));
            openAdapter = reinterpret_cast<OpenAdapterFromLuid>(GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid"));
            adapterInfo = reinterpret_cast<QueryAdapterInfo>(GetProcAddress(gdi, "D3DKMTQueryAdapterInfo"));
            closeAdapter = reinterpret_cast<CloseAdapter>(GetProcAddress(gdi, "D3DKMTCloseAdapter"));
        }
    }
    bool prepare(IDXGIAdapter *adapter) {
        DXGI_ADAPTER_DESC desc{};
        if (FAILED(adapter->GetDesc(&desc)))
            return false;
        if (memcmp(&desc.AdapterLuid, &luid, sizeof luid) == 0 && (ready || failed))
            return ready; // Known adapter: never rediscover, and never retry a failure every second
        luid = desc.AdapterLuid;
        nodes.clear();
        ready = failed = primed = false;
        const std::string gpu = utf8(desc.Description);
        auto fail = [&](const std::string &why) {
            failed = true;
            logInfo("GPU engine utilization unavailable on " + gpu + ": " + why);
            return false;
        };
        if (!query || !openAdapter || !adapterInfo || !closeAdapter)
            return fail("the kernel graphics statistics interface is missing");
        D3DKMT_QUERYSTATISTICS statistics{};
        statistics.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
        statistics.AdapterLuid = luid;
        if (query(&statistics) < 0 || !statistics.QueryResult.AdapterInformation.NodeCount)
            return fail("the adapter reports no engines");
        const ULONG count = std::min<ULONG>(statistics.QueryResult.AdapterInformation.NodeCount, 64);
        D3DKMT_OPENADAPTERFROMLUID open{};
        open.AdapterLuid = luid;
        if (openAdapter(&open) < 0)
            return fail("the adapter could not be opened for queries");
        std::string map;
        bool typed = false;
        for (ULONG i = 0; i < count; ++i) {
            D3DKMT_NODEMETADATA metadata{};
            metadata.NodeOrdinalAndAdapterIndex = i; // Low word: engine ordinal; high word: physical adapter 0
            D3DKMT_QUERYADAPTERINFO info{};
            info.hAdapter = open.hAdapter;
            info.Type = KMTQAITYPE_NODEMETADATA;
            info.pPrivateDriverData = &metadata;
            info.PrivateDriverDataSize = sizeof metadata;
            Node node;
            std::string name;
            if (adapterInfo(&info) >= 0) {
                typed = true;
                node.engine = classify(int(metadata.NodeData.EngineType));
                const auto &friendly = metadata.NodeData.FriendlyName;
                std::wstring wide(friendly, wcsnlen(friendly, std::size(friendly)));
                name = printable(utf8(wide.c_str()), 32);
            }
            nodes.push_back(node);
            map += (i ? ", " : "") + std::to_string(i) + " " + gpuEngineName(node.engine) +
                   (name.empty() ? "" : " (" + name + ")");
        }
        D3DKMT_CLOSEADAPTER close{};
        close.hAdapter = open.hAdapter;
        closeAdapter(&close);
        if (!typed) {
            nodes.clear();
            return fail("the driver does not say what its engines are");
        }
        // Which engine is which is the key to reading every gpu_engine_* field, and it differs per vendor.
        logInfo("GPU engines on " + gpu + ": " + map);
        ready = true;
        return true;
    }
    void sample(IDXGIAdapter *adapter, ResourceSample &s, Clock::time_point now) {
        if (!adapter || !prepare(adapter))
            return;
        const double seconds = primed ? std::chrono::duration<double>(now - last).count() : 0;
        std::array<bool, size_t(GpuEngine::Count)> complete{};
        complete.fill(primed);
        auto running = [&](D3DKMT_QUERYSTATISTICS_TYPE type, ULONG node) -> int64_t {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = type;
            q.AdapterLuid = luid;
            if (type == D3DKMT_QUERYSTATISTICS_PROCESS_NODE) {
                q.hProcess = process;
                q.QueryProcessNode.NodeId = node;
            } else
                q.QueryNode.NodeId = node;
            if (query(&q) < 0)
                return -1;
            const auto &result = type == D3DKMT_QUERYSTATISTICS_PROCESS_NODE
                                     ? q.QueryResult.ProcessNodeInformation
                                     : q.QueryResult.NodeInformation.GlobalInformation;
            return result.RunningTime.QuadPart;
        };
        for (ULONG i = 0; i < nodes.size(); ++i) {
            auto &node = nodes[i];
            const auto slot = size_t(node.engine);
            auto &usage = s.gpuEngines[slot];
            ++usage.nodes;
            const int64_t mine = running(D3DKMT_QUERYSTATISTICS_PROCESS_NODE, i);
            const int64_t all = running(D3DKMT_QUERYSTATISTICS_NODE, i);
            // A class is reported only when every one of its engines gave a reading: a maximum over some of them
            // could hide the busy one.
            const auto mineBusy = node.process >= 0 && mine >= 0 ? busyPercent(node.process, mine, seconds)
                                                                   : std::nullopt;
            const auto allBusy = node.system >= 0 && all >= 0 ? busyPercent(node.system, all, seconds)
                                                              : std::nullopt;
            if (!mineBusy || !allBusy)
                complete[slot] = false;
            else {
                usage.process = std::max(usage.process.value_or(0), *mineBusy);
                usage.system = std::max(usage.system.value_or(0), *allBusy);
            }
            node.process = mine;
            node.system = all;
        }
        for (size_t c = 0; c < complete.size(); ++c)
            if (!complete[c]) {
                s.gpuEngines[c].process.reset();
                s.gpuEngines[c].system.reset();
            }
        primed = true;
        last = now;
    }
};
ResourceMeter::ResourceMeter() : gpu_(std::make_unique<GpuEngines>()) {}
ResourceMeter::~ResourceMeter() = default;
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
        gpu_->sample(adapter, s, now);
    }
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        s.onBattery = power.ACLineStatus == 0;
        s.batterySaver = power.SystemStatusFlag != 0;
        s.batteryPercent = power.BatteryLifePercent == 255 ? -1 : power.BatteryLifePercent;
    }
    primed_ = true;
    time_ = now;
    s.sampleMs = std::chrono::duration<double, std::milli>(Clock::now() - now).count();
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
    // Absent for a class the adapter does not have (or when engine time is unavailable altogether); null for the
    // first sample and whenever a reading failed.
    for (size_t c = 0; c < s.gpuEngines.size(); ++c) {
        const auto &usage = s.gpuEngines[c];
        if (!usage.nodes)
            continue;
        const std::string name = gpuEngineName(GpuEngine(c));
        record["gpu_engine_" + name + "_percent"] = optional(usage.process);
        record["gpu_engine_" + name + "_system_percent"] = optional(usage.system);
    }
    record["resource_sample_ms"] = s.sampleMs;
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
std::string summarizeGpuEngines(const ResourceSample &s) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(0);
    for (size_t c = 0; c < s.gpuEngines.size(); ++c) {
        const auto &usage = s.gpuEngines[c];
        if (!usage.nodes || !usage.process || !usage.system)
            continue;
        std::string name = gpuEngineName(GpuEngine(c));
        std::replace(name.begin(), name.end(), '_', ' ');
        if (out.tellp() > 0)
            out << " | ";
        out << name << ' ' << *usage.process << '/' << *usage.system << '%';
    }
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
nlohmann::json machineProfileJson() {
    nlohmann::json j;
    j["logical_processors"] = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof memory;
    if (GlobalMemoryStatusEx(&memory)) {
        j["ram_mb"] = megabytes(memory.ullTotalPhys);
        j["memory_load_percent"] = memory.dwMemoryLoad;
    }
    HKEY key{};
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows NT\CurrentVersion)", 0, KEY_READ, &key) ==
        ERROR_SUCCESS) {
        j["windows_version"] = registryText(key, L"DisplayVersion");
        j["windows_build"] = registryText(key, L"CurrentBuildNumber");
        RegCloseKey(key);
    }
    SYSTEM_POWER_STATUS power{};
    if (GetSystemPowerStatus(&power)) {
        j["on_battery"] = power.ACLineStatus == 0;
        j["battery_saver"] = power.SystemStatusFlag != 0;
        if (power.BatteryLifePercent != 255)
            j["battery_percent"] = power.BatteryLifePercent;
    }
    // Encode numbers depend on the GPU and, as much, on its driver: the same Iris Xe moves with a driver update.
    auto gpus = nlohmann::json::array();
    ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                continue;
            nlohmann::json gpu = {{"name", utf8(desc.Description)},
                                  {"vendor_id", desc.VendorId},
                                  {"device_id", desc.DeviceId},
                                  {"dedicated_video_mb", megabytes(desc.DedicatedVideoMemory)},
                                  {"shared_system_mb", megabytes(desc.SharedSystemMemory)}};
            LARGE_INTEGER version{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version)))
                gpu["driver_version"] = std::to_string(HIWORD(version.HighPart)) + "." +
                                        std::to_string(LOWORD(version.HighPart)) + "." +
                                        std::to_string(HIWORD(version.LowPart)) + "." +
                                        std::to_string(LOWORD(version.LowPart));
            gpus.push_back(std::move(gpu));
        }
    }
    j["gpus"] = std::move(gpus);
    return j;
}
} // namespace lm
