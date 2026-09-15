#include "platform.hpp"
namespace bm {
Converter::Converter(Device &d, unsigned iw, unsigned ih, unsigned ow, unsigned oh)
    : device_(d), width_(ow), height_(oh) {
    check(d.device.As(&video_), "D3D11 video device");
    check(d.context.As(&context_), "D3D11 video context");
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputWidth = iw;
    desc.InputHeight = ih;
    desc.OutputWidth = ow;
    desc.OutputHeight = oh;
    desc.InputFrameRate = {60, 1};
    desc.OutputFrameRate = {60, 1};
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    check(video_->CreateVideoProcessorEnumerator(&desc, &enumerator_), "Video processor enumeration");
    UINT flags = 0;
    check(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &flags), "NV12 support");
    if (!(flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        throw std::runtime_error("GPU cannot output NV12");
    check(video_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_), "Video processor");
    context_->VideoProcessorSetStreamAutoProcessingMode(processor_.Get(), 0, FALSE);
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgb{};
    rgb.RGB_Range = 0;
    rgb.YCbCr_Matrix = 1;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuv{};
    yuv.YCbCr_Matrix = 1;
    yuv.Nominal_Range = 1;
    context_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &rgb);
    context_->VideoProcessorSetOutputColorSpace(processor_.Get(), &yuv);
    RECT source{0, 0, LONG(iw), LONG(ih)}, dest{0, 0, LONG(ow), LONG(oh)};
    context_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &source);
    context_->VideoProcessorSetStreamDestRect(processor_.Get(), 0, TRUE, &dest);
    context_->VideoProcessorSetOutputTargetRect(processor_.Get(), TRUE, &dest);
    for (size_t i = 0; i < textures_.size(); ++i) {
        D3D11_TEXTURE2D_DESC t{};
        t.Width = ow;
        t.Height = oh;
        t.MipLevels = 1;
        t.ArraySize = 1;
        t.Format = DXGI_FORMAT_NV12;
        t.SampleDesc.Count = 1;
        t.Usage = D3D11_USAGE_DEFAULT;
        t.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(d.device->CreateTexture2D(&t, nullptr, &textures_[i]), "NV12 texture pool");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC v{};
        v.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        check(video_->CreateVideoProcessorOutputView(textures_[i].Get(), enumerator_.Get(), &v, &views_[i]),
              "NV12 output view");
    }
    for (auto &timing : timings_) {
        D3D11_QUERY_DESC q{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        check(d.device->CreateQuery(&q, &timing.disjoint), "GPU timing query");
        q.Query = D3D11_QUERY_TIMESTAMP;
        check(d.device->CreateQuery(&q, &timing.start), "GPU start query");
        check(d.device->CreateQuery(&q, &timing.end), "GPU end query");
    }
}
ID3D11Texture2D *Converter::convert(ID3D11Texture2D *input, size_t slot) {
    auto found = std::find_if(inputViews_.begin(), inputViews_.end(),
                              [&](const auto &v) { return v.texture.Get() == input; });
    if (found == inputViews_.end()) {
        if (inputViews_.size() >= 4)
            inputViews_.erase(inputViews_.begin());
        InputView cached;
        cached.texture = input;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC v{};
        v.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        check(video_->CreateVideoProcessorInputView(input, enumerator_.Get(), &v, &cached.view),
              "BGRA input view");
        inputViews_.push_back(std::move(cached));
        found = std::prev(inputViews_.end());
    }
    auto &timing = timings_[timingIndex_++ % timings_.size()];
    auto *dc = device_.context.Get();
    if (timing.pending) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0, end = 0;
        if (dc->GetData(timing.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) ==
                S_OK &&
            dc->GetData(timing.start.Get(), &start, sizeof(start), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            dc->GetData(timing.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
            if (!disjoint.Disjoint)
                gpuTimes_.add(1000.0 * (end - start) / disjoint.Frequency);
            timing.pending = false;
        }
    }
    const bool measure = !timing.pending;
    if (measure) {
        dc->Begin(timing.disjoint.Get());
        dc->End(timing.start.Get());
    }
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = found->view.Get();
    check(context_->VideoProcessorBlt(processor_.Get(), views_.at(slot).Get(), 0, 1, &stream),
          "GPU BGRA to NV12");
    if (measure) {
        dc->End(timing.end.Get());
        dc->End(timing.disjoint.Get());
        timing.pending = true;
    }
    return textures_.at(slot).Get();
}
} // namespace bm
