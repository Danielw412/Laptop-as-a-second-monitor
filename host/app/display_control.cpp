#include "display_control.hpp"
#include "app.hpp"
#include "logging.hpp"
#include "setup.hpp"
#include "sha256.hpp"
#include <bcrypt.h>
namespace lm::app {
namespace {
constexpr DWORD kPipeConnectMs = 25000; // The task engine has to launch the helper first
constexpr DWORD kReadyMs = 20000;       // ...and the helper then has to create the device
constexpr DWORD kWriteMs = 3000;        // A command the helper is not reading means the helper is gone
constexpr DWORD kPollMs = 200;          // How often the watch loop re-checks its deadlines
constexpr DWORD kStopConfirmMs = 8000;  // Report the stop anyway if the helper never confirms it
constexpr DWORD kExitWaitMs = 5000;     // Let the helper finish removing the device before we report
std::wstring randomNonce() {
    uint8_t bytes[16];
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Secure random generator unavailable");
    return widen(hexLower(bytes, sizeof bytes));
}
/// One overlapped read: >0 bytes read, 0 nothing arrived within `timeoutMs` (or `wake` fired), -1 the pipe is
/// gone. A read this cancels is always collected before returning, so `chunk` is free again on every path.
int readChunk(HANDLE pipe, HANDLE event, HANDLE wake, char *chunk, DWORD size, DWORD timeoutMs) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    ResetEvent(event);
    DWORD got = 0;
    bool cancelled = false;
    if (!ReadFile(pipe, chunk, size, &got, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING)
            return -1;
        HANDLE waits[] = {event, wake};
        if (WaitForMultipleObjects(wake ? 2 : 1, waits, FALSE, timeoutMs) != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &overlapped);
            cancelled = true;
        }
    }
    if (!GetOverlappedResult(pipe, &overlapped, &got, TRUE))
        return cancelled && GetLastError() == ERROR_OPERATION_ABORTED ? 0 : -1;
    // The read may have completed in the gap before the cancel took effect; those bytes are still ours.
    return got ? int(got) : -1;
}
/// Reads one line, waiting up to `timeoutMs` in total. Returns false on timeout or disconnect.
bool readLine(HANDLE pipe, HANDLE event, std::string &buffer, std::string &line, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        const auto now = GetTickCount64();
        if (now >= deadline)
            return false;
        char chunk[256];
        int got = readChunk(pipe, event, nullptr, chunk, sizeof chunk, DWORD(deadline - now));
        if (got < 0)
            return false;
        buffer.append(chunk, size_t(got));
    }
}
} // namespace
DisplayController::DisplayController(std::function<void(Event)> post) : post_(std::move(post)) {
    wake_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}
DisplayController::~DisplayController() {
    shutdown();
    closeHandles();
    if (wake_)
        CloseHandle(wake_);
}
void DisplayController::shutdown() {
    abandon();
    quiet_ = true; // Anything the worker still has to say would arrive after our owner stopped listening
    if (wake_)
        SetEvent(wake_);
    if (worker_.joinable())
        worker_.join();
}
void DisplayController::report(Event e) {
    if (!quiet_ && post_)
        post_(std::move(e));
}
void DisplayController::closeHandles() {
    std::lock_guard lock(mutex_);
    if (pipe_) {
        CloseHandle(pipe_);
        pipe_ = nullptr;
    }
    if (helper_) {
        CloseHandle(helper_);
        helper_ = nullptr;
    }
}
bool DisplayController::writeCommand(const char *command) {
    std::lock_guard lock(mutex_);
    if (!pipe_)
        return false;
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event)
        return false;
    // `line` backs the overlapped buffer, so every path below collects the write before it goes out of scope.
    std::string line = std::string(command) + "\n";
    OVERLAPPED overlapped{};
    overlapped.hEvent = event;
    DWORD written = 0;
    bool started = WriteFile(pipe_, line.data(), DWORD(line.size()), &written, &overlapped) != 0;
    if (!started && GetLastError() != ERROR_IO_PENDING) {
        CloseHandle(event);
        return false;
    }
    if (!started && WaitForSingleObject(event, kWriteMs) != WAIT_OBJECT_0)
        CancelIoEx(pipe_, &overlapped);
    const bool ok = GetOverlappedResult(pipe_, &overlapped, &written, TRUE) != 0;
    CloseHandle(event);
    return ok && written == line.size();
}
void DisplayController::start() {
    if (busy_.exchange(true)) {
        logWarning("Display helper start ignored: an operation is already in progress");
        return;
    }
    if (worker_.joinable()) {
        // A helper from an earlier generation may still be connected: take it down before replacing it, so the
        // join below is bounded and we never leave a second virtual display behind.
        if (owned_) {
            stopping_ = true;
            writeCommand("stop");
        }
        SetEvent(wake_);
        worker_.join();
        ResetEvent(wake_);
    }
    stopping_ = false;
    const auto generation = ++generation_;
    worker_ = std::thread([this, generation] { serve(generation); });
}
void DisplayController::serve(uint64_t generation) {
    // Starting the virtual display is the longest wait in the whole app: a scheduled task has to launch an
    // elevated process, which then creates a software device Windows has to enumerate. Timed in three parts so a
    // slow start can be blamed on the right one.
    const auto startedAt = GetTickCount64();
    uint64_t taskLaunchedAt = 0, pipeConnectedAt = 0;
    auto giveUp = [&](std::string reason) {
        busy_ = false;
        logWarning("Virtual display did not start after " + std::to_string(GetTickCount64() - startedAt) +
                   " ms: " + reason);
        report({EventType::DisplayStartFailed, std::move(reason)});
    };
    std::wstring nonce;
    try {
        nonce = randomNonce();
    } catch (const std::exception &e) {
        giveUp(e.what());
        return;
    }
    std::wstring argument = std::to_wstring(GetCurrentProcessId()) + L":" + nonce;
    std::wstring error;
    if (!runDisplayTask(argument, error)) {
        giveUp(narrow(error));
        return;
    }
    taskLaunchedAt = GetTickCount64();
    // The helper creates the pipe once the task engine has launched it; that takes a moment.
    std::wstring pipeName = L"\\\\.\\pipe\\LaptopMonitor.Display." + nonce;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + kPipeConnectMs;
    while (GetTickCount64() < deadline && WaitForSingleObject(wake_, 0) != WAIT_OBJECT_0) {
        pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        DWORD code = GetLastError();
        if (code == ERROR_PIPE_BUSY)
            WaitNamedPipeW(pipeName.c_str(), 500);
        else
            Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        giveUp("The display helper did not start. Windows may have blocked the task, or setup needs to be "
               "repeated.");
        return;
    }
    pipeConnectedAt = GetTickCount64();
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        CloseHandle(pipe);
        giveUp("Could not create the event the display helper pipe needs.");
        return;
    }
    auto abandonStart = [&](HANDLE helper, std::string reason) {
        if (helper)
            CloseHandle(helper);
        CloseHandle(event);
        CloseHandle(pipe);
        giveUp(std::move(reason));
    };
    // Verify the pipe really belongs to our installed helper before trusting anything it says.
    ULONG serverPid = 0;
    HANDLE helper = nullptr;
    if (GetNamedPipeServerProcessId(pipe, &serverPid)) {
        helper = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, serverPid);
        if (helper) {
            wchar_t image[MAX_PATH * 2];
            DWORD size = DWORD(std::size(image));
            if (QueryFullProcessImageNameW(helper, 0, image, &size)) {
                auto expected = installedHelperPath().wstring();
                if (_wcsicmp(image, expected.c_str()) != 0) {
                    logWarning("Display helper pipe is served by an unexpected image: " + narrow(image));
                    abandonStart(helper, "The display helper pipe was not served by the installed "
                                         "LaptopMonitorDisplay.exe.");
                    return;
                }
            }
        }
    }
    std::string buffer, line;
    if (!readLine(pipe, event, buffer, line, kReadyMs)) {
        abandonStart(helper, "The display helper did not report its status.");
        return;
    }
    if (!line.starts_with("ready")) {
        // "error <hresult> <message>"
        std::string message = line.starts_with("error ") ? line.substr(6) : line;
        auto space = message.find(' ');
        std::string code = space == std::string::npos ? "" : message.substr(0, space);
        std::string text = space == std::string::npos ? message : message.substr(space + 1);
        if (!code.empty() && code != "0") {
            char hex[16];
            std::snprintf(hex, sizeof hex, "0x%08x", unsigned(std::stoul(code)));
            text += " (" + std::string(hex) + ")";
        }
        abandonStart(helper, text);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        pipe_ = pipe;
        helper_ = helper;
    }
    owned_ = true;
    busy_ = false;
    const auto readyAt = GetTickCount64();
    logInfo("Virtual display device created (" + line.substr(6) + ") in " +
            std::to_string(readyAt - startedAt) + " ms (scheduled task " +
            std::to_string(taskLaunchedAt - startedAt) + " ms, helper pipe " +
            std::to_string(pipeConnectedAt - taskLaunchedAt) + " ms, device " +
            std::to_string(readyAt - pipeConnectedAt) + " ms)");
    report({EventType::DisplayStarted});
    // Watch the pipe until the helper leaves. stop() writes on this same pipe from the UI thread, which is why the
    // handle is overlapped; the short poll interval is what lets us notice an unconfirmed stop.
    uint64_t stopDeadline = 0;
    while (WaitForSingleObject(wake_, 0) != WAIT_OBJECT_0) {
        char chunk[256];
        int got = readChunk(pipe, event, wake_, chunk, sizeof chunk, kPollMs);
        if (got < 0)
            break; // The helper closed the pipe or exited
        buffer.append(chunk, size_t(got));
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line == "stopped")
                logInfo("Virtual display device removed");
        }
        if (stopping_ && !stopDeadline)
            stopDeadline = GetTickCount64() + kStopConfirmMs;
        if (stopDeadline && GetTickCount64() >= stopDeadline) {
            // Safety net: the UI must never be left waiting on a helper that stopped answering.
            logWarning("Display helper did not exit within " + std::to_string(kStopConfirmMs / 1000) +
                       " s; continuing");
            break;
        }
    }
    CloseHandle(event);
    if (helper)
        WaitForSingleObject(helper, kExitWaitMs); // The device is gone once the process that held it is
    closeHandles();
    owned_ = false;
    if (generation != generation_)
        return; // A newer start superseded this helper; its outcome is reported by the newer serve()
    report({stopping_ ? EventType::DisplayStopped : EventType::DisplayHelperExited});
}
void DisplayController::stop() {
    if (!owned_) {
        report({EventType::DisplayStopped});
        return;
    }
    stopping_ = true;
    // The worker reports DisplayStopped once the helper goes, or after kStopConfirmMs if it never does. Nothing
    // here waits: this runs on the UI thread.
    if (!writeCommand("stop"))
        logWarning("Could not send stop to the display helper");
}
void DisplayController::abandon() {
    if (owned_) {
        stopping_ = true;
        writeCommand("stop");
    }
}
} // namespace lm::app
