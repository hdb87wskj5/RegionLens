#pragma once
#include <algorithm>
#include <cstdint>
#include <utility>

namespace RegionLens::native
{
    struct RenderExtent
    {
        uint32_t width{}, height{};
        bool Valid() const noexcept { return width && height; }
        bool operator==(RenderExtent const&) const = default;
    };

    inline bool FitsCompositionStorage(RenderExtent viewport, RenderExtent storage) noexcept
    { return viewport.Valid() && viewport.width <= storage.width && viewport.height <= storage.height; }

    inline RenderExtent ReserveCompositionStorage(RenderExtent viewport, RenderExtent display,
        RenderExtent previous = {}) noexcept
    {
        return { std::max({viewport.width, display.width, previous.width}),
            std::max({viewport.height, display.height, previous.height}) };
    }

    // Only the buffer resolution is held; frame acquisition, drawing and
    // presentation continue normally. HWND selection overlays never opt in.
    class LiveResizePolicy
    {
    public:
        bool Begin(bool composition) noexcept
        {
            if (!composition || m_holding) return false;
            m_holding = true; return true;
        }
        bool End() noexcept { return std::exchange(m_holding, false); }
        bool Holding() const noexcept { return m_holding; }
        RenderExtent BufferTarget(RenderExtent client, RenderExtent current) const noexcept
        {
            if (!client.Valid() || (m_holding && current.Valid())) return current;
            return client;
        }
    private:
        bool m_holding{};
    };

    // Tracks which buffer is actually attached to DirectComposition. A newly
    // sized buffer is rendered and presented first; only then may the visual
    // switch away from the old, continuously stretched frame.
    class CompositionResizeHandoff
    {
    public:
        void Initialize(RenderExtent displayed) noexcept
        {
            m_displayed = displayed;
            m_prepared = displayed;
            m_pending = false;
        }
        bool Prepare(RenderExtent replacement) noexcept
        {
            if (!replacement.Valid() || !m_displayed.Valid()) return false;
            m_prepared = replacement;
            // A new resource must be published even when its dimensions match
            // the previously displayed resource (e.g. rapid resize reversal).
            m_pending = true;
            return m_pending;
        }
        void Commit() noexcept
        {
            if (!m_pending) return;
            m_displayed = m_prepared;
            m_pending = false;
        }
        [[nodiscard]] bool Pending() const noexcept { return m_pending; }
        [[nodiscard]] RenderExtent Displayed() const noexcept { return m_displayed; }
        [[nodiscard]] RenderExtent Prepared() const noexcept { return m_prepared; }

    private:
        RenderExtent m_displayed{};
        RenderExtent m_prepared{};
        bool m_pending{};
    };

    // A bounded gate for the pre-fullscreen presentation. The first busy GPU
    // result waits one nominal display interval; a second busy result cancels
    // the request rather than falling back to an already-visible stretched
    // frame. Kept independent of HWND/D3D so every state edge is testable.
    class FullscreenTransitionGate
    {
    public:
        static constexpr uint64_t RetryDelayMilliseconds = 16;

        void Begin() noexcept
        {
            m_active = true;
            m_busyCount = 0;
            m_retryAt = 0;
        }
        void Finish() noexcept
        {
            m_active = false;
            m_retryAt = 0;
        }
        [[nodiscard]] bool Active() const noexcept { return m_active; }
        [[nodiscard]] bool Ready(uint64_t now) const noexcept
        { return m_active && (!m_retryAt || now >= m_retryAt); }
        // true means retry later; false means the bounded retry was exhausted.
        bool OnBusy(uint64_t now) noexcept
        {
            if (!m_active) return false;
            if (m_busyCount++ == 0) {
                m_retryAt = now + RetryDelayMilliseconds;
                return true;
            }
            // The owner must run its cancellation cleanup before ending the gate.
            // Ending here caused CancelFullscreenTransition to skip input restore.
            return false;
        }
        [[nodiscard]] uint32_t BusyCount() const noexcept { return m_busyCount; }
        [[nodiscard]] uint64_t RetryAt() const noexcept { return m_retryAt; }

    private:
        bool m_active{};
        uint32_t m_busyCount{};
        uint64_t m_retryAt{};
    };
}
