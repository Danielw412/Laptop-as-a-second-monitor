#pragma once
// Application controller: owns the model, executes the reducer's effects on the UI thread, and hosts the
// streaming engine and display helper. The window reads state from here and calls the user actions.
#include "app_state.hpp"
#include "display_control.hpp"
#include "metrics.hpp"
#include "pipeline.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <windows.h>
namespace lm::app {
class AppController {
  public:
    AppController(HWND window, SettingsStore store, Settings settings, std::string hostSecret);
    ~AppController();
    void init();
    /// Thread-safe: queues an event and wakes the UI thread.
    void post(Event);
    /// UI thread: applies queued events.
    void drain();
    /// UI thread: something about the desktop changed; look again if we are waiting for LaptopMon.
    void displayChanged();
    void pollDisplay();
    // User actions (UI thread).
    void startMonitor();
    void stopMonitor();
    void startStreaming();
    void stopStreaming();
    void restart();
    void exitApp();
    void disconnectViewer();
    void runSetup(HWND owner);
    void runUninstall(HWND owner);
    void setupProcessFinished(int exitCode);
    void refreshSetupStatus();
    // State for the view.
    const AppModel &model() const {
        return model_;
    }
    const Settings &settings() const {
        return settings_;
    }
    void updateSettings(const Settings &);
    const SetupStatus &setupStatus() const {
        return setup_;
    }
    MetricsSnapshot metrics() const;
    std::string pairingCode() const;
    int pairingSecondsLeft() const;
    bool quitRequested() const {
        return quit_;
    }
    bool setupRunning() const {
        return setupProcess_ != nullptr;
    }
    bool uninstallRequested() const {
        return uninstalling_;
    }
    std::string lastNotification() const {
        return notification_;
    }
    /// Recent log lines for the Details page.
    std::vector<std::string> recentLog() const;
    void viewerConnectedChanged(std::function<void(bool)> callback) {
        viewerCallback_ = std::move(callback);
    }
    void stateChanged(std::function<void()> callback) {
        stateCallback_ = std::move(callback);
    }

  private:
    HWND window_;
    SettingsStore store_;
    Settings settings_;
    std::string hostSecret_;
    AppModel model_;
    SetupStatus setup_;
    DisplayController display_;
    std::unique_ptr<StreamingEngine> engine_;
    // Identifies which engine an event came from: written on the UI thread, read on every engine thread.
    std::atomic<uint64_t> engineGeneration_{0};
    std::vector<std::unique_ptr<StreamingEngine>> retired_; // Stopped engines awaiting join
    std::mutex queueMutex_;
    std::deque<Event> queue_;
    bool quit_ = false, uninstalling_ = false, findArmed_ = false;
    int findPolls_ = 0;
    HANDLE setupProcess_ = nullptr;
    std::thread setupWaiter_;
    std::string notification_;
    std::function<void(bool)> viewerCallback_;
    std::function<void()> stateCallback_;
    mutable std::mutex logMutex_;
    std::deque<std::string> log_;
    bool scaleApplied_ = false; // Per display session: we set the scale once, then leave it to the user
    void dispatch(const Event &);
    void execute(const Effect &);
    void findDisplay();
    /// Applies the configured Windows scaling to the virtual display only.
    void applyScale(const DisplayTarget &, bool force);
    void startEngine();
    void stopEngine();
    void onEngineEvent(uint64_t generation, const EngineEvent &);
    void reapEngines();
    void launchElevatedAndWait(HWND owner, const wchar_t *argument);
};
} // namespace lm::app
