#pragma once
#include "D3DDevice.h"

namespace RegionLens::native
{
    class CursorTexture
    {
    public:
        bool Update(HCURSOR cursor, ID3D11Device* device);
        ID3D11ShaderResourceView* View() const noexcept { return m_view.Get(); }
        POINT Hotspot() const noexcept { return m_hotspot; }
        SIZE Size() const noexcept { return m_size; }
        float Mode() const noexcept { return m_alpha ? 1.0f : 2.0f; }
    private:
        bool TryUpdate(HCURSOR cursor, ID3D11Device* device);
        HCURSOR m_cursor{};
        POINT m_hotspot{};
        SIZE m_size{};
        bool m_alpha{};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_view;
    };
}
