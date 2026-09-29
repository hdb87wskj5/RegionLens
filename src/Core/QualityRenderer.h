#pragma once
#include "pch.h"
#include "LensQuality.h"

namespace RegionLens::native
{
    // GPU-only content stage, also usable by offscreen tests without HWND/input.
    class QualityRenderer
    {
    public:
        HRESULT Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
            ID3D11ShaderResourceView* source, QualityCacheKey const& key, LensSharpness sharpness);
        ID3D11ShaderResourceView* View() const noexcept { return m_sharpened ? m_sharpView.Get() : m_resizedView.Get(); }
        void Reset() noexcept;
        uint64_t ResampleCount() const noexcept { return m_resamples; }
        uint64_t SharpenCount() const noexcept { return m_sharpens; }
        uint64_t ResourceBytes() const noexcept { return uint64_t(m_size.width) * m_size.height * (m_sharpView ? 8 : 4); }
        static bool ValidateShaders();
    private:
        HRESULT Initialize(ID3D11Device* device);
        HRESULT Allocate(ID3D11Device* device, RenderExtent size, bool sharp);
        HRESULT Draw(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
            ID3D11RenderTargetView* target, ID3D11ShaderResourceView* source, QualityCacheKey const& key, float strength);
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertex;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_resize, m_cas;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_resizedTarget, m_sharpTarget;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_resizedView, m_linearView, m_sharpView;
        RenderExtent m_size{};
        QualityCacheKey m_key{};
        LensSharpness m_strength{};
        bool m_valid{}, m_sharpValid{}, m_sharpened{};
        uint64_t m_resamples{}, m_sharpens{};
    };
}
