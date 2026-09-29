#pragma once
#include <windows.h>
#include <array>

namespace RegionLens::native
{
    // Bounded, deduplicated UI commands. Never repost a command from inside an
    // active Shell message loop; the lifecycle-idle notification drains it later.
    class DeferredWindowActions
    {
    public:
        struct Action { UINT message{}; WPARAM wParam{}; LPARAM lParam{}; };
        bool Push(UINT message, WPARAM wParam, LPARAM lParam) noexcept
        {
            for (size_t i = 0; i < m_count; ++i)
                if (m_items[i].message == message && m_items[i].wParam == wParam)
                { m_items[i].lParam = lParam; return true; }
            if (m_count == m_items.size()) return false;
            m_items[m_count++] = { message, wParam, lParam }; return true;
        }
        bool Pop(Action& action) noexcept
        {
            if (!m_count) return false;
            action = m_items[0];
            for (size_t i = 1; i < m_count; ++i) m_items[i - 1] = m_items[i];
            --m_count; return true;
        }
        bool Empty() const noexcept { return m_count == 0; }
        void Clear() noexcept { m_count = 0; }
    private:
        std::array<Action, 64> m_items{};
        size_t m_count{};
    };
}
