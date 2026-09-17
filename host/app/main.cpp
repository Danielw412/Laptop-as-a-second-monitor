// Laptop Monitor desktop application entry point.
//   LaptopMonitor.exe                start (or bring the running instance forward)
//   LaptopMonitor.exe --background   start hidden in the tray (used by "start at sign-in")
//   LaptopMonitor.exe --setup        elevated one-time setup (launched by the app through UAC)
//   LaptopMonitor.exe --uninstall    elevated removal of everything setup created
#include "app.hpp"
#include "logging.hpp"
#include "resources.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "ui/main_window.hpp"
#include <shellapi.h>
using namespace lm;
using namespace lm::app;
namespace {
void activateExisting() {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (!existing)
        return;
    DWORD pid = 0;
    GetWindowThreadProcessId(existing, &pid);
    if (pid)
        AllowSetForegroundWindow(pid);
    PostMessageW(existing, WM_APP_ACTIVATE, 0, 0);
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto launched = Clock::now();
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool background = false, setup = false, uninstall = false;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring_view a = argv[i];
        if (a == L"--background")
            background = true;
        else if (a == L"--setup")
            setup = true;
        else if (a == L"--uninstall")
            uninstall = true;
    }
    if (argv)
        LocalFree(argv);
    if (setup || uninstall) {
        try {
            Log::instance().openFile(logDirectory(), "setup.log");
        } catch (...) {
        }
        return setup ? performSetup() : performUninstall();
    }
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        activateExisting();
        return 0;
    }
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 1;
    try {
        SettingsStore store(settingsPath());
        Settings settings = store.load();
        if (settings.diagnosticsLog) {
            Log::instance().openFile(logDirectory());
            Log::instance().openRecordFile(logDirectory());
        }
        auto elapsed = [&launched] {
            return std::to_string(
                int(std::chrono::duration<double, std::milli>(Clock::now() - launched).count()));
        };
        logInfo("Laptop Monitor " LM_VERSION " starting");
        logInfo(machineProfile());
        // Every later measurement is relative to this machine and these settings; without them a log is a list of
        // numbers with nothing to compare against.
        logInfo(std::string("Settings: ") + std::to_string(settings.fps) + " fps | quality " +
                qualityName(settings.quality) + " | capture " + backendName(settings.backend) + " | scale " +
                std::to_string(scalePercent(settings.displayScale)) + "% | start at sign-in " +
                (settings.startAtSignIn ? "on" : "off") + " | auto-start display " +
                (settings.autoStartDisplay ? "on" : "off") + " | signaling " + settings.signalingUrl);
        if (settings.diagnosticsLog)
            logInfo("Diagnostics: " + Log::instance().path().string() + " and " +
                    Log::instance().recordPath().string() + " (one performance sample per second)");
        // Keep the registry in step with the setting in case the executable moved.
        if (settings.startAtSignIn)
            setStartAtSignIn(true);
        std::string secret = loadOrCreateHostSecret(credentialPath());
        logInfo("Startup: settings and credential ready " + elapsed() + " ms after launch");
        ui::MainWindow window(instance, background, std::move(store), std::move(settings), std::move(secret));
        logInfo("Startup: window ready " + elapsed() + " ms after launch" +
                (background ? " (started hidden in the tray)" : ""));
        code = window.run();
        logInfo("Laptop Monitor exited");
    } catch (const std::exception &e) {
        logError(std::string("Fatal: ") + e.what());
        MessageBoxW(nullptr, (L"Laptop Monitor could not start:\n\n" + widen(e.what())).c_str(), L"Laptop Monitor",
                    MB_ICONERROR | MB_OK);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
