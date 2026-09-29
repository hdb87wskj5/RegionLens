#include "pch.h"
#include "D3DDevice.h"
#include "AppRuntime.h"

using Microsoft::WRL::ComPtr;

namespace RegionLens::native
{
    bool D3DDevice::Initialize()
    {
        constexpr D3D_FEATURE_LEVEL levels[] =
        {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL selected{};
        auto hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            levels,
            static_cast<UINT>(std::size(levels)),
            D3D11_SDK_VERSION,
            m_device.ReleaseAndGetAddressOf(),
            &selected,
            m_context.ReleaseAndGetAddressOf());

#if defined(_DEBUG)
        if (hr == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            hr = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                levels,
                static_cast<UINT>(std::size(levels)),
                D3D11_SDK_VERSION,
                m_device.ReleaseAndGetAddressOf(),
                &selected,
                m_context.ReleaseAndGetAddressOf());
        }
#endif
        if (FAILED(hr))
        {
            return false;
        }

        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(m_context.As(&multithread)))
        {
            multithread->SetMultithreadProtected(TRUE);
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        if (FAILED(m_device.As(&dxgiDevice)))
        {
            return false;
        }

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDevice->GetAdapter(adapter.ReleaseAndGetAddressOf())))
        {
            return false;
        }
        if (FAILED(adapter->GetParent(IID_PPV_ARGS(m_factory.ReleaseAndGetAddressOf()))))
        {
            return false;
        }

        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(adapter->GetDesc(&description))) {
            DEVMODEW mode{}; mode.dmSize=sizeof(mode);EnumDisplaySettingsW(nullptr,ENUM_CURRENT_SETTINGS,&mode);
            Record(DiagnosticEvent::Adapter,{description.VendorId,description.DeviceId,
                description.AdapterLuid.HighPart,description.AdapterLuid.LowPart,
                mode.dmDisplayFrequency,mode.dmPelsWidth,mode.dmPelsHeight});
        }

        ComPtr<IInspectable> inspectable;
        if (FAILED(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.ReleaseAndGetAddressOf())))
        {
            return false;
        }
        m_winrtDevice = winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice{
            inspectable.Detach(), winrt::take_ownership_from_abi };

        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(dxgiDevice.As(&dxgiDevice1)))
        {
            dxgiDevice1->SetMaximumFrameLatency(1);
        }
        return true;
    }
}
