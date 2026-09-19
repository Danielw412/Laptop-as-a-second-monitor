#pragma once
// The single application window: three compact pages (Overview, Details, Settings) drawn with Direct2D, plus the
// tray icon. Controls are laid out and drawn immediately each paint; hit testing uses the last layout.
#include "controller.hpp"
#include "resources.hpp"
#include "ui/renderer.hpp"
#include "ui/tray.hpp"
#include <memory>
#include <vector>
namespace lm::app::ui {
class MainWindow {
  public:
    MainWindow(HINSTANCE instance, bool startHidden, SettingsStore store, Settings settings, std::string hostSecret);
    ~MainWindow();
    HWND hwnd() const {
        return hwnd_;
    }
    int run();

  private:
    enum class Page { Overview, Details, Settings };
    enum class Id {
        None,
        TabOverview,
        TabDetails,
        TabSettings,
        CopyCode,
        CopyLink,
        DisconnectViewer,
        ToggleStream,
        ToggleMonitor,
        Restart,
        Exit,
        Setup,
        Uninstall,
        OpenLogs,
        CopyDiagnostics,
        OpenViewer,
        SetStartAtSignIn,
        SetAutoStart,
        SetMinimize,
        SetLog,
        SetBackend,
        SetFps,
        SetQuality,
        SetScale,
        ResetUrl,
        Retry,
    };
    enum class Kind { Button, Primary, Danger, Ghost, Tab, Toggle, Choice, Link };
    struct Control {
        Id id;
        D2D1_RECT_F rect;
        std::wstring label;
        Kind kind;
        bool enabled = true;
        bool on = false;
        std::wstring value;
    };
    static constexpr float kWidth = 440.f, kHeight = 664.f;
    HINSTANCE instance_;
    HWND hwnd_ = nullptr, urlEdit_ = nullptr;
    HFONT editFont_ = nullptr;
    std::unique_ptr<AppController> controller_;
    Renderer renderer_;
    TrayIcon tray_;
    Page page_ = Page::Overview;
    std::vector<Control> controls_;
    Id hover_ = Id::None, pressed_ = Id::None, focus_ = Id::None;
    bool startHidden_, exitTimerArmed_ = false, tracking_ = false, destroying_ = false;
    float dpi_ = 96.f;
    std::wstring toast_;
    Id toastTarget_ = Id::None; // Which button shows toast_ in place of its label
    UINT taskbarCreated_ = 0;
    HICON icons_[6]{};
    int lastIcon_ = -1;
    std::wstring lastTip_;
    MetricsSnapshot metrics_;
    // What the window itself costs. A tray application that repaints when nobody is looking is pure waste, so
    // paints are counted and timed and reported to the log now and then.
    Samples<256> paintTimes_;
    uint64_t paints_ = 0, paintsHidden_ = 0;
    double paintMaxMs_ = 0;
    Clock::time_point paintReport_ = Clock::now();
    ResourceMeter uiResources_;
    void reportUiCost();
    static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT, WPARAM, LPARAM);
    void createControls();
    void applyDpi(UINT dpi, const RECT *suggested);
    void paint();
    void drawHeader();
    void drawOverview();
    void drawDetails();
    void drawSettings();
    void drawControls();
    Control &add(Id, D2D1_RECT_F, std::wstring label, Kind, bool enabled = true);
    Id hitTest(POINT client);
    void activate(Id);
    void moveFocus(int direction);
    void showWindow();
    void hideWindow();
    void onTray(WPARAM, LPARAM);
    void trayMenu();
    void updateTray();
    void copyCode();
    void copyLink();
    void copyDiagnostics();
    void copyToClipboard(const std::wstring &text, const wchar_t *toast, Id target);
    /// The receiver page, with the current pairing code in the fragment when there is one, so it connects on open.
    std::string viewerLink() const;
    /// A control's label, or the toast while this control is the one showing it.
    std::wstring labelFor(Id id, const wchar_t *label) const;
    void chooseSetting(Id, const D2D1_RECT_F &anchor);
    /// "Open log folder": the rolling logs, this run's archive, or the folder of all archived runs.
    void chooseLogFolder(const D2D1_RECT_F &anchor);
    void applyUrlFromEdit();
    void syncUrlEdit();
    void onStateChanged();
    D2D1_POINT_2F toDip(POINT p) const;
    RECT toPixels(const D2D1_RECT_F &r) const;
    std::wstring viewerUrlText() const;
};
} // namespace lm::app::ui
