#include "ui/tray.hpp"
#include <cstring>
namespace bm::app::ui {
void TrayIcon::add(HWND owner, UINT callbackMessage, HICON icon, const wchar_t *tip) {
    if (added_)
        remove();
    data_ = {};
    data_.cbSize = sizeof data_;
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = callbackMessage;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tip, _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) {
        data_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data_);
    }
}
void TrayIcon::update(HICON icon, const wchar_t *tip) {
    if (!added_)
        return;
    data_.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.hIcon = icon;
    wcsncpy_s(data_.szTip, tip, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data_);
}
void TrayIcon::balloon(const wchar_t *title, const wchar_t *text) {
    if (!added_)
        return;
    NOTIFYICONDATAW info = data_;
    info.uFlags = NIF_INFO | NIF_SHOWTIP;
    info.dwInfoFlags = NIIF_USER | NIIF_LARGE_ICON | NIIF_RESPECT_QUIET_TIME;
    info.hBalloonIcon = data_.hIcon;
    wcsncpy_s(info.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(info.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &info);
}
void TrayIcon::remove() {
    if (added_) {
        Shell_NotifyIconW(NIM_DELETE, &data_);
        added_ = false;
    }
}
} // namespace bm::app::ui
