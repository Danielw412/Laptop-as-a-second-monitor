#include "core.hpp"
#include <iostream>
#include <limits>
void require(bool condition) {
    if (!condition)
        throw std::runtime_error("test failed");
}
int main() {
    lm::BitrateController c;
    require(c.update(.1, 20, 0) == 6000000);
    for (int i = 0; i < 100; ++i)
        c.update(.1, 300, 50);
    require(c.bitrate() == 1500000);
    for (int i = 0; i < 100; ++i)
        c.update(0, 20, 0);
    require(c.bitrate() > 1500000);
    const auto old = c.bitrate();
    require(c.update(std::numeric_limits<double>::quiet_NaN(), 0, 0) == old);
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
    std::cout << "Core tests passed\n";
}
