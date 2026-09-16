#include "pipeline.hpp"
#include "logging.hpp"
#include "pattern.hpp"
#include <avrt.h>
#include <d3d11_1.h>
#include <fstream>
#include <winrt/base.h>
namespace bm {
namespace {
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
class CpuMeter {
    uint64_t previous_ = 0;
    Clock::time_point time_ = Clock::now();

  public:
    std::optional<double> sample() {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
            return std::nullopt;
        auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        const auto total = ticks(kernel) + ticks(user);
        const auto now = Clock::now();
        const auto seconds = std::chrono::duration<double>(now - time_).count();
        std::optional<double> result;
        if (previous_ && seconds > 0)
            result = (total - previous_) / 1e7 / seconds / GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) * 100;
        previous_ = total;
        time_ = now;
        return result;
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
nlohmann::json optionalNumber(const std::optional<double> &v) {
    return v ? nlohmann::json(*v) : nlohmann::json(nullptr);
}
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
    CpuMeter cpu;
    cpu.sample();
    RealtimeScope realtime;
    logInfo(std::string("Streaming engine thread scheduling: ") + realtime.description());
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
            Device device(selected);
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
            std::unique_ptr<Converter> converter;
            std::unique_ptr<IEncoder> encoder;
            if (o.mode != PipelineMode::Capture)
                converter = std::make_unique<Converter>(device, iw, ih, ow, oh);
            if (o.mode == PipelineMode::Encode || o.mode == PipelineMode::CaptureEncode || stream)
                encoder = hardwareEncoder(device, selected, ow, oh, o.fps, configuredBitrate, kMaxInFlight);
            const std::string backendLabel =
                o.synthetic || o.mode == PipelineMode::Encode ? "Generated GPU source" : backendName(used);
            logInfo("Pipeline: " + backendLabel + " -> GPU NV12 -> " + (encoder ? encoder->name() : "benchmark") +
                    " | GPU " + selected.gpu + " | " + std::to_string(ow) + "x" + std::to_string(oh) + "@" +
                    std::to_string(o.fps) + " | CPU readback: no | mode " + modeName(o.mode));
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
            Samples<> capTimes, conversionTimes, encodeTimes, pipelineTimes, sizes, acquireDelays, sourceToEncoded,
                sendTimes, submitIntervals;
            uint64_t iterations = 0, previousIterations = 0, keyframes = 0;
            double loopMaxMs = 0, sendMaxMs = 0, frameBytesMax = 0;
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
            while (!stop_ && !timeUp()) {
                // ---- Sleep until there is something to do: a frame, encoder news, a transport message, or the
                // housekeeping deadline. Nothing here spins on a timer while connected.
                const bool active = !transport_ || transport_->connected();
                const bool busy = encoder && (encoder->pending() || carried);
                std::optional<Frame> frame;
                bool fresh = false;
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
                } else {
                    HANDLE handles[3];
                    DWORD n = 0;
                    if (capture && active)
                        handles[n++] = capture->frameEvent();
                    if (encoder)
                        handles[n++] = encoder->event();
                    if (transport_)
                        handles[n++] = transport_->wakeEvent();
                    const DWORD timeout = active ? 50 : 20;
                    const DWORD r = n ? WaitForMultipleObjects(n, handles, FALSE, timeout) : WAIT_TIMEOUT;
                    if (!n)
                        pollTimer.wait(timeout);
                    const bool frameSignalled = capture && active && r == WAIT_OBJECT_0;
                    // A carried frame is only replaced once a newer one has actually arrived.
                    if (capture && active && (!carried || frameSignalled))
                        frame = capture->acquire(0);
                }
                const auto iterationStart = Clock::now();
                ++iterations;
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
                            lastEncoderChange = Clock::now();
                            logInfo("Recreating hardware encoder at " + std::to_string(target) +
                                    " bps (live bitrate update unsupported)");
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
                        if (f.keyframe)
                            ++keyframes;
                        if (transport_) {
                            const auto sendStart = Clock::now();
                            transport_->send(f);
                            const auto sendMs =
                                std::chrono::duration<double, std::milli>(Clock::now() - sendStart).count();
                            sendTimes.add(sendMs);
                            sendMaxMs = std::max(sendMaxMs, sendMs);
                        }
                    }
                    if (encoder->pending() && Clock::now() - lastInput > std::chrono::seconds(3))
                        throw std::runtime_error("Encoder stalled; recreating GPU pipeline");
                }
                auto now = Clock::now();
                if (frame) {
                    fresh = true;
                    ++captured;
                    if (frame->accumulated > 1)
                        dropped += frame->accumulated - 1;
                    if (carried) {
                        // Superseded by a newer frame before the encoder could take it.
                        ++dropped;
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
                                if (!encoder->submit(nv12, sampleTime, frame->presented))
                                    ++dropped;
                                else {
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
                    s.loopWakeupsPerSecond = (iterations - previousIterations) / interval;
                    s.loopMaxMs = loopMaxMs;
                    s.sendMsMean = sendTimes.mean();
                    s.sendMsMax = sendMaxMs;
                    s.submitIntervalMsP95 = submitIntervals.percentile(.95);
                    s.submitIntervalMsMax = submitIntervals.count() ? submitIntervals.percentile(1.0) : 0;
                    s.frameBytesMax = frameBytesMax;
                    s.keyframes = keyframes;
                    s.frameBytesMean = sizes.mean();
                    s.bitrate = bitrate;
                    s.targetBitrate = transport_ ? transport_->targetBitrate() : bitrate;
                    s.dynamicBitrate = dynamicBitrate;
                    s.queueDepth = encoder ? encoder->pending() : 0;
                    s.cpuPercent = cpu.sample();
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
                                            {"pattern_presented", pattern ? pattern->presented() : 0}};
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
                    previousCaptured = captured;
                    previousEncoded = encoded;
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
                        auto current = displays();
                        auto found = std::find_if(current.begin(), current.end(),
                                                  [&](auto &d) { return d.name == selected.name; });
                        if (found == current.end() || found->monitor != selected.monitor ||
                            found->primary != selected.primary ||
                            memcmp(&found->rect, &selected.rect, sizeof(RECT)) ||
                            memcmp(&found->luid, &selected.luid, sizeof(LUID)))
                            throw std::runtime_error("Display topology changed; revalidate selection");
                    }
                }
                loopMaxMs = std::max(
                    loopMaxMs, std::chrono::duration<double, std::milli>(Clock::now() - iterationStart).count());
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
} // namespace bm
