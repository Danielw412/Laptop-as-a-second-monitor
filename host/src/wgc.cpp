#include "platform.hpp"
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
namespace lm {
using namespace winrt::Windows::Graphics;
namespace {
// C++/WinRT reports failures as winrt::hresult_error, which is not a std::exception. Everything leaving this file
// is translated so the pipeline's error handling (and its retry loop) sees ordinary runtime errors.
[[noreturn]] void rethrow(const winrt::hresult_error &e, const char *what) {
    std::ostringstream s;
    s << what << " (0x" << std::hex << uint32_t(e.code()) << ")";
    throw std::runtime_error(s.str());
}
// Owns the auto-reset event the frame pool's thread signals. Shared with the FrameArrived handler so a callback
// that runs after teardown can never touch a closed handle.
struct FrameSignal {
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~FrameSignal() {
        if (event)
            CloseHandle(event);
    }
};
} // namespace
class Wgc final : public ICapture {
    Capture::GraphicsCaptureItem item_{nullptr};
    Capture::Direct3D11CaptureFramePool pool_{nullptr};
    Capture::GraphicsCaptureSession session_{nullptr};
    Capture::Direct3D11CaptureFrame held_{nullptr};
    winrt::event_token arrived_{};
    std::shared_ptr<FrameSignal> signal_ = std::make_shared<FrameSignal>();
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
        // The pool's own thread wakes the engine the moment the compositor hands over a frame, so the engine
        // never polls for frames on a timer and never sits on a finished frame until its next tick.
        arrived_ = pool_.FrameArrived([signal = signal_](auto &&, auto &&) { SetEvent(signal->event); });
        session_ = pool_.CreateCaptureSession(item_);
        session_.IsCursorCaptureEnabled(true);
        session_.StartCapture();
    }

  public:
    Wgc(Device &device, const Display &display) {
        if (!signal_->event)
            throw std::runtime_error("WGC frame event");
        try {
            create(device, display);
        } catch (const winrt::hresult_error &e) {
            rethrow(e, "Windows Graphics Capture setup failed");
        }
    }
    // Destructors must not throw; Close() can fail once the display or the capture item is already gone.
    ~Wgc() {
        try {
            if (pool_ && arrived_)
                pool_.FrameArrived(arrived_);
        } catch (...) {
        }
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
    HANDLE frameEvent() const override {
        return signal_->event;
    }
    std::optional<Frame> acquire(unsigned timeoutMs) override {
        try {
            auto next = pool_.TryGetNextFrame();
            if (!next && timeoutMs) {
                WaitForSingleObject(signal_->event, timeoutMs);
                next = pool_.TryGetNextFrame();
            }
            if (!next)
                return {}; // Nothing newer: a frame the caller still holds stays valid.
            release();
            held_ = next;
            uint32_t skipped = 0;
            // Retain only the newest available frame; anything older is stale by definition.
            while (auto newer = pool_.TryGetNextFrame()) {
                held_.Close();
                held_ = newer;
                ++skipped;
            }
            auto size = held_.ContentSize();
            if (size.Width != size_.Width || size.Height != size_.Height)
                throw std::runtime_error("WGC display size changed; recreate pipeline");
            auto access =
                held_.Surface().as<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            Frame result;
            check(access->GetInterface(IID_PPV_ARGS(&result.texture)), "WGC GPU texture");
            result.timestamp = now100ns();
            // SystemRelativeTime is QPC-based, like steady_clock: the compositor's own stamp for this frame.
            result.presented = held_.SystemRelativeTime().count();
            result.accumulated = 1 + skipped;
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
} // namespace lm
