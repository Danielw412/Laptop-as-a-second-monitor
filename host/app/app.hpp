#pragma once
// Shared declarations for the desktop application: window messages, timers and small helpers.
#include "platform.hpp"
#include <string>
namespace lm::app {
inline constexpr UINT WM_APP_EVENT = WM_APP + 1;    // Controller event queue has entries (any thread -> UI)
inline constexpr UINT WM_APP_TRAY = WM_APP + 2;     // Shell_NotifyIcon callback
inline constexpr UINT WM_APP_ACTIVATE = WM_APP + 3; // A second instance asked us to come forward
inline constexpr UINT WM_APP_SETUP_DONE = WM_APP + 4; // Elevated setup/uninstall process finished; wParam = exit code
inline constexpr UINT_PTR TIMER_UI = 1;             // Metrics/countdown repaint
inline constexpr UINT_PTR TIMER_FIND = 2;           // Poll for LaptopMon while the display starts
inline constexpr UINT_PTR TIMER_TOAST = 3;          // Clear transient "Copied" feedback
inline constexpr const wchar_t *kWindowClass = L"LaptopMonitorMain";
inline constexpr const wchar_t *kInstanceMutex = L"Local\\LaptopMonitor.App.Instance";
inline constexpr const wchar_t *kTaskName = L"Laptop Monitor Display";
inline constexpr const wchar_t *kRunValue = L"LaptopMonitor";
inline std::wstring widen(std::string_view s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
inline std::string narrow(std::wstring_view w) {
    if (w.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
inline std::wstring lastErrorText(DWORD error = GetLastError()) {
    wchar_t *buffer = nullptr;
    auto n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = n ? std::wstring(buffer, n) : L"error " + std::to_wstring(error);
    if (buffer)
        LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        text.pop_back();
    return text;
}
std::wstring modulePath();
} // namespace lm::app
