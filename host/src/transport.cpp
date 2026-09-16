#include "transport.hpp"
#include "ice_config.hpp"
#include "logging.hpp"
#include <atomic>
#include <bcrypt.h>
#include <deque>
#include <mutex>
#include <rtc/plihandler.hpp>
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
struct Mailbox {
    std::mutex mutex;
    std::deque<Json> messages;
    std::atomic<bool> idr{false};
    std::atomic<uint32_t> remb{0};
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
    void requestKeyframe() {
        idr = true;
        SetEvent(event);
    }
};
// Playout-delay RTP header extension: tells the browser to render every frame as soon as it is decoded instead of
// holding it in an adaptive jitter buffer. This is what cloud-gaming receivers rely on.
constexpr int kPlayoutDelayExtensionId = 6;
constexpr const char *kPlayoutDelayUri = "http://www.webrtc.org/experiments/rtp-hdrext/playout-delay";
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
    std::string rejection_;
    Clock::time_point retry_ = Clock::now(), deadline_ = Clock::now(), lastTelemetry_ = Clock::now();
    unsigned attempts_ = 0;
    uint64_t frames_ = 0, bytes_ = 0, dropped_ = 0;
    Clock::time_point peerStart_{};
    std::optional<Clock::time_point> connectedSince_;
    Json timings_ = Json::object();
    uint64_t keyframeRequests_ = 0, keyframesSent_ = 0;
    std::optional<Clock::time_point> keyframeRequested_;
    BitrateController adaptation_;
    Json receiver_ = Json::object();
    mutable std::mutex pairingMutex_;
    PairingSnapshot pairingSnapshot_;
    void emit(TransportEventType type, std::string detail = {}) {
        if (events_)
            events_({type, std::move(detail)});
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
        socket_ = std::make_shared<rtc::WebSocket>(config);
        auto box = mailbox_;
        auto epoch = ++socketEpoch_;
        socket_->onOpen([box, epoch] { box->push({{"event", "socket-open"}, {"socket", epoch}}); });
        socket_->onClosed([box, epoch] { box->push({{"event", "socket-close"}, {"socket", epoch}}); });
        socket_->onError(
            [box, epoch](std::string) { box->push({{"event", "socket-close"}, {"socket", epoch}}); });
        socket_->onMessage([box, epoch](rtc::message_variant message) {
            if (auto text = std::get_if<std::string>(&message); text && text->size() <= 24576) {
                try {
                    box->push({{"event", "signal"}, {"socket", epoch}, {"body", Json::parse(*text)}});
                } catch (...) {
                }
            }
        });
        setSignaling(SignalingState::Connecting);
        socket_->open(url_ + "/room/" + room_);
        retry_ = Clock::time_point::max();
    }
    void createPeer() {
        reset();
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
        rtc::Description::Video video("video", rtc::Description::Direction::SendOnly);
        // Baseline, level 4.2 permits 1080p60. Packetization mode 1 supports FU-A fragmentation.
        video.addH264Codec(96, "profile-level-id=42002a;packetization-mode=1;level-asymmetry-allowed=1");
        video.addSSRC(42, "laptop-monitor", "display", "video");
        video.addExtMap(rtc::Description::Entry::ExtMap(kPlayoutDelayExtensionId, kPlayoutDelayUri));
        track_ = peer_->addTrack(video);
        rtp_ = std::make_shared<rtc::RtpPacketizationConfig>(42u, "laptop-monitor", uint8_t(96), 90000u);
        rtp_->playoutDelayId = kPlayoutDelayExtensionId;
        rtp_->playoutDelayMin = 0;
        rtp_->playoutDelayMax = 0;
        auto packetizer =
            std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp_, 1200);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([box] { box->requestKeyframe(); }));
        // The browser's receiver-side bandwidth estimate; recorded for diagnostics only.
        packetizer->addToChain(std::make_shared<rtc::RembHandler>([box](unsigned bps) { box->remb = bps; }));
        testLoss_ = std::make_shared<TestPacketLoss>(test_.dropEvery);
        if (test_.dropEvery)
            packetizer->addToChain(testLoss_);
        track_->setMediaHandler(packetizer);
        track_->onOpen([box] { box->requestKeyframe(); });
        rtc::DataChannelInit init;
        init.reliability.unordered = true;
        init.reliability.maxRetransmits = 0;
        channel_ = peer_->createDataChannel("telemetry", init);
        channel_->onMessage([box, epoch](rtc::message_variant m) {
            if (auto text = std::get_if<std::string>(&m); text && text->size() < 4096) {
                try {
                    box->push({{"event", "telemetry"}, {"peer", epoch}, {"body", Json::parse(*text)}});
                } catch (...) {
                }
            }
        });
        peer_->setLocalDescription();
        deadline_ = Clock::now() + std::chrono::milliseconds(ice["connectionTimeoutMs"].get<int>());
        reported_ = false;
        logInfo("Negotiating direct WebRTC");
    }

  public:
    Transport(std::string server, std::string secret, std::function<void(const TransportEvent &)> events,
              TransportTestOptions test, BitratePlan plan)
        : test_(test), url_(std::move(server)), secret_(std::move(secret)), events_(std::move(events)),
          codes_(systemRandom, Clock::now()), adaptation_(plan.initial, plan.minimum, plan.maximum) {
        if (!validSecret(secret_))
            throw std::runtime_error("Invalid host credential");
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
                if (event == "socket-open")
                    socket_->send(
                        Json{{"type", "auth"}, {"version", 2}, {"role", "host"}, {"secret", secret_}}.dump());
                else if (event == "socket-close") {
                    reset();
                    setViewer(false);
                    if (signaling_ != SignalingState::Rejected)
                        setSignaling(SignalingState::Disconnected);
                    if (retry_ == Clock::time_point::max())
                        retry_ = Clock::now() + std::chrono::milliseconds(
                                                    std::min(10000u, 500u << std::min(attempts_++, 5u)));
                    if (!fatal_)
                        logInfo("Signaling disconnected; retrying");
                } else if (event == "signal") {
                    auto &body = m["body"];
                    auto type = body.value("type", "");
                    if (type == "authenticated") {
                        attempts_ = 0;
                        setSignaling(SignalingState::Connected);
                        publishCodes();
                        logInfo("Room authenticated; waiting for receiver");
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
                        generation_ = body.at("generation").get<std::string>();
                        setViewer(true);
                        createPeer();
                    } else if (type == "peer-left") {
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
                                continue;
                            rtc::Candidate c(body.at("candidate").get<std::string>(),
                                             body.at("mid").get<std::string>());
                            if (remote_)
                                peer_->addRemoteCandidate(c);
                            else if (candidates_.size() < 128)
                                candidates_.push_back(c);
                        }
                    }
                } else if (event == "local-description")
                    signal({{"type", "offer"},
                            {"sdp", test_.blockIce ? withoutCandidates(m["sdp"].get<std::string>())
                                                   : m["sdp"].get<std::string>()}});
                else if (event == "local-ice" && !test_.blockIce)
                    signal({{"type", "ice"}, {"candidate", m["candidate"]}, {"mid", m["mid"]}});
                else if (event == "peer-state") {
                    auto s = rtc::PeerConnection::State(m["state"].get<int>());
                    if (s == rtc::PeerConnection::State::Connected) {
                        logInfo("Direct WebRTC connected");
                        reported_ = false;
                        timings_["connected_ms"] =
                            std::chrono::duration<double, std::milli>(Clock::now() - peerStart_).count();
                        setWebRtc(true);
                    } else if (s == rtc::PeerConnection::State::Disconnected) {
                        deadline_ = Clock::now() + std::chrono::seconds(20);
                        setWebRtc(false);
                    } else if (s == rtc::PeerConnection::State::Failed) {
                        logWarning(directFailure);
                        reported_ = true;
                        setWebRtc(false);
                    } else if (s == rtc::PeerConnection::State::Closed)
                        setWebRtc(false);
                } else if (event == "telemetry") {
                    const auto &b = m["body"];
                    if (b.value("type", "") != "telemetry" ||
                        Clock::now() - lastTelemetry_ < std::chrono::milliseconds(500))
                        continue;
                    receiver_ = b;
                    if (!b.contains("loss") || !b["loss"].is_number() || !b.contains("rttMs") ||
                        !b["rttMs"].is_number() || !b.contains("jitterMs") || !b["jitterMs"].is_number())
                        continue;
                    double loss = b.at("loss").get<double>(), rtt = b.at("rttMs").get<double>(),
                           jitter = b.at("jitterMs").get<double>();
                    adaptation_.update(loss, rtt, jitter);
                    lastTelemetry_ = Clock::now();
                }
            } catch (const std::exception &) {
                logWarning("Rejected invalid peer message or negotiation failed");
            }
        }
        if (peer_ && !connected() && Clock::now() > deadline_ && !reported_) {
            logWarning(directFailure);
            reported_ = true;
        }
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
        if (!connected() || track_->bufferedAmount() > 128 * 1024) {
            ++dropped_;
            mailbox_->idr = true;
            return false;
        }
        rtp_->timestamp = rtp_->startTimestamp + rtpTimestamp(frame.timestamp);
        try {
            bool ok =
                track_->send(reinterpret_cast<const rtc::byte *>(frame.bytes.data()), frame.bytes.size());
            if (ok) {
                ++frames_;
                bytes_ += frame.bytes.size();
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
            } else {
                ++dropped_;
                mailbox_->idr = true;
            }
            return ok;
        } catch (...) {
            ++dropped_;
            mailbox_->idr = true;
            return false;
        }
    }
    HANDLE wakeEvent() const override {
        return mailbox_->event;
    }
    bool consumeKeyframeRequest() override {
        if (!mailbox_->idr.exchange(false))
            return false;
        ++keyframeRequests_;
        if (!keyframeRequested_)
            keyframeRequested_ = Clock::now();
        return true;
    }
    uint32_t targetBitrate() const override {
        return adaptation_.bitrate();
    }
    void diagnostics(const Json &value) override {
        if (channel_ && channel_->isOpen() && channel_->bufferedAmount() < 4096)
            channel_->send(value.dump());
    }
    Json stats() const override {
        return {{"sent_frames", frames_},
                {"timings", timings_},
                {"keyframe_requests", keyframeRequests_},
                {"keyframes_sent", keyframesSent_},
                {"test_rtp_dropped", testLoss_ ? testLoss_->dropped.load() : 0},
                {"encoded_bytes_sent", bytes_},
                {"transport_dropped", dropped_},
                {"transport_buffer_bytes", track_ ? track_->bufferedAmount() : 0},
                {"receiver_estimate_bps", mailbox_->remb.load()},
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
        m.bufferBytes = track_ ? track_->bufferedAmount() : 0;
        m.targetBitrate = adaptation_.bitrate();
        if (auto remb = mailbox_->remb.load())
            m.receiverEstimateBps = remb;
        m.connectedSince = connectedSince_;
        static const char *const states[] = {"disconnected", "connecting", "connected", "rejected"};
        m.signalingState = states[size_t(signaling_)];
        if (!peer_)
            m.webrtcState = viewer_ ? "negotiating" : "none";
        else {
            switch (peer_->state()) {
            case rtc::PeerConnection::State::New:
                m.webrtcState = "new";
                break;
            case rtc::PeerConnection::State::Connecting:
                m.webrtcState = "connecting";
                break;
            case rtc::PeerConnection::State::Connected:
                m.webrtcState = "connected";
                break;
            case rtc::PeerConnection::State::Disconnected:
                m.webrtcState = "disconnected";
                break;
            case rtc::PeerConnection::State::Failed:
                m.webrtcState = "failed";
                break;
            case rtc::PeerConnection::State::Closed:
                m.webrtcState = "closed";
                break;
            }
        }
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
        if (auto v = number("dropped"))
            m.viewerDropped = uint64_t(std::max(0.0, *v));
        if (auto v = number("decoded"))
            m.viewerDecoded = uint64_t(std::max(0.0, *v));
        m.pairing = pairing();
    }
};
} // namespace
std::unique_ptr<ITransport> webRtc(std::string server, std::string hostSecret,
                                   std::function<void(const TransportEvent &)> events, TransportTestOptions test,
                                   BitratePlan plan) {
    return std::make_unique<Transport>(std::move(server), std::move(hostSecret), std::move(events), test, plan);
}
} // namespace lm
