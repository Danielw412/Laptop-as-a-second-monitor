#include "platform.hpp"
#include "pattern.hpp"
#include "transport.hpp"
#include "settings.hpp"
#include <atomic>
#include <bcrypt.h>
#include <d3d10.h>
#include <d3d11_1.h>
#include <fstream>
#include <thread>
#include <winrt/base.h>
#include <shellapi.h>
namespace lm {
std::atomic<bool> running = true;
NOTIFYICONDATAW tray{};
BOOL WINAPI control(DWORD) {
    running = false;
    return TRUE;
}
std::string randomString(size_t length, bool room) {
    std::vector<UCHAR> bytes(length);
    check(BCryptGenRandom(nullptr, bytes.data(), ULONG(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG),
          "Secure pairing RNG");
    std::string s;
    const char *alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const char *hex = "0123456789abcdef";
    for (auto b : bytes)
        if (room)
            s += alphabet[b % 32];
        else {
            s += hex[b >> 4];
            s += hex[b & 15];
        }
    return s;
}
LRESULT CALLBACK pairingProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_CLOSE) { ShowWindow(window, SW_HIDE); return 0; }
    if (message == WM_APP + 1) {
        if (l == WM_LBUTTONDBLCLK) { ShowWindow(window, SW_SHOW); SetForegroundWindow(window); }
        if (l == WM_RBUTTONUP) {
            auto menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Show pairing");
            AppendMenuW(menu, MF_STRING, 2, L"Exit Laptop Monitor");
            POINT point; GetCursorPos(&point); SetForegroundWindow(window);
            auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (command == 1) ShowWindow(window, SW_SHOW);
            if (command == 2) DestroyWindow(window);
        }
        return 0;
    }
    if (message == WM_DESTROY) {
        Shell_NotifyIconW(NIM_DELETE, &tray);
        running = false;
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void pairingWindow(const std::string &room, const std::string &secret, bool background) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = pairingProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"LaptopMonitorPairing";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    auto window =
        CreateWindowExW(0, wc.lpszClassName, L"Laptop Monitor — pairing", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, 720, 230, nullptr, nullptr, wc.hInstance, nullptr);
    std::string text = "Room: " + room + "\r\n\r\nSession secret (select and copy into the receiver):\r\n" +
                       secret +
                       "\r\n\r\nKeep this secret private. It is never written to diagnostics or logs.";
    std::wstring wide(text.begin(), text.end());
    auto edit = CreateWindowExW(0, L"EDIT", wide.c_str(), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                                16, 16, 670, 145, window, nullptr, wc.hInstance, nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    tray.cbSize = sizeof(tray); tray.hWnd = window; tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; tray.uCallbackMessage = WM_APP + 1;
    tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"Laptop Monitor — right-click for pairing or exit");
    Shell_NotifyIconW(NIM_ADD, &tray);
    ShowWindow(window, background ? SW_HIDE : SW_SHOW);
}
struct Options {
    std::string display, identity, backend = "dxgi", mode = "stream", server, csv;
    unsigned fps = 60, seconds = 0, width = 1920, height = 1080;
    bool list = false, allowPrimary = false, pairingStdin = false, pattern = false, synthetic = false;
    bool remember = false, background = false, newPairing = false;
    bool flushGpu = false;
    TransportTestOptions test;
};
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
    nlohmann::json sample() {
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return nullptr;
        auto ticks = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        const auto total = ticks(kernel) + ticks(user);
        const auto now = Clock::now();
        const auto seconds = std::chrono::duration<double>(now-time_).count();
        nlohmann::json result = nullptr;
        if (previous_ && seconds > 0) result = (total-previous_) / 1e7 / seconds / GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) * 100;
        previous_ = total; time_ = now;
        return result;
    }
};
Options parse(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto value = [&]() {
            if (i + 1 >= argc)
                throw std::runtime_error("Missing option value");
            return std::string(argv[++i]);
        };
        if (a == "--list")
            o.list = true;
        else if (a == "--pairing-stdin")
            o.pairingStdin = true;
        else if (a == "--pattern")
            o.pattern = true;
        else if (a == "--synthetic")
            o.synthetic = true;
        else if (a == "--remember") o.remember = true;
        else if (a == "--background") o.background = true;
        else if (a == "--new-pairing") o.newPairing = true;
        else if (a == "--test-drop-every") o.test.dropEvery = std::stoul(value());
        else if (a == "--test-drop-first-keyframe") o.test.dropFirstKeyframe = true;
        else if (a == "--test-block-ice") o.test.blockIce = true;
        else if (a == "--flush-gpu") o.flushGpu = true;
        else if (a == "--display")
            o.display = value();
        else if (a == "--capture")
            o.backend = value();
        else if (a == "--mode")
            o.mode = value();
        else if (a == "--signaling")
            o.server = value();
        else if (a == "--csv")
            o.csv = value();
        else if (a == "--fps")
            o.fps = std::stoul(value());
        else if (a == "--seconds")
            o.seconds = std::stoul(value());
        else if (a == "--allow-primary")
            o.allowPrimary = true;
        else
            throw std::runtime_error("Unknown option: " + a);
    }
    if (o.fps < 1 || o.fps > 60)
        throw std::runtime_error("FPS must be 1..60");
    if (o.backend != "dxgi" && o.backend != "wgc")
        throw std::runtime_error("Capture must be dxgi or wgc");
    if (o.mode != "capture" && o.mode != "convert" && o.mode != "encode" && o.mode != "capture-encode" &&
        o.mode != "stream")
        throw std::runtime_error("Unknown benchmark mode");
    return o;
}
int run(int argc, char **argv) {
    auto o = parse(argc, argv);
    nlohmann::json saved = nlohmann::json::object();
    if (o.mode == "stream" && !o.pairingStdin && !o.list) {
        saved = loadSettings();
        if (o.display.empty()) { o.display = saved.value("display", ""); o.identity = saved.value("identity", ""); }
        if (o.server.empty()) o.server = saved.value("server", "");
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    check(MFStartup(MF_VERSION), "Media Foundation startup");
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(control, TRUE);
    auto outputs = displays();
    for (const auto &d : outputs)
        std::cout << d.name << " | " << d.gpu << " | " << d.rect.right - d.rect.left << 'x'
                  << d.rect.bottom - d.rect.top << (d.primary ? " | PRIMARY" : " | secondary") << '\n';
    if (o.list) {
        MFShutdown();
        return 0;
    }
    if (o.display.empty())
        throw std::runtime_error("Explicit --display is required. Run --list first.");
    if (o.mode == "stream" && o.server.empty())
        throw std::runtime_error("--signaling https://your-worker.workers.dev is required");
    std::unique_ptr<ITransport> transport;
    if (o.mode == "stream") {
        auto room = randomString(8, true), secret = randomString(32, false);
        if (!o.newPairing && saved.contains("room") && saved.contains("secret")) {
            room = saved.at("room").get<std::string>(); secret = saved.at("secret").get<std::string>();
        }
        if (o.pairingStdin) {
            std::string line;
            char ch;
            while (std::cin.get(ch) && ch != '\n' && line.size() <= 256) line += ch;
            auto pairing = nlohmann::json::parse(line, nullptr, false);
            if (line.size() > 256 || !pairing.is_object() || !pairing.contains("room") || !pairing["room"].is_string() || !pairing.contains("secret") || !pairing["secret"].is_string())
                throw std::runtime_error("Invalid pairing input");
            room = pairing.at("room");
            secret = pairing.at("secret");
        }
            if (room.size() != 8 ||
                room.find_first_not_of("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") != std::string::npos ||
                secret.size() != 64 || secret.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Invalid pairing input");
        if (o.remember) {
            auto selected = std::find_if(outputs.begin(), outputs.end(), [&](const auto &d) { return d.name == o.display; });
            if (selected == outputs.end() || selected->primary || o.synthetic)
                throw std::runtime_error("--remember requires a connected secondary display; generated sources and primary displays cannot be saved.");
            o.identity = displayIdentity(*selected);
            if (o.identity.empty()) throw std::runtime_error("Cannot obtain a stable display identity; settings were not saved");
            saveSettings({{"display",o.display},{"identity",o.identity},{"server",o.server},{"room",room},{"secret",secret}});
        }
        if (!o.pairingStdin) pairingWindow(room, secret, o.background);
        if (o.background) ShowWindow(GetConsoleWindow(), SW_HIDE);
        std::cout << "Pairing room: " << room << " (secret excluded from logs)\n";
        transport = webRtc(o.server, room, secret, o.test);
    }
    std::ofstream csv;
    if (!o.csv.empty()) {
        csv.open(o.csv);
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
    uint32_t configuredBitrate = 8000000;
    auto lastEncoderChange = Clock::now();
    while (running && (!o.seconds || Clock::now() - start < std::chrono::seconds(o.seconds))) {
        try {
            auto all = displays();
            auto it = std::find_if(all.begin(), all.end(), [&](auto &d) { return o.identity.empty() ? d.name == o.display : displayIdentity(d) == o.identity; });
            if (it == all.end())
                throw std::runtime_error("Selected display disconnected; waiting for the same display identity");
            const auto selected = *it;
            if (selected.primary && !o.allowPrimary && !o.synthetic && o.mode != "encode")
                throw std::runtime_error("Selected display is primary. Capture refused; --allow-primary is "
                                         "required to explicitly authorize it.");
            unsigned iw = selected.rect.right - selected.rect.left,
                     ih = selected.rect.bottom - selected.rect.top;
            if (o.synthetic || o.mode == "encode") { iw = 1920; ih = 1080; }
            double scale = std::min({1.0, 1920.0 / iw, 1080.0 / ih});
            unsigned ow = unsigned(iw * scale) & ~1u, oh = unsigned(ih * scale) & ~1u;
            Device device(selected);
            std::unique_ptr<Pattern> pattern;
            if(o.pattern)pattern=std::make_unique<Pattern>(device,selected);
            std::unique_ptr<ICapture> capture;
            if (!o.synthetic && o.mode != "encode")
                capture = o.backend == "wgc" ? wgc(device, selected) : duplication(device, selected);
            std::unique_ptr<Converter> converter;
            std::unique_ptr<IEncoder> encoder;
            if (o.mode != "capture")
                converter = std::make_unique<Converter>(device, iw, ih, ow, oh);
            if (o.mode == "encode" || o.mode == "capture-encode" || o.mode == "stream")
                encoder = hardwareEncoder(device, selected, ow, oh, o.fps, configuredBitrate);
            std::cout << "Pipeline: " << o.backend << " -> GPU NV12 -> "
                      << (encoder ? encoder->name() : "benchmark") << " | GPU " << selected.gpu << " | " << ow
                      << 'x' << oh << '@' << o.fps << " | CPU readback: no\n";
            uint64_t captured = 0, encoded = 0, dropped = 0, noChange = 0, previousCaptured = 0,
                     previousEncoded = 0;
            Samples<> capTimes, conversionTimes, encodeTimes, pipelineTimes, sizes;
            uint32_t bitrate = configuredBitrate, attemptedBitrate = configuredBitrate;
            bool dynamicBitrate = true;
            auto report = Clock::now(), next = Clock::now(),
                 lastInput = Clock::now();
            ComPtr<ID3D11Texture2D> synthetic;
            ComPtr<ID3D11RenderTargetView> syntheticView;
            ComPtr<ID3D11DeviceContext1> context1;
            bool keyframePending = true, haveSurface = false, repeatRequested = false;
            if (o.mode == "encode" || o.synthetic) {
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
                if (o.mode == "encode") { converter->convert(synthetic.Get(), 0); device.context->Flush(); }
            }
            while (running && (!o.seconds || Clock::now() - start < std::chrono::seconds(o.seconds))) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                if (transport) {
                    transport->poll();
                    if (transport->consumeKeyframeRequest()) {
                        keyframePending = true;
                        repeatRequested = true;
                    }
                    if (encoder && dynamicBitrate && transport->targetBitrate() != attemptedBitrate) {
                        auto target = transport->targetBitrate();
                        attemptedBitrate = target;
                        if (encoder->bitrate(target))
                            bitrate = target;
                        else dynamicBitrate = false;
                    }
                    if (encoder && !dynamicBitrate && Clock::now()-lastEncoderChange > std::chrono::seconds(5)) {
                        const auto target = transport->targetBitrate();
                        if (target <= bitrate * 3 / 4 || target >= bitrate + 2000000) {
                            configuredBitrate = target;
                            lastEncoderChange = Clock::now();
                            std::cout << "Recreating hardware encoder at " << target << " bps (live bitrate update unsupported)\n";
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
                        pipelineTimes.add((now100ns()-epoch-f.timestamp)/10000.0);
                        sizes.add(double(f.bytes.size()));
                        if (transport)
                            transport->send(f);
                    }
                    if (encoder->pending() && Clock::now() - lastInput > std::chrono::seconds(3))
                        throw std::runtime_error("Encoder stalled; recreating GPU pipeline");
                }
                auto now = Clock::now();
                if (now >= next) {
                    if(pattern)pattern->draw();
                    next += std::chrono::nanoseconds(1000000000 / o.fps);
                    if (next < now)
                        next = now + std::chrono::nanoseconds(1000000000 / o.fps);
                    bool active = !transport || transport->connected();
                    if (active) {
                        // Do not consume the first unchanged desktop frame before the encoder can accept it.
                        if (encoder && !encoder->ready()) { pollTimer.wait(); continue; }
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
                                        float background[4]{.04f,.08f,.12f,1}, foreground[4]{.3f,.85f,.6f,1};
                                        device.context->ClearRenderTargetView(syntheticView.Get(), background);
                                        LONG x = LONG((captured * 12) % (iw - 100));
                                        D3D11_RECT rect{x,0,x+100,LONG(ih)};
                                        context1->ClearView(syntheticView.Get(), foreground, &rect, 1);
                                    }
                                    auto nv12 = o.mode == "encode" ? converter->texture(0)
                                                          : converter->convert(frame->texture.Get(), 0);
                                    if (o.flushGpu) device.context->Flush();
                                    if (o.mode != "encode")
                                        conversionTimes.add(
                                            std::chrono::duration<double, std::milli>(Clock::now() - t)
                                                .count());
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
                        if (capture) capture->release();
                    }
                }
                if (now - report >= std::chrono::seconds(1)) {
                    auto elapsed = std::chrono::duration<double>(now - start).count();
                    nlohmann::json stats = {{"seconds", elapsed},
                                            {"captured", captured},
                                            {"encoded", encoded},
                                            {"dropped", dropped},
                                            {"no_change", noChange},
                                            {"capture_ms_mean", capTimes.mean()},
                                            {"capture_ms_p95", capTimes.percentile(.95)},
                                            {"capture_ms_p99", capTimes.percentile(.99)},
                                            {"convert_submit_ms_mean", conversionTimes.mean()},
                                            {"encode_ms_mean", encodeTimes.mean()},
                                            {"encode_ms_p95", encodeTimes.percentile(.95)},
                                            {"encode_ms_p99", encodeTimes.percentile(.99)},
                                            {"frame_bytes_mean", sizes.mean()},
                                            {"bitrate", bitrate},
                                            {"queue_depth", encoder ? encoder->pending() : 0}};
                    if (transport)
                        stats["webrtc"] = transport->stats();
                    stats["type"] = "host-stats";
                    stats["capture_fps"] = (captured - previousCaptured) / std::chrono::duration<double>(now-report).count();
                    stats["encode_fps"] = (encoded - previousEncoded) / std::chrono::duration<double>(now-report).count();
                    stats["capture_backend"] = o.synthetic || o.mode == "encode" ? "Generated GPU source" : o.backend;
                    stats["encoder"] = encoder ? encoder->name() : "none";
                    stats["gpu"] = selected.gpu;
                    stats["video_path"] = "GPU";
                    stats["hardware_encoder"] = bool(encoder);
                    stats["cpu_percent"] = cpu.sample();
                    stats["dynamic_bitrate"] = dynamicBitrate;
                    stats["target_bitrate"] = transport ? transport->targetBitrate() : bitrate;
                    stats["flush_gpu"] = o.flushGpu;
                    stats["pipeline_ms_mean"] = pipelineTimes.count() ? nlohmann::json(pipelineTimes.mean()) : nlohmann::json(nullptr);
                    stats["pipeline_ms_p95"] = pipelineTimes.count() ? nlohmann::json(pipelineTimes.percentile(.95)) : nlohmann::json(nullptr);
                    if (!encodeTimes.count()) { stats["encode_ms_mean"] = nullptr; stats["encode_ms_p95"] = nullptr; stats["encode_ms_p99"] = nullptr; }
                    if (converter && converter->gpuTimes().count()) {
                        // This asynchronous GPU command span includes driver submission delay.
                        // It is not a pure video-processor execution duration.
                        stats["gpu_command_span_ms_mean"] = converter->gpuTimes().mean();
                    }
                    if (transport) transport->diagnostics(stats);
                    std::cout << stats.dump() << '\n';
                    std::cout << "Interval FPS capture="
                              << (captured - previousCaptured) /
                                     std::chrono::duration<double>(now - report).count()
                              << " encode="
                              << (encoded - previousEncoded) /
                                     std::chrono::duration<double>(now - report).count();
                    if (converter)
                        std::cout << " | GPU conversion ms mean=" << converter->gpuTimes().mean()
                                  << " p95=" << converter->gpuTimes().percentile(.95)
                                  << " p99=" << converter->gpuTimes().percentile(.99);
                    std::cout << '\n';
                    previousCaptured = captured;
                    previousEncoded = encoded;
                    if (csv)
                        csv << elapsed << ',' << captured << ',' << encoded << ',' << dropped << ','
                            << noChange << ',' << capTimes.mean() << ',' << capTimes.percentile(.95) << ','
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
                pollTimer.wait(transport && !transport->connected() ? 20 : 1);
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            if (o.mode != "stream")
                throw;
            std::cerr << "Retrying the explicitly selected display in 1 second\n";
            for (int i = 0; i < 100 && running; ++i) {
                MSG message;
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
                if (transport)
                    transport->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    transport.reset();
    if (tray.hWnd) Shell_NotifyIconW(NIM_DELETE, &tray);
    MFShutdown();
    return 0;
}
} // namespace lm
int main(int argc, char **argv) {
    try {
        return lm::run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "Laptop Monitor: " << e.what()
                  << "\nUsage: laptop-monitor --list\n  --display \\\\.\\DISPLAY2 --signaling "
                     "https://worker.workers.dev\n  --mode capture|convert|encode|capture-encode|stream "
                     "--capture dxgi|wgc --fps 60 --seconds 30 --csv results.csv\n";
        return 1;
    }
}
