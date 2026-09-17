#include "pipeline.hpp"
#include "logging.hpp"
#include "pattern.hpp"
#include "resources.hpp"
#include <avrt.h>
#include <d3d11_1.h>
#include <fstream>
#include <iomanip>
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
// Watches for the episode users describe as "it goes extremely pixelated and glitchy, then fixes itself", and
// writes one line when it starts and one when it ends. Those are two different faults that look similar:
//
//   pixelation - the encoder was given too few bits for 1080p, so it raises the quantizer and the picture turns
//                into blocks. Seen here as a collapse in encoded bits per frame, and at the receiver as a high
//                quantizer. On this encoder a bitrate cut also means a full rebuild, which is a visible hitch.
//   glitching  - packets were lost, so the decoder shows torn and smeared blocks until a keyframe repairs it.
//                Seen as corrupted frames, freezes and keyframe requests at the receiver.
//
// Both usually end on their own, which is exactly why they need to be on record: by the time anyone looks, the
// stream is fine again. Nothing here changes behaviour; it only reports.
class QualityWatch {
    static constexpr double kBlockyQp = 36;        // Above this the receiver's own decoder calls it blocky
    static constexpr double kCollapseRatio = 0.55; // Encoded rate this far below normal is a visible drop
    static constexpr int kRecoverySeconds = 3;     // Consecutive clean seconds before an episode is over
    Samples<64> baseline_;                         // Encoded bits per second while the picture was healthy
    bool inEpisode_ = false;
    int clean_ = 0;
    Clock::time_point started_{};
    double worstQp_ = 0, lowestBitrate_ = 0;
    uint64_t corrupted_ = 0, freezes_ = 0, keyframeRequests_ = 0;
    uint32_t rebuilds_ = 0, rebuildsAtStart_ = 0;
    uint64_t keyframeRequestsAtStart_ = 0;
    std::string trigger_;

  public:
    /// Called once a second with the snapshot that was just published, plus the session's rebuild count.
    void update(const MetricsSnapshot &s, uint32_t encoderRebuilds) {
        const double encodedBitsPerSecond = s.encodeFps * s.frameBytesMean * 8;
        const bool haveBaseline = baseline_.count() >= 10;
        const double normal = baseline_.mean();
        std::string trigger;
        if (s.viewerQp && *s.viewerQp > kBlockyQp)
            trigger = "receiver quantizer " + fixed(*s.viewerQp, 0) + " (blocky)";
        else if (haveBaseline && encodedBitsPerSecond > 0 && encodedBitsPerSecond < normal * kCollapseRatio)
            trigger = "encoded rate fell to " + fixed(encodedBitsPerSecond / 1000000, 1) + " Mbps from a usual " +
                      fixed(normal / 1000000, 1) + " Mbps";
        else if (s.viewerCorrupted.value_or(0) > 0)
            trigger = fixed(*s.viewerCorrupted, 0) + " frames arrived but never decoded";
        else if (s.viewerFreezes.value_or(0) > 0)
            trigger = fixed(*s.viewerFreezes, 0) + " freezes at the receiver";
        if (trigger.empty() && encodedBitsPerSecond > 0 && !inEpisode_)
            baseline_.add(encodedBitsPerSecond); // Only healthy seconds define what normal looks like
        if (!inEpisode_ && trigger.empty())
            return;
        if (!inEpisode_) {
            inEpisode_ = true;
            clean_ = 0;
            started_ = Clock::now();
            trigger_ = trigger;
            worstQp_ = s.viewerQp.value_or(0);
            lowestBitrate_ = encodedBitsPerSecond;
            corrupted_ = freezes_ = keyframeRequests_ = 0;
            rebuilds_ = 0;
            rebuildsAtStart_ = encoderRebuilds;
            keyframeRequestsAtStart_ = s.keyframeRequests;
            logWarning("Picture quality dropped: " + trigger + " | encoder " +
                       std::to_string(s.bitrate / 1000) + " kbps (target " +
                       std::to_string(s.targetBitrate / 1000) + ") | receiver " +
                       (s.viewerFps ? fixed(*s.viewerFps, 0) : "-") + " fps, loss " +
                       (s.loss ? fixed(*s.loss * 100, 2) + "%" : "-") + ", rtt " +
                       (s.rttMs ? fixed(*s.rttMs, 0) + " ms" : "-") + " | host encode " + fixed(s.encodeFps, 0) +
                       " fps, queue " + std::to_string(s.queueDepth));
            return;
        }
        worstQp_ = std::max(worstQp_, s.viewerQp.value_or(0));
        if (encodedBitsPerSecond > 0)
            lowestBitrate_ = lowestBitrate_ > 0 ? std::min(lowestBitrate_, encodedBitsPerSecond) : encodedBitsPerSecond;
        corrupted_ += uint64_t(s.viewerCorrupted.value_or(0));
        freezes_ += uint64_t(s.viewerFreezes.value_or(0));
        rebuilds_ = encoderRebuilds - rebuildsAtStart_;
        keyframeRequests_ = s.keyframeRequests - keyframeRequestsAtStart_;
        if (!trigger.empty()) {
            clean_ = 0;
            return;
        }
        if (++clean_ < kRecoverySeconds)
            return;
        inEpisode_ = false;
        const auto seconds =
            std::chrono::duration<double>(Clock::now() - started_).count() - kRecoverySeconds;
        // Which of the two faults it was, named from what actually happened during the episode rather than from
        // the one signal that tripped it.
        std::string cause = "cause unclear";
        if (rebuilds_)
            cause = "encoder was recreated " + std::to_string(rebuilds_) +
                    " time(s) at a lower bitrate: too few bits for this resolution";
        else if (corrupted_ || freezes_ || keyframeRequests_)
            cause = "packet loss: " + std::to_string(corrupted_) + " corrupted, " + std::to_string(freezes_) +
                    " freezes, " + std::to_string(keyframeRequests_) + " keyframe requests";
        else if (worstQp_ > kBlockyQp)
            cause = "encoder raised the quantizer without a bitrate change: the desktop content got harder to "
                    "encode";
        logInfo("Picture quality recovered after " + fixed(seconds, 0) + " s (started: " + trigger_ +
                ") | worst quantizer " + (worstQp_ > 0 ? fixed(worstQp_, 0) : "-") + ", lowest encoded rate " +
                fixed(lowestBitrate_ / 1000000, 2) + " Mbps | " + cause);
    }
};
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
                for (int i = 0; i < 25 && !stop_; ++i) {
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
            if (selected.primary && !o.allowPrimary && !o.synthetic && o.mode != PipelineMode::Encode)
                throw std::runtime_error("Selected display is primary. Capture refused.");
            unsigned iw = selected.rect.right - selected.rect.left, ih = selected.rect.bottom - selected.rect.top;
            if (o.synthetic || o.mode == PipelineMode::Encode) {
                iw = 1920;
                ih = 1080;
            }
            double scale = std::min({1.0, 1920.0 / iw, 1080.0 / ih});
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
                pattern = std::make_unique<Pattern>(selected);
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
            if (o.mode == PipelineMode::Encode || o.mode == PipelineMode::CaptureEncode || stream)
                encoder = hardwareEncoder(device, selected, ow, oh, o.fps, configuredBitrate, kMaxInFlight);
            const double encoderMs = sinceBuildStart() - deviceMs - captureMs - converterMs;
            const double buildMs = sinceBuildStart();
            const auto ready = Clock::now();
            const std::string backendLabel =
                o.synthetic || o.mode == PipelineMode::Encode ? "Generated GPU source" : backendName(used);
            logInfo("Pipeline: " + backendLabel + " -> GPU NV12 -> " + (encoder ? encoder->name() : "benchmark") +
                    " | GPU " + selected.gpu + " | " + std::to_string(ow) + "x" + std::to_string(oh) + "@" +
                    std::to_string(o.fps) + " | CPU readback: no | mode " + modeName(o.mode));
            // Where the wait before the first frame actually goes. The encoder stage dominates on most machines,
            // which is why a bitrate change that recreates it is expensive rather than free.
            logInfo("Pipeline built in " + fixed(buildMs) + " ms (device " + fixed(deviceMs) + ", capture " +
                    fixed(captureMs) + ", NV12 converter " + fixed(converterMs) + ", encoder " + fixed(encoderMs) +
                    ") at " + std::to_string(configuredBitrate) + " bps");
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
            emit(EngineEventType::EncoderReady, encoder ? encoder->name() : "");
            if (stream && !transport_)
                transport_ = webRtc(o.signalingUrl, o.hostSecret, transportEvents, o.test, o.bitrate);
            {
                MetricsSnapshot s = snapshot();
                s.width = ow;
                s.height = oh;
                s.fps = o.fps;
                s.encoder = encoder ? encoder->name() : "none";
                s.gpu = selected.gpu;
                s.backend = backendLabel;
                s.bitrate = configuredBitrate;
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
            std::optional<double> firstEncodedMs, firstSentMs;
            uint64_t iterations = 0, previousIterations = 0, keyframes = 0;
            double loopMaxMs = 0, sendMaxMs = 0, frameBytesMax = 0, topologyCheckMaxMs = 0;
            std::optional<Clock::time_point> lastSubmit;
            bool sourceTimeWarned = false;
            uint32_t bitrate = configuredBitrate, attemptedBitrate = configuredBitrate;
            bool dynamicBitrate = true;
            auto report = Clock::now(), next = Clock::now(), lastInput = Clock::now(), lastFullCheck = Clock::now(),
                 lastBitrateSwitch = Clock::now();
            ComPtr<ID3D11Texture2D> synthetic;
            ComPtr<ID3D11RenderTargetView> syntheticView;
            ComPtr<ID3D11DeviceContext1> context1;
            bool keyframePending = true, haveSurface = false, repeatRequested = false;
            // Frame pacing state. Frames are taken the moment the compositor delivers them (event-driven); the
            // configured rate only thins them out, it never schedules them.
            const int64_t period100ns = 10000000 / o.fps;
            const auto period = std::chrono::nanoseconds(1000000000 / o.fps);
            int64_t lastAccepted = 0, lastSampleTime = -1;
            size_t slot = 0, lastSlot = 0;
            std::optional<Frame> carried; // Taken from the capture but not yet accepted by the encoder
            if (o.mode == PipelineMode::Encode || o.synthetic) {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = iw;
                d.Height = ih;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                d.BindFlags = D3D11_BIND_RENDER_TARGET;
                check(device.device->CreateTexture2D(&d, nullptr, &synthetic), "Benchmark BGRA source");
                check(device.device->CreateRenderTargetView(synthetic.Get(), nullptr, &syntheticView),
                      "Benchmark render target");
                float color[4]{.08f, .3f, .18f, 1};
                device.context->ClearRenderTargetView(syntheticView.Get(), color);
                check(device.context.As(&context1), "D3D11.1 generated test source");
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
                const double seconds = std::chrono::duration<double>(Clock::now() - ready).count();
                if (seconds < 1 || !captured)
                    return;
                logInfo("Pipeline session ended after " + fixed(seconds, 0) + " s: captured " +
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
                // ---- Sleep until there is something to do: a frame, encoder news, a transport message, or the
                // housekeeping deadline. Nothing here spins on a timer while connected.
                const bool active = !transport_ || transport_->connected();
                const bool busy = encoder && (encoder->pending() || carried);
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
                ++iterations;
                loop.woke(wake);
                const uint64_t encodedBefore = encoded;
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (transport_) {
                    serviceTransport();
                    if (transport_->consumeKeyframeRequest() || keyframe_.exchange(false)) {
                        keyframePending = true;
                        repeatRequested = true;
                    }
                    if (encoder && dynamicBitrate && transport_->targetBitrate() != attemptedBitrate) {
                        auto target = transport_->targetBitrate();
                        attemptedBitrate = target;
                        if (encoder->bitrate(target))
                            bitrate = target;
                        else
                            dynamicBitrate = false;
                    }
                    if (encoder && !dynamicBitrate && Clock::now() - lastEncoderChange > std::chrono::seconds(5)) {
                        const auto target = transport_->targetBitrate();
                        if (target <= bitrate * 3 / 4 || target >= bitrate + 2000000) {
                            configuredBitrate = target;
                            const auto since =
                                std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - lastEncoderChange)
                                    .count();
                            lastEncoderChange = Clock::now();
                            ++encoderRebuilds;
                            // The receiver's numbers that drove the controller to this target, so a rebuild storm
                            // can be traced back to the link rather than guessed at.
                            auto s = snapshot();
                            logInfo("Recreating hardware encoder: " + std::to_string(bitrate) + " -> " +
                                    std::to_string(target) + " bps after " + std::to_string(since) +
                                    " s (rebuild " + std::to_string(encoderRebuilds) + " this session; live "
                                    "bitrate update unsupported) | receiver loss " +
                                    (s.loss ? fixed(*s.loss * 100, 2) + "%" : "-") + ", rtt " +
                                    (s.rttMs ? fixed(*s.rttMs) + " ms" : "-") + ", jitter " +
                                    (s.jitterMs ? fixed(*s.jitterMs) + " ms" : "-") + ", estimate " +
                                    (s.receiverEstimateBps ? std::to_string(*s.receiverEstimateBps) + " bps" : "-"));
                            encoderRebuildStart = Clock::now();
                            break;
                        }
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
                    for (auto &f : encoder->poll()) {
                        ++encoded;
                        const auto done = now100ns();
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
                        if (transport_) {
                            const auto sendStart = Clock::now();
                            const bool sent = transport_->send(f);
                            const auto sendMs =
                                std::chrono::duration<double, std::milli>(Clock::now() - sendStart).count();
                            sendTimes.add(sendMs);
                            sendMaxMs = std::max(sendMaxMs, sendMs);
                            if (sent && !firstSentMs)
                                firstSentMs =
                                    std::chrono::duration<double, std::milli>(Clock::now() - ready).count();
                        }
                    }
                    if (encoder->pending() && Clock::now() - lastInput > std::chrono::seconds(3))
                        throw std::runtime_error("Encoder stalled; recreating GPU pipeline");
                }
                auto now = Clock::now();
                if (frame) {
                    fresh = true;
                    ++captured;
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
                        if (waited < -50 || waited > 5000) {
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
                        if (fd.Width != iw || fd.Height != ih)
                            throw std::runtime_error("Display dimensions changed; recreate pipeline");
                    }
                } else if (carried) {
                    frame = carried;
                    carried.reset();
                } else if (capture && active)
                    ++noChange;
                if (frame) {
                    // Timestamps come from the compositor when it provides them: evenly spaced RTP timestamps
                    // instead of ones that carry this thread's scheduling jitter.
                    const int64_t stamp = frame->presented ? frame->presented : frame->timestamp;
                    if (fresh && o.fps < 60 && lastAccepted && stamp - lastAccepted < period100ns * 3 / 4) {
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
                        if (o.synthetic) {
                            float background[4]{.04f, .08f, .12f, 1}, foreground[4]{.3f, .85f, .6f, 1};
                            device.context->ClearRenderTargetView(syntheticView.Get(), background);
                            LONG x = LONG((captured * 12) % (iw - 100));
                            D3D11_RECT rect{x, 0, x + 100, LONG(ih)};
                            context1->ClearView(syntheticView.Get(), foreground, &rect, 1);
                        }
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
                                converter->convert(frame->texture.Get(), slot);
                                if (o.flushGpu)
                                    device.context->Flush();
                                conversionTimes.add(
                                    std::chrono::duration<double, std::milli>(Clock::now() - t).count());
                            }
                            lastAccepted = stamp;
                            haveSurface = true;
                            if (encoder) {
                                const int64_t sampleTime = std::max(stamp - epoch, lastSampleTime + 1);
                                if (!encoder->submit(nv12, sampleTime, frame->presented)) {
                                    ++dropped;
                                    ++droppedSubmitFailed;
                                } else {
                                    lastSampleTime = sampleTime;
                                    lastSlot = o.mode == PipelineMode::Encode ? 0 : slot;
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
                        const int64_t sampleTime = std::max(now100ns() - epoch, lastSampleTime + 1);
                        if (encoder->submit(last, sampleTime, 0)) {
                            lastSampleTime = sampleTime;
                            lastInput = now;
                            repeatRequested = false;
                        }
                    }
                }
                if (now - report >= std::chrono::seconds(1)) {
                    const auto interval = std::chrono::duration<double>(now - report).count();
                    auto elapsed = std::chrono::duration<double>(now - start).count();
                    MetricsSnapshot s;
                    s.captureFps = (captured - previousCaptured) / interval;
                    s.encodeFps = (encoded - previousEncoded) / interval;
                    s.captured = captured;
                    s.encoded = encoded;
                    s.dropped = dropped;
                    s.noChange = noChange;
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
                    s.targetBitrate = transport_ ? transport_->targetBitrate() : bitrate;
                    s.dynamicBitrate = dynamicBitrate;
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
                    s.fps = o.fps;
                    s.encoder = encoder ? encoder->name() : "none";
                    s.gpu = selected.gpu;
                    s.backend = backendLabel;
                    s.updated = now;
                    if (transport_)
                        transport_->fillMetrics(s);
                    publish(s);
                    if (stream)
                        quality.update(s, encoderRebuilds);
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
                    if (converter && converter->gpuTimes().count()) {
                        // This asynchronous GPU command span includes driver submission delay.
                        // It is not a pure video-processor execution duration.
                        stats["gpu_command_span_ms_mean"] = converter->gpuTimes().mean();
                    }
                    if (transport_) {
                        stats["webrtc"] = transport_->stats();
                        transport_->diagnostics(stats);
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
                            std::to_string(s.targetBitrate / 1000) + ") | frame " +
                            std::to_string(uint64_t(s.frameBytesMean)) + " B, keyframe " +
                            std::to_string(uint64_t(s.keyframeBytesMean)) + " B";
                        logInfo(line);
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
                                    receiver(s.jitterMs, " ms jitter") + " | quantizer " +
                                    receiver(s.viewerQp, "", 0) + " | corrupted " +
                                    receiver(s.viewerCorrupted, "", 0) + " | freezes " +
                                    receiver(s.viewerFreezes, "", 0) + " | jitter buffer " +
                                    receiver(s.viewerJitterBufferMs, " ms") + " | decode " +
                                    receiver(s.viewerDecodeMs, " ms") + " | keyframe requests " +
                                    std::to_string(s.keyframeRequests) + " | transport buffer " +
                                    std::to_string(s.bufferBytes / 1024) + " KB | send dropped " +
                                    std::to_string(s.transportDropped));
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
                    report = now;
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
            if (!stream)
                throw;
            emit(EngineEventType::Error, e.what());
            logInfo("Retrying the selected display in 1 second");
            for (int i = 0; i < 50 && !stop_; ++i) {
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
            if (!stream)
                throw std::runtime_error(what);
            emit(EngineEventType::Error, what);
            for (int i = 0; i < 50 && !stop_; ++i) {
                serviceTransport();
                pollTimer.wait(20);
            }
        }
    }
}
} // namespace lm
