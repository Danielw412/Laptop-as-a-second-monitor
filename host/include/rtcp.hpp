#pragma once
// What the receiver's browser tells the host over RTCP, read on the host itself: receiver reports (loss, jitter and,
// with the host's own sender reports, the round-trip time), NACKs, PLIs and FIRs. These are the network's own
// account of the media path, independent of the telemetry data channel and of the receiver page, so they are there
// even when that channel is not. Portable and header-only so the logic tests cover the parsing.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
namespace lm::rtcp {
struct ReportBlock {
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;   // Since the previous report, in 1/256
    int32_t cumulativeLost = 0; // Since the stream began (24-bit signed)
    uint32_t highestSeq = 0;    // Extended highest sequence number received
    uint32_t jitter = 0;        // Interarrival jitter, RTP timestamp units (1/90000 s for video)
    uint32_t lsr = 0, dlsr = 0; // Middle 32 bits of the last SR's NTP time; delay since it, 1/65536 s
};
struct Feedback {
    std::vector<ReportBlock> reports; // Blocks about `mediaSsrc` only
    unsigned pli = 0, fir = 0, remb = 0, nackMessages = 0, nackedPackets = 0;
    std::vector<uint16_t> nacked; // Sequence numbers asked for again
    bool malformed = false;
};
inline uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint16_t be16(const uint8_t *p) {
    return uint16_t(p[0] << 8 | p[1]);
}
/// Parses one compound RTCP packet (already decrypted). Unknown packet types are skipped.
inline Feedback parse(std::span<const uint8_t> data, uint32_t mediaSsrc) {
    Feedback f;
    size_t at = 0;
    while (at + 4 <= data.size()) {
        const uint8_t *h = data.data() + at;
        if ((h[0] >> 6) != 2) {
            f.malformed = true;
            break;
        }
        const unsigned count = h[0] & 0x1f, type = h[1];
        const size_t length = (size_t(be16(h + 2)) + 1) * 4;
        if (at + length > data.size()) {
            f.malformed = true;
            break;
        }
        const uint8_t *body = h + 4, *end = h + length;
        auto blocks = [&](const uint8_t *p) {
            for (unsigned i = 0; i < count && p + 24 <= end; ++i, p += 24) {
                ReportBlock b;
                b.ssrc = be32(p);
                b.fractionLost = p[4];
                int32_t lost = int32_t(uint32_t(p[5]) << 16 | uint32_t(p[6]) << 8 | p[7]);
                if (lost & 0x800000)
                    lost -= 0x1000000;
                b.cumulativeLost = lost;
                b.highestSeq = be32(p + 8);
                b.jitter = be32(p + 12);
                b.lsr = be32(p + 16);
                b.dlsr = be32(p + 20);
                if (b.ssrc == mediaSsrc)
                    f.reports.push_back(b);
            }
        };
        if (type == 200 && length >= 28)
            blocks(body + 24); // Sender SSRC (4) and sender info (20) precede the blocks
        else if (type == 201 && length >= 8)
            blocks(body + 4);
        else if (type == 205 && count == 1 && length >= 12) {
            ++f.nackMessages;
            for (const uint8_t *p = body + 8; p + 4 <= end; p += 4) {
                const uint16_t pid = be16(p), blp = be16(p + 2);
                f.nacked.push_back(pid);
                for (int bit = 0; bit < 16; ++bit)
                    if (blp & (1u << bit))
                        f.nacked.push_back(uint16_t(pid + bit + 1));
            }
            f.nackedPackets = unsigned(f.nacked.size());
        } else if (type == 206) {
            if (count == 1)
                ++f.pli;
            else if (count == 4)
                ++f.fir;
            else if (count == 15)
                ++f.remb;
        }
        at += length;
    }
    return f;
}
/// Middle 32 bits of the NTP time of `t` (1/65536 s), the clock libdatachannel's sender reports carry.
inline uint32_t ntpMiddle(std::chrono::system_clock::time_point t) {
    const double seconds = std::chrono::duration<double>(t.time_since_epoch()).count() + 2208988800.0;
    return uint32_t(uint64_t(std::floor(seconds * 65536.0)) & 0xffffffffu);
}
/// Round-trip time from a report block that echoes one of our sender reports (RFC 3550 6.4.1).
inline std::optional<double> rttMs(uint32_t arrivalMiddle, uint32_t lsr, uint32_t dlsr) {
    if (!lsr)
        return std::nullopt;
    const uint32_t rtt = arrivalMiddle - lsr - dlsr;
    if (rtt > 65536u * 60) // More than a minute: the clocks or the echo are not what we think
        return std::nullopt;
    return rtt * 1000.0 / 65536.0;
}
/// Loss between two consecutive reports from the cumulative counters (the per-report fraction is rounded to 1/256
/// and covers whatever interval the receiver chose).
struct IntervalLoss {
    uint64_t expected = 0;
    int64_t lost = 0;
    double fraction() const {
        return expected ? std::clamp(double(lost) / double(expected), 0.0, 1.0) : 0.0;
    }
};
inline std::optional<IntervalLoss> lossBetween(const ReportBlock &previous, const ReportBlock &current) {
    if (current.highestSeq < previous.highestSeq)
        return std::nullopt; // A new stream (SSRC reuse after renegotiation)
    IntervalLoss l;
    l.expected = current.highestSeq - previous.highestSeq;
    l.lost = int64_t(current.cumulativeLost) - int64_t(previous.cumulativeLost);
    return l;
}
} // namespace lm::rtcp
