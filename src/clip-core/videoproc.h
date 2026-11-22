#pragma once
#include "d3d.h"

#include <vector>

namespace wl {

// BGRA -> NV12 (BT.709, limited range) on the GPU with the D3D11 video processor.
// Also scales/letterboxes when the source size changes (e.g. a resized window), so the
// encoder always gets a fixed output size.
class VideoConverter {
public:
    void Init(const GpuDevice& gpu, UINT outWidth, UINT outHeight, UINT fps);

    // Copies a newly captured frame into our own input texture.
    void Upload(ID3D11Texture2D* source, UINT contentWidth, UINT contentHeight);

    // Converts the last uploaded frame into the NV12 texture (or array slice) `dst`.
    // Calling it again without a new Upload repeats the previous frame (constant frame rate).
    void Convert(ID3D11Texture2D* dst, UINT dstSubresource);

    bool HasFrame() const { return inputView_ != nullptr; }

private:
    void Recreate(UINT inWidth, UINT inHeight);
    void UpdateRects();
    ID3D11VideoProcessorOutputView* OutputViewFor(ID3D11Texture2D* dst, UINT subresource);

    com_ptr<ID3D11Device> device_;
    com_ptr<ID3D11DeviceContext> context_;
    com_ptr<ID3D11VideoDevice> videoDevice_;
    com_ptr<ID3D11VideoContext> videoContext_;
    com_ptr<ID3D11VideoProcessorEnumerator> enumerator_;
    com_ptr<ID3D11VideoProcessor> processor_;
    com_ptr<ID3D11Texture2D> input_;
    com_ptr<ID3D11VideoProcessorInputView> inputView_;

    struct OutputView {
        com_ptr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        com_ptr<ID3D11VideoProcessorOutputView> view;
    };
    std::vector<OutputView> outputViews_;

    UINT outWidth_ = 0, outHeight_ = 0, fps_ = 60;
    UINT inWidth_ = 0, inHeight_ = 0;
    UINT contentWidth_ = 0, contentHeight_ = 0;
    UINT frameIndex_ = 0;
};

}  // namespace wl
