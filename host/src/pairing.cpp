#include "pairing.hpp"
#include "sha256.hpp"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace bm {
std::string PairingCodes::generate(const Rng &rng) {
    uint8_t bytes[kCodeLength];
    rng(bytes, sizeof bytes);
    std::string code;
    // 32 symbols divide 256 evenly, so a byte modulo 32 is uniform: no bias, no rejection loop.
    for (auto b : bytes)
        code += kCodeAlphabet[b % kCodeAlphabet.size()];
    return code;
}
std::string PairingCodes::normalize(std::string_view typed) {
    std::string out;
    for (unsigned char c : typed) {
        if (std::isalnum(c))
            out += char(std::toupper(c));
    }
    return out;
}
bool PairingCodes::wellFormed(std::string_view code) {
    return code.size() == kCodeLength &&
           std::all_of(code.begin(), code.end(), [](char c) { return kCodeAlphabet.find(c) != std::string_view::npos; });
}
PairingCodes::PairingCodes(Rng rng, TimePoint now) : rng_(std::move(rng)) {
    if (!rng_)
        throw std::invalid_argument("PairingCodes requires a random source");
    current_ = {generate(rng_), now, now + lifetime};
}
std::optional<std::string> PairingCodes::previous(TimePoint now) const {
    if (previous_ && previous_->expires > now)
        return previous_->code;
    return std::nullopt;
}
std::chrono::seconds PairingCodes::secondsUntilRotation(TimePoint now) const {
    auto left = std::chrono::duration_cast<std::chrono::seconds>(currentRotatesAt() - now);
    return left.count() < 0 ? std::chrono::seconds{0} : left;
}
bool PairingCodes::tick(TimePoint now) {
    if (now < currentRotatesAt())
        return false;
    rotate(now);
    return true;
}
void PairingCodes::rotate(TimePoint now) {
    previous_ = current_;
    // A forced rotation retires the old code immediately; a scheduled one keeps it through the overlap window.
    if (now < previous_->issued + rotation)
        previous_.reset();
    std::string fresh;
    do {
        fresh = generate(rng_);
    } while (fresh == current_.code || (previous_ && fresh == previous_->code));
    current_ = {fresh, now, now + lifetime};
    ++generation_;
}
bool PairingCodes::accepts(std::string_view typed, TimePoint now) const {
    const auto code = normalize(typed);
    if (!wellFormed(code))
        return false;
    if (code == current_.code && now < current_.expires)
        return true;
    if (auto old = previous(now); old && code == *old)
        return true;
    return false;
}
std::vector<CodeRegistration> PairingCodes::registrations(TimePoint now) const {
    std::vector<CodeRegistration> out;
    auto add = [&](const Entry &e) {
        auto ttl = std::chrono::duration_cast<std::chrono::milliseconds>(e.expires - now).count();
        if (ttl > 0)
            out.push_back({sha256Hex(e.code), ttl});
    };
    if (previous_ && previous_->expires > now)
        add(*previous_);
    add(current_);
    return out;
}
} // namespace bm
