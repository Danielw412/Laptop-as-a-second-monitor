#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <stdexcept>
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
    uint32_t update(double loss, double rtt, double jitter) {
        if (!std::isfinite(loss) || !std::isfinite(rtt) || !std::isfinite(jitter) || loss < 0 || loss > 1 ||
            rtt < 0 || rtt > 60000 || jitter < 0)
            return bitrate_;
        loss_ = 0.7 * loss_ + 0.3 * loss;
        rtt_ = 0.7 * rtt_ + 0.3 * rtt;
        if (loss > 0.05 || loss_ > 0.025 || rtt > 250 || jitter > 40) {
            bitrate_ = std::max(minimum_, bitrate_ * 75 / 100);
            good_ = 0;
        } else if (loss_ < 0.005 && rtt_ < 120 && jitter < 15) {
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
};
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
