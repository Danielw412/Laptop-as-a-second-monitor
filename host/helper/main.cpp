// BrowserMonitorDisplay: the only elevated piece. It creates the software device that loads the BrowserMonitorIdd
// driver (the virtual monitor exists while this process holds the device) and takes exactly two commands from the
// unelevated app over a named pipe: stop and ping. It exits, removing the monitor, when told to, when the pipe
// closes, or when the app process that started it goes away.
//
//   BrowserMonitorDisplay.exe --serve <appPid>:<nonce>
//
// Started through the "Browser Monitor Display" scheduled task that setup registers with highest privileges.
#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <string>
#include <swdevice.h>
#include <vector>
namespace {
struct Creation {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT result = E_PENDING;
    std::wstring instanceId;
};
void WINAPI created(HSWDEVICE, HRESULT hr, PVOID context, PCWSTR instanceId) {
    auto *c = static_cast<Creation *>(context);
    c->result = hr;
    if (instanceId)
        c->instanceId = instanceId;
    SetEvent(c->event);
}
std::string narrow(const std::wstring &w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
bool writeLine(HANDLE pipe, const std::string &line) {
    std::string out = line + "\n";
    DWORD written = 0;
    return WriteFile(pipe, out.data(), DWORD(out.size()), &written, nullptr) && written == out.size();
}
/// Allows the interactive user (same account as this elevated process) to open the pipe read/write.
SECURITY_ATTRIBUTES *pipeSecurity(SECURITY_ATTRIBUTES &sa, PSECURITY_DESCRIPTOR &descriptor) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return nullptr;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        CloseHandle(token);
        return nullptr;
    }
    CloseHandle(token);
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sidText))
        return nullptr;
    // DACL only (no explicit owner, which an unelevated diagnostic run could not assign): full control for SYSTEM
    // and Administrators, read/write for the interactive user who started the task.
    std::wstring sddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + std::wstring(sidText) + L")";
    LocalFree(sidText);
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
        return nullptr;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = descriptor;
    sa.bInheritHandle = FALSE;
    return &sa;
}
int serve(const std::wstring &argument) {
    auto colon = argument.find(L':');
    if (colon == std::wstring::npos)
        return 2;
    DWORD appPid = DWORD(std::wcstoul(argument.substr(0, colon).c_str(), nullptr, 10));
    std::wstring nonce = argument.substr(colon + 1);
    if (!appPid || nonce.size() < 16 || nonce.size() > 64)
        return 2;
    for (wchar_t c : nonce)
        if (!iswalnum(c))
            return 2;
    HANDLE app = OpenProcess(SYNCHRONIZE, FALSE, appPid);
    if (!app)
        return 3; // The app is already gone; nothing to serve.
    SECURITY_ATTRIBUTES sa{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    auto security = pipeSecurity(sa, descriptor);
    std::wstring pipeName = L"\\\\.\\pipe\\BrowserMonitor.Display." + nonce;
    HANDLE pipe = CreateNamedPipeW(pipeName.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1,
                                   4096, 4096, 0, security);
    if (descriptor)
        LocalFree(descriptor);
    if (pipe == INVALID_HANDLE_VALUE)
        return 4;
    // Create the device before accepting the client so the first line is the outcome.
    Creation creation;
    SW_DEVICE_CREATE_INFO info{};
    info.cbSize = sizeof info;
    info.pszInstanceId = L"BrowserMonitorIdd";
    info.pszzHardwareIds = L"BrowserMonitorIdd\0\0";
    info.pszzCompatibleIds = L"BrowserMonitorIdd\0\0";
    info.pszDeviceDescription = L"Browser Monitor Virtual Display";
    info.CapabilityFlags =
        SWDeviceCapabilitiesRemovable | SWDeviceCapabilitiesSilentInstall | SWDeviceCapabilitiesDriverRequired;
    HSWDEVICE device = nullptr;
    HRESULT hr = SwDeviceCreate(L"BrowserMonitorIdd", L"HTREE\\ROOT\\0", &info, 0, nullptr, created, &creation, &device);
    std::string status;
    if (FAILED(hr))
        status = "error " + std::to_string(uint32_t(hr)) + " SwDeviceCreate failed";
    else if (WaitForSingleObject(creation.event, 15000) != WAIT_OBJECT_0)
        status = "error 0 Timed out waiting for the virtual display device";
    else if (FAILED(creation.result))
        status = "error " + std::to_string(uint32_t(creation.result)) +
                 " Device creation failed; is the driver package installed?";
    else
        status = "ready " + narrow(creation.instanceId);
    const bool ok = status.starts_with("ready");
    // Wait up to 20 s for the app to connect.
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = false;
    if (ConnectNamedPipe(pipe, &overlapped) || GetLastError() == ERROR_PIPE_CONNECTED)
        connected = true;
    else if (GetLastError() == ERROR_IO_PENDING) {
        HANDLE waits[] = {overlapped.hEvent, app};
        connected = WaitForMultipleObjects(2, waits, FALSE, 20000) == WAIT_OBJECT_0;
    }
    if (!connected || !writeLine(pipe, status) || !ok) {
        if (device)
            SwDeviceClose(device);
        CloseHandle(pipe);
        return ok ? 5 : 1;
    }
    // Serve until told to stop, the pipe breaks, or the app exits.
    std::string buffer;
    char chunk[256];
    for (;;) {
        OVERLAPPED read{};
        read.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL started = ReadFile(pipe, chunk, sizeof chunk, &got, &read);
        if (!started && GetLastError() != ERROR_IO_PENDING) {
            CloseHandle(read.hEvent);
            break; // Pipe closed: the app is gone
        }
        if (!started) {
            HANDLE waits[] = {read.hEvent, app};
            DWORD which = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (which != WAIT_OBJECT_0) {
                CancelIo(pipe);
                CloseHandle(read.hEvent);
                break; // App process exited
            }
            if (!GetOverlappedResult(pipe, &read, &got, FALSE)) {
                CloseHandle(read.hEvent);
                break;
            }
        }
        CloseHandle(read.hEvent);
        buffer.append(chunk, got);
        bool stop = false;
        size_t newline;
        while ((newline = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newline);
            buffer.erase(0, newline + 1);
            if (line == "ping")
                writeLine(pipe, "pong");
            else if (line == "stop")
                stop = true;
        }
        if (stop)
            break;
    }
    SwDeviceClose(device);
    writeLine(pipe, "stopped");
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    CloseHandle(app);
    return 0;
}
} // namespace
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 2;
    int code = 2;
    if (argc >= 3 && std::wstring(argv[1]) == L"--serve")
        code = serve(argv[2]);
    else if (argc >= 2 && std::wstring(argv[1]) == L"--version") {
        // For setup's version check: the exit code encodes success; stdout is unavailable in a GUI subsystem.
        code = 0;
    } else
        MessageBoxW(nullptr,
                    L"BrowserMonitorDisplay is started by Browser Monitor through its scheduled task.\n"
                    L"Run BrowserMonitor.exe instead.",
                    L"Browser Monitor", MB_ICONINFORMATION);
    LocalFree(argv);
    return code;
}
