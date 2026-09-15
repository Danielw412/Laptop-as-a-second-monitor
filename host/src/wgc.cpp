#include "platform.hpp"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
namespace bm {
using namespace winrt::Windows::Graphics;
namespace {
// C++/WinRT reports failures as winrt::hresult_error, which is not a std::exception. Everything leaving this file
// is translated so the pipeline's error handling (and its retry loop) sees ordinary runtime errors.
[[noreturn]] void rethrow(const winrt::hresult_error &e, const char *what) {
    std::ostringstream s;
    s << what << " (0x" << std::hex << uint32_t(e.code()) << ")";
    throw std::runtime_error(s.str());
}
} // namespace
class Wgc final : public ICapture {
    Capture::GraphicsCaptureItem item_{nullptr};
    Capture::Direct3D11CaptureFramePool pool_{nullptr};
    Capture::GraphicsCaptureSession session_{nullptr};
    Capture::Direct3D11CaptureFrame held_{nullptr};
    SizeInt32 size_{};
    void create(Device &device, const Display &display) {
        if (!Capture::GraphicsCaptureSession::IsSupported())
            throw std::runtime_error("Windows Graphics Capture is unavailable");
        auto factory =
            winrt::get_activation_factory<Capture::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        check(factory->CreateForMonitor(display.monitor, winrt::guid_of<Capture::GraphicsCaptureItem>(),
                                        winrt::put_abi(item_)),
              "WGC selected monitor");
        size_ = item_.Size();
        ComPtr<IDXGIDevice> dxgi;
        check(device.device.As(&dxgi), "WGC DXGI device");
        winrt::com_ptr<IInspectable> inspectable;
        check(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()), "WGC D3D interop");
        auto d3d = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
        pool_ = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            d3d, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size_);
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(true);
        session_.StartCapture();
    }

  public:
    Wgc(Device &device, const Display &display) {
        try {
            create(device, display);
        } catch (const winrt::hresult_error &e) {
            rethrow(e, "Windows Graphics Capture setup failed");
        }
    }
    // Destructors must not throw; Close() can fail once the display or the capture item is already gone.
    ~Wgc() {
        try {
            release();
        } catch (...) {
        }
        try {
            if (session_)
                session_.Close();
        } catch (...) {
        }
        try {
            if (pool_)
                pool_.Close();
        } catch (...) {
        }
    }
    std::optional<Frame> acquire() override {
        try {
            release();
            held_ = pool_.TryGetNextFrame();
            if (!held_)
                return {};
            // Retain only the newest available frame.
            while (auto newer = pool_.TryGetNextFrame()) {
                held_.Close();
                held_ = newer;
            }
            auto size = held_.ContentSize();
            if (size.Width != size_.Width || size.Height != size_.Height)
                throw std::runtime_error("WGC display size changed; recreate pipeline");
            auto access =
                held_.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            Frame result;
            check(access->GetInterface(IID_PPV_ARGS(&result.texture)), "WGC GPU texture");
            result.timestamp = now100ns();
            return result;
        } catch (const winrt::hresult_error &e) {
            rethrow(e, "Windows Graphics Capture frame failed (display changed; rebuilding capture)");
        }
    }
    void release() override {
        if (held_) {
            held_.Close();
            held_ = nullptr;
        }
    }
};
std::unique_ptr<ICapture> wgc(Device &d, const Display &s) {
    return std::make_unique<Wgc>(d, s);
}
} // namespace bm
