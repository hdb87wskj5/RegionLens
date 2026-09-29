#pragma once

#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <limits>

namespace RegionLens::native
{
    inline bool IsCornerSizingEdge(UINT edge) noexcept
    {
        return edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT ||
            edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
    }

    // Locks only a native LensWindow corner drag to the aspect ratio that was
    // visible when the drag began. SelectionOverlay never uses this policy.
    class LensResizePolicy
    {
    public:
        bool Begin(RECT bounds) noexcept
        {
            End();
            auto width = int64_t(bounds.right) - bounds.left;
            auto height = int64_t(bounds.bottom) - bounds.top;
            if (width <= 0 || height <= 0 || width > std::numeric_limits<LONG>::max() ||
                height > std::numeric_limits<LONG>::max()) return false;
            m_width = width;
            m_height = height;
            return true;
        }

        void End() noexcept { m_width = m_height = 0; }
        [[nodiscard]] bool Active() const noexcept { return m_width > 0 && m_height > 0; }

        bool Adjust(UINT edge, RECT& proposed, LONG minimumWidth, LONG minimumHeight) const noexcept
        {
            if (!Active() || !IsCornerSizingEdge(edge)) return false;
            auto candidateWidth = std::clamp<int64_t>(int64_t(proposed.right) - proposed.left,
                1, std::numeric_limits<LONG>::max());
            auto candidateHeight = std::clamp<int64_t>(int64_t(proposed.bottom) - proposed.top,
                1, std::numeric_limits<LONG>::max());

            auto heightFromWidth = DivideRounded(candidateWidth * m_height, m_width);
            auto widthFromHeight = DivideRounded(candidateHeight * m_width, m_height);
            auto verticalCorrection = std::llabs(heightFromWidth - candidateHeight);
            auto horizontalCorrection = std::llabs(widthFromHeight - candidateWidth);

            int64_t width{}, height{};
            if (verticalCorrection <= horizontalCorrection)
            {
                width = std::max(candidateWidth, RequiredWidth(minimumWidth, minimumHeight));
                height = DivideRounded(width * m_height, m_width);
            }
            else
            {
                height = std::max(candidateHeight, RequiredHeight(minimumWidth, minimumHeight));
                width = DivideRounded(height * m_width, m_height);
            }

            width = std::max<int64_t>(width, minimumWidth);
            height = std::max<int64_t>(height, minimumHeight);
            // Width/height are supplied by the native sizing loop and are far
            // below LONG_MAX in practice. Clamp before anchor arithmetic so a
            // malformed synthetic message cannot overflow RECT coordinates.
            width = std::min<int64_t>(width, std::numeric_limits<LONG>::max());
            height = std::min<int64_t>(height, std::numeric_limits<LONG>::max());
            ApplyAnchored(edge, proposed, width, height);
            return true;
        }

    private:
        static int64_t DivideRounded(int64_t numerator, int64_t denominator) noexcept
        {
            return denominator > 0 ? (numerator + denominator / 2) / denominator : 0;
        }

        int64_t RequiredWidth(LONG minimumWidth, LONG minimumHeight) const noexcept
        {
            auto fromHeight = (std::max<LONG>(1, minimumHeight) * m_width + m_height - 1) / m_height;
            return std::max<int64_t>(std::max<LONG>(1, minimumWidth), fromHeight);
        }

        int64_t RequiredHeight(LONG minimumWidth, LONG minimumHeight) const noexcept
        {
            auto fromWidth = (std::max<LONG>(1, minimumWidth) * m_height + m_width - 1) / m_width;
            return std::max<int64_t>(std::max<LONG>(1, minimumHeight), fromWidth);
        }

        static void ApplyAnchored(UINT edge, RECT& rect, int64_t width, int64_t height) noexcept
        {
            if (edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                rect.left = Saturate(int64_t(rect.right) - width);
            else
                rect.right = Saturate(int64_t(rect.left) + width);

            if (edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                rect.top = Saturate(int64_t(rect.bottom) - height);
            else
                rect.bottom = Saturate(int64_t(rect.top) + height);
        }

        static LONG Saturate(int64_t value) noexcept
        {
            return static_cast<LONG>(std::clamp<int64_t>(value,
                std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::max()));
        }

        int64_t m_width{};
        int64_t m_height{};
    };
}
