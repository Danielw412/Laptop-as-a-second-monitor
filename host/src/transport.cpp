#include "transport.hpp"
#include "ice_config.hpp"
#include "logging.hpp"
#include "rtcp.hpp"
#include <atomic>
#include <bcrypt.h>
#include <deque>
#include <iomanip>
#include <mutex>
#include <rtc/rembhandler.hpp>
#include <rtc/rtc.hpp>
#include <wincrypt.h>
namespace lm {
using Json = nlohmann::json;
namespace {
class TestPacketLoss final : public rtc::MediaHandler {
    unsigned every_;
    uint64_t packets_ = 0;

  public:
    std::atomic<uint64_t> dropped{0};
    explicit TestPacketLoss(unsigned every) : every_(every) {}
    void outgoing(rtc::message_vector &messages, const rtc::message_callback &) override {
        std::erase_if(messages, [&](const auto &m) {
            if (m->type == rtc::Message::Control || !every_ || ++packets_ % every_)
                return false;
            ++dropped;
            return true;
        });
    }
};
std::string withoutCandidates(const std::string &sdp) {
    std::istringstream input(sdp);
    std::string line, output;
    while (std::getline(input, line))
        if (!line.starts_with("a=candidate:") && !line.starts_with("a=end-of-candidates"))
            output += line + "\n";
    return output;
}
// MbedTLS does not automatically import the Windows trust store.
std::string windowsRoots() {
    HCERTSTORE store = CertOpenSystemStoreW(0, L"ROOT");
    if (!store)
        throw std::runtime_error("Cannot open Windows certificate roots");
    std::string pem;
    PCCERT_CONTEXT certificate = nullptr;
    while ((certificate = CertEnumCertificatesInStore(store, certificate))) {
        DWORD size = 0;
        if (CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded, CRYPT_STRING_BASE64HEADER,
                                 nullptr, &size)) {
            std::string encoded(size, '\0');
            if (CryptBinaryToStringA(certificate->pbCertEncoded, certificate->cbCertEncoded,
                                     CRYPT_STRING_BASE64HEADER, encoded.data(), &size)) {
                encoded.resize(size);
                pem += encoded;
            }
        }
    }
    CertCloseStore(store, 0);
    if (pem.empty())
        throw std::runtime_error("Windows certificate root store is empty");
    return pem;
}
void systemRandom(uint8_t *out, size_t n) {
    if (BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Secure random generator unavailable");
}
constexpr const char *directFailure = "Direct WebRTC connection failed. This network may block peer-to-peer "
                                      "WebRTC traffic. TURN relay is not enabled.";
constexpr uint32_t kVideoSsrc = 42;
// libdatachannel's own diagnostics (ICE, DTLS, SCTP, WebSocket) go to host.log, because they are where the exact
// reason for a failed or dropped connection is written. Rate limited: a congested socket can report every packet.
void routeLibraryLog() {
    static std::once_flag once;
    std::call_once(once, [] {
        rtc::InitLogger(rtc::LogLevel::Info, [](rtc::LogLevel level, std::string message) {
            static std::mutex mutex;
            static auto window = Clock::now();
            static unsigned lines = 0, suppressed = 0;
            std::lock_guard lock(mutex);
            const auto now = Clock::now();
            if (now - window > std::chrono::seconds(10)) {
                if (suppressed)
                    logInfo("libdatachannel: " + std::to_string(suppressed) + " more lines suppressed");
                window = now;
                lines = suppressed = 0;
            }
            if (++lines > 40) {
                ++suppressed;
                return;
            }
            const auto text = "libdatachannel: " + printable(message, 300);
            if (level <= rtc::LogLevel::Error)
                logError(text);
            else if (level == rtc::LogLevel::Warning)
                logWarning(text);
            else if (level == rtc::LogLevel::Info)
                logDebug(text);
        });
    });
}
// What the RTCP handler learned, shared with the engine thread.
struct RtcpState {
    std::mutex mutex;
    std::optional<rtcp::ReportBlock> latest;
    std::optional<double> latestRttMs, intervalMinRttMs;
    uint64_t reports = 0, nackMessages = 0, nackedPackets = 0, pli = 0, fir = 0;
};
struct Mailbox {
    std::mutex mutex;
    std::deque<Json> messages;
    std::atomic<uint32_t> keyframes{0}; // Bit mask of KeyframePolicy::Reason
    std::atomic<uint32_t> remb{0};
    RtcpState rtcp;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr); // Auto-reset: wakes the engine thread
    ~Mailbox() {
        if (event)
            CloseHandle(event);
    }
    void push(Json j) {
        {
            std::lock_guard lock(mutex);
            if (messages.size() < 256)
                messages.push_back(std::move(j));
        }
        SetEvent(event);
    }
    void requestKeyframe(KeyframePolicy::Reason reason) {
        keyframes.fetch_or(1u << reason);
        SetEvent(event);
    }
};
// Reads every RTCP packet the receiver sends (receiver reports, NACK, PLI, FIR) and leaves it in place for the rest
// of the chain. Replaces libdatachannel's PliHandler, which treats payload type 196 (RFC 2032's H.261 FIR) as a FIR
// and so never recognises the RFC 5104 FIR (PSFB, FMT 4) a browser actually sends.
class RtcpMonitor final : public rtc::MediaHandler {
    std::shared_ptr<Mailbox> box_;

  public:
    explicit RtcpMonitor(std::shared_ptr<Mailbox> box) : box_(std::move(box)) {}
    void incoming(rtc::message_vector &messages, const rtc::message_callback &) override {
        for (const auto &m : messages) {
            if (m->type != rtc::Message::Control)
                continue;
            const auto f = rtcp::parse({reinterpret_cast<const uint8_t *>(m->data()), m->size()}, kVideoSsrc);
            const auto arrival = rtcp::ntpMiddle(std::chrono::system_clock::now());
            {
                std::lock_guard lock(box_->rtcp.mutex);
                auto &s = box_->rtcp;
                for (auto &r : f.reports) {
                    ++s.reports;
                    s.latest = r;
                    if (const auto rtt = rtcp::rttMs(arrival, r.lsr, r.dlsr)) {
                        s.latestRttMs = rtt;
                        // A busy receiver sends some reports late (seen: 200 ms on loopback while its decoder was
                        // saturated); a real queue delays all of them. The interval's minimum is the queue.
                        s.intervalMinRttMs = s.intervalMinRttMs ? std::min(*s.intervalMinRttMs, *rtt) : *rtt;
                    }
                }
                s.nackMessages += f.nackMessages;
                s.nackedPackets += f.nackedPackets;
                s.pli += f.pli;
                s.fir += f.fir;
            }
            if (f.pli || f.fir)
                box_->requestKeyframe(KeyframePolicy::Receiver);
        }
    }
};
// Playout-delay RTP header extension: tells the browser to render every frame as soon as it is decoded instead of
// holding it in an adaptive jitter buffer. This is what cloud-gaming receivers rely on.
constexpr int kPlayoutDelayExtensionId = 6;
constexpr const char *kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
const char *peerStateName(rtc::PeerConnection::State s) {
    switch (s) {
    case rtc::PeerConnection::State::New:
        return "new";
    case rtc::PeerConnection::State::Connecting:
        return "connecting";
    case rtc::PeerConnection::State::Connected:
        return "connected";
    case rtc::PeerConnection::State::Disconnected:
        return "disconnected";
    case rtc::PeerConnection::State::Failed:
        return "failed";
    default:
        return "closed";
    }
}
const char *iceStateName(rtc::PeerConnection::IceState s) {
    switch (s) {
    case rtc::PeerConnection::IceState::New:
        return "new";
    case rtc::PeerConnection::IceState::Checking:
        return "checking";
    case rtc::PeerConnection::IceState::Connected:
        return "connected";
    case rtc::PeerConnection::IceState::Completed:
        return "completed";
    case rtc::PeerConnection::IceState::Failed:
        return "failed";
    case rtc::PeerConnection::IceState::Disconnected:
        return "disconnected";
    default:
        return "closed";
    }
}
std::string seconds(Clock::duration d) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1) << std::chrono::duration<double>(d).count() << " s";
    return out.str();
}
class Transport final : public ITransport {
    std::shared_ptr<Mailbox> mailbox_ = std::make_shared<Mailbox>();
    std::shared_ptr<rtc::WebSocket> socket_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_;
    TransportTestOptions test_;
    std::shared_ptr<TestPacketLoss> testLoss_;
    bool testDroppedKeyframe_ = false;
    std::string url_, room_, secret_, generation_;
    std::function<void(const TransportEvent &)> events_;
    PairingCodes codes_;
    uint64_t socketEpoch_ = 0, peerEpoch_ = 0;
    std::vector<rtc::Candidate> candidates_;
    bool remote_ = false, fatal_ = false, reported_ = false;
    SignalingState signaling_ = SignalingState::Disconnected;
    bool viewer_ = false, webrtcUp_ = false;
    // The receiver's signaling connection dropped while the media connection it set up is still running. The
    // media is kept; only if it fails too is the receiver gone.
    bool viewerAway_ = false;
    std::string rejection_;
    // What the worker supports ("resume": a reconnected WebSocket does not force a new WebRTC connection).
    std::vector<std::string> features_;
    Clock::time_point retry_ = Clock::now(), deadline_ = Clock::now(), lastTelemetry_ = Clock::now();
    std::optional<Clock::time_point> socketOpened_, lastDisconnected_;
    unsigned attempts_ = 0;
    uint64_t socketMessages_ = 0, signalingDrops_ = 0, resumed_ = 0, renegotiations_ = 0, mediaInterruptions_ = 0;
    uint64_t frames_ = 0, bytes_ = 0, dropped_ = 0;
    Clock::time_point peerStart_{};
    std::optional<Clock::time_point> connectedSince_;
    Json timings_ = Json::object();
    uint64_t keyframeRequests_ = 0, keyframesSent_ = 0;
    std::array<uint64_t, KeyframePolicy::kReasons> keyframeReasons_{};
    std::optional<Clock::time_point> keyframeRequested_;
    Clock::time_point keyframeWindow_ = Clock::now();
    uint64_t keyframeWindowRequests_ = 0;
    // RTCP bookkeeping for takeNetworkReport(): the report block and counters the previous report ended at.
    std::optional<rtcp::ReportBlock> reportedBlock_;
    uint64_t reportedReports_ = 0, reportedNacked_ = 0, reportedPli_ = 0, reportedFir_ = 0;
    Json receiver_ = Json::object();
    std::optional<Clock::time_point> receiverAt_;
    uint64_t receiverEventId_ = 0; // Newest receiver event already written to the log
    std::vector<Json> receiverMessages_;
    mutable std::mutex pairingMutex_;
    PairingSnapshot pairingSnapshot_;
    void emit(TransportEventType type, std::string detail = {}) {
        if (events_)
            events_({type, std::move(detail)});
    }
    bool supports(const char *feature) const {
        return std::find(features_.begin(), features_.end(), feature) != features_.end();
    }
    void setSignaling(SignalingState s) {
        if (signaling_ == s)
            return;
        signaling_ = s;
        switch (s) {
        case SignalingState::Connecting:
            emit(TransportEventType::SignalingConnecting);
            break;
        case SignalingState::Connected:
            emit(TransportEventType::SignalingConnected);
            break;
        case SignalingState::Disconnected:
            emit(TransportEventType::SignalingDisconnected);
            break;
        case SignalingState::Rejected:
            emit(TransportEventType::SignalingRejected, rejection_);
            break;
        }
    }
    void setViewer(bool present) {
        if (viewer_ == present)
            return;
        viewer_ = present;
        emit(present ? TransportEventType::ViewerJoined : TransportEventType::ViewerLeft);
    }
    void setWebRtc(bool up) {
        if (webrtcUp_ == up)
            return;
        webrtcUp_ = up;
        if (up)
            connectedSince_ = Clock::now();
        else
            connectedSince_.reset();
        emit(up ? TransportEventType::WebRtcConnected : TransportEventType::WebRtcDisconnected);
    }
    /// The candidate pair ICE settled on, as "host -> host over UDP". A relayed pair or a TCP one explains a
    /// higher latency that has nothing to do with the encoder.
    std::string describeRoute() const {
        static const char *const kinds[] = {"unknown", "host", "server-reflexive", "peer-reflexive", "relayed"};
        static const char *const transports[] = {"unknown", "UDP", "TCP-active", "TCP-passive", "TCP-so", "TCP"};
        rtc::Candidate local, remote;
        if (!peer_ || !peer_->getSelectedCandidatePair(&local, &remote))
            return "not selected yet";
        return std::string(kinds[size_t(local.type())]) + " -> " + kinds[size_t(remote.type())] + " over " +
               transports[size_t(local.transportType())];
    }
    void updatePairingSnapshot() {
        std::lock_guard lock(pairingMutex_);
        pairingSnapshot_.code = codes_.current();
        pairingSnapshot_.rotatesAt = codes_.currentRotatesAt();
        pairingSnapshot_.generation = codes_.generation();
    }
    void signal(Json j) {
        if (socket_ && socket_->isOpen()) {
            j["generation"] = generation_;
            socket_->send(j.dump());
        }
    }
    /// Tells a resume-capable worker whether this side still has a working media connection for the current
    /// generation; when both sides say so after a reconnect, the worker lets them keep it.
    void sendState(bool live) {
        if (supports("resume") && socket_ && socket_->isOpen() && signaling_ == SignalingState::Connected &&
            !generation_.empty())
            socket_->send(Json{{"type", "state"}, {"generation", generation_}, {"live", live}}.dump());
    }
    /// Sends the hashes of every currently valid code with their remaining lifetimes. Codes never leave the host.
    void publishCodes() {
        if (!socket_ || !socket_->isOpen() || signaling_ != SignalingState::Connected)
            return;
        Json list = Json::array();
        for (auto &r : codes_.registrations(Clock::now()))
            list.push_back({{"hash", r.hash}, {"ttlMs", r.ttlMs}});
        socket_->send(Json{{"type", "codes"}, {"codes", list}}.dump());
    }
    void reset() {
        ++peerEpoch_;
        track_.reset();
        channel_.reset();
        if (peer_)
            peer_->close();
        peer_.reset();
        rtp_.reset();
        candidates_.clear();
        remote_ = false;
        viewerAway_ = false;
        reportedBlock_.reset();
        setWebRtc(false);
    }
    void connectSocket() {
        if (socket_)
            socket_->close();
        rtc::WebSocketConfiguration config;
        if (url_.starts_with("wss://")) {
            static const auto roots = windowsRoots();
            config.caCertificatePemFile = roots;
        }
        // A ping after 10 s without traffic (the library default), and give up after three unanswered ones: a
        // half-open connection (laptop slept, Wi-Fi roamed) is then noticed in about 40 s instead of never.
        config.pingInterval = std::chrono::seconds(10);
        config.maxOutstandingPings = 3;
        socket_ = std::make_shared<rtc::WebSocket>(config);
        auto box = mailbox_;
        auto epoch = ++socketEpoch_;
        socket_->onOpen([box, epoch] { box->push({{"event", "socket-open"}, {"socket", epoch}}); });
        socket_->onClosed([box, epoch] { box->push({{"event", "socket-close"}, {"socket", epoch}}); });
        socket_->onError([box, epoch](std::string error) {
            box->push({{"event", "socket-close"}, {"socket", epoch}, {"error", std::move(error)}});
        });
        socket_->onMessage([box, epoch](rtc::message_variant message) {
            if (auto text = std::get_if<std::string>(&message); text && text->size() <= 24576) {
                try {
                    box->push({{"event", "signal"}, {"socket", epoch}, {"body", Json::parse(*text)}});
                } catch (...) {
                }
            }
        });
        setSignaling(SignalingState::Connecting);
        socketOpened_.reset();
        socketMessages_ = 0;
        socket_->open(url_ + "/room/" + room_);
        retry_ = Clock::time_point::max();
    }
    void createPeer() {
        reset();
        ++renegotiations_;
        peerStart_ = Clock::now();
        timings_ = Json::object();
        auto box = mailbox_;
        auto epoch = peerEpoch_;
        auto ice = Json::parse(iceJson);
        rtc::Configuration config;
        for (auto &server : ice["iceServers"])
            for (auto &url : server["urls"])
                config.iceServers.emplace_back(url.get<std::string>());
        config.disableAutoNegotiation = true;
        peer_ = std::make_shared<rtc::PeerConnection>(config);
        peer_->onLocalDescription([box, epoch](rtc::Description d) {
            box->push({{"event", "local-description"},
                       {"peer", epoch},
                       {"sdp", std::string(d)},
                       {"type", d.typeString()}});
        });
        peer_->onLocalCandidate([box, epoch](rtc::Candidate c) {
            box->push(
                {{"event", "local-ice"}, {"peer", epoch}, {"candidate", c.candidate()}, {"mid", c.mid()}});
        });
        peer_->onStateChange([box, epoch](rtc::PeerConnection::State state) {
            box->push({{"event", "peer-state"}, {"peer", epoch}, {"state", int(state)}});
        });
        peer_->onIceStateChange([box, epoch](rtc::PeerConnection::IceState state) {
            box->push({{"event", "ice-state"}, {"peer", epoch}, {"state", int(state)}});
        });
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        // Baseline, level 4.2 permits 1080p60. Packetization mode 1 supports FU-A fragmentation.
        video.addH264Codec(96, "profile-level-id=42002a;packetization-mode=1;level-asymmetry-allowed=1");
        video.addSSRC(kVideoSsrc, "laptop-monitor", "display", "video");
        video.addExtMap(rtc::Description::Entry::ExtMap(kPlayoutDelayExtensionId, kPlayoutDelayUri));
        track_ = peer_->addTrack(video);
        rtp_ = std::make_shared<rtc::RtpPacketizationConfig>(kVideoSsrc, "laptop-monitor", uint8_t(96), 90000u);
        rtp_->playoutDelayId = kPlayoutDelayExtensionId;
        rtp_->playoutDelayMin = 0;
        rtp_->playoutDelayMax = 0;
        auto packetizer =
            std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp_, 1200);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_));
        // 512 packets is about a second of 8 Mbps video: a NACK for anything older is too late to help anyway.
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
        packetizer->addToChain(std::make_shared<RtcpMonitor>(box));
        // The browser's receiver-side bandwidth estimate; recorded for diagnostics only.
        packetizer->addToChain(std::make_shared<rtc::RembHandler>([box](unsigned bps) { box->remb = bps; }));
        testLoss_ = std::make_shared<TestPacketLoss>(test_.dropEvery);
        if (test_.dropEvery)
            packetizer->addToChain(testLoss_);
        track_->setMediaHandler(packetizer);
        track_->onOpen([box] { box->requestKeyframe(KeyframePolicy::Join); });
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        channel_ = peer_->createDataChannel("telemetry", init);
        channel_->onMessage([box, epoch](rtc::message_variant m) {
            // Probe replies carry a 240x135 grid twice (~90 KB of base64); everything else is a few KB.
            if (auto text = std::get_if<std::string>(&m); text && text->size() < 262144) {
                try {
                    box->push({{"event", "telemetry"}, {"peer", epoch}, {"body", Json::parse(*text)}});
                } catch (...) {
                }
            }
        });
        peer_->setLocalDescription();
        deadline_ = Clock::now() + std::chrono::milliseconds(ice["connectionTimeoutMs"].get<int>());
        reported_ = false;
        logInfo("Negotiating direct WebRTC (generation " + generation_.substr(0, 8) + ")");
    }
    void onSocketClosed(const Json &m) {
        // onError and onClosed both report the same socket; handle it once.
        if (retry_ != Clock::time_point::max())
            return;
        ++signalingDrops_;
        const std::string lived = socketOpened_ ? seconds(Clock::now() - *socketOpened_) : "never opened";
        const std::string reason = m.contains("error") ? m["error"].get<std::string>() : "closed by the other end";
        if (connected()) {
            // The WebSocket only introduces the two machines. A working media connection does not need it, so it
            // is kept; the socket reconnects in the background and the worker resumes the session.
            logWarning("Signaling connection lost (" + reason + ", open " + lived + ", " +
                       std::to_string(socketMessages_) + " messages); the media connection is kept while it "
                       "reconnects");
        } else {
            reset();
            setViewer(false);
            logWarning("Signaling connection lost (" + reason + ", open " + lived + ", " +
                       std::to_string(socketMessages_) + " messages)");
        }
        if (signaling_ != SignalingState::Rejected)
            setSignaling(SignalingState::Disconnected);
        retry_ = Clock::now() + std::chrono::milliseconds(std::min(10000u, 500u << std::min(attempts_++, 5u)));
        if (m.contains("error"))
            logWarning("Signaling connection to " + url_ + " failed: " + reason +
                       ". A firewall, web filter or TLS-inspecting proxy may be blocking it.");
    }
    void onSignal(const Json &body) {
        ++socketMessages_;
        auto type = body.value("type", "");
        if (type == "authenticated") {
            attempts_ = 0;
            features_.clear();
            if (body.contains("features") && body["features"].is_array())
                for (auto &f : body["features"])
                    if (f.is_string() && features_.size() < 16)
                        features_.push_back(f.get<std::string>());
            setSignaling(SignalingState::Connected);
            publishCodes();
            if (connected())
                sendState(true);
            logInfo(std::string("Room authenticated") + (supports("resume") ? " (worker keeps sessions)" : "") +
                    (connected() ? "; media connection still up" : "; waiting for receiver"));
        } else if (type == "error") {
            const auto code = body.value("code", "unknown");
            logWarning("Signaling rejected the host: " + code);
            fatal_ = code != "expired" && code != "role-occupied" && code != "rate-limit";
            reset();
            if (fatal_) {
                rejection_ = code;
                setSignaling(SignalingState::Rejected);
            }
            socket_->close();
        } else if (type == "ready") {
            const auto generation = body.at("generation").get<std::string>();
            viewerAway_ = false;
            if (body.value("resume", false) && generation == generation_ && connected()) {
                ++resumed_;
                logInfo("Signaling resumed; the media connection was kept (no renegotiation)");
                setViewer(true);
                return;
            }
            generation_ = generation;
            setViewer(true);
            createPeer();
        } else if (type == "peer-left") {
            if (connected()) {
                viewerAway_ = true;
                logInfo("Receiver's signaling connection dropped; its media connection is still up");
                return;
            }
            reset();
            setViewer(false);
            logInfo("Receiver left; waiting for it to return");
        } else if (peer_ && body.value("generation", "") == generation_) {
            if (type == "answer") {
                peer_->setRemoteDescription(rtc::Description(
                    test_.blockIce ? withoutCandidates(body.at("sdp").get<std::string>())
                                   : body.at("sdp").get<std::string>(),
                    "answer"));
                remote_ = true;
                timings_["answer_ms"] =
                    std::chrono::duration<double, std::milli>(Clock::now() - peerStart_).count();
                for (auto &c : candidates_)
                    peer_->addRemoteCandidate(c);
                candidates_.clear();
            } else if (type == "ice") {
                if (test_.blockIce)
                    return;
                rtc::Candidate c(body.at("candidate").get<std::string>(), body.at("mid").get<std::string>());
                if (remote_)
                    peer_->addRemoteCandidate(c);
                else if (candidates_.size() < 128)
                    candidates_.push_back(c);
            }
        }
    }
    void onPeerState(rtc::PeerConnection::State s) {
        logDebug(std::string("WebRTC state: ") + peerStateName(s));
        if (s == rtc::PeerConnection::State::Connected) {
            reported_ = false;
            timings_["connected_ms"] = std::chrono::duration<double, std::milli>(Clock::now() - peerStart_).count();
            if (lastDisconnected_) {
                // ICE recovered on its own: the receiver may have lost frames meanwhile, so start clean.
                logInfo("Direct WebRTC recovered after " + seconds(Clock::now() - *lastDisconnected_) +
                        " | route " + describeRoute());
                lastDisconnected_.reset();
                mailbox_->requestKeyframe(KeyframePolicy::Recovery);
            } else {
                logInfo("Direct WebRTC connected");
                // How long each half of the handshake took, and over which route. This is the whole of
                // "why did it take that long before my screen appeared", on the host's side of it.
                logInfo("Negotiation: answer " + std::to_string(int(timings_.value("answer_ms", 0.0))) +
                        " ms, connected " + std::to_string(int(timings_.value("connected_ms", 0.0))) +
                        " ms | route " + describeRoute());
            }
            setWebRtc(true);
            sendState(true);
        } else if (s == rtc::PeerConnection::State::Disconnected) {
            ++mediaInterruptions_;
            lastDisconnected_ = Clock::now();
            deadline_ = Clock::now() + std::chrono::seconds(20);
            logWarning("Direct WebRTC disconnected; waiting up to 20 s for it to recover (the libdatachannel lines "
                       "around this say why)");
            setWebRtc(false);
        } else if (s == rtc::PeerConnection::State::Failed || s == rtc::PeerConnection::State::Closed) {
            if (s == rtc::PeerConnection::State::Failed) {
                logWarning(std::string(directFailure) + " (WebRTC state failed after " +
                           seconds(Clock::now() - peerStart_) + ")");
                reported_ = true;
            } else
                logInfo("Direct WebRTC closed by the receiver");
            lastDisconnected_.reset();
            setWebRtc(false);
            sendState(false);
            if (viewerAway_) {
                // Its signaling had already gone; with the media gone too, the receiver has left.
                reset();
                setViewer(false);
                logInfo("Receiver left; waiting for it to return");
            }
        }
    }
    void onTelemetry(const Json &b) {
        const auto type = b.value("type", "");
        if (type == "probe" || type == "mark") {
            if (receiverMessages_.size() < 32)
                receiverMessages_.push_back(b);
            return;
        }
        if (type != "telemetry")
            return;
        // Receiver-side events (its connection states, WebSocket closes and their codes) arrive in every telemetry
        // message until they are old; each is logged once, so a disconnect is on record from both ends.
        if (b.contains("events") && b["events"].is_array())
            for (auto &e : b["events"]) {
                if (!e.is_object() || !e.contains("id") || !e["id"].is_number_unsigned())
                    continue;
                // Ids grow with the receiver's clock (a reloaded page continues above the old one), so anything not
                // newer than the last one logged has been logged.
                const auto id = e["id"].get<uint64_t>();
                if (id <= receiverEventId_)
                    continue;
                receiverEventId_ = id;
                logInfo("Receiver event: " + printable(e.value("text", ""), 200) + " (receiver clock " +
                        printable(e.value("at", ""), 32) + ")");
            }
        if (Clock::now() - lastTelemetry_ < std::chrono::milliseconds(500))
            return;
        receiver_ = b;
        receiverAt_ = Clock::now();
        lastTelemetry_ = Clock::now();
    }

  public:
    Transport(std::string server, std::string secret, std::function<void(const TransportEvent &)> events,
              TransportTestOptions test)
        : test_(test), url_(std::move(server)), secret_(std::move(secret)), events_(std::move(events)),
          codes_(systemRandom, Clock::now()) {
        if (!validSecret(secret_))
            throw std::runtime_error("Invalid host credential");
        routeLibraryLog();
        room_ = roomIdFor(secret_);
        while (!url_.empty() && url_.back() == '/')
            url_.pop_back();
        if (url_.starts_with("https://"))
            url_.replace(0, 8, "wss://");
        if (url_.starts_with("http://"))
            url_.replace(0, 7, "ws://");
        if (!url_.starts_with("wss://") && !url_.starts_with("ws://127.0.0.1:") &&
            !url_.starts_with("ws://localhost:"))
            throw std::runtime_error("Signaling requires HTTPS/WSS except localhost");
        if (test.dropEvery || test.dropFirstKeyframe || test.blockIce)
            logWarning("TEST MODE: deliberate media loss or ICE blocking enabled");
        updatePairingSnapshot();
    }
    ~Transport() {
        // No state events from a dying transport: the engine reports Stopped, which resets the viewer state.
        events_ = nullptr;
        reset();
        if (socket_)
            socket_->close();
    }
    void poll() override {
        const auto now = Clock::now();
        if (!fatal_ && now >= retry_)
            connectSocket();
        if (codes_.tick(now)) {
            updatePairingSnapshot();
            publishCodes();
            emit(TransportEventType::CodeRotated);
        }
        std::deque<Json> messages;
        {
            std::lock_guard lock(mailbox_->mutex);
            messages.swap(mailbox_->messages);
        }
        for (auto &m : messages) {
            try {
                if (m.contains("socket") && m["socket"] != socketEpoch_)
                    continue;
                if (m.contains("peer") && m["peer"] != peerEpoch_)
                    continue;
                const auto event = m["event"].get<std::string>();
                if (event == "socket-open") {
                    socketOpened_ = Clock::now();
                    Json auth{{"type", "auth"}, {"version", 2}, {"role", "host"}, {"secret", secret_}};
                    // A worker that keeps sessions resumes this one instead of starting a new WebRTC connection.
                    // Older workers drop the unknown field.
                    if (connected() && !generation_.empty())
                        auth["live"] = generation_;
                    socket_->send(auth.dump());
                } else if (event == "socket-close")
                    onSocketClosed(m);
                else if (event == "signal")
                    onSignal(m["body"]);
                else if (event == "local-description")
                    signal({{"type", "offer"},
                            {"sdp", test_.blockIce ? withoutCandidates(m["sdp"].get<std::string>())
                                                   : m["sdp"].get<std::string>()}});
                else if (event == "local-ice" && !test_.blockIce)
                    signal({{"type", "ice"}, {"candidate", m["candidate"]}, {"mid", m["mid"]}});
                else if (event == "peer-state")
                    onPeerState(rtc::PeerConnection::State(m["state"].get<int>()));
                else if (event == "ice-state")
                    logDebug(std::string("ICE state: ") +
                             iceStateName(rtc::PeerConnection::IceState(m["state"].get<int>())));
                else if (event == "telemetry")
                    onTelemetry(m["body"]);
            } catch (const std::exception &e) {
                logWarning(std::string("Rejected invalid peer message or negotiation failed: ") + e.what());
            }
        }
        if (peer_ && !connected() && !lastDisconnected_ && Clock::now() > deadline_ && !reported_) {
            logWarning(std::string(directFailure) + " (no connection " +
                       seconds(Clock::now() - peerStart_) + " after the offer)");
            reported_ = true;
        }
        if (lastDisconnected_ && Clock::now() > deadline_ && !reported_) {
            logWarning("Direct WebRTC did not recover within 20 s; waiting for the receiver to reconnect");
            reported_ = true;
        }
        reportPressure();
    }
    /// The receiver asking for keyframes over and over (each one is a 200-300 KB frame) quietly ruins the picture
    /// without ever raising an error.
    void reportPressure() {
        const auto now = Clock::now();
        if (now - keyframeWindow_ < std::chrono::seconds(10))
            return;
        const auto secondsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - keyframeWindow_).count();
        const auto requests = keyframeReasons_[KeyframePolicy::Receiver] - keyframeWindowRequests_;
        keyframeWindow_ = now;
        keyframeWindowRequests_ = keyframeReasons_[KeyframePolicy::Receiver];
        if (requests > 2 && connected())
            logWarning("Receiver asked for " + std::to_string(requests) + " keyframes in " +
                       std::to_string(secondsElapsed) + " s (PLI/FIR); usually packet loss.");
    }
    bool connected() const override {
        return peer_ && peer_->state() == rtc::PeerConnection::State::Connected && track_ && track_->isOpen();
    }
    bool send(const Encoded &frame) override {
        if (test_.dropFirstKeyframe && !testDroppedKeyframe_ && frame.keyframe) {
            testDroppedKeyframe_ = true;
            logWarning("TEST MODE: discarded first keyframe to exercise browser PLI");
            return false;
        }
        if (!connected()) {
            ++dropped_;
            return false;
        }
        rtp_->timestamp = rtp_->startTimestamp + rtpTimestamp(frame.timestamp);
        try {
            // track_->send returns the result of the frame's last packet only; a packet that could not be
            // written has a sequence number regardless, so the receiver sees the gap and NACKs it.
            const bool ok =
                track_->send(reinterpret_cast<const rtc::byte *>(frame.bytes.data()), frame.bytes.size());
            ++frames_;
            bytes_ += frame.bytes.size();
            if (!ok)
                ++dropped_;
            if (!timings_.contains("first_sent_ms"))
                timings_["first_sent_ms"] =
                    std::chrono::duration<double, std::milli>(Clock::now() - peerStart_).count();
            if (frame.keyframe) {
                ++keyframesSent_;
                if (!timings_.contains("first_keyframe_ms"))
                    timings_["first_keyframe_ms"] =
                        std::chrono::duration<double, std::milli>(Clock::now() - peerStart_).count();
                if (keyframeRequested_) {
                    timings_["keyframe_response_ms"] =
                        std::chrono::duration<double, std::milli>(Clock::now() - *keyframeRequested_).count();
                    keyframeRequested_.reset();
                }
            }
            return true;
        } catch (const std::exception &e) {
            ++dropped_;
            logWarning(std::string("Media send failed: ") + e.what());
            return false;
        }
    }
    HANDLE wakeEvent() const override {
        return mailbox_->event;
    }
    uint32_t consumeKeyframeRequests() override {
        const uint32_t mask = mailbox_->keyframes.exchange(0);
        for (unsigned r = 0; r < KeyframePolicy::kReasons; ++r)
            if (mask & (1u << r))
                ++keyframeReasons_[r];
        if (mask) {
            ++keyframeRequests_;
            if (!keyframeRequested_)
                keyframeRequested_ = Clock::now();
        }
        return mask;
    }
    NetworkReport takeNetworkReport() override {
        NetworkReport r;
        std::optional<rtcp::ReportBlock> latest;
        {
            std::lock_guard lock(mailbox_->rtcp.mutex);
            auto &s = mailbox_->rtcp;
            latest = s.latest;
            r.reports = s.reports - reportedReports_;
            r.nackedPackets = s.nackedPackets - reportedNacked_;
            r.pli = s.pli - reportedPli_;
            r.fir = s.fir - reportedFir_;
            reportedReports_ = s.reports;
            reportedNacked_ = s.nackedPackets;
            reportedPli_ = s.pli;
            reportedFir_ = s.fir;
            if (r.reports)
                r.rttMs = s.intervalMinRttMs;
            s.intervalMinRttMs.reset();
        }
        if (latest && r.reports) {
            r.jitterMs = latest->jitter / 90.0;
            if (reportedBlock_) {
                if (const auto l = rtcp::lossBetween(*reportedBlock_, *latest)) {
                    r.loss = l->fraction();
                    r.packets = l->expected;
                    r.source = "rtcp";
                }
            }
            reportedBlock_ = latest;
        }
        if (!r.loss && receiverAt_ && Clock::now() - *receiverAt_ < std::chrono::milliseconds(2500)) {
            // No usable receiver report this second: the telemetry message is the next best source.
            auto number = [&](const char *key) -> std::optional<double> {
                if (receiver_.contains(key) && receiver_[key].is_number())
                    return receiver_[key].get<double>();
                return std::nullopt;
            };
            r.loss = number("loss");
            if (const auto p = number("intervalPacketsReceived"))
                r.packets = uint64_t(std::max(0.0, *p));
            if (!r.rttMs)
                r.rttMs = number("rttMs");
            if (!r.jitterMs)
                r.jitterMs = number("jitterMs");
            if (r.loss)
                r.source = "receiver";
        }
        return r;
    }
    std::optional<uint32_t> rtpTimestampOf(int64_t sampleTime) const override {
        if (!rtp_)
            return std::nullopt;
        return rtp_->startTimestamp + rtpTimestamp(sampleTime);
    }
    std::vector<Json> takeReceiverMessages() override {
        std::vector<Json> out;
        out.swap(receiverMessages_);
        return out;
    }
    bool sendToReceiver(const Json &value) override {
        if (!channel_ || !channel_->isOpen() || channel_->bufferedAmount() >= 16384)
            return false;
        try {
            return channel_->send(value.dump());
        } catch (...) {
            return false;
        }
    }
    void diagnostics(const Json &value) override {
        sendToReceiver(value);
    }
    Json stats() const override {
        Json reasons = Json::object();
        for (unsigned r = 0; r < KeyframePolicy::kReasons; ++r)
            reasons[KeyframePolicy::name(KeyframePolicy::Reason(r))] = keyframeReasons_[r];
        Json rtcpStats = Json::object();
        {
            std::lock_guard lock(mailbox_->rtcp.mutex);
            const auto &s = mailbox_->rtcp;
            rtcpStats = {{"reports", s.reports},
                         {"nack_messages", s.nackMessages},
                         {"nacked_packets", s.nackedPackets},
                         {"pli", s.pli},
                         {"fir", s.fir},
                         {"rtt_ms", s.latestRttMs ? Json(*s.latestRttMs) : Json(nullptr)},
                         {"cumulative_lost", s.latest ? Json(s.latest->cumulativeLost) : Json(nullptr)},
                         {"jitter_ms", s.latest ? Json(s.latest->jitter / 90.0) : Json(nullptr)}};
        }
        return {{"sent_frames", frames_},
                {"timings", timings_},
                {"keyframe_requests", keyframeRequests_},
                {"keyframe_request_reasons", reasons},
                {"keyframes_sent", keyframesSent_},
                {"test_rtp_dropped", testLoss_ ? testLoss_->dropped.load() : 0},
                {"encoded_bytes_sent", bytes_},
                {"transport_dropped", dropped_},
                {"rtcp", rtcpStats},
                {"receiver_estimate_bps", mailbox_->remb.load()},
                {"route", describeRoute()},
                {"webrtc_state", peer_ ? peerStateName(peer_->state()) : "none"},
                {"signaling_state", signaling_ == SignalingState::Connected    ? "connected"
                                    : signaling_ == SignalingState::Connecting ? "connecting"
                                    : signaling_ == SignalingState::Rejected   ? "rejected"
                                                                               : "disconnected"},
                {"signaling_drops", signalingDrops_},
                {"signaling_resumed", resumed_},
                {"renegotiations", renegotiations_},
                {"media_interruptions", mediaInterruptions_},
                {"receiver_away", viewerAway_},
                {"receiver_age_ms", receiverAt_ ? Json(std::chrono::duration<double, std::milli>(
                                                           Clock::now() - *receiverAt_)
                                                           .count())
                                                : Json(nullptr)},
                {"signaling_url", url_},
                {"receiver", receiver_}};
    }
    SignalingState signaling() const override {
        return signaling_;
    }
    bool viewerPresent() const override {
        return viewer_;
    }
    void disconnectViewer() override {
        if (socket_ && socket_->isOpen() && signaling_ == SignalingState::Connected)
            socket_->send(Json{{"type", "kick"}}.dump());
        reset();
        setViewer(false);
        rotateCode();
        logInfo("Receiver disconnected by the host");
    }
    void rotateCode() override {
        codes_.rotate(Clock::now());
        updatePairingSnapshot();
        publishCodes();
        emit(TransportEventType::CodeRotated);
    }
    PairingSnapshot pairing() const override {
        std::lock_guard lock(pairingMutex_);
        return pairingSnapshot_;
    }
    void fillMetrics(MetricsSnapshot &m) const override {
        m.sentFrames = frames_;
        m.sentBytes = bytes_;
        m.transportDropped = dropped_;
        m.keyframeRequests = keyframeRequests_;
        m.keyframesSent = keyframesSent_;
        m.bufferBytes = 0;
        if (auto remb = mailbox_->remb.load())
            m.receiverEstimateBps = remb;
        m.connectedSince = connectedSince_;
        static const char *const states[] = {"disconnected", "connecting", "connected", "rejected"};
        m.signalingState = states[size_t(signaling_)];
        if (!peer_)
            m.webrtcState = viewer_ ? "negotiating" : "none";
        else
            m.webrtcState = peerStateName(peer_->state());
        if (timings_.contains("connected_ms"))
            m.connectMs = timings_["connected_ms"].get<double>();
        if (timings_.contains("first_keyframe_ms"))
            m.firstKeyframeMs = timings_["first_keyframe_ms"].get<double>();
        auto number = [&](const char *key) -> std::optional<double> {
            if (receiver_.contains(key) && receiver_[key].is_number())
                return receiver_[key].get<double>();
            return std::nullopt;
        };
        m.viewerFps = number("fps");
        m.viewerBitrate = number("bitrate");
        m.rttMs = number("rttMs");
        m.loss = number("loss");
        m.jitterMs = number("jitterMs");
        m.viewerJitterBufferMs = number("jitterBufferMs");
        m.viewerDecodeMs = number("decodeMs");
        m.viewerProcessingMs = number("processingMs");
        m.viewerQp = number("qp");
        m.viewerCorrupted = number("corrupted");
        m.viewerFreezes = number("freezes");
        m.viewerPli = number("pli");
        m.viewerNack = number("nack");
        // Optional since the receiver added them; an older receiver simply leaves them unset.
        m.viewerFramesReceived = number("intervalFramesReceived");
        m.viewerFramesDecoded = number("intervalFramesDecoded");
        m.viewerFreezeMs = number("freezeMs");
        m.viewerPauseMs = number("pauseMs");
        if (auto v = number("dropped"))
            m.viewerDropped = uint64_t(std::max(0.0, *v));
        if (auto v = number("decoded"))
            m.viewerDecoded = uint64_t(std::max(0.0, *v));
        {
            // The host's own view of the path from RTCP overrides the telemetry's where it has one.
            std::lock_guard lock(mailbox_->rtcp.mutex);
            if (mailbox_->rtcp.latestRttMs)
                m.rttMs = mailbox_->rtcp.latestRttMs;
        }
        m.pairing = pairing();
    }
};
} // namespace
std::unique_ptr<ITransport> webRtc(std::string server, std::string hostSecret,
                                   std::function<void(const TransportEvent &)> events, TransportTestOptions test) {
    return std::make_unique<Transport>(std::move(server), std::move(hostSecret), std::move(events), test);
}
} // namespace lm
