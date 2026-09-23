#include "pipeline.hpp"
#include "frame_diagnostics.hpp"
#include "h264.hpp"
#include "logging.hpp"
#include "pattern.hpp"
#include "resources.hpp"
#include "session_archive.hpp"
#include <avrt.h>
#include <d3d11_1.h>
#include <deque>
#include <fstream>
#include <iomanip>
#include <map>
#include <thread>
#include <tuple>
#include <winrt/base.h>
namespace lm {
namespace {
nlohmann::json optionalNumber(const std::optional<double> &v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}
/// Fixed-precision number for the readable log, where full double precision is only noise.
std::string fixed(double value, int decimals = 1) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(decimals) << value;
    return out.str();
}
std::string fixed(const std::optional<double> &value, int decimals = 1) {
    return value ? fixed(*value, decimals) : std::string("-");
}
double milliseconds(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}
// The frame path on the host's steady clock only, one per pipeline session. A frame's stages are sampled when it
// comes out of the encoder and when it is handed to the transport, from the FrameTrace that travelled with it, so
// every stage is measured on the same frames. The compositor's stamps never enter any of these. Heap-allocated:
// the rings are about 200 KB and the engine thread's stack already carries the historical ones.
struct HostTiming {
    LatencyTrack acquireToConvert, acquireToEncodeSubmit, acquireToEncoded, acquireToSend, encodedToSend,
        outputPickup;
    SourceActivity source;
    uint64_t repeats = 0;  // Interval: re-encodes of the last surface (still desktop keep-alive, keyframe answers)
    uint64_t untraced = 0; // Interval: encoder outputs whose submission could not be identified
    void endInterval() {
        for (auto *track : {&acquireToConvert, &acquireToEncodeSubmit, &acquireToEncoded, &acquireToSend,
                            &encodedToSend, &outputPickup})
            track->endInterval();
        repeats = untraced = 0;
    }
};
/// What the encoded stream itself says about each frame, over one reporting interval. The quantizer is pixelation
// measured at the source: a frame encoded at QP 45 is blocky on every receiver, however clean the network. The
// encoder's own per-frame QP is used when it reports one (Intel does, and keeps the slice QP fixed at 26 while
// varying QP per macroblock); otherwise the slice header's QP.
struct BitstreamStats {
    Samples<512> qp;                 // QP of each frame this interval
    double qpMax = 0;                // Highest frame QP this interval
    uint64_t coarse = 0, severe = 0; // Frames at QP >= 40 (visibly soft) and >= 46 (flat blocks)
    uint64_t idr = 0, unparsed = 0;  // IDR frames; frames with a slice header the parser could not read
    unsigned slicesMax = 0;
    uint64_t deltaBytesMax = 0;      // Largest non-IDR frame this interval
    static std::optional<double> frameQp(const h264::AccessUnit &au, const std::optional<int> &reported) {
        if (reported)
            return double(*reported);
        if (au.qpMin)
            return au.qpMean;
        return std::nullopt;
    }
    void add(const h264::AccessUnit &au, size_t bytes, const std::optional<int> &reported) {
        if (const auto q = frameQp(au, reported)) {
            qp.add(*q);
            qpMax = std::max(qpMax, *q);
            if (*q >= 40)
                ++coarse;
            if (*q >= 46)
                ++severe;
        }
        if (au.idr)
            ++idr;
        else
            deltaBytesMax = std::max<uint64_t>(deltaBytesMax, bytes);
        if (au.unparsedSlices)
            ++unparsed;
        slicesMax = std::max(slicesMax, au.slices);
    }
    void describe(nlohmann::json &record) const {
        if (qp.count()) {
            record["qp_mean"] = qp.mean();
            record["qp_p95"] = qp.percentile(.95);
            record["qp_max"] = qpMax;
        } else
            record["qp_mean"] = record["qp_p95"] = record["qp_max"] = nullptr;
        record["qp_coarse_frames"] = coarse;
        record["qp_severe_frames"] = severe;
        record["idr_frames"] = idr;
        record["unparsed_frames"] = unparsed;
        record["slices_per_frame_max"] = slicesMax;
        record["delta_frame_bytes_max"] = deltaBytesMax;
    }
    void endInterval() {
        qp.clear();
        qpMax = 0;
        coarse = severe = idr = unparsed = deltaBytesMax = 0;
        slicesMax = 0;
    }
};
// name_mean/_p95/_p99 over the rolling ring and name_max over this interval; null where nothing was measured.
void describeLatency(nlohmann::json &record, const std::string &name, const LatencyTrack &track) {
    if (track.rolling.count()) {
        const auto [p95, p99] = track.rolling.percentiles(.95, .99);
        record[name + "_mean"] = track.rolling.mean();
        record[name + "_p95"] = p95;
        record[name + "_p99"] = p99;
    } else
        record[name + "_mean"] = record[name + "_p95"] = record[name + "_p99"] = nullptr;
    record[name + "_max"] = track.intervalCount ? nlohmann::json(track.intervalMax) : nlohmann::json(nullptr);
}
/// What the source was doing in the last second, so a quality line can say whether the desktop was simply still.
std::string sourceContext(const MetricsSnapshot &s) {
    std::string text = std::to_string(s.sourceFramesInterval) + " new source frames in the last second";
    text += s.msSinceSourceFrame ? ", newest " + fixed(*s.msSinceSourceFrame / 1000, 1) + " s ago"
                                 : ", none yet this pipeline session";
    text += ", " + std::to_string(s.repeatFramesInterval) + " repeat encodes";
    if (s.userInputIdleMs)
        text += ", user input " + fixed(*s.userInputIdleMs / 1000, 1) + " s ago";
    return text;
}
class PollTimer {
    HANDLE timer_ =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  public:
    PollTimer() {
        if (!timer_)
            throw std::runtime_error("High resolution waitable timer unavailable");
    }
    ~PollTimer() {
        CloseHandle(timer_);
    }
    void wait(unsigned ms = 1) {
        LARGE_INTEGER due;
        due.QuadPart = -10000LL * ms;
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer_, 100);
    }
};
// Why the engine thread woke up. Every wake-up costs a context switch, so knowing which ones did no useful work
// is the first thing to look at when the process burns CPU while the desktop is still.
enum class Wake { Frame, Encoder, Transport, Timeout };
// Counts wake-ups by reason and how much of the wall clock the thread spent awake.
struct LoopAccounting {
    uint64_t frame = 0, encoder = 0, transport = 0, timeout = 0, idle = 0;
    double busyMs = 0;
    void woke(Wake reason) {
        switch (reason) {
        case Wake::Frame:
            ++frame;
            break;
        case Wake::Encoder:
            ++encoder;
            break;
        case Wake::Transport:
            ++transport;
            break;
        case Wake::Timeout:
            ++timeout;
            break;
        }
    }
    LoopAccounting operator-(const LoopAccounting &o) const {
        return {frame - o.frame,       encoder - o.encoder, transport - o.transport,
                timeout - o.timeout,   idle - o.idle,       busyMs - o.busyMs};
    }
};
// Watches for the episodes users describe as "it goes pixelated and glitchy, then fixes itself" and writes one line
// when one starts and one when it ends, naming the cause from what happened in between. The candidates, each with
// its own evidence:
//
//   quantization - the encoder ran out of bits: frame QP 44 and above, where changed regions turn into flat 16x16
//                  blocks and the rate control leaves some macroblocks stale. Measured from the encoder's own
//                  per-frame QP, so it shows even on a perfect network; a starved bitrate target is the usual reason.
//   loss         - packets were lost on the way: RTCP loss, NACKs, PLI/FIR from the receiver.
//   receiver     - a quality probe found the receiver showing content that differs from what was encoded.
//   stall        - the receiver froze while the desktop was actually changing.
//
// The receiver's "frames received minus decoded" is not a trigger: it is one frame in flight at the moment of
// sampling far more often than it is a lost frame. Nothing here changes behaviour; it only reports.
class QualityWatch {
    static constexpr double kBlockyQp = 44;
    static constexpr int kRecoverySeconds = 3; // Consecutive clean seconds before an episode is over
    bool inEpisode_ = false;
    int clean_ = 0;
    Clock::time_point started_{};
    double worstQp_ = 0, lowestBitrate_ = 0;
    uint64_t lostPackets_ = 0, keyframeRequests_ = 0, freezes_ = 0, badProbes_ = 0;
    uint32_t rebuildsAtStart_ = 0;
    unsigned seconds_ = 0;
    std::string trigger_;

  public:
    /// Called once a second with the published snapshot, the network evidence of that second, the session's
    /// rebuild count and the verdict of any quality probe that completed in it.
    void update(const MetricsSnapshot &s, const NetworkReport &net, uint32_t encoderRebuilds,
                const std::string &probeVerdict) {
        const bool sourceActive = s.sourceFramesInterval >= 10;
        const uint64_t lost = net.loss && net.packets ? uint64_t(*net.loss * double(*net.packets) + 0.5) : 0;
        std::string trigger;
        if (probeVerdict == "corrupted" || probeVerdict == "unrelated")
            trigger = "a quality probe found the receiver showing different content than was encoded";
        else if (net.loss && net.packets && *net.packets >= 50 && *net.loss >= 0.02)
            trigger = "packet loss " + fixed(*net.loss * 100, 1) + "% over " + std::to_string(*net.packets) +
                      " packets";
        else if (net.pli || net.fir)
            trigger = "the receiver asked for a keyframe (" + std::to_string(net.pli) + " PLI, " +
                      std::to_string(net.fir) + " FIR)";
        else if (s.encoderQp && *s.encoderQp >= kBlockyQp && sourceActive)
            trigger = "encoder quantizer " + fixed(*s.encoderQp, 0) + " at " + std::to_string(s.bitrate / 1000) +
                      " kbps (flat blocks where the picture changes)";
        else if (s.viewerFreezes.value_or(0) > 0 && sourceActive)
            trigger = fixed(*s.viewerFreezes, 0) + " freezes at the receiver while the desktop was changing" +
                      (s.viewerFreezeMs ? " (" + fixed(*s.viewerFreezeMs, 0) + " ms frozen)" : "");
        if (!inEpisode_ && trigger.empty())
            return;
        const double encodedBitsPerSecond = s.encodeFps * s.frameBytesMean * 8;
        if (!inEpisode_) {
            inEpisode_ = true;
            clean_ = 0;
            started_ = Clock::now();
            trigger_ = trigger;
            worstQp_ = s.encoderQp.value_or(0);
            lowestBitrate_ = encodedBitsPerSecond;
            lostPackets_ = keyframeRequests_ = freezes_ = badProbes_ = 0;
            rebuildsAtStart_ = encoderRebuilds;
            seconds_ = 0;
            logWarning("Picture quality dropped: " + trigger + " | encoder " + std::to_string(s.bitrate / 1000) +
                       " kbps (target " + std::to_string(s.targetBitrate / 1000) + "), " + std::to_string(s.fps) +
                       " fps, QP " + (s.encoderQp ? fixed(*s.encoderQp, 0) : "-") + " | network " + net.source +
                       ": loss " + (net.loss ? fixed(*net.loss * 100, 2) + "%" : "-") + ", rtt " +
                       (net.rttMs ? fixed(*net.rttMs, 0) + " ms" : "-") + ", NACKed " +
                       std::to_string(net.nackedPackets) + " | receiver " +
                       (s.viewerFps ? fixed(*s.viewerFps, 0) : "-") + " fps | source: " + sourceContext(s));
        }
        if (!trigger.empty()) {
            ++seconds_;
            clean_ = 0;
        }
        worstQp_ = std::max(worstQp_, s.encoderQp.value_or(0));
        if (encodedBitsPerSecond > 0)
            lowestBitrate_ = lowestBitrate_ > 0 ? std::min(lowestBitrate_, encodedBitsPerSecond) : encodedBitsPerSecond;
        lostPackets_ += lost;
        keyframeRequests_ += net.pli + net.fir;
        freezes_ += uint64_t(s.viewerFreezes.value_or(0));
        if (probeVerdict == "corrupted" || probeVerdict == "unrelated")
            ++badProbes_;
        if (!trigger.empty() || ++clean_ < kRecoverySeconds)
            return;
        inEpisode_ = false;
        const auto seconds = std::chrono::duration<double>(Clock::now() - started_).count() - kRecoverySeconds;
        const uint32_t rebuilds = encoderRebuilds - rebuildsAtStart_;
        std::string cause;
        if (badProbes_)
            cause = "the receiver showed content that differs from the encoded frames (" + std::to_string(badProbes_) +
                    " probes): transmission or the receiver's decoder";
        else if (lostPackets_ || keyframeRequests_)
            cause = "packet loss: about " + std::to_string(lostPackets_) + " packets lost, " +
                    std::to_string(keyframeRequests_) + " keyframe requests";
        else if (worstQp_ >= kBlockyQp)
            cause = "too few bits for what changed on screen (encoder QP up to " + fixed(worstQp_, 0) + ")";
        else if (freezes_)
            cause = std::to_string(freezes_) + " receiver freezes with no loss or quantizer problem on record";
        else
            cause = "cause unclear";
        logInfo("Picture quality recovered after " + fixed(seconds, 0) + " s (started: " + trigger_ + ") | " + cause +
                " | worst QP " + (worstQp_ > 0 ? fixed(worstQp_, 0) : "-") + ", lowest encoded rate " +
                fixed(lowestBitrate_ / 1000000, 2) + " Mbps, " + std::to_string(rebuilds) +
                " encoder rebuilds, " + std::to_string(seconds_) + " degraded seconds");
    }
};
// Base64 for the probe grids that travel over the telemetry channel.
std::string base64(std::span<const uint8_t> in) {
    static const char *const table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const uint32_t v = uint32_t(in[i]) << 16 | (i + 1 < in.size() ? uint32_t(in[i + 1]) << 8 : 0) |
                           (i + 2 < in.size() ? in[i + 2] : 0);
        out += table[v >> 18 & 63];
        out += table[v >> 12 & 63];
        out += i + 1 < in.size() ? table[v >> 6 & 63] : '=';
        out += i + 2 < in.size() ? table[v & 63] : '=';
    }
    return out;
}
std::optional<std::vector<uint8_t>> unbase64(const std::string &in) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    if (in.size() % 4)
        return std::nullopt;
    std::vector<uint8_t> out;
    out.reserve(in.size() / 4 * 3);
    for (size_t i = 0; i < in.size(); i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k)
            v[k] = in[i + k] == '=' ? 0 : value(in[i + k]);
        if (v[0] < 0 || v[1] < 0 || v[2] < 0 || v[3] < 0)
            return std::nullopt;
        const uint32_t n = uint32_t(v[0]) << 18 | uint32_t(v[1]) << 12 | uint32_t(v[2]) << 6 | uint32_t(v[3]);
        out.push_back(uint8_t(n >> 16));
        if (in[i + 2] != '=')
            out.push_back(uint8_t(n >> 8));
        if (in[i + 3] != '=')
            out.push_back(uint8_t(n));
    }
    return out;
}
// One quality probe: a source frame whose NV12 surface (what the encoder read) and captured BGRA frame were read back,
// followed through the encoder and the network to the receiver, which reports the same luma grid for the frame it
// displayed with that RTP timestamp. Comparing the three grids says which stage changed the picture.
struct Probe {
    uint64_t id = 0, seq = 0;
    Clock::time_point created{};
    std::optional<CellStats> source, capture, received;
    unsigned sourceWidth = 0, captureWidth = 0;
    std::optional<uint32_t> rtp;
    std::optional<double> qp;
    size_t bytes = 0;
    bool idr = false, encoded = false, requested = false;
    std::filesystem::path dumpDirectory; // Set for a probe a receiver's mark asked for: its frames are saved here
};
constexpr uint64_t kCaptureTag = uint64_t(1) << 63; // Readback tag bit: the BGRA capture, not the NV12 surface
constexpr auto kProbeInterval = std::chrono::seconds(10);
// Puts the engine thread into the multimedia scheduling class (MMCSS) for the lifetime of the loop, so a busy
// desktop application cannot hold up capture or encoder servicing for a scheduler quantum. Falls back to a plain
// priority boost where MMCSS is unavailable.
class RealtimeScope {
    HANDLE task_ = nullptr;
    int previous_ = THREAD_PRIORITY_NORMAL;

  public:
    RealtimeScope() {
        DWORD index = 0;
        task_ = AvSetMmThreadCharacteristicsW(L"Capture", &index);
        if (!task_) {
            previous_ = GetThreadPriority(GetCurrentThread());
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        }
    }
    ~RealtimeScope() {
        if (task_)
            AvRevertMmThreadCharacteristics(task_);
        else
            SetThreadPriority(GetCurrentThread(), previous_);
    }
    const char *description() const {
        return task_ ? "MMCSS Capture" : "above-normal priority";
    }
};
const char *modeName(PipelineMode m) {
    switch (m) {
    case PipelineMode::Capture:
        return "capture";
    case PipelineMode::Convert:
        return "convert";
    case PipelineMode::Encode:
        return "encode";
    case PipelineMode::CaptureEncode:
        return "capture-encode";
    default:
        return "stream";
    }
}
// Cheap check that the selected output still is where it was. The capture backends fail loudly on real changes;
// this only catches a silent move or primary swap, without re-enumerating DXGI on the frame thread.
bool sameMonitor(const Display &d) {
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(d.monitor, &mi))
        return false;
    return memcmp(&mi.rcMonitor, &d.rect, sizeof(RECT)) == 0 &&
           ((mi.dwFlags & MONITORINFOF_PRIMARY) != 0) == d.primary && utf8(mi.szDevice) == d.name;
}
// Frames inside the encoder at once. A keyframe takes 20-30 ms on an integrated GPU; with three in flight the
// two frames arriving meanwhile queue up briefly instead of being dropped, and the queue drains within ~50 ms.
constexpr unsigned kMaxInFlight = 3;
static_assert(kMaxInFlight < Converter::slots(), "the NV12 ring must exceed the encoder queue depth");
} // namespace
PipelineMode parseMode(const std::string &s) {
    if (s == "capture")
        return PipelineMode::Capture;
    if (s == "convert")
        return PipelineMode::Convert;
    if (s == "encode")
        return PipelineMode::Encode;
    if (s == "capture-encode")
        return PipelineMode::CaptureEncode;
    if (s == "stream")
        return PipelineMode::Stream;
    throw std::runtime_error("Unknown benchmark mode");
}
StreamingEngine::StreamingEngine(EngineConfig config, std::function<void(const EngineEvent &)> events)
    : config_(std::move(config)), events_(std::move(events)) {
    if (!config_.matcher)
        throw std::invalid_argument("StreamingEngine needs a display matcher");
    if (config_.fps < 1 || config_.fps > 60)
        throw std::runtime_error("FPS must be 1..60");
}
StreamingEngine::~StreamingEngine() {
    requestStop();
    join();
}
void StreamingEngine::emit(EngineEventType type, std::string detail) {
    if (events_)
        events_({type, std::move(detail)});
}
void StreamingEngine::publish(const MetricsSnapshot &s) {
    std::lock_guard lock(snapshotMutex_);
    snapshot_ = s;
}
MetricsSnapshot StreamingEngine::snapshot() const {
    std::lock_guard lock(snapshotMutex_);
    return snapshot_;
}
const char *StreamingEngine::resolvedBackend() const {
    return backendName(resolved_);
}
void StreamingEngine::start() {
    if (thread_.joinable())
        return;
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}
void StreamingEngine::requestStop() {
    stop_ = true;
}
void StreamingEngine::join() {
    if (thread_.joinable())
        thread_.join();
}
void StreamingEngine::run() {
    running_ = true;
    std::string fatal;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error &) {
        // Already initialised on this thread: fine.
    }
    HRESULT mf = MFStartup(MF_VERSION);
    std::atomic<bool> watching{true};
    std::thread watchdog([this, &watching] {
        int64_t stalledAt = 0;
        const char *stalledIn = "";
        while (watching) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const int64_t beat = heartbeat_.load();
            if (!beat)
                continue;
            const auto age = Clock::now() - Clock::time_point(Clock::duration(beat));
            if (!stalledAt && age > std::chrono::seconds(3)) {
                stalledAt = beat;
                stalledIn = stage_.load();
                logWarning(std::string("Engine thread has not finished a loop iteration for ") +
                           fixed(std::chrono::duration<double>(age).count()) + " s (stage: " + stalledIn +
                           "); frames, signaling and keyframe requests wait until it does");
            } else if (stalledAt && beat != stalledAt) {
                logInfo(std::string("Engine thread resumed after ") +
                        fixed(std::chrono::duration<double>(Clock::duration(beat - stalledAt)).count()) +
                        " s stuck in " + stalledIn);
                stalledAt = 0;
            }
        }
    });
    try {
        if (FAILED(mf))
            throw std::runtime_error("Media Foundation startup failed");
        emit(EngineEventType::Started);
        loop();
    } catch (const std::exception &e) {
        fatal = e.what();
        logError(std::string("Streaming engine stopped: ") + fatal);
    } catch (const winrt::hresult_error &e) {
        fatal = "Windows Runtime error 0x" + std::to_string(uint32_t(e.code()));
        logError(std::string("Streaming engine stopped: ") + fatal);
    } catch (...) {
        fatal = "Unknown error in the streaming engine";
        logError(fatal);
    }
    watching = false;
    watchdog.join();
    try {
        transport_.reset();
    } catch (...) {
        logWarning("Transport teardown reported an error");
    }
    if (SUCCEEDED(mf))
        MFShutdown();
    running_ = false;
    emit(EngineEventType::Stopped, fatal);
}
void StreamingEngine::loop() {
    const auto &o = config_;
    const bool stream = o.mode == PipelineMode::Stream;
    std::ofstream csv;
    if (!o.csvPath.empty()) {
        csv.open(o.csvPath);
        if (!csv)
            throw std::runtime_error("Cannot open CSV");
        csv << "seconds,captured,encoded,dropped,no_change,capture_ms_mean,capture_ms_p95,capture_ms_p99,"
               "convert_submit_ms_mean,encode_ms_mean,encode_ms_p95,encode_ms_p99,frame_bytes_mean,bitrate,"
               "queue_depth,acquire_delay_ms_mean,acquire_delay_ms_p95,source_to_encoded_ms_mean,"
               "source_to_encoded_ms_p95,pipeline_ms_mean,pipeline_ms_p95,wakeups_per_s,loop_max_ms,send_ms_mean,"
               "send_ms_max,submit_interval_ms_p95,submit_interval_ms_max,frame_bytes_max,keyframes,cpu_percent,"
               "capture_fps,encode_fps\n";
    }
    auto start = Clock::now();
    const int64_t epoch = now100ns();
    PollTimer pollTimer;
    ResourceMeter resources;
    resources.sample();
    RealtimeScope realtime;
    logInfo(std::string("Streaming engine thread scheduling: ") + realtime.description());
    // How often the readable log gets a performance line. The full sample goes to perf.jsonl every second; this
    // is only so someone reading host.log can see what the stream was doing around an event.
    constexpr auto kSummaryInterval = std::chrono::seconds(30);
    auto lastSummary = Clock::now();
    uint32_t encoderRebuilds = 0;
    Samples<64> encoderRebuildTimes;
    std::optional<Clock::time_point> encoderRebuildStart; // Set when a rebuild is requested, read when it lands
    // Outlives a pipeline rebuild on purpose: a rebuild is usually part of the episode, not the end of it.
    QualityWatch quality;
    uint32_t configuredBitrate = o.bitrate.initial;
    auto lastEncoderChange = Clock::now();
    bool hadDisplay = false, reportedMissing = false;
    // Every record says which engine and which pipeline build it belongs to, and why that build happened, so a
    // counter that restarts from zero is explained by the record itself.
    const uint64_t engineSession = nextEngineSession();
    std::string buildReason = "start"; // Then encoder_rebuild, recovery (after an error) or display_returned
    uint64_t sourceSequence = 0;       // Numbers every new source frame this engine takes, in order
    // Network adaptation and the stream shape it implies. They outlive pipeline rebuilds: a rebuild is how a
    // decision is applied, not a reason to forget it.
    NetworkAdaptation adaptation(o.bitrate.initial, o.bitrate.minimum, o.bitrate.maximum);
    // A pinned bitrate (bench --bitrate) means no adaptation at all, so the shape stays as configured too.
    const bool adaptive = o.bitrate.minimum < o.bitrate.maximum;
    StreamShape shape = adaptive ? shapeFor(o.bitrate.initial, o.bitrate.initial, o.fps, StreamShape{o.fps, 1080})
                                 : StreamShape{o.fps, 1080};
    NetworkAdaptation::Decision lastDecision;
    NetworkReport lastNet;
    std::optional<Clock::time_point> disconnectedSince;
    auto lastAdaptationLog = Clock::now() - std::chrono::hours(1);
    uint64_t sentBytesInterval = 0;
    KeyframePolicy keyframePolicy;
    // Sample times only ever move forward, across rebuilds too: they are the RTP timestamps the receiver orders by.
    int64_t lastSampleTime = -1;
    uint64_t rtpAdjusted = 0, framesWithheld = 0, chainBreaks = 0;
    // The last seconds of the stream, dumped when a receiver marks a damaged picture (diagnostics on only).
    FlightRecorder flight;
    std::deque<Probe> probes;
    uint64_t probeIds = 0, probesSent = 0, probesMissed = 0, conversionMismatches = 0, marks = 0;
    std::map<std::string, uint64_t> probeVerdicts;
    std::string intervalProbeVerdict;
    nlohmann::json lastProbe = nullptr;
    auto lastProbeAt = Clock::now();
    auto lastMarkAt = Clock::now() - std::chrono::hours(1);
    std::optional<std::filesystem::path> markPending;
    auto diagnosticsDirectory = [] {
        auto dir = Log::instance().archiveDirectory();
        if (dir.empty())
            dir = Log::instance().path().parent_path();
        if (dir.empty())
            dir = std::filesystem::current_path();
        return dir;
    };
    // Compares a probe's receiver grid with its source grid and records the verdict. True when done.
    auto completeProbe = [&](Probe &probe) {
        if (!probe.source || !probe.received)
            return false;
        const auto c = compareCells(*probe.source, *probe.received);
        const auto verdict = c.verdict();
        ++probeVerdicts[verdict];
        intervalProbeVerdict = verdict;
        lastProbe = {{"verdict", verdict},
                     {"seq", probe.seq},
                     {"qp", probe.qp ? nlohmann::json(*probe.qp) : nlohmann::json()},
                     {"bytes", probe.bytes},
                     {"mean_abs_diff", c.meanAbsDiff},
                     {"max_abs_diff", c.maxAbsDiff},
                     {"mismatched_cells", c.mismatchedCells},
                     {"flattened_cells", c.flattenedCells},
                     {"detailed_cells", c.detailedCells},
                     {"bad_rows", c.firstBadRow < 0 ? nlohmann::json() : nlohmann::json{c.firstBadRow, c.lastBadRow}},
                     {"gain", c.gain},
                     {"offset", c.offset}};
        const std::string text =
            "Quality probe (frame " + std::to_string(probe.seq) + ", QP " + (probe.qp ? fixed(*probe.qp, 0) : "-") +
            ", " + std::to_string(probe.bytes) + " B): receiver " + verdict + " | mean difference " +
            fixed(c.meanAbsDiff) + ", max " + fixed(c.maxAbsDiff) + " levels, " + std::to_string(c.mismatchedCells) +
            " cells differ" +
            (c.firstBadRow >= 0 ? " (grid rows " + std::to_string(c.firstBadRow) + "-" + std::to_string(c.lastBadRow) +
                                      " of " + std::to_string(probe.received->rows) + ")"
                                : "") +
            ", " + std::to_string(c.flattenedCells) + " of " + std::to_string(c.detailedCells) +
            " detailed cells lost their detail";
        if (verdict == "match")
            logDebug(text);
        else
            logWarning(text);
        return true;
    };
    // Bench recording: the encoded stream, and optionally sampled source surfaces, for offline comparison.
    std::unique_ptr<BitstreamRecorder> recorder;
    std::ofstream sourceIndex;
    std::filesystem::path sourcePath;
    uint64_t sourceSnapshots = 0;
    if (!o.recordPath.empty()) {
        recorder = std::make_unique<BitstreamRecorder>(o.recordPath);
        if (!recorder->open())
            throw std::runtime_error("Cannot open the recording file");
        if (o.recordSourceEvery) {
            sourcePath = o.recordPath;
            sourcePath.replace_extension(".nv12");
            std::filesystem::remove(sourcePath);
            auto indexPath = std::filesystem::path(o.recordPath);
            indexPath.replace_extension(".source.jsonl");
            sourceIndex.open(indexPath, std::ios::trunc);
        }
    }
    auto lastDisplayReport = Clock::now() - std::chrono::hours(1);
    auto timeUp = [&] { return o.seconds && Clock::now() - start >= std::chrono::seconds(o.seconds); };
    auto transportEvents = [this](const TransportEvent &t) {
        static const EngineEventType map[] = {
            EngineEventType::SignalingConnecting, EngineEventType::SignalingConnected,
            EngineEventType::SignalingDisconnected, EngineEventType::SignalingRejected,
            EngineEventType::ViewerJoined, EngineEventType::ViewerLeft, EngineEventType::WebRtcConnected,
            EngineEventType::WebRtcDisconnected, EngineEventType::CodeRotated};
        emit(map[size_t(t.type)], t.detail);
        // transport_ is null while the transport is being destroyed (unique_ptr::reset clears it first).
        if (transport_ && (t.type == TransportEventType::CodeRotated || t.type == TransportEventType::ViewerJoined ||
                           t.type == TransportEventType::ViewerLeft || t.type == TransportEventType::WebRtcConnected ||
                           t.type == TransportEventType::WebRtcDisconnected ||
                           t.type == TransportEventType::SignalingConnected ||
                           t.type == TransportEventType::SignalingDisconnected)) {
            auto s = snapshot();
            transport_->fillMetrics(s);
            publish(s);
        }
    };
    // Waits outside the frame loop (no display yet, retrying after an error) still count as progress for the
    // stall watchdog.
    auto alive = [this](const char *stage) {
        heartbeat_ = Clock::now().time_since_epoch().count();
        stage_ = stage;
    };
    auto serviceTransport = [&] {
        if (!transport_)
            return;
        transport_->poll();
        if (kick_.exchange(false))
            transport_->disconnectViewer();
        if (rotate_.exchange(false))
            transport_->rotateCode();
    };
    while (!stop_ && !timeUp()) {
        try {
            // Locate the display by identity every time the pipeline is (re)built. Nothing else is ever captured.
            auto match = o.matcher();
            if (!match.display) {
                if (!reportedMissing || Clock::now() - lastDisplayReport > std::chrono::seconds(30)) {
                    logWarning(match.detail.empty() ? "Selected display is not available" : match.detail);
                    lastDisplayReport = Clock::now();
                }
                if (hadDisplay && !reportedMissing)
                    emit(EngineEventType::DisplayLost, match.detail);
                reportedMissing = true;
                if (hadDisplay)
                    buildReason = "display_returned";
                for (int i = 0; i < 25 && !stop_; ++i) {
                    alive("waiting for the display");
                    serviceTransport();
                    pollTimer.wait(20);
                }
                continue;
            }
            if (reportedMissing && hadDisplay)
                emit(EngineEventType::DisplayFound);
            reportedMissing = false;
            hadDisplay = true;
            const auto selected = *match.display;
            alive("pipeline build");
            if (selected.primary && !o.allowPrimary && !o.synthetic && o.mode != PipelineMode::Encode)
                throw std::runtime_error("Selected display is primary. Capture refused.");
            unsigned iw = selected.rect.right - selected.rect.left, ih = selected.rect.bottom - selected.rect.top;
            if (o.synthetic || o.mode == PipelineMode::Encode) {
                iw = 1920;
                ih = 1080;
            }
            // The ladder may ask for 720p when the link cannot carry 1080p at a usable quantizer.
            double scale = std::min({1.0, 1920.0 / iw, 1080.0 / ih, double(shape.height) / std::max(1u, ih)});
            unsigned ow = unsigned(iw * scale) & ~1u, oh = unsigned(ih * scale) & ~1u;
            // Stage timings for the build. A slow start is felt as "it takes a moment before my screen appears",
            // and each stage has a different cause, so they are measured apart rather than as one total.
            const auto buildStart = Clock::now();
            auto sinceBuildStart = [&] {
                return std::chrono::duration<double, std::milli>(Clock::now() - buildStart).count();
            };
            Device device(selected);
            const double deviceMs = sinceBuildStart();
            std::unique_ptr<Pattern> pattern;
            if (o.pattern)
                pattern = std::make_unique<Pattern>(selected, o.syntheticContent);
            std::unique_ptr<ICapture> capture;
            CaptureBackend used = resolved_;
            if (!o.synthetic && o.mode != PipelineMode::Encode) {
                if (used == CaptureBackend::Auto)
                    used = o.backend;
                if (used == CaptureBackend::Auto) {
                    // Auto prefers Windows Graphics Capture (frame-arrival events, cursor included) and falls back
                    // to desktop duplication where it is unavailable.
                    try {
                        capture = wgc(device, selected);
                        used = CaptureBackend::Wgc;
                    } catch (const std::exception &e) {
                        logWarning(std::string("WGC unavailable, using DXGI duplication: ") + e.what());
                        capture = duplication(device, selected);
                        used = CaptureBackend::Dxgi;
                    }
                } else
                    capture = used == CaptureBackend::Wgc ? wgc(device, selected) : duplication(device, selected);
                resolved_ = used;
            }
            const double captureMs = sinceBuildStart() - deviceMs;
            std::unique_ptr<Converter> converter;
            std::unique_ptr<IEncoder> encoder;
            if (o.mode != PipelineMode::Capture)
                converter = std::make_unique<Converter>(device, iw, ih, ow, oh);
            const double converterMs = sinceBuildStart() - deviceMs - captureMs;
            if (o.mode == PipelineMode::Encode || o.mode == PipelineMode::CaptureEncode || stream) {
                // Periodic keyframes are forced on a timer by the keyframe policy; the encoder's own GOP only has to
                // be long enough never to fire first.
                auto tuning = o.encoder;
                if (!tuning.gopFrames)
                    tuning.gopFrames = shape.fps * 60;
                encoder = hardwareEncoder(device, selected, ow, oh, shape.fps, configuredBitrate, kMaxInFlight, tuning);
            }
            const double encoderMs = sinceBuildStart() - deviceMs - captureMs - converterMs;
            const double buildMs = sinceBuildStart();
            const auto ready = Clock::now();
            const uint64_t pipelineSession = nextPipelineSession();
            const std::string pipelineReason = buildReason;
            const std::string backendLabel =
                o.synthetic || o.mode == PipelineMode::Encode ? "Generated GPU source" : backendName(used);
            logInfo("Pipeline: " + backendLabel + " -> GPU NV12 -> " + (encoder ? encoder->name() : "benchmark") +
                    " | GPU " + selected.gpu + " | " + std::to_string(ow) + "x" + std::to_string(oh) + "@" +
                    std::to_string(shape.fps) + " | CPU readback: no | mode " + modeName(o.mode));
            // Where the wait before the first frame actually goes. The encoder stage dominates on most machines,
            // which is why a bitrate change that recreates it is expensive rather than free.
            logInfo("Pipeline built in " + fixed(buildMs) + " ms (device " + fixed(deviceMs) + ", capture " +
                    fixed(captureMs) + ", NV12 converter " + fixed(converterMs) + ", encoder " + fixed(encoderMs) +
                    ") at " + std::to_string(configuredBitrate) + " bps | pipeline session " +
                    std::to_string(pipelineSession) + " (" + pipelineReason + "), engine session " +
                    std::to_string(engineSession));
            if (encoderRebuildStart) {
                // The gap the receiver actually saw: from deciding to rebuild until the new pipeline is standing.
                const double gap =
                    std::chrono::duration<double, std::milli>(ready - *encoderRebuildStart).count();
                encoderRebuildTimes.add(gap);
                encoderRebuildStart.reset();
                logInfo("Encoder rebuild interrupted the stream for " + fixed(gap) + " ms (mean " +
                        fixed(encoderRebuildTimes.mean()) + " ms over " + std::to_string(encoderRebuilds) +
                        " rebuilds)");
            }
            if (encoder)
                logInfo("Encoder configuration: " + encoder->configuration());
            emit(EngineEventType::EncoderReady, encoder ? encoder->name() : "");
            if (stream && !transport_)
                transport_ = webRtc(o.signalingUrl, o.hostSecret, transportEvents, o.test);
            {
                MetricsSnapshot s = snapshot();
                s.width = ow;
                s.height = oh;
                s.fps = shape.fps;
                s.encoder = encoder ? encoder->name() : "none";
                s.gpu = selected.gpu;
                s.backend = backendLabel;
                s.bitrate = configuredBitrate;
                s.targetBitrate = adaptation.bitrate();
                s.updated = Clock::now();
                if (transport_)
                    transport_->fillMetrics(s);
                publish(s);
            }
            uint64_t captured = 0, encoded = 0, dropped = 0, noChange = 0, paced = 0, previousCaptured = 0,
                     previousEncoded = 0;
            // Every drop has a different remedy: coalesced frames mean the loop is behind, a busy ring means the
            // encoder is, a refused submit means the encoder is unhappy. Lumping them together hides all three.
            uint64_t droppedCoalesced = 0, droppedSuperseded = 0, droppedRingBusy = 0, droppedSubmitFailed = 0;
            Samples<> capTimes, conversionTimes, encodeTimes, pipelineTimes, sizes, acquireDelays, sourceToEncoded,
                sendTimes, submitIntervals;
            Samples<256> keyframeSizes, deltaSizes, keyframeEncodeTimes, topologyCheckTimes;
            LoopAccounting loop, previousLoop;
            auto timing = std::make_unique<HostTiming>();
            std::optional<double> firstEncodedMs, firstSentMs;
            uint64_t iterations = 0, previousIterations = 0, keyframes = 0;
            double loopMaxMs = 0, sendMaxMs = 0, frameBytesMax = 0, topologyCheckMaxMs = 0;
            std::optional<Clock::time_point> lastSubmit;
            bool sourceTimeWarned = false;
            uint32_t bitrate = configuredBitrate;
            bool dynamicBitrate = true;
            // Frames handed to the network must form an unbroken reference chain: after a frame that never reached
            // the packetizer, the next P-frames reference a picture the receiver does not have, and it would decode
            // them anyway (their RTP sequence numbers are continuous). They are withheld until the next IDR.
            bool awaitingIdr = true;
            h264::ReferenceChain sentChain;
            keyframePolicy.reset();
            std::unique_ptr<Readback> probeReadback;
            if (converter && stream)
                probeReadback = std::make_unique<Readback>(device, 4);
            bool pitchLogged = false;
            std::optional<std::tuple<unsigned, unsigned, DXGI_FORMAT>> captureSurface;
            auto report = Clock::now(), next = Clock::now(), lastInput = Clock::now(), lastFullCheck = Clock::now(),
                 lastBitrateSwitch = Clock::now();
            ComPtr<ID3D11Texture2D> synthetic;
            std::unique_ptr<SyntheticSource> generator;
            h264::Parser bitstream; // Parameter sets arrive with this encoder's first keyframe
            BitstreamStats streamStats;
            std::unique_ptr<Readback> sourceReadback;
            if (recorder && o.recordSourceEvery && converter)
                sourceReadback = std::make_unique<Readback>(device, 4);
            auto collectSource = [&](bool wait) {
                if (!sourceReadback)
                    return;
                for (auto &cpu : sourceReadback->collect(wait)) {
                    writeRaw(sourcePath, cpu, true);
                    sourceIndex << nlohmann::json{{"seq", cpu.tag},
                                                  {"index", sourceSnapshots++},
                                                  {"width", cpu.width},
                                                  {"height", cpu.height},
                                                  {"row_pitch", cpu.rowPitch},
                                                  {"depth_pitch", cpu.depthPitch},
                                                  {"map_wait_ms", cpu.mapWaitMs}}
                                       .dump()
                                << '\n';
                }
            };
            bool keyframePending = true, haveSurface = false, repeatRequested = false;
            // Frame pacing state. Frames are taken the moment the compositor delivers them (event-driven); the
            // configured rate only thins them out, it never schedules them.
            const int64_t period100ns = 10000000 / shape.fps;
            const auto period = std::chrono::nanoseconds(1000000000 / shape.fps);
            int64_t lastAccepted = 0;
            size_t slot = 0, lastSlot = 0;
            std::optional<Frame> carried; // Taken from the capture but not yet accepted by the encoder
            if (o.mode == PipelineMode::Encode || o.synthetic) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = iw;
                d.Height = ih;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                check(device.device->CreateTexture2D(&d, nullptr, &synthetic), "Benchmark BGRA source");
                generator = std::make_unique<SyntheticSource>(device.device.Get(), device.context.Get(), iw, ih,
                                                              o.syntheticContent, o.scrollSpeed);
                generator->render(synthetic.Get(), 0);
                if (o.mode == PipelineMode::Encode) {
                    converter->convert(synthetic.Get(), 0);
                    device.context->Flush();
                }
            }
            // However this pipeline session ends - a stop, an encoder rebuild or a failure - it leaves one line
            // with the totals, so the log can be read backwards from a complaint to the session that caused it.
            struct SessionSummary {
                std::function<void()> report;
                ~SessionSummary() {
                    report();
                }
            } sessionSummary{[&] {
                collectSource(true);
                const double seconds = std::chrono::duration<double>(Clock::now() - ready).count();
                if (seconds < 1 || !captured)
                    return;
                logInfo("Pipeline session ended after " + fixed(seconds, 0) + " s (pipeline session " +
                        std::to_string(pipelineSession) + "): captured " +
                        std::to_string(captured) + " (" + fixed(captured / seconds) + "/s), encoded " +
                        std::to_string(encoded) + " (" + fixed(encoded / seconds) + "/s), dropped " +
                        std::to_string(dropped) + ", paced " + std::to_string(paced) + ", keyframes " +
                        std::to_string(keyframes) + " | encode mean " + fixed(encodeTimes.mean()) + " ms, p99 " +
                        fixed(encodeTimes.percentile(.99)) + " ms | mean frame " +
                        std::to_string(uint64_t(sizes.mean())) + " B | engine thread busy " +
                        fixed(loop.busyMs / (seconds * 1000) * 100) + "% over " + std::to_string(iterations) +
                        " wake-ups | first frame encoded " + (firstEncodedMs ? fixed(*firstEncodedMs) : "-") +
                        " ms, sent " + (firstSentMs ? fixed(*firstSentMs) : "-") + " ms after build");
            }};
            while (!stop_ && !timeUp()) {
                heartbeat_ = Clock::now().time_since_epoch().count();
                stage_ = "waiting";
                // ---- Sleep until there is something to do: a frame, encoder news, a transport message, or the
                // housekeeping deadline. Nothing here spins on a timer while connected.
                const bool active = !transport_ || transport_->connected();
                const bool busy = encoder && (encoder->pending() || carried);
                if (!active)
                    timing->source.pause();
                std::optional<Frame> frame;
                bool fresh = false;
                Wake wake = Wake::Timeout;
                if (synthetic) {
                    auto now = Clock::now();
                    if (now < next) {
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count();
                        HANDLE handles[2];
                        DWORD n = 0;
                        if (encoder)
                            handles[n++] = encoder->event();
                        if (transport_)
                            handles[n++] = transport_->wakeEvent();
                        if (n && ms > 0)
                            WaitForMultipleObjects(n, handles, FALSE, DWORD(ms));
                        else
                            pollTimer.wait(unsigned(std::max<long long>(1, ms)));
                        now = Clock::now();
                    }
                    if (now >= next && active) {
                        next += period;
                        if (next < now)
                            next = now + period;
                        frame = Frame{synthetic, now100ns(), now100ns(), 1};
                        wake = Wake::Frame;
                    }
                } else if (capture && active && !capture->frameEvent()) {
                    // Desktop duplication has no event: block inside AcquireNextFrame, checking the encoder every
                    // millisecond while a frame is in flight. A carried frame must stay acquired, so only wait.
                    if (carried) {
                        HANDLE handles[2];
                        DWORD n = 0;
                        if (encoder)
                            handles[n++] = encoder->event();
                        if (transport_)
                            handles[n++] = transport_->wakeEvent();
                        if (n)
                            WaitForMultipleObjects(n, handles, FALSE, 1);
                    } else
                        frame = capture->acquire(busy ? 1 : 8);
                    if (frame)
                        wake = Wake::Frame;
                } else {
                    // The wait reasons are recorded in the same order the handles are added, so an idle desktop
                    // can be told apart from one where the encoder or the transport keeps waking the thread.
                    HANDLE handles[3];
                    Wake reasons[3];
                    DWORD n = 0;
                    if (capture && active) {
                        reasons[n] = Wake::Frame;
                        handles[n++] = capture->frameEvent();
                    }
                    if (encoder) {
                        reasons[n] = Wake::Encoder;
                        handles[n++] = encoder->event();
                    }
                    if (transport_) {
                        reasons[n] = Wake::Transport;
                        handles[n++] = transport_->wakeEvent();
                    }
                    const DWORD timeout = active ? 50 : 20;
                    const DWORD r = n ? WaitForMultipleObjects(n, handles, FALSE, timeout) : WAIT_TIMEOUT;
                    if (!n)
                        pollTimer.wait(timeout);
                    if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n)
                        wake = reasons[r - WAIT_OBJECT_0];
                    const bool frameSignalled = capture && active && r == WAIT_OBJECT_0;
                    // A carried frame is only replaced once a newer one has actually arrived.
                    if (capture && active && (!carried || frameSignalled))
                        frame = capture->acquire(0);
                }
                const auto iterationStart = Clock::now();
                heartbeat_ = iterationStart.time_since_epoch().count();
                stage_ = "capture";
                ++iterations;
                loop.woke(wake);
                const uint64_t encodedBefore = encoded;
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                {
                    uint32_t requests = 0;
                    if (transport_) {
                        stage_ = "transport";
                        serviceTransport();
                        requests = transport_->consumeKeyframeRequests();
                        for (auto &m : transport_->takeReceiverMessages()) {
                            const auto type = m.value("type", "");
                            if (type == "mark") {
                                // The person watching saw a damaged picture and pressed the key: keep the evidence.
                                if (Clock::now() - lastMarkAt < std::chrono::seconds(5) || marks >= 20)
                                    continue;
                                lastMarkAt = Clock::now();
                                ++marks;
                                auto stamp = logTimestamp();
                                std::replace(stamp.begin(), stamp.end(), ':', '-');
                                std::replace(stamp.begin(), stamp.end(), ' ', '_');
                                const auto dir = diagnosticsDirectory() / "marks" / stamp;
                                const auto units = flight.dump(dir);
                                markPending = dir;
                                logWarning("Receiver marked a damaged picture (receiver RTP timestamp " +
                                           (m.contains("rtp") && m["rtp"].is_number() ? m["rtp"].dump() : "-") +
                                           "): wrote the last " + std::to_string(units) +
                                           " frames of the stream to " + dir.string() +
                                           " and will save the next source frame there");
                            } else if (type == "probe") {
                                const auto id = m.value("id", uint64_t(0));
                                auto it = std::find_if(probes.begin(), probes.end(),
                                                       [&](const Probe &p) { return p.id == id; });
                                if (it == probes.end())
                                    continue;
                                if (m.value("missed", false)) {
                                    ++probesMissed;
                                    probes.erase(it);
                                    continue;
                                }
                                CellStats received;
                                received.columns = m.value("cols", 0u);
                                received.rows = m.value("rows", 0u);
                                if (auto mean = unbase64(m.value("mean", "")))
                                    received.mean = std::move(*mean);
                                if (auto detail = unbase64(m.value("detail", "")))
                                    received.detail = std::move(*detail);
                                it->received = std::move(received);
                                // The host's own readback usually lands first; if not, it completes the probe.
                                if (it->source && completeProbe(*it))
                                    probes.erase(it);
                            }
                        }
                    }
                    if (keyframe_.exchange(false))
                        requests |= 1u << KeyframePolicy::Host;
                    const auto t = Clock::now();
                    bool force = false;
                    for (unsigned r = 0; r < KeyframePolicy::kReasons; ++r)
                        if (requests & (1u << r))
                            force = keyframePolicy.request(KeyframePolicy::Reason(r), t) || force;
                    if (keyframePolicy.poll(t, transport_ && transport_->connected()))
                        force = true;
                    if (force) {
                        keyframePending = true;
                        repeatRequested = true;
                    }
                }
                if (encoder && o.bitrateSwitchSeconds &&
                    Clock::now() - lastBitrateSwitch >= std::chrono::seconds(o.bitrateSwitchSeconds)) {
                    // Bench experiment: does the encoder really follow a live bitrate change? Watch frame sizes.
                    lastBitrateSwitch = Clock::now();
                    const uint32_t target = bitrate == o.bitrate.maximum ? o.bitrate.minimum : o.bitrate.maximum;
                    const bool ok = encoder->bitrate(target);
                    logInfo("Bitrate switch to " + std::to_string(target) + " bps: " + (ok ? "accepted" : "rejected"));
                    if (ok)
                        bitrate = target;
                }
                if (encoder) {
                    if (keyframePending) {
                        encoder->keyframe();
                        keyframePending = false;
                    }
                    stage_ = "encoder output";
                    for (auto &f : encoder->poll()) {
                        ++encoded;
                        const auto done = now100ns();
                        const auto au = bitstream.parse(f.bytes);
                        streamStats.add(au, f.bytes.size(), f.qp);
                        const auto frameQp = BitstreamStats::frameQp(au, f.qp);
                        const bool idr = f.keyframe || au.idr;
                        if (idr)
                            keyframePolicy.produced(Clock::now());
                        if (recorder)
                            recorder->write(f.bytes, {{"seq", f.trace.sequence},
                                                      {"sample_time", f.timestamp},
                                                      {"rtp", rtpTimestamp(f.timestamp)},
                                                      {"bytes", f.bytes.size()},
                                                      {"key", f.keyframe},
                                                      {"idr", au.idr},
                                                      {"qp", frameQp ? nlohmann::json(*frameQp) : nlohmann::json()},
                                                      {"slice_qp", au.qpMin ? nlohmann::json(au.qpMean) : nlohmann::json()},
                                                      {"slices", au.slices},
                                                      {"frame_num", au.frameNum ? nlohmann::json(*au.frameNum)
                                                                                : nlohmann::json()},
                                                      {"pipeline_session", pipelineSession}});
                        encodeTimes.add(f.latencyMs);
                        pipelineTimes.add((done - epoch - f.timestamp) / 10000.0);
                        if (f.presented)
                            sourceToEncoded.add((done - f.presented) / 10000.0);
                        sizes.add(double(f.bytes.size()));
                        frameBytesMax = std::max(frameBytesMax, double(f.bytes.size()));
                        // Keyframes and delta frames are averaged apart: a keyframe here is an order of magnitude
                        // larger and several times slower, so one mean over both hides both numbers.
                        if (f.keyframe) {
                            ++keyframes;
                            keyframeSizes.add(double(f.bytes.size()));
                            keyframeEncodeTimes.add(f.latencyMs);
                        } else
                            deltaSizes.add(double(f.bytes.size()));
                        if (!firstEncodedMs)
                            firstEncodedMs = std::chrono::duration<double, std::milli>(Clock::now() - ready).count();
                        // Host-clock stages of this very frame. Repeats of the last surface carry no trace (they
                        // are not new source frames), and an output the encoder could not match is counted, not
                        // guessed at.
                        if (!f.matched)
                            ++timing->untraced;
                        if (f.outputSignalled)
                            timing->outputPickup.add(milliseconds(f.retrieved - *f.outputSignalled));
                        if (f.trace.sequence) {
                            if (f.trace.convertStarted)
                                timing->acquireToConvert.add(
                                    milliseconds(*f.trace.convertStarted - f.trace.acquired));
                            timing->acquireToEncodeSubmit.add(
                                milliseconds(f.trace.encodeSubmitted - f.trace.acquired));
                            timing->acquireToEncoded.add(milliseconds(f.retrieved - f.trace.acquired));
                        }
                        if (transport_) {
                            stage_ = "transport send";
                            const auto sendStart = Clock::now();
                            bool sent = false;
                            if (awaitingIdr && !idr)
                                ++framesWithheld;
                            else {
                                sent = transport_->send(f);
                                if (sent) {
                                    awaitingIdr = false;
                                    sentBytesInterval += f.bytes.size();
                                    if (!sentChain.follows(au)) {
                                        // Never expected: the encoder skipped a reference frame on its own.
                                        ++chainBreaks;
                                        awaitingIdr = true;
                                        logError("Encoded frame " + std::to_string(f.trace.sequence) +
                                                 " does not follow the previous reference frame (frame_num " +
                                                 (au.frameNum ? std::to_string(*au.frameNum) : "-") +
                                                 "); holding delta frames until the next IDR");
                                        if (keyframePolicy.request(KeyframePolicy::ChainBroken, Clock::now()))
                                            encoder->keyframe();
                                    }
                                    if (logRecording())
                                        flight.push(f.bytes, idr,
                                                    {{"seq", f.trace.sequence},
                                                     {"rtp", transport_->rtpTimestampOf(f.timestamp).value_or(0)},
                                                     {"bytes", f.bytes.size()},
                                                     {"idr", idr},
                                                     {"qp", frameQp ? nlohmann::json(*frameQp) : nlohmann::json()},
                                                     {"at", logTimestamp()}});
                                } else {
                                    if (!awaitingIdr && transport_->connected() &&
                                        keyframePolicy.request(KeyframePolicy::ChainBroken, Clock::now()))
                                        encoder->keyframe();
                                    awaitingIdr = true;
                                }
                            }
                            for (auto &probe : probes)
                                if (!probe.encoded && f.trace.sequence && probe.seq == f.trace.sequence) {
                                    probe.encoded = true;
                                    probe.qp = frameQp;
                                    probe.bytes = f.bytes.size();
                                    probe.idr = idr;
                                }
                            const auto sendEnd = Clock::now();
                            const auto sendMs = milliseconds(sendEnd - sendStart);
                            sendTimes.add(sendMs);
                            sendMaxMs = std::max(sendMaxMs, sendMs);
                            if (sent) {
                                timing->encodedToSend.add(milliseconds(sendEnd - f.retrieved));
                                if (f.trace.sequence)
                                    timing->acquireToSend.add(milliseconds(sendEnd - f.trace.acquired));
                            }
                            if (sent && !firstSentMs)
                                firstSentMs =
                                    std::chrono::duration<double, std::milli>(Clock::now() - ready).count();
                        }
                    }
                    if (encoder->pending() && Clock::now() - lastInput > std::chrono::seconds(3))
                        throw std::runtime_error("Encoder stalled; recreating GPU pipeline");
                }
                collectSource(false);
                if (probeReadback)
                    for (auto &cpu : probeReadback->collect()) {
                        const bool isCapture = cpu.tag & kCaptureTag;
                        const uint64_t seq = cpu.tag & ~kCaptureTag;
                        if (!isCapture && !pitchLogged) {
                            // The stride the driver really uses for this surface, once per pipeline: a wrong row pitch
                            // shows up as a sheared or banded picture, so it is on record rather than assumed.
                            pitchLogged = true;
                            logInfo("NV12 readback: " + std::to_string(cpu.width) + "x" + std::to_string(cpu.height) +
                                    ", row pitch " + std::to_string(cpu.rowPitch) + " B, depth pitch " +
                                    std::to_string(cpu.depthPitch) + " B (tightly packed would be " +
                                    std::to_string(cpu.width) + " and " +
                                    std::to_string(size_t(cpu.width) * cpu.height * 3 / 2) + "), copy ready after " +
                                    fixed(cpu.mapWaitMs) + " ms");
                        }
                        auto it = std::find_if(probes.begin(), probes.end(), [&](const Probe &p) { return p.seq == seq; });
                        if (it == probes.end())
                            continue;
                        (isCapture ? it->capture : it->source) = lumaCells(cpu);
                        (isCapture ? it->captureWidth : it->sourceWidth) = cpu.width;
                        if (!it->dumpDirectory.empty()) {
                            std::error_code ec;
                            std::filesystem::create_directories(it->dumpDirectory, ec);
                            writeBmp(it->dumpDirectory / ((isCapture ? "captured-" : "encoder-input-") +
                                                          std::to_string(seq) + ".bmp"),
                                     cpu);
                        }
                        if (it->source && it->capture) {
                            // Capture stage versus conversion stage: the NV12 surface must carry the same picture as
                            // the captured frame it was converted from. When the ladder scaled it down, detail
                            // legitimately differs, so only the content (cell means) is compared.
                            auto converted = *it->source;
                            if (it->captureWidth != it->sourceWidth)
                                converted.detail = it->capture->detail;
                            const auto c = compareCells(*it->capture, converted, 8);
                            if (c.valid && c.verdict() != "match") {
                                ++conversionMismatches;
                                logWarning("Probe: the NV12 surface the encoder read differs from the captured frame "
                                           "(frame " + std::to_string(seq) + ", " + c.verdict() + ", max " +
                                           fixed(c.maxAbsDiff) + " levels, " + std::to_string(c.mismatchedCells) +
                                           " cells): the conversion stage changed the picture");
                            }
                            it->capture.reset(); // Checked once
                        }
                        if (completeProbe(*it))
                            probes.erase(it);
                    }
                auto now = Clock::now();
                if (frame) {
                    fresh = true;
                    ++captured;
                    frame->sequence = ++sourceSequence;
                    timing->source.frame(from100ns(frame->timestamp));
                    if (frame->accumulated > 1) {
                        dropped += frame->accumulated - 1;
                        droppedCoalesced += frame->accumulated - 1;
                    }
                    if (carried) {
                        // Superseded by a newer frame before the encoder could take it.
                        ++dropped;
                        ++droppedSuperseded;
                        carried.reset();
                    }
                    if (frame->presented) {
                        const double waited = (frame->timestamp - frame->presented) / 10000.0;
                        if (waited > 5000) {
                            // A frame that sat in the capture pool while nobody was watching (capture pauses without
                            // a receiver): its stamp is honest but old, so it is not a latency sample.
                            frame->presented = 0;
                        } else if (waited < -50) {
                            // Not on our clock: ignore the source stamp rather than report nonsense.
                            if (!sourceTimeWarned)
                                logWarning("Capture source timestamps are not comparable to the host clock");
                            sourceTimeWarned = true;
                            frame->presented = 0;
                        } else
                            acquireDelays.add(waited);
                    }
                    if (!synthetic) {
                        // Time from the compositor's stamp to this thread taking the frame, in the historical
                        // "capture latency" column; the acquire call itself is no longer where frames wait.
                        capTimes.add(frame->presented ? (frame->timestamp - frame->presented) / 10000.0 : 0.0);
                        D3D11_TEXTURE2D_DESC fd{};
                        frame->texture->GetDesc(&fd);
                        if (!captureSurface || *captureSurface != std::make_tuple(fd.Width, fd.Height, fd.Format)) {
                            captureSurface = std::make_tuple(fd.Width, fd.Height, fd.Format);
                            std::ostringstream flags;
                            flags << std::hex << "bind 0x" << fd.BindFlags << ", misc 0x" << fd.MiscFlags;
                            logInfo("Capture surface: " + std::to_string(fd.Width) + "x" + std::to_string(fd.Height) +
                                    ", DXGI format " + std::to_string(int(fd.Format)) +
                                    (fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? " (B8G8R8A8_UNORM)" : "") + ", " +
                                    flags.str() + ", mips " + std::to_string(fd.MipLevels) + ", array " +
                                    std::to_string(fd.ArraySize));
                        }
                        if (fd.Width != iw || fd.Height != ih)
                            throw std::runtime_error("Display dimensions changed; recreate pipeline");
                    }
                } else if (carried) {
                    frame = carried;
                    carried.reset();
                } else if (capture && active) {
                    ++noChange;
                    timing->source.noChange();
                }
                if (frame) {
                    // Timestamps come from the compositor when it provides them: evenly spaced RTP timestamps
                    // instead of ones that carry this thread's scheduling jitter.
                    const int64_t stamp = frame->presented ? frame->presented : frame->timestamp;
                    if (fresh && shape.fps < 60 && lastAccepted && stamp - lastAccepted < period100ns * 3 / 4) {
                        ++paced; // Rate limiter for the 30 fps setting: skip frames that came too soon.
                        if (capture)
                            capture->release();
                    } else if (!converter) {
                        if (capture)
                            capture->release();
                    } else if (encoder && !encoder->ready()) {
                        carried = frame; // Keep it (and its capture buffer) until the encoder frees up.
                    } else {
                        auto t = Clock::now();
                        if (o.synthetic)
                            generator->render(synthetic.Get(), frame->sequence);
                        // Rotate through the NV12 ring so converting this frame never touches the surface the
                        // encoder is still reading for the previous one.
                        ID3D11Texture2D *nv12 = nullptr;
                        if (o.mode == PipelineMode::Encode)
                            nv12 = converter->texture(0);
                        else {
                            slot = (slot + 1) % Converter::slots();
                            nv12 = converter->texture(slot);
                        }
                        if (encoder && o.mode != PipelineMode::Encode && encoder->holds(nv12)) {
                            ++dropped; // Encoder is more than a ring behind: drop rather than overwrite.
                            ++droppedRingBusy;
                            if (capture)
                                capture->release();
                        } else {
                            if (o.mode != PipelineMode::Encode) {
                                stage_ = "convert";
                                converter->convert(frame->texture.Get(), slot);
                                if (sourceReadback && frame->sequence % o.recordSourceEvery == 0)
                                    sourceReadback->request(nv12, frame->sequence);
                                if (probeReadback && fresh && probes.size() < 4 && transport_ &&
                                    transport_->connected() &&
                                    (markPending || (logRecording() && t - lastProbeAt >= kProbeInterval)) &&
                                    probeReadback->pending() == 0 && probeReadback->request(nv12, frame->sequence) &&
                                    probeReadback->request(frame->texture.Get(), frame->sequence | kCaptureTag)) {
                                    Probe probe;
                                    probe.id = ++probeIds;
                                    probe.seq = frame->sequence;
                                    probe.created = t;
                                    if (markPending)
                                        probe.dumpDirectory = *markPending;
                                    markPending.reset();
                                    lastProbeAt = t;
                                    probes.push_back(std::move(probe));
                                }
                                if (o.flushGpu)
                                    device.context->Flush();
                                conversionTimes.add(
                                    std::chrono::duration<double, std::milli>(Clock::now() - t).count());
                            }
                            lastAccepted = stamp;
                            haveSurface = true;
                            if (encoder) {
                                const int64_t sampleTime = nextSampleTime(stamp - epoch, lastSampleTime);
                                if (sampleTime != stamp - epoch)
                                    ++rtpAdjusted;
                                stage_ = "encoder submit";
                                FrameTrace trace;
                                trace.sequence = frame->sequence;
                                trace.acquired = from100ns(frame->timestamp);
                                if (o.mode != PipelineMode::Encode)
                                    trace.convertStarted = t;
                                if (!encoder->submit(nv12, sampleTime, frame->presented, trace)) {
                                    ++dropped;
                                    ++droppedSubmitFailed;
                                } else {
                                    lastSampleTime = sampleTime;
                                    lastSlot = o.mode == PipelineMode::Encode ? 0 : slot;
                                    // The receiver is told which frame to measure before the frame leaves the
                                    // encoder, so it is already reading frames when that one is decoded.
                                    for (auto &probe : probes)
                                        if (probe.seq == frame->sequence && !probe.requested && transport_) {
                                            probe.rtp = transport_->rtpTimestampOf(sampleTime);
                                            if (probe.rtp && transport_->sendToReceiver({{"type", "probe-request"},
                                                                                        {"id", probe.id},
                                                                                        {"rtp", *probe.rtp},
                                                                                        {"cols", kProbeColumns},
                                                                                        {"rows", kProbeRows}})) {
                                                probe.requested = true;
                                                ++probesSent;
                                            }
                                        }
                                    if (lastSubmit)
                                        submitIntervals.add(
                                            std::chrono::duration<double, std::milli>(t - *lastSubmit).count());
                                    lastSubmit = t;
                                    lastInput = now;
                                    repeatRequested = false;
                                }
                            }
                            if (capture)
                                capture->release();
                        }
                    }
                } else if (encoder && haveSurface && active && encoder->ready() &&
                           (repeatRequested || now - lastInput > std::chrono::seconds(1))) {
                    // Static desktop: keep the receiver alive, and answer keyframe requests without waiting for
                    // the desktop to change.
                    auto *last = converter->texture(lastSlot);
                    if (!encoder->holds(last)) {
                        const int64_t sampleTime = nextSampleTime(now100ns() - epoch, lastSampleTime);
                        if (encoder->submit(last, sampleTime, 0)) {
                            lastSampleTime = sampleTime;
                            lastInput = now;
                            repeatRequested = false;
                            ++timing->repeats;
                        }
                    }
                }
                if (now - report >= std::chrono::seconds(1)) {
                    stage_ = "report";
                    const auto interval = std::chrono::duration<double>(now - report).count();
                    // ---- Network evidence for this second, and what adaptation makes of it.
                    const bool mediaUp = transport_ && transport_->connected();
                    const NetworkReport net = transport_ ? transport_->takeNetworkReport() : NetworkReport{};
                    lastNet = net;
                    const double sentBps = sentBytesInterval * 8 / interval;
                    sentBytesInterval = 0;
                    if (mediaUp) {
                        if (disconnectedSince && now - *disconnectedSince > std::chrono::seconds(10) &&
                            adaptation.bitrate() < o.bitrate.initial) {
                            // A new connection after a long gap is not the congested link of before.
                            adaptation = NetworkAdaptation(o.bitrate.initial, o.bitrate.minimum, o.bitrate.maximum);
                            logInfo("Bitrate target reset to " + std::to_string(o.bitrate.initial / 1000) +
                                    " kbps for the new connection");
                        }
                        disconnectedSince.reset();
                        lastDecision = adaptation.update({net.loss, net.packets, net.rttMs, sentBps});
                        if (lastDecision.changed ||
                            (lastDecision.congested && now - lastAdaptationLog > std::chrono::seconds(5))) {
                            lastAdaptationLog = now;
                            const auto base = adaptation.baselineRttMs();
                            logInfo(std::string(lastDecision.changed ? "Bitrate target " : "Congestion signal ") +
                                    (lastDecision.changed ? "now " + std::to_string(adaptation.bitrate() / 1000) +
                                                                " kbps"
                                                          : std::string("held")) +
                                    " (" + lastDecision.reason + ") | network " + net.source + ": loss " +
                                    (net.loss ? fixed(*net.loss * 100, 2) + "%" : "-") + " over " +
                                    (net.packets ? std::to_string(*net.packets) : "-") + " packets, rtt " +
                                    (net.rttMs ? fixed(*net.rttMs, 0) + " ms" : "-") + " (baseline " +
                                    (base ? fixed(*base, 0) + " ms" : "-") + "), NACKed " +
                                    std::to_string(net.nackedPackets) + ", sent " + fixed(sentBps / 1e6, 2) +
                                    " Mbps");
                        }
                    } else if (!disconnectedSince)
                        disconnectedSince = now;
                    auto elapsed = std::chrono::duration<double>(now - start).count();
                    const auto source = timing->source.take(now);
                    // Evidence independent of capture: when anyone last touched keyboard or mouse (session-wide),
                    // and whether the pointer is on the captured display, where moving it must produce frames.
                    std::optional<double> inputIdleMs;
                    LASTINPUTINFO input{sizeof input};
                    if (GetLastInputInfo(&input))
                        inputIdleMs = double(DWORD(GetTickCount() - input.dwTime));
                    std::optional<bool> cursorOnDisplay;
                    POINT cursor{};
                    if (GetCursorPos(&cursor))
                        cursorOnDisplay = PtInRect(&selected.rect, cursor) != FALSE;
                    MetricsSnapshot s;
                    s.captureFps = (captured - previousCaptured) / interval;
                    s.encodeFps = (encoded - previousEncoded) / interval;
                    s.captured = captured;
                    s.encoded = encoded;
                    s.dropped = dropped;
                    s.noChange = noChange;
                    s.sourceFramesInterval = source.frames;
                    s.noChangeInterval = source.noChange;
                    s.repeatFramesInterval = timing->repeats;
                    s.sourceGapMsMax = source.gapMax;
                    s.msSinceSourceFrame = source.sinceLastMs;
                    s.userInputIdleMs = inputIdleMs;
                    s.captureMsMean = capTimes.mean();
                    s.captureMsP95 = capTimes.percentile(.95);
                    s.convertSubmitMsMean = conversionTimes.mean();
                    if (encodeTimes.count()) {
                        s.encodeMsMean = encodeTimes.mean();
                        s.encodeMsP95 = encodeTimes.percentile(.95);
                        s.encodeMsP99 = encodeTimes.percentile(.99);
                    }
                    if (pipelineTimes.count()) {
                        s.pipelineMsMean = pipelineTimes.mean();
                        s.pipelineMsP95 = pipelineTimes.percentile(.95);
                    }
                    if (converter && converter->gpuTimes().count())
                        s.gpuSpanMsMean = converter->gpuTimes().mean();
                    if (acquireDelays.count()) {
                        s.acquireDelayMsMean = acquireDelays.mean();
                        s.acquireDelayMsP95 = acquireDelays.percentile(.95);
                    }
                    if (sourceToEncoded.count()) {
                        s.sourceToEncodedMsMean = sourceToEncoded.mean();
                        s.sourceToEncodedMsP95 = sourceToEncoded.percentile(.95);
                    }
                    const auto wakes = loop - previousLoop;
                    s.loopWakeupsPerSecond = (iterations - previousIterations) / interval;
                    s.loopMaxMs = loopMaxMs;
                    s.loopBusyPercent = wakes.busyMs / (interval * 1000) * 100;
                    s.wokeForFrame = wakes.frame;
                    s.wokeForEncoder = wakes.encoder;
                    s.wokeForTransport = wakes.transport;
                    s.wokeForTimeout = wakes.timeout;
                    s.wokeIdle = wakes.idle;
                    s.droppedCoalesced = droppedCoalesced;
                    s.droppedSuperseded = droppedSuperseded;
                    s.droppedRingBusy = droppedRingBusy;
                    s.droppedSubmitFailed = droppedSubmitFailed;
                    s.paced = paced;
                    s.sendMsMean = sendTimes.mean();
                    s.sendMsMax = sendMaxMs;
                    s.submitIntervalMsP95 = submitIntervals.percentile(.95);
                    s.submitIntervalMsMax = submitIntervals.count() ? submitIntervals.percentile(1.0) : 0;
                    s.frameBytesMax = frameBytesMax;
                    s.keyframes = keyframes;
                    s.frameBytesMean = sizes.mean();
                    s.keyframeBytesMean = keyframeSizes.mean();
                    s.deltaBytesMean = deltaSizes.mean();
                    s.keyframeEncodeMsMean = keyframeEncodeTimes.mean();
                    s.encoderRebuilds = encoderRebuilds;
                    s.encoderRebuildMsMean = encoderRebuildTimes.mean();
                    s.buildMs = buildMs;
                    s.firstEncodedMs = firstEncodedMs;
                    s.firstSentMs = firstSentMs;
                    s.topologyCheckMsMax = topologyCheckMaxMs;
                    s.bitrate = bitrate;
                    s.targetBitrate = transport_ ? adaptation.bitrate() : bitrate;
                    s.dynamicBitrate = dynamicBitrate;
                    if (streamStats.qp.count())
                        s.encoderQp = streamStats.qp.mean();
                    s.queueDepth = encoder ? encoder->pending() : 0;
                    const auto usage = resources.sample(selected.adapter.Get());
                    s.cpuPercent = usage.cpuPercent;
                    s.cpuKernelPercent = usage.cpuKernelPercent;
                    s.workingSetMb = (usage.workingSetBytes + 512 * 1024) / (1024 * 1024);
                    s.privateMb = (usage.privateBytes + 512 * 1024) / (1024 * 1024);
                    s.gpuMemoryMb = (usage.gpuLocalUsedBytes.value_or(0) + usage.gpuSharedUsedBytes.value_or(0) +
                                     512 * 1024) /
                                    (1024 * 1024);
                    s.handles = usage.handles;
                    s.onBattery = usage.onBattery;
                    s.batterySaver = usage.batterySaver;
                    s.width = ow;
                    s.height = oh;
                    s.fps = shape.fps;
                    s.encoder = encoder ? encoder->name() : "none";
                    s.gpu = selected.gpu;
                    s.backend = backendLabel;
                    s.updated = now;
                    if (transport_)
                        transport_->fillMetrics(s);
                    publish(s);
                    // Probes the receiver never answered (the frame was not shown, or the page is an older one).
                    for (auto it = probes.begin(); it != probes.end();)
                        if (now - it->created > std::chrono::seconds(5)) {
                            if (it->requested)
                                ++probesMissed;
                            it = probes.erase(it);
                        } else
                            ++it;
                    if (stream)
                        quality.update(s, net, encoderRebuilds, intervalProbeVerdict);
                    nlohmann::json stats = {{"type", "host-stats"},
                                            {"seconds", elapsed},
                                            {"captured", captured},
                                            {"encoded", encoded},
                                            {"dropped", dropped},
                                            {"no_change", noChange},
                                            {"paced", paced},
                                            {"capture_ms_mean", capTimes.mean()},
                                            {"capture_ms_p95", capTimes.percentile(.95)},
                                            {"capture_ms_p99", capTimes.percentile(.99)},
                                            {"convert_submit_ms_mean", conversionTimes.mean()},
                                            {"encode_ms_mean", optionalNumber(s.encodeMsMean)},
                                            {"encode_ms_p95", optionalNumber(s.encodeMsP95)},
                                            {"encode_ms_p99", optionalNumber(s.encodeMsP99)},
                                            {"frame_bytes_mean", sizes.mean()},
                                            {"bitrate", bitrate},
                                            {"queue_depth", encoder ? encoder->pending() : 0},
                                            {"capture_fps", s.captureFps},
                                            {"encode_fps", s.encodeFps},
                                            {"capture_backend", backendLabel},
                                            {"encoder", s.encoder},
                                            {"gpu", selected.gpu},
                                            {"video_path", "GPU"},
                                            {"hardware_encoder", bool(encoder)},
                                            {"cpu_percent", optionalNumber(s.cpuPercent)},
                                            {"dynamic_bitrate", dynamicBitrate},
                                            {"target_bitrate", s.targetBitrate},
                                            {"flush_gpu", o.flushGpu},
                                            {"pipeline_ms_mean", optionalNumber(s.pipelineMsMean)},
                                            {"pipeline_ms_p95", optionalNumber(s.pipelineMsP95)},
                                            {"acquire_delay_ms_mean", optionalNumber(s.acquireDelayMsMean)},
                                            {"acquire_delay_ms_p95", optionalNumber(s.acquireDelayMsP95)},
                                            {"source_to_encoded_ms_mean", optionalNumber(s.sourceToEncodedMsMean)},
                                            {"source_to_encoded_ms_p95", optionalNumber(s.sourceToEncodedMsP95)},
                                            {"wakeups_per_s", s.loopWakeupsPerSecond},
                                            {"loop_max_ms", s.loopMaxMs},
                                            {"send_ms_mean", s.sendMsMean},
                                            {"send_ms_max", s.sendMsMax},
                                            {"submit_interval_ms_p95", s.submitIntervalMsP95},
                                            {"submit_interval_ms_max", s.submitIntervalMsMax},
                                            {"frame_bytes_max", s.frameBytesMax},
                                            {"keyframes", keyframes},
                                            {"pattern_presented", pattern ? pattern->presented() : 0},
                                            // Added for optimization work: where the CPU goes, why frames are
                                            // lost, what a keyframe really costs, and how long a rebuild hurts.
                                            {"loop_busy_percent", s.loopBusyPercent},
                                            {"woke_for_frame", wakes.frame},
                                            {"woke_for_encoder", wakes.encoder},
                                            {"woke_for_transport", wakes.transport},
                                            {"woke_for_timeout", wakes.timeout},
                                            {"woke_idle", wakes.idle},
                                            {"dropped_coalesced", droppedCoalesced},
                                            {"dropped_superseded", droppedSuperseded},
                                            {"dropped_ring_busy", droppedRingBusy},
                                            {"dropped_submit_failed", droppedSubmitFailed},
                                            {"keyframe_bytes_mean", keyframeSizes.mean()},
                                            {"delta_bytes_mean", deltaSizes.mean()},
                                            {"keyframe_encode_ms_mean", keyframeEncodeTimes.mean()},
                                            {"encoder_rebuilds", encoderRebuilds},
                                            {"encoder_rebuild_ms_mean", encoderRebuildTimes.mean()},
                                            {"build_ms", buildMs},
                                            {"first_encoded_ms", optionalNumber(firstEncodedMs)},
                                            {"first_sent_ms", optionalNumber(firstSentMs)},
                                            {"topology_check_ms_mean", topologyCheckTimes.mean()},
                                            {"topology_check_ms_max", topologyCheckMaxMs},
                                            {"viewer_connected", transport_ ? transport_->connected() : false},
                                            {"fps_setting", o.fps},
                                            // Hoisted out of the receiver block: these are what "pixelated" and
                                            // "glitchy" actually are, and they are the first thing to plot.
                                            {"receiver_qp", optionalNumber(s.viewerQp)},
                                            {"receiver_corrupted", optionalNumber(s.viewerCorrupted)},
                                            {"receiver_freezes", optionalNumber(s.viewerFreezes)},
                                            {"receiver_pli", optionalNumber(s.viewerPli)},
                                            {"encoded_bits_per_second", s.encodeFps * s.frameBytesMean * 8}};
                    describe(stats, usage);
                    streamStats.describe(stats);
                    streamStats.endInterval();
                    // Adaptation: what the network said this second and what the encoder is asked to do about it.
                    stats["network_source"] = net.source;
                    stats["network_loss"] = optionalNumber(net.loss);
                    stats["network_packets"] = net.packets ? nlohmann::json(*net.packets) : nlohmann::json(nullptr);
                    stats["network_rtt_ms"] = optionalNumber(net.rttMs);
                    stats["network_jitter_ms"] = optionalNumber(net.jitterMs);
                    stats["network_rtt_baseline_ms"] = optionalNumber(adaptation.baselineRttMs());
                    stats["nacked_packets_interval"] = net.nackedPackets;
                    stats["pli_interval"] = net.pli;
                    stats["fir_interval"] = net.fir;
                    stats["rtcp_reports_interval"] = net.reports;
                    stats["sent_bps"] = sentBps;
                    stats["adaptation_target_bitrate"] = adaptation.bitrate();
                    stats["adaptation_congested"] = lastDecision.congested;
                    stats["adaptation_reason"] = lastDecision.reason;
                    stats["adaptation_clean_seconds"] = adaptation.cleanSeconds();
                    stats["stream_fps"] = shape.fps;
                    stats["stream_height"] = shape.height;
                    // Keyframes: who asked, how many were forced, how many requests were folded into one in flight.
                    stats["keyframes_forced"] = keyframePolicy.forced();
                    {
                        nlohmann::json reasons = nlohmann::json::object();
                        for (unsigned r = 0; r < KeyframePolicy::kReasons; ++r)
                            reasons[KeyframePolicy::name(KeyframePolicy::Reason(r))] =
                                keyframePolicy.requested(KeyframePolicy::Reason(r));
                        stats["keyframe_requests_by_reason"] = reasons;
                    }
                    stats["keyframe_requests_coalesced"] = keyframePolicy.coalesced();
                    stats["seconds_since_idr"] =
                        keyframePolicy.lastIdr()
                            ? nlohmann::json(std::chrono::duration<double>(now - *keyframePolicy.lastIdr()).count())
                            : nlohmann::json(nullptr);
                    // The reference chain the receiver decodes against, and the RTP timestamps it orders frames by.
                    stats["frames_withheld"] = framesWithheld;
                    stats["reference_chain_breaks"] = chainBreaks;
                    stats["rtp_timestamps_adjusted"] = rtpAdjusted;
                    // Quality probes: does the receiver show what was encoded?
                    stats["probes_sent"] = probesSent;
                    stats["probes_missed"] = probesMissed;
                    stats["probe_verdicts"] = probeVerdicts;
                    stats["probe_conversion_mismatches"] = conversionMismatches;
                    stats["probe_last"] = lastProbe;
                    stats["receiver_marks"] = marks;
                    stats["flight_recorder_units"] = flight.units();
                    stats["flight_recorder_bytes"] = flight.bytes();
                    intervalProbeVerdict.clear();
                    // Which run, engine and pipeline build this record belongs to. A cumulative counter restarts
                    // exactly when pipeline_session changes; pipeline_build_reason says why it did.
                    stats["run_id"] = runIdentity().runId;
                    if (!diagnosticTag().empty())
                        stats["diagnostic_tag"] = diagnosticTag();
                    stats["engine_session"] = engineSession;
                    stats["pipeline_session"] = pipelineSession;
                    stats["pipeline_session_seconds"] = std::chrono::duration<double>(now - ready).count();
                    stats["pipeline_build_reason"] = pipelineReason;
                    // The frame path on the host's steady clock alone, measured on the same traced frames. Kept
                    // apart from acquire_delay/source_to_encoded, which start from the compositor's stamp.
                    describeLatency(stats, "host_acquire_to_convert_ms", timing->acquireToConvert);
                    describeLatency(stats, "host_acquire_to_encode_submit_ms", timing->acquireToEncodeSubmit);
                    describeLatency(stats, "host_acquire_to_encoded_ms", timing->acquireToEncoded);
                    describeLatency(stats, "host_acquire_to_send_ms", timing->acquireToSend);
                    describeLatency(stats, "encoded_to_send_ms", timing->encodedToSend);
                    describeLatency(stats, "encoder_output_pickup_ms", timing->outputPickup);
                    stats["untraced_outputs_interval"] = timing->untraced;
                    // Still desktop or stalled capture: the interval view no_change cannot give on its own.
                    stats["no_change_interval"] = source.noChange;
                    stats["repeat_frames_interval"] = timing->repeats;
                    stats["source_frame_gap_ms_p95"] = optionalNumber(source.gapP95);
                    stats["source_frame_gap_ms_max"] = optionalNumber(source.gapMax);
                    stats["ms_since_last_source_frame"] = optionalNumber(source.sinceLastMs);
                    stats["user_input_idle_ms"] = optionalNumber(inputIdleMs);
                    stats["cursor_on_display"] =
                        cursorOnDisplay ? nlohmann::json(*cursorOnDisplay) : nlohmann::json(nullptr);
                    stats["receiver_frames_received"] = optionalNumber(s.viewerFramesReceived);
                    stats["receiver_frames_decoded"] = optionalNumber(s.viewerFramesDecoded);
                    stats["receiver_freeze_ms"] = optionalNumber(s.viewerFreezeMs);
                    if (converter && converter->gpuTimes().count()) {
                        // This asynchronous GPU command span includes driver submission delay.
                        // It is not a pure video-processor execution duration.
                        stats["gpu_command_span_ms_mean"] = converter->gpuTimes().mean();
                    }
                    if (transport_) {
                        stats["webrtc"] = transport_->stats();
                        // The receiver's dashboard reads a handful of these; the full record is several KB and would
                        // not fit its message limit.
                        nlohmann::json dashboard = {{"type", "host-stats"}};
                        for (const char *key :
                             {"capture_fps", "capture_backend", "encoder", "encode_ms_mean", "encode_ms_p95",
                              "encode_ms_p99", "acquire_delay_ms_mean", "source_to_encoded_ms_mean", "video_path", "gpu",
                              "cpu_percent", "queue_depth", "qp_mean", "bitrate", "target_bitrate", "stream_fps",
                              "stream_height", "network_rtt_ms", "network_loss", "keyframes_forced"})
                            if (stats.contains(key))
                                dashboard[key] = stats[key];
                        transport_->diagnostics(dashboard);
                    }
                    if (o.statsSink)
                        o.statsSink(stats);
                    // The full sample goes to perf.jsonl every second; host.log gets a readable digest far less
                    // often, so it stays a story of what happened rather than a wall of numbers.
                    logRecord(stats.dump());
                    if (now - lastSummary >= kSummaryInterval) {
                        lastSummary = now;
                        std::string line =
                            "Stream: capture " + fixed(s.captureFps) + " fps, encode " + fixed(s.encodeFps) +
                            " fps | encode " + fixed(s.encodeMsMean.value_or(0)) + " ms (p95 " +
                            fixed(s.encodeMsP95.value_or(0)) + ") | present to encoded " +
                            fixed(s.sourceToEncodedMsMean.value_or(0)) + " ms | queue " +
                            std::to_string(s.queueDepth) + " | " + std::to_string(bitrate / 1000) + " kbps (target " +
                            std::to_string(s.targetBitrate / 1000) + "), " + std::to_string(shape.fps) + " fps, " +
                            std::to_string(oh) + "p, QP " + (s.encoderQp ? fixed(*s.encoderQp, 0) : "-") + " | frame " +
                            std::to_string(uint64_t(s.frameBytesMean)) + " B, keyframe " +
                            std::to_string(uint64_t(s.keyframeBytesMean)) + " B";
                        logInfo(line);
                        logInfo("Source: " + std::to_string(source.frames) +
                                " new frames in the last second, gap p95 " + fixed(source.gapP95, 0) + " ms, max " +
                                fixed(source.gapMax, 0) + " ms | " +
                                std::to_string(source.noChange) + " polls with no new frame | newest frame " +
                                (source.sinceLastMs ? fixed(*source.sinceLastMs / 1000, 1) + " s ago" : "-") +
                                " | " + std::to_string(timing->repeats) + " repeat encodes | user input " +
                                (inputIdleMs ? fixed(*inputIdleMs / 1000, 1) + " s ago" : "-") +
                                (cursorOnDisplay && *cursorOnDisplay ? ", pointer on this display" : ""));
                        auto mean = [](const LatencyTrack &track) {
                            return track.rolling.count() ? fixed(track.rolling.mean()) : std::string("-");
                        };
                        auto p95 = [](const LatencyTrack &track) {
                            return track.rolling.count() ? fixed(track.rolling.percentile(.95))
                                                         : std::string("-");
                        };
                        logInfo("Host clock: acquire to convert " + mean(timing->acquireToConvert) +
                                ", to encoder " + mean(timing->acquireToEncodeSubmit) + ", to encoded " +
                                mean(timing->acquireToEncoded) + " (p95 " + p95(timing->acquireToEncoded) +
                                "), to sent " + mean(timing->acquireToSend) + " (p95 " +
                                p95(timing->acquireToSend) + ") ms | encoder output pickup " +
                                mean(timing->outputPickup) + " ms (p95 " + p95(timing->outputPickup) + ")");
                        if (const auto engines = summarizeGpuEngines(usage); !engines.empty())
                            logInfo("GPU engines, this process / all processes: " + engines);
                        logInfo("Engine thread: " + fixed(s.loopWakeupsPerSecond, 0) + " wake-ups/s (" +
                                std::to_string(wakes.frame) + " frame, " + std::to_string(wakes.encoder) +
                                " encoder, " + std::to_string(wakes.transport) + " transport, " +
                                std::to_string(wakes.timeout) + " timeout; " + std::to_string(wakes.idle) +
                                " did nothing), busy " + fixed(s.loopBusyPercent) + "% of wall time, longest " +
                                fixed(loopMaxMs) + " ms | " + summarize(usage));
                        if (dropped || paced)
                            logInfo("Frames not encoded: " + std::to_string(dropped) + " dropped (" +
                                    std::to_string(droppedCoalesced) + " coalesced, " +
                                    std::to_string(droppedSuperseded) + " superseded, " +
                                    std::to_string(droppedRingBusy) + " ring busy, " +
                                    std::to_string(droppedSubmitFailed) + " refused), " + std::to_string(paced) +
                                    " paced out, " + std::to_string(noChange) + " polls with no new frame");
                        if (transport_) {
                            auto receiver = [](const std::optional<double> &v, const char *unit, int decimals = 1) {
                                return v ? fixed(*v, decimals) + unit : std::string("-");
                            };
                            logInfo("Receiver: " + receiver(s.viewerFps, " fps") + " | " +
                                    receiver(s.rttMs, " ms rtt") + " | " +
                                    (s.loss ? fixed(*s.loss * 100, 2) + "% loss" : "- loss") + " | " +
                                    receiver(s.jitterMs, " ms jitter") + " | NACKed " +
                                    std::to_string(net.nackedPackets) + ", PLI " + std::to_string(net.pli) +
                                    ", FIR " + std::to_string(net.fir) + " this second | freezes " +
                                    receiver(s.viewerFreezes, "", 0) + " | jitter buffer " +
                                    receiver(s.viewerJitterBufferMs, " ms") + " | decode " +
                                    receiver(s.viewerDecodeMs, " ms") + " | frozen " +
                                    receiver(s.viewerFreezeMs, " ms", 0) + " | keyframe requests " +
                                    std::to_string(s.keyframeRequests) + " (" + std::to_string(keyframePolicy.forced()) +
                                    " forced, " + std::to_string(keyframePolicy.coalesced()) + " folded) | frames held "
                                    "for a broken chain " + std::to_string(framesWithheld) + " | socket write failures " +
                                    std::to_string(s.transportDropped) + " | probes " + std::to_string(probesSent) +
                                    " sent, " + std::to_string(probesMissed) + " missed");
                        }
                    }
                    previousCaptured = captured;
                    previousEncoded = encoded;
                    previousLoop = loop;
                    previousIterations = iterations;
                    if (csv) {
                        auto opt = [](const std::optional<double> &v) { return v ? *v : 0.0; };
                        csv << elapsed << ',' << captured << ',' << encoded << ',' << dropped << ',' << noChange
                            << ',' << capTimes.mean() << ',' << capTimes.percentile(.95) << ','
                            << capTimes.percentile(.99) << ',' << conversionTimes.mean() << ','
                            << encodeTimes.mean() << ',' << encodeTimes.percentile(.95) << ','
                            << encodeTimes.percentile(.99) << ',' << sizes.mean() << ',' << bitrate << ','
                            << (encoder ? encoder->pending() : 0) << ',' << opt(s.acquireDelayMsMean) << ','
                            << opt(s.acquireDelayMsP95) << ',' << opt(s.sourceToEncodedMsMean) << ','
                            << opt(s.sourceToEncodedMsP95) << ',' << opt(s.pipelineMsMean) << ','
                            << opt(s.pipelineMsP95) << ',' << s.loopWakeupsPerSecond << ',' << s.loopMaxMs << ','
                            << s.sendMsMean << ',' << s.sendMsMax << ',' << s.submitIntervalMsP95 << ','
                            << s.submitIntervalMsMax << ',' << s.frameBytesMax << ',' << keyframes << ','
                            << opt(s.cpuPercent) << ',' << s.captureFps << ',' << s.encodeFps << '\n';
                    }
                    loopMaxMs = 0;
                    sendMaxMs = 0;
                    frameBytesMax = 0;
                    timing->endInterval();
                    report = now;
                    if (stream && encoder && transport_) {
                        const uint32_t target = adaptation.bitrate();
                        const auto wanted = adaptive ? shapeFor(target, o.bitrate.initial, o.fps, shape) : shape;
                        const double ratio = double(target) / double(std::max<uint32_t>(1, bitrate));
                        const bool shapeChange = !(wanted == shape), rateChange = ratio < 0.85 || ratio > 1.15;
                        const auto since = now - lastEncoderChange;
                        if ((shapeChange || rateChange) &&
                            (since >= std::chrono::seconds(5) ||
                             (lastDecision.congested && since >= std::chrono::seconds(2)))) {
                            if (!shapeChange && dynamicBitrate && encoder->bitrate(target)) {
                                logInfo("Encoder bitrate " + std::to_string(bitrate / 1000) + " -> " +
                                        std::to_string(target / 1000) + " kbps (live)");
                                bitrate = configuredBitrate = target;
                                lastEncoderChange = now;
                            } else {
                                // Intel's encoder ignores a live bitrate change (see encoder.cpp): rebuild it.
                                ++encoderRebuilds;
                                logInfo("Recreating hardware encoder: " + std::to_string(bitrate / 1000) + " -> " +
                                        std::to_string(target / 1000) + " kbps, " + std::to_string(shape.fps) + " -> " +
                                        std::to_string(wanted.fps) + " fps, " + std::to_string(shape.height) + "p -> " +
                                        std::to_string(wanted.height) + "p after " +
                                        fixed(std::chrono::duration<double>(since).count(), 0) + " s (rebuild " +
                                        std::to_string(encoderRebuilds) + " this session, " +
                                        (lastDecision.reason[0] ? lastDecision.reason : "adaptation") + ")");
                                configuredBitrate = target;
                                shape = wanted;
                                lastEncoderChange = now;
                                encoderRebuildStart = now;
                                buildReason = "adaptation";
                                break;
                            }
                        }
                    }
                    stage_ = "topology";
                    // Topology: a cheap GDI check every second; the full DXGI enumeration (a multi-millisecond
                    // stall on this thread) only every ten seconds, to notice a GPU change as well.
                    if (!sameMonitor(selected))
                        throw std::runtime_error("Display topology changed; revalidate selection");
                    if (now - lastFullCheck >= std::chrono::seconds(10)) {
                        lastFullCheck = now;
                        // DXGI enumeration is a known multi-millisecond stall on this thread; measured so its
                        // real cost is on record rather than assumed.
                        const auto enumerateStart = Clock::now();
                        auto current = displays();
                        const double enumerateMs =
                            std::chrono::duration<double, std::milli>(Clock::now() - enumerateStart).count();
                        topologyCheckTimes.add(enumerateMs);
                        topologyCheckMaxMs = std::max(topologyCheckMaxMs, enumerateMs);
                        auto found = std::find_if(current.begin(), current.end(),
                                                  [&](auto &d) { return d.name == selected.name; });
                        if (found == current.end() || found->monitor != selected.monitor ||
                            found->primary != selected.primary ||
                            memcmp(&found->rect, &selected.rect, sizeof(RECT)) ||
                            memcmp(&found->luid, &selected.luid, sizeof(LUID)))
                            throw std::runtime_error("Display topology changed; revalidate selection");
                    }
                }
                const double iterationMs =
                    std::chrono::duration<double, std::milli>(Clock::now() - iterationStart).count();
                loopMaxMs = std::max(loopMaxMs, iterationMs);
                loop.busyMs += iterationMs;
                // A wake-up that produced neither a frame to work on nor an encoded frame did nothing useful.
                // While the desktop is still, these are the entire CPU cost of running at all.
                if (!frame && encoded == encodedBefore)
                    ++loop.idle;
            }
        } catch (const std::exception &e) {
            logWarning(e.what());
            buildReason = "recovery";
            if (!stream)
                throw;
            emit(EngineEventType::Error, e.what());
            logInfo("Retrying the selected display in 1 second");
            for (int i = 0; i < 50 && !stop_; ++i) {
                alive("retry wait");
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&message);
                    DispatchMessageW(&message);
                }
                serviceTransport();
                pollTimer.wait(20);
            }
        } catch (const winrt::hresult_error &e) {
            // Not a std::exception; treat it like any other pipeline failure and rebuild.
            std::string what = "Windows Runtime error 0x" + std::to_string(uint32_t(e.code()));
            logWarning(what);
            buildReason = "recovery";
            if (!stream)
                throw std::runtime_error(what);
            emit(EngineEventType::Error, what);
            for (int i = 0; i < 50 && !stop_; ++i) {
                alive("retry wait");
                serviceTransport();
                pollTimer.wait(20);
            }
        }
    }
}
} // namespace lm
