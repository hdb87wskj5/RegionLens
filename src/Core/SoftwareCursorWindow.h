#pragma once

#include <windows.h>
#include <cstdint>

namespace RegionLens::native
{
    // UI-thread-owned cursor surface. It never receives input and does not
    // change the desktop cursor's visibility or position.
    class SoftwareCursorWindow
    {
    public:
        SoftwareCursorWindow() = default;
        ~SoftwareCursorWindow() { Destroy(); }
        SoftwareCursorWindow(SoftwareCursorWindow const&) = delete;
        SoftwareCursorWindow& operator=(SoftwareCursorWindow const&) = delete;

        bool Create(HWND owner);
        // Upload the sprite while the window remains hidden. The caller can
        // hide the native cursor only after this succeeds, then call ShowAt.
        bool PrepareAt(POINT screenPoint, HCURSOR shape);
        bool ShowAt(POINT screenPoint, HCURSOR shape);
        bool EnsureTopmost() noexcept;
        void Hide() noexcept;
        void Destroy() noexcept;

        HWND Window() const noexcept { return m_window; }
        bool Ready() const noexcept { return m_window != nullptr; }

    private:
        bool RenderShape(HCURSOR shape, POINT screenPoint, bool reveal);
        bool EnsureSurface(int width, int height);
        void ReleaseSurface() noexcept;

        HWND m_window{};
        HDC m_memoryDc{};
        HBITMAP m_bitmap{};
        HGDIOBJ m_oldBitmap{};
        uint32_t* m_pixels{};
        HCURSOR m_shape{};
        POINT m_hotspot{};
        SIZE m_size{};
        POINT m_lastPoint{};
        DWORD m_uiThread{};
        bool m_visible{};
    };
}
