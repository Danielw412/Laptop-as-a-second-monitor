#pragma once
// Signaling (WebSocket to the Cloudflare worker) plus the direct WebRTC media path. The transport owns the pairing
// codes: it publishes their hashes to the room, rotates them on schedule, and reports viewer/peer state.
#include "metrics.hpp"
#include "pairing.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include <functional>
#include <nlohmann/json.hpp>
namespace lm {
enum class SignalingState { Disconnected, Connecting, Connected, Rejected };
enum class TransportEventType {
    SignalingConnecting,
    SignalingConnected,
    SignalingDisconnected,
    SignalingRejected,
    ViewerJoined,
    ViewerLeft,
    WebRtcConnected,
    WebRtcDisconnected,
    CodeRotated,
};
struct TransportEvent {
    TransportEventType type;
    std::string detail;
};
/// Network evidence gathered since the previous takeNetworkReport(), for bitrate adaptation and the log. Loss and RTT
/// come from the receiver's RTCP reports when there were any, otherwise from its telemetry message.
struct NetworkReport {
    std::optional<double> loss, rttMs, jitterMs;
    std::optional<uint64_t> packets; // Packets the loss fraction is over
    uint64_t reports = 0, nackedPackets = 0, pli = 0, fir = 0;
    const char *source = "none"; // "rtcp", "receiver" or "none"
};
class ITransport {
  public:
    virtual ~ITransport() = default;
    virtual void poll() = 0;
    virtual bool connected() const = 0;
    virtual bool send(const Encoded &) = 0;
    /// Keyframe requests since the last call, as a bit mask of KeyframePolicy::Reason.
    virtual uint32_t consumeKeyframeRequests() = 0;
    virtual NetworkReport takeNetworkReport() = 0;
    /// Messages the receiver page sent that the engine acts on: quality probe results and "mark" (the viewer
    /// flagged a damaged picture). Each is a JSON object with a "type".
    virtual std::vector<nlohmann::json> takeReceiverMessages() = 0;
    /// Best effort, over the unreliable telemetry channel; false when it is not open or is backed up.
    virtual bool sendToReceiver(const nlohmann::json &) = 0;
    /// The RTP timestamp a frame with this encoder sample time carries on the wire (what the receiver's
    /// requestVideoFrameCallback reports), or empty without a media connection.
    virtual std::optional<uint32_t> rtpTimestampOf(int64_t sampleTime) const = 0;
    virtual nlohmann::json stats() const = 0;
    virtual void diagnostics(const nlohmann::json &) = 0;
    virtual SignalingState signaling() const = 0;
    virtual bool viewerPresent() const = 0;
    /// Drops the current viewer (server-side kick) and issues a fresh code.
    virtual void disconnectViewer() = 0;
    virtual void rotateCode() = 0;
    virtual PairingSnapshot pairing() const = 0;
    virtual void fillMetrics(MetricsSnapshot &) const = 0;
    /// Signalled when a signaling message, telemetry or a keyframe request arrives; lets the engine sleep on it.
    virtual HANDLE wakeEvent() const = 0;
};
struct TransportTestOptions {
    unsigned dropEvery = 0;
    bool dropFirstKeyframe = false, blockIce = false;
};
std::unique_ptr<ITransport> webRtc(std::string server, std::string hostSecret,
                                   std::function<void(const TransportEvent &)> events = {},
                                   TransportTestOptions test = {});
} // namespace lm
