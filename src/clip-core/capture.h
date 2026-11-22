#pragma once
#include "d3d.h"

#include <atomic>
#include <mutex>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>

namespace wl {

struct CapturedFrame {
    // Keep the frame alive until its texture has been copied; closing it returns the
    // surface to the frame pool.
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
    com_ptr<ID3D11Texture2D> texture;
    UINT contentWidth = 0;
    UINT contentHeight = 0;
};

// Windows.Graphics.Capture source for a monitor or a window. Frames are not copied when
// they arrive: only the newest one is kept, and the video thread takes it at the target
// frame rate. A game running at 143 FPS therefore costs us 60 copies per second, not 143.
class CaptureSource {
public:
    explicit CaptureSource(winrt::Windows::Graphics::Capture::GraphicsCaptureItem item);
    ~CaptureSource();
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;

    static winrt::Windows::Graphics::Capture::GraphicsCaptureItem ItemForMonitor(HMONITOR monitor);
    static winrt::Windows::Graphics::Capture::GraphicsCaptureItem ItemForWindow(HWND window);

    void Start(const GpuDevice& gpu, bool captureCursor);
    void Stop();

    // Moves out the newest frame that arrived since the previous call.
    bool TakeLatest(CapturedFrame& out);

    winrt::Windows::Graphics::SizeInt32 ItemSize() const { return item_.Size(); }
    uint64_t ArrivedFrames() const { return arrived_.load(); }
    bool Closed() const { return closed_.load(); }
    bool BorderDisabled() const { return borderDisabled_; }

private:
    void OnFrameArrived(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender);
    void HandleFrame(winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender);

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device_{nullptr};
    winrt::event_token frameToken_{};
    winrt::event_token closedToken_{};
    winrt::Windows::Graphics::SizeInt32 poolSize_{};

    std::mutex mutex_;
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame latest_{nullptr};
    std::atomic<uint64_t> arrived_{0};
    std::atomic<bool> closed_{false};
    bool borderDisabled_ = false;
};

}  // namespace wl
