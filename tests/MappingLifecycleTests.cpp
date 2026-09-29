#include "InputMappingCoordinator.h"
#include "AppRuntime.h"
#include "DeferredWindowActions.h"
#include "MappingStandbyPolicy.h"
#include "LensQuality.h"
#include <iostream>
#include <vector>

using namespace RegionLens::native;

namespace
{
    int failures{};
    void Check(bool value, char const* name)
    {
        if (!value) { ++failures; std::cerr << "FAILED mapping lifecycle: " << name << '\n'; }
    }
    MappingSessionConfig Config(uint64_t id = 1)
    {
        MappingSessionConfig config;
        config.lensId = id;
        config.source = { 100, 100, 300, 300 };
        config.destination = { 400, 200, 1000, 800 };
        config.desktop = { -1920, 0, 1920, 1080 };
        return config;
    }
    struct EngineState
    {
        int created{}, destroyed{}, starts{}, stops{}, drains{}, takes{}, depth{};
        int cursorBegins{}, cursorEnds{}, cursorHeartbeats{};
        bool cursorBeginSucceeded{ true }, cursorEndSucceeded{ true }, nativeShowConfirmed{ true };
        std::vector<char> cursorOrder;
        bool startSucceeded{ true }, drainSucceeded{ true }, healthy{ true };
        ProxyStartupFailure failure{};
        ProxyUiUpdate update{};
        std::vector<MappingSessionConfig> configurations;
        std::vector<std::pair<uint64_t, bool>> acknowledgements;
        std::function<void()> onStart, onStop, onDrain, onTake, onConfigure;
        void ClearCallbacks() { onStart = {}; onStop = {}; onDrain = {}; onTake = {}; onConfigure = {}; }
    };
    // No hook, cursor API, SendInput, watchdog process or desktop hit testing is
    // reachable through this backend. Reentry is injected on the UI side only.
    class FakeEngine final : public IInputMappingEngine
    {
        EngineState& s;
        struct Call
        {
            EngineState& state;
            explicit Call(EngineState& value) : state(value)
            { Check(state.depth++ == 0, "no overlapping engine methods"); }
            ~Call() { --state.depth; }
        };
    public:
        explicit FakeEngine(EngineState& state) : s(state) { ++s.created; }
        ~FakeEngine() override { Check(s.depth == 0, "engine is destroyed only after its call returns"); ++s.destroyed; }
        bool Start() override
        {
            Call call(s); ++s.starts;
            if (s.onStart) s.onStart();
            return s.startSucceeded;
        }
        void Stop() override { Call call(s); ++s.stops; s.cursorOrder.push_back('S'); if (s.onStop) s.onStop(); }
        void Configure(MappingSessionConfig config, bool probeCursor) override
        {
            Call call(s); Check(probeCursor, "a new route probes the current cursor");
            s.configurations.push_back(config); if (s.onConfigure) s.onConfigure();
        }
        bool DisableAndDrain() override
        { Call call(s); ++s.drains; if (s.onDrain) s.onDrain(); return s.drainSucceeded; }
        void Acknowledge(uint64_t number, bool success) override
        { Call call(s); s.acknowledgements.emplace_back(number, success); }
        ProxyUiUpdate TakeUiUpdate() override
        {
            Call call(s); ++s.takes; if (s.onTake) s.onTake();
            auto update = s.update; s.update.surfaces.reset(); return update;
        }
        bool BeginSoftwareCursor() override
        { Call call(s); ++s.cursorBegins; return s.cursorBeginSucceeded; }
        bool EndSoftwareCursor() override
        { Call call(s); ++s.cursorEnds; s.cursorOrder.push_back('E'); return s.cursorEndSucceeded; }
        void SoftwareCursorHeartbeat() noexcept override { ++s.cursorHeartbeats; }
        bool NativeCursorShowConfirmed() const noexcept override { return s.nativeShowConfirmed; }
        bool GuardHealthy() const noexcept override { return s.healthy; }
        ProxyStartupFailure LastFailure() const noexcept override { return s.failure; }
    };
    struct Fixture
    {
        EngineState engine;
        int notifications{}, hides{}, shows{}, cursors{}, externalMoves{};
        POINT externalPosition{};
        bool uiAccess{ true };
        std::function<void(bool)> onSurface;
        std::function<void(CursorSnapshot const&)> onCursor;
        std::function<void()> onNotification;
        std::unique_ptr<InputMappingCoordinator> coordinator;
        Fixture()
        {
            coordinator = std::make_unique<InputMappingCoordinator>(nullptr,
                [this](auto const&, auto const&, DWORD) { ++notifications; if (onNotification) onNotification(); },
                InputMappingDependencies{ [this](HWND) { return std::make_unique<FakeEngine>(engine); },
                    [this] { return uiAccess; } });
            coordinator->Initialize();
            coordinator->SetUiCallbacks([this](bool transparent) {
                if (transparent) ++hides; else ++shows;
                if (onSurface) onSurface(transparent); return true;
            }, [this](CursorSnapshot const& cursor) { ++cursors; if (onCursor) onCursor(cursor); },
                [this](POINT point) { ++externalMoves; externalPosition = point; });
        }
        ~Fixture()
        {
            engine.ClearCallbacks(); onSurface = {}; onCursor = {}; onNotification = {};
            coordinator->Shutdown();
        }
        void Publish(ProxyPhase phase)
        {
            auto const& config = engine.configurations.back();
            engine.update.cursor = {};
            engine.update.cursor.generation = config.generation;
            engine.update.cursor.lensId = config.lensId;
            engine.update.cursor.phase = phase;
            coordinator->PumpInputUpdates();
        }
    };
    struct PulseContext { InputMappingCoordinator* coordinator{}; int count{}; };
    LRESULT CALLBACK PulseWindow(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(
                reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
        auto context = reinterpret_cast<PulseContext*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_TIMER && context)
        {
            ++context->count;
            context->coordinator->PumpInputUpdates();
            Check(context->coordinator->Route(Config(2)) == MappingRouteResult::Deferred,
                "a dispatched timer cannot recursively start the engine");
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int RunMappingLifecycleTests()
{
    {
        auto original = Runtime();
        auto dev = original; dev.identity = &DevIdentity; dev.persistentSoftwareCursor = true;
        SetRuntime(dev);
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor() && c.BeginSoftwareCursor() && f.engine.cursorBegins == 1 &&
                f.engine.created == 1, "persistent software cursor begins once before routing");
            c.SoftwareCursorHeartbeat();
            Check(f.engine.cursorHeartbeats == 1, "UI heartbeat reaches the input runtime");
            Check(c.Route(Config()) == MappingRouteResult::Routed, "persistent standby accepts a route");
            auto config = f.engine.configurations.back();
            f.engine.update.cursor.generation = config.generation;
            f.engine.update.cursor.lensId = config.lensId;
            f.engine.update.cursor.phase = ProxyPhase::Armed;
            f.engine.update.externalCursorMoved = true;
            f.engine.update.externalCursorPosition = { -900, 420 };
            c.PumpInputUpdates();
            Check(f.externalMoves == 1 && f.externalPosition.x == -900 && f.externalPosition.y == 420,
                "coalesced external cursor location reaches UI");
            Check(c.ReleaseRoute() && c.EndSoftwareCursor() && c.EndSoftwareCursor() &&
                f.engine.cursorEnds == 1, "last standby releases the native hide exactly once");
            c.DeactivateAll();
            Check(f.engine.stops == 1, "normal stop follows cursor restoration");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            f.engine.cursorBeginSucceeded = false;
            Check(!c.BeginSoftwareCursor() && f.engine.stops == 1 && !c.HasRoute(),
                "failed cursor lease does not leave an input runtime active");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor(), "unhealthy-route fixture begins cursor lease");
            f.engine.healthy = false;
            auto starts = f.engine.starts;
            Check(c.Route(Config()) == MappingRouteResult::Failed && f.engine.starts == starts &&
                f.engine.stops == 1 && c.SoftwareCursorRestoreConfirmed(),
                "unhealthy guardian stops instead of restarting with a stale cursor lease");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor(), "failed-end fixture begins cursor lease");
            f.engine.cursorEndSucceeded = false;
            Check(c.EndSoftwareCursor() && c.EndSoftwareCursor() && f.engine.stops == 1 &&
                c.SoftwareCursorRestoreConfirmed(),
                "a successful final show during Stop clears the earlier End failure");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor(), "failed-show fixture begins cursor lease");
            f.engine.cursorEndSucceeded = false;
            f.engine.nativeShowConfirmed = false;
            Check(!c.EndSoftwareCursor() && !c.SoftwareCursorRestoreConfirmed(),
                "failed End and failed final show keep the overlay recovery obligation");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor() && c.Route(Config()) == MappingRouteResult::Routed,
                "reentrant fault fixture has an active cursor lease");
            auto config = f.engine.configurations.back();
            f.engine.update.cursor.generation = config.generation;
            f.engine.update.cursor.lensId = config.lensId;
            f.engine.update.cursor.phase = ProxyPhase::Armed;
            bool failedInsideCallback = false;
            f.onCursor = [&](CursorSnapshot const&) {
                if (failedInsideCallback) return;
                failedInsideCallback = true;
                Check(!c.ReportSoftwareCursorFailure(ERROR_GEN_FAILURE),
                    "a reentrant overlay fault waits for the outer callback to stop the worker");
            };
            c.PumpInputUpdates();
            Check(failedInsideCallback && f.engine.stops == 1 && c.SoftwareCursorRestoreConfirmed(),
                "the deferred Stop confirmation releases the reentrant overlay obligation");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor() && c.ReportSoftwareCursorFailure(ERROR_GEN_FAILURE) &&
                f.engine.cursorOrder.size() >= 2 && f.engine.cursorOrder[0] == 'E' &&
                f.engine.cursorOrder[1] == 'S',
                "overlay fault drains and shows native before stopping input runtime");
        }
        {
            Fixture f; auto& c = *f.coordinator;
            Check(c.BeginSoftwareCursor(), "unconfirmed-stop fixture begins cursor lease");
            f.engine.nativeShowConfirmed = false;
            c.DeactivateAll();
            Check(!c.EndSoftwareCursor(), "direct shutdown cannot claim a native show that Stop did not confirm");
        }
        SetRuntime(original);
    }
    {
        InputMappingLifecycle state;
        Check(state.Begin(MappingOperation::Starting) && state.Busy(), "start acquires independent UI lifecycle");
        Check(!state.Begin(MappingOperation::Pumping), "snapshot processing cannot release a pending start");
        state.RequestStop(); Check(state.Cancelled(), "cancel is sticky until outer unwind");
        state.Stopping(); Check(!state.Begin(MappingOperation::Starting), "stop callbacks cannot start a route");
        state.Finish(); Check(state.Begin(MappingOperation::Starting), "ordinary cancellation allows a later retry");
        state.RequestStop(true); state.Finish();
        Check(!state.Begin(MappingOperation::Starting) && state.Shutdown(), "shutdown cannot revive an old session");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        f.engine.onStart = [&] {
            for (int i = 0; i < 5000; ++i)
            {
                c.PumpInputUpdates();
                Check(c.Route(Config(i % 2 + 1)) == MappingRouteResult::Deferred, "nested routing is deferred, not a global failure");
                Check(c.RoutePending() && c.LifecycleBusy(), "stale default Off snapshots cannot clear startup protection");
            }
            Check(f.engine.starts == 1 && f.engine.takes == 0 && f.engine.stops == 0,
                "a refresh flood does not touch the in-flight engine");
        };
        Check(c.Route(Config()) == MappingRouteResult::Routed && f.engine.configurations.size() == 1,
            "one outer startup produces exactly one configuration");
        Check(!c.LifecycleBusy() && c.RoutePending() && !f.notifications, "startup finishes without a spurious failure");
        c.PumpInputUpdates(); Check(c.RoutePending(), "old generation remains ignored after startup");
        f.Publish(ProxyPhase::Armed);
        Check(!c.RoutePending() && c.Phase() == ProxyPhase::Armed, "only matching worker state completes the route");
        auto starts = f.engine.starts;
        Check(c.Route(Config()) == MappingRouteResult::Routed && f.engine.starts == starts,
            "hovering the current route does not start or configure again");
        f.engine.onStart = {};
        for (auto phase : { ProxyPhase::Preparing, ProxyPhase::Active, ProxyPhase::Restoring })
        {
            f.Publish(phase);
            Check(c.Route(Config(2)) == MappingRouteResult::Deferred, "active or recovering input refuses cross-lens switching");
        }
        f.Publish(ProxyPhase::Armed);
        Check(c.Route(Config(2)) == MappingRouteResult::Routed && c.RoutedLensId() == 2 && f.engine.created == 1,
            "idle cross-lens routing reuses the one engine");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        PulseContext context{ &c };
        WNDCLASSW cls{}; cls.lpfnWndProc = PulseWindow; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"RegionLens.LifecycleTest.MessageOnly";
        auto atom = RegisterClassW(&cls);
        auto window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, cls.hInstance, &context);
        Check(atom && window, "hidden message-only fixture is created");
        f.engine.onStart = [&] {
            for (int i = 0; i < 2000; ++i) PostMessageW(window, WM_TIMER, InputMappingCoordinator::RefreshTimerId, 0);
            MSG message{};
            while (PeekMessageW(&message, window, WM_TIMER, WM_TIMER, PM_REMOVE)) DispatchMessageW(&message);
        };
        Check(c.Route(Config()) == MappingRouteResult::Routed && context.count == 2000 && f.engine.starts == 1,
            "real Win32 message dispatch during fake shell activation cannot recurse");
        DestroyWindow(window); UnregisterClassW(cls.lpszClassName, cls.hInstance);
    }
    for (int cancellation = 0; cancellation < 3; ++cancellation)
    {
        Fixture f; auto& c = *f.coordinator;
        f.engine.onStart = [&] {
            if (cancellation == 0) c.DeactivateAll();
            else if (cancellation == 1) c.ReleaseRoute();
            else c.Shutdown();
            Check(c.LifecycleBusy() && !c.HasRoute() && f.engine.stops == 0 && f.engine.destroyed == 0,
                "cancel, close and exit defer engine destruction");
            c.PumpInputUpdates();
        };
        Check(c.Route(Config()) == MappingRouteResult::Cancelled && f.engine.configurations.empty(),
            "cancelled startup never publishes a mapping configuration");
        Check(f.engine.stops == 1 && f.engine.destroyed == 1 && !c.LifecycleBusy() && f.shows == 1,
            "outer unwind restores surfaces and stops exactly once");
        Check(!f.notifications, "user cancellation does not report engine failure");
        f.engine.onStart = {};
        if (cancellation == 2)
            Check(c.Route(Config()) == MappingRouteResult::Cancelled && f.engine.created == 1, "exit forbids later startup");
        else
        {
            Check(c.Route(Config(2)) == MappingRouteResult::Routed && f.engine.created == 2, "cancelled startup can retry on another armed lens");
        }
    }
    {
        Fixture f; auto& c = *f.coordinator;
        f.engine.onStart = [&] {
            for (int i = 0; i < 100; ++i)
            {
                auto geometry = Config(); geometry.destination.right = 1100 + i;
                Check(c.Update(geometry), "startup accepts fresh geometry without a nested drain");
            }
            c.Update(Config(2)); // Must not overwrite the starting lens's geometry.
        };
        Check(c.Route(Config()) == MappingRouteResult::Routed && f.engine.configurations.back().destination.right == 1199 &&
            !f.engine.drains && f.engine.configurations.size() == 1, "startup coalesces geometry and configures only the latest target");
        f.engine.onStart = {};
        f.Publish(ProxyPhase::Armed);
        auto blocked = Config(); blocked.blocked = true;
        Check(c.Update(blocked), "resize safely pauses the current route");
        for (int i = 0; i < 100; ++i) c.Update(blocked);
        Check(f.engine.drains == 1 && f.engine.configurations.size() == 1, "resize freeze does not repeatedly drain or configure");
        Check(c.Update(Config()) && f.engine.drains == 1 && f.engine.configurations.size() == 2,
            "resize completion reconfigures once and keeps the route armed");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        c.Route(Config()); f.Publish(ProxyPhase::Armed);
        f.engine.onDrain = [&] { auto latest = Config(); latest.destination.right = 1700; c.Update(latest); };
        Check(c.Update(Config()) && f.engine.configurations.size() == 1,
            "geometry arriving during a drain prevents stale reconfiguration");
        f.engine.onDrain = {}; c.PumpInputUpdates();
        Check(f.engine.configurations.size() == 2 && f.engine.configurations.back().destination.right == 1700,
            "next safe UI pump applies coalesced geometry");
        f.engine.onDrain = [&] { c.DeactivateAll(); };
        Check(c.Update(Config()) && !c.HasRoute() && f.engine.destroyed == 1,
            "cancellation during geometry drain defers stop until drain returns");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        c.Route(Config()); f.Publish(ProxyPhase::Armed);
        f.engine.onStop = [&] {
            c.DeactivateAll(); c.ReleaseRoute(); c.PumpInputUpdates();
            Check(c.Route(Config(2)) == MappingRouteResult::Deferred, "cleanup callbacks cannot start another session");
            Check(f.engine.stops == 1 && f.engine.destroyed == 0, "cleanup reentry does not destroy the executing engine");
        };
        c.DeactivateAll();
        Check(!c.HasRoute() && !c.LifecycleBusy() && f.engine.stops == 1 && f.engine.destroyed == 1,
            "nested stop requests coalesce to one completed recovery");
        Check(f.engine.drains == 1, "ordinary deactivation preserves graceful drain before worker shutdown");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        c.Route(Config()); f.Publish(ProxyPhase::Preparing);
        f.engine.update.surfaces = ProxySurfaceRequest{ 71, true };
        f.onSurface = [&](bool transparent) { if (transparent) c.DeactivateAll(); };
        auto published = f.cursors;
        c.PumpInputUpdates();
        Check(f.engine.acknowledgements.empty() && f.engine.destroyed == 1 && f.shows == 1 && f.cursors == published + 1,
            "surface callback cancellation cannot acknowledge or publish the stale session");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        c.Route(Config()); f.Publish(ProxyPhase::Armed);
        f.engine.update.surfaces = ProxySurfaceRequest{ 80, true };
        f.engine.onTake = [&] { c.Shutdown(); };
        c.PumpInputUpdates();
        Check(!c.HasRoute() && !c.LifecycleBusy() && f.engine.destroyed == 1 &&
            f.engine.acknowledgements.empty() && !f.hides,
            "shutdown during snapshot acquisition cannot revive a stale surface request");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        c.Route(Config()); f.Publish(ProxyPhase::Armed);
        f.engine.update.cursor.generation = 0;
        f.engine.update.surfaces = ProxySurfaceRequest{ 99, true };
        c.PumpInputUpdates();
        Check(f.engine.acknowledgements.size() == 1 && !f.engine.acknowledgements[0].second && f.hides == 0 && c.HasRoute(),
            "expired surface preparation is rejected without hiding current windows");
        f.onCursor = [&](auto const&) { c.DeactivateAll(); };
        f.Publish(ProxyPhase::Active);
        Check(!c.HasRoute() && f.engine.destroyed == 1 && !f.notifications, "cursor callback cancellation safely stops without stale errors");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        Check(c.Route(Config()) == MappingRouteResult::Routed, "input-fault fixture starts a route");
        auto const config = f.engine.configurations.back();
        f.engine.update.cursor = {};
        f.engine.update.cursor.generation = config.generation;
        f.engine.update.cursor.lensId = config.lensId;
        f.engine.update.cursor.phase = ProxyPhase::Off;
        f.engine.update.cursor.error = ERROR_CANCELLED;
        f.engine.update.cursor.errorSite = ProxyFaultSite::HealthCheck;
        f.engine.update.cursor.cancelReason = ProxyCancelReason::ForeignInput;
        c.PumpInputUpdates();
        Check(!c.HasRoute() && f.notifications == 1,
            "genuine input fault reports the error and closes the route");
        Check(c.Route(Config(2)) == MappingRouteResult::Routed, "new route works after input fault");
        auto const next = f.engine.configurations.back();
        f.engine.update.cursor.generation = next.generation;
        f.engine.update.cursor.lensId = next.lensId;
        f.engine.update.cursor.cancelReason = ProxyCancelReason::ForeignInput;
        c.PumpInputUpdates();
        Check(f.notifications == 2, "a second input fault is still reported");
        Check(c.Route(Config(3)) == MappingRouteResult::Routed, "third route starts after genuine fault");
        auto const third = f.engine.configurations.back();
        f.engine.update.cursor.generation = third.generation;
        f.engine.update.cursor.lensId = third.lensId;
        f.engine.update.cursor.cancelReason = ProxyCancelReason::ForeignInput;
        f.engine.update.cursor.secondaryError = ERROR_WRITE_FAULT;
        c.PumpInputUpdates();
        Check(f.notifications == 3, "secondary recovery failure remains reported");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        f.engine.startSucceeded = false;
        f.engine.failure = { ProxyStartupStage::ShellLaunch, ERROR_ACCESS_DENIED };
        f.onNotification = [&] {
            Check(c.Route(Config(2)) == MappingRouteResult::Deferred, "a failure dialog cannot recursively restart input");
            c.PumpInputUpdates();
        };
        Check(c.Route(Config()) == MappingRouteResult::Failed && f.notifications == 1 && f.engine.destroyed == 1,
            "one startup failure is reported once and cleaned up");
        f.engine.startSucceeded = true;
        Check(c.Route(Config()) == MappingRouteResult::Routed, "a genuine failure does not permanently latch startup busy");
        f.engine.drainSucceeded = false;
        Check(!c.ReleaseRoute() && f.engine.destroyed == 2 && !c.HasRoute(), "failed recovery drain closes the unsafe engine");
    }
    {
        Fixture f; f.uiAccess = false; f.coordinator->Initialize();
        Check(f.coordinator->Route(Config()) == MappingRouteResult::Failed && !f.engine.created && f.notifications == 1,
            "missing UIAccess still fails before creating an engine");
    }
    {
        Fixture f; auto& c=*f.coordinator;
        Check(c.Route(Config())==MappingRouteResult::Routed,"popup test starts one fake engine");
        f.Publish(ProxyPhase::Active);
        auto old=f.engine.configurations.back();
        Check(c.ReleaseRoute() && !c.HasRoute() && f.engine.stops==0,
            "opening quality UI drains input but retains the guardian");
        f.engine.update.cursor.generation=old.generation;
        f.engine.update.cursor.lensId=old.lensId;
        f.engine.update.cursor.phase=ProxyPhase::Active;
        f.engine.update.surfaces=ProxySurfaceRequest{77,true};
        auto hides=f.hides;c.PumpInputUpdates();
        Check(f.hides==hides && !c.HasRoute() && !f.engine.acknowledgements.empty() && !f.engine.acknowledgements.back().second,
            "stale transparent acknowledgement cannot take over behind the popup");
        Check(c.Route(Config())==MappingRouteResult::Routed && f.engine.created==1 && f.engine.stops==0,
            "closing the popup can reuse the safely idle engine");
        c.ReleaseRoute(); f.engine.healthy=false;
        Check(!c.InputRuntimeHealthy(), "paused guard failure is observable before attempting a new route");
        c.DeactivateAll();
        Check(!c.HasRoute() && f.engine.stops==1,"global deactivation while paused destroys input once");
    }
    {
        DeferredWindowActions actions;
        Check(actions.Push(WM_COMMAND, 1, 10) && actions.Push(WM_COMMAND, 1, 20), "deferred actions deduplicate without reposting");
        actions.Push(WM_CLOSE, 0, 0);
        DeferredWindowActions::Action action;
        Check(actions.Pop(action) && action.message == WM_COMMAND && action.lParam == 20, "deferred action keeps the latest payload");
        Check(actions.Pop(action) && action.message == WM_CLOSE && actions.Empty(), "destructive actions retain FIFO order");
        for (int i = 0; i < 64; ++i) Check(actions.Push(WM_APP, i, 0), "bounded action slot accepts one request");
        Check(!actions.Push(WM_APP, 65, 0), "deferred action flood cannot grow unbounded");
        actions.Clear(); Check(actions.Empty() && !actions.Pop(action), "exit can clear pending UI actions");
    }
    {
        Fixture f; auto& c=*f.coordinator;
        auto config=Config();c.Route(config);f.Publish(ProxyPhase::Active);
        auto old=f.engine.configurations.back();auto stops=f.engine.stops;
        config.pointerSpeed={31};
        Check(c.Update(config) && f.engine.configurations.back().pointerSpeed == PointerSpeed{31} &&
            f.engine.configurations.back().generation>old.generation && f.engine.drains==1 && f.engine.stops==stops,
            "speed preference safely drains and replaces the route without stopping its watchdog");
        auto newGeneration=f.engine.configurations.back().generation;
        f.engine.update.cursor.generation=old.generation;f.engine.update.cursor.lensId=old.lensId;
        f.engine.update.cursor.phase=ProxyPhase::Active;
        f.engine.update.surfaces=ProxySurfaceRequest{887,true};auto hides=f.hides;c.PumpInputUpdates();
        Check(f.hides==hides && !f.engine.acknowledgements.back().second,
            "previous speed session cannot restore transparent surfaces with an old confirmation");
        f.Publish(ProxyPhase::Armed);
        auto configurations=f.engine.configurations.size();auto drains=f.engine.drains;
        auto other=Config(2);other.pointerSpeed={35};
        Check(!c.Update(other) && f.engine.configurations.size()==configurations && f.engine.drains==drains,
            "unrouted lens preference cannot reconfigure the currently routed lens");
        config.destination.right=1600;config.fullscreen=true;
        Check(c.Update(config) && f.engine.configurations.back().pointerSpeed == PointerSpeed{31} &&
            f.engine.configurations.back().generation>newGeneration && f.engine.configurations.back().destination.right==1600,
            "fullscreen/size replacement retains preference and sends the updated physical extent");
        f.Publish(ProxyPhase::Active);
        auto beforeReset=f.engine.configurations.back();
        drains=f.engine.drains;
        config.pointerSpeed={};
        Check(c.Update(config) && f.engine.configurations.back().pointerSpeed.Multiplier()==1.0 &&
            f.engine.configurations.back().generation>beforeReset.generation &&
            f.engine.drains==drains+1 && f.engine.stops==stops,
            "one-click speed reset safely replaces the current route with neutral speed without stopping standby runtime");
        // The panel-close path releases a route first and applies the final
        // reset preference before submitting a new generation on resume.
        Check(c.ReleaseRoute() && !c.HasRoute(),"settings can release the reset route without shutting down the engine");
        Check(c.Route(config)==MappingRouteResult::Routed &&
            f.engine.configurations.back().pointerSpeed.Multiplier()==1.0 &&
            f.engine.configurations.back().generation>beforeReset.generation,
            "closing a reset panel resumes with 1.00x rather than its last slider value");
        c.DeactivateAll();Check(!c.HasRoute(),"global deactivation after preference change does not re-arm mapping");
    }
    {
        Fixture f; auto& c = *f.coordinator;
        MappingStandbyPolicy standby; MappingUiPauseState pause;
        standby.Arm(1); standby.Arm(2); standby.CommitRoute(1);
        c.Route(Config()); f.Publish(ProxyPhase::Active);
        auto old = f.engine.configurations.back();
        auto hide = [&] {
            pause.Hidden(true);
            if (!standby.Suspended()) { standby.Suspend(); Check(c.ReleaseRoute(), "hide drains active input"); }
        };
        hide(); hide();
        Check(!c.HasRoute() && f.engine.drains == 1 && f.engine.stops == 0 && standby.Count() == 2,
            "repeated hide drains once, preserving standby and the guardian");
        f.engine.update.cursor = { };
        f.engine.update.cursor.generation = old.generation;
        f.engine.update.cursor.lensId = old.lensId;
        f.engine.update.cursor.phase = ProxyPhase::Active;
        f.engine.update.surfaces = ProxySurfaceRequest{991,true};
        auto hides = f.hides; c.PumpInputUpdates();
        Check(f.hides == hides && !f.engine.acknowledgements.back().second && !c.HasRoute(),
            "late active cursor and transparency requests cannot reactivate hidden regions");
        pause.Hidden(false);
        auto resumed = standby.Resume();
        auto config = Config(resumed); config.fullscreen = true; config.pointerSpeed = {31};
        Check(c.Route(config) == MappingRouteResult::Routed && standby.CommitRoute(resumed) &&
            f.engine.configurations.back().generation > old.generation && f.engine.created == 1 &&
            f.engine.configurations.back().fullscreen && f.engine.configurations.back().pointerSpeed == PointerSpeed{31},
            "show starts a fresh session on the retained engine using current lens preferences");
        hide(); standby.Clear(); standby.Suspend(); c.DeactivateAll();
        pause.Hidden(false);
        Check(standby.Resume() == 0 && standby.Empty() && !c.HasRoute() && f.engine.stops == 1,
            "global deactivation while hidden leaves no route or buttons to resume");
    }
    if (!failures) std::cout << "Mapping coordinator startup reentrancy, cancellation, geometry, visibility and deferred UI tests passed.\n";
    return failures;
}
