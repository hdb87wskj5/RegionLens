#pragma once

#include <cstdint>

namespace RegionLens::native
{
    // Per-window, monotonic success pulse. A queued timer from an earlier
    // screenshot cannot end a newer pulse; no animation frame queue is needed.
    class ScreenshotFeedback
    {
    public:
        static constexpr uint32_t DurationMs = 400;
        void Start(uint64_t now) noexcept { m_started = now; m_running = true; }
        void Reset() noexcept { m_running = false; }
        bool Active(uint64_t now) const noexcept
        { return m_running && now - m_started < DurationMs; }
        bool Expire(uint64_t now) noexcept
        {
            if (!m_running || Active(now)) return false;
            Reset();
            return true;
        }
    private:
        uint64_t m_started{};
        bool m_running{};
    };
}
