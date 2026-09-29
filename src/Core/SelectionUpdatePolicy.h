#pragma once

#include <algorithm>
#include <cstdint>

namespace RegionLens::native
{
    // Fast enough for high-refresh selection feedback, while the DXGI waitable
    // object remains the final authority on whether DWM can accept a new frame.
    constexpr uint64_t SelectionVisualIntervalMs = 16;

    inline bool SelectionVisualUpdateDue(uint64_t lastUpdate, uint64_t now) noexcept
    {
        return !lastUpdate || now - lastUpdate >= SelectionVisualIntervalMs;
    }

    inline uint32_t SelectionVisualUpdateDelay(uint64_t lastUpdate, uint64_t now) noexcept
    {
        if (SelectionVisualUpdateDue(lastUpdate, now)) return 1;
        return static_cast<uint32_t>(std::max<uint64_t>(1, SelectionVisualIntervalMs - (now - lastUpdate)));
    }
}
