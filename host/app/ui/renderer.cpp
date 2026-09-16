#include "ui/renderer.hpp"
#include <algorithm>
namespace lm::app::ui {
namespace {
struct FontSpec {
    const wchar_t *familyKind; // "text", "display" or "mono"
    float size;
    DWRITE_FONT_WEIGHT weight;
};
FontSpec spec(Font f) {
    switch (f) {
    case Font::Title:
        return {L"display", 18.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Subtitle:
        return {L"text", 13.f, DWRITE_FONT_WEIGHT_NORMAL};
    case Font::Body:
        return {L"text", 13.f, DWRITE_FONT_WEIGHT_NORMAL};
    case Font::BodyStrong:
        return {L"text", 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Small:
        return {L"text", 12.f, DWRITE_FONT_WEIGHT_NORMAL};
    case Font::SmallStrong:
        return {L"text", 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Caption:
        return {L"text", 11.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Code:
        return {L"display", 40.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Metric:
        return {L"display", 21.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Button:
        return {L"text", 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD};
    case Font::Mono:
        return {L"mono", 11.f, DWRITE_FONT_WEIGHT_NORMAL};
    }
    return {L"text", 13.f, DWRITE_FONT_WEIGHT_NORMAL};
}
} // namespace
void Renderer::pickFamilies() {
    ComPtr<IDWriteFontCollection> collection;
    auto has = [&](const wchar_t *name) {
        if (!collection && FAILED(dwrite_->GetSystemFontCollection(&collection)))
            return false;
        UINT32 index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(collection->FindFamilyName(name, &index, &exists)) && exists;
    };
    textFamily_ = has(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    displayFamily_ = has(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    monoFamily_ = has(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
}
bool Renderer::ensure(HWND hwnd) {
    hwnd_ = hwnd;
    if (!factory_ && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf())))
        return false;
    if (!dwrite_) {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1),
                                       reinterpret_cast<IUnknown **>(dwrite_.GetAddressOf()))))
            return false;
        pickFamilies();
    }
    if (!target_) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        auto size = D2D1::SizeU(UINT32(std::max(1L, rc.right - rc.left)), UINT32(std::max(1L, rc.bottom - rc.top)));
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                                                  dpi_, dpi_);
        auto hwndProps = D2D1::HwndRenderTargetProperties(hwnd, size, D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(factory_->CreateHwndRenderTarget(props, hwndProps, &target_)))
            return false;
        target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
        target_->CreateSolidColorBrush(theme_.text, &brush_);
    }
    return true;
}
void Renderer::discard() {
    brush_.Reset();
    target_.Reset();
}
void Renderer::resize(UINT width, UINT height) {
    if (target_)
        target_->Resize(D2D1::SizeU(std::max(1u, width), std::max(1u, height)));
}
void Renderer::setDpi(float dpi) {
    dpi_ = dpi;
    if (target_)
        target_->SetDpi(dpi, dpi);
}
bool Renderer::begin() {
    if (!target_)
        return false;
    target_->BeginDraw();
    target_->Clear(theme_.paper);
    return true;
}
bool Renderer::end() {
    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard();
        return false;
    }
    return true;
}
ID2D1SolidColorBrush *Renderer::brush(D2D1_COLOR_F color) {
    brush_->SetColor(color);
    return brush_.Get();
}
IDWriteTextFormat *Renderer::format(Font f) {
    auto it = formats_.find(f);
    if (it != formats_.end())
        return it->second.Get();
    auto s = spec(f);
    const wchar_t *family = std::wstring_view(s.familyKind) == L"display" ? displayFamily_.c_str()
                            : std::wstring_view(s.familyKind) == L"mono" ? monoFamily_.c_str()
                                                                          : textFamily_.c_str();
    ComPtr<IDWriteTextFormat> fmt;
    if (FAILED(dwrite_->CreateTextFormat(family, nullptr, s.weight, DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, s.size, L"en-us", &fmt)))
        return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    ComPtr<IDWriteInlineObject> ellipsis;
    if (SUCCEEDED(dwrite_->CreateEllipsisTrimmingSign(fmt.Get(), &ellipsis))) {
        DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        fmt->SetTrimming(&trimming, ellipsis.Get());
    }
    formats_[f] = fmt;
    return fmt.Get();
}
void Renderer::fill(const D2D1_RECT_F &r, D2D1_COLOR_F color, float radius) {
    if (radius > 0)
        target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush(color));
    else
        target_->FillRectangle(r, brush(color));
}
void Renderer::outline(const D2D1_RECT_F &r, D2D1_COLOR_F color, float radius, float width) {
    auto inset = D2D1::RectF(r.left + width / 2, r.top + width / 2, r.right - width / 2, r.bottom - width / 2);
    if (radius > 0)
        target_->DrawRoundedRectangle(D2D1::RoundedRect(inset, radius, radius), brush(color), width);
    else
        target_->DrawRectangle(inset, brush(color), width);
}
void Renderer::line(float x0, float y0, float x1, float y1, D2D1_COLOR_F color, float width) {
    target_->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush(color), width);
}
void Renderer::dot(float cx, float cy, float radius, D2D1_COLOR_F color) {
    target_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), brush(color));
}
float Renderer::text(std::wstring_view s, const D2D1_RECT_F &r, Font f, D2D1_COLOR_F color, Align align,
                     bool verticalCenter, float spacing) {
    auto *fmt = format(f);
    if (!fmt || s.empty())
        return 0;
    ComPtr<IDWriteTextLayout> layout;
    const float width = std::max(1.f, r.right - r.left), height = std::max(1.f, r.bottom - r.top);
    if (FAILED(dwrite_->CreateTextLayout(s.data(), UINT32(s.size()), fmt, width, height, &layout)))
        return 0;
    layout->SetTextAlignment(align == Align::Left     ? DWRITE_TEXT_ALIGNMENT_LEADING
                             : align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                      : DWRITE_TEXT_ALIGNMENT_TRAILING);
    layout->SetParagraphAlignment(verticalCenter ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (spacing != 0) {
        ComPtr<IDWriteTextLayout1> layout1;
        if (SUCCEEDED(layout.As(&layout1)))
            layout1->SetCharacterSpacing(spacing / 2, spacing / 2, 0, {0, UINT32(s.size())});
    }
    target_->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout.Get(), brush(color),
                            D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return metrics.widthIncludingTrailingWhitespace;
}
D2D1_SIZE_F Renderer::measure(std::wstring_view s, Font f, float maxWidth) {
    auto *fmt = format(f);
    if (!fmt || s.empty())
        return D2D1::SizeF(0, 0);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite_->CreateTextLayout(s.data(), UINT32(s.size()), fmt, maxWidth, 4096, &layout)))
        return D2D1::SizeF(0, 0);
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    return D2D1::SizeF(metrics.widthIncludingTrailingWhitespace, metrics.height);
}
} // namespace lm::app::ui
