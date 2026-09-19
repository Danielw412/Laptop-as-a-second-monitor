#include "core.hpp"
#include <iostream>
#include <limits>
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("test failed");
}
int main() {
    lm::BitrateController c;
    require(c.update(.1, 20, 0, 8000000) == 6000000);
    for (int i = 0; i < 100; ++i)
        c.update(.1, 300, 50, 8000000);
    require(c.bitrate() == 1500000);
    for (int i = 0; i < 100; ++i)
        c.update(0, 20, 0, 8000000);
    require(c.bitrate() > 1500000);
    const auto old = c.bitrate();
    require(c.update(std::numeric_limits<double>::quiet_NaN(), 0, 0, 8000000) == old);
    // Quiet source: a clean connection alone must not trigger costly, useless encoder rebuilds.
    lm::BitrateController quiet;
    for (int i = 0; i < 300; ++i)
        quiet.update(0, 5, 3, 300000);
    require(quiet.bitrate() == 8000000);
    // Seven busy intervals, then idle: growth requires eight consecutive busy, healthy intervals.
    for (int i = 0; i < 7; ++i)
        quiet.update(0, 5, 3, 8000000);
    quiet.update(0, 5, 3, 0);
    for (int i = 0; i < 7; ++i)
        quiet.update(0, 5, 3, 8000000);
    require(quiet.bitrate() == 8000000);
    require(quiet.update(0, 5, 3, 6000000) == 8250000); // 75% threshold includes equality
    // Unknown or invalid demand cannot earn growth. An idle link still reacts to congestion.
    for (auto demand : {std::optional<double>{}, std::optional<double>{-1},
                        std::optional<double>{std::numeric_limits<double>::infinity()},
                        std::optional<double>{std::numeric_limits<double>::quiet_NaN()}}) {
        for (int i = 0; i < 16; ++i)
            quiet.update(0, 5, 3, demand);
        require(quiet.bitrate() == 8250000);
    }
    require(quiet.update(.1, 20, 0, std::nullopt) == 6187500);
    lm::BitrateController rttCut, jitterCut;
    require(rttCut.update(0, 300, 0, 0) == 6000000);
    require(jitterCut.update(0, 20, 50, 0) == 6000000);
    lm::BitrateController capped(8000000, 1500000, 8250000);
    for (int i = 0; i < 32; ++i)
        capped.update(0, 5, 3, 16000000);
    require(capped.bitrate() == 8250000);
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
