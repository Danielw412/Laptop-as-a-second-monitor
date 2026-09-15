#pragma once
// Direct2D/DirectWrite drawing for the main window: theme, fonts and a handful of primitives. Everything is in
// device-independent pixels; the render target's DPI does the scaling.
#include <d2d1.h>
#include <dwrite_1.h>
#include <map>
#include <string>
#include <string_view>
#include <windows.h>
#include <wrl/client.h>
namespace bm::app::ui {
using Microsoft::WRL::ComPtr;
inline D2D1_COLOR_F rgb(uint32_t hex, float alpha = 1.f) {
    return D2D1::ColorF(((hex >> 16) & 255) / 255.f, ((hex >> 8) & 255) / 255.f, (hex & 255) / 255.f, alpha);
}
/// Warm neutral system: #FFDBBB #CCBEB1 #997E67 #664930 as accents over a light off-white base.
struct Theme {
    D2D1_COLOR_F paper = rgb(0xFBF7F3);
    D2D1_COLOR_F card = rgb(0xFFFFFF);
    D2D1_COLOR_F cardBorder = rgb(0xE9E0D7);
    D2D1_COLOR_F divider = rgb(0xEFE7DF);
    D2D1_COLOR_F text = rgb(0x2B2119);
    D2D1_COLOR_F textMuted = rgb(0x7A6A5D);
    D2D1_COLOR_F textFaint = rgb(0xA69B91);
    D2D1_COLOR_F accent = rgb(0x664930);
    D2D1_COLOR_F accentHover = rgb(0x7A5A3E);
    D2D1_COLOR_F accentPressed = rgb(0x52391F);
    D2D1_COLOR_F accentText = rgb(0xFFF6EC);
    D2D1_COLOR_F soft = rgb(0x997E67);
    D2D1_COLOR_F tint = rgb(0xFFDBBB);
    D2D1_COLOR_F tintCard = rgb(0xFFF3E7);
    D2D1_COLOR_F tintBorder = rgb(0xF2DDC7);
    D2D1_COLOR_F track = rgb(0xEFE6DD);
    D2D1_COLOR_F buttonBg = rgb(0xFFFFFF);
    D2D1_COLOR_F buttonHover = rgb(0xF6EFE8);
    D2D1_COLOR_F buttonPressed = rgb(0xEDE3D9);
    D2D1_COLOR_F buttonBorder = rgb(0xD9CFC5);
    D2D1_COLOR_F toggleOff = rgb(0xCCBEB1);
    D2D1_COLOR_F focus = rgb(0x997E67);
    D2D1_COLOR_F success = rgb(0x3F9E6B);
    D2D1_COLOR_F warning = rgb(0xD98B2B);
    D2D1_COLOR_F danger = rgb(0xC94B3E);
    D2D1_COLOR_F info = rgb(0x2F7AD6);
    D2D1_COLOR_F neutral = rgb(0xA0988F);
};
enum class Font { Title, Subtitle, Body, BodyStrong, Small, SmallStrong, Caption, Code, Metric, Button, Mono };
enum class Align { Left, Center, Right };
class Renderer {
  public:
    bool ensure(HWND);
    void discard();
    void resize(UINT width, UINT height);
    void setDpi(float dpi);
    bool begin();
    /// Returns false when the target was lost and must be recreated (the caller repaints).
    bool end();
    const Theme &theme() const {
        return theme_;
    }
    void fill(const D2D1_RECT_F &, D2D1_COLOR_F, float radius = 0);
    void outline(const D2D1_RECT_F &, D2D1_COLOR_F, float radius = 0, float width = 1);
    void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F, float width = 1);
    void dot(float cx, float cy, float radius, D2D1_COLOR_F);
    /// Draws text clipped to the rectangle. Returns the laid-out width.
    float text(std::wstring_view, const D2D1_RECT_F &, Font, D2D1_COLOR_F, Align = Align::Left,
               bool verticalCenter = true, float characterSpacing = 0);
    D2D1_SIZE_F measure(std::wstring_view, Font, float maxWidth = 4096);
    float dpi() const {
        return dpi_;
    }

  private:
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory1> dwrite_;
    ComPtr<ID2D1HwndRenderTarget> target_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    std::map<Font, ComPtr<IDWriteTextFormat>> formats_;
    Theme theme_;
    HWND hwnd_ = nullptr;
    float dpi_ = 96.f;
    std::wstring textFamily_, displayFamily_, monoFamily_;
    IDWriteTextFormat *format(Font);
    ID2D1SolidColorBrush *brush(D2D1_COLOR_F);
    void pickFamilies();
};
} // namespace bm::app::ui
