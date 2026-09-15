#include "platform.hpp"
#include <atomic>
#include <evr.h>
#include <map>
#include <mferror.h>
#include <strmif.h>
namespace bm {
// A texture becomes writable only after the MFT releases its input sample.
class Released final : public IMFAsyncCallback {
    std::atomic<ULONG> refs_{1};
    std::shared_ptr<std::atomic<bool>> busy_;

  public:
    explicit Released(std::shared_ptr<std::atomic<bool>> busy) : busy_(std::move(busy)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **p) override {
        if (!p)
            return E_POINTER;
        if (id == __uuidof(IUnknown) || id == __uuidof(IMFAsyncCallback)) {
            *p = static_cast<IMFAsyncCallback *>(this);
            AddRef();
            return S_OK;
        }
        *p = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        auto n = --refs_;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD *, DWORD *) override {
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult *) override {
        busy_->store(false);
        return S_OK;
    }
};
class MfEncoder final : public IEncoder {
    ComPtr<IMFTransform> transform_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<ICodecAPI> codec_;
    ComPtr<IMFDXGIDeviceManager> manager_;
    ComPtr<IMFActivate> activation_;
    std::shared_ptr<std::atomic<bool>> busy_ = std::make_shared<std::atomic<bool>>(false);
    ComPtr<IMFAsyncCallback> released_;
    std::string name_;
    unsigned fps_;
    DWORD input_ = 0, output_ = 0;
    unsigned needs_ = 0, outputs_ = 0;
    std::map<int64_t, int64_t> pending_;
    bool idr_ = true;
    std::vector<uint8_t> headers_;
    bool setting(const GUID &key, ULONG value, bool boolean, const char *label) {
        if (!codec_ || codec_->IsSupported(&key) != S_OK || codec_->IsModifiable(&key) != S_OK) {
            std::cout << "Encoder option unsupported: " << label << '\n';
            return false;
        }
        VARIANT v;
        VariantInit(&v);
        if (boolean) {
            v.vt = VT_BOOL;
            v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
        } else {
            v.vt = VT_UI4;
            v.ulVal = value;
        }
        HRESULT hr = codec_->SetValue(&key, &v);
        std::cout << "Encoder option " << label << ": " << (SUCCEEDED(hr) ? "accepted" : "rejected") << '\n';
        return SUCCEEDED(hr);
    }
    void pump() {
        for (;;) {
            ComPtr<IMFMediaEvent> e;
            auto hr = events_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &e);
            if (hr == MF_E_NO_EVENTS_AVAILABLE)
                break;
            check(hr, "Encoder event");
            MediaEventType type;
            check(e->GetType(&type), "Encoder event type");
            HRESULT status;
            check(e->GetStatus(&status), "Encoder event status");
            check(status, "Async encoder");
            if (type == METransformNeedInput)
                ++needs_;
            if (type == METransformHaveOutput)
                ++outputs_;
        }
    }
    void readHeaders() {
        ComPtr<IMFMediaType> type;
        check(transform_->GetOutputCurrentType(output_, &type), "H264 type");
        UINT32 size = 0;
        if (SUCCEEDED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) && size) {
            std::vector<uint8_t> raw(size);
            check(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, raw.data(), size, nullptr), "H264 headers");
            try {
                headers_ = annexB(raw);
            } catch (...) {
                headers_.clear();
            }
        }
    }

  public:
    MfEncoder(Device &device, IMFActivate *activation, unsigned width, unsigned height, unsigned fps)
        : activation_(activation), fps_(fps) {
        wchar_t *friendly = nullptr;
        UINT32 length;
        if (SUCCEEDED(activation->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &friendly, &length))) {
            name_ = utf8(friendly);
            CoTaskMemFree(friendly);
        }
        check(activation->ActivateObject(IID_PPV_ARGS(&transform_)), "Activate hardware encoder");
        ComPtr<IMFAttributes> attrs;
        check(transform_->GetAttributes(&attrs), "Encoder attributes");
        check(attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE), "Unlock hardware MFT");
        UINT32 aware = 0;
        check(attrs->GetUINT32(MF_SA_D3D11_AWARE, &aware), "D3D11-aware encoder");
        if (!aware)
            throw std::runtime_error("Encoder is not D3D11 aware");
        UINT token;
        check(MFCreateDXGIDeviceManager(&token, &manager_), "MF DXGI manager");
        check(manager_->ResetDevice(device.device.Get(), token), "MF device association");
        check(transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                         reinterpret_cast<ULONG_PTR>(manager_.Get())),
              "Set MFT D3D manager");
        check(transform_.As(&events_), "Async MFT events");
        transform_.As(&codec_);
        DWORD ins, outs;
        check(transform_->GetStreamCount(&ins, &outs), "Encoder streams");
        if (ins != 1 || outs != 1)
            throw std::runtime_error("Unsupported encoder stream topology");
        auto hr = transform_->GetStreamIDs(1, &input_, 1, &output_);
        if (hr != E_NOTIMPL)
            check(hr, "Encoder stream IDs");
        setting(CODECAPI_AVLowLatencyMode, TRUE, true, "low latency");
        setting(CODECAPI_AVEncCommonRealTime, TRUE, true, "realtime");
        setting(CODECAPI_AVEncMPVDefaultBPictureCount, 0, false, "zero B frames");
        setting(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR, false, "CBR");
        setting(CODECAPI_AVEncMPVGOPSize, fps * 2, false, "2 second GOP");
        ComPtr<IMFMediaType> out;
        check(MFCreateMediaType(&out), "Output type");
        check(out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video), "Video type");
        check(out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264), "H264 type");
        check(out->SetUINT32(MF_MT_AVG_BITRATE, 8000000), "Initial bitrate");
        check(out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive), "Progressive");
        check(out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base), "Baseline profile");
        check(out->SetUINT32(MF_MT_MPEG2_LEVEL, eAVEncH264VLevel4_2), "H264 level 4.2");
        check(MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, width, height), "Frame dimensions");
        check(MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fps, 1), "Frame rate");
        check(MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1), "Aspect ratio");
        check(transform_->SetOutputType(output_, out.Get(), 0), "Set H264 output");
        ComPtr<IMFMediaType> in;
        check(MFCreateMediaType(&in), "Input type");
        check(out->CopyAllItems(in.Get()), "Copy video attributes");
        in->DeleteItem(MF_MT_MPEG2_PROFILE);
        in->DeleteItem(MF_MT_MPEG2_LEVEL);
        in->DeleteItem(MF_MT_AVG_BITRATE);
        check(in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12), "NV12 input");
        check(in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709), "BT709 matrix");
        check(in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235), "Limited YUV range");
        check(transform_->SetInputType(input_, in.Get(), 0), "Set GPU NV12 input");
        released_.Attach(new Released(busy_));
        check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Begin encoding");
        check(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Start encoding");
        readHeaders();
    }
    ~MfEncoder() {
        if (transform_) {
            transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            ComPtr<IMFShutdown> shutdown;
            if (SUCCEEDED(transform_.As(&shutdown)))
                shutdown->Shutdown();
        }
        if (activation_)
            activation_->ShutdownObject();
    }
    const std::string &name() const override {
        return name_;
    }
    size_t pending() const override {
        return pending_.size();
    }
    bool ready() override {
        pump();
        return needs_ > 0 && pending_.empty() && !busy_->load();
    }
    bool submit(ID3D11Texture2D *texture, int64_t timestamp) override {
        if (!ready())
            return false;
        ComPtr<IMFSample> sample;
        check(MFCreateVideoSampleFromSurface(nullptr, &sample), "Tracked GPU sample");
        ComPtr<IMFMediaBuffer> buffer;
        check(MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), texture, 0, FALSE, &buffer),
              "DXGI surface buffer");
        ComPtr<IMF2DBuffer> twoD;
        check(buffer.As(&twoD), "NV12 2D buffer");
        DWORD size;
        check(twoD->GetContiguousLength(&size), "NV12 length");
        check(buffer->SetCurrentLength(size), "NV12 length metadata");
        check(sample->AddBuffer(buffer.Get()), "Attach GPU buffer");
        check(sample->SetSampleTime(timestamp), "Sample timestamp");
        check(sample->SetSampleDuration(10000000 / fps_), "Sample duration");
        if (idr_) {
            setting(CODECAPI_AVEncVideoForceKeyFrame, 1, false, "IDR request");
            idr_ = false;
        }
        ComPtr<IMFTrackedSample> tracked;
        check(sample.As(&tracked), "Tracked sample interface");
        busy_->store(true);
        check(tracked->SetAllocator(released_.Get(), nullptr), "Sample release notification");
        auto hr = transform_->ProcessInput(input_, sample.Get(), 0);
        if (hr == MF_E_NOTACCEPTING)
            return false;
        check(hr, "Encode GPU surface");
        --needs_;
        pending_[timestamp] = now100ns();
        return true;
    }
    std::vector<Encoded> poll() override {
        pump();
        std::vector<Encoded> frames;
        while (outputs_) {
            --outputs_;
            MFT_OUTPUT_STREAM_INFO info{};
            check(transform_->GetOutputStreamInfo(output_, &info), "H264 output requirements");
            ComPtr<IMFSample> allocated;
            MFT_OUTPUT_DATA_BUFFER out{};
            out.dwStreamID = output_;
            if (!(info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
                check(MFCreateSample(&allocated), "Output sample");
                ComPtr<IMFMediaBuffer> b;
                check(MFCreateAlignedMemoryBuffer(std::max<DWORD>(info.cbSize, 1920u * 1080u * 2u),
                                                  info.cbAlignment ? info.cbAlignment - 1 : 0, &b),
                      "Compressed output buffer");
                check(allocated->AddBuffer(b.Get()), "Output sample buffer");
                out.pSample = allocated.Get();
            }
            DWORD status;
            HRESULT hr = transform_->ProcessOutput(0, 1, &out, &status);
            if (out.pEvents)
                out.pEvents->Release();
            ComPtr<IMFSample> sample;
            if (out.pSample != allocated.Get())
                sample.Attach(out.pSample);
            else
                sample = allocated;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                ComPtr<IMFMediaType> type;
                check(transform_->GetOutputAvailableType(output_, 0, &type), "Changed H264 type");
                check(transform_->SetOutputType(output_, type.Get(), 0), "Apply H264 type");
                readHeaders();
                continue;
            }
            check(hr, "H264 output");
            if (!sample)
                continue;
            Encoded frame;
            check(sample->GetSampleTime(&frame.timestamp), "Encoded timestamp");
            UINT32 key = 0;
            sample->GetUINT32(MFSampleExtension_CleanPoint, &key);
            frame.keyframe = key != 0;
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "Compressed H264 buffer");
            BYTE *data;
            DWORD size;
            check(buffer->Lock(&data, nullptr, &size), "Read compressed bitstream");
            try {
                frame.bytes = annexB({data, size});
            } catch (...) {
                buffer->Unlock();
                throw;
            }
            buffer->Unlock();
            if (frame.keyframe) {
                readHeaders();
                if (!headers_.empty())
                    frame.bytes.insert(frame.bytes.begin(), headers_.begin(), headers_.end());
            }
            auto it = pending_.find(frame.timestamp);
            if (it != pending_.end()) {
                frame.latencyMs = (now100ns() - it->second) / 10000.0;
                pending_.erase(it);
            }
            frames.push_back(std::move(frame));
        }
        return frames;
    }
    void keyframe() override {
        idr_ = true;
    }
    bool bitrate(uint32_t b) override {
        return setting(CODECAPI_AVEncCommonMeanBitRate, b, false, "bitrate");
    }
};
std::unique_ptr<IEncoder> hardwareEncoder(Device &device, const Display &display, unsigned width,
                                          unsigned height, unsigned fps) {
    ComPtr<IMFAttributes> attrs;
    check(MFCreateAttributes(&attrs, 1), "Encoder filter");
    UINT64 luid =
        uint64_t(uint32_t(display.luid.LowPart)) | (uint64_t(uint32_t(display.luid.HighPart)) << 32);
    check(attrs->SetUINT64(MFT_ENUM_ADAPTER_LUID, luid), "Encoder adapter LUID");
    MFT_REGISTER_TYPE_INFO in{MFMediaType_Video, MFVideoFormat_NV12},
        out{MFMediaType_Video, MFVideoFormat_H264};
    IMFActivate **acts = nullptr;
    UINT32 count = 0;
    check(MFTEnum2(MFT_CATEGORY_VIDEO_ENCODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER, &in,
                   &out, attrs.Get(), &acts, &count),
          "Enumerate hardware H264 on selected GPU");
    std::vector<ComPtr<IMFActivate>> candidates;
    for (UINT32 i = 0; i < count; ++i) {
        ComPtr<IMFActivate> a;
        a.Attach(acts[i]);
        candidates.push_back(a);
    }
    CoTaskMemFree(acts);
    for (auto &a : candidates) {
        try {
            return std::make_unique<MfEncoder>(device, a.Get(), width, height, fps);
        } catch (const std::exception &e) {
            std::cerr << "Hardware encoder rejected: " << e.what() << '\n';
            a->ShutdownObject();
        }
    }
    throw std::runtime_error("No compatible D3D11 hardware H264 encoder on the display GPU. No CPU-readback "
                             "or cross-GPU fallback is enabled.");
}
} // namespace bm
