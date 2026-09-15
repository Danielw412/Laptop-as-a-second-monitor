// Browser Monitor desktop application entry point.
//   BrowserMonitor.exe                start (or bring the running instance forward)
//   BrowserMonitor.exe --background   start hidden in the tray (used by "start at sign-in")
//   BrowserMonitor.exe --setup        elevated one-time setup (launched by the app through UAC)
//   BrowserMonitor.exe --uninstall    elevated removal of everything setup created
#include "app.hpp"
#include "logging.hpp"
#include "settings.hpp"
#include "setup.hpp"
#include "ui/main_window.hpp"
#include <shellapi.h>
using namespace bm;
using namespace bm::app;
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
        if (settings.diagnosticsLog)
            Log::instance().openFile(logDirectory());
        logInfo("Browser Monitor " BM_VERSION " starting");
        // Keep the registry in step with the setting in case the executable moved.
        if (settings.startAtSignIn)
            setStartAtSignIn(true);
        std::string secret = loadOrCreateHostSecret(credentialPath());
        ui::MainWindow window(instance, background, std::move(store), std::move(settings), std::move(secret));
        code = window.run();
        logInfo("Browser Monitor exited");
    } catch (const std::exception &e) {
        logError(std::string("Fatal: ") + e.what());
        MessageBoxW(nullptr, (L"Browser Monitor could not start:\n\n" + widen(e.what())).c_str(), L"Browser Monitor",
                    MB_ICONERROR | MB_OK);
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return code;
}
