#include "transport.hpp"
#include "ice_config.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <rtc/plihandler.hpp>
#include <rtc/rtc.hpp>
namespace bm {
using Json = nlohmann::json;
namespace {
constexpr const char *directFailure = "Direct WebRTC connection failed.\nThis network may block peer-to-peer "
                                      "WebRTC traffic.\nTURN relay is not enabled.";
struct Mailbox {
    std::mutex mutex;
    std::deque<Json> messages;
    std::atomic<bool> idr{false};
    void push(Json j) {
        std::lock_guard lock(mutex);
        if (messages.size() < 256)
            messages.push_back(std::move(j));
    }
};
class Transport final : public ITransport {
    std::shared_ptr<Mailbox> mailbox_ = std::make_shared<Mailbox>();
    std::shared_ptr<rtc::WebSocket> socket_;
    std::shared_ptr<rtc::PeerConnection> peer_;
    std::shared_ptr<rtc::Track> track_;
    std::shared_ptr<rtc::DataChannel> channel_;
    std::shared_ptr<rtc::RtpPacketizationConfig> rtp_;
    std::string url_, room_, secret_, generation_;
    uint64_t socketEpoch_ = 0, peerEpoch_ = 0;
    std::vector<rtc::Candidate> candidates_;
    bool remote_ = false, fatal_ = false, reported_ = false;
    Clock::time_point retry_ = Clock::now(), deadline_ = Clock::now(), lastTelemetry_ = Clock::now();
    unsigned attempts_ = 0;
    uint64_t frames_ = 0, bytes_ = 0, dropped_ = 0;
    BitrateController adaptation_;
    Json receiver_ = Json::object();
    void signal(Json j) {
        if (socket_ && socket_->isOpen()) {
            j["generation"] = generation_;
            socket_->send(j.dump());
        }
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
    }
    void connectSocket() {
        if (socket_)
            socket_->close();
        socket_ = std::make_shared<rtc::WebSocket>();
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
        socket_->open(url_ + "/room/" + room_);
        retry_ = Clock::time_point::max();
    }
    void createPeer() {
        reset();
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
        video.addSSRC(42, "browser-monitor", "display", "video");
        track_ = peer_->addTrack(video);
        rtp_ = std::make_shared<rtc::RtpPacketizationConfig>(42u, "browser-monitor", uint8_t(96), 90000u);
        auto packetizer =
            std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtp_, 1200);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtp_));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([box] { box->idr = true; }));
        track_->setMediaHandler(packetizer);
        track_->onOpen([box] { box->idr = true; });
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
        std::cout << "Negotiating direct WebRTC\n";
    }

  public:
    Transport(std::string server, std::string room, std::string secret)
        : url_(std::move(server)), room_(std::move(room)), secret_(std::move(secret)) {
        while (!url_.empty() && url_.back() == '/')
            url_.pop_back();
        if (url_.starts_with("https://"))
            url_.replace(0, 8, "wss://");
        if (url_.starts_with("http://"))
            url_.replace(0, 7, "ws://");
        if (!url_.starts_with("wss://") && !url_.starts_with("ws://127.0.0.1:") &&
            !url_.starts_with("ws://localhost:"))
            throw std::runtime_error("Signaling requires HTTPS/WSS except localhost");
    }
    ~Transport() {
        reset();
        if (socket_)
            socket_->close();
    }
    void poll() override {
        if (!fatal_ && Clock::now() >= retry_)
            connectSocket();
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
                        Json{{"type", "auth"}, {"version", 1}, {"role", "host"}, {"secret", secret_}}.dump());
                else if (event == "socket-close") {
                    reset();
                    if (retry_ == Clock::time_point::max())
                        retry_ = Clock::now() + std::chrono::milliseconds(
                                                    std::min(10000u, 500u << std::min(attempts_++, 5u)));
                    std::cout << "Signaling disconnected; retrying\n";
                } else if (event == "signal") {
                    auto &body = m["body"];
                    auto type = body.value("type", "");
                    if (type == "authenticated") {
                        attempts_ = 0;
                        std::cout << "Room authenticated; waiting for viewer\n";
                    } else if (type == "error") {
                        std::cerr << "Pairing rejected: " << body.value("code", "unknown") << '\n';
                        fatal_ = true;
                        reset();
                        socket_->close();
                    } else if (type == "ready") {
                        generation_ = body.at("generation").get<std::string>();
                        createPeer();
                    } else if (type == "peer-left") {
                        reset();
                        std::cout << "Viewer left; waiting for reconnect\n";
                    } else if (peer_ && body.value("generation", "") == generation_) {
                        if (type == "answer") {
                            peer_->setRemoteDescription(
                                rtc::Description(body.at("sdp").get<std::string>(), "answer"));
                            remote_ = true;
                            for (auto &c : candidates_)
                                peer_->addRemoteCandidate(c);
                            candidates_.clear();
                        } else if (type == "ice") {
                            rtc::Candidate c(body.at("candidate").get<std::string>(),
                                             body.at("mid").get<std::string>());
                            if (remote_)
                                peer_->addRemoteCandidate(c);
                            else if (candidates_.size() < 128)
                                candidates_.push_back(c);
                        }
                    }
                } else if (event == "local-description")
                    signal({{"type", "offer"}, {"sdp", m["sdp"]}});
                else if (event == "local-ice")
                    signal({{"type", "ice"}, {"candidate", m["candidate"]}, {"mid", m["mid"]}});
                else if (event == "peer-state") {
                    auto s = rtc::PeerConnection::State(m["state"].get<int>());
                    if (s == rtc::PeerConnection::State::Connected) {
                        std::cout << "Direct WebRTC connected\n";
                        reported_ = false;
                    } else if (s == rtc::PeerConnection::State::Disconnected)
                        deadline_ = Clock::now() + std::chrono::seconds(20);
                    else if (s == rtc::PeerConnection::State::Failed) {
                        std::cerr << directFailure << '\n';
                        reported_ = true;
                    }
                } else if (event == "telemetry") {
                    const auto &b = m["body"];
                    if (b.value("type", "") != "telemetry" ||
                        Clock::now() - lastTelemetry_ < std::chrono::milliseconds(500))
                        continue;
                    double loss = b.at("loss").get<double>(), rtt = b.at("rttMs").get<double>(),
                           jitter = b.at("jitterMs").get<double>();
                    adaptation_.update(loss, rtt, jitter);
                    receiver_ = b;
                    lastTelemetry_ = Clock::now();
                }
            } catch (const std::exception &) {
                std::cerr << "Rejected invalid peer message or negotiation failed\n";
            }
        }
        if (peer_ && !connected() && Clock::now() > deadline_ && !reported_) {
            std::cerr << directFailure << '\n';
            reported_ = true;
        }
    }
    bool connected() const override {
        return peer_ && peer_->state() == rtc::PeerConnection::State::Connected && track_ && track_->isOpen();
    }
    bool send(const Encoded &frame) override {
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
    bool consumeKeyframeRequest() override {
        return mailbox_->idr.exchange(false);
    }
    uint32_t targetBitrate() const override {
        return adaptation_.bitrate();
    }
    Json stats() const override {
        return {{"sent_frames", frames_},
                {"encoded_bytes_sent", bytes_},
                {"transport_dropped", dropped_},
                {"transport_buffer_bytes", track_ ? track_->bufferedAmount() : 0},
                {"receiver", receiver_}};
    }
};
} // namespace
std::unique_ptr<ITransport> webRtc(std::string server, std::string room, std::string secret) {
    return std::make_unique<Transport>(std::move(server), std::move(room), std::move(secret));
}
} // namespace bm
