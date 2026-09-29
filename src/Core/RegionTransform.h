#pragma once

#include "RegionTypes.h"
#include <algorithm>

namespace RegionLens::native
{
    struct UvRect
    {
        float left{};
        float top{};
        float width{};
        float height{};
    };

    inline PixelRect NormalizeRect(POINT first, POINT second) noexcept
    {
        PixelRect result;
        result.x = std::min(first.x, second.x);
        result.y = std::min(first.y, second.y);
        result.width = std::max(first.x, second.x) - result.x;
        result.height = std::max(first.y, second.y) - result.y;
        return result;
    }

    inline PixelRect ClampRect(PixelRect value, int32_t width, int32_t height) noexcept
    {
        auto left = std::clamp(value.x, 0, std::max(0, width));
        auto top = std::clamp(value.y, 0, std::max(0, height));
        auto right = std::clamp(value.Right(), left, std::max(left, width));
        auto bottom = std::clamp(value.Bottom(), top, std::max(top, height));
        return { left, top, right - left, bottom - top };
    }

    inline UvRect ToUv(PixelRect value, int32_t textureWidth, int32_t textureHeight) noexcept
    {
        if (textureWidth <= 0 || textureHeight <= 0)
        {
            return {};
        }
        return {
            static_cast<float>(value.x) / textureWidth,
            static_cast<float>(value.y) / textureHeight,
            static_cast<float>(value.width) / textureWidth,
            static_cast<float>(value.height) / textureHeight
        };
    }

    inline RECT ToScreenRect(PixelRect value, RECT coordinateSpace) noexcept
    {
        return {
            coordinateSpace.left + value.x,
            coordinateSpace.top + value.y,
            coordinateSpace.left + value.Right(),
            coordinateSpace.top + value.Bottom()
        };
    }

    // Largest centred rectangle that preserves the source aspect ratio.  All
    // arithmetic stays in physical pixels; an odd unused pixel is deliberately
    // left on the right or bottom so the top-left origin remains deterministic.
    inline RECT FitAspectRect(PixelRect source, RECT bounds) noexcept
    {
        auto availableWidth = std::max<LONG>(0, bounds.right - bounds.left);
        auto availableHeight = std::max<LONG>(0, bounds.bottom - bounds.top);
        if (source.Empty() || !availableWidth || !availableHeight) return {};

        LONG width = availableWidth;
        LONG height = availableHeight;
        auto sourceWidth = int64_t(source.width);
        auto sourceHeight = int64_t(source.height);
        if (int64_t(availableWidth) * sourceHeight <= int64_t(availableHeight) * sourceWidth)
            height = std::max<LONG>(1, LONG(int64_t(availableWidth) * sourceHeight / sourceWidth));
        else
            width = std::max<LONG>(1, LONG(int64_t(availableHeight) * sourceWidth / sourceHeight));

        auto left = bounds.left + (availableWidth - width) / 2;
        auto top = bounds.top + (availableHeight - height) / 2;
        return { left, top, left + width, top + height };
    }

    // Projects a target-client content rectangle back into the current client.
    // The projected frame can then be stretched by DirectComposition to the
    // target extent without ever exposing pixels in the future black bars.
    // Left/top round inward with ceil and right/bottom with floor. This keeps
    // the existing odd-pixel convention and deliberately prefers a sub-pixel
    // sliver of extra black over leaking stretched image content into a bar.
    inline PixelRect ProjectContentRect(
        RECT targetContent,
        int32_t targetWidth,
        int32_t targetHeight,
        int32_t currentWidth,
        int32_t currentHeight) noexcept
    {
        if (targetWidth <= 0 || targetHeight <= 0 || currentWidth <= 0 || currentHeight <= 0)
            return {};

        auto targetLeft = std::clamp<int64_t>(targetContent.left, 0, targetWidth);
        auto targetTop = std::clamp<int64_t>(targetContent.top, 0, targetHeight);
        auto targetRight = std::clamp<int64_t>(targetContent.right, targetLeft, targetWidth);
        auto targetBottom = std::clamp<int64_t>(targetContent.bottom, targetTop, targetHeight);
        if (targetRight <= targetLeft || targetBottom <= targetTop) return {};

        auto ceilScale = [](int64_t value, int64_t destination, int64_t source) noexcept {
            return (value * destination + source - 1) / source;
        };
        auto floorScale = [](int64_t value, int64_t destination, int64_t source) noexcept {
            return value * destination / source;
        };
        auto left = ceilScale(targetLeft, currentWidth, targetWidth);
        auto top = ceilScale(targetTop, currentHeight, targetHeight);
        auto right = floorScale(targetRight, currentWidth, targetWidth);
        auto bottom = floorScale(targetBottom, currentHeight, targetHeight);

        // Even an extreme one-pixel target axis must retain visible content.
        // Anchor the fallback at the projected centre while remaining bounded.
        if (right <= left) {
            auto centre = floorScale(targetLeft + targetRight, currentWidth, int64_t(targetWidth) * 2);
            left = std::clamp<int64_t>(centre, 0, currentWidth - 1);
            right = left + 1;
        }
        if (bottom <= top) {
            auto centre = floorScale(targetTop + targetBottom, currentHeight, int64_t(targetHeight) * 2);
            top = std::clamp<int64_t>(centre, 0, currentHeight - 1);
            bottom = top + 1;
        }
        return { int32_t(left), int32_t(top), int32_t(right - left), int32_t(bottom - top) };
    }

    inline bool IsPointInResizeBorder(POINT point, RECT bounds, int32_t borderWidth) noexcept
    {
        if (borderWidth <= 0 || !PtInRect(&bounds, point))
        {
            return false;
        }
        return point.x < bounds.left + borderWidth ||
            point.x >= bounds.right - borderWidth ||
            point.y < bounds.top + borderWidth ||
            point.y >= bounds.bottom - borderWidth;
    }
}
