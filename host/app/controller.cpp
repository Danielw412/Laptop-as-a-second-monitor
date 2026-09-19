#include "controller.hpp"
#include "app.hpp"
#include "diagnostics.hpp"
#include "display_query.hpp"
#include "logging.hpp"
#include "session_archive.hpp"
namespace lm::app {
namespace {
constexpr int kFindPollLimit = 60; // 60 polls at 250 ms = 15 s for LaptopMon to appear after device creation
} // namespace
AppController::AppController(HWND window, SettingsStore store, Settings settings, std::string hostSecret)
    : window_(window), store_(std::move(store)), settings_(std::move(settings)), hostSecret_(std::move(hostSecret)),
      display_([this](Event e) { post(std::move(e)); }) {
    Log::instance().setSink([this](LogLevel, const std::string &line) {
        std::lock_guard lock(logMutex_);
        log_.push_back(line);
        while (log_.size() > 200)
            log_.pop_front();
    });
}
AppController::~AppController() {
    // Teardown order matters. Everything that can still call back into this object has to be stopped and joined
    // here, while the members those callbacks touch (the event queue, the log buffer) are all still alive.
    Log::instance().setSink({});
    if (engine_) {
        engine_->requestStop();
        engine_->join();
    }
    for (auto &e : retired_)
        e->join();
    display_.shutdown();
    if (setupWaiter_.joinable())
        setupWaiter_.join();
}
void AppController::init() {
    refreshSetupStatus();
    model_.autoStartDisplay = settings_.autoStartDisplay;
    dispatch({EventType::Init});
}
void AppController::refreshSetupStatus() {
    setup_ = querySetupStatus();
    logInfo("Setup status: " + setup_.summary());
    dispatch({EventType::SetupChanged, setup_.ready() ? "ready" : "missing"});
}
void AppController::post(Event e) {
    {
        std::lock_guard lock(queueMutex_);
        queue_.push_back(std::move(e));
    }
    PostMessageW(window_, WM_APP_EVENT, 0, 0);
}
void AppController::drain() {
    reapEngines();
    for (;;) {
        Event e;
        {
            std::lock_guard lock(queueMutex_);
            if (queue_.empty())
                return;
            e = std::move(queue_.front());
            queue_.pop_front();
        }
        dispatch(e);
    }
}
void AppController::reapEngines() {
    // Engines that have finished their thread are joined here on the UI thread; a finished current engine is
    // released so a new one can start cleanly.
    std::erase_if(retired_, [](auto &engine) {
        if (engine->running())
            return false;
        engine->join();
        return true;
    });
    if (engine_ && !engine_->running()) {
        engine_->join();
        engine_.reset();
    }
}
void AppController::dispatch(const Event &e) {
    const bool wasConnected = model_.viewer == ViewerStatus::Connected;
    if (e.type == EventType::EngineStopped)
        reapEngines();
    auto effects = reduce(model_, e);
    if (e.type != EventType::Init)
        logDebug(std::string("Event ") + eventName(e.type) + (e.detail.empty() ? "" : ": " + e.detail) + " -> " +
                 phaseText(model_.phase));
    for (auto &effect : effects)
        execute(effect);
    const bool connected = model_.viewer == ViewerStatus::Connected;
    if (connected != wasConnected && viewerCallback_)
        viewerCallback_(connected);
    if (stateCallback_)
        stateCallback_();
}
void AppController::execute(const Effect &effect) {
    switch (effect.type) {
    case EffectType::FindDisplay:
        findDisplay();
        break;
    case EffectType::StartDisplay:
        logInfo("Starting the virtual display through the elevated helper");
        // A new device is a new monitor to Windows, including after the helper restarted on its own.
        scaleApplied_ = false;
        display_.start();
        break;
    case EffectType::StopDisplay:
        logInfo("Stopping the virtual display");
        scaleApplied_ = false;
        display_.stop();
        break;
    case EffectType::StartEngine:
        startEngine();
        break;
    case EffectType::StopEngine:
        stopEngine();
        break;
    case EffectType::DisconnectViewer:
        if (engine_)
            engine_->disconnectViewer();
        break;
    case EffectType::Quit:
        quit_ = true;
        display_.abandon();
        break;
    case EffectType::Notify:
        notification_ = effect.detail;
        break;
    }
}
void AppController::findDisplay() {
    auto match = matchLaptopMon(false);
    if (match.display) {
        findArmed_ = false;
        findPolls_ = 0;
        KillTimer(window_, TIMER_FIND);
        logInfo("LaptopMon found on " + match.display->name + " (" + match.display->gpu + ")");
        if (match.target)
            applyScale(*match.target, false);
        dispatch({display_.owned() ? EventType::DisplayFound : EventType::DisplayFoundExternal});
        return;
    }
    // Not there yet. While our helper is starting it (or right after a topology change), keep polling briefly
    // instead of failing immediately.
    const bool waiting = model_.display == DisplayStatus::Missing || model_.display == DisplayStatus::Starting ||
                         match.problem == SelectionProblem::Inactive || match.problem == SelectionProblem::Cloned;
    if (waiting && findPolls_ < kFindPollLimit) {
        if (!findArmed_) {
            findArmed_ = true;
            SetTimer(window_, TIMER_FIND, 250, nullptr);
        }
        ++findPolls_;
        model_.detail = match.detail;
        return;
    }
    findArmed_ = false;
    findPolls_ = 0;
    KillTimer(window_, TIMER_FIND);
    dispatch({EventType::DisplayNotFound, match.detail});
}
void AppController::pollDisplay() {
    if (findArmed_)
        findDisplay();
}
void AppController::displayChanged() {
    if (model_.exiting)
        return;
    if (model_.display == DisplayStatus::Missing || model_.display == DisplayStatus::Starting) {
        findPolls_ = 0;
        findDisplay();
        return;
    }
    if (model_.display == DisplayStatus::Active || model_.display == DisplayStatus::External) {
        auto match = matchLaptopMon(false);
        if (!match.display) {
            dispatch({EventType::DisplayLost, match.detail});
            findPolls_ = 0;
            findDisplay();
        }
    }
}
void AppController::startEngine() {
    if (engine_) {
        // A previous engine is still winding down; keep the newest one only.
        engine_->requestStop();
        retired_.push_back(std::move(engine_));
    }
    EngineConfig config;
    config.mode = PipelineMode::Stream;
    config.backend = settings_.backend;
    config.fps = settings_.fps;
    config.bitrate = bitratePlan(settings_.quality);
    config.signalingUrl = settings_.signalingUrl;
    config.hostSecret = hostSecret_;
    config.matcher = [] { return matchLaptopMon(false); };
    const uint64_t generation = ++engineGeneration_;
    try {
        engine_ = std::make_unique<StreamingEngine>(
            config, [this, generation](const EngineEvent &e) { onEngineEvent(generation, e); });
        engine_->start();
    } catch (const std::exception &e) {
        engine_.reset();
        post({EventType::EngineStopped, e.what()});
    }
}
void AppController::stopEngine() {
    if (!engine_) {
        post({EventType::EngineStopped});
        return;
    }
    engine_->requestStop();
}
void AppController::onEngineEvent(uint64_t generation, const EngineEvent &e) {
    // Engine thread: translate and hand over to the UI thread. Events from a superseded engine must not disturb
    // the state of its replacement; only its final Stopped is still useful, to reap the thread.
    if (generation != engineGeneration_) {
        if (e.type == EngineEventType::Stopped)
            PostMessageW(window_, WM_APP_EVENT, 0, 0);
        return;
    }
    static const EventType map[] = {
        EventType::EngineStarted,        EventType::EncoderReady,        EventType::SignalingConnecting,
        EventType::SignalingConnected,   EventType::SignalingDisconnected, EventType::SignalingRejected,
        EventType::ViewerJoined,         EventType::ViewerLeft,          EventType::WebRtcConnected,
        EventType::WebRtcDisconnected,   EventType::EngineError /* CodeRotated: no state change */,
        EventType::DisplayLost,          EventType::DisplayFound,        EventType::EngineError,
        EventType::EngineStopped};
    if (e.type == EngineEventType::CodeRotated) {
        PostMessageW(window_, WM_APP_EVENT, 0, 0); // repaint only
        return;
    }
    if (e.type == EngineEventType::DisplayFound) {
        // The engine regained the display; the controller re-evaluates ownership itself.
        post({display_.owned() ? EventType::DisplayFound : EventType::DisplayFoundExternal});
        return;
    }
    post({map[size_t(e.type)], e.detail});
}
void AppController::startMonitor() {
    dispatch({EventType::UserStartMonitor});
}
void AppController::stopMonitor() {
    dispatch({EventType::UserStopMonitor});
}
void AppController::startStreaming() {
    dispatch({EventType::UserStartStreaming});
}
void AppController::stopStreaming() {
    dispatch({EventType::UserStopStreaming});
}
void AppController::restart() {
    logInfo("Restart requested");
    dispatch({EventType::UserRestart});
}
void AppController::exitApp() {
    logInfo("Exit requested");
    dispatch({EventType::UserExit});
}
void AppController::disconnectViewer() {
    dispatch({EventType::UserDisconnectViewer});
}
void AppController::launchElevatedAndWait(HWND owner, const wchar_t *argument) {
    if (setupProcess_)
        return;
    std::wstring error;
    HANDLE process = launchElevated(owner, argument, error);
    if (!process) {
        notification_ = narrow(error);
        logWarning("Elevated launch failed: " + notification_);
        if (stateCallback_)
            stateCallback_();
        return;
    }
    setupProcess_ = process;
    if (setupWaiter_.joinable())
        setupWaiter_.join();
    setupWaiter_ = std::thread([this, process] {
        WaitForSingleObject(process, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(process, &code);
        PostMessageW(window_, WM_APP_SETUP_DONE, WPARAM(code), 0);
    });
}
void AppController::runSetup(HWND owner) {
    uninstalling_ = false;
    launchElevatedAndWait(owner, L"--setup");
}
void AppController::runUninstall(HWND owner) {
    // Stop everything we own first so the elevated process can remove the helper and the task.
    uninstalling_ = true;
    if (engine_)
        engine_->requestStop();
    display_.abandon();
    // Uninstall deletes %LOCALAPPDATA%\LaptopMonitor, this run's archive included, and a log file held open there
    // would stop it. The rolling log in Temp keeps recording until the process exits.
    closeArchive(termination::kUninstall);
    launchElevatedAndWait(owner, L"--uninstall");
    if (!setupProcess_ && settings_.diagnosticsLog) {
        // Declined or failed to launch: nothing was deleted, so keep archiving this run.
        for (auto &[level, line] : startDiagnostics(settings_))
            Log::instance().write(level, line);
    }
}
void AppController::setupProcessFinished(int exitCode) {
    if (setupProcess_) {
        CloseHandle(setupProcess_);
        setupProcess_ = nullptr;
    }
    if (uninstalling_) {
        logInfo("Uninstall finished with code " + std::to_string(exitCode));
        quit_ = true;
        return;
    }
    logInfo("Setup finished with code " + std::to_string(exitCode));
    notification_ = exitCode == 0 ? "Setup complete." : "Setup did not complete.";
    refreshSetupStatus();
    if (exitCode == 0 && setup_.ready() && model_.display == DisplayStatus::Stopped)
        dispatch({EventType::UserStartMonitor});
}
void AppController::updateSettings(const Settings &updated) {
    auto clean = sanitized(updated);
    const bool restartNeeded = clean.backend != settings_.backend || clean.fps != settings_.fps ||
                               clean.quality != settings_.quality || clean.signalingUrl != settings_.signalingUrl;
    const bool rescale = clean.displayScale != settings_.displayScale;
    if (clean.startAtSignIn != settings_.startAtSignIn)
        setStartAtSignIn(clean.startAtSignIn);
    if (clean.diagnosticsLog != settings_.diagnosticsLog) {
        if (clean.diagnosticsLog) {
            // Turned on mid-run: the archive for this run is created now (or reopened, if it was on before).
            auto notes = startDiagnostics(clean);
            logInfo("Diagnostics logging turned on: " + Log::instance().path().string() + " and " +
                    Log::instance().recordPath().string() + " | run " + runIdentity().runId);
            for (auto &[level, line] : notes)
                Log::instance().write(level, line);
        } else {
            logInfo("Diagnostics logging turned off");
            stopDiagnostics(termination::kDiagnosticsOff);
        }
    } else if (!(clean == settings_))
        noteSettings(clean);
    settings_ = clean;
    model_.autoStartDisplay = clean.autoStartDisplay;
    try {
        store_.save(settings_);
    } catch (const std::exception &e) {
        logWarning(std::string("Saving settings failed: ") + e.what());
    }
    if (rescale) {
        // Scaling is a property of the monitor, not of the stream: apply it in place, nothing restarts.
        auto match = matchLaptopMon(false);
        if (match.target)
            applyScale(*match.target, true);
        else
            notification_ = "Display scaling will be applied when the virtual display is running.";
    }
    if (restartNeeded && model_.stream != StreamStatus::Stopped) {
        // Apply pipeline settings by restarting only the stream, not the display.
        logInfo("Settings changed; restarting the stream");
        dispatch({EventType::UserStopStreaming});
        model_.wantStream = true;
    }
}
void AppController::applyScale(const DisplayTarget &target, bool force) {
    // Windows keeps a per-monitor scale of its own, so this runs once per display session (and again whenever the
    // user changes the setting). Leaving it alone afterwards means a manual change in Settings sticks.
    if (scaleApplied_ && !force)
        return;
    scaleApplied_ = true;
    const unsigned wanted = scalePercent(settings_.displayScale);
    auto info = displayScaleOf(target);
    if (!wanted) {
        // Recommended: whatever the EDID's 13.3" physical size makes Windows choose.
        if (info)
            logInfo("Virtual display scaling left at " + std::to_string(info->current) + "% (Windows recommends " +
                    std::to_string(info->recommended) + "%)");
        return;
    }
    if (info && info->current == wanted) {
        logInfo("Virtual display already scaled to " + std::to_string(wanted) + "%");
        return;
    }
    auto result = applyDisplayScale(target, wanted);
    if (result.applied) {
        logInfo("Virtual display scaled to " + std::to_string(result.percent) + "%");
        return;
    }
    // Windows would not do it for us, so say exactly what to do by hand instead of failing quietly.
    logWarning("Could not scale the virtual display: " + result.problem);
    notification_ = result.problem + " Set it by hand in Settings > System > Display: pick " +
                    std::string(kLaptopMonFriendlyName) + ", then Scale " + std::to_string(wanted) + "%.";
}
MetricsSnapshot AppController::metrics() const {
    return engine_ ? engine_->snapshot() : MetricsSnapshot{};
}
std::string AppController::pairingCode() const {
    if (!engine_ || model_.stream != StreamStatus::Streaming || model_.signaling != SignalingStatus::Connected)
        return {};
    return engine_->snapshot().pairing.code;
}
int AppController::pairingSecondsLeft() const {
    if (!engine_)
        return 0;
    auto at = engine_->snapshot().pairing.rotatesAt;
    auto left = std::chrono::duration_cast<std::chrono::seconds>(at - std::chrono::steady_clock::now()).count();
    return int(std::max<long long>(0, left));
}
std::vector<std::string> AppController::recentLog() const {
    std::lock_guard lock(logMutex_);
    return {log_.begin(), log_.end()};
}
} // namespace lm::app
