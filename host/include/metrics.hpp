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
    // Source activity over the last interval, on the host clock: tells a still desktop from stalled capture.
    uint64_t sourceFramesInterval = 0, noChangeInterval = 0, repeatFramesInterval = 0;
    std::optional<double> sourceGapMsMax, msSinceSourceFrame, userInputIdleMs;
    double captureMsMean = 0, captureMsP95 = 0;
    double convertSubmitMsMean = 0, gpuSpanMsMean = 0;
    std::optional<double> encodeMsMean, encodeMsP95, encodeMsP99, pipelineMsMean, pipelineMsP95;
    // Compositor presentation -> pipeline acquisition (how long a finished frame waited for the loop) and
    // presentation -> encoded output (host-side glass-to-bitstream). Empty when the backend gives no source time.
    std::optional<double> acquireDelayMsMean, acquireDelayMsP95, sourceToEncodedMsMean, sourceToEncodedMsP95;
    double loopWakeupsPerSecond = 0, loopMaxMs = 0; // Engine thread iterations/s and its longest iteration
    double loopBusyPercent = 0;                     // Share of wall time the engine thread spent not waiting
    double sendMsMean = 0, sendMsMax = 0;           // Time spent handing frames to the transport
    double submitIntervalMsP95 = 0, submitIntervalMsMax = 0; // Spacing between frames given to the encoder
    double frameBytesMean = 0, frameBytesMax = 0;
    uint64_t keyframes = 0;
    // Why the loop woke up, over the interval. Wake-ups that did no work are what idle CPU is made of.
    uint64_t wokeForFrame = 0, wokeForEncoder = 0, wokeForTransport = 0, wokeForTimeout = 0, wokeIdle = 0;
    // Why frames were dropped, over the session. Each cause has a different fix, so they are counted apart.
    uint64_t droppedCoalesced = 0,   // Superseded inside the capture backend before the loop saw them
        droppedSuperseded = 0,       // A newer frame arrived while this one waited for the encoder
        droppedRingBusy = 0,         // The encoder still held every NV12 surface
        droppedSubmitFailed = 0,     // The encoder refused the surface
        paced = 0;                   // Thinned out deliberately to honour a sub-60 fps setting
    double keyframeBytesMean = 0, deltaBytesMean = 0, keyframeEncodeMsMean = 0;
    uint32_t encoderRebuilds = 0;      // Times the encoder was recreated (each one is a visible stutter)
    double encoderRebuildMsMean = 0;
    double buildMs = 0;                // Cost of standing the pipeline up: device, capture, converter, encoder
    std::optional<double> firstEncodedMs, firstSentMs; // From pipeline ready to the first frame out
    double topologyCheckMsMax = 0;     // Longest display re-enumeration stall on the engine thread
    uint32_t bitrate = 0, targetBitrate = 0;
    bool dynamicBitrate = true;
    size_t queueDepth = 0;
    std::optional<double> cpuPercent, cpuKernelPercent;
    uint64_t workingSetMb = 0, privateMb = 0, gpuMemoryMb = 0;
    uint32_t handles = 0;
    bool onBattery = false, batterySaver = false;
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
    // What the picture looked like at the far end. viewerQp is the decoder's mean quantizer (pixelation itself);
    // the rest are loss, which shows as torn blocks until a keyframe repairs them.
    std::optional<double> viewerQp, viewerCorrupted, viewerFreezes, viewerPli, viewerNack;
    std::optional<uint32_t> receiverEstimateBps; // Browser's bandwidth estimate (REMB), for diagnostics
    std::optional<uint64_t> viewerDropped, viewerDecoded;
    // Frames received and decoded over the receiver's interval, and how long its picture was frozen or paused.
    std::optional<double> viewerFramesReceived, viewerFramesDecoded, viewerFreezeMs, viewerPauseMs;
    PairingSnapshot pairing;
    std::chrono::steady_clock::time_point updated{};
};
} // namespace lm
