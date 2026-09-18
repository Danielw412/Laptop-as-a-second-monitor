#include "ui/main_window.hpp"
#include "app.hpp"
#include "logging.hpp"
#include "resources/resource.h"
#include <commctrl.h>
#include <iomanip>
#include <shellapi.h>
#include <sstream>
#include <windowsx.h>
namespace lm::app::ui {
namespace {
constexpr float kMargin = 24.f;
constexpr float kRight = 440.f - kMargin;
constexpr float kInner = kRight - kMargin;
constexpr UINT_PTR TIMER_EXIT = 4;
constexpr UINT_PTR TIMER_METRICS = 5;
constexpr UINT IDM_TRAY_OPEN = 1, IDM_TRAY_COPY = 2, IDM_TRAY_DISCONNECT = 3, IDM_TRAY_STREAM = 4,
               IDM_TRAY_MONITOR = 5, IDM_TRAY_RESTART = 6, IDM_TRAY_EXIT = 7, IDM_TRAY_COPY_LINK = 8;
constexpr UINT IDC_URL = 100;
D2D1_RECT_F rect(float x, float y, float w, float h) {
    return D2D1::RectF(x, y, x + w, y + h);
}
bool contains(const D2D1_RECT_F &r, D2D1_POINT_2F p) {
    return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
}
D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
std::wstring fixed(double v, int digits) {
    wchar_t buffer[64];
    swprintf_s(buffer, L"%.*f", digits, v);
    return buffer;
}
std::wstring fixed(const std::optional<double> &v, int digits, const wchar_t *unit = L"") {
    return v ? fixed(*v, digits) + unit : L"—";
}
std::wstring mbps(double bitsPerSecond) {
    return fixed(bitsPerSecond / 1e6, 1) + L" Mbps";
}
std::wstring mbps(const std::optional<double> &v) {
    return v ? mbps(*v) : L"—";
}
std::wstring duration(std::chrono::seconds s) {
    auto total = s.count();
    wchar_t buffer[32];
    if (total >= 3600)
        swprintf_s(buffer, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    else
        swprintf_s(buffer, L"%lld:%02lld", total / 60, total % 60);
    return buffer;
}
std::wstring shortEncoder(const std::string &name) {
    // "Intel® Quick Sync Video H.264 Encoder MFT" -> "Quick Sync H.264"; unknown names pass through trimmed.
    std::string s = name;
    auto lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (lower.find("quick sync") != std::string::npos)
        return L"Quick Sync";
    if (lower.find("nvidia") != std::string::npos || lower.find("nvenc") != std::string::npos)
        return L"NVENC";
    if (lower.find("amd") != std::string::npos || lower.find("amf") != std::string::npos)
        return L"AMD AMF";
    if (s.empty() || s == "none")
        return L"—";
    return widen(s);
}
} // namespace
MainWindow::MainWindow(HINSTANCE instance, bool startHidden, SettingsStore store, Settings settings,
                       std::string hostSecret)
    : instance_(instance), startHidden_(startHidden) {
    for (int i = 0; i < 6; ++i)
        LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP + i), LIM_SMALL, &icons_[i]);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = icons_[0];
    wc.hbrBackground = CreateSolidBrush(RGB(0xFB, 0xF7, 0xF3));
    RegisterClassExW(&wc);
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
    dpi_ = float(GetDpiForSystem());
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc{0, 0, LONG(kWidth * dpi_ / 96), LONG(kHeight * dpi_ / 96)};
    AdjustWindowRectExForDpi(&rc, style, FALSE, 0, UINT(dpi_));
    hwnd_ = CreateWindowExW(0, kWindowClass, L"Laptop Monitor", style, CW_USEDEFAULT, CW_USEDEFAULT,
                            rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, instance, this);
    if (!hwnd_)
        throw std::runtime_error("Cannot create the main window");
    const UINT actual = GetDpiForWindow(hwnd_);
    if (actual && float(actual) != dpi_)
        applyDpi(actual, nullptr);
    createControls();
    controller_ = std::make_unique<AppController>(hwnd_, std::move(store), std::move(settings), std::move(hostSecret));
    controller_->stateChanged([this] { onStateChanged(); });
    controller_->viewerConnectedChanged([this](bool on) {
        tray_.balloon(L"Laptop Monitor", on ? L"Receiver connected." : L"Receiver disconnected.");
    });
    tray_.add(hwnd_, WM_APP_TRAY, icons_[5], L"Laptop Monitor");
    uiResources_.sample(); // A CPU figure is a delta; without this the first report has nothing to subtract from.
    SetTimer(hwnd_, TIMER_UI, 1000, nullptr);
    SetTimer(hwnd_, TIMER_METRICS, 500, nullptr);
    syncUrlEdit();
    if (!startHidden_)
        showWindow();
    controller_->init();
}
MainWindow::~MainWindow() {
    tray_.remove();
    controller_.reset();
    if (editFont_)
        DeleteObject(editFont_);
    for (auto icon : icons_)
        if (icon)
            DestroyIcon(icon);
}
int MainWindow::run() {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (urlEdit_ && msg.hwnd == urlEdit_ && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_RETURN) {
                applyUrlFromEdit();
                SetFocus(hwnd_);
                continue;
            }
            if (msg.wParam == VK_TAB || msg.wParam == VK_ESCAPE) {
                if (msg.wParam == VK_ESCAPE)
                    syncUrlEdit();
                SetFocus(hwnd_);
                continue;
            }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return int(msg.wParam);
}
void MainWindow::createControls() {
    urlEdit_ = CreateWindowExW(0, WC_EDITW, L"", WS_CHILD | ES_AUTOHSCROLL | ES_LEFT, 0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(UINT_PTR(IDC_URL)), instance_, nullptr);
    SendMessageW(urlEdit_, EM_SETLIMITTEXT, 512, 0);
    applyDpi(UINT(dpi_), nullptr);
}
void MainWindow::applyDpi(UINT dpi, const RECT *suggested) {
    dpi_ = float(dpi);
    renderer_.setDpi(dpi_);
    if (editFont_)
        DeleteObject(editFont_);
    editFont_ = CreateFontW(-int(std::lround(13.f * dpi_ / 96.f)), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, L"Segoe UI");
    if (urlEdit_)
        SendMessageW(urlEdit_, WM_SETFONT, WPARAM(editFont_), TRUE);
    if (suggested)
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
    else {
        const DWORD style = DWORD(GetWindowLongPtrW(hwnd_, GWL_STYLE));
        RECT rc{0, 0, LONG(kWidth * dpi_ / 96), LONG(kHeight * dpi_ / 96)};
        AdjustWindowRectExForDpi(&rc, style, FALSE, 0, dpi);
        SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
D2D1_POINT_2F MainWindow::toDip(POINT p) const {
    return D2D1::Point2F(p.x * 96.f / dpi_, p.y * 96.f / dpi_);
}
RECT MainWindow::toPixels(const D2D1_RECT_F &r) const {
    return {LONG(std::lround(r.left * dpi_ / 96)), LONG(std::lround(r.top * dpi_ / 96)),
            LONG(std::lround(r.right * dpi_ / 96)), LONG(std::lround(r.bottom * dpi_ / 96))};
}
LRESULT CALLBACK MainWindow::windowProc(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
    if (message == WM_NCCREATE) {
        auto *cs = reinterpret_cast<CREATESTRUCTW *>(l);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return DefWindowProcW(hwnd, message, w, l);
    }
    auto *self = reinterpret_cast<MainWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self || !self->controller_) {
        if (message == WM_ERASEBKGND)
            return 1;
        return DefWindowProcW(hwnd, message, w, l);
    }
    return self->handle(message, w, l);
}
LRESULT MainWindow::handle(UINT message, WPARAM w, LPARAM l) {
    if (message == taskbarCreated_ && taskbarCreated_) {
        tray_.add(hwnd_, WM_APP_TRAY, icons_[5], L"Laptop Monitor");
        lastIcon_ = -1;
        updateTray();
        return 0;
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_SIZE:
        renderer_.resize(LOWORD(l), HIWORD(l));
        return 0;
    case WM_DPICHANGED:
        applyDpi(HIWORD(w), reinterpret_cast<RECT *>(l));
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking_) {
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&tme);
            tracking_ = true;
        }
        auto id = hitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (id != hover_) {
            hover_ = id;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking_ = false;
        if (hover_ != Id::None) {
            hover_ = Id::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        pressed_ = hitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        if (pressed_ != Id::None) {
            focus_ = pressed_;
            SetCapture(hwnd_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONUP: {
        auto id = hitTest({GET_X_LPARAM(l), GET_Y_LPARAM(l)});
        ReleaseCapture();
        auto pressed = pressed_;
        pressed_ = Id::None;
        if (pressed != Id::None && id == pressed)
            activate(id);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT && hover_ != Id::None) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        if (w == VK_TAB) {
            moveFocus(GetKeyState(VK_SHIFT) & 0x8000 ? -1 : 1);
            return 0;
        }
        if (w == VK_RETURN || w == VK_SPACE) {
            if (focus_ != Id::None)
                activate(focus_);
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (controller_->settings().minimizeToTray)
                hideWindow();
            return 0;
        }
        if (w == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            // Ctrl+C is the code alone; Ctrl+Shift+C is the receiver link that carries it.
            (GetKeyState(VK_SHIFT) & 0x8000) ? copyLink() : copyCode();
            return 0;
        }
        if (w == VK_LEFT || w == VK_RIGHT) {
            int index = page_ == Page::Overview ? 0 : page_ == Page::Details ? 1 : 2;
            index = (index + (w == VK_RIGHT ? 1 : 2)) % 3;
            page_ = index == 0 ? Page::Overview : index == 1 ? Page::Details : Page::Settings;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_TIMER:
        if (w == TIMER_UI)
            InvalidateRect(hwnd_, nullptr, FALSE);
        else if (w == TIMER_METRICS) {
            metrics_ = controller_->metrics();
            if (IsWindowVisible(hwnd_) && page_ != Page::Settings)
                InvalidateRect(hwnd_, nullptr, FALSE);
            // On this timer rather than in paint(), so the cost of sitting in the tray is reported too.
            reportUiCost();
        } else if (w == TIMER_FIND)
            controller_->pollDisplay();
        else if (w == TIMER_TOAST) {
            KillTimer(hwnd_, TIMER_TOAST);
            toast_.clear();
            toastTarget_ = Id::None;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (w == TIMER_EXIT) {
            KillTimer(hwnd_, TIMER_EXIT);
            if (!destroying_) {
                logWarning("Exit is taking too long; closing anyway");
                destroying_ = true;
                DestroyWindow(hwnd_);
            }
        }
        return 0;
    case WM_DISPLAYCHANGE:
        controller_->displayChanged();
        return 0;
    case WM_APP_EVENT:
        controller_->drain();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_APP_TRAY:
        onTray(w, l);
        return 0;
    case WM_APP_ACTIVATE:
        showWindow();
        return 0;
    case WM_APP_SETUP_DONE:
        controller_->setupProcessFinished(int(w));
        if (controller_->quitRequested() && !destroying_) {
            destroying_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == IDC_URL && HIWORD(w) == EN_KILLFOCUS)
            applyUrlFromEdit();
        return 0;
    case WM_CLOSE:
        if (controller_->settings().minimizeToTray && !controller_->model().exiting)
            hideWindow();
        else
            controller_->exitApp();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (w)
            controller_->exitApp();
        return 0;
    case WM_DESTROY:
        destroying_ = true;
        KillTimer(hwnd_, TIMER_UI);
        KillTimer(hwnd_, TIMER_METRICS);
        KillTimer(hwnd_, TIMER_FIND);
        KillTimer(hwnd_, TIMER_EXIT);
        tray_.remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, message, w, l);
}
void MainWindow::onStateChanged() {
    if (controller_->quitRequested()) {
        // This runs inside controller_->drain(): the window is torn down once, and the remaining events of that
        // drain must not paint, re-arm timers or try to destroy it again.
        if (!destroying_) {
            destroying_ = true;
            DestroyWindow(hwnd_);
        }
        return;
    }
    if (controller_->model().exiting && !exitTimerArmed_) {
        exitTimerArmed_ = true;
        SetTimer(hwnd_, TIMER_EXIT, 12000, nullptr);
    }
    updateTray();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void MainWindow::showWindow() {
    ShowWindow(hwnd_, SW_SHOW);
    if (IsIconic(hwnd_))
        ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
}
void MainWindow::hideWindow() {
    ShowWindow(hwnd_, SW_HIDE);
}
// ---- Tray ------------------------------------------------------------------------------------------------------
void MainWindow::updateTray() {
    const auto &m = controller_->model();
    int icon;
    switch (m.phase) {
    case Phase::Connected:
        icon = 2;
        break;
    case Phase::Ready:
        icon = 1;
        break;
    case Phase::Error:
        icon = 4;
        break;
    case Phase::Idle:
    case Phase::DisplayOnly:
    case Phase::SetupRequired:
        icon = 5;
        break;
    default:
        icon = 3;
        break;
    }
    std::wstring tip = L"Laptop Monitor · " + widen(phaseText(m.phase));
    if (icon != lastIcon_ || tip != lastTip_) {
        lastIcon_ = icon;
        lastTip_ = tip;
        tray_.update(icons_[icon], tip.c_str());
    }
}
void MainWindow::onTray(WPARAM, LPARAM l) {
    switch (LOWORD(l)) {
    case WM_LBUTTONUP:
    case NIN_SELECT:
    case NIN_KEYSELECT:
        if (IsWindowVisible(hwnd_) && GetForegroundWindow() == hwnd_)
            hideWindow();
        else
            showWindow();
        break;
    case WM_CONTEXTMENU:
    case WM_RBUTTONUP:
        trayMenu();
        break;
    }
}
void MainWindow::trayMenu() {
    const auto &m = controller_->model();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, L"Open Laptop Monitor");
    SetMenuDefaultItem(menu, IDM_TRAY_OPEN, FALSE);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto code = controller_->pairingCode();
    AppendMenuW(menu, MF_STRING | (code.empty() ? MF_GRAYED : 0), IDM_TRAY_COPY, L"Copy pairing code");
    AppendMenuW(menu, MF_STRING | (code.empty() ? MF_GRAYED : 0), IDM_TRAY_COPY_LINK, L"Copy receiver link");
    AppendMenuW(menu, MF_STRING | (m.viewer == ViewerStatus::None ? MF_GRAYED : 0), IDM_TRAY_DISCONNECT,
                L"Disconnect receiver");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool streaming = m.stream != StreamStatus::Stopped;
    const bool displayUp = m.display == DisplayStatus::Active || m.display == DisplayStatus::External;
    AppendMenuW(menu, MF_STRING | (m.exiting || (!streaming && !displayUp) ? MF_GRAYED : 0), IDM_TRAY_STREAM,
                streaming ? L"Stop streaming" : L"Start streaming");
    AppendMenuW(menu, MF_STRING | (m.exiting ? MF_GRAYED : 0), IDM_TRAY_MONITOR,
                m.display == DisplayStatus::Stopped || m.display == DisplayStatus::Error ? L"Start monitor"
                                                                                          : L"Stop monitor");
    AppendMenuW(menu, MF_STRING | (m.exiting ? MF_GRAYED : 0), IDM_TRAY_RESTART, L"Restart");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, L"Exit Laptop Monitor");
    POINT point;
    GetCursorPos(&point);
    SetForegroundWindow(hwnd_);
    UINT command = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, point.x, point.y,
                                         hwnd_, nullptr));
    DestroyMenu(menu);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    switch (command) {
    case IDM_TRAY_OPEN:
        showWindow();
        break;
    case IDM_TRAY_COPY:
        copyCode();
        break;
    case IDM_TRAY_COPY_LINK:
        copyLink();
        break;
    case IDM_TRAY_DISCONNECT:
        controller_->disconnectViewer();
        break;
    case IDM_TRAY_STREAM:
        streaming ? controller_->stopStreaming() : controller_->startStreaming();
        break;
    case IDM_TRAY_MONITOR:
        (m.display == DisplayStatus::Stopped || m.display == DisplayStatus::Error) ? controller_->startMonitor()
                                                                                    : controller_->stopMonitor();
        break;
    case IDM_TRAY_RESTART:
        controller_->restart();
        break;
    case IDM_TRAY_EXIT:
        controller_->exitApp();
        break;
    }
}
// ---- Actions ---------------------------------------------------------------------------------------------------
void MainWindow::copyToClipboard(const std::wstring &text, const wchar_t *toast, Id target) {
    if (OpenClipboard(hwnd_)) {
        EmptyClipboard();
        auto bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void *p = GlobalLock(memory)) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(memory);
                SetClipboardData(CF_UNICODETEXT, memory);
            }
        }
        CloseClipboard();
    }
    toast_ = toast;
    toastTarget_ = target;
    SetTimer(hwnd_, TIMER_TOAST, 1500, nullptr);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
std::wstring MainWindow::labelFor(Id id, const wchar_t *label) const {
    return toast_.empty() || toastTarget_ != id ? std::wstring(label) : toast_;
}
std::string MainWindow::viewerLink() const {
    std::string url = kViewerUrl;
    if (auto code = controller_->pairingCode(); !code.empty())
        url += "#code=" + code;
    return url;
}
void MainWindow::copyCode() {
    auto code = controller_->pairingCode();
    if (code.empty())
        return;
    copyToClipboard(widen(code), L"Copied", Id::CopyCode);
}
void MainWindow::copyLink() {
    // Paste it on the other laptop (cloud clipboard, a chat, anything) and the receiver connects on open: the
    // code rides in the fragment, which never reaches a server.
    if (controller_->pairingCode().empty())
        return;
    copyToClipboard(widen(viewerLink()), L"Link copied", Id::CopyLink);
}
void MainWindow::copyDiagnostics() {
    const auto &m = controller_->model();
    const auto &s = metrics_;
    std::string text = "Laptop Monitor " LM_VERSION "\n";
    text += std::string("State: ") + phaseText(m.phase) + " | display " + displayText(m.display) + " | stream " +
            streamText(m.stream) + " | receiver " + viewerText(m.viewer) + " | signaling " +
            signalingText(m.signaling) + "\n";
    if (!m.error.empty())
        text += "Error: " + m.error + "\n";
    text += "Setup: " + controller_->setupStatus().summary() + "\n";
    text += "Pipeline: " + s.backend + " -> GPU NV12 -> " + s.encoder + " | " + s.gpu + " | " + std::to_string(s.width) +
            "x" + std::to_string(s.height) + "@" + std::to_string(s.fps) + " | video path " + s.videoPath +
            " | CPU readback: " + (s.cpuReadback ? "yes" : "no") + "\n";
    text += "Capture fps " + std::to_string(s.captureFps) + " | encode fps " + std::to_string(s.encodeFps) +
            " | dropped " + std::to_string(s.dropped) + " | bitrate " + std::to_string(s.bitrate) + "\n";
    text += "Dropped: " + std::to_string(s.droppedCoalesced) + " coalesced, " +
            std::to_string(s.droppedSuperseded) + " superseded, " + std::to_string(s.droppedRingBusy) +
            " ring busy, " + std::to_string(s.droppedSubmitFailed) + " refused | paced " +
            std::to_string(s.paced) + " | encoder rebuilds " + std::to_string(s.encoderRebuilds) + "\n";
    text += "Engine thread: " + std::to_string(int(s.loopWakeupsPerSecond)) + " wake-ups/s, busy " +
            std::to_string(int(s.loopBusyPercent)) + "%, longest " + std::to_string(int(s.loopMaxMs)) + " ms\n";
    text += "Process: RAM " + std::to_string(s.workingSetMb) + " MB | GPU memory " +
            std::to_string(s.gpuMemoryMb) + " MB | handles " + std::to_string(s.handles) + " | " +
            (s.onBattery ? "on battery" : "on AC") + (s.batterySaver ? ", battery saver on" : "") + "\n";
    text += "Signaling " + s.signalingState + " | WebRTC " + s.webrtcState + "\n";
    text += "Logs: " + Log::instance().path().string() + "\n";
    text += "Recent log (codes and credentials are never logged):\n";
    for (auto &line : controller_->recentLog())
        text += line + "\n";
    copyToClipboard(widen(text), L"Diagnostics copied", Id::CopyDiagnostics);
}
void MainWindow::applyUrlFromEdit() {
    if (!urlEdit_)
        return;
    wchar_t buffer[600];
    GetWindowTextW(urlEdit_, buffer, int(std::size(buffer)));
    auto settings = controller_->settings();
    std::string url = narrow(buffer);
    if (url == settings.signalingUrl)
        return;
    settings.signalingUrl = url;
    controller_->updateSettings(settings);
    syncUrlEdit();
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void MainWindow::syncUrlEdit() {
    if (urlEdit_)
        SetWindowTextW(urlEdit_, widen(controller_ ? controller_->settings().signalingUrl : kDefaultSignalingUrl).c_str());
}
void MainWindow::chooseSetting(Id id, const D2D1_RECT_F &anchor) {
    auto settings = controller_->settings();
    HMENU menu = CreatePopupMenu();
    auto item = [&](UINT command, const wchar_t *label, bool checked) {
        AppendMenuW(menu, MF_STRING | (checked ? MF_CHECKED : 0), command, label);
    };
    if (id == Id::SetBackend) {
        item(1, L"Auto (prefer Windows Graphics Capture)", settings.backend == CaptureBackend::Auto);
        item(2, L"Windows Graphics Capture", settings.backend == CaptureBackend::Wgc);
        item(3, L"DXGI desktop duplication", settings.backend == CaptureBackend::Dxgi);
    } else if (id == Id::SetFps) {
        item(1, L"60 fps", settings.fps == 60);
        item(2, L"30 fps", settings.fps == 30);
    } else if (id == Id::SetScale) {
        item(1, L"Recommended (from the monitor's size)", settings.displayScale == DisplayScale::Recommended);
        item(2, L"100%", settings.displayScale == DisplayScale::Percent100);
        item(3, L"125%", settings.displayScale == DisplayScale::Percent125);
        item(4, L"150% (like a 13\" laptop)", settings.displayScale == DisplayScale::Percent150);
        item(5, L"175%", settings.displayScale == DisplayScale::Percent175);
    } else {
        item(1, L"Efficient (5 Mbps start, 10 Mbps max)", settings.quality == QualityPreset::Efficient);
        item(2, L"Balanced (8 Mbps start, 16 Mbps max)", settings.quality == QualityPreset::Balanced);
        item(3, L"Quality (12 Mbps start, 20 Mbps max)", settings.quality == QualityPreset::Quality);
    }
    auto px = toPixels(anchor);
    POINT origin{px.left, px.bottom};
    ClientToScreen(hwnd_, &origin);
    UINT command = UINT(TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, origin.x, origin.y, hwnd_,
                                         nullptr));
    DestroyMenu(menu);
    if (!command)
        return;
    if (id == Id::SetBackend)
        settings.backend = command == 1 ? CaptureBackend::Auto : command == 2 ? CaptureBackend::Wgc : CaptureBackend::Dxgi;
    else if (id == Id::SetFps)
        settings.fps = command == 1 ? 60 : 30;
    else if (id == Id::SetScale)
        settings.displayScale = command == 1   ? DisplayScale::Recommended
                                : command == 2 ? DisplayScale::Percent100
                                : command == 3 ? DisplayScale::Percent125
                                : command == 4 ? DisplayScale::Percent150
                                               : DisplayScale::Percent175;
    else
        settings.quality = command == 1   ? QualityPreset::Efficient
                           : command == 2 ? QualityPreset::Balanced
                                          : QualityPreset::Quality;
    controller_->updateSettings(settings);
    InvalidateRect(hwnd_, nullptr, FALSE);
}
void MainWindow::activate(Id id) {
    const auto &m = controller_->model();
    auto settings = controller_->settings();
    D2D1_RECT_F anchor{};
    for (auto &c : controls_)
        if (c.id == id)
            anchor = c.rect;
    switch (id) {
    case Id::TabOverview:
        page_ = Page::Overview;
        break;
    case Id::TabDetails:
        page_ = Page::Details;
        break;
    case Id::TabSettings:
        page_ = Page::Settings;
        break;
    case Id::CopyCode:
        copyCode();
        break;
    case Id::CopyLink:
        copyLink();
        break;
    case Id::DisconnectViewer:
        controller_->disconnectViewer();
        break;
    case Id::ToggleStream:
        m.stream == StreamStatus::Stopped ? controller_->startStreaming() : controller_->stopStreaming();
        break;
    case Id::ToggleMonitor:
        (m.display == DisplayStatus::Stopped || m.display == DisplayStatus::Error) ? controller_->startMonitor()
                                                                                    : controller_->stopMonitor();
        break;
    case Id::Restart:
        controller_->restart();
        break;
    case Id::Retry:
        controller_->startMonitor();
        break;
    case Id::Exit:
        controller_->exitApp();
        break;
    case Id::Setup:
        controller_->runSetup(hwnd_);
        break;
    case Id::Uninstall:
        if (MessageBoxW(hwnd_,
                        L"This removes the virtual display driver, the display helper, the scheduled task, "
                        L"start-at-sign-in and all Laptop Monitor data on this PC. Laptop Monitor exits afterwards.\n\n"
                        L"Windows will ask for administrator approval.",
                        L"Uninstall Laptop Monitor", MB_ICONWARNING | MB_OKCANCEL | MB_DEFBUTTON2) == IDOK)
            controller_->runUninstall(hwnd_);
        break;
    case Id::OpenLogs: {
        std::error_code ec;
        std::filesystem::create_directories(logDirectory(), ec);
        ShellExecuteW(hwnd_, L"open", logDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case Id::CopyDiagnostics:
        copyDiagnostics();
        break;
    case Id::OpenViewer:
        ShellExecuteW(hwnd_, L"open", widen(viewerLink()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case Id::SetStartAtSignIn:
        settings.startAtSignIn = !settings.startAtSignIn;
        controller_->updateSettings(settings);
        break;
    case Id::SetAutoStart:
        settings.autoStartDisplay = !settings.autoStartDisplay;
        controller_->updateSettings(settings);
        break;
    case Id::SetMinimize:
        settings.minimizeToTray = !settings.minimizeToTray;
        controller_->updateSettings(settings);
        break;
    case Id::SetLog:
        settings.diagnosticsLog = !settings.diagnosticsLog;
        controller_->updateSettings(settings);
        break;
    case Id::SetBackend:
    case Id::SetFps:
    case Id::SetQuality:
    case Id::SetScale:
        chooseSetting(id, anchor);
        break;
    case Id::ResetUrl:
        settings.signalingUrl = kDefaultSignalingUrl;
        controller_->updateSettings(settings);
        syncUrlEdit();
        break;
    case Id::None:
        break;
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
MainWindow::Id MainWindow::hitTest(POINT client) {
    auto p = toDip(client);
    for (auto it = controls_.rbegin(); it != controls_.rend(); ++it)
        if (it->enabled && contains(it->rect, p))
            return it->id;
    return Id::None;
}
void MainWindow::moveFocus(int direction) {
    std::vector<Id> order;
    for (auto &c : controls_)
        if (c.enabled)
            order.push_back(c.id);
    if (order.empty())
        return;
    int index = -1;
    for (size_t i = 0; i < order.size(); ++i)
        if (order[i] == focus_)
            index = int(i);
    index = index < 0 ? (direction > 0 ? 0 : int(order.size()) - 1)
                      : (index + direction + int(order.size())) % int(order.size());
    focus_ = order[size_t(index)];
    InvalidateRect(hwnd_, nullptr, FALSE);
}
// ---- Drawing ---------------------------------------------------------------------------------------------------
MainWindow::Control &MainWindow::add(Id id, D2D1_RECT_F r, std::wstring label, Kind kind, bool enabled) {
    controls_.push_back({id, r, std::move(label), kind, enabled});
    return controls_.back();
}
void MainWindow::reportUiCost() {
    const auto now = Clock::now();
    if (now - paintReport_ < std::chrono::minutes(2))
        return;
    const auto seconds = std::chrono::duration<double>(now - paintReport_).count();
    paintReport_ = now;
    const auto usage = uiResources_.sample();
    auto twoDecimals = [](double v) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << v;
        return out.str();
    };
    // Logged even with no paints at all: that line is the record of what the app costs while it sits in the tray,
    // which is most of its life and the part nobody ever measures.
    logInfo("App process: " + std::to_string(paints_) + " window paints in " + std::to_string(int(seconds)) +
            " s (" + std::to_string(paintsHidden_) + " while hidden), mean " + twoDecimals(paintTimes_.mean()) +
            " ms, longest " + twoDecimals(paintMaxMs_) + " ms | " + summarize(usage));
    paints_ = paintsHidden_ = 0;
    paintMaxMs_ = 0;
}
void MainWindow::paint() {
    const auto paintStart = Clock::now();
    ++paints_;
    if (!IsWindowVisible(hwnd_))
        ++paintsHidden_;
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    if (renderer_.ensure(hwnd_)) {
        renderer_.setDpi(dpi_);
        controls_.clear();
        if (urlEdit_ && page_ != Page::Settings)
            ShowWindow(urlEdit_, SW_HIDE);
        renderer_.begin();
        drawHeader();
        switch (page_) {
        case Page::Overview:
            drawOverview();
            break;
        case Page::Details:
            drawDetails();
            break;
        case Page::Settings:
            drawSettings();
            break;
        }
        drawControls();
        if (!renderer_.end())
            InvalidateRect(hwnd_, nullptr, FALSE);
    }
    EndPaint(hwnd_, &ps);
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - paintStart).count();
    paintTimes_.add(ms);
    paintMaxMs_ = std::max(paintMaxMs_, ms);
}
void MainWindow::drawHeader() {
    const auto &t = renderer_.theme();
    const auto &m = controller_->model();
    renderer_.text(L"Laptop Monitor", rect(kMargin, 14, 240, 30), Font::Title, t.text);
    // State pill.
    D2D1_COLOR_F dot = t.neutral;
    switch (m.phase) {
    case Phase::Connected:
        dot = t.info;
        break;
    case Phase::Ready:
        dot = t.success;
        break;
    case Phase::Error:
        dot = t.danger;
        break;
    case Phase::Idle:
    case Phase::DisplayOnly:
    case Phase::SetupRequired:
        dot = t.neutral;
        break;
    default:
        dot = t.warning;
        break;
    }
    std::wstring phase = widen(phaseText(m.phase));
    if (m.phase == Phase::Connected && metrics_.connectedSince) {
        auto since = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                      *metrics_.connectedSince);
        phase += L" · " + duration(since);
    }
    auto size = renderer_.measure(phase, Font::SmallStrong);
    float w = size.width + 30;
    auto pill = rect(kRight - w, 19, w, 22);
    renderer_.fill(pill, t.card, 11);
    renderer_.outline(pill, t.cardBorder, 11);
    renderer_.dot(pill.left + 11, 30, 3.5f, dot);
    renderer_.text(phase, rect(pill.left + 20, 19, w - 26, 22), Font::SmallStrong, t.text);
    // Tabs.
    float x = kMargin;
    const struct {
        Id id;
        Page page;
        const wchar_t *label;
    } tabs[] = {{Id::TabOverview, Page::Overview, L"Overview"},
                {Id::TabDetails, Page::Details, L"Details"},
                {Id::TabSettings, Page::Settings, L"Settings"}};
    for (auto &tab : tabs) {
        auto tw = renderer_.measure(tab.label, Font::BodyStrong).width + 4;
        auto &c = add(tab.id, rect(x - 2, 54, tw + 4, 32), tab.label, Kind::Tab);
        c.on = page_ == tab.page;
        x += tw + 24;
    }
    renderer_.line(0, 86, 440, 86, t.divider);
}
void MainWindow::drawControls() {
    const auto &t = renderer_.theme();
    for (auto &c : controls_) {
        const bool hover = hover_ == c.id && c.enabled, pressed = pressed_ == c.id && c.enabled;
        const float a = c.enabled ? 1.f : 0.45f;
        switch (c.kind) {
        case Kind::Primary: {
            auto bg = pressed ? t.accentPressed : hover ? t.accentHover : t.accent;
            renderer_.fill(c.rect, alpha(bg, a), 6);
            renderer_.text(c.label, c.rect, Font::Button, alpha(t.accentText, a), Align::Center);
            break;
        }
        case Kind::Button:
        case Kind::Danger: {
            auto bg = pressed ? t.buttonPressed : hover ? t.buttonHover : t.buttonBg;
            renderer_.fill(c.rect, alpha(bg, a), 6);
            renderer_.outline(c.rect, alpha(t.buttonBorder, a), 6);
            renderer_.text(c.label, c.rect, Font::Button, alpha(c.kind == Kind::Danger ? t.danger : t.text, a),
                           Align::Center);
            break;
        }
        case Kind::Ghost: {
            if (hover || pressed)
                renderer_.fill(c.rect, alpha(t.buttonHover, a), 6);
            renderer_.text(c.label, c.rect, Font::Button, alpha(hover ? t.danger : t.textMuted, a), Align::Center);
            break;
        }
        case Kind::Tab: {
            renderer_.text(c.label, c.rect, Font::BodyStrong, c.on ? t.text : hover ? t.textMuted : t.textFaint,
                           Align::Center);
            if (c.on)
                renderer_.fill(D2D1::RectF(c.rect.left + 2, c.rect.bottom - 2, c.rect.right - 2, c.rect.bottom),
                               t.accent, 1);
            break;
        }
        case Kind::Toggle: {
            // Label on the left, switch at the right edge of the row.
            renderer_.text(c.label, D2D1::RectF(c.rect.left, c.rect.top, c.rect.right - 60, c.rect.bottom), Font::Body,
                           alpha(t.text, a));
            auto sw = rect(c.rect.right - 40, (c.rect.top + c.rect.bottom) / 2 - 10, 40, 20);
            renderer_.fill(sw, alpha(c.on ? (hover ? t.accentHover : t.accent) : (hover ? t.soft : t.toggleOff), a),
                           10);
            float knob = c.on ? sw.right - 10 : sw.left + 10;
            renderer_.dot(knob, (sw.top + sw.bottom) / 2, 7, alpha(t.card, a));
            break;
        }
        case Kind::Choice: {
            renderer_.text(c.label, D2D1::RectF(c.rect.left, c.rect.top, c.rect.right - 170, c.rect.bottom),
                           Font::Body, alpha(t.text, a));
            auto box = rect(c.rect.right - 164, (c.rect.top + c.rect.bottom) / 2 - 15, 164, 30);
            renderer_.fill(box, alpha(hover ? t.buttonHover : t.buttonBg, a), 6);
            renderer_.outline(box, alpha(t.buttonBorder, a), 6);
            renderer_.text(c.value, D2D1::RectF(box.left + 10, box.top, box.right - 26, box.bottom), Font::Body,
                           alpha(t.text, a));
            // Chevron.
            float cx = box.right - 14, cy = (box.top + box.bottom) / 2 - 1;
            renderer_.line(cx - 4, cy, cx, cy + 4, alpha(t.textMuted, a), 1.5f);
            renderer_.line(cx, cy + 4, cx + 4, cy, alpha(t.textMuted, a), 1.5f);
            break;
        }
        case Kind::Link: {
            float w = renderer_.text(c.label, c.rect, Font::Small, alpha(hover ? t.accent : t.soft, a), Align::Right);
            renderer_.line(c.rect.right - w, c.rect.bottom - 3, c.rect.right, c.rect.bottom - 3,
                           alpha(hover ? t.accent : t.soft, 0.6f));
            break;
        }
        }
        if (focus_ == c.id && c.enabled && GetFocus() == hwnd_) {
            auto ring = D2D1::RectF(c.rect.left - 3, c.rect.top - 3, c.rect.right + 3, c.rect.bottom + 3);
            renderer_.outline(ring, t.focus, 8, 1.5f);
        }
    }
}
std::wstring MainWindow::viewerUrlText() const {
    std::string url = kViewerUrl;
    if (url.starts_with("https://"))
        url.erase(0, 8);
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return widen(url);
}
void MainWindow::drawOverview() {
    const auto &t = renderer_.theme();
    const auto &m = controller_->model();
    const auto &s = metrics_;
    // Pairing card.
    auto card = rect(kMargin, 100, kInner, 158);
    const std::string code = controller_->pairingCode();
    renderer_.fill(card, code.empty() ? t.card : t.tintCard, 10);
    renderer_.outline(card, code.empty() ? t.cardBorder : t.tintBorder, 10);
    if (!code.empty()) {
        renderer_.text(L"PAIRING CODE", rect(card.left + 20, card.top + 14, 200, 16), Font::Caption, t.textMuted,
                       Align::Left, true, 1.2f);
        renderer_.text(widen(code), rect(card.left + 18, card.top + 32, 250, 54), Font::Code, t.accent, Align::Left,
                       true, 7.f);
        // Two ways to hand the code over: the code itself, or a receiver link that carries it and connects on open.
        add(Id::CopyCode, rect(card.right - 104, card.top + 33, 84, 30), labelFor(Id::CopyCode, L"Copy code"),
            Kind::Button);
        add(Id::CopyLink, rect(card.right - 104, card.top + 67, 84, 30), labelFor(Id::CopyLink, L"Copy link"),
            Kind::Button);
        const int left = controller_->pairingSecondsLeft();
        auto track = rect(card.left + 20, card.top + 104, kInner - 40, 4);
        renderer_.fill(track, t.track, 2);
        float fraction = std::clamp(left / 120.f, 0.f, 1.f);
        if (fraction > 0)
            renderer_.fill(D2D1::RectF(track.left, track.top, track.left + (track.right - track.left) * fraction,
                                       track.bottom),
                           t.soft, 2);
        renderer_.text(L"New code in " + duration(std::chrono::seconds(left)),
                       rect(card.left + 20, card.top + 116, 180, 20), Font::Small, t.textMuted);
        add(Id::OpenViewer, rect(card.right - 220, card.top + 116, 200, 20), viewerUrlText(), Kind::Link);
    } else {
        std::wstring title, detail;
        Id action = Id::None;
        std::wstring actionLabel;
        Kind actionKind = Kind::Primary;
        switch (m.phase) {
        case Phase::SetupRequired:
            title = L"One-time setup needed";
            detail = controller_->setupRunning()
                         ? L"Waiting for administrator approval…"
                         : L"Approve administrator access once to install the virtual display helper.";
            action = Id::Setup;
            actionLabel = L"Set up…";
            break;
        case Phase::Idle:
            title = L"Virtual display is off";
            detail = L"Start the monitor to show a pairing code.";
            action = Id::ToggleMonitor;
            actionLabel = L"Start monitor";
            break;
        case Phase::DisplayOnly:
            title = L"Streaming stopped";
            detail = m.detail.empty() ? L"LaptopMon stays available as a desktop extension." : widen(m.detail);
            action = Id::ToggleStream;
            actionLabel = L"Start streaming";
            break;
        case Phase::Error:
            title = L"Something needs attention";
            detail = widen(m.error.empty() ? std::string(m.detail) : m.error);
            action = Id::Retry;
            actionLabel = L"Try again";
            actionKind = Kind::Button;
            break;
        case Phase::Exiting:
            title = L"Exiting…";
            detail = L"Stopping the stream and removing the virtual display.";
            break;
        default:
            title = widen(phaseText(m.phase)) + L"…";
            detail = widen(!m.detail.empty() && m.detail != "reconnect" ? m.detail
                           : m.phase == Phase::StartingDisplay      ? "Starting the elevated display helper."
                           : m.phase == Phase::FindingDisplay       ? "Waiting for LaptopMon to join the desktop."
                           : m.phase == Phase::StartingEncoder      ? "Preparing GPU capture and the hardware encoder."
                           : m.phase == Phase::ConnectingSignaling  ? "Reaching the signaling service."
                           : m.phase == Phase::Reconnecting         ? "Restoring the connection."
                                                                    : "Please wait.");
            break;
        }
        renderer_.text(title, rect(card.left + 20, card.top + 22, kInner - 40, 24), Font::BodyStrong, t.text);
        renderer_.text(detail, rect(card.left + 20, card.top + 48, kInner - 40, 40), Font::Small, t.textMuted,
                       Align::Left, false);
        if (action != Id::None) {
            bool enabled = !controller_->setupRunning() && !m.exiting;
            add(action, rect(card.left + 20, card.top + 104, 150, 34), actionLabel, actionKind, enabled);
        }
        if (m.phase == Phase::SetupRequired && !controller_->lastNotification().empty())
            renderer_.text(widen(controller_->lastNotification()), rect(card.left + 180, card.top + 104, 200, 34),
                           Font::Small, t.danger);
    }
    // Status rows.
    struct Row {
        const wchar_t *label;
        std::wstring value;
        D2D1_COLOR_F color;
    };
    auto statusColor = [&](bool good, bool busy, bool bad) {
        return bad ? t.danger : good ? t.success : busy ? t.warning : t.neutral;
    };
    std::wstring displayValue = widen(displayText(m.display));
    if (m.display == DisplayStatus::Active || m.display == DisplayStatus::External)
        displayValue = m.display == DisplayStatus::External ? L"LaptopMon · started elsewhere" : L"LaptopMon · 1920×1080 @ 60";
    std::wstring streamValue = widen(streamText(m.stream));
    if (m.stream == StreamStatus::Streaming && s.width)
        streamValue = std::to_wstring(s.width) + L"×" + std::to_wstring(s.height) + L" · " + std::to_wstring(s.fps) +
                      L" fps · " + widen(s.backend);
    std::wstring viewerValue = widen(viewerText(m.viewer));
    if (m.viewer == ViewerStatus::Connected && s.connectedSince)
        viewerValue = L"Connected · " + duration(std::chrono::duration_cast<std::chrono::seconds>(
                                            std::chrono::steady_clock::now() - *s.connectedSince));
    Row rows[] = {
        {L"Virtual display", displayValue,
         statusColor(m.display == DisplayStatus::Active || m.display == DisplayStatus::External,
                     m.display == DisplayStatus::Starting || m.display == DisplayStatus::Missing ||
                         m.display == DisplayStatus::Stopping,
                     m.display == DisplayStatus::Error)},
        {L"Streaming", streamValue,
         statusColor(m.stream == StreamStatus::Streaming, m.stream == StreamStatus::Starting || m.stream == StreamStatus::Stopping,
                     m.stream == StreamStatus::Error)},
        {L"Receiver", viewerValue,
         statusColor(m.viewer == ViewerStatus::Connected, m.viewer == ViewerStatus::Connecting, false)},
        {L"Signaling", widen(signalingText(m.signaling)),
         statusColor(m.signaling == SignalingStatus::Connected, m.signaling == SignalingStatus::Connecting,
                     m.signaling == SignalingStatus::Rejected)},
    };
    float y = 274;
    for (auto &row : rows) {
        renderer_.dot(kMargin + 5, y + 12, 4, row.color);
        renderer_.text(row.label, rect(kMargin + 18, y, 120, 24), Font::BodyStrong, t.text);
        renderer_.text(row.value, rect(kMargin + 140, y, kInner - 140, 24), Font::Body, t.textMuted, Align::Right);
        y += 26;
    }
    // One line of context under the rows: an error, or the most recent hint.
    std::wstring context;
    D2D1_COLOR_F contextColor = t.textMuted;
    if (!m.error.empty() && !code.empty()) {
        context = widen(m.error);
        contextColor = t.danger;
    } else if (!m.detail.empty() && m.detail != "reconnect" && m.phase != Phase::Error)
        context = widen(m.detail);
    else if (!controller_->lastNotification().empty() && m.phase != Phase::SetupRequired)
        context = widen(controller_->lastNotification());
    if (!context.empty())
        renderer_.text(context, rect(kMargin, y + 2, kInner, 18), Font::Small, contextColor);
    // Metrics grid.
    struct Cell {
        const wchar_t *label;
        std::wstring value;
    };
    const bool live = m.stream == StreamStatus::Streaming;
    std::wstring dropped = live ? std::to_wstring(s.dropped) : L"—";
    if (live && s.viewerDropped && *s.viewerDropped)
        dropped += L" / " + std::to_wstring(*s.viewerDropped);
    Cell cells[] = {
        {L"FPS", live ? fixed(s.encodeFps, 1) : L"—"},
        {L"BITRATE", live ? (s.viewerBitrate ? mbps(s.viewerBitrate) : mbps(double(s.bitrate))) : L"—"},
        {L"RTT", live && s.rttMs ? fixed(*s.rttMs, 0) + L" ms" : L"—"},
        {L"DROPPED", dropped},
        {L"RESOLUTION", live && s.width ? std::to_wstring(s.width) + L"×" + std::to_wstring(s.height) : L"—"},
        {L"ENCODER", live ? shortEncoder(s.encoder) : L"—"},
    };
    float gy = 406;
    renderer_.line(kMargin, gy - 10, kRight, gy - 10, t.divider);
    for (int i = 0; i < 6; ++i) {
        float cx = kMargin + (i % 3) * (kInner / 3), cy = gy + (i / 3) * 56;
        renderer_.text(cells[i].label, rect(cx, cy, kInner / 3 - 8, 14), Font::Caption, t.textFaint, Align::Left,
                       true, 0.8f);
        renderer_.text(cells[i].value, rect(cx, cy + 16, kInner / 3 - 8, 30), Font::Metric, t.text);
    }
    // Actions.
    const bool exiting = m.exiting;
    const bool displayUp = m.display == DisplayStatus::Active || m.display == DisplayStatus::External;
    const float bw = (kInner - 8) / 2, by = 530;
    add(Id::DisconnectViewer, rect(kMargin, by, bw, 34), L"Disconnect receiver", Kind::Button,
        !exiting && m.stream == StreamStatus::Streaming && m.viewer != ViewerStatus::None);
    add(Id::ToggleStream, rect(kMargin + bw + 8, by, bw, 34),
        m.stream == StreamStatus::Stopped ? L"Start streaming" : L"Stop streaming", Kind::Button,
        !exiting && (m.stream == StreamStatus::Streaming || (m.stream == StreamStatus::Stopped && displayUp)));
    const bool monitorOff = m.display == DisplayStatus::Stopped || m.display == DisplayStatus::Error;
    add(Id::ToggleMonitor, rect(kMargin, by + 42, bw, 34), monitorOff ? L"Start monitor" : L"Stop monitor",
        Kind::Button, !exiting && (monitorOff ? m.setupReady : m.display != DisplayStatus::Stopping));
    add(Id::Restart, rect(kMargin + bw + 8, by + 42, bw, 34), L"Restart", Kind::Button, !exiting);
    add(Id::Exit, rect(kMargin + 96, by + 86, kInner - 192, 26), L"Exit Laptop Monitor", Kind::Ghost, !exiting);
}
void MainWindow::drawDetails() {
    const auto &t = renderer_.theme();
    const auto &m = controller_->model();
    const auto &s = metrics_;
    const bool live = m.stream == StreamStatus::Streaming;
    auto pct = [](const std::optional<double> &v) { return v ? fixed(*v * 100, 2) + L"%" : L"—"; };
    auto latency = [&](const std::optional<double> &mean, const std::optional<double> &p95) {
        return mean ? fixed(*mean, 1) + L" / " + fixed(p95, 1) + L" ms" : L"—";
    };
    std::wstring connected = L"—";
    if (s.connectedSince)
        connected = duration(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                                              *s.connectedSince));
    std::wstring code = controller_->pairingCode().empty()
                            ? L"—"
                            : L"renews in " + duration(std::chrono::seconds(controller_->pairingSecondsLeft()));
    struct Row {
        std::wstring label, value;
        bool heading = false;
    };
    std::vector<Row> rows = {
        {L"VIDEO", L"", true},
        {L"Capture / encode fps", live ? fixed(s.captureFps, 1) + L" / " + fixed(s.encodeFps, 1) : L"—"},
        {L"Receiver fps", live ? fixed(s.viewerFps, 1) : L"—"},
        {L"Encoder bitrate (target)", live ? mbps(double(s.bitrate)) + L" (" + mbps(double(s.targetBitrate)) + L")" : L"—"},
        {L"Receiver bitrate", live ? mbps(s.viewerBitrate) : L"—"},
        {L"Frames dropped (host / receiver)",
         live ? std::to_wstring(s.dropped) + L" / " + (s.viewerDropped ? std::to_wstring(*s.viewerDropped) : L"—") : L"—"},
        {L"Capture latency (mean / p95)", live ? fixed(s.captureMsMean, 2) + L" / " + fixed(s.captureMsP95, 2) + L" ms" : L"—"},
        {L"Encode latency (mean / p95)", live ? latency(s.encodeMsMean, s.encodeMsP95) : L"—"},
        {L"Frame to encoded (mean / p95)", live ? latency(s.pipelineMsMean, s.pipelineMsP95) : L"—"},
        {L"Frame wait before capture (mean / p95)", live ? latency(s.acquireDelayMsMean, s.acquireDelayMsP95) : L"—"},
        {L"Receiver jitter buffer / decode", live ? fixed(s.viewerJitterBufferMs, 1, L" ms") + L" / " + fixed(s.viewerDecodeMs, 1, L" ms") : L"—"},
        {L"Encoder queue / keyframes", live ? std::to_wstring(s.queueDepth) + L" / " + std::to_wstring(s.keyframesSent) : L"—"},
        {L"NETWORK", L"", true},
        {L"RTT / jitter", live ? fixed(s.rttMs, 0, L" ms") + L" / " + fixed(s.jitterMs, 1, L" ms") : L"—"},
        {L"Packet loss", live ? pct(s.loss) : L"—"},
        {L"Signaling / WebRTC", widen(s.signalingState) + L" / " + widen(s.webrtcState)},
        {L"Connected for", connected},
        {L"Sent", live ? std::to_wstring(s.sentFrames) + L" frames · " + fixed(s.sentBytes / 1048576.0, 1) + L" MB" : L"—"},
        {L"Pairing code", code},
        {L"PIPELINE", L"", true},
        {L"GPU", live ? widen(s.gpu) : L"—"},
        {L"Encoder", live ? widen(s.encoder) : L"—"},
        {L"Capture backend", live ? widen(s.backend) : widen(backendName(controller_->settings().backend))},
        {L"Video path", live ? widen(s.videoPath) + (s.cpuReadback ? L" · CPU readback" : L" · no CPU readback") : L"—"},
        {L"Host CPU", live ? fixed(s.cpuPercent, 1, L"%") : L"—"},
        {L"Setup", widen(controller_->setupStatus().summary())},
    };
    float y = 98;
    for (auto &row : rows) {
        if (row.heading) {
            y += 6;
            renderer_.text(row.label, rect(kMargin, y, 200, 14), Font::Caption, t.textFaint, Align::Left, true, 0.8f);
            y += 18;
            continue;
        }
        renderer_.text(row.label, rect(kMargin, y, 200, 18), Font::Small, t.textMuted);
        renderer_.text(row.value, rect(kMargin + 190, y, kInner - 190, 18), Font::SmallStrong, t.text, Align::Right);
        y += 19;
    }
    renderer_.line(kMargin, 604, kRight, 604, t.divider);
    const float bw = (kInner - 8) / 2;
    add(Id::CopyDiagnostics, rect(kMargin, 616, bw, 32), labelFor(Id::CopyDiagnostics, L"Copy diagnostics"),
        Kind::Button);
    add(Id::OpenLogs, rect(kMargin + bw + 8, 616, bw, 32), L"Open log folder", Kind::Button);
}
void MainWindow::drawSettings() {
    const auto &t = renderer_.theme();
    const auto &settings = controller_->settings();
    const auto &setup = controller_->setupStatus();
    float y = 98;
    auto caption = [&](const wchar_t *text) {
        renderer_.text(text, rect(kMargin, y, 200, 14), Font::Caption, t.textFaint, Align::Left, true, 0.8f);
        y += 20;
    };
    auto toggle = [&](Id id, const wchar_t *label, bool on) {
        auto &c = add(id, rect(kMargin, y, kInner, 32), label, Kind::Toggle);
        c.on = on;
        y += 36;
    };
    caption(L"GENERAL");
    toggle(Id::SetStartAtSignIn, L"Start Laptop Monitor at sign-in", settings.startAtSignIn);
    toggle(Id::SetAutoStart, L"Start the virtual display automatically", settings.autoStartDisplay);
    toggle(Id::SetMinimize, L"Keep running in the tray when the window closes", settings.minimizeToTray);
    toggle(Id::SetLog, L"Write a diagnostics log", settings.diagnosticsLog);
    y += 6;
    caption(L"DISPLAY AND STREAMING");
    auto choice = [&](Id id, const wchar_t *label, std::wstring value) {
        auto &c = add(id, rect(kMargin, y, kInner, 34), label, Kind::Choice);
        c.value = std::move(value);
        y += 40;
    };
    // Scaling applies to the virtual display alone, which is what the label has to make obvious.
    choice(Id::SetScale, L"Windows scaling (this display only)",
           settings.displayScale == DisplayScale::Recommended ? L"Recommended"
                                                              : widen(scaleName(settings.displayScale)) + L"%");
    choice(Id::SetBackend, L"Capture backend",
           settings.backend == CaptureBackend::Auto ? L"Auto" : settings.backend == CaptureBackend::Wgc ? L"WGC" : L"DXGI");
    choice(Id::SetFps, L"Frame rate", std::to_wstring(settings.fps) + L" fps");
    choice(Id::SetQuality, L"Quality",
           settings.quality == QualityPreset::Efficient ? L"Efficient" : settings.quality == QualityPreset::Quality ? L"Quality" : L"Balanced");
    y += 2;
    caption(L"SIGNALING");
    renderer_.text(L"Signaling URL", rect(kMargin, y, 200, 18), Font::Small, t.textMuted);
    y += 20;
    auto editRect = rect(kMargin, y, kInner - 78, 28);
    renderer_.fill(editRect, t.card, 4);
    renderer_.outline(editRect, t.buttonBorder, 4);
    if (urlEdit_) {
        auto px = toPixels(D2D1::RectF(editRect.left + 8, editRect.top + 6, editRect.right - 8, editRect.bottom - 5));
        SetWindowPos(urlEdit_, nullptr, px.left, px.top, px.right - px.left, px.bottom - px.top, SWP_NOZORDER | SWP_SHOWWINDOW);
    }
    add(Id::ResetUrl, rect(kRight - 70, y, 70, 28), L"Reset", Kind::Button, settings.signalingUrl != kDefaultSignalingUrl);
    y += 34;
    renderer_.text(L"Coordinates the connection only; video travels directly over WebRTC.", rect(kMargin, y, kInner, 16),
                   Font::Small, t.textFaint);
    y += 26;
    caption(L"SETUP");
    std::wstring status = setup.ready() && setup.helperCurrent ? L"Virtual display helper installed and ready."
                          : setup.ready()                    ? L"Installed. The helper next to this build is newer: repair to update it."
                                                             : L"Not set up: " + widen(setup.summary()) + L".";
    renderer_.text(status, rect(kMargin, y, kInner, 18), Font::Small, setup.ready() ? t.textMuted : t.warning);
    y += 24;
    const bool busy = controller_->setupRunning();
    add(Id::Setup, rect(kMargin, y, 150, 32), setup.ready() ? L"Repair setup…" : L"Set up…", Kind::Primary, !busy);
    add(Id::Uninstall, rect(kMargin + 158, y, 150, 32), L"Uninstall…", Kind::Danger, !busy);
    y += 40;
    std::wstring footer = L"Laptop Monitor " LM_VERSION L" · log: " + Log::instance().path().wstring();
    if (Log::instance().path().empty())
        footer = L"Laptop Monitor " LM_VERSION L" · diagnostics log off";
    renderer_.text(footer, rect(kMargin, kHeight - 30, kInner, 16), Font::Small, t.textFaint);
}
} // namespace lm::app::ui
