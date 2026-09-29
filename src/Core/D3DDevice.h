#pragma once

namespace RegionLens::native
{
    class D3DDevice
    {
    public:
        bool Initialize();

        [[nodiscard]] ID3D11Device* Device() const noexcept { return m_device.Get(); }
        [[nodiscard]] ID3D11DeviceContext* Context() const noexcept { return m_context.Get(); }
        [[nodiscard]] IDXGIFactory2* Factory() const noexcept { return m_factory.Get(); }
        [[nodiscard]] winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice WinrtDevice() const noexcept { return m_winrtDevice; }

    private:
        Microsoft::WRL::ComPtr<ID3D11Device> m_device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
        Microsoft::WRL::ComPtr<IDXGIFactory2> m_factory;
        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice m_winrtDevice{ nullptr };
    };
}
