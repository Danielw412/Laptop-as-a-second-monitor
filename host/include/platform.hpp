#pragma once
#include "core.hpp"
#include <array>
#include <chrono>
#include <codecapi.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <functional>
#include <iostream>
#include <memory>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <optional>
#include <sstream>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace lm {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
inline int64_t now100ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() /
           100;
}
inline void check(HRESULT hr, const char *op) {
    if (FAILED(hr)) {
        std::ostringstream s;
        s << op << " (0x" << std::hex << uint32_t(hr) << ")";
        throw std::runtime_error(s.str());
    }
}
inline std::string utf8(const wchar_t *text) {
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, s.data(), n, nullptr, nullptr);
    s.pop_back();
    return s;
}
struct Display {
    std::string name, gpu;
    LUID luid{};
    HMONITOR monitor{};
    RECT rect{};
    bool primary{};
    DXGI_MODE_ROTATION rotation{};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
};
std::vector<Display> displays();
struct Device {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    explicit Device(const Display &);
};
struct Frame {
    ComPtr<ID3D11Texture2D> texture;
    int64_t timestamp{};
    uint32_t accumulated = 1;
};
class ICapture {
  public:
    virtual ~ICapture() = default;
    virtual std::optional<Frame> acquire() = 0;
    virtual void release() = 0;
};
std::unique_ptr<ICapture> duplication(Device &, const Display &);
std::unique_ptr<ICapture> wgc(Device &, const Display &);
class Converter {
    Device &device_;
    ComPtr<ID3D11VideoDevice> video_;
    ComPtr<ID3D11VideoContext> context_;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    ComPtr<ID3D11VideoProcessor> processor_;
    unsigned width_, height_;
    std::array<ComPtr<ID3D11Texture2D>, 3> textures_;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, 3> views_;
    struct InputView {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11VideoProcessorInputView> view;
    };
    std::vector<InputView> inputViews_;
    struct Timing {
        ComPtr<ID3D11Query> disjoint, start, end;
        bool pending = false;
    };
    std::array<Timing, 8> timings_;
    size_t timingIndex_ = 0;
    Samples<> gpuTimes_;

  public:
    Converter(Device &, unsigned inputWidth, unsigned inputHeight, unsigned outputWidth,
              unsigned outputHeight);
    ID3D11Texture2D *convert(ID3D11Texture2D *, size_t slot);
    ID3D11Texture2D *texture(size_t slot) {
        return textures_.at(slot).Get();
    }
    const Samples<> &gpuTimes() const {
        return gpuTimes_;
    }
};
struct Encoded {
    std::vector<uint8_t> bytes;
    int64_t timestamp{};
    bool keyframe{};
    double latencyMs{};
};
class IEncoder {
  public:
    virtual ~IEncoder() = default;
    virtual bool ready() = 0;
    virtual bool submit(ID3D11Texture2D *, int64_t) = 0;
    virtual std::vector<Encoded> poll() = 0;
    virtual void keyframe() = 0;
    virtual bool bitrate(uint32_t) = 0;
    virtual const std::string &name() const = 0;
    virtual size_t pending() const = 0;
};
std::unique_ptr<IEncoder> hardwareEncoder(Device &, const Display &, unsigned width, unsigned height,
                                          unsigned fps, uint32_t bitrate = 8000000);
} // namespace lm
