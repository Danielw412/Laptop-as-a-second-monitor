#include "app_state.hpp"
namespace bm {
namespace {
constexpr int kMaxDisplayRetries = 3;
void emit(std::vector<Effect> &effects, EffectType type, std::string detail = {}) {
    effects.push_back({type, std::move(detail)});
}
bool displayUsable(const AppModel &m) {
    return m.display == DisplayStatus::Active || m.display == DisplayStatus::External;
}
bool streamIdle(const AppModel &m) {
    return m.stream == StreamStatus::Stopped || m.stream == StreamStatus::Error;
}
/// Recomputes the headline phase from component status and pending intents.
void settle(AppModel &m) {
    if (m.exiting) {
        m.phase = Phase::Exiting;
        return;
    }
    if (m.display == DisplayStatus::Stopping || m.stream == StreamStatus::Stopping) {
        m.phase = Phase::Stopping;
        return;
    }
    if (m.display == DisplayStatus::Starting) {
        m.phase = Phase::StartingDisplay;
        return;
    }
    if (m.display == DisplayStatus::Missing) {
        m.phase = m.stream == StreamStatus::Stopped ? Phase::FindingDisplay : Phase::Reconnecting;
        return;
    }
    if (m.display == DisplayStatus::Error) {
        m.phase = Phase::Error;
        return;
    }
    if (m.display == DisplayStatus::Stopped) {
        m.phase = m.setupReady ? Phase::Idle : Phase::SetupRequired;
        return;
    }
    // Display is Active or External from here on.
    switch (m.stream) {
    case StreamStatus::Stopped:
        m.phase = Phase::DisplayOnly;
        return;
    case StreamStatus::Error:
        m.phase = Phase::Error;
        return;
    case StreamStatus::Starting:
        m.phase = Phase::StartingEncoder;
        return;
    default:
        break;
    }
    if (m.signaling == SignalingStatus::Rejected) {
        m.phase = Phase::Error;
        return;
    }
    if (m.viewer == ViewerStatus::Connected) {
        m.phase = Phase::Connected;
        return;
    }
    if (m.signaling != SignalingStatus::Connected) {
        // Lost signaling after having been up is a reconnect; never having had it is the initial connect.
        m.phase = m.detail == "reconnect" ? Phase::Reconnecting : Phase::ConnectingSignaling;
        return;
    }
    m.phase = m.viewer == ViewerStatus::Connecting ? Phase::Reconnecting : Phase::Ready;
}
/// Drives the stop chain forward: engine, then display, then whatever the pending intent asks for.
void continueStopChain(AppModel &m, std::vector<Effect> &effects) {
    if (m.stream == StreamStatus::Stopping)
        return; // Wait for EngineStopped
    if (!streamIdle(m) && (!m.wantStream || m.exiting || m.restartPending || m.stopDisplayPending)) {
        m.stream = StreamStatus::Stopping;
        emit(effects, EffectType::StopEngine);
        return;
    }
    if (m.display == DisplayStatus::Stopping || m.display == DisplayStatus::Starting)
        return; // Wait for DisplayStopped, or for the helper to report before telling it to stop
    if ((m.display == DisplayStatus::Active || m.display == DisplayStatus::Missing) &&
        (m.stopDisplayPending || m.exiting || m.restartPending)) {
        m.display = DisplayStatus::Stopping;
        emit(effects, EffectType::StopDisplay);
        return;
    }
    if (m.stopDisplayPending && m.display == DisplayStatus::External) {
        // Not ours to remove; leave it and report.
        m.stopDisplayPending = false;
        m.detail = "The virtual display is managed outside Browser Monitor and was left running.";
    }
    m.stopDisplayPending = false;
    if (m.exiting) {
        emit(effects, EffectType::Quit);
        return;
    }
    if (m.restartPending) {
        m.restartPending = false;
        m.wantDisplay = true;
        m.wantStream = true;
        m.displayRetries = 0;
        m.error.clear();
        m.detail.clear();
        emit(effects, EffectType::FindDisplay);
    }
}
void startDisplayOrReport(AppModel &m, std::vector<Effect> &effects) {
    if (!m.setupReady) {
        m.display = DisplayStatus::Stopped;
        m.error.clear();
        return; // settle() reports SetupRequired
    }
    m.display = DisplayStatus::Starting;
    emit(effects, EffectType::StartDisplay);
}
} // namespace
std::vector<Effect> reduce(AppModel &m, const Event &e) {
    std::vector<Effect> effects;
    switch (e.type) {
    case EventType::Init:
        m.wantDisplay = m.autoStartDisplay;
        m.wantStream = true;
        emit(effects, EffectType::FindDisplay);
        break;
    case EventType::SetupChanged:
        m.setupReady = e.detail == "ready";
        if (m.setupReady && m.display == DisplayStatus::Stopped && m.wantDisplay && !m.exiting)
            emit(effects, EffectType::FindDisplay);
        break;
    case EventType::UserStartMonitor:
        if (m.exiting)
            break;
        m.wantDisplay = true;
        m.wantStream = true;
        m.displayRetries = 0;
        m.error.clear();
        m.detail.clear();
        if (m.display == DisplayStatus::Stopped || m.display == DisplayStatus::Error ||
            m.display == DisplayStatus::Missing)
            emit(effects, EffectType::FindDisplay);
        else if (displayUsable(m) && streamIdle(m)) {
            m.stream = StreamStatus::Starting;
            emit(effects, EffectType::StartEngine);
        }
        break;
    case EventType::UserStopMonitor:
        if (m.exiting)
            break;
        m.wantDisplay = false;
        m.wantStream = false;
        m.stopDisplayPending = m.display != DisplayStatus::Stopped;
        continueStopChain(m, effects);
        break;
    case EventType::UserStartStreaming:
        if (m.exiting)
            break;
        m.wantStream = true;
        m.error.clear();
        if (displayUsable(m) && streamIdle(m)) {
            m.stream = StreamStatus::Starting;
            emit(effects, EffectType::StartEngine);
        } else if (m.display == DisplayStatus::Stopped) {
            m.wantDisplay = true;
            emit(effects, EffectType::FindDisplay);
        }
        break;
    case EventType::UserStopStreaming:
        if (m.exiting)
            break;
        m.wantStream = false;
        continueStopChain(m, effects);
        break;
    case EventType::UserRestart:
        if (m.exiting)
            break;
        m.restartPending = true;
        m.error.clear();
        if (streamIdle(m) && (m.display == DisplayStatus::Stopped || m.display == DisplayStatus::External)) {
            m.restartPending = false;
            m.wantDisplay = true;
            m.wantStream = true;
            m.displayRetries = 0;
            emit(effects, EffectType::FindDisplay);
        } else
            continueStopChain(m, effects);
        break;
    case EventType::UserExit:
        if (m.exiting)
            break;
        m.exiting = true;
        m.restartPending = false;
        continueStopChain(m, effects);
        break;
    case EventType::UserDisconnectViewer:
        if (m.stream == StreamStatus::Streaming && m.viewer != ViewerStatus::None)
            emit(effects, EffectType::DisconnectViewer);
        break;
    case EventType::DisplayStarted:
        if (m.display == DisplayStatus::Starting) {
            if (m.exiting || m.restartPending || !m.wantDisplay) {
                // The user changed their mind while the helper was starting: take it straight back down.
                m.display = DisplayStatus::Active;
                m.stopDisplayPending = m.stopDisplayPending || !m.wantDisplay;
                continueStopChain(m, effects);
                break;
            }
            m.display = DisplayStatus::Missing; // Created; now wait for it to show up on the desktop
            emit(effects, EffectType::FindDisplay);
        }
        break;
    case EventType::DisplayStartFailed:
        m.display = DisplayStatus::Error;
        m.error = e.detail.empty() ? "The virtual display could not be started." : e.detail;
        if (m.exiting || m.restartPending) {
            m.display = DisplayStatus::Stopped;
            continueStopChain(m, effects);
        }
        break;
    case EventType::DisplayHelperExited:
        if (m.display == DisplayStatus::Stopping) {
            m.display = DisplayStatus::Stopped;
            continueStopChain(m, effects);
        } else if (m.display == DisplayStatus::Active || m.display == DisplayStatus::Starting ||
                   m.display == DisplayStatus::Missing) {
            m.display = DisplayStatus::Stopped;
            if (!m.exiting && m.wantDisplay && m.setupReady && ++m.displayRetries <= kMaxDisplayRetries) {
                m.detail = "The virtual display stopped unexpectedly; restarting it.";
                m.display = DisplayStatus::Starting;
                emit(effects, EffectType::StartDisplay);
            } else if (!m.exiting) {
                m.display = DisplayStatus::Error;
                m.error = "The virtual display stopped unexpectedly.";
            }
            if (m.stream != StreamStatus::Stopped && m.display != DisplayStatus::Starting) {
                m.stream = StreamStatus::Stopping;
                emit(effects, EffectType::StopEngine);
            }
        }
        break;
    case EventType::DisplayStopped:
        if (m.display != DisplayStatus::External)
            m.display = DisplayStatus::Stopped;
        continueStopChain(m, effects);
        break;
    case EventType::DisplayFound:
    case EventType::DisplayFoundExternal:
        if (m.exiting)
            break;
        // Ownership: DisplayFound means our helper is running it; External means someone else created it.
        if (e.type == EventType::DisplayFoundExternal && m.display != DisplayStatus::Active)
            m.display = DisplayStatus::External;
        else
            m.display = DisplayStatus::Active;
        m.displayRetries = 0;
        if (m.restartPending) {
            continueStopChain(m, effects);
            break;
        }
        if (m.wantStream && streamIdle(m)) {
            m.stream = StreamStatus::Starting;
            emit(effects, EffectType::StartEngine);
        }
        break;
    case EventType::DisplayNotFound:
        if (m.exiting)
            break;
        if (m.display == DisplayStatus::Missing && m.stream != StreamStatus::Stopped) {
            m.detail = e.detail; // Still streaming-wise waiting; engine retries on its own
            break;
        }
        if (m.display == DisplayStatus::Active || m.display == DisplayStatus::External) {
            m.display = DisplayStatus::Missing;
            m.detail = e.detail;
            break;
        }
        if (m.display == DisplayStatus::Missing) {
            // We created it but it never became part of the desktop.
            m.display = DisplayStatus::Error;
            m.error = e.detail.empty() ? "BrowserMon did not appear on the desktop." : e.detail;
            break;
        }
        if (m.wantDisplay && !m.restartPending)
            startDisplayOrReport(m, effects);
        else if (m.restartPending) {
            m.restartPending = false;
            m.wantDisplay = true;
            m.wantStream = true;
            startDisplayOrReport(m, effects);
        }
        break;
    case EventType::DisplayLost:
        if (displayUsable(m)) {
            m.display = DisplayStatus::Missing;
            m.detail = e.detail.empty() ? "BrowserMon disappeared; waiting for it to return." : e.detail;
        }
        break;
    case EventType::EngineStarted:
        if (m.stream == StreamStatus::Starting)
            m.stream = StreamStatus::Streaming;
        m.signaling = SignalingStatus::Disconnected;
        m.viewer = ViewerStatus::None;
        break;
    case EventType::EncoderReady:
        if (m.stream == StreamStatus::Starting)
            m.stream = StreamStatus::Streaming;
        m.error.clear();
        break;
    case EventType::SignalingConnecting:
        if (m.signaling != SignalingStatus::Rejected)
            m.signaling = SignalingStatus::Connecting;
        break;
    case EventType::SignalingConnected:
        m.signaling = SignalingStatus::Connected;
        m.detail.clear();
        m.error.clear();
        break;
    case EventType::SignalingDisconnected:
        if (m.signaling == SignalingStatus::Connected)
            m.detail = "reconnect";
        if (m.signaling != SignalingStatus::Rejected)
            m.signaling = SignalingStatus::Disconnected;
        m.viewer = ViewerStatus::None;
        break;
    case EventType::SignalingRejected:
        m.signaling = SignalingStatus::Rejected;
        m.viewer = ViewerStatus::None;
        m.error = "Signaling rejected this host: " + e.detail;
        break;
    case EventType::ViewerJoined:
        m.viewer = ViewerStatus::Connecting;
        break;
    case EventType::ViewerLeft:
        m.viewer = ViewerStatus::None;
        break;
    case EventType::WebRtcConnected:
        m.viewer = ViewerStatus::Connected;
        m.error.clear();
        break;
    case EventType::WebRtcDisconnected:
        if (m.viewer == ViewerStatus::Connected)
            m.viewer = ViewerStatus::Connecting;
        break;
    case EventType::EngineError:
        // The engine retries by itself; surface the reason without changing intent.
        m.error = e.detail;
        break;
    case EventType::EngineStopped:
        m.stream = StreamStatus::Stopped;
        m.viewer = ViewerStatus::None;
        m.signaling = SignalingStatus::Disconnected;
        if (!e.detail.empty() && !m.exiting && !m.restartPending) {
            // A fatal engine exit is shown, never retried automatically.
            m.stream = StreamStatus::Error;
            m.error = e.detail;
            m.wantStream = false;
        }
        continueStopChain(m, effects);
        // Start Streaming pressed while the previous stream was still winding down: honour it now.
        if (m.wantStream && !m.exiting && !m.restartPending && !m.stopDisplayPending && displayUsable(m) &&
            m.stream == StreamStatus::Stopped) {
            m.stream = StreamStatus::Starting;
            emit(effects, EffectType::StartEngine);
        }
        break;
    }
    settle(m);
    return effects;
}
const char *phaseText(Phase p) {
    switch (p) {
    case Phase::SetupRequired:
        return "Setup required";
    case Phase::Idle:
        return "Stopped";
    case Phase::StartingDisplay:
        return "Starting virtual display";
    case Phase::FindingDisplay:
        return "Finding BrowserMon";
    case Phase::DisplayOnly:
        return "Streaming stopped";
    case Phase::StartingEncoder:
        return "Starting encoder";
    case Phase::ConnectingSignaling:
        return "Connecting signaling";
    case Phase::Ready:
        return "Ready for receiver";
    case Phase::Connected:
        return "Connected";
    case Phase::Reconnecting:
        return "Reconnecting";
    case Phase::Stopping:
        return "Stopping";
    case Phase::Exiting:
        return "Exiting";
    case Phase::Error:
        return "Error";
    }
    return "";
}
const char *displayText(DisplayStatus s) {
    switch (s) {
    case DisplayStatus::Stopped:
        return "Off";
    case DisplayStatus::Starting:
        return "Starting";
    case DisplayStatus::Active:
        return "Active";
    case DisplayStatus::External:
        return "Active (external)";
    case DisplayStatus::Missing:
        return "Not on desktop";
    case DisplayStatus::Stopping:
        return "Stopping";
    case DisplayStatus::Error:
        return "Failed";
    }
    return "";
}
const char *streamText(StreamStatus s) {
    switch (s) {
    case StreamStatus::Stopped:
        return "Stopped";
    case StreamStatus::Starting:
        return "Starting";
    case StreamStatus::Streaming:
        return "Streaming";
    case StreamStatus::Stopping:
        return "Stopping";
    case StreamStatus::Error:
        return "Failed";
    }
    return "";
}
const char *viewerText(ViewerStatus s) {
    switch (s) {
    case ViewerStatus::None:
        return "Waiting for receiver";
    case ViewerStatus::Connecting:
        return "Connecting";
    case ViewerStatus::Connected:
        return "Connected";
    }
    return "";
}
const char *signalingText(SignalingStatus s) {
    switch (s) {
    case SignalingStatus::Disconnected:
        return "Disconnected";
    case SignalingStatus::Connecting:
        return "Connecting";
    case SignalingStatus::Connected:
        return "Connected";
    case SignalingStatus::Rejected:
        return "Rejected";
    }
    return "";
}
const char *eventName(EventType t) {
    static const char *names[] = {
        "Init", "SetupChanged", "UserStartMonitor", "UserStopMonitor", "UserStartStreaming", "UserStopStreaming",
        "UserRestart", "UserExit", "UserDisconnectViewer", "DisplayStarted", "DisplayStartFailed",
        "DisplayHelperExited", "DisplayStopped", "DisplayFound", "DisplayFoundExternal", "DisplayNotFound",
        "DisplayLost", "EngineStarted", "EncoderReady", "SignalingConnecting", "SignalingConnected",
        "SignalingDisconnected", "SignalingRejected", "ViewerJoined", "ViewerLeft", "WebRtcConnected",
        "WebRtcDisconnected", "EngineError", "EngineStopped"};
    return names[size_t(t)];
}
const char *effectName(EffectType t) {
    static const char *names[] = {"FindDisplay", "StartDisplay", "StopDisplay", "StartEngine",
                                  "StopEngine",  "DisconnectViewer", "Quit",     "Notify"};
    return names[size_t(t)];
}
} // namespace bm
