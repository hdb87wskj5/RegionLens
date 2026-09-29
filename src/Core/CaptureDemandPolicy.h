#pragma once

#include <windows.h>
#include <cstddef>

namespace RegionLens::native
{
    // Keep a monitor capture only while a selection is being made on that
    // monitor or at least one live lens consumes its frames.
    inline bool CaptureIsNeeded(HMONITOR monitor, HMONITOR pendingSelection, size_t lensCount) noexcept
    {
        return monitor != nullptr && (monitor == pendingSelection || lensCount != 0);
    }
}
