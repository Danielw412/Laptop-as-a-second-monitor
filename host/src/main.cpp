#include "platform.hpp"
#include "pattern.hpp"
#include "transport.hpp"
#include <atomic>
#include <bcrypt.h>
#include <d3d10.h>
#include <fstream>
#include <thread>
#include <winrt/base.h>
namespace bm {
std::atomic<bool> running = true;
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
    if (message == WM_DESTROY) {
        return 0;
    }
    return DefWindowProcW(window, message, w, l);
}
void pairingWindow(const std::string &room, const std::string &secret) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = pairingProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BrowserMonitorPairing";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassW(&wc);
    auto window =
        CreateWindowExW(0, wc.lpszClassName, L"Browser Monitor — pairing", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                        CW_USEDEFAULT, 720, 230, nullptr, nullptr, wc.hInstance, nullptr);
    std::string text = "Room: " + room + "\r\n\r\nSession secret (select and copy into the receiver):\r\n" +
                       secret +
                       "\r\n\r\nKeep this secret private. It is never written to diagnostics or logs.";
    std::wstring wide(text.begin(), text.end());
    auto edit = CreateWindowExW(0, L"EDIT", wide.c_str(), WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY,
                                16, 16, 670, 145, window, nullptr, wc.hInstance, nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    ShowWindow(window, SW_SHOW);
}
struct Options {
    std::string display, backend = "dxgi", mode = "stream", server, csv;
    unsigned fps = 60, seconds = 0, width = 1920, height = 1080;
    bool list = false, allowPrimary = false, pairingStdin = false, pattern = false;
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
    void wait() {
        LARGE_INTEGER due;
        due.QuadPart = -10000;
        SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer_, 100);
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
    const auto o = parse(argc, argv);
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
        if (o.pairingStdin) {
            std::string line;
            std::getline(std::cin, line);
            if (line.size() > 256)
                throw std::runtime_error("Pairing input too long");
            auto pairing = nlohmann::json::parse(line);
            room = pairing.at("room");
            secret = pairing.at("secret");
            if (room.size() != 8 ||
                room.find_first_not_of("ABCDEFGHJKLMNPQRSTUVWXYZ23456789") != std::string::npos ||
                secret.size() != 64 || secret.find_first_not_of("0123456789abcdef") != std::string::npos)
                throw std::runtime_error("Invalid pairing input");
        } else
            pairingWindow(room, secret);
        std::cout << "Pairing room: " << room << " (secret excluded from logs)\n";
        transport = webRtc(o.server, room, secret);
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
    unsigned recoveries = 0;
    const int64_t epoch = now100ns();
    PollTimer pollTimer;
    while (running && (!o.seconds || Clock::now() - start < std::chrono::seconds(o.seconds))) {
        try {
            auto all = displays();
            auto it = std::find_if(all.begin(), all.end(), [&](auto &d) { return d.name == o.display; });
            if (it == all.end())
                throw std::runtime_error("Selected display disconnected; waiting for the same display name");
            const auto selected = *it;
            if (selected.primary && !o.allowPrimary)
                throw std::runtime_error("Selected display is primary. Capture refused; --allow-primary is "
                                         "required to explicitly authorize it.");
            unsigned iw = selected.rect.right - selected.rect.left,
                     ih = selected.rect.bottom - selected.rect.top;
            double scale = std::min({1.0, 1920.0 / iw, 1080.0 / ih});
            unsigned ow = unsigned(iw * scale) & ~1u, oh = unsigned(ih * scale) & ~1u;
            Device device(selected);
            std::unique_ptr<Pattern> pattern;
            if(o.pattern)pattern=std::make_unique<Pattern>(device,selected);
            auto capture = o.backend == "wgc" ? wgc(device, selected) : duplication(device, selected);
            std::unique_ptr<Converter> converter;
            std::unique_ptr<IEncoder> encoder;
            if (o.mode != "capture")
                converter = std::make_unique<Converter>(device, iw, ih, ow, oh);
            if (o.mode == "encode" || o.mode == "capture-encode" || o.mode == "stream")
                encoder = hardwareEncoder(device, selected, ow, oh, o.fps);
            std::cout << "Pipeline: " << o.backend << " -> GPU NV12 -> "
                      << (encoder ? encoder->name() : "benchmark") << " | GPU " << selected.gpu << " | " << ow
                      << 'x' << oh << '@' << o.fps << " | CPU readback: no\n";
            uint64_t captured = 0, encoded = 0, dropped = 0, noChange = 0, previousCaptured = 0,
                     previousEncoded = 0;
            Samples<> capTimes, conversionTimes, encodeTimes, sizes;
            uint32_t bitrate = 8000000, attemptedBitrate = 8000000;
            auto report = Clock::now(), next = Clock::now(), lastOutput = Clock::now(),
                 lastInput = Clock::now();
            ComPtr<ID3D11Texture2D> synthetic;
            bool keyframePending = true, haveSurface = false, repeatRequested = false;
            if (o.mode == "encode") {
                D3D11_TEXTURE2D_DESC d{};
                d.Width = iw;
                d.Height = ih;
                d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
                d.BindFlags = D3D11_BIND_RENDER_TARGET;
                check(device.device->CreateTexture2D(&d, nullptr, &synthetic), "Benchmark BGRA source");
                ComPtr<ID3D11RenderTargetView> view;
                check(device.device->CreateRenderTargetView(synthetic.Get(), nullptr, &view),
                      "Benchmark render target");
                float color[4]{.08f, .3f, .18f, 1};
                device.context->ClearRenderTargetView(view.Get(), color);
                converter->convert(synthetic.Get(), 0);
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
                    if (encoder && transport->targetBitrate() != attemptedBitrate) {
                        auto target = transport->targetBitrate();
                        attemptedBitrate = target;
                        if (encoder->bitrate(target))
                            bitrate = target;
                    }
                }
                if (encoder) {
                    if (keyframePending) {
                        encoder->keyframe();
                        keyframePending = false;
                    }
                    for (auto &f : encoder->poll()) {
                        ++encoded;
                        lastOutput = Clock::now();
                        encodeTimes.add(f.latencyMs);
                        sizes.add(double(f.bytes.size()));
                        if (transport)
                            transport->send(f);
                    }
                    if (encoder->pending() && Clock::now() - lastOutput > std::chrono::seconds(3))
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
                                    auto nv12 = synthetic ? converter->texture(0)
                                                          : converter->convert(frame->texture.Get(), 0);
                                    if (!synthetic)
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
                        capture->release();
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
                                              [&](auto &d) { return d.name == o.display; });
                    if (found == current.end() || found->monitor != selected.monitor ||
                        found->primary != selected.primary ||
                        memcmp(&found->rect, &selected.rect, sizeof(RECT)) ||
                        memcmp(&found->luid, &selected.luid, sizeof(LUID)))
                        throw std::runtime_error("Display topology changed; revalidate selection");
                }
                // Poll async output promptly; do not wait a full frame period to transmit it.
                pollTimer.wait();
            }
        } catch (const std::exception &e) {
            std::cerr << e.what() << '\n';
            if (o.mode != "stream" || ++recoveries > 20)
                throw;
            std::cerr << "Retrying the explicitly selected display in 1 second\n";
            for (int i = 0; i < 100 && running; ++i) {
                if (transport)
                    transport->poll();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }
    transport.reset();
    MFShutdown();
    return 0;
}
} // namespace bm
int main(int argc, char **argv) {
    try {
        return bm::run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "Browser Monitor: " << e.what()
                  << "\nUsage: browser-monitor --list\n  --display \\\\.\\DISPLAY2 --signaling "
                     "https://worker.workers.dev\n  --mode capture|convert|encode|capture-encode|stream "
                     "--capture dxgi|wgc --fps 60 --seconds 30 --csv results.csv\n";
        return 1;
    }
}
