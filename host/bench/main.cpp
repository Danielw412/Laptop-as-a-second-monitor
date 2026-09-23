// Command-line benchmark and diagnostics tool. Drives the same engine as the desktop app so the numbers in
// benchmarks/ stay comparable. Streaming from here pairs like the app (one code) using the app's credential.
#include "display_query.hpp"
#include "logging.hpp"
#include "pipeline.hpp"
#include "session_archive.hpp"
#include <iostream>
#include <winrt/base.h>
namespace lm {
namespace {
std::atomic<bool> interrupted = false;
BOOL WINAPI control(DWORD) {
    interrupted = true;
    return TRUE;
}
struct Options {
    std::string display, backend = "auto", mode = "capture-encode", server = kDefaultSignalingUrl, csv, tag, record,
        logDir;
    unsigned fps = 60, seconds = 0, bitrateSwitch = 0, recordSourceEvery = 0, scrollSpeed = 4;
    std::optional<uint32_t> bitrate;
    bool list = false, allowPrimary = false, pattern = false, synthetic = false, flushGpu = false,
         laptopMon = false;
    SyntheticContent content = SyntheticContent::Bar;
    std::optional<EncoderTuning> tuning;
    TransportTestOptions test;
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
        else if (a == "--pattern")
            o.pattern = true;
        else if (a == "--synthetic")
            o.synthetic = true;
        else if (a == "--laptopmon")
            o.laptopMon = true;
        else if (a == "--test-drop-every")
            o.test.dropEvery = std::stoul(value());
        else if (a == "--test-drop-first-keyframe")
            o.test.dropFirstKeyframe = true;
        else if (a == "--test-block-ice")
            o.test.blockIce = true;
        else if (a == "--flush-gpu")
            o.flushGpu = true;
        else if (a == "--test-bitrate-switch")
            o.bitrateSwitch = std::stoul(value());
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
        else if (a == "--diagnostic-tag")
            o.tag = value();
        else if (a == "--content")
            o.content = parseSyntheticContent(value());
        else if (a == "--scroll-speed")
            o.scrollSpeed = std::stoul(value());
        else if (a == "--bitrate")
            o.bitrate = uint32_t(std::stoul(value()));
        else if (a == "--record")
            o.record = value();
        else if (a == "--log-dir")
            o.logDir = value();
        else if (a == "--record-source-every")
            o.recordSourceEvery = std::stoul(value());
        else if (a == "--rate-control" || a == "--buffer-ms" || a == "--max-qp" || a == "--min-qp" ||
                 a == "--peak-percent" || a == "--gop") {
            if (!o.tuning)
                o.tuning = EngineConfig{}.encoder;
            const auto v = value();
            if (a == "--rate-control")
                o.tuning->rateControl = parseRateControl(v);
            else if (a == "--buffer-ms")
                o.tuning->bufferMs = std::stoul(v);
            else if (a == "--max-qp")
                o.tuning->maxQp = std::stoul(v);
            else if (a == "--min-qp")
                o.tuning->minQp = std::stoul(v);
            else if (a == "--peak-percent")
                o.tuning->peakPercent = std::stoul(v);
            else
                o.tuning->gopFrames = std::stoul(v);
        } else
            throw std::runtime_error("Unknown option: " + a);
    }
    if (o.backend != "dxgi" && o.backend != "wgc" && o.backend != "auto")
        throw std::runtime_error("Capture must be auto, dxgi or wgc");
    parseMode(o.mode);
    if (!o.tag.empty() && !validDiagnosticTag(o.tag))
        throw std::runtime_error("--diagnostic-tag takes 1-48 letters, digits, '.', '_' or '-'");
    return o;
}
int run(int argc, char **argv) {
    auto o = parse(argc, argv);
    // Labels every JSON record this run prints (with its run_id), so controlled runs can be told apart later.
    setDiagnosticTag(o.tag);
    Log::instance().setConsole(true);
    if (!o.logDir.empty()) {
        // The app's diagnostics files, written by the bench: quality probes and the flight recorder only run while
        // they are open, and receiver marks are saved next to them.
        Log::instance().openFile(o.logDir);
        Log::instance().openRecordFile(o.logDir);
    }
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(control, TRUE);
    auto outputs = displays();
    for (const auto &d : outputs)
        std::cout << d.name << " | " << d.gpu << " | " << d.rect.right - d.rect.left << 'x'
                  << d.rect.bottom - d.rect.top << (d.primary ? " | PRIMARY" : " | secondary") << '\n';
    for (const auto &t : queryDisplayTargets())
        if (isLaptopMon(t))
            std::cout << "LaptopMon: " << (t.gdiName.empty() ? "(inactive)" : t.gdiName) << " | " << t.devicePath
                      << (t.cloned ? " | DUPLICATE" : "") << (t.primary ? " | PRIMARY" : "") << '\n';
    if (o.list)
        return 0;
    if (o.display.empty() && !o.laptopMon && !o.synthetic && o.mode != "encode")
        throw std::runtime_error("Choose a source: --display \\\\.\\DISPLAYn (from --list) or --laptopmon");
    EngineConfig config;
    config.mode = parseMode(o.mode);
    config.backend = o.backend == "wgc" ? CaptureBackend::Wgc
                     : o.backend == "dxgi" ? CaptureBackend::Dxgi
                                           : CaptureBackend::Auto;
    config.fps = o.fps;
    config.pattern = o.pattern;
    config.synthetic = o.synthetic;
    config.flushGpu = o.flushGpu;
    config.allowPrimary = o.allowPrimary;
    config.seconds = o.seconds;
    config.bitrateSwitchSeconds = o.bitrateSwitch;
    config.csvPath = o.csv;
    config.test = o.test;
    config.syntheticContent = o.content;
    config.scrollSpeed = o.scrollSpeed;
    config.recordPath = o.record;
    config.recordSourceEvery = o.recordSourceEvery;
    if (o.tuning)
        config.encoder = *o.tuning;
    if (o.bitrate) // A fixed bitrate: adaptation has nowhere to go
        config.bitrate = {*o.bitrate, *o.bitrate, *o.bitrate};
    config.signalingUrl = o.server;
    if (config.mode == PipelineMode::Stream)
        config.hostSecret = loadOrCreateHostSecret(credentialPath());
    const bool allowPrimary = o.allowPrimary || o.synthetic || config.mode == PipelineMode::Encode;
    if (o.laptopMon)
        config.matcher = [allowPrimary] { return matchLaptopMon(allowPrimary); };
    else if (!o.display.empty())
        config.matcher = [name = o.display, allowPrimary] { return matchNamedDisplay(name, allowPrimary); };
    else
        config.matcher = [] {
            // Generated sources still need a GPU: use whichever adapter drives the first output.
            DisplayMatch m;
            auto all = displays();
            if (!all.empty()) {
                m.display = all.front();
                m.problem = SelectionProblem::None;
            }
            return m;
        };
    config.statsSink = [](const nlohmann::json &stats) {
        std::cout << stats.dump() << '\n';
        std::cout << "Interval FPS capture=" << stats["capture_fps"].get<double>()
                  << " encode=" << stats["encode_fps"].get<double>();
        if (stats.contains("gpu_command_span_ms_mean"))
            std::cout << " | GPU conversion ms mean=" << stats["gpu_command_span_ms_mean"].get<double>();
        std::cout << '\n';
    };
    std::string lastCode;
    StreamingEngine engine(config, [&](const EngineEvent &e) {
        switch (e.type) {
        case EngineEventType::SignalingConnected:
        case EngineEventType::CodeRotated:
        case EngineEventType::EncoderReady:
            break;
        case EngineEventType::Stopped:
            if (!e.detail.empty())
                std::cerr << "Engine stopped: " << e.detail << '\n';
            break;
        default:
            break;
        }
        if (config.mode == PipelineMode::Stream) {
            auto code = engine.snapshot().pairing.code;
            if (!code.empty() && code != lastCode) {
                lastCode = code;
                // Printed for the person running the benchmark; the log file never receives it.
                std::cout << "Pairing code: " << code << " (enter it at " << kViewerUrl << ")\n";
            }
        }
    });
    engine.start();
    while (engine.running()) {
        if (interrupted)
            engine.requestStop();
        Sleep(50);
    }
    engine.join();
    return 0;
}
} // namespace
} // namespace lm
int main(int argc, char **argv) {
    try {
        return lm::run(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "laptop-monitor-bench: " << e.what()
                  << "\nUsage: laptop-monitor-bench --list\n"
                     "  --laptopmon | --display \\\\.\\DISPLAYn\n"
                     "  --mode capture|convert|encode|capture-encode|stream --capture auto|dxgi|wgc --fps 60\n"
                     "  --seconds 30 --csv results.csv --pattern --synthetic --allow-primary\n"
                     "  --test-bitrate-switch 5 (alternate encoder bitrate every 5 s)\n"
                     "  --diagnostic-tag scrolling (label the JSON records of a controlled run)\n"
                     "  --content bar|scroll|desktop (what --synthetic and --pattern draw) --scroll-speed 4\n"
                     "  --bitrate 8000000 (fixed) --rate-control cbr|vbr|low-delay-vbr|quality --peak-percent 150\n"
                     "  --buffer-ms 500 --max-qp 40 --min-qp 18 --gop 600\n"
                     "  --record out.h264 [--record-source-every 30] (encoded stream + index, sampled NV12 source)\n"
                     "  --log-dir dir (write host.log and perf.jsonl there; enables quality probes and marks)\n";
        return 1;
    }
}
