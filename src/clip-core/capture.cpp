#include "capture.h"

#include <algorithm>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

using namespace winrt::Windows::Graphics::Capture;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

namespace wl {

namespace {
constexpr auto kPixelFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;
constexpr int32_t kPoolBuffers = 2;
}  // namespace

GraphicsCaptureItem CaptureSource::ItemForMonitor(HMONITOR monitor) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    Check(interop->CreateForMonitor(monitor, winrt::guid_of<IGraphicsCaptureItem>(),
                                    winrt::put_abi(item)),
          "IGraphicsCaptureItemInterop::CreateForMonitor");
    return item;
}

GraphicsCaptureItem CaptureSource::ItemForWindow(HWND window) {
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{nullptr};
    Check(interop->CreateForWindow(window, winrt::guid_of<IGraphicsCaptureItem>(),
                                   winrt::put_abi(item)),
          "IGraphicsCaptureItemInterop::CreateForWindow");
    return item;
}

CaptureSource::CaptureSource(GraphicsCaptureItem item) : item_(std::move(item)) {}

CaptureSource::~CaptureSource() {
    try {
        Stop();
    } catch (...) {
    }
}

void CaptureSource::Start(const GpuDevice& gpu, bool captureCursor) {
    if (!GraphicsCaptureSession::IsSupported())
        throw std::runtime_error("Windows.Graphics.Capture bu sistemde desteklenmiyor");

    device_ = gpu.winrtDevice;
    poolSize_ = item_.Size();
    pool_ = Direct3D11CaptureFramePool::CreateFreeThreaded(device_, kPixelFormat, kPoolBuffers, poolSize_);
    frameToken_ = pool_.FrameArrived([this](auto const& sender, auto const&) { OnFrameArrived(sender); });
    closedToken_ = item_.Closed([this](auto const&, auto const&) { closed_ = true; });

    session_ = pool_.CreateCaptureSession(item_);
    try {
        session_.IsCursorCaptureEnabled(captureCursor);
    } catch (...) {
        Warn("Imlec ayari desteklenmiyor");
    }
    try {
        session_.IsBorderRequired(false);  // no yellow border (Windows 11)
        borderDisabled_ = true;
    } catch (...) {
        Warn("Sari yakalama cercevesi kapatilamadi (Windows surumu desteklemiyor olabilir)");
    }
    session_.StartCapture();
}

void CaptureSource::Stop() {
    if (session_) {
        session_.Close();
        session_ = nullptr;
    }
    if (pool_) {
        pool_.FrameArrived(frameToken_);
        pool_.Close();
        pool_ = nullptr;
    }
    if (item_ && closedToken_.value) {
        item_.Closed(closedToken_);
        closedToken_ = {};
    }
    std::lock_guard lock(mutex_);
    if (latest_) {
        latest_.Close();
        latest_ = nullptr;
    }
}

void CaptureSource::OnFrameArrived(Direct3D11CaptureFramePool const& sender) {
    try {
        HandleFrame(sender);
    } catch (const winrt::hresult_error& e) {
        Warn("Yakalama karesi islenemedi: %s (0x%08X)", ToUtf8(e.message().c_str()).c_str(),
             static_cast<unsigned>(e.code().value));
    }
}

void CaptureSource::HandleFrame(Direct3D11CaptureFramePool const& sender) {
    auto frame = sender.TryGetNextFrame();
    if (!frame) return;
    ++arrived_;

    // The window (or monitor resolution) changed size: resize the pool. The current frame
    // stays valid and is cropped to its content size by the converter.
    const SizeInt32 content = frame.ContentSize();
    if (content.Width > 0 && content.Height > 0 &&
        (content.Width != poolSize_.Width || content.Height != poolSize_.Height)) {
        poolSize_ = content;
        sender.Recreate(device_, kPixelFormat, kPoolBuffers, poolSize_);
    }

    std::lock_guard lock(mutex_);
    if (latest_) latest_.Close();  // never consumed: drop it
    latest_ = frame;
}

bool CaptureSource::TakeLatest(CapturedFrame& out) {
    Direct3D11CaptureFrame frame{nullptr};
    {
        std::lock_guard lock(mutex_);
        frame = latest_;
        latest_ = nullptr;
    }
    if (!frame) return false;

    auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    com_ptr<ID3D11Texture2D> texture;
    Check(access->GetInterface(IID_PPV_ARGS(texture.put())), "IDirect3DDxgiInterfaceAccess::GetInterface");

    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    const SizeInt32 content = frame.ContentSize();

    out.frame = frame;
    out.texture = texture;
    out.contentWidth = std::min<UINT>(static_cast<UINT>(std::max(content.Width, 1)), desc.Width);
    out.contentHeight = std::min<UINT>(static_cast<UINT>(std::max(content.Height, 1)), desc.Height);
    return true;
}

}  // namespace wl
