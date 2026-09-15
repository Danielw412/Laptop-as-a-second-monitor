#include "platform.hpp"
#include <d3d10.h>
namespace bm {
std::vector<Display> displays() {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    std::vector<Display> list;
    for (UINT a = 0;; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 ad{};
        check(adapter->GetDesc1(&ad), "Adapter description");
        for (UINT o = 0;; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC od{};
            check(output->GetDesc(&od), "Output description");
            if (!od.AttachedToDesktop)
                continue;
            MONITORINFO mi{sizeof(mi)};
            GetMonitorInfoW(od.Monitor, &mi);
            Display d{utf8(od.DeviceName),
                      utf8(ad.Description),
                      ad.AdapterLuid,
                      od.Monitor,
                      od.DesktopCoordinates,
                      (mi.dwFlags & MONITORINFOF_PRIMARY) != 0,
                      od.Rotation,
                      adapter,
                      {}};
            check(output.As(&d.output), "Output1");
            list.push_back(d);
        }
    }
    return list;
}
Device::Device(const Display &d) {
    D3D_FEATURE_LEVEL level;
    check(D3D11CreateDevice(d.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &device, &level, &context),
          "D3D11 on selected display adapter");
    ComPtr<ID3D10Multithread> multithread;
    check(context.As(&multithread), "D3D multithread protection");
    multithread->SetMultithreadProtected(TRUE);
}
class Duplication final : public ICapture {
    ComPtr<IDXGIOutputDuplication> duplication_;
    bool held_ = false;

  public:
    Duplication(Device &device, const Display &d) {
        if (d.rotation != DXGI_MODE_ROTATION_IDENTITY)
            throw std::runtime_error("Rotated display: use landscape orientation for DXGI capture");
        check(d.output->DuplicateOutput(device.device.Get(), &duplication_), "DuplicateOutput");
    }
    ~Duplication() {
        release();
    }
    std::optional<Frame> acquire() override {
        release();
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource> resource;
        HRESULT hr = duplication_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return {};
        check(hr, "AcquireNextFrame (display changed/access lost; rebuilding capture)");
        held_ = true;
        if (!info.LastPresentTime.QuadPart) {
            release();
            return {};
        }
        Frame frame;
        check(resource.As(&frame.texture), "Capture texture");
        frame.timestamp = now100ns();
        frame.accumulated = info.AccumulatedFrames;
        return frame;
    }
    void release() override {
        if (held_) {
            duplication_->ReleaseFrame();
            held_ = false;
        }
    }
};
std::unique_ptr<ICapture> duplication(Device &d, const Display &s) {
    return std::make_unique<Duplication>(d, s);
}
} // namespace bm
