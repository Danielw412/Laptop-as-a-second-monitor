#pragma once
// One-time elevated setup: stage the driver, install the display helper under Program Files and register the
// highest-privilege scheduled task the unelevated app uses to start it. Also its exact inverse.
#include <filesystem>
#include <string>
#include <windows.h>
namespace bm::app {
struct SetupStatus {
    bool taskRegistered = false;
    bool helperInstalled = false;
    bool helperCurrent = true; // Installed helper matches the one next to this executable
    bool driverStaged = false;
    bool ready() const {
        return taskRegistered && helperInstalled && driverStaged;
    }
    std::string summary() const;
};
SetupStatus querySetupStatus();
std::filesystem::path installedHelperPath();
std::filesystem::path bundledHelperPath();
bool driverPackageStaged();
/// Launches this executable elevated with the given switch. Returns the process handle (caller waits) or null when
/// the UAC prompt was declined or launching failed; `error` receives the reason.
HANDLE launchElevated(HWND owner, const wchar_t *argument, std::wstring &error);
/// Body of `BrowserMonitor --setup` (must already be elevated). Returns 0 on success.
int performSetup();
/// Body of `BrowserMonitor --uninstall` (must already be elevated). Returns 0 on success.
int performUninstall();
bool isElevated();
/// Runs the display task on demand with the given argument. Returns false with `error` set on failure.
bool runDisplayTask(const std::wstring &argument, std::wstring &error);
bool displayTaskRegistered();
bool startAtSignIn();
void setStartAtSignIn(bool);
} // namespace bm::app
