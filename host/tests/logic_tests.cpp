// Portable tests for pairing, LaptopMon detection, lifecycle state transitions, settings and hashing.
#include "app_state.hpp"
#include "display_identity.hpp"
#include "pairing.hpp"
#include "settings.hpp"
#include "sha256.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
using namespace lm;
using namespace std::chrono_literals;
namespace {
int failures = 0;
void require(bool condition, const char *what) {
    if (!condition) {
        std::cerr << "FAILED: " << what << '\n';
        ++failures;
    }
}
#define CHECK(x) require(bool(x), #x)
// A deterministic byte source: counts upward, so successive codes differ predictably.
struct Counter {
    uint8_t next = 0;
    void operator()(uint8_t *out, size_t n) {
        for (size_t i = 0; i < n; ++i)
            out[i] = next++;
    }
};
PairingCodes::Rng counter() {
    auto c = std::make_shared<Counter>();
    return [c](uint8_t *out, size_t n) { (*c)(out, n); };
}
PairingCodes::Rng constant(uint8_t v) {
    return [v](uint8_t *out, size_t n) { std::fill(out, out + n, v); };
}
void testSha256() {
    CHECK(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    // Multi-block input crosses the 64-byte boundary.
    CHECK(sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    CHECK(roomIdFor("abc") == "ba7816bf8f01cfea414140de5dae2223");
    CHECK(validSecret(std::string(64, 'a')));
    CHECK(!validSecret(std::string(63, 'a')));
    CHECK(!validSecret(std::string(64, 'A')));
    CHECK(!validSecret(std::string(64, 'g')));
}
void testPairingGeneration() {
    auto t0 = PairingCodes::TimePoint{};
    PairingCodes codes(counter(), t0);
    const auto &c = codes.current();
    CHECK(c.size() == kCodeLength);
    CHECK(PairingCodes::wellFormed(c));
    CHECK(c == "ABCDEF"); // bytes 0..5 map to the first six alphabet symbols
    for (char ch : c)
        CHECK(kCodeAlphabet.find(ch) != std::string_view::npos);
    // Every byte value maps into the alphabet; no I, O, 0 or 1 ever appears.
    for (int b = 0; b < 256; ++b) {
        auto code = PairingCodes::generate(constant(uint8_t(b)));
        CHECK(PairingCodes::wellFormed(code));
        CHECK(code.find_first_of("IO01") == std::string::npos);
    }
    CHECK(PairingCodes::normalize("k7m 4q2") == "K7M4Q2");
    CHECK(PairingCodes::normalize("K7M-4Q2\n") == "K7M4Q2");
    CHECK(!PairingCodes::wellFormed("K7M4Q"));
    CHECK(!PairingCodes::wellFormed("K7M4Q21"));
    CHECK(!PairingCodes::wellFormed("I0O1AB"));
    CHECK(!PairingCodes::wellFormed("k7m4q2"));
    bool threw = false;
    try {
        PairingCodes broken(nullptr, t0);
    } catch (const std::invalid_argument &) {
        threw = true;
    }
    CHECK(threw);
}
void testPairingRotation() {
    auto t0 = PairingCodes::TimePoint{} + 1000s;
    PairingCodes codes(counter(), t0);
    const std::string first = codes.current();
    CHECK(codes.accepts(first, t0));
    CHECK(codes.accepts(" " + first.substr(0, 3) + "-" + first.substr(3), t0)); // typed with separators
    CHECK(!codes.accepts("ZZZZZZ", t0));
    CHECK(!codes.previous(t0));
    CHECK(codes.secondsUntilRotation(t0) == 120s);
    CHECK(codes.secondsUntilRotation(t0 + 50s) == 70s);
    // No rotation before the two-minute mark.
    CHECK(!codes.tick(t0 + 119s));
    CHECK(codes.current() == first);
    CHECK(codes.generation() == 0);
    // Rotation at 120 s: new current, old one still valid for 15 s.
    CHECK(codes.tick(t0 + 120s));
    const std::string second = codes.current();
    CHECK(second != first);
    CHECK(codes.generation() == 1);
    CHECK(codes.previous(t0 + 120s) == first);
    CHECK(codes.accepts(first, t0 + 120s));
    CHECK(codes.accepts(second, t0 + 120s));
    CHECK(codes.accepts(first, t0 + 134s));
    CHECK(!codes.accepts(first, t0 + 135s)); // overlap over
    CHECK(!codes.previous(t0 + 135s));
    CHECK(codes.accepts(second, t0 + 135s));
    // Registrations carry remaining TTLs for both codes during overlap and only the current one afterwards.
    auto regs = codes.registrations(t0 + 125s);
    CHECK(regs.size() == 2);
    CHECK(regs[0].hash == sha256Hex(first));
    CHECK(regs[0].ttlMs == 10000);
    CHECK(regs[1].hash == sha256Hex(second));
    CHECK(regs[1].ttlMs == 130000);
    regs = codes.registrations(t0 + 140s);
    CHECK(regs.size() == 1);
    CHECK(regs[0].hash == sha256Hex(second));
    // Hashes never equal the code itself and are 64 hex chars.
    CHECK(regs[0].hash.size() == 64);
    CHECK(regs[0].hash.find(second) == std::string::npos);
    // A very late tick (app was asleep) still rotates exactly once per tick.
    CHECK(codes.tick(t0 + 1000s));
    CHECK(codes.generation() == 2);
    CHECK(!codes.tick(t0 + 1000s));
    // The current code is never accepted after its own lifetime if rotation never ran (defensive).
    PairingCodes stale(counter(), t0);
    CHECK(!stale.accepts(stale.current(), t0 + 136s));
}
void testForcedRotation() {
    auto t0 = PairingCodes::TimePoint{} + 10s;
    PairingCodes codes(counter(), t0);
    const std::string first = codes.current();
    // Disconnecting a viewer forces a new code and retires the old one immediately.
    codes.rotate(t0 + 30s);
    CHECK(codes.current() != first);
    CHECK(!codes.accepts(first, t0 + 30s));
    CHECK(!codes.previous(t0 + 30s));
    CHECK(codes.registrations(t0 + 30s).size() == 1);
    CHECK(codes.secondsUntilRotation(t0 + 30s) == 120s);
    // Rotation never hands out the code it just retired, even when the byte source repeats itself.
    PairingCodes unique(counter(), t0);
    for (int i = 0; i < 40; ++i) {
        const std::string before = unique.current();
        unique.rotate(t0 + std::chrono::seconds(i + 1));
        CHECK(unique.current() != before);
    }
    // With a real-looking source, many consecutive codes are all distinct (32^6 possibilities).
    auto state = std::make_shared<uint64_t>(0x9E3779B97F4A7C15ull);
    PairingCodes::Rng lcg = [state](uint8_t *out, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            *state = *state * 6364136223846793005ull + 1442695040888963407ull;
            out[i] = uint8_t(*state >> 56);
        }
    };
    PairingCodes many(lcg, t0);
    std::set<std::string> seen{many.current()};
    for (int i = 0; i < 200; ++i) {
        many.rotate(t0 + std::chrono::seconds(i + 1));
        CHECK(seen.insert(many.current()).second);
    }
}
DisplayTarget laptopMon(std::string gdi = "\\\\.\\DISPLAY7") {
    DisplayTarget t;
    t.gdiName = gdi;
    t.devicePath = "\\\\?\\DISPLAY#LMV0001#1&1eee597c&2&UID256#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    t.friendlyName = "LaptopMon";
    t.edidManufacturer = 0x31B6;
    t.edidProduct = 1;
    t.active = true;
    return t;
}
DisplayTarget laptopPanel() {
    DisplayTarget t;
    t.gdiName = "\\\\.\\DISPLAY1";
    t.devicePath = "\\\\?\\DISPLAY#LGD0709#4&266e0cb&0&UID8388688#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}";
    t.friendlyName = "";
    t.edidManufacturer = 0x1E6D;
    t.edidProduct = 0x0709;
    t.active = true;
    t.primary = true;
    return t;
}
void testDetection() {
    CHECK(isLaptopMon(laptopMon()));
    CHECK(!isLaptopMon(laptopPanel()));
    // Any one of the EDID-derived markers is enough; the DISPLAYn number is irrelevant.
    auto pathOnly = laptopMon("\\\\.\\DISPLAY2");
    pathOnly.friendlyName = "";
    pathOnly.edidManufacturer = 0;
    pathOnly.edidProduct = 0;
    CHECK(isLaptopMon(pathOnly));
    auto edidOnly = laptopMon("\\\\.\\DISPLAY9");
    edidOnly.devicePath = "\\\\?\\DISPLAY#XXX#";
    edidOnly.friendlyName = "";
    CHECK(isLaptopMon(edidOnly));
    edidOnly.edidManufacturer = 0xB631; // byte-swapped as some APIs report it
    CHECK(isLaptopMon(edidOnly));
    edidOnly.edidProduct = 2;
    CHECK(!isLaptopMon(edidOnly));
    auto nameOnly = laptopMon();
    nameOnly.devicePath = "";
    nameOnly.edidManufacturer = 0;
    nameOnly.edidProduct = 0;
    CHECK(isLaptopMon(nameOnly));
    // Selection ignores physical displays entirely and reports what is wrong with LaptopMon.
    auto sel = selectLaptopMon({laptopPanel(), laptopMon()});
    CHECK(sel.problem == SelectionProblem::None);
    CHECK(sel.display && sel.display->gdiName == "\\\\.\\DISPLAY7");
    sel = selectLaptopMon({laptopPanel()});
    CHECK(sel.problem == SelectionProblem::NotFound);
    CHECK(!sel.display);
    sel = selectLaptopMon({});
    CHECK(sel.problem == SelectionProblem::NotFound);
    // Primary protection: LaptopMon set as primary is refused, and the laptop panel is never substituted.
    auto primaryMon = laptopMon();
    primaryMon.primary = true;
    auto secondaryPanel = laptopPanel();
    secondaryPanel.primary = false;
    sel = selectLaptopMon({secondaryPanel, primaryMon});
    CHECK(sel.problem == SelectionProblem::Primary);
    CHECK(sel.display && isLaptopMon(*sel.display));
    // Duplicate mode and inactive targets are reported so the app can switch to Extend.
    auto cloned = laptopMon("\\\\.\\DISPLAY1");
    cloned.cloned = true;
    CHECK(selectLaptopMon({laptopPanel(), cloned}).problem == SelectionProblem::Cloned);
    auto inactive = laptopMon("");
    inactive.active = false;
    CHECK(selectLaptopMon({laptopPanel(), inactive}).problem == SelectionProblem::Inactive);
    auto detached = laptopMon();
    detached.available = false;
    CHECK(selectLaptopMon({laptopPanel(), detached}).problem == SelectionProblem::NotFound);
    // A usable instance wins over a stale unavailable one (reinstall leaves old devnodes behind).
    CHECK(selectLaptopMon({detached, inactive, laptopMon("\\\\.\\DISPLAY12")}).display->gdiName == "\\\\.\\DISPLAY12");
    CHECK(std::string(describe(SelectionProblem::Primary)).find("primary") != std::string::npos);
}
bool has(const std::vector<Effect> &effects, EffectType type) {
    for (auto &e : effects)
        if (e.type == type)
            return true;
    return false;
}
size_t count(const std::vector<Effect> &effects, EffectType type) {
    size_t n = 0;
    for (auto &e : effects)
        n += e.type == type;
    return n;
}
std::vector<Effect> step(AppModel &m, EventType type, std::string detail = {}) {
    return reduce(m, {type, std::move(detail)});
}
/// Drives a fresh model through the happy path to Ready.
AppModel readyModel() {
    AppModel m;
    m.setupReady = true;
    auto fx = step(m, EventType::Init);
    CHECK(has(fx, EffectType::FindDisplay));
    fx = step(m, EventType::DisplayNotFound, "LaptopMon is not connected");
    CHECK(has(fx, EffectType::StartDisplay));
    CHECK(m.phase == Phase::StartingDisplay);
    fx = step(m, EventType::DisplayStarted);
    CHECK(has(fx, EffectType::FindDisplay));
    CHECK(m.phase == Phase::FindingDisplay);
    fx = step(m, EventType::DisplayFound);
    CHECK(has(fx, EffectType::StartEngine));
    CHECK(m.phase == Phase::StartingEncoder);
    step(m, EventType::EngineStarted);
    step(m, EventType::EncoderReady);
    CHECK(m.stream == StreamStatus::Streaming);
    step(m, EventType::SignalingConnecting);
    CHECK(m.phase == Phase::ConnectingSignaling);
    step(m, EventType::SignalingConnected);
    CHECK(m.phase == Phase::Ready);
    return m;
}
void testLifecycleHappyPath() {
    auto m = readyModel();
    step(m, EventType::ViewerJoined);
    CHECK(m.viewer == ViewerStatus::Connecting);
    step(m, EventType::WebRtcConnected);
    CHECK(m.phase == Phase::Connected);
    step(m, EventType::ViewerLeft);
    CHECK(m.phase == Phase::Ready);
    // Signaling drops after being up: reconnecting, not "connecting" from scratch.
    step(m, EventType::SignalingDisconnected);
    CHECK(m.phase == Phase::Reconnecting);
    step(m, EventType::SignalingConnected);
    CHECK(m.phase == Phase::Ready);
}
void testSetupRequired() {
    AppModel m;
    m.setupReady = false;
    auto fx = step(m, EventType::Init);
    CHECK(has(fx, EffectType::FindDisplay));
    fx = step(m, EventType::DisplayNotFound);
    CHECK(!has(fx, EffectType::StartDisplay)); // Never tries to elevate without setup
    CHECK(m.phase == Phase::SetupRequired);
    // An externally created LaptopMon (e.g. the sample app) is still usable without setup.
    fx = step(m, EventType::DisplayFoundExternal);
    CHECK(has(fx, EffectType::StartEngine));
    CHECK(m.display == DisplayStatus::External);
    // Stop Monitor cannot remove a display we do not own; it stops streaming and says so.
    step(m, EventType::EngineStarted);
    fx = step(m, EventType::UserStopMonitor);
    CHECK(has(fx, EffectType::StopEngine));
    CHECK(!has(fx, EffectType::StopDisplay));
    fx = step(m, EventType::EngineStopped);
    CHECK(!has(fx, EffectType::StopDisplay));
    CHECK(m.display == DisplayStatus::External);
    CHECK(m.stream == StreamStatus::Stopped);
    CHECK(m.phase == Phase::DisplayOnly);
    // Setup completing while idle triggers a fresh look at the desktop.
    AppModel n;
    n.wantDisplay = true;
    fx = step(n, EventType::SetupChanged, "ready");
    CHECK(has(fx, EffectType::FindDisplay));
}
void testStopStreamingAndRepeatedStops() {
    auto m = readyModel();
    auto fx = step(m, EventType::UserStopStreaming);
    CHECK(count(fx, EffectType::StopEngine) == 1);
    CHECK(m.phase == Phase::Stopping);
    // Pressing stop again while stopping issues nothing new.
    fx = step(m, EventType::UserStopStreaming);
    CHECK(fx.empty());
    fx = step(m, EventType::EngineStopped);
    CHECK(fx.empty());
    CHECK(m.phase == Phase::DisplayOnly);
    CHECK(m.display == DisplayStatus::Active);
    // Stop once more when already stopped: no effects, state unchanged.
    fx = step(m, EventType::UserStopStreaming);
    CHECK(fx.empty());
    CHECK(m.phase == Phase::DisplayOnly);
    // Start streaming again reuses the running display.
    fx = step(m, EventType::UserStartStreaming);
    CHECK(has(fx, EffectType::StartEngine));
    CHECK(!has(fx, EffectType::StartDisplay));
    // Start pressed while the old stream is still stopping: no second engine yet, but it starts once the old one
    // has reported its exit.
    auto q = readyModel();
    step(q, EventType::UserStopStreaming);
    fx = step(q, EventType::UserStartStreaming);
    CHECK(!has(fx, EffectType::StartEngine));
    fx = step(q, EventType::EngineStopped);
    CHECK(count(fx, EffectType::StartEngine) == 1);
    CHECK(q.stream == StreamStatus::Starting);
    // A fatal engine exit is an error, not a restart loop.
    auto f = readyModel();
    fx = step(f, EventType::EngineStopped, "Media Foundation startup failed");
    CHECK(!has(fx, EffectType::StartEngine));
    CHECK(f.phase == Phase::Error);
    CHECK(f.error == "Media Foundation startup failed");
    fx = step(f, EventType::UserStartStreaming);
    CHECK(has(fx, EffectType::StartEngine));
    CHECK(f.stream == StreamStatus::Starting);
    // A fatal exit during Exit or Restart does not block the chain.
    auto x = readyModel();
    step(x, EventType::UserExit);
    fx = step(x, EventType::EngineStopped, "device lost");
    CHECK(has(fx, EffectType::StopDisplay));
    fx = step(x, EventType::DisplayStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
}
void testStopMonitor() {
    auto m = readyModel();
    auto fx = step(m, EventType::UserStopMonitor);
    CHECK(has(fx, EffectType::StopEngine));
    CHECK(!has(fx, EffectType::StopDisplay)); // Display waits for the engine to finish
    fx = step(m, EventType::UserStopMonitor);
    CHECK(fx.empty());
    fx = step(m, EventType::EngineStopped);
    CHECK(count(fx, EffectType::StopDisplay) == 1);
    CHECK(m.phase == Phase::Stopping);
    fx = step(m, EventType::UserStopMonitor);
    CHECK(fx.empty());
    fx = step(m, EventType::DisplayStopped);
    CHECK(fx.empty());
    CHECK(m.phase == Phase::Idle);
    CHECK(m.display == DisplayStatus::Stopped);
    fx = step(m, EventType::UserStopMonitor);
    CHECK(fx.empty());
    // Start Monitor from idle goes through the desktop query, not straight to elevation.
    fx = step(m, EventType::UserStartMonitor);
    CHECK(has(fx, EffectType::FindDisplay));
    // Stop Monitor while only the display runs (streaming stopped) removes the display directly.
    auto d = readyModel();
    step(d, EventType::UserStopStreaming);
    step(d, EventType::EngineStopped);
    fx = step(d, EventType::UserStopMonitor);
    CHECK(has(fx, EffectType::StopDisplay));
    CHECK(!has(fx, EffectType::StopEngine));
}
/// Start -> stop -> start, several times over, is the path that used to leave the app wedged. The reducer must
/// come back to exactly the same state every lap and never overlap a teardown with the next start.
void testRepeatedMonitorCycles() {
    auto m = readyModel();
    for (int lap = 0; lap < 4; ++lap) {
        auto fx = step(m, EventType::UserStopMonitor);
        CHECK(count(fx, EffectType::StopEngine) == 1);
        CHECK(!has(fx, EffectType::StopDisplay)); // Streaming always stops first
        CHECK(m.phase == Phase::Stopping);
        fx = step(m, EventType::EngineStopped);
        CHECK(count(fx, EffectType::StopDisplay) == 1);
        CHECK(!has(fx, EffectType::StartEngine));
        fx = step(m, EventType::DisplayStopped);
        CHECK(fx.empty());
        CHECK(m.phase == Phase::Idle);
        CHECK(m.display == DisplayStatus::Stopped);
        CHECK(m.stream == StreamStatus::Stopped);
        CHECK(m.viewer == ViewerStatus::None);
        CHECK(m.signaling == SignalingStatus::Disconnected);
        CHECK(!m.stopDisplayPending);
        CHECK(m.error.empty());
        // ...and back up again.
        fx = step(m, EventType::UserStartMonitor);
        CHECK(count(fx, EffectType::FindDisplay) == 1);
        fx = step(m, EventType::DisplayNotFound, "LaptopMon is not connected");
        CHECK(count(fx, EffectType::StartDisplay) == 1);
        fx = step(m, EventType::DisplayStarted);
        CHECK(has(fx, EffectType::FindDisplay));
        fx = step(m, EventType::DisplayFound);
        CHECK(count(fx, EffectType::StartEngine) == 1);
        step(m, EventType::EngineStarted);
        step(m, EventType::EncoderReady);
        step(m, EventType::SignalingConnected);
        CHECK(m.phase == Phase::Ready);
        CHECK(m.displayRetries == 0);
    }
    // Exiting while a receiver is connected still unwinds in order, one effect each.
    step(m, EventType::ViewerJoined);
    step(m, EventType::WebRtcConnected);
    auto fx = step(m, EventType::UserExit);
    CHECK(count(fx, EffectType::StopEngine) == 1);
    fx = step(m, EventType::EngineStopped);
    CHECK(count(fx, EffectType::StopDisplay) == 1);
    fx = step(m, EventType::DisplayStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
    // The helper timing out instead of confirming reaches the same place: the app never stays in Stopping.
    auto t = readyModel();
    step(t, EventType::UserStopMonitor);
    step(t, EventType::EngineStopped);
    fx = step(t, EventType::DisplayStopped); // Posted by the worker's stop deadline, not by the helper
    CHECK(t.phase == Phase::Idle);
    CHECK(t.display == DisplayStatus::Stopped);
    // A helper that dies on its own during a requested stop is a clean stop, not an unexpected exit.
    auto h = readyModel();
    step(h, EventType::UserStopMonitor);
    step(h, EventType::EngineStopped);
    CHECK(h.display == DisplayStatus::Stopping);
    fx = step(h, EventType::DisplayHelperExited);
    CHECK(h.phase == Phase::Idle);
    CHECK(h.display == DisplayStatus::Stopped);
    CHECK(!has(fx, EffectType::StartDisplay)); // Not a crash to recover from: the user asked for this
}
void testRestart() {
    auto m = readyModel();
    step(m, EventType::ViewerJoined);
    step(m, EventType::WebRtcConnected);
    auto fx = step(m, EventType::UserRestart);
    CHECK(has(fx, EffectType::StopEngine));
    fx = step(m, EventType::UserRestart); // double click
    CHECK(fx.empty());
    fx = step(m, EventType::EngineStopped);
    CHECK(has(fx, EffectType::StopDisplay));
    fx = step(m, EventType::DisplayStopped);
    CHECK(has(fx, EffectType::FindDisplay));
    CHECK(!m.restartPending);
    fx = step(m, EventType::DisplayNotFound);
    CHECK(has(fx, EffectType::StartDisplay));
    fx = step(m, EventType::DisplayStarted);
    fx = step(m, EventType::DisplayFound);
    CHECK(has(fx, EffectType::StartEngine));
    // Restart when nothing runs simply starts.
    AppModel idle;
    idle.setupReady = true;
    fx = step(idle, EventType::UserRestart);
    CHECK(has(fx, EffectType::FindDisplay));
}
void testExit() {
    // Exit from connected: engine, then display, then quit. Quit is emitted exactly once.
    auto m = readyModel();
    step(m, EventType::ViewerJoined);
    step(m, EventType::WebRtcConnected);
    auto fx = step(m, EventType::UserExit);
    CHECK(has(fx, EffectType::StopEngine));
    CHECK(!has(fx, EffectType::Quit));
    CHECK(m.phase == Phase::Exiting);
    fx = step(m, EventType::UserExit);
    CHECK(fx.empty());
    // User actions after exit are ignored.
    fx = step(m, EventType::UserStartMonitor);
    CHECK(fx.empty());
    fx = step(m, EventType::EngineStopped);
    CHECK(has(fx, EffectType::StopDisplay));
    CHECK(!has(fx, EffectType::Quit));
    fx = step(m, EventType::DisplayStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
    // Late events after quit produce no further quit.
    fx = step(m, EventType::DisplayFound);
    CHECK(!has(fx, EffectType::Quit));
    CHECK(!has(fx, EffectType::StartEngine));
    // Exit from idle quits immediately.
    AppModel idle;
    idle.setupReady = true;
    fx = step(idle, EventType::UserExit);
    CHECK(count(fx, EffectType::Quit) == 1);
    // Exit while only the display runs.
    auto d = readyModel();
    step(d, EventType::UserStopStreaming);
    step(d, EventType::EngineStopped);
    fx = step(d, EventType::UserExit);
    CHECK(has(fx, EffectType::StopDisplay));
    fx = step(d, EventType::DisplayStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
    // Exit with an external display: never tries to stop it, quits after the engine.
    AppModel ext;
    step(ext, EventType::Init);
    step(ext, EventType::DisplayFoundExternal);
    step(ext, EventType::EngineStarted);
    fx = step(ext, EventType::UserExit);
    CHECK(has(fx, EffectType::StopEngine));
    fx = step(ext, EventType::EngineStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
    CHECK(!has(fx, EffectType::StopDisplay));
    // Exit while the display is still starting and then fails: still quits.
    AppModel starting;
    starting.setupReady = true;
    step(starting, EventType::Init);
    step(starting, EventType::DisplayNotFound);
    CHECK(starting.phase == Phase::StartingDisplay);
    fx = step(starting, EventType::UserExit);
    CHECK(!has(fx, EffectType::Quit)); // waits for the helper outcome
    fx = step(starting, EventType::DisplayStartFailed, "denied");
    CHECK(count(fx, EffectType::Quit) == 1);
    // ...and when the helper does come up during exit, it is stopped before quitting.
    AppModel late;
    late.setupReady = true;
    step(late, EventType::Init);
    step(late, EventType::DisplayNotFound);
    step(late, EventType::UserExit);
    fx = step(late, EventType::DisplayStarted);
    CHECK(has(fx, EffectType::StopDisplay));
    CHECK(!has(fx, EffectType::Quit));
    fx = step(late, EventType::DisplayStopped);
    CHECK(count(fx, EffectType::Quit) == 1);
    // Stop Monitor pressed while the display is starting: no engine start, display taken down once it reports.
    AppModel changed;
    changed.setupReady = true;
    step(changed, EventType::Init);
    step(changed, EventType::DisplayNotFound);
    fx = step(changed, EventType::UserStopMonitor);
    CHECK(fx.empty());
    fx = step(changed, EventType::DisplayStarted);
    CHECK(has(fx, EffectType::StopDisplay));
    CHECK(!has(fx, EffectType::StartEngine));
    step(changed, EventType::DisplayStopped);
    CHECK(changed.phase == Phase::Idle);
}
void testFailuresAndRecovery() {
    // Helper failure surfaces as an error without retry storms.
    AppModel m;
    m.setupReady = true;
    step(m, EventType::Init);
    step(m, EventType::DisplayNotFound);
    auto fx = step(m, EventType::DisplayStartFailed, "UAC was cancelled");
    CHECK(m.phase == Phase::Error);
    CHECK(m.error == "UAC was cancelled");
    CHECK(fx.empty());
    // Start Monitor retries from the error state.
    fx = step(m, EventType::UserStartMonitor);
    CHECK(has(fx, EffectType::FindDisplay));
    CHECK(m.error.empty());
    // Display disappearing while streaming: reconnecting; engine untouched.
    auto r = readyModel();
    fx = step(r, EventType::DisplayLost);
    CHECK(fx.empty());
    CHECK(r.phase == Phase::Reconnecting);
    CHECK(r.display == DisplayStatus::Missing);
    fx = step(r, EventType::DisplayFound);
    CHECK(!has(fx, EffectType::StartEngine)); // already running
    CHECK(r.phase == Phase::Ready);
    // Helper dying while streaming: restart the display (bounded), stop nothing else while it restarts.
    fx = step(r, EventType::DisplayHelperExited);
    CHECK(has(fx, EffectType::StartDisplay));
    CHECK(!has(fx, EffectType::StopEngine));
    CHECK(r.displayRetries == 1);
    step(r, EventType::DisplayStarted);
    step(r, EventType::DisplayFound);
    CHECK(r.displayRetries == 0);
    // Too many helper deaths in a row become an error and stop the engine.
    AppModel dying = readyModel();
    for (int i = 0; i < 3; ++i) {
        fx = step(dying, EventType::DisplayHelperExited);
        CHECK(has(fx, EffectType::StartDisplay));
        step(dying, EventType::DisplayStarted);
        // Display never re-found; helper dies again.
    }
    fx = step(dying, EventType::DisplayHelperExited);
    CHECK(!has(fx, EffectType::StartDisplay));
    CHECK(has(fx, EffectType::StopEngine));
    CHECK(dying.display == DisplayStatus::Error);
    // Signaling rejection is an error state that Restart clears.
    auto s = readyModel();
    step(s, EventType::SignalingRejected, "authentication");
    CHECK(s.phase == Phase::Error);
    CHECK(s.signaling == SignalingStatus::Rejected);
    fx = step(s, EventType::UserRestart);
    CHECK(has(fx, EffectType::StopEngine));
    // Engine errors are informational while the engine keeps retrying.
    auto e = readyModel();
    fx = step(e, EventType::EngineError, "Encoder stalled; recreating GPU pipeline");
    CHECK(fx.empty());
    CHECK(e.stream == StreamStatus::Streaming);
    CHECK(e.error == "Encoder stalled; recreating GPU pipeline");
    // Disconnect viewer only acts when a viewer exists.
    auto v = readyModel();
    fx = step(v, EventType::UserDisconnectViewer);
    CHECK(fx.empty());
    step(v, EventType::ViewerJoined);
    fx = step(v, EventType::UserDisconnectViewer);
    CHECK(has(fx, EffectType::DisconnectViewer));
    // Auto-start disabled: init only looks, then rests at Idle without elevating.
    AppModel manual;
    manual.setupReady = true;
    manual.autoStartDisplay = false;
    step(manual, EventType::Init);
    fx = step(manual, EventType::DisplayNotFound);
    CHECK(!has(fx, EffectType::StartDisplay));
    CHECK(manual.phase == Phase::Idle);
    // LaptopMon exists but is primary: no engine start, actionable problem shown.
    AppModel p;
    p.setupReady = true;
    step(p, EventType::Init);
    step(p, EventType::DisplayNotFound);
    step(p, EventType::DisplayStarted);
    fx = step(p, EventType::DisplayNotFound, describe(SelectionProblem::Primary));
    CHECK(!has(fx, EffectType::StartEngine));
    CHECK(p.phase == Phase::Error);
    CHECK(p.error.find("primary") != std::string::npos);
}
void testSettings() {
    Settings defaults;
    CHECK(defaults.signalingUrl == std::string(kDefaultSignalingUrl));
    CHECK(defaults.fps == 60);
    CHECK(defaults.backend == CaptureBackend::Auto);
    // The virtual display ships scaled like the 13.3" panel its EDID describes.
    CHECK(defaults.displayScale == DisplayScale::Percent150);
    CHECK(scalePercent(defaults.displayScale) == 150);
    CHECK(scalePercent(DisplayScale::Recommended) == 0);
    for (auto v : {DisplayScale::Recommended, DisplayScale::Percent100, DisplayScale::Percent125,
                   DisplayScale::Percent150, DisplayScale::Percent175})
        CHECK(scaleFromName(scaleName(v)) == v);
    CHECK(scaleFromName("133") == DisplayScale::Recommended); // Anything unknown means "leave it to Windows"
    // Round trip preserves every field.
    Settings s;
    s.startAtSignIn = true;
    s.autoStartDisplay = false;
    s.minimizeToTray = false;
    s.diagnosticsLog = false;
    s.backend = CaptureBackend::Dxgi;
    s.fps = 30;
    s.quality = QualityPreset::Quality;
    s.displayScale = DisplayScale::Percent125;
    s.signalingUrl = "https://example.invalid/signaling";
    CHECK(settingsFromJson(toJson(s)) == s);
    // Tolerant parsing: garbage and unknown keys fall back to defaults instead of failing.
    auto parsed = settingsFromJson(nlohmann::json::parse(R"({"fps":"fast","captureBackend":"vulkan","quality":7,"extra":true,"signalingUrl":"ftp://nope"})"));
    CHECK(parsed.fps == 60);
    CHECK(parsed.backend == CaptureBackend::Auto);
    CHECK(parsed.quality == QualityPreset::Balanced);
    CHECK(parsed.displayScale == DisplayScale::Percent150);
    CHECK(parsed.signalingUrl == std::string(kDefaultSignalingUrl));
    // A settings file written before scaling existed keeps the new default rather than silently meaning 100%.
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"fps":30})")).displayScale == DisplayScale::Percent150);
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"displayScale":"recommended"})")).displayScale ==
          DisplayScale::Recommended);
    CHECK(settingsFromJson(nlohmann::json::array()) == Settings{});
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"fps":45})")).fps == 60);
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"fps":20})")).fps == 30);
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"signalingUrl":"http://127.0.0.1:8787/"})")).signalingUrl ==
          "http://127.0.0.1:8787");
    CHECK(settingsFromJson(nlohmann::json::parse(R"({"signalingUrl":"http://evil.example"})")).signalingUrl ==
          std::string(kDefaultSignalingUrl));
    CHECK(bitratePlan(QualityPreset::Balanced).initial == 8000000);
    CHECK(bitratePlan(QualityPreset::Quality).maximum > bitratePlan(QualityPreset::Efficient).maximum);
    // Persistence through the store, including a missing file, a corrupt file and overwrite.
    auto dir = std::filesystem::temp_directory_path() / ("lm-settings-test-" + std::to_string(std::rand()));
    SettingsStore store(dir / "nested" / "settings.json");
    CHECK(store.load() == Settings{});
    store.save(s);
    CHECK(store.load() == s);
    s.fps = 60;
    store.save(s);
    CHECK(store.load() == s);
    {
        std::ofstream corrupt(store.path(), std::ios::trunc);
        corrupt << "{not json";
    }
    CHECK(store.load() == Settings{});
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}
} // namespace
int main() {
    testSha256();
    testPairingGeneration();
    testPairingRotation();
    testForcedRotation();
    testDetection();
    testLifecycleHappyPath();
    testSetupRequired();
    testStopStreamingAndRepeatedStops();
    testStopMonitor();
    testRepeatedMonitorCycles();
    testRestart();
    testExit();
    testFailuresAndRecovery();
    testSettings();
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "Logic tests passed\n";
    return 0;
}
