#pragma once

#include <windows.h>
#include <cstdint>

namespace RegionLens::native
{
    struct PixelRect
    {
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};

        [[nodiscard]] int32_t Right() const noexcept { return x + width; }
        [[nodiscard]] int32_t Bottom() const noexcept { return y + height; }
        [[nodiscard]] bool Empty() const noexcept { return width <= 0 || height <= 0; }
    };

    struct LensDescriptor
    {
        uint64_t id{};
        HMONITOR monitor{};
        PixelRect source{};
        RECT windowBounds{};
        bool topmost{ true };
    };

    inline bool operator==(PixelRect const& left, PixelRect const& right) noexcept
    {
        return left.x == right.x && left.y == right.y && left.width == right.width && left.height == right.height;
    }
}
