#pragma once

#include "TopmostPolicy.h"

namespace RegionLens::native
{
    enum class WeTypeTrialStopReason : unsigned
    {
        None, Timeout, Reasserted, DemoteFailed, RestoreFailed, CandidateChanged
    };

    struct TopmostLensTarget
    {
        HWND window{};
        bool keepTopmost{};
        bool fullscreen{};
        HWND hintWindow{};
        HWND tooltipWindow{};
    };

    // UI-thread only. Out-of-context window events, never mouse/keyboard hooks,
    // injected DLLs, or access to mouse proxy state. The opt-in Dev trial is
    // the sole path allowed to reposition a verified foreign candidate.
    class TopmostGuard
    {
    public:
        static constexpr UINT WakeMessage = WM_APP + 9;
        ~TopmostGuard();
        bool StartEvents(HWND controller);
        void StopEvents();
        // UI-thread only. Registers exactly one owned software-cursor popup;
        // nullptr unregisters it. Other own UI keeps its existing protection.
        bool SetPassiveOverlay(HWND window) noexcept;
        // An opt-in, time-limited Dev experiment that temporarily demotes one
        // verified WeType candidate. Stable never permits this path.
        bool SetWeTypeDemotionExperiment(bool enabled) noexcept;
        [[nodiscard]] bool WeTypeDemotionExperimentEnabled() const noexcept { return m_demoteEnabled; }
        [[nodiscard]] bool WeTypeDemotionIdle() const noexcept { return !m_demoteEnabled && !m_demoted.window; }
        [[nodiscard]] WeTypeTrialStopReason TakeWeTypeDemotionStopReason() noexcept
        { auto reason = m_demoteStopReason; m_demoteStopReason = WeTypeTrialStopReason::None; return reason; }
        void Refresh(std::span<TopmostLensTarget const> lenses, bool suspended, bool eventWake = false);
        [[nodiscard]] bool FastWakeNeeded() const noexcept { return m_fastWakeNeeded; }

    private:
        struct TrackedPopup { HWND window{}; DWORD process{}; };
        bool WeTypeOrderMayHaveChanged(std::span<TopmostLensTarget const> lenses) const;
        void Trace(std::array<int64_t, 8> values);
        void TraceSummary(uint64_t now, bool force = false);
        void RestoreWeTypeCandidate(WeTypeTrialStopReason reason) noexcept;
        bool RefreshWeTypeExperiment(std::span<TopmostLensTarget const> lenses,
            std::span<TopmostWindowInfo const> windows, bool suspended) noexcept;
        static void CALLBACK WindowEvent(HWINEVENTHOOK hook, DWORD event, HWND window,
            LONG object, LONG child, DWORD eventThread, DWORD eventTime);
        static thread_local TopmostGuard* s_eventOwner;
        HWND m_controller{};
        HWND m_passiveOverlay{};
        std::array<HWINEVENTHOOK, 4> m_hooks{};
        TopmostEventSignal m_events;
        bool m_eventsEnabled{};
        bool m_fastWakeNeeded{};
        TopmostRateLimit m_rateLimit;
        bool m_refreshing{};
        std::array<TrackedPopup, 16> m_weTypePopups{};
        size_t m_weTypeCount{};
        TopmostTraceBudget m_traceBudget;
        // Events, full snapshots, fast probes, attempts, verified, unverified,
        // deferrals. UI-owned and submitted only to the optional async sink.
        std::array<int64_t, 7> m_traceCounts{};
        uint64_t m_nextSummary{}, m_maxEventDelay{}, m_maxRepairTime{}, m_nextFullInspect{};
        uint64_t m_lastInspect{}, m_maxTrackedGap{};
        struct DemotedCandidate
        {
            HWND window{};
            DWORD process{};
            DWORD thread{};
            uint64_t creationTime{};
        } m_demoted;
        bool m_demoteEnabled{};
        uint64_t m_demoteDeadline{};
        uint64_t m_nextRestoreAttempt{};
        WeTypeTrialStopReason m_demoteStopReason{ WeTypeTrialStopReason::None };
    };
}
