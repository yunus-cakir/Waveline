#include "videoproc.h"

#include <algorithm>

namespace wl {

void VideoConverter::Init(const GpuDevice& gpu, UINT outWidth, UINT outHeight, UINT fps) {
    device_ = gpu.device;
    context_ = gpu.context;
    videoDevice_ = device_.as<ID3D11VideoDevice>();
    videoContext_ = context_.as<ID3D11VideoContext>();
    outWidth_ = outWidth;
    outHeight_ = outHeight;
    fps_ = fps;
}

void VideoConverter::Recreate(UINT inWidth, UINT inHeight) {
    inWidth_ = inWidth;
    inHeight_ = inHeight;
    outputViews_.clear();
    inputView_ = nullptr;
    processor_ = nullptr;
    enumerator_ = nullptr;
    input_ = nullptr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = inWidth;
    td.Height = inHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    Check(device_->CreateTexture2D(&td, nullptr, input_.put()), "CreateTexture2D(input)");

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {fps_, 1};
    cd.InputWidth = inWidth;
    cd.InputHeight = inHeight;
    cd.OutputFrameRate = {fps_, 1};
    cd.OutputWidth = outWidth_;
    cd.OutputHeight = outHeight_;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    Check(videoDevice_->CreateVideoProcessorEnumerator(&cd, enumerator_.put()),
          "CreateVideoProcessorEnumerator");
    Check(videoDevice_->CreateVideoProcessor(enumerator_.get(), 0, processor_.put()),
          "CreateVideoProcessor");

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Check(videoDevice_->CreateVideoProcessorInputView(input_.get(), enumerator_.get(), &ivd,
                                                      inputView_.put()),
          "CreateVideoProcessorInputView");

    if (auto vc1 = videoContext_.try_as<ID3D11VideoContext1>()) {
        vc1->VideoProcessorSetStreamColorSpace1(processor_.get(), 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        vc1->VideoProcessorSetOutputColorSpace1(processor_.get(), DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{};
        in.RGB_Range = 0;  // full range RGB
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE out{};
        out.YCbCr_Matrix = 1;  // BT.709
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.get(), 0, &in);
        videoContext_->VideoProcessorSetOutputColorSpace(processor_.get(), &out);
    }
    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.get(), 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    videoContext_->VideoProcessorSetStreamAutoProcessingMode(processor_.get(), 0, FALSE);

    D3D11_VIDEO_COLOR black{};
    black.YCbCr = {0.0625f, 0.5f, 0.5f, 1.0f};
    videoContext_->VideoProcessorSetOutputBackgroundColor(processor_.get(), TRUE, &black);
}

void VideoConverter::UpdateRects() {
    const RECT src{0, 0, static_cast<LONG>(contentWidth_), static_cast<LONG>(contentHeight_)};
    videoContext_->VideoProcessorSetStreamSourceRect(processor_.get(), 0, TRUE, &src);

    // Fit the content into the output while keeping its aspect ratio.
    const double scale = std::min(static_cast<double>(outWidth_) / contentWidth_,
                                  static_cast<double>(outHeight_) / contentHeight_);
    const LONG w = static_cast<LONG>(contentWidth_ * scale) & ~1L;
    const LONG h = static_cast<LONG>(contentHeight_ * scale) & ~1L;
    const LONG x = ((static_cast<LONG>(outWidth_) - w) / 2) & ~1L;
    const LONG y = ((static_cast<LONG>(outHeight_) - h) / 2) & ~1L;
    const RECT dst{x, y, x + w, y + h};
    videoContext_->VideoProcessorSetStreamDestRect(processor_.get(), 0, TRUE, &dst);

    const RECT target{0, 0, static_cast<LONG>(outWidth_), static_cast<LONG>(outHeight_)};
    videoContext_->VideoProcessorSetOutputTargetRect(processor_.get(), TRUE, &target);
}

void VideoConverter::Upload(ID3D11Texture2D* source, UINT contentWidth, UINT contentHeight) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Width != inWidth_ || desc.Height != inHeight_ || !processor_) {
        Recreate(desc.Width, desc.Height);
        contentWidth_ = 0;  // force rect update
    }
    context_->CopyResource(input_.get(), source);
    if (contentWidth != contentWidth_ || contentHeight != contentHeight_) {
        contentWidth_ = contentWidth;
        contentHeight_ = contentHeight;
        UpdateRects();
    }
}

ID3D11VideoProcessorOutputView* VideoConverter::OutputViewFor(ID3D11Texture2D* dst, UINT subresource) {
    for (auto& v : outputViews_)
        if (v.texture.get() == dst && v.subresource == subresource) return v.view.get();

    if (outputViews_.size() > 64) outputViews_.clear();

    D3D11_TEXTURE2D_DESC desc{};
    dst->GetDesc(&desc);
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    if (desc.ArraySize > 1) {
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
        ovd.Texture2DArray.MipSlice = 0;
        ovd.Texture2DArray.FirstArraySlice = subresource;
        ovd.Texture2DArray.ArraySize = 1;
    } else {
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
    }

    OutputView entry;
    entry.texture.copy_from(dst);
    entry.subresource = subresource;
    Check(videoDevice_->CreateVideoProcessorOutputView(dst, enumerator_.get(), &ovd, entry.view.put()),
          "CreateVideoProcessorOutputView");
    outputViews_.push_back(std::move(entry));
    return outputViews_.back().view.get();
}

void VideoConverter::Convert(ID3D11Texture2D* dst, UINT dstSubresource) {
    if (!inputView_) throw std::runtime_error("VideoConverter: henuz kare yok");
    ID3D11VideoProcessorOutputView* view = OutputViewFor(dst, dstSubresource);

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inputView_.get();
    Check(videoContext_->VideoProcessorBlt(processor_.get(), view, frameIndex_++, 1, &stream),
          "VideoProcessorBlt");
}

}  // namespace wl
