#include "display_control.hpp"
#include "app.hpp"
#include "logging.hpp"
#include "setup.hpp"
#include "sha256.hpp"
#include <bcrypt.h>
namespace bm::app {
namespace {
std::wstring randomNonce() {
    uint8_t bytes[16];
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("Secure random generator unavailable");
    return widen(hexLower(bytes, sizeof bytes));
}
/// Reads one line from the pipe, waiting up to `timeoutMs`. Returns false on timeout or disconnect.
bool readLine(HANDLE pipe, std::string &buffer, std::string &line, DWORD timeoutMs) {
    const auto deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        auto newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            return true;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
            return false;
        if (!available) {
            if (GetTickCount64() >= deadline)
                return false;
            Sleep(20);
            continue;
        }
        char chunk[256];
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, DWORD(std::min<DWORD>(available, sizeof chunk)), &got, nullptr) || !got)
            return false;
        buffer.append(chunk, got);
    }
}
} // namespace
DisplayController::DisplayController(std::function<void(Event)> post) : post_(std::move(post)) {}
DisplayController::~DisplayController() {
    abandon();
    if (worker_.joinable())
        worker_.join();
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
    std::string line = std::string(command) + "\n";
    DWORD written = 0;
    return WriteFile(pipe_, line.data(), DWORD(line.size()), &written, nullptr) && written == line.size();
}
void DisplayController::start() {
    if (generationBusy_.exchange(true)) {
        logWarning("Display helper start ignored: an operation is already in progress");
        return;
    }
    if (worker_.joinable())
        worker_.join();
    stopping_ = false;
    const auto generation = ++generation_;
    worker_ = std::thread([this, generation] { serve(generation); });
}
void DisplayController::serve(uint64_t generation) {
    std::wstring nonce;
    try {
        nonce = randomNonce();
    } catch (const std::exception &e) {
        generationBusy_ = false;
        post_({EventType::DisplayStartFailed, e.what()});
        return;
    }
    std::wstring argument = std::to_wstring(GetCurrentProcessId()) + L":" + nonce;
    std::wstring error;
    if (!runDisplayTask(argument, error)) {
        generationBusy_ = false;
        post_({EventType::DisplayStartFailed, narrow(error)});
        return;
    }
    // The helper creates the pipe once the task engine has launched it; that takes a moment.
    std::wstring pipeName = L"\\\\.\\pipe\\BrowserMonitor.Display." + nonce;
    HANDLE pipe = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + 25000;
    while (GetTickCount64() < deadline) {
        pipe = CreateFileW(pipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE)
            break;
        DWORD code = GetLastError();
        if (code == ERROR_PIPE_BUSY)
            WaitNamedPipeW(pipeName.c_str(), 500);
        else
            Sleep(100);
    }
    if (pipe == INVALID_HANDLE_VALUE) {
        generationBusy_ = false;
        post_({EventType::DisplayStartFailed, "The display helper did not start. Windows may have blocked the task, "
                                              "or setup needs to be repeated."});
        return;
    }
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
                    CloseHandle(helper);
                    CloseHandle(pipe);
                    generationBusy_ = false;
                    post_({EventType::DisplayStartFailed, "The display helper pipe was not served by the installed "
                                                          "BrowserMonitorDisplay.exe."});
                    return;
                }
            }
        }
    }
    std::string buffer, line;
    if (!readLine(pipe, buffer, line, 20000)) {
        if (helper)
            CloseHandle(helper);
        CloseHandle(pipe);
        generationBusy_ = false;
        post_({EventType::DisplayStartFailed, "The display helper did not report its status."});
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
        if (helper)
            CloseHandle(helper);
        CloseHandle(pipe);
        generationBusy_ = false;
        post_({EventType::DisplayStartFailed, text});
        return;
    }
    {
        std::lock_guard lock(mutex_);
        pipe_ = pipe;
        helper_ = helper;
    }
    owned_ = true;
    generationBusy_ = false;
    logInfo("Virtual display device created (" + line.substr(6) + ")");
    post_({EventType::DisplayStarted});
    // Watch the pipe until the helper leaves. The read blocks; stop() writes on the same pipe from another thread.
    for (;;) {
        char chunk[256];
        DWORD got = 0;
        if (!ReadFile(pipe, chunk, sizeof chunk, &got, nullptr) || !got)
            break;
        buffer.append(chunk, got);
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line == "stopped")
                logInfo("Virtual display device removed");
        }
    }
    if (helper)
        WaitForSingleObject(helper, 5000);
    closeHandles();
    owned_ = false;
    if (generation != generation_)
        return; // A newer start superseded this helper; its outcome is reported by the newer serve()
    post_({stopping_ ? EventType::DisplayStopped : EventType::DisplayHelperExited});
}
void DisplayController::stop() {
    if (!owned_) {
        post_({EventType::DisplayStopped});
        return;
    }
    stopping_ = true;
    if (!writeCommand("stop")) {
        // The pipe is already gone; the watcher will report the exit.
        logWarning("Could not send stop to the display helper");
    }
    // Safety net: if the helper never confirms, report the stop anyway after 8 s so the UI cannot hang.
    std::thread([this] {
        HANDLE helper = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (helper_)
                DuplicateHandle(GetCurrentProcess(), helper_, GetCurrentProcess(), &helper, 0, FALSE,
                                DUPLICATE_SAME_ACCESS);
        }
        if (helper) {
            if (WaitForSingleObject(helper, 8000) == WAIT_TIMEOUT && owned_) {
                logWarning("Display helper did not exit within 8 s; continuing");
                post_({EventType::DisplayStopped});
            }
            CloseHandle(helper);
        }
    }).detach();
}
void DisplayController::abandon() {
    if (owned_) {
        stopping_ = true;
        writeCommand("stop");
    }
}
} // namespace bm::app
