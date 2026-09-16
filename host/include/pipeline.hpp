#pragma once
// The streaming engine: the proven capture -> GPU NV12 -> hardware H.264 -> WebRTC loop, running on its own thread
// and publishing a metrics snapshot about once a second. The GUI never touches the frame path.
#include "display_query.hpp"
#include "metrics.hpp"
#include "settings.hpp"
#include "transport.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
namespace bm {
enum class PipelineMode { Capture, Convert, Encode, CaptureEncode, Stream };
using DisplayMatcher = std::function<DisplayMatch()>;
struct EngineConfig {
    PipelineMode mode = PipelineMode::Stream;
    CaptureBackend backend = CaptureBackend::Auto;
    unsigned fps = 60;
    BitratePlan bitrate = bitratePlan(QualityPreset::Balanced);
    bool pattern = false, synthetic = false, flushGpu = false, allowPrimary = false;
    unsigned seconds = 0;   // Bench: stop after this long (0 = until stopped)
    unsigned bitrateSwitchSeconds = 0; // Bench: alternate the encoder bitrate between plan min/max every N s
    std::string csvPath;    // Bench: per-second CSV
    std::string signalingUrl, hostSecret;
    TransportTestOptions test;
    DisplayMatcher matcher; // Required: how to find the display to capture
    std::function<void(const nlohmann::json &)> statsSink; // Bench: per-second JSON line
};
enum class EngineEventType {
    Started,
    EncoderReady,
    SignalingConnecting,
    SignalingConnected,
    SignalingDisconnected,
    SignalingRejected,
    ViewerJoined,
    ViewerLeft,
    WebRtcConnected,
    WebRtcDisconnected,
    CodeRotated,
    DisplayLost,
    DisplayFound,
    Error,   // detail: what went wrong; the engine keeps retrying
    Stopped, // detail: fatal reason, or empty for a requested stop
};
struct EngineEvent {
    EngineEventType type;
    std::string detail;
};
class StreamingEngine {
  public:
    StreamingEngine(EngineConfig config, std::function<void(const EngineEvent &)> events);
    ~StreamingEngine();
    /// Runs the loop on a background thread.
    void start();
    /// Runs the loop on the calling thread until stopped or the configured duration elapses (bench).
    void run();
    void requestStop();
    void join();
    bool running() const {
        return running_;
    }
    void disconnectViewer() {
        kick_ = true;
    }
    void rotateCode() {
        rotate_ = true;
    }
    void requestKeyframe() {
        keyframe_ = true;
    }
    MetricsSnapshot snapshot() const;
    const char *resolvedBackend() const;

  private:
    struct Session; // One pipeline lifetime (device, capture, converter, encoder)
    EngineConfig config_;
    std::function<void(const EngineEvent &)> events_;
    std::thread thread_;
    std::atomic<bool> stop_{false}, running_{false}, kick_{false}, rotate_{false}, keyframe_{false};
    std::atomic<CaptureBackend> resolved_{CaptureBackend::Auto};
    mutable std::mutex snapshotMutex_;
    MetricsSnapshot snapshot_;
    std::unique_ptr<ITransport> transport_;
    void emit(EngineEventType type, std::string detail = {});
    void loop();
    void publish(const MetricsSnapshot &);
};
PipelineMode parseMode(const std::string &);
} // namespace bm
