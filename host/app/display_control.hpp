#pragma once
// Drives the elevated display helper from the unelevated app: runs its scheduled task, talks to it over a
// nonce-named pipe, and reports what happened as controller events. All waiting happens on a worker thread.
#include "app_state.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
namespace bm::app {
class DisplayController {
  public:
    explicit DisplayController(std::function<void(Event)> post);
    ~DisplayController();
    /// Starts the helper. Posts DisplayStarted or DisplayStartFailed, later DisplayHelperExited if it dies.
    void start();
    /// Asks the helper to remove the device. Posts DisplayStopped when it has gone (or after a timeout).
    void stop();
    /// True while we hold a connection to a helper we started.
    bool owned() const {
        return owned_;
    }
    /// Exit path: tell the helper to stop without waiting for confirmation.
    void abandon();

  private:
    std::function<void(Event)> post_;
    std::thread worker_;
    std::mutex mutex_;
    HANDLE pipe_ = nullptr;
    HANDLE helper_ = nullptr;
    std::atomic<bool> owned_{false}, stopping_{false}, generationBusy_{false};
    uint64_t generation_ = 0;
    void serve(uint64_t generation);
    void closeHandles();
    bool writeCommand(const char *command);
};
} // namespace bm::app
