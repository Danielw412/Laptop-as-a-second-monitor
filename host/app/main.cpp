// Laptop Monitor desktop application entry point.
//   LaptopMonitor.exe                start (or bring the running instance forward)
//   LaptopMonitor.exe --background   start hidden in the tray (used by "start at sign-in")
//   LaptopMonitor.exe --setup        elevated one-time setup (launched by the app through UAC)
//   LaptopMonitor.exe --uninstall    elevated removal of everything setup created
//   LaptopMonitor.exe --diagnostic-tag scrolling
//                                    label this run's diagnostics (session.json and every perf.jsonl record) for a
//                                    controlled test; combines with --background
#include "app.hpp"
#include "diagnostics.hpp"
#include "logging.hpp"
#include "resources.hpp"
#include "session_archive.hpp"
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
    runIdentity(); // Computed once, up front, so nothing later pays for it
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool background = false, setup = false, uninstall = false, tagged = false;
    std::string tag;
    for (int i = 1; argv && i < argc; ++i) {
        std::wstring_view a = argv[i];
        if (a == L"--background")
            background = true;
        else if (a == L"--setup")
            setup = true;
        else if (a == L"--uninstall")
            uninstall = true;
        else if (a == L"--diagnostic-tag") {
            tagged = true;
            if (i + 1 < argc)
                tag = narrow(argv[++i]);
        }
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
        // A tag belongs to a whole run, and that run has already started untagged (or with another tag).
        if (tagged)
            MessageBoxW(nullptr,
                        L"Laptop Monitor is already running, so this launch cannot start a tagged diagnostics run. "
                        L"Exit Laptop Monitor from its tray icon, then launch it again with --diagnostic-tag.",
                        L"Laptop Monitor", MB_ICONINFORMATION | MB_OK);
        activateExisting();
        return 0;
    }
    if (tagged) {
        if (validDiagnosticTag(tag))
            setDiagnosticTag(tag);
        else {
            MessageBoxW(nullptr,
                        L"The diagnostic tag was ignored. Use 1 to 48 letters, digits, dots, dashes or underscores, "
                        L"for example --diagnostic-tag scrolling.",
                        L"Laptop Monitor", MB_ICONWARNING | MB_OK);
            tag.clear();
        }
    }
    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = 1;
    std::string fatal;
    try {
        SettingsStore store(settingsPath());
        Settings settings = store.load();
        rememberLaunch({{"background", background},
                        {"diagnostic_tag", tag.empty() ? nlohmann::json(nullptr) : nlohmann::json(tag)}});
        std::vector<std::pair<LogLevel, std::string>> notes;
        if (settings.diagnosticsLog)
            notes = startDiagnostics(settings);
        auto elapsed = [&launched] {
            return std::to_string(
                int(std::chrono::duration<double, std::milli>(Clock::now() - launched).count()));
        };
        logInfo("Laptop Monitor " LM_VERSION " starting");
        // The rolling host.log holds several runs back to back; this line is where each one begins.
        logInfo("Run " + runIdentity().runId + " (process " + std::to_string(runIdentity().pid) + ")" +
                (tag.empty() ? "" : " | diagnostic tag " + tag));
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
        for (auto &[level, line] : notes)
            Log::instance().write(level, line);
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
        fatal = e.what();
        logError(std::string("Fatal: ") + e.what());
        MessageBoxW(nullptr, (L"Laptop Monitor could not start:\n\n" + widen(e.what())).c_str(), L"Laptop Monitor",
                    MB_ICONERROR | MB_OK);
    }
    // The window, and with it the engine, is gone by now, so its last lines are already in the archive.
    if (fatal.empty())
        stopDiagnostics(termination::kGraceful, {{"exit_code", code}});
    else
        stopDiagnostics(termination::kFatal, {{"fatal_error", fatal}});
    if (SUCCEEDED(com))
        CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
