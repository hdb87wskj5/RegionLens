#pragma once
#include <cstdint>

namespace RegionLens::native
{
    // UI-thread state, independent of the user's enabled toggle. A modal resize
    // produces hundreds of geometry messages but only one pause and one resume.
    class MappingGeometryGate
    {
    public:
        enum class Action { Pause, Wait, Resume, Replace };
        Action Update(bool blocked) noexcept
        {
            if (blocked)
            {
                if (m_paused) return Action::Wait;
                m_paused = true; return Action::Pause;
            }
            if (m_paused) { m_paused = false; return Action::Resume; }
            return Action::Replace;
        }
        void Reset() noexcept { m_paused = false; }
        bool Paused() const noexcept { return m_paused; }
    private:
        bool m_paused{};
    };
    inline bool MappingGeometryCanSettle(uint64_t now, uint64_t changed,
        bool buttonDown, bool nativeMoveLoop, bool captured) noexcept
    {
        return now >= changed && now - changed >= 180 && !buttonDown && !nativeMoveLoop && !captured;
    }
}
