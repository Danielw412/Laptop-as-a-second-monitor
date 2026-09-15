#pragma once
// The controller's state machine as a pure reducer: (model, event) -> effects. The Win32 side executes effects and
// feeds results back as events. Keeping this free of platform calls makes every lifecycle path unit-testable.
#include <cstdint>
#include <string>
#include <vector>
namespace bm {
enum class Phase {
    SetupRequired,
    Idle,          // Virtual display off, nothing streaming
    StartingDisplay,
    FindingDisplay,
    DisplayOnly,   // Display up, streaming deliberately stopped
    StartingEncoder,
    ConnectingSignaling,
    Ready,         // Waiting for a receiver
    Connected,
    Reconnecting,
    Stopping,
    Exiting,
    Error,
};
enum class DisplayStatus { Stopped, Starting, Active, External, Missing, Stopping, Error };
enum class StreamStatus { Stopped, Starting, Streaming, Stopping, Error };
enum class ViewerStatus { None, Connecting, Connected };
enum class SignalingStatus { Disconnected, Connecting, Connected, Rejected };
struct AppModel {
    Phase phase = Phase::Idle;
    DisplayStatus display = DisplayStatus::Stopped;
    StreamStatus stream = StreamStatus::Stopped;
    ViewerStatus viewer = ViewerStatus::None;
    SignalingStatus signaling = SignalingStatus::Disconnected;
    bool setupReady = false;     // Helper task registered and driver staged
    bool autoStartDisplay = true;
    bool wantDisplay = true;     // User intent, survives transient failures
    bool wantStream = true;
    bool exiting = false;
    bool restartPending = false;
    bool stopDisplayPending = false;
    int displayRetries = 0;
    std::string error;           // Last user-facing problem
    std::string detail;          // Secondary status line
};
enum class EventType {
    Init,
    SetupChanged,          // detail: "ready" or "missing"
    UserStartMonitor,
    UserStopMonitor,
    UserStartStreaming,
    UserStopStreaming,
    UserRestart,
    UserExit,
    UserDisconnectViewer,
    DisplayStarted,        // Helper created the device
    DisplayStartFailed,    // detail: reason
    DisplayHelperExited,   // The helper went away on its own
    DisplayStopped,
    DisplayFound,          // Our display is on the desktop
    DisplayFoundExternal,  // BrowserMon exists but we do not own it
    DisplayNotFound,       // detail: SelectionProblem description
    DisplayLost,
    EngineStarted,
    EncoderReady,
    SignalingConnecting,
    SignalingConnected,
    SignalingDisconnected,
    SignalingRejected,     // detail: server error code (fatal for this session)
    ViewerJoined,
    ViewerLeft,
    WebRtcConnected,
    WebRtcDisconnected,
    EngineError,           // detail: message; the engine keeps retrying
    EngineStopped,
};
struct Event {
    EventType type;
    std::string detail;
};
enum class EffectType {
    FindDisplay,       // Query the desktop for BrowserMon now
    StartDisplay,      // Run the elevated helper
    StopDisplay,       // Ask the helper to remove the device
    StartEngine,
    StopEngine,
    DisconnectViewer,
    Quit,
    Notify,            // detail: user-facing message
};
struct Effect {
    EffectType type;
    std::string detail;
    bool operator==(const Effect &) const = default;
};
std::vector<Effect> reduce(AppModel &, const Event &);
const char *phaseText(Phase);
const char *displayText(DisplayStatus);
const char *streamText(StreamStatus);
const char *viewerText(ViewerStatus);
const char *signalingText(SignalingStatus);
const char *eventName(EventType);
const char *effectName(EffectType);
} // namespace bm
