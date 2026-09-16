#pragma once
// Signaling (WebSocket to the Cloudflare worker) plus the direct WebRTC media path. The transport owns the pairing
// codes: it publishes their hashes to the room, rotates them on schedule, and reports viewer/peer state.
#include "metrics.hpp"
#include "pairing.hpp"
#include "platform.hpp"
#include "settings.hpp"
#include <functional>
#include <nlohmann/json.hpp>
namespace bm {
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
class ITransport {
  public:
    virtual ~ITransport() = default;
    virtual void poll() = 0;
    virtual bool connected() const = 0;
    virtual bool send(const Encoded &) = 0;
    virtual bool consumeKeyframeRequest() = 0;
    virtual uint32_t targetBitrate() const = 0;
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
                                   TransportTestOptions test = {}, BitratePlan plan = bitratePlan(QualityPreset::Balanced));
} // namespace bm
