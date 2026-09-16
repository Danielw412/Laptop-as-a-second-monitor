#pragma once
#include <string>
#include <windows.h>
#include <shellapi.h>
namespace lm::app::ui {
class TrayIcon {
  public:
    void add(HWND owner, UINT callbackMessage, HICON icon, const wchar_t *tip);
    void update(HICON icon, const wchar_t *tip);
    void balloon(const wchar_t *title, const wchar_t *text);
    void remove();
    bool added() const {
        return added_;
    }

  private:
    NOTIFYICONDATAW data_{};
    bool added_ = false;
};
} // namespace lm::app::ui
