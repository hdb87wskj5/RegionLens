#include "TopmostPolicy.h"
#include "TopmostGuard.h"
#include "TopmostFastWake.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <vector>

using namespace RegionLens::native;

namespace
{
    int failures{};
    void Check(bool condition, std::string_view name)
    {
        if (!condition) { ++failures; std::cerr << "FAILED: " << name << '\n'; }
    }
    TopmostWindowInfo Lens(uintptr_t id, bool pinned = true)
    { return { id, { 100, 100, 500, 400 }, true, pinned, true, true, pinned }; }
    TopmostWindowInfo Popup(uintptr_t id = 90)
    { return { id, { 200, 350, 700, 450 }, true, true, false, false, false }; }


}

int RunTopmostTests()
{
    {
        TopmostGuard stableGuard;
        Check(!stableGuard.SetWeTypeDemotionExperiment(true) &&
            !stableGuard.WeTypeDemotionExperimentEnabled(),
            "Stable runtime cannot enable the foreign-window trial");
    }
    Check(!PlanTopmostMaintenance({}).count, "No lenses means no z-order changes");
    std::vector<TopmostWindowInfo> windows{ Lens(1), Popup() };
    Check(!PlanTopmostMaintenance(windows).count, "Candidate already below lens needs no mutation");
    Check(!ForeignWindowBlocksLens(windows, 90),
        "An already covered candidate does not fail the interception trial");
    Check(ShouldAttachWeTypeCompatibility(true, false, true, false, true, 1000, 1000) &&
        !ShouldAttachWeTypeCompatibility(true, false, true, false, true, 999, 1000) &&
        !ShouldAttachWeTypeCompatibility(true, true, true, false, true, 1000, 1000) &&
        !ShouldAttachWeTypeCompatibility(true, false, false, false, true, 1000, 1000) &&
        !ShouldAttachWeTypeCompatibility(true, false, true, true, true, 1000, 1000) &&
        !ShouldAttachWeTypeCompatibility(true, false, true, false, false, 1000, 1000) &&
        !ShouldAttachWeTypeCompatibility(false, false, true, false, true, 1000, 1000),
        "Long-running compatibility attaches only when enabled, ready and unpaused");
    Check(!FatalWeTypeAttachError(ERROR_NOT_FOUND) &&
        !FatalWeTypeAttachError(ERROR_INVALID_THREAD_ID) &&
        !FatalWeTypeAttachError(ERROR_INVALID_WINDOW_HANDLE) &&
        FatalWeTypeAttachError(ERROR_NOT_SUPPORTED) &&
        FatalWeTypeAttachError(ERROR_DUP_NAME) &&
        FatalWeTypeAttachError(ERROR_ACCESS_DENIED),
        "Missing candidates retry; incompatible or ambiguous renderers disable interception");
    std::swap(windows[0], windows[1]);
    auto plan = PlanTopmostMaintenance(windows);
    Check(plan.count == 1 && plan.bottomToTop[0] == 1 && plan.blocker == 90 && plan.blockedLens == 1,
        "Overlapping physical candidate above pinned lens triggers only lens promotion");
    Check(ForeignWindowBlocksLens(windows, 90) &&
        !PlanTopmostMaintenance(windows, true, false, 90).count,
        "Interception trial suppresses only the verified candidate's post-change repair");
    auto otherPopup = Popup(91);
    windows.insert(windows.begin(), otherPopup);
    Check(PlanTopmostMaintenance(windows, true, false, 90).blocker == 91,
        "Other overlapping applications retain ordinary topmost maintenance");
    windows.erase(windows.begin());
    Check(windows[0].bounds.bottom == 450 && windows[0].bounds.right == 700,
        "Candidate geometry including portion outside lens is untouched");
    windows[0].bounds = { 500, 100, 700, 300 };
    Check(!PlanTopmostMaintenance(windows).count, "Touching edges do not overlap");
    windows[0].bounds = { 1000, 1000, 1300, 1200 };
    Check(!PlanTopmostMaintenance(windows).count, "Unrelated popup on another area is ignored");
    windows[0] = Popup(); windows[0].visible = false;
    Check(!PlanTopmostMaintenance(windows).count, "Hidden minimized or cloaked candidate is ignored");
    windows[0] = Popup(); windows[0].bounds.right = windows[0].bounds.left;
    Check(!PlanTopmostMaintenance(windows).count, "Empty popup rectangle is ignored");
    windows[0] = Popup(); windows[0].topmost = false;
    Check(PlanTopmostMaintenance(windows).count == 1,
        "Actual occlusion is detected even if a special-band popup lacks the topmost bit");
    windows[0] = Popup(); windows[1] = Lens(1, false);
    Check(!PlanTopmostMaintenance(windows).count, "Unpin opts out of maintenance");
    windows[1] = Lens(1); windows[1].visible = false;
    Check(!PlanTopmostMaintenance(windows).count, "Hidden lens is never raised or shown");
    windows = { Lens(1) }; windows[0].topmost = false;
    Check(PlanTopmostMaintenance(windows).count == 1, "Lost topmost style is restored while still pinned");
    windows[0].ownProcess = false;
    Check(!PlanTopmostMaintenance(windows).count, "Foreign handle cannot become a promotion target");

    windows = { Popup(), Lens(1) };
    windows[0].bounds = { -900, -300, -500, -150 };
    windows[1].bounds = { -1000, -500, -600, -200 };
    Check(PlanTopmostMaintenance(windows).count == 1,
        "Negative-coordinate monitor intersection uses physical pixels");
    Check(PlanTopmostMaintenance(windows, false).deferred && !PlanTopmostMaintenance(windows, false).count,
        "Incomplete native snapshot is not applied or mistaken for clear readback");
    Check(PlanTopmostMaintenance(windows, true, true).deferred && !PlanTopmostMaintenance(windows, true, true).count,
        "Selection or modal UI suspends repair without claiming success");

    windows = { Popup(), Lens(1), Lens(2), Lens(3) };
    auto hint = Popup(99); hint.ownProcess = true;
    windows.insert(windows.begin(), hint);
    Check(!PlanTopmostMaintenance(windows).count && PlanTopmostMaintenance(windows).deferred,
        "Own fullscreen hint or dialog is never overtaken or mistaken for clear readback");
    Check(IsRegisteredLensHud(99, 99, 0, 1, 1, true) &&
        !IsRegisteredLensHud(99, 99, 0, 2, 1, true) &&
        !IsRegisteredLensHud(99, 0, 0, 1, 1, true) &&
        !IsRegisteredLensHud(99, 99, 0, 1, 1, false),
        "Only an exact owned and registered lens hint may bypass the modal-UI deferral");
    auto fullscreenWindows = windows;
    std::erase_if(fullscreenWindows, [](auto const& window) {
        return IsRegisteredLensHud(window.window, 99, 0, 1, 1, window.ownProcess);
    });
    Check(!PlanTopmostMaintenance(fullscreenWindows).deferred &&
        PlanTopmostMaintenance(fullscreenWindows).count == 3,
        "An owned fullscreen hint does not suspend WeType repair during its three-second display");
    auto unrelatedDialog = hint; unrelatedDialog.window = 98;
    fullscreenWindows.insert(fullscreenWindows.begin(), unrelatedDialog);
    Check(PlanTopmostMaintenance(fullscreenWindows).deferred,
        "An unrelated own dialog still blocks z-order repair");
    Check(IsRegisteredPassiveOverlay(77, 77, true) &&
        !IsRegisteredPassiveOverlay(77, 0, true) &&
        !IsRegisteredPassiveOverlay(78, 77, true) &&
        !IsRegisteredPassiveOverlay(77, 77, false),
        "Only the registered own software cursor is absent from the z-order snapshot");
    auto passiveOverlay = hint; passiveOverlay.window = 77;
    windows.insert(windows.begin(), passiveOverlay);
    std::erase_if(windows, [](auto const& window) {
        return IsRegisteredPassiveOverlay(window.window, 77, window.ownProcess);
    });
    Check(windows[0].window == hint.window,
        "Ignoring the cursor overlay retains the unrelated own dialog");
    Check(PlanTopmostMaintenance(windows).deferred,
        "Another own dialog still defers z-order maintenance while a passive overlay is registered");
    windows.erase(windows.begin());
    windows.insert(windows.begin() + 2, Lens(4, false));
    plan = PlanTopmostMaintenance(windows);
    Check(plan.count == 3 && plan.bottomToTop[0] == 3 && plan.bottomToTop[1] == 2 && plan.bottomToTop[2] == 1,
        "Multiple pinned lenses are promoted bottom to top without unpinned lens");
    // Fake desktop: execute the production plan on values only. No HWND APIs.
    for (size_t i = 0; i < plan.count; ++i)
    {
        auto target = std::find_if(windows.begin(), windows.end(),
            [&](auto const& window) { return window.window == plan.bottomToTop[i]; });
        Check(target != windows.end() && target->ownProcess && target->keepTopmost,
            "Every fake promotion targets only an owned pinned lens");
        if (target == windows.end()) continue;
        auto window = *target; windows.erase(target); windows.insert(windows.begin(), window);
    }
    Check(windows[0].window == 1 && windows[1].window == 2 && windows[2].window == 3 && windows[3].window == 90,
        "Final relative lens order is preserved, with original candidate underneath");
    Check(!PlanTopmostMaintenance(windows).count, "Repaired order does not trigger repeated promotion");
    windows = { Lens(1), Popup(), Lens(2), Lens(3) };
    plan = PlanTopmostMaintenance(windows);
    Check(plan.count == 3 && plan.bottomToTop[2] == 1,
        "Repairing a lower lens also preserves the lens already above the popup");
    windows = { Popup(), Lens(1), Lens(2), Lens(3) };
    windows[2].bounds = { 800, 100, 1100, 400 };
    windows[3].bounds = { -800, 100, -500, 400 };
    plan = PlanTopmostMaintenance(windows);
    Check(plan.count == 1 && plan.bottomToTop[0] == 1,
        "Lower non-overlapping lenses receive no unnecessary native position messages");
    windows = { Lens(1), Popup(), Lens(2), Lens(3) };
    windows[0].bounds = { 800, 100, 1100, 400 };
    windows[3].bounds = { -800, 100, -500, 400 };
    plan = PlanTopmostMaintenance(windows);
    Check(plan.count == 2 && plan.bottomToTop[0] == 2 && plan.bottomToTop[1] == 1,
        "Promotion prefix keeps an unrelated upper lens in front without touching lower lenses");
    windows = { Popup() };
    for (uintptr_t id = 1; id <= TopmostPlan::MaximumLenses; ++id) windows.push_back(Lens(id));
    Check(PlanTopmostMaintenance(windows).count == TopmostPlan::MaximumLenses, "All 16 lenses fit the bounded plan");
    windows.push_back(Lens(17));
    Check(!PlanTopmostMaintenance(windows).count, "Overflow rejects whole plan instead of reordering a subset");

    Check(ShouldMaintainLensTopmost(true, false) && ShouldMaintainLensTopmost(false, true) &&
        !ShouldMaintainLensTopmost(false, false) && !ShouldMaintainLensTopmost(true, true, true),
        "Pin fullscreen exit-fullscreen and closing preferences remain consistent");
    Check((TopmostPositionFlags & (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER)) ==
        (SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER) &&
        !(TopmostPositionFlags & (SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_HIDEWINDOW | SWP_NOZORDER)),
        "Promotion retains geometry focus visibility and owner order without suppressing z-order change");

    LensInteractionOrder interactionOrder;
    Check(interactionOrder.NeedsRaise(1), "The first interacted lens requests promotion");
    interactionOrder.MarkFront(1);
    Check(!interactionOrder.NeedsRaise(1) && interactionOrder.NeedsRaise(2),
        "Repeated clicks on one lens are coalesced while another lens can move in front");
    interactionOrder.MarkFront(2);
    interactionOrder.Remove(1);
    Check(interactionOrder.Front() == 2 && !interactionOrder.NeedsRaise(2),
        "Removing a background lens preserves the active RegionLens order");
    interactionOrder.Remove(2);
    Check(interactionOrder.Front() == 0 && interactionOrder.NeedsRaise(1),
        "Removing the front lens permits the next interaction to establish a new front");

    TopmostRateLimit rate;
    Check(rate.Inspect(0) && !rate.Inspect(1) && !rate.Inspect(49) && rate.Inspect(50),
        "Timer and frame paths share the same 50 ms inspection budget");
    Check(rate.Inspect(58, true) && !rate.Inspect(59, true) && rate.Inspect(66, true),
        "Window events bypass polling delay while respecting the 8 ms burst limit");
    Check(!rate.Inspect(67) && rate.Inspect(116), "Idle inspection returns to poll budget after events");
    TopmostRateLimit trackedRate;
    Check(trackedRate.Inspect(0) && !trackedRate.Inspect(7, false, true) &&
        trackedRate.Inspect(8, false, true) && trackedRate.Inspect(16, false, true),
        "Tracked overlapping WeType popup bypasses 50 ms delay without requiring WinEvents");
    Check(!trackedRate.Inspect(17, true, true) && trackedRate.Inspect(24, true, true),
        "Tracking and event callbacks share one bounded 8 ms inspection budget");
    Check(!trackedRate.Inspect(32) && !trackedRate.Inspect(73) && trackedRate.Inspect(74),
        "Candidate disappearance returns inspection to the ordinary idle interval");
    TopmostRateLimit fullscreenRate;
    Check(fullscreenRate.Inspect(0, false, true, true) &&
        !fullscreenRate.Inspect(3, false, true, true) &&
        fullscreenRate.Inspect(4, false, true, true) &&
        fullscreenRate.Attempt(4, 1, 90, true),
        "Overlapping fullscreen WeType candidate uses the bounded four-millisecond probe path");
    fullscreenRate.Complete(4, true, true);
    Check(fullscreenRate.RetryDelay(4) == 4 && !fullscreenRate.Attempt(7, 1, 90, true) &&
        fullscreenRate.Attempt(8, 1, 90, true),
        "Verified fullscreen repair may respond to the next candidate reassertion after four milliseconds");
    fullscreenRate.Complete(8, false, true);
    Check(!fullscreenRate.Inspect(12, false, true, true) &&
        fullscreenRate.Inspect(58, false, true, true),
        "A failed fullscreen repair still retains native backoff");
    TopmostRateLimit failedTracking;
    Check(failedTracking.Inspect(0, false, true) && failedTracking.Attempt(0, 1, 90),
        "Tracked first repair remains immediate");
    failedTracking.Complete(0, false);
    Check(!failedTracking.Inspect(8, false, true) && failedTracking.Inspect(50, false, true) &&
        !failedTracking.Inspect(58, false, true) && failedTracking.Inspect(100, false, true),
        "Unverified native repair retains low-frequency inspection during failure backoff");
    Check(rate.Attempt(0, 1, 90), "First repair is immediate");
    rate.Complete(0, false);
    Check(!rate.Attempt(99, 1, 90) && rate.Attempt(100, 1, 90), "Failed repair retries after 100 ms");
    rate.Complete(100, false);
    Check(!rate.Attempt(299, 1, 90) && rate.Attempt(300, 1, 90), "Second failed repair retries after 200 ms");
    rate.Complete(300, false);
    Check(!rate.Attempt(699, 1, 90) && rate.Attempt(700, 1, 90), "Third failed repair retries after 400 ms");
    rate.Complete(700, false);
    Check(!rate.Attempt(1499, 1, 90) && rate.Attempt(1500, 1, 90), "Fourth failed repair retries after 800 ms");
    rate.Complete(1500, false);
    Check(!rate.Attempt(2499, 1, 90) && rate.Attempt(2500, 1, 90), "Ineffective repairs still back off up to one second");
    rate.Complete(2500, true);
    Check(rate.RetryDelay(2500) == 8 && !rate.Attempt(2507, 1, 90) && rate.Attempt(2508, 1, 90),
        "Verified repair immediately resets accumulated failure backoff");
    rate.Complete(2508, false);
    Check(rate.Attempt(2550, 1, 91), "Different popup can be repaired without old-popup backoff");
    rate.Complete(2550, false);
    Check(rate.Attempt(6000, 1, 91), "Popup reused after quiet period is repaired immediately");
    Check(!rate.Attempt(6100, 0, 0), "No conflict never consumes mutation budget");

    TopmostRateLimit slowRepair;
    Check(slowRepair.Inspect(0, true) && slowRepair.Attempt(0, 1, 90), "Slow repair starts within shared event budget");
    slowRepair.Complete(20, true);
    Check(slowRepair.RetryDelay(20) == 0 && slowRepair.Inspect(21, true) && slowRepair.Attempt(21, 1, 90),
        "Event arriving during slow successful repair does not fall back to 50 ms poll");

    TopmostEventSignal signal;
    Check(!signal.Pending() && signal.Notify(1, EVENT_OBJECT_SHOW), "First structural event requests one wake");
    for (uint64_t i = 2; i <= 10000; ++i)
        Check(!signal.Notify(i, EVENT_OBJECT_REORDER), "Event storm coalesces instead of flooding message queue");
    auto batch = signal.Take(); // A frame can drain dirty state before the posted wake.
    Check(batch.count == 10000 && batch.firstTick == 1 && batch.lastEvent == EVENT_OBJECT_REORDER && !signal.Pending(),
        "Coalesced event batch preserves earliest arrival and last event type");
    Check(!signal.Notify(10001, EVENT_OBJECT_IME_CHANGE), "Event during repair reuses outstanding posted wake");
    signal.WakeHandled();
    Check(signal.Pending(), "Acknowledging an old wake does not erase an event that arrived during repair");
    batch = signal.Take();
    Check(batch.count == 1 && batch.firstTick == 10001, "Event during repair survives for follow-up inspection");
    Check(signal.Notify(10002, EVENT_OBJECT_LOCATIONCHANGE), "Drained signal can schedule another wake");
    signal.PostFailed();
    Check(signal.Pending() && signal.Notify(10003, EVENT_OBJECT_SHOW), "Failed PostMessage keeps dirty fallback and permits repost");
    signal.Discard();
    Check(!signal.Pending() && !signal.Notify(10004, EVENT_OBJECT_SHOW), "Suspending discards stale work without duplicating outstanding wake");
    signal.Reset();
    Check(!signal.Pending() && signal.Notify(10005, EVENT_OBJECT_SHOW), "Stop/reset clears coalescing state for clean restart");
    signal.Reset();
    Check(signal.Notify(11000, EVENT_OBJECT_SHOW, 90, OBJID_WINDOW, 0, 10999),
        "Structural metadata can be recorded without window queries in callback");
    Check(!signal.Notify(11002, EVENT_OBJECT_LOCATIONCHANGE, 91, OBJID_CLIENT, 0, 11001),
        "Recreated window metadata still coalesces into a single wake");
    batch = signal.Take();
    Check(batch.firstTick == 11000 && batch.count == 2 && batch.lastWindow == 91 &&
        batch.lastEvent == EVENT_OBJECT_LOCATIONCHANGE && batch.lastObject == OBJID_CLIENT &&
        batch.lastChild == 0 && batch.nativeTick == 11001,
        "Batch carries latest identity and native tick while retaining earliest queue arrival");

    TopmostTraceBudget traceBudget;
    for (unsigned i = 0; i < TopmostTraceBudget::DetailsPerSecond; ++i)
        Check(traceBudget.Detail(2000 + i), "Bounded structural details fit the current second");
    for (unsigned i = 0; i < 10000; ++i)
        Check(!traceBudget.Detail(2999), "Typing storms cannot exceed the detail quota");
    Check(traceBudget.Detail(3000), "Diagnostic detail quota resumes in the next second");

    constexpr LONG_PTR passive = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    Check(IsWeTypePopup(L"wetype.flutter.setting", passive) &&
        IsWeTypePopup(L"wetype.future.candidate", passive),
        "Observed passive WeType class and compatible class family enable targeted tracking");
    Check(!IsWeTypePopup(L"wetype.flutter.setting", 0x100) &&
        !IsWeTypePopup(L"wetype.flutter.setting", WS_EX_TOPMOST) &&
        !IsWeTypePopup(L"MSCTFIME UI", passive) && !IsWeTypePopup(L"SogouCandidate", passive),
        "WeType settings dialog and existing Microsoft/Sogou behavior do not enter fast tracking");
    windows = { Lens(1), Popup() };
    windows[1].weTypePopup = true;
    windows[1].process = 22320;
    Check(ShouldTrackWeTypePopup(windows[1], windows),
        "Visible overlapping passive WeType popup remains tracked even while behind the lens");
    Check(ShouldDemoteWeTypePopup(windows[1], windows),
        "An overlapping topmost WeType candidate is eligible for the opt-in trial");
    windows[1].topmost = false;
    Check(!ShouldDemoteWeTypePopup(windows[1], windows),
        "Already-demoted candidates are never repositioned again");
    windows[1].topmost = true;
    Check(!ShouldFastWakeForFullscreenWeType(windows[1], windows),
        "An ordinary overlapping lens keeps the low-frequency wake source");
    windows[0].fullscreen = true;
    Check(ShouldFastWakeForFullscreenWeType(windows[1], windows),
        "Only an overlapping full-screen lens activates the short wake period");
    windows[0].fullscreen = false;
    windows[1].visible = false;
    Check(!ShouldTrackWeTypePopup(windows[1], windows) &&
        !ShouldFastWakeForFullscreenWeType(windows[1], windows) &&
        !ShouldDemoteWeTypePopup(windows[1], windows),
        "Hidden popup immediately stops fast tracking and its short wake period");
    windows[1].visible = true;
    windows[1].bounds = { 500, 400, 700, 500 };
    windows[0].fullscreen = true;
    Check(!ShouldTrackWeTypePopup(windows[1], windows) &&
        !ShouldFastWakeForFullscreenWeType(windows[1], windows) &&
        !ShouldDemoteWeTypePopup(windows[1], windows),
        "Non-overlapping popup stops fast tracking and its short wake period");
    windows[1] = Popup(); windows[1].weTypePopup = true;
    windows[0].keepTopmost = false;
    Check(!ShouldTrackWeTypePopup(windows[1], windows) && !ShouldDemoteWeTypePopup(windows[1], windows),
        "Unpinned lens cannot authorize the foreign-window experiment");
    windows[0] = Lens(1); windows[0].visible = false;
    Check(!ShouldTrackWeTypePopup(windows[1], windows), "Closed or hidden lens cannot retain popup tracking");
    windows[0] = Lens(1); windows[1].ownProcess = true;
    Check(!ShouldTrackWeTypePopup(windows[1], windows) && !ShouldDemoteWeTypePopup(windows[1], windows),
        "Own popup never receives foreign-IME classification");
    windows[1].ownProcess = false; windows[1].weTypePopup = false;
    Check(!ShouldTrackWeTypePopup(windows[1], windows) && !ShouldDemoteWeTypePopup(windows[1], windows),
        "Generic topmost windows cannot enter the WeType experiment");

    // A message-only window exercises the production wake source without
    // touching desktop z-order, capture, keyboard or mouse input.
    constexpr UINT fastWakeMessage = WM_APP + 223;
    HWND wakeWindow = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(wakeWindow != nullptr, "Message-only fast-wake test window is available");
    if (wakeWindow)
    {
        auto takeWake = [&]() {
            MSG message{};
            return PeekMessageW(&message, wakeWindow, fastWakeMessage, fastWakeMessage, PM_REMOVE) != FALSE;
        };
        {
            TopmostFastWake wake(wakeWindow, fastWakeMessage);
            Check(wake.SetEnabled(true) && wake.Enabled(),
                "Full-screen candidate can arm the high-resolution wake source");
            auto deadline = GetTickCount64() + 500;
            bool received{};
            while (!received && GetTickCount64() < deadline)
            {
                received = takeWake();
                if (!received) Sleep(1);
            }
            Check(received, "Armed wake source posts to the UI message queue");
            Sleep(30);
            Check(!takeWake(), "Unacknowledged high-frequency ticks coalesce to one pending wake");
            wake.Acknowledge();
            Check(wake.SetEnabled(false) && !wake.Enabled(),
                "Leaving the full-screen overlap disarms the wake source");
            while (takeWake()) {}
            wake.Acknowledge();
            Sleep(30);
            Check(!takeWake(), "A disarmed wake source does not continue posting");
            Check(wake.SetEnabled(true), "A later overlap can rearm the same wake source");
            deadline = GetTickCount64() + 500;
            received = false;
            while (!received && GetTickCount64() < deadline)
            {
                received = takeWake();
                if (!received) Sleep(1);
            }
            Check(received, "Rearmed wake source resumes without a stale pending-message latch");
        }
        DestroyWindow(wakeWindow);
    }
    {
        TopmostFastWake invalidWake(reinterpret_cast<HWND>(uintptr_t{ 1 }), fastWakeMessage);
        if (invalidWake.SetEnabled(true))
        {
            auto deadline = GetTickCount64() + 500;
            while (!invalidWake.Error() && GetTickCount64() < deadline) Sleep(1);
        }
        Check(invalidWake.Error() != 0 && !invalidWake.Enabled() &&
            !invalidWake.SetEnabled(true),
            "Failed posting disables the fast wake source instead of leaving a hidden retry loop");
    }

    std::vector<uintptr_t> order{ 1, 98, 2, 90, 3 };
    auto previous = [&](uintptr_t id) {
        auto found = std::find(order.begin(), order.end(), id);
        return found == order.end() || found == order.begin() ? uintptr_t{} : *(found - 1);
    };
    std::array<uintptr_t, 2> targets{ 1, 2 };
    size_t budget = 256;
    Check(TopmostPredecessorsContain(90, targets, budget, previous) && budget == 253,
        "Production fast probe finds all overlapping lenses above candidate through unrelated windows");
    targets = { 1, 3 };
    Check(!TopmostPredecessorsContain(90, targets, budget, previous),
        "One overlapping lens below candidate forces a full validated snapshot");
    targets = { 1, 2 }; budget = 2;
    Check(!TopmostPredecessorsContain(90, targets, budget, previous) && budget == 0,
        "Insufficient total probe budget never claims known-good order");
    budget = 256;
    Check(!TopmostPredecessorsContain(91, targets, budget, previous),
        "Destroyed or recreated candidate handle forces full cache refresh");
    unsigned lookups{};
    budget = 7;
    Check(!TopmostPredecessorsContain(90, targets, budget, [&](uintptr_t id) { ++lookups; return id; }) &&
        lookups == 7 && budget == 0, "Changing or cyclic native chain stops after bounded lookups");
    Check(!TopmostPredecessorsContain(90, targets, budget, [&](uintptr_t id) { ++lookups; return id; }) &&
        lookups == 7, "Several tracked popups share the same exhausted budget without extra queries");

    Check(IsTopmostStructureEvent(EVENT_OBJECT_SHOW, OBJID_WINDOW, 0) &&
        IsTopmostStructureEvent(EVENT_OBJECT_HIDE, OBJID_WINDOW, 0) &&
        IsTopmostStructureEvent(EVENT_OBJECT_LOCATIONCHANGE, OBJID_WINDOW, 0) &&
        IsTopmostStructureEvent(EVENT_OBJECT_REORDER, OBJID_WINDOW, 0) &&
        IsTopmostStructureEvent(EVENT_OBJECT_REORDER, OBJID_CLIENT, 0) &&
        IsTopmostStructureEvent(EVENT_SYSTEM_FOREGROUND, OBJID_WINDOW, 0),
        "Window visibility position foreground and desktop reorder notifications are accepted");
    Check(IsTopmostStructureEvent(EVENT_OBJECT_IME_SHOW, OBJID_CLIENT, 0) &&
        IsTopmostStructureEvent(EVENT_OBJECT_IME_CHANGE, OBJID_CLIENT, 1) &&
        IsTopmostStructureEvent(EVENT_OBJECT_IME_HIDE, OBJID_WINDOW, 0),
        "IME visibility and geometry notifications need no candidate-content queries");
    Check(!IsTopmostStructureEvent(EVENT_OBJECT_LOCATIONCHANGE, OBJID_CARET, 0) &&
        !IsTopmostStructureEvent(EVENT_OBJECT_SHOW, OBJID_CURSOR, 0) &&
        !IsTopmostStructureEvent(EVENT_OBJECT_NAMECHANGE, OBJID_WINDOW, 0) &&
        !IsTopmostStructureEvent(EVENT_OBJECT_VALUECHANGE, OBJID_CLIENT, 0) &&
        !IsTopmostStructureEvent(EVENT_OBJECT_TEXTSELECTIONCHANGED, OBJID_CLIENT, 0) &&
        !IsTopmostStructureEvent(EVENT_OBJECT_SHOW, OBJID_WINDOW, 1),
        "Text caret cursor and unrelated child-control notifications are ignored");

    // Regression for the observed log: successful restores to the same lens and
    // blocker used to accumulate 100/200/400/800/1000 ms delays while typing.
    TopmostRateLimit typingRate;
    TopmostEventSignal typingEvents;
    for (uint64_t i = 0; i < 1000; ++i)
    {
        auto tick = 1000 + i * 12;
        Check(typingEvents.Notify(tick, EVENT_OBJECT_IME_CHANGE), "Candidate update wakes UI throughout continuous typing");
        typingEvents.WakeHandled();
        Check(typingRate.Inspect(tick, typingEvents.Pending()), "Continuous candidate events do not wait for 50 ms poll");
        typingEvents.Take();
        windows = { Popup(), Lens(1) };
        plan = PlanTopmostMaintenance(windows);
        Check(plan.count == 1 && typingRate.Attempt(tick, plan.blockedLens, plan.blocker),
            "Every successful candidate update may repair the same HWND again without long backoff");
        std::swap(windows[0], windows[1]); // Fake repair only; never touch desktop HWNDs.
        auto readback = PlanTopmostMaintenance(windows);
        typingRate.Complete(tick, !readback.deferred && readback.count == 0);
        Check(typingRate.RetryDelay(tick) == 8, "One thousand successful updates never accumulate one-second delay");
    }

    // Reproduction: the SAME candidate handle repeatedly overtakes the lens
    // without SHOW/REORDER/LOCATIONCHANGE. Run only on a value-based desktop.
    TopmostRateLimit silentRate;
    windows = { Lens(1), Popup() }; windows[1].weTypePopup = true;
    for (uint64_t i = 0; i < 1000; ++i)
    {
        auto tick = 20000 + i * 16;
        auto tracked = ShouldTrackWeTypePopup(windows[1], windows);
        std::swap(windows[0], windows[1]); // WeType reasserts; no WinEvent emitted.
        Check(silentRate.Inspect(tick, false, tracked), "Silent reassertion is inspected at next UI tick");
        plan = PlanTopmostMaintenance(windows);
        Check(plan.count == 1 && silentRate.Attempt(tick, plan.blockedLens, plan.blocker),
            "Silent same-handle contention remains repairable throughout continuous typing");
        std::swap(windows[0], windows[1]);
        silentRate.Complete(tick, !PlanTopmostMaintenance(windows).count);
        Check(!PlanTopmostMaintenance(windows).count, "Already repaired order never requests a redundant raise");
    }
    if (!failures) std::cout << "Topmost policy tests passed (fake desktop only).\n";
    return failures;
}
