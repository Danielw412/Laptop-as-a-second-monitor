#pragma once
// What this process costs the machine: CPU split into kernel and user time, memory, GPU memory, handles, and the
// power state that explains a sudden change in any of them. Sampling is a handful of cheap Win32 calls, meant to
// run about once a second from the engine thread; nothing here touches the frame path.
#include "platform.hpp"
#include <nlohmann/json.hpp>
#include <optional>
namespace lm {
struct ResourceSample {
    // Percent of the whole machine's CPU capacity, over the interval since the previous sample.
    std::optional<double> cpuPercent, cpuKernelPercent, cpuUserPercent;
    uint64_t workingSetBytes = 0, peakWorkingSetBytes = 0, privateBytes = 0, pagefileBytes = 0;
    uint64_t pageFaults = 0;       // Since process start
    uint64_t pageFaultsDelta = 0;  // Over the interval: steady growth means the working set is being trimmed
    uint64_t readBytes = 0, writtenBytes = 0; // File I/O over the interval, logging included
    uint32_t handles = 0, gdiObjects = 0, userObjects = 0;
    // Video memory as the adapter reports it for this process. "Local" is dedicated VRAM, "shared" is system
    // memory the GPU borrows - on an integrated GPU almost everything lands in the shared segment.
    std::optional<uint64_t> gpuLocalUsedBytes, gpuLocalBudgetBytes, gpuSharedUsedBytes, gpuSharedBudgetBytes;
    bool onBattery = false, batterySaver = false;
    int batteryPercent = -1; // -1 when Windows does not know
};
class ResourceMeter {
  public:
    /// One sample covering the time since the previous call. adapter may be null (then no GPU memory figures).
    ResourceSample sample(IDXGIAdapter *adapter = nullptr);

  private:
    uint64_t kernel_ = 0, user_ = 0, faults_ = 0, read_ = 0, written_ = 0;
    bool primed_ = false;
    Clock::time_point time_ = Clock::now();
};
/// Adds the sample's fields to a per-second record, with the byte counts converted to whole megabytes.
void describe(nlohmann::json &record, const ResourceSample &);
/// Compact human form for the readable log: "cpu 7.4% (k 2.1) | ram 182 MB | gpu 412/3891 MB | AC".
std::string summarize(const ResourceSample &);
/// One-off description of the machine, logged at startup so every later measurement has a baseline.
std::string machineProfile();
} // namespace lm
