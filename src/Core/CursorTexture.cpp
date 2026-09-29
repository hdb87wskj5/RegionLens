#include "pch.h"
#include "CursorTexture.h"

namespace RegionLens::native
{
    namespace
    {
        bool ReadBitmap(HBITMAP bitmap, int width, int height, std::vector<uint32_t>& pixels)
        {
            BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
            pixels.resize(size_t(width) * height);
            auto dc = CreateCompatibleDC(nullptr);
            if (!dc) return false;
            auto lines = GetDIBits(dc, bitmap, 0, height, pixels.data(), &info, DIB_RGB_COLORS);
            DeleteDC(dc);
            return lines == height;
        }
    }
    bool CursorTexture::Update(HCURSOR cursor, ID3D11Device* device)
    {
        auto arrow = LoadCursorW(nullptr, IDC_ARROW);
        if (!cursor) cursor = arrow;
        return TryUpdate(cursor, device) || (cursor != arrow && TryUpdate(arrow, device));
    }
    bool CursorTexture::TryUpdate(HCURSOR cursor, ID3D11Device* device)
    {
        if (m_cursor == cursor && m_view) return true;
        ICONINFO icon{};
        if (!GetIconInfo(cursor, &icon)) return false;
        struct Cleanup
        {
            ICONINFO& icon;
            ~Cleanup() { if (icon.hbmColor) DeleteObject(icon.hbmColor); if (icon.hbmMask) DeleteObject(icon.hbmMask); }
        } cleanup{ icon };
        BITMAP bitmap{};
        if (!GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap)) return false;
        int width = bitmap.bmWidth;
        int height = icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
        if (width <= 0 || height <= 0 || width > 512 || height > 512) return false;
        std::vector<uint32_t> colors, masks;
        if (icon.hbmColor && !ReadBitmap(icon.hbmColor, width, height, colors)) return false;
        bool alpha = !colors.empty() && std::any_of(colors.begin(), colors.end(), [](uint32_t c) { return (c >> 24) != 0; });
        if (!alpha && !ReadBitmap(icon.hbmMask, width, icon.hbmColor ? height : height * 2, masks)) return false;
        size_t size = size_t(width) * height;
        std::vector<uint32_t> rgba(size);
        for (size_t i = 0; i < size; ++i)
        {
            uint32_t color = icon.hbmColor ? colors[i] : masks[i + size];
            uint32_t a = alpha ? color >> 24 : ((masks[i] & 0xFFFFFF) ? 255 : 0);
            // Legacy cursors preserve the complete AND/XOR operation, including
            // inversion (not approximated with an alpha-blended white rectangle).
            rgba[i] = ((color >> 16) & 255) | (color & 0xFF00) | ((color & 255) << 16) | (a << 24);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{ rgba.data(), UINT(width * 4), 0 };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        if (FAILED(device->CreateTexture2D(&desc, &data, &texture)) ||
            FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view))) return false;
        m_view = std::move(view); m_cursor = cursor; m_hotspot = { LONG(icon.xHotspot), LONG(icon.yHotspot) };
        m_size = { width, height }; m_alpha = alpha;
        return true;
    }
}
