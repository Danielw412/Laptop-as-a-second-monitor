#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
namespace lm {
/// One network measurement, about once a second. Every field is optional: a receiver report can be missing or late,
/// and a missing value must never read as a clean link or as congestion.
struct NetworkSample {
    std::optional<double> loss;       // Fraction of packets lost over the interval (0..1)
    std::optional<uint64_t> packets;  // Packets the loss fraction is over; a handful of packets proves nothing
    std::optional<double> rttMs;      // Round-trip time
    std::optional<double> sentBps;    // What was actually sent over the interval
};
/// Chooses the encoder's target bitrate from real congestion evidence: packet loss over enough packets, or a
/// round-trip time well above the link's own baseline (a queue building up). Jitter and the browser's REMB are
/// deliberately not inputs: on a clean LAN both swing with frame sizes (a 200 KB keyframe arrives as a burst),
/// and acting on them cut a loss-free stream from 8 to 1.9 Mbps, where the encoder runs at QP 50 and damages
/// every changed region. After a cut the target recovers once the link has been clean for a while, so one bad
/// second can never hold the picture at the floor for the rest of a session.
class NetworkAdaptation {
  public:
    struct Decision {
        uint32_t bitrate = 0;
        bool changed = false, congested = false;
        const char *reason = ""; // "loss", "delay", "recovery", "probe" or ""
    };
    NetworkAdaptation(uint32_t initial = 8000000, uint32_t minimum = 1500000, uint32_t maximum = 16000000)
        : target_(std::clamp(initial, minimum, maximum)), initial_(target_), minimum_(minimum), maximum_(maximum) {}
    uint32_t bitrate() const {
        return target_;
    }
    double smoothedLoss() const {
        return loss_;
    }
    std::optional<double> baselineRttMs() const {
        return rtts_.empty() ? std::nullopt : std::optional<double>(*std::min_element(rtts_.begin(), rtts_.end()));
    }
    unsigned cleanSeconds() const {
        return clean_;
    }
    Decision update(const NetworkSample &s) {
        Decision d;
        const bool lossKnown = s.loss && std::isfinite(*s.loss) && *s.loss >= 0 && *s.loss <= 1 && s.packets;
        const bool rttKnown = s.rttMs && std::isfinite(*s.rttMs) && *s.rttMs >= 0 && *s.rttMs < 60000;
        if (s.sentBps && std::isfinite(*s.sentBps) && *s.sentBps >= 0) {
            sent_[sentAt_++ % sent_.size()] = *s.sentBps;
            sentSamples_ = std::min(sentSamples_ + 1, sent_.size());
        }
        bool lossy = false, delayed = false, lossClean = true;
        if (lossKnown && *s.packets >= 20) {
            loss_ = 0.7 * loss_ + 0.3 * *s.loss;
            // Heavy loss now, or steady loss that is still happening. The smoothed value alone would keep cutting
            // for seconds after the loss has stopped.
            lossy = *s.packets >= 50 && (*s.loss >= 0.05 || (*s.loss >= 0.01 && loss_ >= 0.02));
            lossClean = *s.loss < 0.005;
        }
        if (rttKnown) {
            const auto base = baselineRttMs();
            rtts_.push_back(*s.rttMs);
            if (rtts_.size() > 60)
                rtts_.erase(rtts_.begin());
            // A standing queue: well above this link's own recent minimum for three seconds in a row. Shorter
            // spikes are a receiver that stalled and sent its reports late (seen: 714 and 484 ms on loopback while
            // the receiving browser caught up after an encoder rebuild), not a queue on the path.
            const bool high = base && *s.rttMs >= *base + std::max(80.0, *base);
            highStreak_ = high ? highStreak_ + 1 : 0;
            delayed = highStreak_ >= 3;
        }
        if (holdoff_)
            --holdoff_;
        if (lossy || delayed) {
            clean_ = 0;
            d.congested = true;
            d.reason = lossy ? "loss" : "delay";
            if (!holdoff_) {
                // Cut below what was actually being sent (a target far above an idle desktop's usage says
                // nothing about the link), then give the queue time to drain before judging again.
                const double peak = recentSentPeak();
                const double basis = peak > 0 ? std::min<double>(target_, std::max(peak, double(minimum_))) : target_;
                const auto next = std::max(minimum_, uint32_t(basis * 0.75));
                if (next < target_) {
                    target_ = next;
                    d.changed = true;
                }
                holdoff_ = 3;
            }
        } else if (!lossClean)
            clean_ = 0; // Some loss, not enough to act on: not a reason to raise either
        else if (lossKnown || rttKnown) {
            // Recovery in large, rare steps: each change rebuilds Intel's encoder, a visible hitch of 250-400 ms.
            // Back to the configured bitrate after 10 clean seconds; above it only after 20, and only while the
            // encoder is actually using most of what it has.
            ++clean_;
            uint32_t next = target_;
            if (target_ < initial_ && clean_ >= 10) {
                next = std::min(initial_, uint32_t(std::llround(target_ * 1.3)));
                d.reason = "recovery";
            } else if (target_ >= initial_ && target_ < maximum_ && clean_ >= 20 &&
                       recentSentPeak() >= 0.75 * target_) {
                next = std::min(maximum_, uint32_t(std::llround(target_ * 1.4)));
                d.reason = "probe";
            }
            if (next != target_) {
                target_ = next;
                d.changed = true;
                clean_ = 0;
            }
        }
        d.bitrate = target_;
        return d;
    }

  private:
    double recentSentPeak() const {
        double peak = 0;
        for (size_t i = 0; i < sentSamples_; ++i)
            peak = std::max(peak, sent_[i]);
        return peak;
    }
    uint32_t target_, initial_, minimum_, maximum_;
    double loss_ = 0;
    std::vector<double> rtts_;
    unsigned highStreak_ = 0, clean_ = 0, holdoff_ = 0;
    std::array<double, 10> sent_{};
    size_t sentAt_ = 0, sentSamples_ = 0;
};
/// What the encoder should run at for a target bitrate. Measured on this machine's Quick Sync encoder with a busy
/// synthetic desktop: halving the frame rate is worth roughly a doubling of bitrate (4 Mbps at 30 fps looks like
/// 8 Mbps at 60), and below ~2.5 Mbps even 30 fps 1080p frames sit at QP 45+. So under pressure the ladder gives up
/// frames first, then resolution, and never lets 1080p60 starve. Hysteresis keeps it from flapping.
struct StreamShape {
    unsigned fps = 60, height = 1080;
    bool operator==(const StreamShape &) const = default;
};
inline StreamShape shapeFor(uint32_t bitrate, uint32_t nominal, unsigned maxFps, const StreamShape &current) {
    StreamShape s = current;
    const uint32_t fullRate = std::min<uint32_t>(6000000, nominal * 3 / 4);
    if (maxFps <= 30)
        s.fps = maxFps;
    else if (bitrate < fullRate)
        s.fps = 30;
    else if (bitrate >= fullRate + fullRate / 5 || current.fps == maxFps)
        s.fps = maxFps;
    if (bitrate < 2500000)
        s.height = 720;
    else if (bitrate >= 3000000)
        s.height = 1080;
    return s;
}
/// When to force an IDR. Receiver requests (PLI/FIR), a viewer joining, a broken reference chain and recovery all
/// ask for one; requests that arrive while one is already on its way, or within `minInterval` of the last, are
/// folded into it (a lossy link otherwise answers every PLI with a 200 KB frame and makes itself worse). A slow
/// periodic refresh is the safety net for a receiver that went wrong without saying so.
class KeyframePolicy {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    enum Reason { Receiver, Join, Transport, Host, Periodic, ChainBroken, Recovery, kReasons };
    static const char *name(Reason r) {
        static const char *const names[] = {"receiver", "join", "transport", "host", "periodic", "chain", "recovery"};
        return names[r];
    }
    explicit KeyframePolicy(std::chrono::milliseconds minInterval = std::chrono::milliseconds(300),
                            std::chrono::seconds periodic = std::chrono::seconds(20))
        : minInterval_(minInterval), periodic_(periodic) {}
    /// Records a request. True when an IDR should be forced now; false when it was folded into one in flight or
    /// deferred until minInterval has passed (poll() then fires it).
    bool request(Reason r, TimePoint now) {
        ++requested_[r];
        if (inFlight_) {
            ++coalesced_;
            return false;
        }
        if (lastForced_ && now - *lastForced_ < minInterval_) {
            ++coalesced_;
            deferred_ = true;
            return false;
        }
        force(now);
        return true;
    }
    /// True when a deferred request is due, or the periodic refresh is (only while `streaming`).
    bool poll(TimePoint now, bool streaming) {
        if (inFlight_) {
            // An IDR asked for but never seen (the encoder rebuilt, say): the request still stands, so ask again
            // rather than wait forever.
            if (now - *lastForced_ <= std::chrono::seconds(2))
                return false;
            deferred_ = false;
            force(now);
            return true;
        }
        if (deferred_ && (!lastForced_ || now - *lastForced_ >= minInterval_)) {
            deferred_ = false;
            force(now);
            return true;
        }
        if (streaming && lastIdr_ && now - *lastIdr_ >= periodic_) {
            ++requested_[Periodic];
            force(now);
            return true;
        }
        return false;
    }
    /// An IDR left the encoder (forced or not).
    void produced(TimePoint now) {
        lastIdr_ = now;
        inFlight_ = false;
    }
    /// A new encoder starts with an IDR of its own; nothing asked for before still applies.
    void reset() {
        inFlight_ = deferred_ = false;
    }
    uint64_t requested(Reason r) const {
        return requested_[r];
    }
    uint64_t forced() const {
        return forced_;
    }
    uint64_t coalesced() const {
        return coalesced_;
    }
    std::optional<TimePoint> lastIdr() const {
        return lastIdr_;
    }

  private:
    void force(TimePoint now) {
        lastForced_ = now;
        inFlight_ = true;
        ++forced_;
    }
    std::chrono::milliseconds minInterval_;
    std::chrono::seconds periodic_;
    std::optional<TimePoint> lastForced_, lastIdr_;
    bool inFlight_ = false, deferred_ = false;
    std::array<uint64_t, kReasons> requested_{};
    uint64_t forced_ = 0, coalesced_ = 0;
};
template <size_t N = 4096> class Samples {
    std::array<double, N> values_{};
    size_t count_ = 0, total_ = 0;

  public:
    size_t count() const { return count_; }
    void add(double v) {
        values_[total_++ % N] = v;
        count_ = std::min(count_ + 1, N);
    }
    double mean() const {
        double s = 0;
        for (size_t i = 0; i < count_; ++i)
            s += values_[i];
        return count_ ? s / count_ : 0;
    }
    double percentile(double q) const {
        if (!count_)
            return 0;
        auto copy = values_;
        auto i = std::min(count_ - 1, size_t(std::ceil(q * count_) - 1));
        std::nth_element(copy.begin(), copy.begin() + i, copy.begin() + count_);
        return copy[i];
    }
    /// Two percentiles from one copy of the ring (a copy is 32 KB at the default size).
    std::pair<double, double> percentiles(double a, double b) const {
        if (!count_)
            return {0, 0};
        auto copy = values_;
        auto index = [&](double q) { return std::min(count_ - 1, size_t(std::ceil(q * count_) - 1)); };
        const auto i = index(a), j = index(b);
        std::nth_element(copy.begin(), copy.begin() + i, copy.begin() + count_);
        const double first = copy[i];
        std::nth_element(copy.begin(), copy.begin() + j, copy.begin() + count_);
        return {first, copy[j]};
    }
    void clear() {
        count_ = total_ = 0;
    }
};
// A per-frame latency: the same rolling ring as every other frame-path sample (so its mean and percentiles lag a
// change by about a minute at 60 fps), plus the worst value and count of the current reporting interval, so a
// one-second spike is visible rather than averaged into the ring.
struct LatencyTrack {
    Samples<> rolling;
    double intervalMax = 0;
    uint64_t intervalCount = 0;
    void add(double ms) {
        rolling.add(ms);
        intervalMax = intervalCount ? std::max(intervalMax, ms) : ms;
        ++intervalCount;
    }
    void endInterval() {
        intervalMax = 0;
        intervalCount = 0;
    }
};
// When new source frames arrive, on the host's steady clock. no_change alone cannot tell "nothing on the screen
// changed" from "capture stopped delivering frames"; the spacing of frames within an interval and the time since
// the last one can. Reporting only: nothing may decide anything from these.
class SourceActivity {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    struct Interval {
        uint64_t frames = 0, noChange = 0;
        // Gaps between consecutive source frames that ended inside the interval. Empty when fewer than two
        // frames bracket a gap: a stall that is still going on shows in sinceLastMs, not here.
        std::optional<double> gapP95, gapMax;
        // Time from the newest source frame to the report. Empty before the first frame, and while capture is
        // not being polled (no receiver), because then no frame could have been taken.
        std::optional<double> sinceLastMs;
    };
    void frame(TimePoint at) {
        if (last_) {
            const double gap = milliseconds(at - *last_);
            gaps_.add(gap);
            gapMax_ = gaps_.count() == 1 ? gap : std::max(gapMax_, gap);
        }
        last_ = at;
        ++frames_;
    }
    void noChange() {
        ++noChange_;
    }
    /// Capture stopped being polled: the next frame's gap would measure that pause, not the source.
    void pause() {
        last_.reset();
    }
    /// The interval that just ended; starts the next one.
    Interval take(TimePoint now) {
        Interval i;
        i.frames = frames_;
        i.noChange = noChange_;
        if (gaps_.count()) {
            i.gapP95 = gaps_.percentile(.95);
            i.gapMax = gapMax_;
        }
        if (last_)
            i.sinceLastMs = std::max(0.0, milliseconds(now - *last_));
        gaps_.clear();
        gapMax_ = 0;
        frames_ = noChange_ = 0;
        return i;
    }

  private:
    static double milliseconds(std::chrono::steady_clock::duration d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }
    Samples<256> gaps_; // More than 256 frames in one second only happens above 240 Hz; gapMax_ stays exact
    double gapMax_ = 0;
    std::optional<TimePoint> last_;
    uint64_t frames_ = 0, noChange_ = 0;
};
/// Share of wall time a GPU engine was busy, from two readings of its cumulative running time in 100 ns units (as
/// D3DKMT reports it). Empty when the counter went backwards (adapter reset) or no time passed. Capped at 100:
/// the kernel books a DMA packet's time when it completes, so one reading can include a little of the previous
/// interval.
inline std::optional<double> busyPercent(int64_t runningBefore, int64_t runningNow, double wallSeconds) {
    if (!(wallSeconds > 0) || runningNow < runningBefore)
        return std::nullopt;
    return std::min(100.0, double(runningNow - runningBefore) / (wallSeconds * 1e7) * 100);
}
// Append AVCC length-prefixed access units as Annex B (already Annex B input is copied as is); never
// reinterpret malformed lengths.
inline void appendAnnexB(std::vector<uint8_t> &out, std::span<const uint8_t> bytes) {
    if (bytes.size() >= 4 && bytes[0] == 0 && bytes[1] == 0 &&
        (bytes[2] == 1 || (bytes[2] == 0 && bytes[3] == 1))) {
        out.insert(out.end(), bytes.begin(), bytes.end());
        return;
    }
    out.reserve(out.size() + bytes.size());
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < 4)
            throw std::runtime_error("Truncated H264 length");
        uint32_t n = uint32_t(bytes[offset]) << 24 | uint32_t(bytes[offset + 1]) << 16 |
                     uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
        offset += 4;
        if (!n || n > bytes.size() - offset)
            throw std::runtime_error("Invalid H264 NAL length");
        out.insert(out.end(), {0, 0, 0, 1});
        out.insert(out.end(), bytes.begin() + offset, bytes.begin() + offset + n);
        offset += n;
    }
}
inline std::vector<uint8_t> annexB(std::span<const uint8_t> bytes) {
    std::vector<uint8_t> out;
    appendAnnexB(out, bytes);
    return out;
}
inline uint32_t rtpTimestamp(int64_t sample100ns) {
    return uint32_t((uint64_t(sample100ns) * 9) / 1000);
}
/// The sample time for the next frame handed to the encoder: its own stamp, but always at least one 90 kHz RTP tick
/// after the previous frame. Two frames with the same RTP timestamp are one frame to a WebRTC receiver (it
/// assembles H.264 frames by timestamp), and the old "+1" (100 ns) guard produced exactly that whenever a repeat
/// or keyframe answer followed a frame stamped slightly in the future by the compositor.
inline int64_t nextSampleTime(int64_t candidate, int64_t previous) {
    constexpr int64_t kOneRtpTick = 112; // 100 ns units; 1/90000 s is 111.1
    return previous < 0 ? std::max<int64_t>(0, candidate) : std::max(candidate, previous + kOneRtpTick);
}
} // namespace lm
