#include "synthetic.hpp"
#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
namespace lm {
SyntheticContent parseSyntheticContent(const std::string &s) {
    if (s == "bar")
        return SyntheticContent::Bar;
    if (s == "scroll")
        return SyntheticContent::Scroll;
    if (s == "desktop")
        return SyntheticContent::Desktop;
    throw std::runtime_error("Synthetic content must be bar, scroll or desktop");
}
const char *syntheticContentName(SyntheticContent c) {
    switch (c) {
    case SyntheticContent::Scroll:
        return "scroll";
    case SyntheticContent::Desktop:
        return "desktop";
    default:
        return "bar";
    }
}
namespace {
// Deterministic pseudo-random lines that look like a busy terminal: timestamps, a coloured level tag, words and
// numbers of varying length. The same seed gives the same atlas on every machine.
struct Lcg {
    uint32_t state;
    uint32_t next() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    }
    uint32_t below(uint32_t n) {
        return next() % n;
    }
};
const wchar_t *const kWords[] = {L"pipeline", L"encoder",  L"frame",   L"capture", L"session", L"receiver",
                                 L"bitrate",  L"keyframe", L"surface", L"latency", L"queue",   L"signaling",
                                 L"rtp",      L"nack",     L"texture", L"nv12",    L"display", L"compositor",
                                 L"packet",   L"decode",   L"stream",  L"engine",  L"wake",    L"interval"};
const wchar_t *const kLevels[] = {L"[info ]", L"[debug]", L"[warn ]", L"[error]"};
} // namespace
struct SyntheticSource::Impl {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext1> context1;
    unsigned width = 0, height = 0, block = 0, speed = 4;
    ComPtr<ID3D11Texture2D> atlas; // Two copies of the text block stacked, so any window of `height` rows is contiguous
    ID3D11Texture2D *lastTarget = nullptr;
    ComPtr<ID3D11RenderTargetView> targetView;
    void buildAtlas() {
        block = height * 2;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = width;
        d.Height = block * 2;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&d, nullptr, &atlas), "Synthetic text atlas");
        ComPtr<ID2D1Factory1> factory;
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                reinterpret_cast<void **>(factory.GetAddressOf())),
              "Direct2D factory");
        ComPtr<IDXGISurface> surface;
        check(atlas.As(&surface), "Synthetic atlas surface");
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96, 96);
        ComPtr<ID2D1RenderTarget> target;
        check(factory->CreateDxgiSurfaceRenderTarget(surface.Get(), &props, &target), "Synthetic text target");
        ComPtr<IDWriteFactory> write;
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown **>(write.GetAddressOf())),
              "DirectWrite factory");
        ComPtr<IDWriteTextFormat> format;
        const float size = 20.0f; // Consolas 20 px: a 150%-scaled terminal on a 1080p display
        check(write->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format),
              "Synthetic text format");
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        ComPtr<IDWriteTextLayout> probe;
        check(write->CreateTextLayout(L"M", 1, format.Get(), 100, 100, &probe), "Synthetic glyph metrics");
        DWRITE_TEXT_METRICS metrics{};
        probe->GetMetrics(&metrics);
        const float advance = metrics.widthIncludingTrailingWhitespace, lineHeight = 26.0f;
        ComPtr<ID2D1SolidColorBrush> text, dim, tags[4];
        target->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.86f, 0.84f), &text);
        target->CreateSolidColorBrush(D2D1::ColorF(0.52f, 0.52f, 0.50f), &dim);
        const D2D1_COLOR_F tagColors[4] = {D2D1::ColorF(0.35f, 0.80f, 0.45f), D2D1::ColorF(0.40f, 0.65f, 0.95f),
                                           D2D1::ColorF(0.95f, 0.80f, 0.30f), D2D1::ColorF(0.95f, 0.35f, 0.30f)};
        for (int i = 0; i < 4; ++i)
            target->CreateSolidColorBrush(tagColors[i], &tags[i]);
        Lcg random{20260922u};
        target->BeginDraw();
        target->Clear(D2D1::ColorF(0.047f, 0.047f, 0.047f));
        const unsigned columns = unsigned(width / advance);
        for (float y = 4; y + lineHeight <= float(block); y += lineHeight) {
            auto draw = [&](const std::wstring &s, float x, ID2D1Brush *brush) {
                target->DrawText(s.c_str(), UINT32(s.size()), format.Get(),
                                 D2D1::RectF(x, y, float(width), y + lineHeight), brush);
                return x + advance * float(s.size());
            };
            wchar_t stamp[40];
            swprintf_s(stamp, L"2026-09-22 15:%02u:%02u.%03u ", random.below(60), random.below(60), random.below(1000));
            float x = draw(stamp, 8, dim.Get());
            const auto level = random.below(10) < 7 ? 0 : 1 + random.below(3);
            x = draw(std::wstring(kLevels[level]) + L" ", x, tags[level].Get());
            std::wstring message;
            const unsigned words = 3 + random.below(14);
            for (unsigned w = 0; w < words; ++w) {
                if (random.below(3) == 0)
                    message += std::to_wstring(random.below(100000)) + (random.below(2) ? L" ms " : L" ");
                else
                    message += std::wstring(kWords[random.below(std::size(kWords))]) + L" ";
            }
            if (message.size() + 32 > columns)
                message.resize(columns > 32 ? columns - 32 : 0);
            draw(message, x, text.Get());
        }
        check(target->EndDraw(), "Synthetic text draw");
        // Second copy directly below the first, so the scroll window never has to wrap.
        D3D11_BOX box{0, 0, 0, width, block, 1};
        context->CopySubresourceRegion(atlas.Get(), 0, 0, block, 0, atlas.Get(), 0, &box);
        context->Flush();
    }
};
SyntheticSource::SyntheticSource(ID3D11Device *device, ID3D11DeviceContext *context, unsigned width, unsigned height,
                                 SyntheticContent content, unsigned scrollPixelsPerFrame)
    : impl_(new Impl), content_(content) {
    try {
        impl_->device = device;
        impl_->context = context;
        impl_->width = width;
        impl_->height = height;
        impl_->speed = std::max(1u, scrollPixelsPerFrame);
        check(impl_->context.As(&impl_->context1), "D3D11.1 synthetic source");
        if (content != SyntheticContent::Bar)
            impl_->buildAtlas();
    } catch (...) {
        delete impl_;
        throw;
    }
}
SyntheticSource::~SyntheticSource() {
    delete impl_;
}
unsigned SyntheticSource::scrollOffset(uint64_t n) const {
    if (!impl_->block)
        return 0;
    // The desktop jumps a third of the atlas every 180 frames: a window switch, every pixel new.
    const uint64_t jumps = content_ == SyntheticContent::Desktop ? n / 180 : 0;
    return unsigned((n * impl_->speed + jumps * (impl_->block / 3)) % impl_->block);
}
void SyntheticSource::render(ID3D11Texture2D *target, uint64_t n) {
    auto &i = *impl_;
    if (content_ != SyntheticContent::Bar) {
        const unsigned y = scrollOffset(n);
        D3D11_BOX box{0, y, 0, i.width, y + i.height, 1};
        i.context->CopySubresourceRegion(target, 0, 0, 0, 0, i.atlas.Get(), 0, &box);
        if (content_ == SyntheticContent::Desktop && i.width >= 1280 && i.height >= 720) {
            // The "video": a 640x360 window whose content is replaced by an unrelated part of the atlas at 30 fps.
            const unsigned w = 640, h = 360;
            uint32_t state = uint32_t(n / 2) * 2654435761u + 12345u;
            const unsigned sx = (state >> 7) % (i.width - w), sy = (state >> 3) % (i.block * 2 - h);
            D3D11_BOX patch{sx, sy, 0, sx + w, sy + h, 1};
            i.context->CopySubresourceRegion(target, 0, i.width - w - 40, i.height - h - 40, 0, i.atlas.Get(), 0,
                                             &patch);
        }
        return;
    }
    if (target != i.lastTarget) {
        i.targetView.Reset();
        check(i.device->CreateRenderTargetView(target, nullptr, &i.targetView), "Synthetic render target");
        i.lastTarget = target;
    }
    float background[4]{.04f, .08f, .12f, 1}, foreground[4]{.3f, .85f, .6f, 1};
    i.context->ClearRenderTargetView(i.targetView.Get(), background);
    const LONG x = LONG((n * 12) % (i.width - 100));
    D3D11_RECT rect{x, 0, x + 100, LONG(i.height)};
    i.context1->ClearView(i.targetView.Get(), foreground, &rect, 1);
}
} // namespace lm
