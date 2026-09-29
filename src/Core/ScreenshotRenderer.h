#pragma once
#include "QualityRenderer.h"

namespace RegionLens::native
{
    constexpr uint64_t MaximumScreenshotBytes = 128ull * 1024 * 1024;
    inline bool ValidScreenshotExtent(RenderExtent size) noexcept
    {
        return size.width && size.height && size.width <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
            size.height <= D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION &&
            uint64_t(size.width) * size.height * 4 <= MaximumScreenshotBytes;
    }

    // All D3D calls stay on the UI/render thread. No HWND capture or Present:
    // buttons, pointer, hints and selection overlays are never part of this pass.
    class ScreenshotRenderer
    {
    public:
        HRESULT Initialize(ID3D11Device* device);
        HRESULT Capture(ID3D11Device* device, ID3D11DeviceContext* context,
            ID3D11ShaderResourceView* source, PixelRect crop, RenderExtent sourceSize,
            RenderExtent output, LensQualitySettings quality, CaptureStamp stamp,
            Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging);
    private:
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixel;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
    };
}
