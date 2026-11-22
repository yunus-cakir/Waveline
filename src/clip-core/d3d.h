#pragma once
#include "common.h"

#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

namespace wl {

struct GpuDevice {
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    std::string adapterName;
    UINT vendorId = 0;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice winrtDevice{nullptr};
};

// Creates a D3D11 device on the NVIDIA GPU (falls back to the high-performance adapter).
// Capture, colour conversion and NVENC all run on this one device, so frames never leave
// the GPU.
GpuDevice CreateGpuDevice();

}  // namespace wl
