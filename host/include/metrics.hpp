#pragma once
// Snapshot the streaming engine publishes about once per second. The GUI copies it on its own timer; nothing here
// is touched by the per-frame path beyond the counters the old console output already kept.
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
namespace lm {
struct PairingSnapshot {
    std::string code;                                 // Current code to show
    std::chrono::steady_clock::time_point rotatesAt{}; // When it will change
    uint64_t generation = 0;
};
struct MetricsSnapshot {
    // Pipeline
    double captureFps = 0, encodeFps = 0;
    uint64_t captured = 0, encoded = 0, dropped = 0, noChange = 0;
    double captureMsMean = 0, captureMsP95 = 0;
    double convertSubmitMsMean = 0, gpuSpanMsMean = 0;
    std::optional<double> encodeMsMean, encodeMsP95, encodeMsP99, pipelineMsMean, pipelineMsP95;
    // Compositor presentation -> pipeline acquisition (how long a finished frame waited for the loop) and
    // presentation -> encoded output (host-side glass-to-bitstream). Empty when the backend gives no source time.
    std::optional<double> acquireDelayMsMean, acquireDelayMsP95, sourceToEncodedMsMean, sourceToEncodedMsP95;
    double loopWakeupsPerSecond = 0, loopMaxMs = 0; // Engine thread iterations/s and its longest iteration
    double sendMsMean = 0, sendMsMax = 0;           // Time spent handing frames to the transport
    double submitIntervalMsP95 = 0, submitIntervalMsMax = 0; // Spacing between frames given to the encoder
    double frameBytesMean = 0, frameBytesMax = 0;
    uint64_t keyframes = 0;
    uint32_t bitrate = 0, targetBitrate = 0;
    bool dynamicBitrate = true;
    size_t queueDepth = 0;
    std::optional<double> cpuPercent;
    unsigned width = 0, height = 0, fps = 0;
    std::string encoder, gpu, backend, videoPath = "GPU";
    bool cpuReadback = false;
    // Transport
    uint64_t sentFrames = 0, sentBytes = 0, transportDropped = 0, keyframeRequests = 0, keyframesSent = 0;
    size_t bufferBytes = 0;
    std::string webrtcState = "none", signalingState = "disconnected";
    std::optional<std::chrono::steady_clock::time_point> connectedSince;
    std::optional<double> connectMs, firstKeyframeMs;
    // Receiver telemetry (what the browser reports back)
    std::optional<double> viewerFps, viewerBitrate, rttMs, loss, jitterMs;
    std::optional<double> viewerJitterBufferMs, viewerDecodeMs, viewerProcessingMs; // Receiver-side delays
    std::optional<uint32_t> receiverEstimateBps; // Browser's bandwidth estimate (REMB), for diagnostics
    std::optional<uint64_t> viewerDropped, viewerDecoded;
    PairingSnapshot pairing;
    std::chrono::steady_clock::time_point updated{};
};
} // namespace lm
