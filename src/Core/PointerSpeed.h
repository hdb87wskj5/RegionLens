#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace RegionLens::native
{
    // Integer slider ticks avoid accumulating 0.05 rounding errors in settings.
    // This is an in-process, per-lens preference, not a system mouse setting.
    struct PointerSpeed
    {
        static constexpr int Minimum = 10, Maximum = 50, Default = 20;
        int tick{ Default };
        int Tick() const noexcept { return std::clamp(tick, Minimum, Maximum); }
        double Multiplier() const noexcept { return Tick() / 20.0; }
        bool Adjusted() const noexcept { return Tick() != Default; }
        bool operator==(PointerSpeed const&) const = default;
    };
    enum class PointerSpeedClick { OpenPanel, ClosePanel, ResetSpeed };
    inline PointerSpeedClick PointerSpeedButtonAction(PointerSpeed speed, bool panelOpen) noexcept
    {
        // Reset wins even while the slider is open. The second click on the
        // now-neutral/dim button can open the panel for a new adjustment.
        if (speed.Adjusted()) return PointerSpeedClick::ResetSpeed;
        return panelOpen ? PointerSpeedClick::ClosePanel : PointerSpeedClick::OpenPanel;
    }
    inline std::wstring PointerSpeedText(PointerSpeed speed)
    {
        auto hundredths = speed.Tick() * 5;
        return std::to_wstring(hundredths / 100) + L"." +
            (hundredths % 100 < 10 ? L"0" : L"") + std::to_wstring(hundredths % 100) + L"×";
    }
    inline PointerSpeed PointerSpeedAtTrackPosition(LONG x, LONG first, LONG last) noexcept
    {
        auto span = int64_t(last) - first;
        if (span <= 0) return {};
        auto offset = std::clamp(int64_t(x) - first, int64_t(0), span);
        auto steps = PointerSpeed::Maximum - PointerSpeed::Minimum;
        // Round to the nearest 0.05 tick; use integer arithmetic so boundaries
        // and half-step ties are deterministic at every DPI/control width.
        return { PointerSpeed::Minimum + int((offset * steps + span / 2) / span) };
    }
    inline RECT PointerSpeedPopupBounds(POINT anchor, SIZE desired, RECT work) noexcept
    {
        auto width = std::min(desired.cx, std::max(0L, work.right - work.left));
        auto height = std::min(desired.cy, std::max(0L, work.bottom - work.top));
        auto x = std::clamp(anchor.x - width / 2, work.left, work.right - width);
        auto y = std::clamp(anchor.y, work.top, work.bottom - height);
        return { x, y, x + width, y + height };
    }
}
