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
class BitrateController {
    uint32_t bitrate_ = 8000000, minimum_ = 1500000, maximum_ = 16000000;
    int good_ = 0;
    double loss_ = 0, rtt_ = 0;

  public:
    BitrateController() = default;
    BitrateController(uint32_t initial, uint32_t minimum, uint32_t maximum)
        : bitrate_(std::clamp(initial, minimum, maximum)), minimum_(minimum), maximum_(maximum) {}
    uint32_t bitrate() const {
        return bitrate_;
    }
    /// The smoothed values the last decision was actually made on, which are what explain it in a log.
    double smoothedLoss() const {
        return loss_;
    }
    double smoothedRtt() const {
        return rtt_;
    }
    uint32_t update(double loss, double rtt, double jitter, std::optional<double> sentBitsPerSecond) {
        if (!std::isfinite(loss) || !std::isfinite(rtt) || !std::isfinite(jitter) || loss < 0 || loss > 1 ||
            rtt < 0 || rtt > 60000 || jitter < 0)
            return bitrate_;
        loss_ = 0.7 * loss_ + 0.3 * loss;
        rtt_ = 0.7 * rtt_ + 0.3 * rtt;
        if (loss > 0.05 || loss_ > 0.025 || rtt > 250 || jitter > 40) {
            bitrate_ = std::max(minimum_, bitrate_ * 75 / 100);
            good_ = 0;
        } else if (loss_ < 0.005 && rtt_ < 120 && jitter < 15 && sentBitsPerSecond &&
                   std::isfinite(*sentBitsPerSecond) && *sentBitsPerSecond >= bitrate_ * 0.75) {
            // A quiet desktop is not a bandwidth probe. Raising its budget recreates Intel's encoder
            // (hundreds of milliseconds without video) without helping the frames it is actually sending.
            // Require sustained use of the current target; congestion cuts above remain unconditional.
            if (++good_ >= 8) {
                bitrate_ = std::min(maximum_, bitrate_ + 250000);
                good_ = 0;
            }
        } else
            good_ = 0;
        return bitrate_;
    }
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
} // namespace lm
