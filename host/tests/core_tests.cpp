#include "core.hpp"
#include "h264.hpp"
#include "probe.hpp"
#include "rtcp.hpp"
#include <iostream>
#include <limits>
void requireAt(bool condition, int line) {
    if (!condition) {
        std::cerr << "core test failed at line " << line << std::endl;
        std::exit(1);
    }
}
#define require(condition) requireAt(bool(condition), __LINE__)
namespace {
// Writes H.264 syntax elements so the parser can be tested on streams with known contents.
struct BitWriter {
    std::vector<uint8_t> bytes;
    int bit = 0;
    void put(uint32_t value, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (bit == 0)
                bytes.push_back(0);
            if ((value >> i) & 1)
                bytes.back() |= uint8_t(0x80 >> bit);
            bit = (bit + 1) % 8;
        }
    }
    void flag(bool b) {
        put(b ? 1 : 0, 1);
    }
    void ue(uint32_t v) {
        const uint32_t x = v + 1;
        int n = 0;
        while ((x >> n) > 1)
            ++n;
        put(0, n);
        put(x, n + 1);
    }
    void se(int32_t v) {
        ue(v > 0 ? uint32_t(2 * v - 1) : uint32_t(-2 * v));
    }
    std::vector<uint8_t> finish() { // rbsp_stop_one_bit and alignment
        put(1, 1);
        while (bit)
            put(0, 1);
        return bytes;
    }
};
// Start code, NAL header, payload with emulation prevention bytes inserted.
void appendNal(std::vector<uint8_t> &out, uint8_t header, const std::vector<uint8_t> &rbsp) {
    out.insert(out.end(), {0, 0, 0, 1, header});
    int zeros = 0;
    for (auto b : rbsp) {
        if (zeros >= 2 && b <= 3) {
            out.push_back(3);
            zeros = 0;
        }
        out.push_back(b);
        zeros = b == 0 ? zeros + 1 : 0;
    }
}
std::vector<uint8_t> sps() {
    BitWriter w;
    w.put(66, 8); // Baseline
    w.put(0xc0, 8);
    w.put(42, 8);
    w.ue(0);    // sps id
    w.ue(0);    // log2_max_frame_num_minus4: 16 frame numbers
    w.ue(2);    // pic_order_cnt_type 2
    w.ue(1);    // max_num_ref_frames
    w.flag(false);
    w.ue(119);  // 120 macroblocks wide
    w.ue(67);   // 68 high (1088)
    w.flag(true);  // frame_mbs_only
    w.flag(true);  // direct_8x8_inference
    w.flag(true);  // frame_cropping
    w.ue(0);
    w.ue(0);
    w.ue(0);
    w.ue(4);       // 8 rows off the bottom: 1080
    w.flag(true);  // vui
    w.flag(false); // aspect ratio
    w.flag(false); // overscan
    w.flag(true);  // video_signal_type
    w.put(5, 3);
    w.flag(false); // limited range
    w.flag(true);  // colour description
    w.put(1, 8);
    w.put(1, 8);
    w.put(1, 8); // BT.709 matrix
    return w.finish();
}
std::vector<uint8_t> pps() {
    BitWriter w;
    w.ue(0);
    w.ue(0);
    w.flag(false); // CAVLC
    w.flag(false);
    w.ue(0);       // one slice group
    w.ue(0);
    w.ue(0);
    w.flag(false);
    w.put(0, 2);
    w.se(4);       // pic_init_qp 30
    w.se(0);
    w.se(0);
    w.flag(true);  // deblocking_filter_control_present
    w.flag(false);
    w.flag(false);
    return w.finish();
}
std::vector<uint8_t> slice(bool idr, unsigned frameNum, int qpDelta, unsigned firstMb = 0) {
    BitWriter w;
    w.ue(firstMb);
    w.ue(idr ? 7 : 5); // I or P, all slices of the picture alike
    w.ue(0);
    w.put(frameNum, 4);
    if (idr)
        w.ue(0); // idr_pic_id
    if (!idr) {
        w.flag(false); // num_ref_idx_active_override
        w.flag(false); // ref_pic_list_modification_flag_l0
    }
    // dec_ref_pic_marking (nal_ref_idc != 0)
    w.flag(false);
    if (idr)
        w.flag(false);
    w.se(qpDelta);
    w.ue(idr ? 1 : 0); // disable_deblocking_filter_idc
    if (!idr) {
        w.se(0);
        w.se(0);
    }
    w.put(0x5a5a, 16); // Some slice data
    return w.finish();
}
std::vector<uint8_t> accessUnit(bool idr, unsigned frameNum, int qpDelta, unsigned slices = 1) {
    std::vector<uint8_t> au;
    if (idr) {
        appendNal(au, 0x67, sps());
        appendNal(au, 0x68, pps());
    }
    for (unsigned i = 0; i < slices; ++i)
        appendNal(au, idr ? 0x65 : 0x41, slice(idr, frameNum, qpDelta, i * 1000));
    return au;
}
std::vector<uint8_t> be(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out;
    for (auto w : words)
        out.insert(out.end(), {uint8_t(w >> 24), uint8_t(w >> 16), uint8_t(w >> 8), uint8_t(w)});
    return out;
}
} // namespace
int main() {
    using namespace std::chrono_literals;
    // Network adaptation reacts to loss and to a standing queue, never to jitter alone, and recovers afterwards.
    {
        lm::NetworkAdaptation a(8000000, 1500000, 16000000);
        for (int i = 0; i < 30; ++i) // A clean LAN with a big keyframe now and then: nothing to cut
            require(!a.update({0.0, 400, 5.0 + (i % 3), 3000000.0}).changed);
        require(a.bitrate() == 8000000);
        auto d = a.update({0.10, 400, 6.0, 8000000.0}); // 10% loss over 400 packets: congestion
        require(d.changed && d.congested && std::string(d.reason) == "loss" && a.bitrate() == 6000000);
        d = a.update({0.10, 400, 6.0, 8000000.0}); // Hold-off: the queue gets time to drain before another cut
        require(!d.changed && d.congested && a.bitrate() == 6000000);
        a.update({0.10, 400, 6.0, 6000000.0});
        a.update({0.10, 400, 6.0, 6000000.0});
        require(a.bitrate() == 4500000); // Next cut after the hold-off
        // Ten clean seconds bring it back up towards the configured bitrate, in one large step.
        for (int i = 0; i < 9; ++i)
            require(!a.update({0.0, 400, 6.0, 4000000.0}).changed);
        d = a.update({0.0, 400, 6.0, 4000000.0});
        require(d.changed && std::string(d.reason) == "recovery" && a.bitrate() == 5850000);
        for (int i = 0; i < 10; ++i)
            a.update({0.0, 400, 6.0, 4000000.0});
        require(a.bitrate() == 7605000);
        for (int i = 0; i < 10; ++i)
            a.update({0.0, 400, 6.0, 4000000.0});
        require(a.bitrate() == 8000000); // Capped at the configured bitrate...
        for (int i = 0; i < 30; ++i)
            a.update({0.0, 400, 6.0, 2000000.0});
        require(a.bitrate() == 8000000); // ...and above it only when the encoder actually needs more
        a.update({0.0, 400, 6.0, 7000000.0}); // Clean for 30 s already: the first busy second may probe up
        require(a.bitrate() == 11200000);
        for (int i = 0; i < 19; ++i)
            a.update({0.0, 400, 6.0, 10000000.0});
        require(a.bitrate() == 11200000); // Then twenty clean seconds before the next step
        a.update({0.0, 400, 6.0, 10000000.0});
        require(a.bitrate() == 15680000);
    }
    {
        // A handful of packets proves nothing, and nonsense is ignored.
        lm::NetworkAdaptation a(8000000, 1500000, 16000000);
        require(!a.update({0.5, 10, 5.0, 100000.0}).congested);
        require(!a.update({std::numeric_limits<double>::quiet_NaN(), 400, -1.0, std::nullopt}).congested);
        require(!a.update({std::nullopt, std::nullopt, std::nullopt, std::nullopt}).changed);
        require(a.bitrate() == 8000000);
        // Queueing delay: RTT far above this link's own baseline three seconds in a row is congestion; one or two
        // seconds is a receiver that stalled and reported late.
        for (int i = 0; i < 5; ++i)
            a.update({0.0, 400, 5.0, 8000000.0});
        require(!a.update({0.0, 400, 700.0, 8000000.0}).congested);
        require(!a.update({0.0, 400, 480.0, 8000000.0}).congested);
        a.update({0.0, 400, 6.0, 8000000.0});
        require(!a.update({0.0, 400, 250.0, 8000000.0}).congested);
        require(!a.update({0.0, 400, 250.0, 8000000.0}).congested);
        auto d = a.update({0.0, 400, 260.0, 8000000.0});
        require(d.congested && std::string(d.reason) == "delay" && a.bitrate() == 6000000);
        // A cut is taken from what was really sent, not from a target an idle desktop never used.
        lm::NetworkAdaptation idle(16000000, 1500000, 16000000);
        idle.update({0.0, 400, 5.0, 2000000.0});
        idle.update({0.2, 400, 5.0, 2000000.0});
        require(idle.bitrate() == 1500000); // 0.75 x 2 Mbps, floored at the minimum
    }
    // The stream shape gives up frames before it lets 60 fps starve, then resolution; with hysteresis.
    {
        lm::StreamShape full{60, 1080};
        require(lm::shapeFor(8000000, 8000000, 60, full) == full);
        const auto thirty = lm::shapeFor(5000000, 8000000, 60, full);
        require(thirty.fps == 30 && thirty.height == 1080);
        require(lm::shapeFor(6500000, 8000000, 60, thirty).fps == 30); // Not back to 60 until 7.2 Mbps
        require(lm::shapeFor(7500000, 8000000, 60, thirty).fps == 60);
        const auto small = lm::shapeFor(2000000, 8000000, 60, thirty);
        require(small.fps == 30 && small.height == 720);
        require(lm::shapeFor(2700000, 8000000, 60, small).height == 720);
        require(lm::shapeFor(3200000, 8000000, 60, small).height == 1080);
        require(lm::shapeFor(16000000, 8000000, 30, full).fps == 30); // The user's 30 fps setting is a ceiling
        require(lm::shapeFor(4000000, 5000000, 60, full).fps == 60);  // Efficient preset: 60 fps down to 3.75 Mbps
    }
    // Keyframe policy: requests fold into one in flight, a burst is spaced out, and a slow refresh backs it up.
    {
        const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
        lm::KeyframePolicy k(300ms, 30s);
        require(k.request(lm::KeyframePolicy::Join, t0));
        require(!k.request(lm::KeyframePolicy::Receiver, t0 + 10ms)); // Already on its way
        k.produced(t0 + 30ms);
        require(!k.request(lm::KeyframePolicy::Receiver, t0 + 100ms)); // Too soon after the last: deferred
        require(!k.poll(t0 + 200ms, true));
        require(k.poll(t0 + 350ms, true)); // ...and fired once the interval has passed
        k.produced(t0 + 360ms);
        require(!k.poll(t0 + 20s, true));
        require(k.poll(t0 + 31s, true));   // Periodic refresh while streaming
        require(!k.poll(t0 + 31s + 10ms, true));
        k.produced(t0 + 31s + 50ms);
        require(!k.poll(t0 + 70s, false)); // Never while nothing is streaming
        require(k.forced() == 3 && k.coalesced() == 2 && k.requested(lm::KeyframePolicy::Receiver) == 2);
        // An IDR asked for but never produced (the encoder was rebuilt) is asked for again.
        lm::KeyframePolicy lost(300ms, 30s);
        require(lost.request(lm::KeyframePolicy::Receiver, t0));
        require(!lost.request(lm::KeyframePolicy::Receiver, t0 + 1s));
        require(lost.poll(t0 + 3s, true));
    }
    // Sample times are always at least one RTP tick apart, so no two frames share an RTP timestamp.
    {
        int64_t previous = -1;
        const int64_t stamps[] = {1000000, 1000000, 1000001, 1000050, 900000, 1166667};
        uint32_t lastRtp = 0;
        for (auto stamp : stamps) {
            const auto t = lm::nextSampleTime(stamp, previous);
            require(previous < 0 || lm::rtpTimestamp(t) > lastRtp);
            lastRtp = lm::rtpTimestamp(t);
            previous = t;
        }
        require(lm::nextSampleTime(1166667, 1000000) == 1166667); // Normal spacing is left alone
    }
    // H.264 inspection: parameter sets, slice QP, IDR, slice count, and the reference chain across frames.
    {
        lm::h264::Parser parser;
        const auto idr = parser.parse(accessUnit(true, 0, -2));
        require(idr.sps && idr.pps && idr.idr && idr.reference && idr.slices == 1 && !idr.unparsedSlices);
        require(idr.qpMin && *idr.qpMin == 28 && idr.frameNum && *idr.frameNum == 0 && *idr.maxFrameNum == 16);
        require(idr.sliceType && *idr.sliceType == 2 && idr.deblockingDisabledSlices == 1);
        const auto *s = parser.activeSps();
        require(s && s->width == 1920 && s->height == 1080 && s->fullRange && !*s->fullRange && s->matrix &&
                *s->matrix == 1);
        const auto p1 = parser.parse(accessUnit(false, 1, 5, 3));
        require(!p1.idr && p1.slices == 3 && p1.qpMin == 35 && p1.qpMax == 35 && *p1.sliceType == 0 &&
                *p1.frameNum == 1);
        lm::h264::ReferenceChain chain;
        require(chain.follows(idr) && chain.follows(p1));
        require(chain.follows(parser.parse(accessUnit(false, 2, 0))));
        require(!chain.follows(parser.parse(accessUnit(false, 4, 0)))); // frame 3 never arrived
        require(chain.follows(parser.parse(accessUnit(true, 0, 0))));   // An IDR starts over
        // A slice whose parameter set was never seen is counted, not guessed at.
        lm::h264::Parser cold;
        const auto orphan = cold.parse(accessUnit(false, 1, 0));
        require(orphan.slices == 1 && orphan.unparsedSlices == 1 && !orphan.qpMin);
        // Emulation prevention bytes are removed before reading.
        lm::h264::BitReader r(std::vector<uint8_t>{0, 0, 3, 1, 0x80});
        require(r.bits(8) == 0 && r.bits(8) == 0 && r.bits(8) == 1 && r.flag() && r.ok());
    }
    // RTCP: receiver report blocks for our stream only, NACKed sequence numbers, PLI and FIR, and RTT.
    {
        auto rr = be({0x82C9000D, 0x00000001,                                                // RR, 2 blocks
                      42, 0x0A000005, 1000, 90 * 7, 0x12345678, 0x00001000,                   // ours
                      7, 0x00000001, 5, 0, 0, 0});                                             // someone else's
        auto nack = be({0x81CD0003, 1, 42, (100u << 16) | 0x0005});                           // 100, 101, 103
        auto pli = be({0x81CE0002, 1, 42});
        auto fir = be({0x84CE0004, 1, 0, 42, 0x01000000});
        std::vector<uint8_t> compound = rr;
        compound.insert(compound.end(), nack.begin(), nack.end());
        compound.insert(compound.end(), pli.begin(), pli.end());
        compound.insert(compound.end(), fir.begin(), fir.end());
        const auto f = lm::rtcp::parse(compound, 42);
        require(!f.malformed && f.reports.size() == 1 && f.pli == 1 && f.fir == 1 && f.nackMessages == 1);
        require(f.nacked == std::vector<uint16_t>({100, 101, 103}) && f.nackedPackets == 3);
        const auto &b = f.reports[0];
        require(b.fractionLost == 10 && b.cumulativeLost == 5 && b.highestSeq == 1000 && b.jitter == 630);
        const auto rtt = lm::rtcp::rttMs(0x12345678u + 0x1000u + 6554u, b.lsr, b.dlsr);
        require(rtt && std::abs(*rtt - 100.0) < 0.1);
        require(!lm::rtcp::rttMs(123, 0, 0)); // No sender report echoed yet
        lm::rtcp::ReportBlock later = b;
        later.highestSeq = 1400;
        later.cumulativeLost = 25;
        const auto loss = lm::rtcp::lossBetween(b, later);
        require(loss && loss->expected == 400 && loss->lost == 20 && std::abs(loss->fraction() - 0.05) < 1e-9);
        require(lm::rtcp::parse(std::vector<uint8_t>{0x80, 0xc9, 0x00, 0x09}, 42).malformed); // Length past the end
    }
    // Quality probes: the receiver's grid is compared after fitting brightness and contrast, so a different colour
    // matrix is not damage, a wrong band is, and lost detail is quantization.
    {
        std::vector<uint8_t> plane(64 * 36);
        for (unsigned y = 0; y < 36; ++y)
            for (unsigned x = 0; x < 64; ++x) // Brightness varying by cell, with texture inside each cell
                plane[y * 64 + x] = uint8_t(16 + ((x / 8) * 23 + (y / 6) * 17) % 170 + (x % 2) * 40);
        const auto source = lm::lumaCells(plane.data(), 64, 36, 64, 8, 6);
        require(source.valid() && source.columns == 8 && source.rows == 6);
        require(lm::compareCells(source, source).verdict() == "match");
        auto shifted = source;
        for (auto &m : shifted.mean)
            m = uint8_t(std::min(255, int(m * 1.05 + 6)));
        require(lm::compareCells(source, shifted).verdict() == "match");
        auto band = source;
        for (unsigned c = 0; c < 8; ++c)
            band.mean[3 * 8 + c] = uint8_t(band.mean[3 * 8 + c] > 128 ? 20 : 230); // Row 3 shows something else
        const auto broken = lm::compareCells(source, band);
        require(broken.verdict() == "corrupted" && broken.firstBadRow == 3 && broken.lastBadRow == 3);
        auto flat = source;
        for (auto &d : flat.detail)
            d = uint8_t(d / 4);
        require(lm::compareCells(source, flat).verdict() == "quantized");
        require(lm::compareCells(source, lm::CellStats{}).verdict() == "invalid");
    }
    lm::Samples<100> s;
    for (int i = 1; i <= 100; ++i)
        s.add(i);
    require(s.percentile(.95) == 95);
    require(s.percentile(.99) == 99);
    require(s.mean() == 50.5);
    std::vector<uint8_t> avcc{0, 0, 0, 2, 0x65, 0xaa};
    require(lm::annexB(avcc) == std::vector<uint8_t>({0, 0, 0, 1, 0x65, 0xaa}));
    require(lm::rtpTimestamp(10000000) == 90000);
    require(lm::rtpTimestamp(166667) == 1500);
    bool rejected = false;
    try {
        lm::annexB(std::vector<uint8_t>{0, 0, 0, 255});
    } catch (...) {
        rejected = true;
    }
    require(rejected);
    // Two percentiles from one copy agree with asking for each; an emptied ring reads like a new one.
    const auto [p95, p99] = s.percentiles(.95, .99);
    require(p95 == 95 && p99 == 99);
    s.clear();
    require(s.count() == 0 && s.mean() == 0 && s.percentile(.95) == 0);
    s.add(7);
    require(s.count() == 1 && s.mean() == 7);
    // A latency track keeps the rolling ring across intervals, but its max and count are per interval.
    lm::LatencyTrack track;
    track.add(4);
    track.add(9);
    track.add(2);
    require(track.intervalMax == 9 && track.intervalCount == 3 && track.rolling.count() == 3);
    track.endInterval();
    require(track.intervalMax == 0 && track.intervalCount == 0 && track.rolling.count() == 3);
    track.add(1); // A first sample below the old max still becomes the interval's max
    require(track.intervalMax == 1 && track.intervalCount == 1);
    // Source activity: gaps between frames within an interval, the time since the newest one, and nothing
    // invented before there is a frame or while capture is paused.
    using namespace std::chrono_literals;
    lm::SourceActivity source;
    const auto t0 = std::chrono::steady_clock::time_point{} + 1h;
    auto idle = source.take(t0);
    require(idle.frames == 0 && !idle.gapP95 && !idle.gapMax && !idle.sinceLastMs);
    source.frame(t0);
    for (int i = 1; i <= 20; ++i)
        source.frame(t0 + i * 16ms);
    source.frame(t0 + 20 * 16ms + 500ms); // One long gap: the desktop went still, or capture hiccupped
    source.noChange();
    source.noChange();
    auto busy = source.take(t0 + 20 * 16ms + 700ms);
    require(busy.frames == 22 && busy.noChange == 2);
    require(busy.gapMax && *busy.gapMax == 500);
    require(busy.gapP95 && *busy.gapP95 == 16); // 20 of the 21 gaps were one frame apart
    require(busy.sinceLastMs && *busy.sinceLastMs == 200);
    // Nothing new: the interval has no gaps (a stall still in progress shows as time since the newest frame).
    auto still = source.take(t0 + 20 * 16ms + 3500ms);
    require(still.frames == 0 && still.noChange == 0 && !still.gapMax && still.sinceLastMs &&
            *still.sinceLastMs == 3000);
    // The frame that ends a stall reports the whole stall as its gap.
    source.frame(t0 + 20 * 16ms + 4500ms);
    auto resumed = source.take(t0 + 20 * 16ms + 4600ms);
    require(resumed.frames == 1 && resumed.gapMax && *resumed.gapMax == 4000);
    // Paused (no receiver, so capture is not polled): no gap spans the pause and no age is reported.
    source.pause();
    auto paused = source.take(t0 + 60s);
    require(!paused.sinceLastMs);
    source.frame(t0 + 61s);
    auto afterPause = source.take(t0 + 61s + 10ms);
    require(afterPause.frames == 1 && !afterPause.gapMax && afterPause.sinceLastMs &&
            *afterPause.sinceLastMs == 10);
    // GPU engine busy share from cumulative running time in 100 ns units.
    require(lm::busyPercent(0, 5000000, 1.0) == 50.0);           // 0.5 s busy in 1 s
    require(lm::busyPercent(1000, 1000, 1.0) == 0.0);            // Idle is a real zero
    require(lm::busyPercent(0, 20000000, 1.0) == 100.0);         // Booked late: capped, not 200%
    require(!lm::busyPercent(5000000, 1000, 1.0));               // Counter went backwards: unknown, not negative
    require(!lm::busyPercent(0, 1000, 0.0));                     // No time passed: unknown
    std::cout << "Core tests passed\n";
}
