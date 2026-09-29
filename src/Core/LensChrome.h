#pragma once

#include "Localization.h"
#include "LensQuality.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>

namespace RegionLens::native
{
    constexpr int PinButtonId = 2001;
    constexpr int CloseButtonId = 2002;
    constexpr int FullscreenButtonId = 2003;
    constexpr int InputMappingButtonId = 2004;
    constexpr int RestoreSizeButtonId = 2005;
    constexpr int ScreenshotButtonId = 2007;
    constexpr int PointerSpeedButtonId = 2008;
    inline constexpr std::array ChromeButtonIds{ CloseButtonId, PinButtonId, FullscreenButtonId,
        RestoreSizeButtonId, InputMappingButtonId, PointerSpeedButtonId, ScreenshotButtonId };
    constexpr int ChromeButtonCount = static_cast<int>(ChromeButtonIds.size());
    constexpr int ChromeButtonRadius = 14;
    constexpr int FullscreenChromeRevealBorder = 10;
    inline LONG ChromeButtonSpacing(LONG width) noexcept
    { return std::clamp<LONG>((width - 40) / (ChromeButtonCount - 1), 20, 36); }
    inline LONG ChromeRadius(LONG width) noexcept { return std::min<LONG>(ChromeButtonRadius, ChromeButtonSpacing(width) / 2 - 1); }
    inline POINT ChromeButtonCenter(int index, RECT client) noexcept
    { return { client.right - 20 - index * ChromeButtonSpacing(client.right - client.left), client.top + 20 }; }
    inline POINT ChromeButtonCenterFor(int button, RECT client) noexcept
    {
        auto found = std::find(ChromeButtonIds.begin(), ChromeButtonIds.end(), button);
        return found == ChromeButtonIds.end() ? POINT{} :
            ChromeButtonCenter(static_cast<int>(found - ChromeButtonIds.begin()), client);
    }
    constexpr int FullscreenChromeRevealControlsHeight = 40;

    inline int HitTestChromeButton(POINT point, RECT client) noexcept
    {
        auto squaredDistance = [](POINT value, POINT center)
        {
            auto dx = static_cast<int64_t>(value.x) - center.x;
            auto dy = static_cast<int64_t>(value.y) - center.y;
            return dx * dx + dy * dy;
        };
        auto radius = ChromeRadius(client.right - client.left);
        int64_t maximumDistance = radius * radius;
        for(int index=0; index<ChromeButtonCount; ++index)
            if(squaredDistance(point, ChromeButtonCenter(index,client)) <= maximumDistance) return ChromeButtonIds[index];
        return 0;
    }

    inline bool FullscreenChromeRevealAt(POINT point, RECT client) noexcept
    {
        if (!PtInRect(&client, point)) return false;
        bool border = point.x < client.left + FullscreenChromeRevealBorder ||
            point.y < client.top + FullscreenChromeRevealBorder ||
            point.x >= client.right - FullscreenChromeRevealBorder ||
            point.y >= client.bottom - FullscreenChromeRevealBorder;
        auto first = ChromeButtonCenter(ChromeButtonCount - 1, client);
        bool controls = point.x >= first.x - ChromeRadius(client.right - client.left) - 6 &&
            point.y < client.top + FullscreenChromeRevealControlsHeight;
        return border || controls;
    }

    inline wchar_t const* ChromeTooltipText(int button, bool mapping, bool fullscreen, bool topmost,
        bool pointerSpeedAdjusted = false) noexcept
    {
        switch (button)
        {
        case ScreenshotButtonId: return Localized(L"截图（复制并保存）", L"Screenshot (copy and save)");
        case PointerSpeedButtonId: return pointerSpeedAdjusted ?
            Localized(L"鼠标速度（点击恢复 1.00×）", L"Mouse speed (reset to 1.00x)") :
            Localized(L"鼠标速度", L"Mouse speed");
        case InputMappingButtonId: return mapping ? Localized(L"关闭鼠标映射", L"Disable mouse mapping") :
            Localized(L"开启鼠标映射", L"Enable mouse mapping");
        case RestoreSizeButtonId: return Localized(L"恢复初始尺寸", L"Restore initial size");
        case FullscreenButtonId: return fullscreen ? Localized(L"退出全屏（Esc）", L"Exit full screen (Esc)") :
            Localized(L"全屏", L"Full screen");
        case PinButtonId: return fullscreen ? Localized(L"全屏期间保持置顶", L"Always on top in full screen") :
            (topmost ? Localized(L"取消置顶", L"Disable always on top") : Localized(L"置顶", L"Always on top"));
        case CloseButtonId: return Localized(L"关闭实时区域", L"Close live region");
        default: return L"";
        }
    }

    // Tooltip stays below the controls and within the lens, so it cannot
    // obscure the pointer or unrelated desktop content. All units are physical
    // pixels; font measurement/padding are scaled by the window's actual DPI.
    inline RECT ChromeTooltipBounds(RECT lens, SIZE desired, LONG gap) noexcept
    {
        auto availableWidth = std::max<LONG>(0, lens.right - lens.left - 2 * gap);
        auto availableHeight = std::max<LONG>(0, lens.bottom - lens.top - 40 - 2 * gap);
        auto width = std::clamp<LONG>(desired.cx, 0, availableWidth);
        auto height = std::clamp<LONG>(desired.cy, 0, availableHeight);
        auto right = lens.right - gap;
        auto top = lens.top + 40 + gap;
        return { right - width, top, right, top + height };
    }

    inline bool ChromeAcceptsNativePointer(bool passThrough, bool geometryChanging, bool chromeVisible) noexcept
    {
        return !passThrough && !geometryChanging && chromeVisible;
    }

    inline bool ChromeUsesSoftwareCursor(bool mappingEnabled, bool acceptsNativePointer, int button) noexcept
    {
        return mappingEnabled && acceptsNativePointer && button != 0;
    }
}
