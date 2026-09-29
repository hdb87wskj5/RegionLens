#pragma once
#include <windows.h>

namespace RegionLens::native
{
    inline bool CanBeginMappedMove(bool mappingEnabled, bool fullscreen,
        bool inputEngineConfirmedGesture, bool alreadyMoving) noexcept
    {
        // The low-level hook deliberately consumes the physical LEFT DOWN, so
        // GetAsyncKeyState is not a reliable second source of truth on the UI
        // thread. The input engine publishes this confirmation only after its
        // own physical-button state still says LEFT is held and all surfaces
        // have been restored.
        return mappingEnabled && !fullscreen && inputEngineConfirmedGesture && !alreadyMoving;
    }

    inline POINT MappedMoveOrigin(RECT original, POINT anchor, POINT current) noexcept
    {
        return { original.left + current.x - anchor.x,
                 original.top + current.y - anchor.y };
    }

    inline RECT InitialSizeAtCurrentOrigin(RECT current, SIZE initialSize) noexcept
    {
        return { current.left, current.top,
            current.left + initialSize.cx, current.top + initialSize.cy };
    }
}
