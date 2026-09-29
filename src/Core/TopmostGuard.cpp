#include "pch.h"
#include "AppRuntime.h"
#include "TopmostGuard.h"
#include <dwmapi.h>

namespace RegionLens::native
{
    thread_local TopmostGuard* TopmostGuard::s_eventOwner{};

    TopmostGuard::~TopmostGuard()
    {
        StopEvents();
    }

    bool TopmostGuard::SetPassiveOverlay(HWND window) noexcept
    {
        if (window)
        {
            DWORD process{};
            auto thread = GetWindowThreadProcessId(window, &process);
            if (!thread || process != GetCurrentProcessId() || thread != GetCurrentThreadId()) return false;
        }
        m_passiveOverlay = window;
        // A newly registered popup changes the interpretation of the next
        // snapshot, even when no external WinEvent is delivered for own UI.
        m_lastInspect = 0;
        return true;
    }

    bool TopmostGuard::SetWeTypeDemotionExperiment(bool enabled) noexcept
    {
        // The Stable binary shares this core, but cannot opt into foreign
        // window changes. The experiment is intentionally not persisted.
        if (!Runtime().identity || Runtime().identity->channel != AppChannel::Dev ||
            !Runtime().diagnostics)
            return false;
        if (enabled)
        {
            if (m_demoted.window) return false; // A previous restore is still pending.
            m_demoteEnabled = true;
            m_demoteDeadline = GetTickCount64() + 30000;
            m_demoteStopReason = WeTypeTrialStopReason::None;
            Record(DiagnosticEvent::WindowLayer, { 6, 1, 30000 }, true);
        }
        else
        {
            m_demoteEnabled = false;
            m_nextRestoreAttempt = 0;
            RestoreWeTypeCandidate(WeTypeTrialStopReason::None);
            Record(DiagnosticEvent::WindowLayer, { 6, 0 }, true);
        }
        return true;
    }

    bool TopmostGuard::StartEvents(HWND controller)
    {
        if (!controller || (s_eventOwner && s_eventOwner != this)) return false;
        if (m_controller) return std::all_of(m_hooks.begin(), m_hooks.end(), [](auto hook) { return hook != nullptr; });
        m_controller = controller;
        s_eventOwner = this;
        constexpr std::array<std::array<DWORD, 2>, 4> ranges{{
            { EVENT_OBJECT_SHOW, EVENT_OBJECT_REORDER },
            { EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE },
            { EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND },
            { EVENT_OBJECT_IME_SHOW, EVENT_OBJECT_IME_CHANGE }
        }};
        for (size_t i = 0; i < ranges.size(); ++i)
        {
            m_hooks[i] = SetWinEventHook(ranges[i][0], ranges[i][1], nullptr, WindowEvent,
                0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        }
        // Partial registration is safe: working hooks remain useful, polling
        // covers missing events. No retry loop or dependency on IME-specific DLLs.
        return std::all_of(m_hooks.begin(), m_hooks.end(), [](auto hook) { return hook != nullptr; });
    }

    void TopmostGuard::StopEvents()
    {
        m_demoteEnabled = false;
        m_nextRestoreAttempt = 0;
        RestoreWeTypeCandidate(WeTypeTrialStopReason::None);
        // Detach before unhooking: late callbacks cannot dereference an owner
        // being destroyed. Registration and unregistration use the same UI thread.
        if (s_eventOwner == this) s_eventOwner = nullptr;
        m_eventsEnabled = false;
        m_controller = nullptr;
        for (auto& hook : m_hooks)
        {
            if (hook) UnhookWinEvent(hook);
            hook = nullptr;
        }
        m_events.Reset();
        m_weTypeCount = 0;
        m_fastWakeNeeded = false;
        m_lastInspect = 0;
        m_passiveOverlay = nullptr;
        TraceSummary(GetTickCount64(), true);
    }

    void CALLBACK TopmostGuard::WindowEvent(HWINEVENTHOOK hook, DWORD event, HWND window,
        LONG object, LONG child, DWORD, DWORD eventTime)
    {
        auto self = s_eventOwner;
        if (!self || !self->m_eventsEnabled || !self->m_controller ||
            std::find(self->m_hooks.begin(), self->m_hooks.end(), hook) == self->m_hooks.end() ||
            !IsTopmostStructureEvent(event, object, child)) return;
        // Constant bounded work only. Do not enumerate/move windows, render,
        // allocate, or call back into UI code from a possibly reentrant WinEvent.
        if (self->m_events.Notify(GetTickCount64(), event, reinterpret_cast<uintptr_t>(window),
            object, child, eventTime) &&
            !PostMessageW(self->m_controller, WakeMessage, 0, 0)) self->m_events.PostFailed();
    }

    namespace
    {
        uint64_t RateClockMilliseconds() noexcept
        {
            static LARGE_INTEGER const frequency = [] {
                LARGE_INTEGER value{};
                QueryPerformanceFrequency(&value);
                return value;
            }();
            LARGE_INTEGER counter{};
            if (frequency.QuadPart <= 0 || !QueryPerformanceCounter(&counter)) return GetTickCount64();
            auto seconds = counter.QuadPart / frequency.QuadPart;
            auto remainder = counter.QuadPart % frequency.QuadPart;
            return uint64_t(seconds) * 1000 + uint64_t(remainder * 1000 / frequency.QuadPart);
        }

        bool IsWeTypeWindow(HWND window, LONG_PTR extendedStyle)
        {
            // Class/style metadata only. Never read a window title or IME text.
            constexpr auto passivePopup = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            if ((extendedStyle & passivePopup) != passivePopup) return false;
            wchar_t name[128]{};
            auto count = GetClassNameW(window, name, static_cast<int>(std::size(name)));
            return count > 0 && IsWeTypePopup({ name, static_cast<size_t>(count) }, extendedStyle);
        }

        bool ExactWeTypeCandidate(HWND window, DWORD process, DWORD* thread = nullptr) noexcept
        {
            DWORD actualProcess{};
            auto actualThread = GetWindowThreadProcessId(window, &actualProcess);
            if (!actualThread || actualProcess != process) return false;
            wchar_t name[128]{};
            if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
                wcscmp(name, L"wetype.flutter.setting")) return false;
            // Demoting an owned popup can alter its owner's z-order as well.
            if (GetWindow(window, GW_OWNER)) return false;
            constexpr auto passive = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & passive) != passive) return false;
            if (thread) *thread = actualThread;
            return true;
        }

        bool VerifiedWeTypeImage(DWORD processId, uint64_t& creationTime) noexcept
        {
            auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process) return false;
            wchar_t path[32768]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            FILETIME created{}, exited{}, kernel{}, user{};
            bool okay = QueryFullProcessImageNameW(process, 0, path, &length) &&
                GetProcessTimes(process, &created, &exited, &kernel, &user);
            CloseHandle(process);
            if (!okay) return false;
            constexpr wchar_t segment[] = L"\\Tencent\\WeType\\";
            constexpr wchar_t executable[] = L"\\wetype_renderer.exe";
            constexpr size_t segmentLength = std::size(segment) - 1;
            constexpr size_t executableLength = std::size(executable) - 1;
            if (length < segmentLength + executableLength ||
                _wcsicmp(path + length - executableLength, executable)) return false;
            bool inWeTypeDirectory{};
            for (size_t offset = 0; offset + segmentLength <= length; ++offset)
                if (!_wcsnicmp(path + offset, segment, segmentLength))
                { inWeTypeDirectory = true; break; }
            if (!inWeTypeDirectory) return false;
            creationTime = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            return true;
        }

        bool CandidateOverlapsLens(HWND candidate, std::span<TopmostLensTarget const> lenses) noexcept
        {
            RECT candidateBounds{};
            if (!IsWindowVisible(candidate) || !GetWindowRect(candidate, &candidateBounds)) return false;
            for (auto const& lens : lenses)
            {
                RECT lensBounds{};
                if (lens.keepTopmost && lens.window && IsWindowVisible(lens.window) &&
                    GetWindowRect(lens.window, &lensBounds) &&
                    TopmostOverlaps(candidateBounds, lensBounds)) return true;
            }
            return false;
        }

        bool ReadVisibleBounds(HWND window, RECT& bounds)
        {
            if (!IsWindowVisible(window) || IsIconic(window)) return false;
            DWORD cloaked{};
            if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked)
                return false;
            return GetWindowRect(window, &bounds) && bounds.left < bounds.right && bounds.top < bounds.bottom;
        }

        struct WindowSnapshot
        {
            std::array<TopmostWindowInfo, 512> windows{};
            size_t count{};
            bool complete{};

            std::span<TopmostWindowInfo const> Entries() const { return { windows.data(), count }; }
        };

        WindowSnapshot ReadWindowOrder(std::span<TopmostLensTarget const> lenses, HWND passiveOverlay)
        {
            WindowSnapshot result;
            // GetWindow explicitly traverses z-order. Bound the walk and reject
            // repeats/stale handles instead of trusting a changing desktop list.
            std::array<HWND, 2048> visited{};
            size_t visitedCount{};
            auto pid = GetCurrentProcessId();
            for (auto window = GetTopWindow(nullptr); window; window = GetWindow(window, GW_HWNDNEXT))
            {
                if (visitedCount == visited.size() || !IsWindow(window) ||
                    std::find(visited.begin(), visited.begin() + visitedCount, window) != visited.begin() + visitedCount)
                    return result;
                visited[visitedCount++] = window;
                if (!IsWindowVisible(window) || IsIconic(window)) continue;
                auto target = std::find_if(lenses.begin(), lenses.end(),
                    [window](auto const& lens) { return lens.window == window; });
                DWORD ownerPid{};
                GetWindowThreadProcessId(window, &ownerPid);
                if (IsRegisteredPassiveOverlay(reinterpret_cast<uintptr_t>(window),
                    reinterpret_cast<uintptr_t>(passiveOverlay), ownerPid == pid)) continue;
                if (ownerPid == pid)
                {
                    auto hud = std::find_if(lenses.begin(), lenses.end(), [window](auto const& lens) {
                        if (!lens.keepTopmost ||
                            (window != lens.hintWindow && window != lens.tooltipWindow)) return false;
                        return IsRegisteredLensHud(reinterpret_cast<uintptr_t>(window),
                            reinterpret_cast<uintptr_t>(lens.hintWindow),
                            reinterpret_cast<uintptr_t>(lens.tooltipWindow),
                            reinterpret_cast<uintptr_t>(GetWindow(window, GW_OWNER)),
                            reinterpret_cast<uintptr_t>(lens.window), true);
                    });
                    if (hud != lenses.end()) continue;
                }
                auto extendedStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
                bool topmost = (extendedStyle & WS_EX_TOPMOST) != 0;
                RECT bounds{};
                if (!ReadVisibleBounds(window, bounds)) continue;
                if (result.count == result.windows.size()) return result;
                result.windows[result.count++] = { reinterpret_cast<uintptr_t>(window), bounds,
                    true, topmost, ownerPid == pid, target != lenses.end(),
                    target != lenses.end() && target->keepTopmost, ownerPid,
                    ownerPid != pid && IsWeTypeWindow(window, extendedStyle),
                    target != lenses.end() && target->fullscreen };
            }
            result.complete = true;
            return result;
        }

        bool OwnUiIsBusy()
        {
            GUITHREADINFO info{ sizeof(info) };
            if (!GetGUIThreadInfo(GetCurrentThreadId(), &info)) return true;
            return info.hwndCapture || (info.flags &
                (GUI_INMENUMODE | GUI_POPUPMENUMODE | GUI_SYSTEMMENUMODE | GUI_INMOVESIZE));
        }
    }

    bool TopmostGuard::WeTypeOrderMayHaveChanged(std::span<TopmostLensTarget const> lenses) const
    {
        // Usually one popup and one short predecessor walk. Bound the TOTAL
        // walk, including cycles and a changing native order. Uncertainty falls
        // back to the existing full validated snapshot, never to a blind raise.
        size_t stepsRemaining = 256;
        for (size_t i = 0; i < m_weTypeCount; ++i)
        {
            auto const& popup = m_weTypePopups[i];
            DWORD process{};
            RECT bounds{};
            if (!GetWindowThreadProcessId(popup.window, &process) || process != popup.process ||
                !ReadVisibleBounds(popup.window, bounds) ||
                !IsWeTypeWindow(popup.window, GetWindowLongPtrW(popup.window, GWL_EXSTYLE))) return true;
            std::array<uintptr_t, TopmostPlan::MaximumLenses> above{};
            size_t count{};
            for (auto const& lens : lenses)
            {
                if (!lens.keepTopmost) continue;
                RECT lensBounds{};
                if (!ReadVisibleBounds(lens.window, lensBounds)) continue;
                if (!(GetWindowLongPtrW(lens.window, GWL_EXSTYLE) & WS_EX_TOPMOST)) return true;
                if (!TopmostOverlaps(bounds, lensBounds)) continue;
                if (count == above.size()) return true;
                above[count++] = reinterpret_cast<uintptr_t>(lens.window);
            }
            if (!count) return true; // Left the overlap; remove the fast-path hint.
            if (!TopmostPredecessorsContain(reinterpret_cast<uintptr_t>(popup.window),
                { above.data(), count }, stepsRemaining, [](uintptr_t window) {
                    return reinterpret_cast<uintptr_t>(GetWindow(reinterpret_cast<HWND>(window), GW_HWNDPREV));
                })) return true;
        }
        return false;
    }

    void TopmostGuard::Trace(std::array<int64_t, 8> values)
    {
        if (m_traceBudget.Detail(GetTickCount64())) Record(DiagnosticEvent::WindowLayer, values);
    }

    void TopmostGuard::TraceSummary(uint64_t now, bool force)
    {
        if ((!force && now < m_nextSummary) ||
            std::all_of(m_traceCounts.begin(), m_traceCounts.end(), [](auto count) { return count == 0; })) return;
        // Two bounded aggregate records per second, independent of detail quota.
        std::array<int64_t, 8> values{};
        std::copy(m_traceCounts.begin(), m_traceCounts.end(), values.begin() + 1);
        Record(DiagnosticEvent::WindowLayer, values);
        Record(DiagnosticEvent::WindowLayer, { 4, int64_t(m_maxEventDelay),
            int64_t(m_maxRepairTime), int64_t(m_weTypeCount), int64_t(m_maxTrackedGap) });
        m_traceCounts = {};
        m_maxEventDelay = m_maxRepairTime = m_maxTrackedGap = 0;
        m_nextSummary = now + 1000;
    }

    void TopmostGuard::RestoreWeTypeCandidate(WeTypeTrialStopReason reason) noexcept
    {
        if (!m_demoted.window) return;
        auto const lease = m_demoted;
        DWORD thread{};
        if (!ExactWeTypeCandidate(lease.window, lease.process, &thread) || thread != lease.thread)
        {
            // The HWND is gone or has been reused. Never change a new window.
            m_demoted = {};
            Record(DiagnosticEvent::WindowLayer, { 8, int64_t(reinterpret_cast<uintptr_t>(lease.window)),
                int64_t(WeTypeTrialStopReason::CandidateChanged) }, true);
            return;
        }
        uint64_t creation{};
        if (VerifiedWeTypeImage(lease.process, creation) && creation != lease.creationTime)
        {
            m_demoted = {};
            Record(DiagnosticEvent::WindowLayer, { 8, int64_t(reinterpret_cast<uintptr_t>(lease.window)),
                int64_t(WeTypeTrialStopReason::CandidateChanged) }, true);
            return;
        }
        if (GetWindowLongPtrW(lease.window, GWL_EXSTYLE) & WS_EX_TOPMOST)
        {
            m_demoted = {};
            Record(DiagnosticEvent::WindowLayer, { 8, int64_t(reinterpret_cast<uintptr_t>(lease.window)),
                int64_t(reason), 1 }, true);
            return;
        }
        auto now = GetTickCount64();
        if (now < m_nextRestoreAttempt) return;
        m_nextRestoreAttempt = now + 250;
        SetLastError(ERROR_SUCCESS);
        bool applied = SetWindowPos(lease.window, HWND_TOPMOST, 0, 0, 0, 0, TopmostPositionFlags) != FALSE;
        auto error = applied ? DWORD{} : GetLastError();
        bool restored = applied && (GetWindowLongPtrW(lease.window, GWL_EXSTYLE) & WS_EX_TOPMOST);
        auto detail = std::array<int64_t, 8>{ 8, int64_t(reinterpret_cast<uintptr_t>(lease.window)),
            int64_t(reason), restored ? 1 : 0, error };
        if (restored) Trace(detail);
        else Record(DiagnosticEvent::WindowLayer, detail, true);
        if (restored) m_demoted = {};
        else
        {
            m_demoteEnabled = false;
            m_demoteStopReason = WeTypeTrialStopReason::RestoreFailed;
        }
    }

    bool TopmostGuard::RefreshWeTypeExperiment(std::span<TopmostLensTarget const> lenses,
        std::span<TopmostWindowInfo const> windows, bool suspended) noexcept
    {
        if (!Runtime().identity || Runtime().identity->channel != AppChannel::Dev) return false;
        auto now = GetTickCount64();
        if (m_demoteEnabled && now >= m_demoteDeadline)
        {
            m_demoteEnabled = false;
            m_demoteStopReason = WeTypeTrialStopReason::Timeout;
            Record(DiagnosticEvent::WindowLayer, { 9, int64_t(WeTypeTrialStopReason::Timeout) }, true);
        }
        if (m_demoted.window)
        {
            DWORD thread{};
            if (!ExactWeTypeCandidate(m_demoted.window, m_demoted.process, &thread) ||
                thread != m_demoted.thread)
            {
                m_demoted = {};
                Record(DiagnosticEvent::WindowLayer, { 9, int64_t(WeTypeTrialStopReason::CandidateChanged) }, true);
            }
            else if (m_demoteEnabled && !suspended &&
                IsWindowVisible(m_demoted.window) &&
                (GetWindowLongPtrW(m_demoted.window, GWL_EXSTYLE) & WS_EX_TOPMOST))
            {
                // If WeType itself restores TOPMOST, stop rather than compete.
                m_demoted = {};
                m_demoteEnabled = false;
                m_demoteStopReason = WeTypeTrialStopReason::Reasserted;
                Record(DiagnosticEvent::WindowLayer, { 9, int64_t(WeTypeTrialStopReason::Reasserted) }, true);
            }
            else if (!m_demoteEnabled || suspended ||
                !CandidateOverlapsLens(m_demoted.window, lenses))
            {
                RestoreWeTypeCandidate(m_demoteStopReason);
                return true;
            }
        }
        if (!m_demoteEnabled || suspended || m_demoted.window || windows.empty()) return false;
        TopmostWindowInfo const* target{};
        for (auto const& popup : windows)
        {
            if (!ShouldDemoteWeTypePopup(popup, windows)) continue;
            if (target) return false; // Ambiguous candidate set: do not alter either.
            target = &popup;
        }
        if (!target) return false;
        auto candidate = reinterpret_cast<HWND>(target->window);
        DWORD thread{};
        uint64_t creation{};
        if (!ExactWeTypeCandidate(candidate, target->process, &thread) ||
            !VerifiedWeTypeImage(target->process, creation))
        {
            Trace({ 10, int64_t(target->window), target->process });
            return false;
        }
        SetLastError(ERROR_SUCCESS);
        bool applied = SetWindowPos(candidate, HWND_NOTOPMOST, 0, 0, 0, 0, TopmostPositionFlags) != FALSE;
        auto error = applied ? DWORD{} : GetLastError();
        bool demoted = applied && IsWindow(candidate) &&
            !(GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_TOPMOST);
        auto detail = std::array<int64_t, 8>{ 7, int64_t(target->window), target->process,
            demoted ? 1 : 0, error };
        if (demoted) Trace(detail);
        else Record(DiagnosticEvent::WindowLayer, detail, true);
        if (!demoted)
        {
            m_demoteEnabled = false;
            m_demoteStopReason = WeTypeTrialStopReason::DemoteFailed;
            return applied;
        }
        m_demoted = { candidate, target->process, thread, creation };
        return true;
    }

    void TopmostGuard::Refresh(std::span<TopmostLensTarget const> lenses, bool suspended, bool eventWake)
    {
        if (eventWake) m_events.WakeHandled();
        if (m_refreshing) return;
        auto now = GetTickCount64();
        RefreshWeTypeExperiment(lenses, {}, suspended);
        m_eventsEnabled = !suspended && std::any_of(lenses.begin(), lenses.end(),
            [](auto const& lens) { return lens.keepTopmost; });
        if (!m_eventsEnabled)
        {
            m_events.Discard();
            m_weTypeCount = 0;
            m_fastWakeNeeded = false;
            m_lastInspect = 0;
            TraceSummary(now, true);
            return;
        }
        TraceSummary(now);
        auto rateNow = RateClockMilliseconds();
        bool hasFullscreenLens = std::any_of(lenses.begin(), lenses.end(),
            [](auto const& lens) { return lens.keepTopmost && lens.fullscreen; });
        if (!hasFullscreenLens) m_fastWakeNeeded = false;
        if (!m_rateLimit.Inspect(rateNow, m_events.Pending(), m_weTypeCount != 0,
            m_fastWakeNeeded)) return;
        if (OwnUiIsBusy()) { ++m_traceCounts[6]; return; }
        if (m_weTypeCount && m_lastInspect) m_maxTrackedGap = std::max(m_maxTrackedGap, rateNow - m_lastInspect);
        m_lastInspect = rateNow;
        m_refreshing = true;
        struct ResetFlag { bool& flag; ~ResetFlag() { flag = false; } } reset{ m_refreshing };

        auto events = m_events.Take();
        if (events.count)
        {
            m_traceCounts[0] += events.count;
            auto queueDelay = now - events.firstTick;
            auto nativeAge = events.nativeTick ? DWORD(now) - events.nativeTick : 0;
            m_maxEventDelay = std::max(m_maxEventDelay, std::max(queueDelay, uint64_t(nativeAge)));
            Trace({ 1, int64_t(events.lastWindow), events.lastEvent, events.lastObject,
                events.lastChild, events.nativeTick, nativeAge, int64_t(queueDelay) });
        }
        if (!events.count && m_weTypeCount && now < m_nextFullInspect)
        {
            ++m_traceCounts[2];
            if (!WeTypeOrderMayHaveChanged(lenses)) return;
        }

        // Preserve the original 50 ms full-desktop fallback for other popups,
        // even while a cached WeType candidate is stable below the lenses.
        m_nextFullInspect = now + TopmostRateLimit::InspectIntervalMs;
        ++m_traceCounts[1];
        auto snapshot = ReadWindowOrder(lenses, m_passiveOverlay);
        if (snapshot.complete && RefreshWeTypeExperiment(lenses, snapshot.Entries(), false))
            snapshot = ReadWindowOrder(lenses, m_passiveOverlay);
        auto oldPopups = m_weTypePopups;
        auto oldCount = m_weTypeCount;
        m_weTypeCount = 0;
        m_fastWakeNeeded = false;
        if (snapshot.complete)
        {
            for (auto const& window : snapshot.Entries())
            {
                if (m_weTypeCount == m_weTypePopups.size()) break;
                if (ShouldTrackWeTypePopup(window, snapshot.Entries()))
                {
                    m_weTypePopups[m_weTypeCount++] = { reinterpret_cast<HWND>(window.window), window.process };
                    m_fastWakeNeeded |= ShouldFastWakeForFullscreenWeType(window, snapshot.Entries());
                }
            }
        }
        if (oldCount != m_weTypeCount || !std::equal(oldPopups.begin(), oldPopups.begin() + oldCount,
            m_weTypePopups.begin(), [](auto const& a, auto const& b) {
                return a.window == b.window && a.process == b.process;
            }))
            Trace({ 3, int64_t(m_weTypeCount), m_weTypeCount ? int64_t(reinterpret_cast<uintptr_t>(m_weTypePopups[0].window)) : 0,
                m_weTypeCount ? m_weTypePopups[0].process : 0 });
        auto probe = Runtime().weTypeProbe;
        auto ignoredCandidate = probe ?
            reinterpret_cast<uintptr_t>(probe->InterceptionTarget()) : uintptr_t{};
        auto unfilteredPlan = PlanTopmostMaintenance(snapshot.Entries(), snapshot.complete);
        // A verified rise during the trial means the pre-change path did not
        // prevent this transition. Stop the trial and immediately resume the
        // ordinary recovery policy instead of hiding the failure.
        if (ignoredCandidate && snapshot.complete &&
            ForeignWindowBlocksLens(snapshot.Entries(), ignoredCandidate))
        {
            probe->ReportUnexpectedOrder(reinterpret_cast<HWND>(ignoredCandidate));
            ignoredCandidate = 0;
        }
        auto plan = ignoredCandidate ?
            PlanTopmostMaintenance(snapshot.Entries(), snapshot.complete, false, ignoredCandidate) :
            unfilteredPlan;
        if (plan.deferred) { ++m_traceCounts[6]; return; }
        if (!plan.count) return;
        auto blocker = std::find_if(snapshot.Entries().begin(), snapshot.Entries().end(),
            [&](auto const& window) { return window.window == plan.blocker; });
        auto blockedLens = std::find_if(lenses.begin(), lenses.end(), [&](auto const& lens) {
            return reinterpret_cast<uintptr_t>(lens.window) == plan.blockedLens;
        });
        bool fullscreenWeType = blockedLens != lenses.end() && blockedLens->fullscreen &&
            blocker != snapshot.Entries().end() && blocker->weTypePopup;
        if (!m_rateLimit.Attempt(rateNow, plan.blockedLens, plan.blocker, fullscreenWeType))
        { ++m_traceCounts[6]; return; }
        ++m_traceCounts[3];

        // Revalidate every target against the owned set before submitting one
        // batch. Ordinary maintenance never alters the IME/source window; only
        // the separately gated Dev trial above can temporarily demote it.
        DWORD error{};
        for (size_t i = 0; i < plan.count; ++i)
        {
            auto window = reinterpret_cast<HWND>(plan.bottomToTop[i]);
            auto target = std::find_if(lenses.begin(), lenses.end(),
                [window](auto const& lens) { return lens.window == window && lens.keepTopmost; });
            DWORD pid{};
            auto thread = GetWindowThreadProcessId(window, &pid);
            if (target == lenses.end() || pid != GetCurrentProcessId() || thread != GetCurrentThreadId())
            {
                // Own windows cannot disappear concurrently on this UI thread.
                error = ERROR_INVALID_WINDOW_HANDLE;
                break;
            }
        }
        auto batch = error ? nullptr : BeginDeferWindowPos(static_cast<int>(plan.count));
        if (!batch && !error) error = GetLastError();
        for (size_t i = 0; batch && i < plan.count; ++i)
        {
            auto window = reinterpret_cast<HWND>(plan.bottomToTop[i]);
            batch = DeferWindowPos(batch, window, HWND_TOPMOST, 0, 0, 0, 0, TopmostPositionFlags);
            if (!batch) error = GetLastError();
        }
        bool applied = batch && !error && EndDeferWindowPos(batch);
        if (!applied && !error) error = GetLastError();

        auto after = ReadWindowOrder(lenses, m_passiveOverlay);
        auto remaining = PlanTopmostMaintenance(after.Entries(), after.complete, false,
            ignoredCandidate);
        bool verified = applied && after.complete && !remaining.deferred && !remaining.count;
        auto finished = RateClockMilliseconds();
        m_rateLimit.Complete(finished, verified, fullscreenWeType);
        ++m_traceCounts[verified ? 4 : 5];
        m_maxRepairTime = std::max(m_maxRepairTime, finished - rateNow);
        std::array<int64_t, 8> repairRecord{ 2, int64_t(plan.blockedLens), int64_t(plan.blocker),
            blocker != snapshot.Entries().end() ? blocker->process : 0,
            blocker != snapshot.Entries().end() && blocker->weTypePopup ? 1 : 0,
            int64_t(plan.count), verified ? 1 : 0, int64_t(finished - rateNow) };
        if (repairRecord[4] && Runtime().weTypeProbe && Runtime().weTypeProbe->Enabled())
            Record(DiagnosticEvent::WindowLayer, repairRecord, true);
        else Trace(repairRecord);
        if (!verified) Record(DiagnosticEvent::Fault, { error, int64_t(plan.count), int64_t(finished - rateNow) }, true);
    }
}
