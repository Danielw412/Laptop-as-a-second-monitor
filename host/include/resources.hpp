#pragma once
// What this process costs the machine: CPU split into kernel and user time, memory, GPU memory, GPU engine time,
// handles, and the power state that explains a sudden change in any of them. Sampling is a handful of cheap Win32
// and D3DKMT calls, meant to run about once a second from the engine thread; nothing here touches the frame path.
#include "platform.hpp"
#include <array>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
namespace lm {
/// GPU engine classes, from the engine type each engine (WDDM "node") declares, folded into what a reader needs.
enum class GpuEngine { ThreeD, Copy, VideoDecode, VideoEncode, VideoProcessing, VideoCodec, Other, Count };
/// "3d", "copy", "video_decode", "video_encode", "video_processing", "video_codec", "other".
const char *gpuEngineName(GpuEngine);
/// How busy one engine class was over the interval, in percent of wall time. A class can have several engines;
/// each value is its busiest engine (as Task Manager graphs them), so it reads 100 when that engine is saturated,
/// and several classes can approach 100 at once because the engines run in parallel.
struct GpuEngineUsage {
    unsigned nodes = 0;            // Engines of this class on the adapter; 0: the class does not exist here
    std::optional<double> process; // This process's work, on the class's busiest engine for this process
    std::optional<double> system;  // Every process's work, on the class's busiest engine overall
};
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
    // GPU engine time on the adapter passed to sample(). Every class has nodes == 0 when that is unavailable.
    std::array<GpuEngineUsage, size_t(GpuEngine::Count)> gpuEngines{};
    bool onBattery = false, batterySaver = false;
    int batteryPercent = -1; // -1 when Windows does not know
    double sampleMs = 0;     // What taking this sample cost the calling thread
};
class ResourceMeter {
  public:
    ResourceMeter();
    ~ResourceMeter();
    /// One sample covering the time since the previous call. adapter may be null (then no GPU figures).
    ResourceSample sample(IDXGIAdapter *adapter = nullptr);

  private:
    struct GpuEngines; // D3DKMT state: discovered once per adapter, then two queries per engine per sample
    std::unique_ptr<GpuEngines> gpu_;
    uint64_t kernel_ = 0, user_ = 0, faults_ = 0, read_ = 0, written_ = 0;
    bool primed_ = false;
    Clock::time_point time_ = Clock::now();
};
/// Adds the sample's fields to a per-second record, with the byte counts converted to whole megabytes.
void describe(nlohmann::json &record, const ResourceSample &);
/// Compact human form for the readable log: "cpu 7.4% (k 2.1) | ram 182 MB | gpu 412/3891 MB | AC".
std::string summarize(const ResourceSample &);
/// GPU engines for the readable log, "this process / all processes": "3d 12/30% | video decode 25/25%". Empty
/// when engine time is unavailable.
std::string summarizeGpuEngines(const ResourceSample &);
/// One-off description of the machine, logged at startup so every later measurement has a baseline.
std::string machineProfile();
/// The same, structured, plus every GPU adapter with its driver version: session.json's "machine".
nlohmann::json machineProfileJson();
} // namespace lm
