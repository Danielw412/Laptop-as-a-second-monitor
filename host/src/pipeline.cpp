#include "pipeline.hpp"
#include "logging.hpp"
#include "pattern.hpp"
#include <d3d11_1.h>
#include <fstream>
#include <winrt/base.h>
namespace lm {
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
               "queue_depth\n";
    }
    auto start = Clock::now();
    const int64_t epoch = now100ns();
    PollTimer pollTimer;
    CpuMeter cpu;
    cpu.sample();
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
                pattern = std::make_unique<Pattern>(device, selected);
            std::unique_ptr<ICapture> capture;
            CaptureBackend used = resolved_;
            if (!o.synthetic && o.mode != PipelineMode::Encode) {
                if (used == CaptureBackend::Auto)
                    used = o.backend;
                if (used == CaptureBackend::Auto) {
                    // Auto prefers Windows Graphics Capture (no merged-frame drops in the baseline runs) and falls
                    // back to desktop duplication where it is unavailable.
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
                encoder = hardwareEncoder(device, selected, ow, oh, o.fps, configuredBitrate);
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
            uint64_t captured = 0, encoded = 0, dropped = 0, noChange = 0, previousCaptured = 0,
                     previousEncoded = 0;
            Samples<> capTimes, conversionTimes, encodeTimes, pipelineTimes, sizes;
            uint32_t bitrate = configuredBitrate, attemptedBitrate = configuredBitrate;
            bool dynamicBitrate = true;
            auto report = Clock::now(), next = Clock::now(), lastInput = Clock::now();
            ComPtr<ID3D11Texture2D> synthetic;
            ComPtr<ID3D11RenderTargetView> syntheticView;
            ComPtr<ID3D11DeviceContext1> context1;
            bool keyframePending = true, haveSurface = false, repeatRequested = false;
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
                if (encoder) {
                    if (keyframePending) {
                        encoder->keyframe();
                        keyframePending = false;
                    }
                    for (auto &f : encoder->poll()) {
                        ++encoded;
                        encodeTimes.add(f.latencyMs);
                        pipelineTimes.add((now100ns() - epoch - f.timestamp) / 10000.0);
                        sizes.add(double(f.bytes.size()));
                        if (transport_)
                            transport_->send(f);
                    }
                    if (encoder->pending() && Clock::now() - lastInput > std::chrono::seconds(3))
                        throw std::runtime_error("Encoder stalled; recreating GPU pipeline");
                }
                auto now = Clock::now();
                if (now >= next) {
                    if (pattern)
                        pattern->draw();
                    next += std::chrono::nanoseconds(1000000000 / o.fps);
                    if (next < now)
                        next = now + std::chrono::nanoseconds(1000000000 / o.fps);
                    bool active = !transport_ || transport_->connected();
                    if (active) {
                        // Do not consume the first unchanged desktop frame before the encoder can accept it.
                        if (encoder && !encoder->ready()) {
                            pollTimer.wait();
                            continue;
                        }
                        auto begin = Clock::now();
                        std::optional<Frame> frame;
                        if (synthetic)
                            frame = Frame{synthetic, now100ns(), 1};
                        else
                            frame = capture->acquire();
                        capTimes.add(std::chrono::duration<double, std::milli>(Clock::now() - begin).count());
                        if (frame) {
                            ++captured;
                            if (frame->accumulated > 1)
                                dropped += frame->accumulated - 1;
                            D3D11_TEXTURE2D_DESC fd{};
                            frame->texture->GetDesc(&fd);
                            if (fd.Width != iw || fd.Height != ih)
                                throw std::runtime_error("Display dimensions changed; recreate pipeline");
                            if (converter) {
                                if (encoder && !encoder->ready())
                                    ++dropped;
                                else {
                                    auto t = Clock::now();
                                    if (o.synthetic) {
                                        float background[4]{.04f, .08f, .12f, 1}, foreground[4]{.3f, .85f, .6f, 1};
                                        device.context->ClearRenderTargetView(syntheticView.Get(), background);
                                        LONG x = LONG((captured * 12) % (iw - 100));
                                        D3D11_RECT rect{x, 0, x + 100, LONG(ih)};
                                        context1->ClearView(syntheticView.Get(), foreground, &rect, 1);
                                    }
                                    auto nv12 = o.mode == PipelineMode::Encode
                                                    ? converter->texture(0)
                                                    : converter->convert(frame->texture.Get(), 0);
                                    if (o.flushGpu)
                                        device.context->Flush();
                                    if (o.mode != PipelineMode::Encode)
                                        conversionTimes.add(
                                            std::chrono::duration<double, std::milli>(Clock::now() - t).count());
                                    haveSurface = true;
                                    if (encoder) {
                                        if (!encoder->submit(nv12, frame->timestamp - epoch))
                                            ++dropped;
                                        else {
                                            lastInput = now;
                                            repeatRequested = false;
                                        }
                                    }
                                }
                            }
                        } else {
                            ++noChange;
                            if (encoder && haveSurface && encoder->ready() &&
                                (repeatRequested || now - lastInput > std::chrono::seconds(1))) {
                                if (encoder->submit(converter->texture(0), now100ns() - epoch)) {
                                    lastInput = now;
                                    repeatRequested = false;
                                }
                            }
                        }
                        if (capture)
                            capture->release();
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
                                            {"pipeline_ms_p95", optionalNumber(s.pipelineMsP95)}};
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
                    if (csv)
                        csv << elapsed << ',' << captured << ',' << encoded << ',' << dropped << ',' << noChange
                            << ',' << capTimes.mean() << ',' << capTimes.percentile(.95) << ','
                            << capTimes.percentile(.99) << ',' << conversionTimes.mean() << ','
                            << encodeTimes.mean() << ',' << encodeTimes.percentile(.95) << ','
                            << encodeTimes.percentile(.99) << ',' << sizes.mean() << ',' << bitrate << ','
                            << (encoder ? encoder->pending() : 0) << '\n';
                    report = now;
                    auto current = displays();
                    auto found = std::find_if(current.begin(), current.end(),
                                              [&](auto &d) { return d.name == selected.name; });
                    if (found == current.end() || found->monitor != selected.monitor ||
                        found->primary != selected.primary ||
                        memcmp(&found->rect, &selected.rect, sizeof(RECT)) ||
                        memcmp(&found->luid, &selected.luid, sizeof(LUID)))
                        throw std::runtime_error("Display topology changed; revalidate selection");
                }
                // Poll async output promptly; do not wait a full frame period to transmit it.
                pollTimer.wait(transport_ && !transport_->connected() ? 20 : 1);
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
