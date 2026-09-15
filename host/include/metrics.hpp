#pragma once
// Snapshot the streaming engine publishes about once per second. The GUI copies it on its own timer; nothing here
// is touched by the per-frame path beyond the counters the old console output already kept.
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
namespace bm {
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
    double frameBytesMean = 0;
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
    std::optional<uint64_t> viewerDropped, viewerDecoded;
    PairingSnapshot pairing;
    std::chrono::steady_clock::time_point updated{};
};
} // namespace bm
