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
inline int64_t to100ns(Clock::time_point t) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count() / 100;
}
inline int64_t now100ns() {
    return to100ns(Clock::now());
}
/// The inverse of now100ns() for host stamps (Frame::timestamp). Never use it on a compositor stamp.
inline Clock::time_point from100ns(int64_t ticks) {
    return Clock::time_point(std::chrono::duration_cast<Clock::duration>(std::chrono::nanoseconds(ticks * 100)));
}
/// QueryPerformanceCounter ticks (what DXGI and WGC stamp frames with) in the same 100 ns units as now100ns().
/// MSVC's steady_clock is QPC-based, so the two are directly comparable.
inline int64_t qpcTo100ns(int64_t ticks) {
    static const int64_t frequency = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? f.QuadPart : 1;
    }();
    return int64_t((double(ticks) * 10000000.0) / double(frequency));
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
    int64_t timestamp{};  // When the pipeline took the frame (now100ns)
    int64_t presented{};  // When the compositor produced it (100 ns, same clock); 0 if unknown
    uint32_t accumulated = 1;
    uint64_t sequence = 0; // Assigned by the engine to each new source frame, in acquisition order
};
/// One source frame's milestones on the host's steady clock, carried through the encoder so each stage is measured
/// on the same frame and on one monotonic clock. The compositor's stamps (Frame::presented) never enter it.
struct FrameTrace {
    uint64_t sequence = 0;                           // 0: not a source frame (a static-desktop repeat)
    Clock::time_point acquired{};                    // The capture backend handed the frame to the engine
    std::optional<Clock::time_point> convertStarted; // The NV12 conversion was issued (absent in encode-only mode)
    Clock::time_point encodeSubmitted{};             // The encoder accepted the surface (set by the encoder)
};
class ICapture {
  public:
    virtual ~ICapture() = default;
    /// Returns the newest frame, waiting up to timeoutMs for one to arrive. It stays valid until release() or the
    /// next acquire(). Frames skipped to reach the newest one are counted in Frame::accumulated.
    virtual std::optional<Frame> acquire(unsigned timeoutMs) = 0;
    virtual void release() = 0;
    /// Signalled when a frame arrives, or null when the backend can only wait inside acquire() (DXGI).
    virtual HANDLE frameEvent() const = 0;
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
    // Output ring: one more surface than the encoder may hold, so a conversion never waits for the encoder.
    static constexpr size_t kSlots = 4;
    std::array<ComPtr<ID3D11Texture2D>, kSlots> textures_;
    std::array<ComPtr<ID3D11VideoProcessorOutputView>, kSlots> views_;
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
    static constexpr size_t slots() {
        return kSlots;
    }
    const Samples<> &gpuTimes() const {
        return gpuTimes_;
    }
};
struct Encoded {
    std::vector<uint8_t> bytes;
    int64_t timestamp{};
    int64_t presented{}; // Source presentation time carried through the encoder (0 if unknown)
    bool keyframe{};
    double latencyMs{};
    std::optional<int> qp; // Frame QP when the encoder reports one on its output (MFSampleExtension_VideoEncodeQP)
    // Host-clock bookkeeping. `matched` is false when the output's sample time did not identify a submission, in
    // which case latencyMs and trace are empty rather than guessed.
    bool matched = false;
    FrameTrace trace;
    std::optional<Clock::time_point> outputSignalled; // The MFT announced this output (its HaveOutput event)
    Clock::time_point retrieved{};                    // The engine thread took it from the MFT
};
class IEncoder {
  public:
    virtual ~IEncoder() = default;
    /// True when the encoder can take another frame right now.
    virtual bool ready() = 0;
    /// True while the encoder still reads this input surface; writing to it would corrupt a frame in flight.
    virtual bool holds(ID3D11Texture2D *) const = 0;
    /// trace travels with the frame and comes back on its Encoded output, with encodeSubmitted filled in.
    virtual bool submit(ID3D11Texture2D *, int64_t timestamp, int64_t presented = 0,
                        const FrameTrace &trace = {}) = 0;
    virtual std::vector<Encoded> poll() = 0;
    virtual void keyframe() = 0;
    virtual bool bitrate(uint32_t) = 0;
    virtual const std::string &name() const = 0;
    /// What the encoder reports for the settings that shape its output (rate control, bitrate, buffer, QP limits,
    /// GOP), read back after configuration, so a log shows what it actually runs with rather than what was asked.
    virtual std::string configuration() const = 0;
    virtual size_t pending() const = 0;
    /// Signalled whenever the encoder has news: output ready, input wanted, or an input surface released.
    virtual HANDLE event() const = 0;
};
/// How the encoder spends its bits. CBR holds every frame to rate/fps (with a small buffer, a burst of change is
/// quantized hard); peak-constrained VBR lets a changed frame borrow from still ones up to a peak rate.
enum class RateControl { Cbr, PeakVbr, LowDelayVbr, Quality };
RateControl parseRateControl(const std::string &);
const char *rateControlName(RateControl);
struct EncoderTuning {
    RateControl rateControl = RateControl::Cbr;
    unsigned peakPercent = 150;          // PeakVbr: peak bitrate as a percentage of the mean
    std::optional<unsigned> bufferMs;    // VBV/HRD buffer as time at the mean bitrate; empty = encoder default
    std::optional<unsigned> maxQp, minQp; // Quantizer limits; empty = encoder default
    unsigned gopFrames = 0;              // 0: effectively never on its own; keyframes are then forced on a timer
};
/// maxInFlight bounds how many frames may be inside the encoder at once (queue depth, and so added latency).
std::unique_ptr<IEncoder> hardwareEncoder(Device &, const Display &, unsigned width, unsigned height,
                                          unsigned fps, uint32_t bitrate = 8000000, unsigned maxInFlight = 2,
                                          const EncoderTuning &tuning = {});
} // namespace lm
