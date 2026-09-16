#include "setup.hpp"
#include "app.hpp"
#include "logging.hpp"
#include "settings.hpp"
#include "sha256.hpp"
#include <comdef.h>
#include <fstream>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
namespace lm::app {
namespace {
struct ComApartment {
    HRESULT hr;
    ComApartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t *s) : value(SysAllocString(s)) {}
    ~Bstr() {
        SysFreeString(value);
    }
    operator BSTR() const {
        return value;
    }
};
std::wstring hresultText(HRESULT hr) {
    _com_error error(hr);
    return error.ErrorMessage();
}
std::filesystem::path programFilesDirectory() {
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &path)))
        return L"C:\\Program Files";
    std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}
std::filesystem::path systemRoot() {
    wchar_t buffer[MAX_PATH];
    GetWindowsDirectoryW(buffer, MAX_PATH);
    return buffer;
}
std::wstring currentUserSid() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return {};
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    std::wstring result;
    if (GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &text)) {
            result = text;
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return result;
}
HRESULT openTaskFolder(ComPtr<ITaskService> &service, ComPtr<ITaskFolder> &root) {
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&service));
    if (FAILED(hr))
        return hr;
    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr))
        return hr;
    return root ? S_OK : service->GetFolder(Bstr(L"\\"), &root);
}
std::string fileHash(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    Sha256 hash;
    char buffer[65536];
    while (file.read(buffer, sizeof buffer) || file.gcount())
        hash.update(buffer, size_t(file.gcount()));
    auto digest = hash.digest();
    return hexLower(digest.data(), digest.size());
}
std::filesystem::path findScript(const wchar_t *name) {
    // Development layout: <repo>/build/<dir>/LaptopMonitor.exe with scripts in <repo>/scripts. Also accept a
    // scripts folder next to the executable for a copied distribution.
    auto exe = std::filesystem::path(modulePath()).parent_path();
    for (auto candidate : {exe / L"scripts" / name, exe.parent_path().parent_path() / L"scripts" / name,
                           exe.parent_path() / L"scripts" / name}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec))
            return candidate;
    }
    return {};
}
int runProcess(const std::wstring &commandLine) {
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring mutableLine = commandLine;
    if (!CreateProcessW(nullptr, mutableLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &si, &pi))
        return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}
void fail(const std::wstring &what) {
    logError("Setup: " + narrow(what));
    MessageBoxW(nullptr, what.c_str(), L"Laptop Monitor setup", MB_ICONERROR | MB_OK);
}
/// What this product installed while it was called Laptop Monitor. Left behind, the scheduled task would keep
/// elevating an executable nothing drives any more, so setup clears it rather than installing alongside it.
void removeLegacyInstall(bool includeUserData) {
    constexpr const wchar_t *legacyTask = L"Laptop Monitor Display";
    constexpr const wchar_t *legacyRunValue = L"LaptopMonitor";
    {
        ComApartment com;
        ComPtr<ITaskService> service;
        ComPtr<ITaskFolder> root;
        ComPtr<IRegisteredTask> task;
        if (SUCCEEDED(openTaskFolder(service, root)) && SUCCEEDED(root->GetTask(Bstr(legacyTask), &task))) {
            task->Stop(0);
            Sleep(500);
            if (SUCCEEDED(root->DeleteTask(Bstr(legacyTask), 0)))
                logInfo("Setup: removed the legacy scheduled task");
        }
    }
    std::error_code ec;
    auto legacyDir = programFilesDirectory() / L"Laptop Monitor";
    if (std::filesystem::exists(legacyDir, ec)) {
        for (int attempt = 0; attempt < 8 && std::filesystem::exists(legacyDir, ec); ++attempt) {
            std::filesystem::remove_all(legacyDir, ec);
            if (std::filesystem::exists(legacyDir, ec))
                Sleep(250);
        }
        logInfo(std::filesystem::exists(legacyDir, ec) ? "Setup: could not remove " + legacyDir.string()
                                                       : "Setup: removed " + legacyDir.string());
    }
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE,
                      &key) == ERROR_SUCCESS) {
        if (RegDeleteValueW(key, legacyRunValue) == ERROR_SUCCESS)
            logInfo("Setup: removed the legacy start-at-sign-in entry");
        RegCloseKey(key);
    }
    if (includeUserData) {
        wchar_t local[32768];
        auto size = GetEnvironmentVariableW(L"LOCALAPPDATA", local, DWORD(std::size(local)));
        if (size && size < std::size(local))
            std::filesystem::remove_all(std::filesystem::path(local) / L"LaptopMonitor", ec);
    }
}
} // namespace
std::string SetupStatus::summary() const {
    if (ready() && helperCurrent)
        return "Ready";
    std::string s;
    if (!driverStaged)
        s += "driver not installed";
    if (!helperInstalled)
        s += std::string(s.empty() ? "" : ", ") + "helper not installed";
    else if (!helperCurrent)
        s += std::string(s.empty() ? "" : ", ") + "helper out of date";
    if (!taskRegistered)
        s += std::string(s.empty() ? "" : ", ") + "task not registered";
    return s;
}
std::filesystem::path installedHelperPath() {
    return programFilesDirectory() / L"Laptop Monitor" / L"LaptopMonitorDisplay.exe";
}
std::filesystem::path bundledHelperPath() {
    return std::filesystem::path(modulePath()).parent_path() / L"LaptopMonitorDisplay.exe";
}
bool driverPackageStaged() {
    std::error_code ec;
    auto repository = systemRoot() / L"System32" / L"DriverStore" / L"FileRepository";
    for (auto &entry : std::filesystem::directory_iterator(repository, ec)) {
        auto name = entry.path().filename().wstring();
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return wchar_t(::towlower(c)); });
        if (name.starts_with(L"laptopmonitoridd.inf_") && entry.is_directory(ec))
            return true;
    }
    return false;
}
bool isElevated() {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof elevation;
    bool result = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated;
    CloseHandle(token);
    return result;
}
bool displayTaskRegistered() {
    ComApartment com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(openTaskFolder(service, root)))
        return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(root->GetTask(Bstr(kTaskName), &task));
}
SetupStatus querySetupStatus() {
    SetupStatus s;
    s.taskRegistered = displayTaskRegistered();
    std::error_code ec;
    s.helperInstalled = std::filesystem::exists(installedHelperPath(), ec);
    if (s.helperInstalled && std::filesystem::exists(bundledHelperPath(), ec))
        s.helperCurrent = fileHash(installedHelperPath()) == fileHash(bundledHelperPath());
    s.driverStaged = driverPackageStaged();
    return s;
}
bool runDisplayTask(const std::wstring &argument, std::wstring &error) {
    ComApartment com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    HRESULT hr = openTaskFolder(service, root);
    ComPtr<IRegisteredTask> task;
    if (SUCCEEDED(hr))
        hr = root->GetTask(Bstr(kTaskName), &task);
    if (FAILED(hr)) {
        error = L"The display task is not registered. Run setup again. (" + hresultText(hr) + L")";
        return false;
    }
    VARIANT params;
    VariantInit(&params);
    params.vt = VT_BSTR;
    params.bstrVal = SysAllocString(argument.c_str());
    ComPtr<IRunningTask> running;
    hr = task->Run(params, &running);
    VariantClear(&params);
    if (FAILED(hr)) {
        error = L"Could not start the display task: " + hresultText(hr);
        return false;
    }
    return true;
}
HANDLE launchElevated(HWND owner, const wchar_t *argument, std::wstring &error) {
    auto exe = modulePath();
    SHELLEXECUTEINFOW info{sizeof info};
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = argument;
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        DWORD code = GetLastError();
        error = code == ERROR_CANCELLED ? L"Administrator approval was declined." : lastErrorText(code);
        return nullptr;
    }
    return info.hProcess;
}
int performSetup() {
    if (!isElevated()) {
        fail(L"Setup must run elevated. Start it from Laptop Monitor's Settings page.");
        return 1;
    }
    logInfo("Setup: starting");
    removeLegacyInstall(false);
    // 1. Driver package.
    if (!driverPackageStaged()) {
        auto script = findScript(L"install-driver.ps1");
        if (script.empty()) {
            fail(L"The LaptopMonitorIdd driver package is not installed and scripts\\install-driver.ps1 was not "
                 L"found next to this build. Build and install the driver first (see README).");
            return 2;
        }
        logInfo("Setup: staging the driver with " + script.string());
        int code = runProcess(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() + L"\"");
        if (code != 0 || !driverPackageStaged()) {
            fail(L"Installing the virtual display driver failed (exit code " + std::to_wstring(code) +
                 L"). Run scripts\\install-driver.ps1 in an elevated PowerShell to see the details.");
            return 3;
        }
    }
    // 2. Helper under Program Files, so only administrators can replace what the task elevates.
    auto source = bundledHelperPath(), target = installedHelperPath();
    std::error_code ec;
    if (!std::filesystem::exists(source, ec)) {
        fail(L"LaptopMonitorDisplay.exe was not found next to LaptopMonitor.exe.");
        return 4;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    {
        // Stop a running helper first; the copy would otherwise hit a sharing violation.
        ComApartment com;
        ComPtr<ITaskService> service;
        ComPtr<ITaskFolder> root;
        ComPtr<IRegisteredTask> task;
        if (SUCCEEDED(openTaskFolder(service, root)) && SUCCEEDED(root->GetTask(Bstr(kTaskName), &task)))
            task->Stop(0);
    }
    bool copied = false;
    for (int attempt = 0; attempt < 20 && !copied; ++attempt) {
        copied = CopyFileW(source.c_str(), target.c_str(), FALSE) != 0;
        if (!copied)
            Sleep(250);
    }
    if (!copied) {
        fail(L"Could not copy the display helper to " + target.wstring() + L": " + lastErrorText());
        return 5;
    }
    // 3. Scheduled task: runs the installed helper with highest privileges, on demand, for this user only.
    ComApartment com;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    HRESULT hr = openTaskFolder(service, root);
    ComPtr<ITaskDefinition> definition;
    if (SUCCEEDED(hr))
        hr = service->NewTask(0, &definition);
    ComPtr<IRegistrationInfo> registration;
    if (SUCCEEDED(hr) && SUCCEEDED(definition->get_RegistrationInfo(&registration))) {
        registration->put_Author(Bstr(L"Laptop Monitor"));
        registration->put_Description(Bstr(L"Starts the Laptop Monitor virtual display helper on demand. Registered "
                                            L"by Laptop Monitor setup; removed by its uninstall."));
    }
    ComPtr<IPrincipal> principal;
    if (SUCCEEDED(hr))
        hr = definition->get_Principal(&principal);
    if (SUCCEEDED(hr)) {
        auto sid = currentUserSid();
        principal->put_Id(Bstr(L"Owner"));
        principal->put_UserId(Bstr(sid.c_str()));
        principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN);
        hr = principal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }
    ComPtr<ITaskSettings> settings;
    if (SUCCEEDED(hr) && SUCCEEDED(definition->get_Settings(&settings))) {
        settings->put_AllowDemandStart(VARIANT_TRUE);
        settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
        settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        settings->put_Hidden(VARIANT_TRUE);
        settings->put_StartWhenAvailable(VARIANT_FALSE);
        settings->put_RunOnlyIfIdle(VARIANT_FALSE);
        settings->put_Priority(5);
        settings->put_Enabled(VARIANT_TRUE);
        ComPtr<IIdleSettings> idle;
        if (SUCCEEDED(settings->get_IdleSettings(&idle)))
            idle->put_StopOnIdleEnd(VARIANT_FALSE);
    }
    ComPtr<IActionCollection> actions;
    ComPtr<IAction> action;
    ComPtr<IExecAction> exec;
    if (SUCCEEDED(hr))
        hr = definition->get_Actions(&actions);
    if (SUCCEEDED(hr))
        hr = actions->Create(TASK_ACTION_EXEC, &action);
    if (SUCCEEDED(hr))
        hr = action.As(&exec);
    if (SUCCEEDED(hr)) {
        exec->put_Path(Bstr(target.c_str()));
        exec->put_Arguments(Bstr(L"--serve $(Arg0)"));
        hr = exec->put_WorkingDirectory(Bstr(target.parent_path().c_str()));
    }
    ComPtr<IRegisteredTask> registered;
    if (SUCCEEDED(hr)) {
        VARIANT user, password, sddl;
        VariantInit(&user);
        VariantInit(&password);
        VariantInit(&sddl);
        hr = root->RegisterTaskDefinition(Bstr(kTaskName), definition.Get(), TASK_CREATE_OR_UPDATE, user, password,
                                          TASK_LOGON_INTERACTIVE_TOKEN, sddl, &registered);
    }
    if (FAILED(hr)) {
        fail(L"Registering the scheduled task failed: " + hresultText(hr));
        return 6;
    }
    logInfo("Setup: complete");
    return 0;
}
int performUninstall() {
    if (!isElevated()) {
        fail(L"Uninstall must run elevated. Start it from Laptop Monitor's Settings page.");
        return 1;
    }
    logInfo("Uninstall: starting");
    removeLegacyInstall(true);
    int problems = 0;
    {
        ComApartment com;
        ComPtr<ITaskService> service;
        ComPtr<ITaskFolder> root;
        ComPtr<IRegisteredTask> task;
        if (SUCCEEDED(openTaskFolder(service, root))) {
            if (SUCCEEDED(root->GetTask(Bstr(kTaskName), &task))) {
                task->Stop(0);
                Sleep(500);
            }
            HRESULT hr = root->DeleteTask(Bstr(kTaskName), 0);
            if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
                logWarning("Uninstall: could not delete the task: " + narrow(hresultText(hr)));
                ++problems;
            }
        }
    }
    std::error_code ec;
    auto helperDir = installedHelperPath().parent_path();
    for (int attempt = 0; attempt < 20 && std::filesystem::exists(helperDir, ec); ++attempt) {
        std::filesystem::remove_all(helperDir, ec);
        if (std::filesystem::exists(helperDir, ec))
            Sleep(250);
    }
    if (std::filesystem::exists(helperDir, ec)) {
        logWarning("Uninstall: could not remove " + helperDir.string());
        ++problems;
    }
    setStartAtSignIn(false);
    Log::instance().closeFile();
    std::filesystem::remove_all(appDataDirectory(), ec);
    // Driver package, device node and the local signing certificate: the install script knows exactly what it
    // created, so it removes them.
    auto script = findScript(L"install-driver.ps1");
    if (!script.empty()) {
        int code = runProcess(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + script.wstring() +
                              L"\" -Uninstall");
        if (code != 0)
            ++problems;
    } else if (driverPackageStaged()) {
        runProcess(L"pnputil.exe /remove-device /deviceid LaptopMonitorIdd");
        // Without the script, look up the published name and delete the package.
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE readEnd, writeEnd;
        if (CreatePipe(&readEnd, &writeEnd, &sa, 0)) {
            STARTUPINFOW si{sizeof si};
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = writeEnd;
            si.hStdError = writeEnd;
            PROCESS_INFORMATION pi{};
            std::wstring line = L"pnputil.exe /enum-drivers";
            if (CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                               &pi)) {
                CloseHandle(writeEnd);
                std::string output;
                char buffer[4096];
                DWORD got;
                while (ReadFile(readEnd, buffer, sizeof buffer, &got, nullptr) && got)
                    output.append(buffer, got);
                WaitForSingleObject(pi.hProcess, INFINITE);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                // Blocks look like "Published Name: oem149.inf\r\nOriginal Name: laptopmonitoridd.inf".
                size_t pos = 0;
                std::string lower = output;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                while ((pos = lower.find("laptopmonitoridd.inf", pos)) != std::string::npos) {
                    auto published = lower.rfind("oem", pos);
                    if (published != std::string::npos) {
                        auto end = lower.find(".inf", published);
                        auto name = output.substr(published, end + 4 - published);
                        runProcess(L"pnputil.exe /delete-driver " + widen(name) + L" /uninstall");
                    }
                    pos += 10;
                }
            } else
                CloseHandle(writeEnd);
            CloseHandle(readEnd);
        }
    }
    logInfo("Uninstall: complete");
    if (problems)
        MessageBoxW(nullptr, L"Laptop Monitor was removed, but some items could not be cleaned up. See the log.",
                    L"Laptop Monitor", MB_ICONWARNING | MB_OK);
    return problems ? 2 : 0;
}
bool startAtSignIn() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    bool present = RegQueryValueExW(key, kRunValue, nullptr, &type, nullptr, &size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return present;
}
void setStartAtSignIn(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    if (on) {
        std::wstring command = L"\"" + modulePath() + L"\" --background";
        RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE *>(command.c_str()),
                       DWORD((command.size() + 1) * sizeof(wchar_t)));
    } else
        RegDeleteValueW(key, kRunValue);
    RegCloseKey(key);
}
std::wstring modulePath() {
    wchar_t buffer[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buffer, DWORD(std::size(buffer)));
    return std::wstring(buffer, n);
}
} // namespace lm::app
