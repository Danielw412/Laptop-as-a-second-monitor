#pragma once
// Temporary pairing codes: one short code is all a viewer types. Codes rotate every two minutes; the previous code
// stays valid for a short overlap so a code read a moment before rotation still works. Rotation only changes what
// new viewers may present; it never touches an established connection.
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace bm {
inline constexpr std::string_view kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
inline constexpr size_t kCodeLength = 6;
struct CodeRegistration {
    std::string hash; // SHA-256 of the code, lowercase hex. The code itself never leaves the host.
    int64_t ttlMs;
};
class PairingCodes {
  public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;
    using Rng = std::function<void(uint8_t *, size_t)>;
    static constexpr std::chrono::seconds rotation{120};
    static constexpr std::chrono::seconds overlap{15};
    static constexpr std::chrono::seconds lifetime = rotation + overlap;
    struct Entry {
        std::string code;
        TimePoint issued;
        TimePoint expires; // issued + lifetime
    };
    PairingCodes(Rng rng, TimePoint now);
    /// The code to show the user right now.
    const std::string &current() const {
        return current_.code;
    }
    /// The previous code while it is still inside its overlap window.
    std::optional<std::string> previous(TimePoint now) const;
    /// When the current code stops being shown (its rotation moment).
    TimePoint currentRotatesAt() const {
        return current_.issued + rotation;
    }
    std::chrono::seconds secondsUntilRotation(TimePoint now) const;
    /// Rotates when due. Returns true if the displayed code changed.
    bool tick(TimePoint now);
    /// Forces a fresh code, for example after disconnecting a viewer.
    void rotate(TimePoint now);
    /// Whether a viewer-typed code is acceptable right now (case-insensitive, separators ignored).
    bool accepts(std::string_view typed, TimePoint now) const;
    /// Every code the signaling server should honour, with the remaining validity of each.
    std::vector<CodeRegistration> registrations(TimePoint now) const;
    static std::string normalize(std::string_view typed);
    static bool wellFormed(std::string_view code);
    static std::string generate(const Rng &rng);
    uint64_t generation() const {
        return generation_;
    }

  private:
    Rng rng_;
    Entry current_;
    std::optional<Entry> previous_;
    uint64_t generation_ = 0;
};
} // namespace bm
