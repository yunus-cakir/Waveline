#include "d3d.h"

#include <windows.graphics.directx.direct3d11.interop.h>

namespace wl {

GpuDevice CreateGpuDevice() {
    com_ptr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory2(0, IID_PPV_ARGS(factory.put())), "CreateDXGIFactory2");

    com_ptr<IDXGIAdapter1> chosen;
    DXGI_ADAPTER_DESC1 chosenDesc{};
    for (UINT i = 0;; ++i) {
        com_ptr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(adapter.put())) == DXGI_ERROR_NOT_FOUND)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const bool isNvidia = desc.VendorId == 0x10DE;
        if (!chosen || (isNvidia && chosenDesc.VendorId != 0x10DE)) {
            chosen = adapter;
            chosenDesc = desc;
        }
    }
    if (!chosen) throw std::runtime_error("Uygun GPU bulunamadi");

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    GpuDevice gpu;
    Check(D3D11CreateDevice(chosen.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels,
                            ARRAYSIZE(levels), D3D11_SDK_VERSION, gpu.device.put(), nullptr,
                            gpu.context.put()),
          "D3D11CreateDevice");

    // The encoder MFT, the capture callback and our video thread share this device.
    if (auto mt = gpu.context.try_as<ID3D11Multithread>()) mt->SetMultithreadProtected(TRUE);

    gpu.adapterName = ToUtf8(chosenDesc.Description);
    gpu.vendorId = chosenDesc.VendorId;

    auto dxgiDevice = gpu.device.as<IDXGIDevice>();
    com_ptr<::IInspectable> inspectable;
    Check(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()),
          "CreateDirect3D11DeviceFromDXGIDevice");
    gpu.winrtDevice = inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
    return gpu;
}

}  // namespace wl
