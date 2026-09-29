#include "pch.h"
#include "SoftwareCursorWindow.h"

#include <limits>

namespace RegionLens::native
{
    namespace
    {
        constexpr wchar_t CursorWindowClass[] = L"RegionLens.SoftwareCursorWindow";
        constexpr int MaximumCursorExtent = 512;

        LRESULT CALLBACK CursorWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
        {
            switch (message)
            {
            case WM_NCHITTEST: return HTTRANSPARENT;
            case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
            case WM_ERASEBKGND: return 1;
            case WM_SETCURSOR: return TRUE;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }

        bool RegisterCursorWindowClass()
        {
            WNDCLASSEXW cls{ sizeof(cls) };
            cls.lpfnWndProc = CursorWindowProc;
            cls.hInstance = GetModuleHandleW(nullptr);
            cls.lpszClassName = CursorWindowClass;
            if (RegisterClassExW(&cls)) return true;
            return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
        }

        bool ReadBitmap(HBITMAP bitmap, int width, int height, std::vector<uint32_t>& pixels)
        {
            if (!bitmap || width <= 0 || height <= 0) return false;
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = width;
            info.bmiHeader.biHeight = -height;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            pixels.resize(size_t(width) * size_t(height));
            HDC dc = CreateCompatibleDC(nullptr);
            if (!dc) return false;
            auto lines = GetDIBits(dc, bitmap, 0, UINT(height), pixels.data(), &info, DIB_RGB_COLORS);
            DeleteDC(dc);
            return lines == height;
        }

        int ScreenCoordinate(LONG point, LONG hotspot)
        {
            auto value = int64_t(point) - hotspot;
            return int(std::clamp(value, int64_t(std::numeric_limits<int>::min()),
                int64_t(std::numeric_limits<int>::max())));
        }

        uint8_t Premultiply(uint8_t channel, uint8_t alpha)
        {
            return uint8_t((unsigned(channel) * unsigned(alpha) + 127U) / 255U);
        }
    }

    bool SoftwareCursorWindow::Create(HWND owner)
    {
        if (m_window) return GetCurrentThreadId() == m_uiThread;
        if (!RegisterCursorWindowClass()) return false;
        m_uiThread = GetCurrentThreadId();
        m_window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
            CursorWindowClass, L"", WS_POPUP,
            0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!m_window) return false;
        // Failure is fatal: a cursor overlay captured by WGC would be recursive.
        if (!SetWindowDisplayAffinity(m_window, WDA_EXCLUDEFROMCAPTURE))
        {
            Destroy();
            return false;
        }
        return true;
    }

    bool SoftwareCursorWindow::EnsureSurface(int width, int height)
    {
        if (m_bitmap && m_size.cx == width && m_size.cy == height) return true;
        ReleaseSurface();
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits{};
        m_memoryDc = CreateCompatibleDC(nullptr);
        if (!m_memoryDc) return false;
        m_bitmap = CreateDIBSection(m_memoryDc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!m_bitmap || !bits)
        {
            ReleaseSurface();
            return false;
        }
        m_oldBitmap = SelectObject(m_memoryDc, m_bitmap);
        if (!m_oldBitmap || m_oldBitmap == HGDI_ERROR)
        {
            ReleaseSurface();
            return false;
        }
        m_pixels = static_cast<uint32_t*>(bits);
        m_size = { width, height };
        return true;
    }

    bool SoftwareCursorWindow::RenderShape(HCURSOR shape, POINT screenPoint, bool reveal)
    {
        ICONINFO icon{};
        if (!GetIconInfo(shape, &icon)) return false;
        struct IconCleanup
        {
            ICONINFO& icon;
            ~IconCleanup()
            {
                if (icon.hbmColor) DeleteObject(icon.hbmColor);
                if (icon.hbmMask) DeleteObject(icon.hbmMask);
            }
        } cleanup{ icon };

        BITMAP bitmap{};
        if (!GetObjectW(icon.hbmColor ? icon.hbmColor : icon.hbmMask, sizeof(bitmap), &bitmap))
            return false;
        const int width = bitmap.bmWidth;
        const int height = icon.hbmColor ? bitmap.bmHeight : bitmap.bmHeight / 2;
        if (width <= 0 || height <= 0 || width > MaximumCursorExtent || height > MaximumCursorExtent)
            return false;

        std::vector<uint32_t> colors;
        std::vector<uint32_t> masks;
        if (icon.hbmColor && !ReadBitmap(icon.hbmColor, width, height, colors)) return false;
        bool hasAlpha = std::any_of(colors.begin(), colors.end(),
            [](uint32_t pixel) { return (pixel >> 24) != 0; });
        if (!hasAlpha && !ReadBitmap(icon.hbmMask, width, icon.hbmColor ? height : height * 2, masks))
            return false;
        if (!EnsureSurface(width, height)) return false;

        size_t count = size_t(width) * size_t(height);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t color = icon.hbmColor ? colors[i] : masks[i + count];
            uint8_t alpha{};
            if (hasAlpha)
                alpha = uint8_t(color >> 24);
            else if ((masks[i] & 0x00FFFFFF) == 0)
                alpha = 255;
            else if ((color & 0x00FFFFFF) != 0)
            {
                // Monochrome AND+XOR "invert" cannot be represented by a
                // transparent layered window. Approximate it with white.
                color = 0x00FFFFFF;
                alpha = 255;
            }

            uint8_t blue = Premultiply(uint8_t(color), alpha);
            uint8_t green = Premultiply(uint8_t(color >> 8), alpha);
            uint8_t red = Premultiply(uint8_t(color >> 16), alpha);
            m_pixels[i] = (uint32_t(alpha) << 24) | (uint32_t(red) << 16) |
                (uint32_t(green) << 8) | blue;
        }

        POINT destination{ ScreenCoordinate(screenPoint.x, LONG(icon.xHotspot)),
            ScreenCoordinate(screenPoint.y, LONG(icon.yHotspot)) };
        POINT origin{};
        SIZE size{ width, height };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        HDC desktop = GetDC(nullptr);
        if (!desktop) return false;
        BOOL updated = UpdateLayeredWindow(m_window, desktop, &destination, &size,
            m_memoryDc, &origin, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, desktop);
        if (!updated) return false;
        m_shape = shape;
        m_hotspot = { LONG(icon.xHotspot), LONG(icon.yHotspot) };
        m_lastPoint = screenPoint;
        if (!reveal) return true;
        return SetWindowPos(m_window, HWND_TOPMOST, destination.x, destination.y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW) != FALSE;
    }

    bool SoftwareCursorWindow::PrepareAt(POINT screenPoint, HCURSOR shape)
    {
        if (!m_window || GetCurrentThreadId() != m_uiThread) return false;
        Hide();
        if (!shape) shape = LoadCursorW(nullptr, IDC_ARROW);
        if (!shape) return false;
        if (RenderShape(shape, screenPoint, false)) return true;
        auto arrow = LoadCursorW(nullptr, IDC_ARROW);
        return arrow && shape != arrow && RenderShape(arrow, screenPoint, false);
    }

    bool SoftwareCursorWindow::ShowAt(POINT screenPoint, HCURSOR shape)
    {
        if (!m_window || GetCurrentThreadId() != m_uiThread) return false;
        if (!shape) shape = LoadCursorW(nullptr, IDC_ARROW);
        if (!shape) return false;
        if (shape != m_shape || !m_bitmap)
        {
            if (!RenderShape(shape, screenPoint, true))
            {
                auto arrow = LoadCursorW(nullptr, IDC_ARROW);
                if (!arrow || shape == arrow || !RenderShape(arrow, screenPoint, true))
                {
                    Hide();
                    return false;
                }
            }
            m_visible = true;
            return true;
        }
        if (m_visible && screenPoint.x == m_lastPoint.x && screenPoint.y == m_lastPoint.y)
            return true;
        POINT destination{ ScreenCoordinate(screenPoint.x, m_hotspot.x),
            ScreenCoordinate(screenPoint.y, m_hotspot.y) };
        if (!SetWindowPos(m_window, HWND_TOPMOST, destination.x, destination.y, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW))
        {
            Hide();
            return false;
        }
        m_lastPoint = screenPoint;
        m_visible = true;
        return true;
    }

    bool SoftwareCursorWindow::EnsureTopmost() noexcept
    {
        if (!m_window || !m_visible) return true;
        if (GetCurrentThreadId() != m_uiThread) return false;
        return SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) != FALSE;
    }

    void SoftwareCursorWindow::Hide() noexcept
    {
        if (m_window && GetCurrentThreadId() == m_uiThread)
        {
            ShowWindow(m_window, SW_HIDE);
            m_visible = false;
        }
    }

    void SoftwareCursorWindow::ReleaseSurface() noexcept
    {
        if (m_memoryDc && m_oldBitmap && m_oldBitmap != HGDI_ERROR)
            SelectObject(m_memoryDc, m_oldBitmap);
        if (m_bitmap) DeleteObject(m_bitmap);
        if (m_memoryDc) DeleteDC(m_memoryDc);
        m_memoryDc = nullptr;
        m_bitmap = nullptr;
        m_oldBitmap = nullptr;
        m_pixels = nullptr;
        m_size = {};
        m_shape = nullptr;
    }

    void SoftwareCursorWindow::Destroy() noexcept
    {
        if (m_window && GetCurrentThreadId() != m_uiThread) return;
        Hide();
        ReleaseSurface();
        if (m_window)
        {
            DestroyWindow(m_window);
            m_window = nullptr;
        }
        m_uiThread = 0;
    }
}
