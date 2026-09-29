#pragma once
#include <windows.h>
#include <array>
#include <cstdint>

namespace RegionLens::native
{
    struct CursorObservation
    {
        bool queried{};
        DWORD flags{}, error{};
        uintptr_t shape{};
        POINT point{};
        bool ReportedVisible() const noexcept
        {
            return queried && (flags & CURSOR_SHOWING) && !(flags & CURSOR_SUPPRESSED) && shape;
        }
    };

    // No native calls or timers: the input thread owns this policy and polls it
    // between dispatches. A new hide/restore invalidates every older check.
    class CursorRecoveryPolicy
    {
    public:
        struct Check { uint64_t epoch{}; unsigned step{}; bool show{}, last{}; };
        static constexpr std::array<uint64_t, 4> CheckTimes{ 32, 96, 256, 512 };
        void BeginHide() noexcept
        {
            ++m_epoch; m_hidden = true; m_checking = false; m_showSucceeded = false;
        }
        bool BeginRestore(uint64_t now) noexcept
        {
            if (!Touched()) return false;
            ++m_epoch; m_hidden = false; m_checking = true; m_started = now; m_step = 0;
            m_showSucceeded = false;
            return true;
        }
        // Guardian adopts an obligation, but never hides a cursor itself.
        void AdoptRestore(uint64_t now) noexcept
        {
            if (!Touched()) ++m_epoch;
            BeginRestore(now);
        }
        void ShowResult(uint64_t epoch, bool success) noexcept
        {
            if (epoch == m_epoch && !m_hidden) m_showSucceeded = success;
        }
        bool Due(uint64_t now) const noexcept
        {
            return m_checking && now >= m_started && now - m_started >= CheckTimes[m_step];
        }
        Check Next(uint64_t now, CursorObservation const& observation) noexcept
        {
            if (!Due(now)) return {};
            bool firstCheck = m_step == 0;
            // A delayed worker does not replay a burst of already-expired checks.
            while (m_step + 1 < CheckTimes.size() && now - m_started >= CheckTimes[m_step + 1]) ++m_step;
            Check check{ m_epoch, m_step + 1,
                firstCheck || !m_showSucceeded || !observation.ReportedVisible(), m_step + 1 == CheckTimes.size() };
            if (++m_step == CheckTimes.size()) m_checking = false;
            return check;
        }
        bool Current(Check const& check) const noexcept { return check.epoch && check.epoch == m_epoch && !m_hidden; }
        bool Touched() const noexcept { return m_epoch != 0; }
        bool Hidden() const noexcept { return m_hidden; }
        bool Checking() const noexcept { return m_checking; }
        bool ShowSucceeded() const noexcept { return m_showSucceeded; }
        bool Unfinished() const noexcept { return m_hidden || m_checking || (Touched() && !m_showSucceeded); }
        uint64_t Epoch() const noexcept { return m_epoch; }
    private:
        uint64_t m_epoch{}, m_started{};
        unsigned m_step{};
        bool m_hidden{}, m_checking{}, m_showSucceeded{};
    };

    // Separate from button/position obligations. CAS completion cannot discharge
    // a newer hide, even if an older guardian/readback finishes late.
    inline LONG64 CursorLease(uint64_t epoch) noexcept { return LONG64((epoch << 1) | 1); }
    inline bool CursorLeasePending(LONG64 lease) noexcept { return (lease & 1) != 0; }
    inline bool CompleteCursorLease(volatile LONG64* value, LONG64 expected) noexcept
    {
        return InterlockedCompareExchange64(value, expected & ~LONG64(1), expected) == expected;
    }
    inline bool GuardMayReleaseResources(bool hasProcess, DWORD waitResult) noexcept
    {
        return !hasProcess || waitResult == WAIT_OBJECT_0;
    }
    inline bool GuardNeedsCursorRecovery(LONG64 lease, LONG64 observedLease, bool force,
        bool checking, bool lastShowSucceeded) noexcept
    {
        return lease && (force || !observedLease || (!checking && !lastShowSucceeded) ||
            (CursorLeasePending(lease) && lease != observedLease));
    }
    inline bool GuardRecoveryComplete(bool inputActive, LONG64 lease, bool cursorUnfinished) noexcept
    {
        return !inputActive && !CursorLeasePending(lease) && !cursorUnfinished;
    }
}
