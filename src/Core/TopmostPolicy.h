#pragma once

#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace RegionLens::native
{
    // Physical desktop pixels; entries are ordered from front to back.
    // No input, window mutation, IME inspection, or capture APIs in this policy.
    struct TopmostWindowInfo
    {
        uintptr_t window{};
        RECT bounds{};
        bool visible{};
        bool topmost{};
        bool ownProcess{};
        bool lens{};
        bool keepTopmost{};
        DWORD process{};
        bool weTypePopup{};
        bool fullscreen{};
    };

    // A compatibility hint, never a security identity. Only passive WeType
    // popup classes get the short-lived fast polling path; settings windows
    // and other programs retain the existing policy.
    inline bool IsWeTypePopup(std::wstring_view name, LONG_PTR extendedStyle) noexcept
    {
        constexpr auto passivePopup = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        return name.starts_with(L"wetype.") && (extendedStyle & passivePopup) == passivePopup;
    }

    struct TopmostPlan
    {
        static constexpr size_t MaximumLenses = 16;
        std::array<uintptr_t, MaximumLenses> bottomToTop{};
        size_t count{};
        uintptr_t blockedLens{};
        uintptr_t blocker{};
        bool deferred{};
    };

    inline constexpr UINT TopmostPositionFlags =
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

    inline bool ShouldMaintainLensTopmost(bool pinned, bool fullscreen, bool closing = false) noexcept
    {
        return !closing && (pinned || fullscreen);
    }

    // Only a registered, owned software-cursor popup is absent from the
    // native z-order snapshot. Other own dialogs/hints still defer repairs.
    inline bool IsRegisteredPassiveOverlay(uintptr_t window, uintptr_t registered,
        bool ownProcess) noexcept
    {
        return registered != 0 && window == registered && ownProcess;
    }

    // A RegionLens-owned hint/tooltip is an explicitly registered HUD, not
    // an independent settings dialog. USER32 keeps an owned popup above its
    // owner even when the lens is promoted past a foreign IME popup.
    inline bool IsRegisteredLensHud(uintptr_t window, uintptr_t registeredHint,
        uintptr_t registeredTooltip, uintptr_t actualOwner, uintptr_t lensOwner,
        bool ownProcess) noexcept
    {
        return ownProcess && lensOwner && actualOwner == lensOwner &&
            ((registeredHint && window == registeredHint) ||
             (registeredTooltip && window == registeredTooltip));
    }

    class LensInteractionOrder
    {
    public:
        [[nodiscard]] bool NeedsRaise(uint64_t id) const noexcept
        {
            return id != 0 && id != m_front;
        }
        void MarkFront(uint64_t id) noexcept { m_front = id; }
        void Remove(uint64_t id) noexcept { if (m_front == id) m_front = 0; }
        void Clear() noexcept { m_front = 0; }
        [[nodiscard]] uint64_t Front() const noexcept { return m_front; }

    private:
        uint64_t m_front{};
    };

    inline bool TopmostVisible(TopmostWindowInfo const& window) noexcept
    {
        return window.window && window.visible &&
            window.bounds.left < window.bounds.right && window.bounds.top < window.bounds.bottom;
    }

    inline bool TopmostOverlaps(RECT a, RECT b) noexcept
    {
        return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
    }

    inline bool ForeignWindowBlocksLens(std::span<TopmostWindowInfo const> windows,
        uintptr_t foreignWindow) noexcept
    {
        for (size_t index = 0; index < windows.size(); ++index)
        {
            auto const& candidate = windows[index];
            if (candidate.window != foreignWindow || !TopmostVisible(candidate) ||
                candidate.ownProcess) continue;
            for (size_t below = index + 1; below < windows.size(); ++below)
            {
                auto const& lens = windows[below];
                if (TopmostVisible(lens) && lens.ownProcess && lens.lens &&
                    lens.keepTopmost && TopmostOverlaps(candidate.bounds, lens.bounds)) return true;
            }
        }
        return false;
    }

    inline bool ShouldTrackWeTypePopup(TopmostWindowInfo const& popup,
        std::span<TopmostWindowInfo const> windows) noexcept
    {
        if (!TopmostVisible(popup) || popup.ownProcess || !popup.weTypePopup) return false;
        return std::any_of(windows.begin(), windows.end(), [&](auto const& lens) {
            return TopmostVisible(lens) && lens.ownProcess && lens.lens && lens.keepTopmost &&
                TopmostOverlaps(popup.bounds, lens.bounds);
        });
    }

    // Dev-only trial eligibility. This policy does not authorize mutation by
    // itself: the native caller must also verify the exact class and image.
    inline bool ShouldDemoteWeTypePopup(TopmostWindowInfo const& popup,
        std::span<TopmostWindowInfo const> windows) noexcept
    {
        return popup.topmost && ShouldTrackWeTypePopup(popup, windows);
    }

    inline bool ShouldFastWakeForFullscreenWeType(TopmostWindowInfo const& popup,
        std::span<TopmostWindowInfo const> windows) noexcept
    {
        if (!TopmostVisible(popup) || popup.ownProcess || !popup.weTypePopup) return false;
        return std::any_of(windows.begin(), windows.end(), [&](auto const& lens) {
            return TopmostVisible(lens) && lens.ownProcess && lens.lens &&
                lens.keepTopmost && lens.fullscreen && TopmostOverlaps(popup.bounds, lens.bounds);
        });
    }

    inline bool ShouldAttachWeTypeCompatibility(bool enabled, bool attached,
        bool hasTargets, bool paused, bool legacyTrialIdle,
        uint64_t nowMs, uint64_t nextAttemptMs) noexcept
    {
        return enabled && !attached && hasTargets && !paused && legacyTrialIdle &&
            nowMs >= nextAttemptMs;
    }

    inline bool FatalWeTypeAttachError(DWORD error) noexcept
    {
        return error != ERROR_NOT_FOUND && error != ERROR_INVALID_THREAD_ID &&
            error != ERROR_INVALID_WINDOW_HANDLE;
    }

    // Shared by the native cache probe and fake-desktop tests. Each predecessor
    // lookup consumes the caller's TOTAL budget; cycles, missing handles and
    // incomplete walks fail closed to a fresh full snapshot, not a blind repair.
    template<class PreviousWindow>
    bool TopmostPredecessorsContain(uintptr_t window, std::span<uintptr_t const> targets,
        size_t& budget, PreviousWindow&& previous)
    {
        if (!window || targets.empty() || targets.size() > TopmostPlan::MaximumLenses) return false;
        std::array<uintptr_t, TopmostPlan::MaximumLenses> remaining{};
        auto count = targets.size();
        std::copy(targets.begin(), targets.end(), remaining.begin());
        while (count && budget)
        {
            --budget;
            window = previous(window);
            if (!window) return false;
            auto found = std::find(remaining.begin(), remaining.begin() + count, window);
            if (found != remaining.begin() + count) *found = remaining[--count];
        }
        return count == 0;
    }

    inline TopmostPlan PlanTopmostMaintenance(
        std::span<TopmostWindowInfo const> windows, bool complete = true, bool suspended = false,
        uintptr_t ignoredForeignWindow = 0) noexcept
    {
        TopmostPlan plan;
        if (!complete || suspended) { plan.deferred = true; return plan; }
        size_t repairPrefix{};
        for (size_t i = 0; i < windows.size(); ++i)
        {
            auto const& lens = windows[i];
            if (!TopmostVisible(lens)) continue;
            // Selection UI, menus and independent dialogs must not be overtaken.
            // Exact registered lens-owned HUDs were filtered from the snapshot.
            // An explicitly unpinned lens is not a transient dialog.
            if (lens.ownProcess && !lens.lens) { plan = {}; plan.deferred = true; return plan; }
            if (!lens.ownProcess || !lens.lens || !lens.keepTopmost) continue;
            if (plan.count == plan.bottomToTop.size()) { plan = {}; plan.deferred = true; return plan; }
            plan.bottomToTop[plan.count++] = lens.window;
            bool needsRepair = !lens.topmost;
            if (needsRepair && !plan.blockedLens) plan.blockedLens = lens.window;
            for (size_t above = 0; above < i; ++above)
            {
                auto const& popup = windows[above];
                if (popup.window != ignoredForeignWindow && TopmostVisible(popup) && !popup.ownProcess &&
                    TopmostOverlaps(popup.bounds, lens.bounds))
                {
                    needsRepair = true;
                    if (!plan.blocker) { plan.blockedLens = lens.window; plan.blocker = popup.window; }
                    break;
                }
            }
            if (needsRepair) repairPrefix = plan.count;
        }
        if (!plan.blockedLens) return {};
        // Only the prefix through the last obstructed lens needs promotion.
        // Lower unaffected windows need no native position messages. Promoting
        // the prefix bottom-first still preserves the user's inter-lens order.
        plan.count = repairPrefix;
        std::reverse(plan.bottomToTop.begin(), plan.bottomToTop.begin() + plan.count);
        return plan;
    }

    class TopmostRateLimit
    {
    public:
        static constexpr uint64_t InspectIntervalMs = 50;
        static constexpr uint64_t EventIntervalMs = 8;
        static constexpr uint64_t FullscreenWeTypeIntervalMs = 4;
        static constexpr UINT TimerIntervalMs = 16;

        bool Inspect(uint64_t now, bool eventPending = false, bool trackWeType = false,
            bool fullscreenWeType = false) noexcept
        {
            auto interval = fullscreenWeType ? FullscreenWeTypeIntervalMs : EventIntervalMs;
            // Silent polling must not turn a genuine native failure's retry
            // backoff into repeated high-frequency full-desktop snapshots.
            if (RetryDelay(now) > interval) trackWeType = false;
            if (now < m_nextEventInspect || (!eventPending && !trackWeType && now < m_nextInspect)) return false;
            m_nextEventInspect = now + interval;
            m_nextInspect = now + InspectIntervalMs;
            return true;
        }

        bool Attempt(uint64_t now, uintptr_t lens, uintptr_t blocker, bool fullscreenWeType = false) noexcept
        {
            if (!lens) return false;
            if (lens != m_lens || blocker != m_blocker || now - m_lastAttempt > 2000)
            {
                m_lens = lens;
                m_blocker = blocker;
                m_retries = 0;
                m_nextAttempt = 0;
            }
            if (now < m_nextAttempt) return false;
            m_lastAttempt = now;
            m_nextAttempt = now + (fullscreenWeType ? FullscreenWeTypeIntervalMs : EventIntervalMs);
            return true;
        }

        // Only failed/unverified repairs back off. Successfully restoring the
        // order does NOT penalize the next candidate update from the same HWND.
        void Complete(uint64_t now, bool verified, bool fullscreenWeType = false) noexcept
        {
            if (verified)
            {
                m_retries = 0;
                // Share the inspection-start budget. Adding 8 ms after a slow
                // native repair could consume the next event too early and leave
                // it waiting for the 50 ms polling path despite successful repair.
                m_nextAttempt = m_lastAttempt +
                    (fullscreenWeType ? FullscreenWeTypeIntervalMs : EventIntervalMs);
                return;
            }
            constexpr std::array<uint64_t, 5> delays{ 100, 200, 400, 800, 1000 };
            m_nextAttempt = now + delays[m_retries];
            if (m_retries + 1 < delays.size()) ++m_retries;
        }

        [[nodiscard]] uint64_t RetryDelay(uint64_t now) const noexcept
        { return m_nextAttempt > now ? m_nextAttempt - now : 0; }

    private:
        uint64_t m_nextInspect{};
        uint64_t m_nextEventInspect{};
        uint64_t m_nextAttempt{};
        uint64_t m_lastAttempt{};
        uintptr_t m_lens{};
        uintptr_t m_blocker{};
        size_t m_retries{};
    };

    // Listen only for window structure/IME visibility, never keyboard, text,
    // caret movement, or candidate contents. No desktop queries in this filter.
    inline bool IsTopmostStructureEvent(DWORD event, LONG object, LONG child) noexcept
    {
        if (event == EVENT_OBJECT_IME_SHOW || event == EVENT_OBJECT_IME_HIDE ||
            event == EVENT_OBJECT_IME_CHANGE) return true;
        if (child != 0) return false; // CHILDID_SELF
        if (event == EVENT_SYSTEM_FOREGROUND) return object == OBJID_WINDOW;
        if (event == EVENT_OBJECT_REORDER) return object == OBJID_WINDOW || object == OBJID_CLIENT;
        return object == OBJID_WINDOW && (event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_HIDE ||
            event == EVENT_OBJECT_LOCATIONCHANGE);
    }

    struct TopmostEventBatch
    {
        uint64_t firstTick{};
        DWORD lastEvent{};
        uint32_t count{};
        uintptr_t lastWindow{};
        LONG lastObject{}, lastChild{};
        DWORD nativeTick{};
    };

    // UI-thread only, including OUTOFCONTEXT callbacks. At most one posted wake;
    // events arriving during a repair remain dirty for the next inspection.
    class TopmostEventSignal
    {
    public:
        bool Notify(uint64_t now, DWORD event, uintptr_t window = 0,
            LONG object = 0, LONG child = 0, DWORD nativeTick = 0) noexcept
        {
            if (!Pending()) m_batch.firstTick = now;
            m_batch.lastEvent = event;
            m_batch.lastWindow = window;
            m_batch.lastObject = object;
            m_batch.lastChild = child;
            m_batch.nativeTick = nativeTick;
            if (m_batch.count < UINT32_MAX) ++m_batch.count;
            if (m_posted) return false;
            m_posted = true;
            return true;
        }
        void WakeHandled() noexcept { m_posted = false; }
        void PostFailed() noexcept { m_posted = false; } // Keep dirty for timer/frame fallback.
        [[nodiscard]] bool Pending() const noexcept { return m_batch.count != 0; }
        TopmostEventBatch Take() noexcept { auto batch = m_batch; m_batch = {}; return batch; }
        void Discard() noexcept { m_batch = {}; } // An already posted wake still needs its acknowledgement.
        void Reset() noexcept { m_batch = {}; m_posted = false; }

    private:
        TopmostEventBatch m_batch;
        bool m_posted{};
    };

    class TopmostTraceBudget
    {
    public:
        static constexpr uint32_t DetailsPerSecond = 16;
        bool Detail(uint64_t now) noexcept
        {
            if (now >= m_nextWindow) { m_nextWindow = now + 1000; m_used = 0; }
            if (m_used == DetailsPerSecond) return false;
            ++m_used; return true;
        }
    private:
        uint64_t m_nextWindow{};
        uint32_t m_used{};
    };
}
