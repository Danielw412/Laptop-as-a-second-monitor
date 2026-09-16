#pragma once
// Drives the elevated display helper from the unelevated app: runs its scheduled task, talks to it over a
// nonce-named pipe, and reports what happened as controller events.
//
// The pipe is opened for overlapped I/O. That is not an optimisation: the worker thread parks in a read for the
// whole life of the helper, and Windows serialises I/O on a synchronous handle, so a "stop" written from the UI
// thread would queue behind that read and never complete. All waiting happens on the worker thread and every wait
// is bounded, so no thread outlives this object.
#include "app_state.hpp"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>
namespace lm::app {
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
    /// Stops reporting and joins the worker. Must run before the event sink's own state is destroyed.
    void shutdown();

  private:
    std::function<void(Event)> post_;
    std::thread worker_;
    std::mutex mutex_;      // Guards the two handles below
    HANDLE pipe_ = nullptr; // Overlapped client end of the helper's pipe
    HANDLE helper_ = nullptr;
    HANDLE wake_ = nullptr; // Manual-reset: release the worker from its read
    std::atomic<bool> owned_{false}, stopping_{false}, busy_{false}, quiet_{false};
    std::atomic<uint64_t> generation_{0};
    void serve(uint64_t generation);
    void closeHandles();
    bool writeCommand(const char *command);
    /// Posts an event unless we are shutting down; by then the sink's own state may already be gone.
    void report(Event);
};
} // namespace lm::app
