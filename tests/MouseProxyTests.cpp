#include "MouseProxyCore.h"
#include "ProxyWatchdog.h"
#include "ProxySendChannel.h"
#include "MappingGeometryGate.h"
#include <iostream>
#include <vector>
#include <thread>
#include <future>

using namespace RegionLens::native;
namespace
{
    int failures{};
    void Check(bool condition, char const* name)
    {
        if (!condition) { ++failures; std::cerr << "FAILED proxy: " << name << '\n'; }
    }
    struct Fake : IProxyBackend
    {
        struct Packet { POINT point; UINT message; DWORD data; };
        struct Request { uint64_t id; bool transparent; };
        struct Fault { ProxyFaultSite site; DWORD error; bool first; };
        bool healthy{ true }, canEnter{ true }, source{ true }, exposed{ true }, visible{ true }, settled{ true };
        bool failHide{}, failShow{}, recoveryFailed{}, recoveryActive{}, recoveredExternally{};
        bool cursorPending{}, inputActiveAtRecoveryFailure{};
        int showCalls{}, hideCalls{}, exposureCalls{};
        int failSendAt{ -1 }, sendCount{};
        uint32_t recoveryButtons{};
        uint64_t tick{};
        std::vector<Packet> packets;
        std::vector<Request> requests;
        std::vector<Fault> faults;
        std::vector<char> handoff;
        CursorSnapshot snapshot;
        bool Healthy() override { return healthy; }
        uint64_t NowTick() override { return tick; }
        bool CanEnter(MappingSessionConfig const&, POINT) override { return canEnter; }
        bool SourceAccessible(MappingSessionConfig const&, POINT) override { return source; }
        bool DestinationExposed(MappingSessionConfig const&, POINT) override { ++exposureCalls; return exposed; }
        bool CursorVisible(bool value) override
        {
            if (value) { ++showCalls; handoff.push_back('S'); }
            else { ++hideCalls; cursorPending = true; }
            if ((!value && failHide) || (value && failShow)) return false;
            visible = value; if (value) cursorPending = false; return true;
        }
        bool SendMouse(POINT p, UINT message, DWORD data, RECT) override
        {
            if (snapshot.phase == ProxyPhase::Restoring)
                handoff.push_back(message == WM_MOUSEMOVE ? 'M' : 'U');
            packets.push_back({ p, message, data }); return ++sendCount != failSendAt;
        }
        bool InputSettled() override { return settled; }
        bool RecoveryCompletedExternally() override { return recoveredExternally; }
        void RequestSurfaces(uint64_t id, bool transparent) override
        { if (!transparent) handoff.push_back('O'); requests.push_back({ id, transparent }); }
        void RecoveryState(bool active, POINT, POINT, uint32_t buttons) override { recoveryActive = active; recoveryButtons = buttons; }
        void RecoveryFailed() override { recoveryFailed = true; inputActiveAtRecoveryFailure = recoveryActive; }
        void Publish(CursorSnapshot const& value) override
        { if (snapshot.visible && !value.visible) handoff.push_back('V'); snapshot = value; }
#ifndef REGIONLENS_PRODUCTION_SOURCE
        void DiagnosticFault(ProxyFaultSite site, DWORD error, bool first) override { faults.push_back({ site, error, first }); }
#endif
    };
    // Simulate the observed transport entirely in memory: callbacks arrive
    // during SendMouse with dwExtraInfo zero-extended from its low 32 bits.
    // No native injection, hook installation or desktop cursor manipulation.
    struct CookieLoopback : Fake
    {
        ULONG_PTR cookie{};
        int pending{}, acknowledged{}, foreign{}, wireInputs{};
        bool SendMouse(POINT p, UINT message, DWORD data, RECT desktop) override
        {
            if (!Fake::SendMouse(p, message, data, desktop)) return false;
            auto batch = MakeProxyInputs(p, message, data, desktop, cookie);
            if (!batch.count) return false;
            wireInputs += int(batch.count);
            pending += int(batch.count);
            for (UINT i = 0; i < batch.count; ++i)
            {
                auto received = static_cast<ULONG_PTR>(uint32_t(batch.inputs[i].mi.dwExtraInfo));
                if (ClassifyProxyInput(LLMHF_INJECTED, received, cookie) == ProxyInputOrigin::OwnInjection)
                { --pending; ++acknowledged; }
                else { ++foreign; healthy = false; }
            }
            return healthy;
        }
        bool InputSettled() override { return pending == 0; }
    };
    struct AsyncFake : Fake
    {
        ProxySendChannel channel;
        bool SendMouse(POINT p, UINT message, DWORD data, RECT desktop) override
        {
            auto batch = MakeProxyInputs(p, message, data, desktop, 123);
            ProxySendJob job; job.count = batch.count; job.message = message; job.point = p; job.tick = tick;
            job.recovery = snapshot.phase == ProxyPhase::Restoring;
            std::copy_n(batch.inputs.begin(), batch.count, job.inputs.begin());
            if (!channel.Submit(job, 456)) return false;
            return Fake::SendMouse(p, message, data, desktop);
        }
        bool InputSettled() override { return !channel.Busy(); }
        void Complete(MouseProxyCore& core, DWORD error = 0)
        {
            ProxySendJob job;
            Check(channel.Take(job), "fake sender takes exactly one queued batch");
            // Deliberately omit ALL payload callbacks (including wheel). Only
            // the terminal fence is an ordering receipt, never target proof.
            channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED,
                job.requiresFence ? ULONG_PTR(456) : job.inputs[0].mi.dwExtraInfo);
            channel.Complete({ error ? 1u : job.count, error, 313 });
            ProxySendResult result;
            Check(channel.Poll(tick, result), "fake batch completion becomes observable");
            if (result.error) core.InjectionFailed(result.error);
            else if (!job.recovery) core.InjectionCompleted();
            core.Pump();
        }
    };
    MappingSessionConfig Config(uint64_t generation = 1)
    {
        MappingSessionConfig config;
        config.generation = generation; config.lensId = generation;
        config.source = { -1000, -500, -600, -200 };
        config.destination = { 100, 100, 900, 700 };
        config.desktop = { -1920, -1080, 3840, 2160 };
        return config;
    }
    MappingSessionConfig AspectFitConfig(uint64_t generation = 1)
    {
        auto config = Config(generation);
        config.fullscreen = true;
        config.lensClient = { 100, 100, 1100, 900 };
        // 4:3 content centred in a 5:4 fullscreen client leaves 25-pixel
        // letterbox bars above and below the picture.
        config.destination = { 100, 125, 1100, 875 };
        return config;
    }
    void Event(MouseProxyCore& core, UINT message, double dx = 0, double dy = 0, DWORD data = 0)
    {
        Check(core.Intercept({ message, {}, dx, dy, data }), "active input is consumed"); core.Pump();
    }
    void Enter(MouseProxyCore& core, Fake& backend, MappingSessionConfig config = Config())
    {
        core.Configure(config);
        Check(!core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }), "entry move remains native until exposure verified");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Preparing, "wait for surface acknowledgement");
        Check(backend.packets.empty() && backend.visible, "no input/hiding before transparent surfaces ready");
        core.SurfaceAcknowledged(backend.requests.back().id, true);
        Check(core.Phase() == ProxyPhase::Active && !backend.visible, "acknowledgement starts proxy");
    }
    void FinishRestore(MouseProxyCore& core, Fake& backend)
    {
        core.Pump();
        Check(!backend.requests.empty() && !backend.requests.back().transparent, "restore requests opaque surfaces");
        core.SurfaceAcknowledged(backend.requests.back().id, true);
    }
}

int RunMouseProxyTests()
{
    // Each fixture owns a bounded event queue. Keep independent cores on the heap
    // so MSVC's combined stack frame does not exceed the default 1 MiB stack.
    failures = 0;
    auto config = Config();
    Check(config.pointerSpeed.Multiplier()==1.0, "new routes preserve ordinary virtual mouse speed at 1.00x");
    for (int tick = PointerSpeed::Minimum; tick <= PointerSpeed::Maximum; ++tick)
    {
        auto scaled=Config();
        scaled.pointerSpeed={tick};
        auto gain=PointerGainFor(scaled);
        auto expected=tick/20.0;
        Check(gain.x==expected && gain.y==expected,"all 41 speed values use one independent user-selected gain");
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled);
        Event(core,WM_MOUSEMOVE,12.25,-5.5);
        auto result=core.Snapshot();
        Check(result.position.x==350+LONG(std::llround(12.25*expected)) &&
            result.position.y==LONG(std::llround(350-5.5*expected)),"preview uses the selected multiplier once");
        auto sourcePoint=ProxyMapPoint(350+12.25*expected,350-5.5*expected,scaled.destination,scaled.source);
        Check(result.sourcePosition.x==sourcePoint.x && result.sourcePosition.y==sourcePoint.y,
            "source receives the inverse of the same fractional preview position");
        Event(core,WM_MOUSEWHEEL,100,100,DWORD(120<<16));
        Check(fake.packets.back().data==DWORD(120<<16) && core.Snapshot().position.x==result.position.x,
            "wheel magnitude and pointer location are never multiplied as movement");
        // Binary-exact fractions avoid testing an arbitrary decimal half-pixel
        // rounding tie instead of whether the remainder is retained.
        double accumulated=350+12.25*expected;
        for(int i=0;i<1000;++i) { Event(core,WM_MOUSEMOVE,1.0/64,0); accumulated+=(1.0/64)*expected; }
        Check(core.Snapshot().position.x==LONG(std::llround(accumulated)),
            "continuous subpixel deltas retain fractional remainder without drift");
        core.Disable(); FinishRestore(core,fake);
        Check(fake.visible && core.Phase()==ProxyPhase::Off,"every scale restores native cursor on disable");
    }
    {
        auto scaled=Config(); scaled.pointerSpeed={40}; scaled.source={-900,-600,-500,600};
        auto gain=PointerGainFor(scaled); Check(gain.x==2 && gain.y==2,"nonuniform picture geometry cannot apply another speed gain");
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled);
        Event(core,WM_MOUSEMOVE,20,20);
        Check(core.Snapshot().position.x==390 && core.Snapshot().position.y==390,"nonuniform magnification does not alter the selected virtual speed");
    }
    {
        auto scaled=Config(); scaled.pointerSpeed={40}; scaled.source={-1000,-500,-999,-499};
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled);
        Event(core,WM_MOUSEMOVE,4,3);
        Check(core.Snapshot().position.x==358 && core.Snapshot().position.y==356 &&
            core.Snapshot().sourcePosition.x==-1000 && core.Snapshot().sourcePosition.y==-500,
            "single-pixel source axis remains fixed even as its virtual pointer moves");
        core.Intercept({WM_LBUTTONDOWN}); core.Pump();
        Event(core,WM_MOUSEMOVE,-1e308,1e308);
        Check(core.Phase()==ProxyPhase::Active && core.Snapshot().position.x==110 && core.Snapshot().position.y==689,
            "extreme finite gain/delta saturates before LONG conversion and respects drag limits");
        Event(core,WM_LBUTTONUP); core.Disable(); FinishRestore(core,fake);
        Check(fake.visible && !fake.recoveryButtons,"extreme motion cannot leave a held button or hidden cursor");
    }
    for(auto invalid:{std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    {
        auto scaled=Config(); scaled.pointerSpeed={40};
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled);
        Event(core,WM_MOUSEMOVE,invalid,0); FinishRestore(core,fake);
        Check(core.Phase()==ProxyPhase::Failed && fake.visible && core.Snapshot().error==ERROR_INVALID_DATA,
            "nonfinite motion safely stops takeover without converting NaN/Inf into coordinates");
    }
    {
        auto scaled=Config(); scaled.pointerSpeed={40};
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled); fake.Complete(core);
        Event(core,WM_LBUTTONDOWN);
        for(int i=0;i<1000;++i) Event(core,WM_MOUSEMOVE,0.05,0);
        Check(core.Snapshot().position.x==450 && core.InputQueueDepth()==1,
            "scaled preview remains current while a send is pending and movements coalesce");
        Event(core,WM_MOUSEHWHEEL,0,0,DWORD(120<<16)); Event(core,WM_MOUSEMOVE,10,0); Event(core,WM_LBUTTONUP);
        for(int i=0;i<16 && fake.channel.Busy();++i) fake.Complete(core);
        auto expected=ProxyMapPoint(470,350,scaled.destination,scaled.source);
        Check(!fake.channel.Busy() && core.InputQueueDepth()==0 && fake.packets.back().point.x==expected.x &&
            core.Snapshot().position.x==470 && !core.Snapshot().buttons,
            "queued virtual stamps are not multiplied again during ordered source dispatch");
        int wheels{}; for(auto const& p:fake.packets) if(p.message==WM_MOUSEHWHEEL) { ++wheels; Check(p.data==DWORD(120<<16),"horizontal wheel payload unchanged"); }
        Check(wheels==1,"coalescing retains exactly one wheel among scaled drag moves");
    }
    for (int button : ChromeButtonIds)
    {
        auto scaled=Config(); scaled.pointerSpeed={40};
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core,fake,scaled);
        RECT client{0,0,800,600};auto target=ChromeButtonCenterFor(button,client);
        target.x+=100;target.y+=100;
        Event(core,WM_MOUSEMOVE,(target.x-350)/2.0,(target.y-350)/2.0); FinishRestore(core,fake);
        Check(core.Phase()==ProxyPhase::Armed && fake.visible && !core.Snapshot().visible &&
            fake.packets.back().point.x==target.x && fake.packets.back().point.y==target.y,
            "all eight controls restore native pointer at the scaled virtual endpoint");
        Check(!core.Intercept({WM_LBUTTONDOWN,target}) && !core.Intercept({WM_LBUTTONUP,target}),
            "all local button clicks remain native while custom speed is set");
    }
    {
        auto scaled=Config();scaled.pointerSpeed={40};
        Fake fake;auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;Enter(core,fake,scaled);
        Event(core,WM_MOUSEMOVE,-150,0);FinishRestore(core,fake);
        Check(core.Phase()==ProxyPhase::Armed && fake.visible && !core.Intercept({WM_MOUSEMOVE,{55,350}}),
            "leaving the lens returns subsequent motion to ordinary Windows input");
    }
    {
        auto scaled=Config();scaled.pointerSpeed={40};
        Fake fake;auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;Enter(core,fake,scaled);
        ProxyMouseEvent gesture{WM_LBUTTONDOWN};gesture.controlDown=true;
        Check(core.Intercept(gesture),"Ctrl-left gesture is initially consumed locally");core.Pump();FinishRestore(core,fake);
        Check(fake.snapshot.localMoveRequested && fake.visible && core.Phase()==ProxyPhase::Armed,
            "Ctrl drag restores input before requesting ordinary window movement");
        auto pointer=core.Snapshot().position;
        Check(!core.Intercept({WM_MOUSEMOVE,{360,360},10,10}) && core.Snapshot().position.x==pointer.x,
            "window dragging with held Ctrl-left is never multiplied by the route gain");
        Check(std::none_of(fake.packets.begin(),fake.packets.end(),[](auto const& p){return p.message==WM_LBUTTONDOWN;}),
            "Ctrl window dragging never injects a source button down");
    }
    {
        auto scaled=Config(); scaled.pointerSpeed={40};
        Fake fake;auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;Enter(core,fake,scaled);
        auto stale=core.SurfaceRequest(); auto changed=scaled; changed.generation=2;changed.pointerSpeed={};
        core.Configure(changed);FinishRestore(core,fake);
        Check(core.Phase()==ProxyPhase::Armed && core.Snapshot().generation==2 && fake.visible,
            "preference replacement waits for restoration then uses new generation");
        core.SurfaceAcknowledged(stale,true);
        Check(core.Phase()==ProxyPhase::Armed && fake.visible,"old prepare confirmation cannot reactivate old multiplier");
        core.Intercept({WM_MOUSEMOVE,{350,350}});core.Pump();core.SurfaceAcknowledged(core.SurfaceRequest(),true);
        Event(core,WM_MOUSEMOVE,10,10);
        Check(core.Snapshot().position.x==360 && core.Snapshot().position.y==360,
            "replacement configuration refreshes cached gains without inheriting the old 2x multiplier");
    }
    auto first = ProxyMapPoint(100, 100, config.destination, config.source);
    auto last = ProxyMapPoint(899, 699, config.destination, config.source);
    Check(first.x == -1000 && first.y == -500 && last.x == -601 && last.y == -201, "negative-monitor endpoints exact");
    auto centre = ProxyMapPoint(499.5, 399.5, config.destination, config.source);
    Check(centre.x == -800 && centre.y == -350, "fractional centre and unequal axes");
    auto single = ProxyMapPoint(10, 20, { 10, 20, 11, 21 }, { -3, -4, -2, -3 });
    Check(single.x == -3 && single.y == -4, "single pixel avoids zero division");
    Check(ProxyMapAxis(1,0,2,(std::numeric_limits<LONG>::min)(),(std::numeric_limits<LONG>::max)())==
        (std::numeric_limits<LONG>::max)()-1,"wide signed source span maps without intermediate integer overflow");
    Check(ProxyMapPoint(-99999, 99999, config.destination, config.source).y == -201, "point mapping clamps");
    for (LONG p = -1920; p < 3840; ++p)
    {
        LONG normalized = ProxyAbsoluteAxis(p, -1920, 3840);
        Check(LONG((uint64_t(normalized) * 5760) / 65536) - 1920 == p, "absolute input resolves to intended physical pixel");
    }
    auto packed = PackProxyPoint({ -1200, -17 });
    Check(UnpackProxyPoint(packed).x == -1200 && UnpackProxyPoint(packed).y == -17, "watchdog coordinates preserve signs");
    Check(!ProxyHeartbeatExpired(2000, 1000) && ProxyHeartbeatExpired(2001, 1000) && !ProxyHeartbeatExpired(1000, 2000),
        "watchdog recovery threshold is greater than one second");

    ProxyEventQueue queue;
    for (int i = 0; i < 1000; ++i) Check(queue.Push({ WM_MOUSEMOVE, { i, i }, 0.25, -0.5 }), "moves coalesce");
    ProxyMouseEvent queued;
    Check(queue.Size() == 1 && queue.Pop(queued) && queued.dx == 250 && queued.dy == -500 && queued.point.x == 999,
        "coalescing retains displacement and latest position");
    for (size_t i = 0; i < ProxyEventQueue::Capacity; ++i) Check(queue.Push({ WM_MOUSEWHEEL }), "bounded queue retains wheel events");
    Check(!queue.Push({ WM_LBUTTONUP }), "overflow is reported, not silently dropped");

    auto wheel = MakeProxyInputs({ -1500, 100 }, WM_MOUSEWHEEL, DWORD(WORD(SHORT(-120))) << 16, config.desktop, 12345);
    Check(wheel.count == 2 && wheel.inputs[1].mi.dwFlags == MOUSEEVENTF_WHEEL && LONG(wheel.inputs[1].mi.mouseData) == -120,
        "vertical wheel preserves signed delta");
    auto side = MakeProxyInputs({}, WM_XBUTTONDOWN, XBUTTON2 << 16, config.desktop, 77);
    Check(side.inputs[1].mi.dwFlags == MOUSEEVENTF_XDOWN && side.inputs[1].mi.mouseData == XBUTTON2 &&
        side.inputs[0].mi.dwExtraInfo == 77 && side.inputs[1].mi.dwExtraInfo == 77, "side button and injection tags exact");
    auto horizontal = MakeProxyInputs({}, WM_MOUSEHWHEEL, 120 << 16, config.desktop, 77);
    Check(horizontal.inputs[1].mi.dwFlags == MOUSEEVENTF_HWHEEL, "horizontal wheel is not vertical");
    Check(MakeProxyInputs({}, WM_KEYDOWN, 0, config.desktop, 1).count == 0, "keyboard cannot be injected by mouse backend");

    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        auto request = core.SurfaceRequest();
        core.SurfaceAcknowledged(request - 1, false);
        Check(core.Phase() == ProxyPhase::Active, "stale acknowledgement ignored");
        auto interaction = core.Snapshot().interactionSequence;
        for (int i = 0; i < 4; ++i) Event(core, WM_MOUSEMOVE, 0.25, 0.25);
        Check(core.Snapshot().position.x == 351 && core.Snapshot().position.y == 351, "subpixel displacement accumulates without drift");
        Check(core.Snapshot().interactionSequence == interaction,
            "mapped motion alone does not promote a window");
        Event(core, WM_MOUSEWHEEL, 0, 0, 120 << 16);
        Check(core.Snapshot().interactionSequence == interaction,
            "wheel input alone does not promote a window");
        Event(core, WM_LBUTTONDOWN);
        Check(core.Snapshot().interactionSequence == interaction + 1,
            "mapped button-down publishes the window promotion signal");
        Event(core, WM_MOUSEMOVE, 10000, 10000);
        Check(core.Snapshot().position.x == 889 && core.Snapshot().position.y == 689, "drag is bounded to content");
        Event(core, WM_LBUTTONUP); Event(core, WM_MOUSEMOVE, 50, 0);
        Check(core.Phase() == ProxyPhase::Restoring, "released pointer may exit lens");
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Armed && fake.visible && !fake.recoveryActive, "ordinary cursor restored, mode still armed");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        Event(core, WM_LBUTTONDOWN); Event(core, WM_LBUTTONUP); Event(core, WM_LBUTTONDOWN); Event(core, WM_LBUTTONUP);
        Check(fake.packets[1].message == WM_LBUTTONDOWN && fake.packets[2].message == WM_LBUTTONUP &&
            fake.packets[3].message == WM_LBUTTONDOWN && fake.packets[4].message == WM_LBUTTONUP,
            "double click sequence contains exactly two pairs");
        Event(core, WM_RBUTTONDOWN); Event(core, WM_MBUTTONDOWN); Event(core, WM_XBUTTONDOWN, 0, 0, XBUTTON1 << 16);
        Event(core, WM_XBUTTONDOWN, 0, 0, XBUTTON2 << 16);
        core.Disable(); FinishRestore(core, fake);
        unsigned ups = 0;
        for (auto const& packet : fake.packets)
            if (packet.message == WM_RBUTTONUP || packet.message == WM_MBUTTONUP || packet.message == WM_XBUTTONUP) ++ups;
        Check(ups == 4 && core.Phase() == ProxyPhase::Off, "shutdown releases each injected button once");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        auto before = fake.packets.size();
        ProxyMouseEvent gesture{ WM_LBUTTONDOWN };
        gesture.controlDown = true;
        Check(core.Intercept(gesture), "Ctrl-left gesture is consumed while mapping is active");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Restoring && fake.packets.size() == before + 1 &&
            fake.packets.back().message == WM_MOUSEMOVE,
            "local move restores only pointer position and never injects its DOWN into source");
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Armed && fake.snapshot.localMoveRequested && fake.visible,
            "local move is requested only after opaque surfaces and native cursor are restored");
        Check(!core.Intercept({ WM_LBUTTONUP }), "gesture UP remains native for the captured lens window");
        core.Configure(Config(2));
        Check(core.Phase() == ProxyPhase::Armed && core.Snapshot().generation == 2,
            "mapping remains enabled and accepts final moved geometry");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        core.Configure(Config(2));
        Check(core.Phase() == ProxyPhase::Restoring && core.Snapshot().lensId == 1,
            "a replacement config cannot create a second active mapping session");
        auto staleRequest = core.SurfaceRequest() - 1;
        core.Configure(Config(3));
        core.SurfaceAcknowledged(staleRequest, true);
        Check(core.Phase() == ProxyPhase::Restoring && fake.showCalls == 1,
            "rapid reroute and stale UI acknowledgement cannot repeat native show");
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Armed && core.Snapshot().lensId == 3 &&
            core.Snapshot().generation == 3 && fake.visible && fake.showCalls == 1,
            "only newest standby lens is armed after the old session fully restores");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        ProxyMouseEvent gesture{ WM_LBUTTONDOWN };
        gesture.controlDown = true;
        core.Intercept(gesture); core.Pump();
        Check(!core.Intercept({ WM_LBUTTONUP }), "quick gesture release becomes native after recovery gate opens");
        FinishRestore(core, fake);
        Check(!fake.snapshot.localMoveRequested && core.Phase() == ProxyPhase::Armed,
            "quick release before UI acknowledgement cannot start a stuck local drag");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; auto fullscreen = Config(); fullscreen.fullscreen = true;
        Enter(core, fake, fullscreen);
        ProxyMouseEvent gesture{ WM_LBUTTONDOWN };
        gesture.controlDown = true;
        Check(core.Intercept(gesture), "fullscreen Ctrl-left remains mapped input");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Active && fake.packets.back().message == WM_LBUTTONDOWN &&
            !fake.snapshot.localMoveRequested,
            "fullscreen cannot move and preserves ordinary source click behavior");
        Event(core, WM_LBUTTONUP);
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        fake.settled = false; core.Disable();
        Check(fake.requests.back().transparent && !fake.visible, "do not restore styles/cursor before injected UPs are acknowledged");
        fake.settled = true; FinishRestore(core, fake);
        Check(fake.visible && core.Phase() == ProxyPhase::Off, "injection acknowledgement completes restore");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        fake.failSendAt = fake.sendCount + 1;
        Event(core, WM_LBUTTONDOWN);
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible, "partial DOWN injection failure restores cursor");
        Check(std::any_of(fake.packets.begin(), fake.packets.end(), [](auto p) { return p.message == WM_LBUTTONUP; }),
            "partial DOWN failure still sends matching UP");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); Event(core, WM_LBUTTONDOWN);
        fake.failSendAt = fake.sendCount + 1;
        core.Disable(); FinishRestore(core, fake);
        Check(fake.recoveryFailed && fake.recoveryActive, "failed cleanup leaves recovery obligation with watchdog");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); Event(core, WM_LBUTTONDOWN);
        fake.healthy = false; core.Pump(); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible, "lost guardian or desktop restores state");
    }
    {
        Fake fake; fake.healthy = false; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        Check(core.Phase() == ProxyPhase::Failed && fake.packets.empty() && fake.visible, "missing watchdog prohibits takeover");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        Check(!core.Intercept({ WM_LBUTTONDOWN, { 10, 10 } }), "outside button remains native");
        Check(!core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }), "do not steal native drag entering lens");
        core.Intercept({ WM_LBUTTONUP, { 350, 350 } });
        Check(!core.Intercept({ WM_MOUSEMOVE, { 105, 200 } }), "resize edge stays local");
        Check(!core.Intercept({ WM_MOUSEMOVE, { 772, 120 } }), "mapping button stays local");
    }
    {
        auto aspect = AspectFitConfig();
        RECT fullLocal{ 0, 0, 1000, 800 };
        auto realButton = ChromeButtonCenterFor(FullscreenButtonId, fullLocal);
        realButton.x += aspect.lensClient.left; realButton.y += aspect.lensClient.top;
        RECT contentLocal{ 0, 0, 1000, 750 };
        auto fakeContentButton = ChromeButtonCenterFor(FullscreenButtonId, contentLocal);
        fakeContentButton.x += aspect.destination.left; fakeContentButton.y += aspect.destination.top;
        Check(ProxyLocalControl(aspect, realButton),
            "aspect-fit chrome is hit-tested against the complete lens client");
        Check(!ProxyLocalControl(aspect, fakeContentButton) && PtInRect(&aspect.destination, fakeContentButton),
            "aspect-fit content top-right does not create a fake toolbar");

        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(aspect);
        Check(!core.Intercept({ WM_MOUSEMOVE, { 350, 110 } }), "letterbox bar movement remains native");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Armed && fake.requests.empty(),
            "letterbox bars never activate mapping");
        Check(!core.Intercept({ WM_MOUSEMOVE, realButton }), "real fullscreen toolbar remains native");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Armed && fake.requests.empty(),
            "real toolbar cannot arm through a black bar");
        Check(!core.Intercept({ WM_MOUSEMOVE, fakeContentButton }), "content entry is native until exposure is verified");
        core.Pump();
        Check(core.Phase() == ProxyPhase::Preparing && !fake.requests.empty(),
            "former content-relative toolbar location is usable mapped content");
        core.SurfaceAcknowledged(fake.requests.back().id, true);
        Event(core, WM_MOUSEMOVE, 0, -1000);
        Check(core.Phase() == ProxyPhase::Restoring && core.InputReleased(),
            "moving from aspect-fit content into a black bar restores ordinary input");
        FinishRestore(core, fake);
    }
    {
        auto aspect = AspectFitConfig();
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake, aspect);
        Event(core, WM_LBUTTONDOWN);
        Event(core, WM_MOUSEMOVE, -10000, 10000);
        Check(core.Phase() == ProxyPhase::Active && core.Snapshot().position.x == aspect.destination.left &&
            core.Snapshot().position.y == aspect.destination.bottom - 1,
            "held fullscreen drag clamps to the visible aspect-fit picture, not the black bars");
        Event(core, WM_LBUTTONUP);
        core.Disable(); FinishRestore(core, fake);
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        auto old = core.SurfaceRequest();
        core.Configure(Config(2));
        core.SurfaceAcknowledged(old, true);
        Check(core.Phase() == ProxyPhase::Restoring, "geometry change invalidates earlier prepare");
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Armed && core.Snapshot().generation == 2 && !core.Snapshot().visible,
            "new geometry waits for old session restoration");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump(); auto prepare = core.SurfaceRequest();
        core.Disable(); core.SurfaceAcknowledged(prepare, true); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Off && fake.packets.empty(), "cancel during preparation never hides or injects");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        for (size_t i = 0; i <= ProxyEventQueue::Capacity; ++i) core.Intercept({ WM_MOUSEWHEEL });
        core.Pump(); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible, "queue overflow fails closed");
    }
    {
        Fake fake; fake.source = false; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;
        core.Configure(config); core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        core.SurfaceAcknowledged(core.SurfaceRequest(), true); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.packets.empty(), "unready source cannot receive a click");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump(); auto stale = core.SurfaceRequest();
        fake.tick = 1001; core.Pump();
        Check(core.InputReleased() && fake.visible && fake.packets.empty(), "UI preparation timeout releases input without UI ack");
        Check(!core.Intercept({ WM_LBUTTONDOWN, { 350, 350 } }), "hung UI cannot trap ordinary desktop clicks");
        core.SurfaceAcknowledged(stale, true);
        Check(fake.visible && fake.packets.empty(), "late ready cannot revive timed-out capture");
        FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed, "prepare timeout disables mapping");
    }
    {
        Fake fake; fake.canEnter = false; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        for (LONG i = 0; i < 20; ++i)
        {
            Check(!core.Intercept({ WM_MOUSEMOVE, { 350 + i, 350 } }), "covered destination receives native movement");
            core.Pump();
        }
        Check(core.Phase() == ProxyPhase::Armed && fake.packets.empty() && fake.requests.empty(), "foreign occluder is never captured");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.SeedPhysicalButtons(1); core.Configure(config);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        Check(core.Phase() == ProxyPhase::Armed, "button held before hook installation prevents takeover");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        Event(core, WM_MOUSEMOVE, 422, -230);
        Check(core.Phase() == ProxyPhase::Restoring && core.InputReleased(), "entering toolbar restores ordinary cursor");
        Check(!core.Intercept({ WM_LBUTTONDOWN, { 772, 120 } }), "toolbar clicks are not swallowed while restore UI ack is pending");
        FinishRestore(core, fake);
    }
    // Exercise the actual asynchronous handoff for all five controls, both
    // windowed and fullscreen. No real mouse or cursor API is called here.
    for (bool fullscreen : { false, true })
    {
        for (LONG offset : { 200L, 164L, 128L, 92L, 56L, 20L })
        {
            AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;
            auto toolbarConfig = config; toolbarConfig.fullscreen = fullscreen;
            Enter(core, fake, toolbarConfig);
            fake.Complete(core);
            POINT button{ config.destination.right - offset, config.destination.top + 20 };
            Event(core, WM_MOUSEMOVE, button.x - 350, button.y - 350);
            Check(core.Phase() == ProxyPhase::Restoring && fake.snapshot.visible && fake.visible &&
                fake.snapshot.position.x == button.x && fake.snapshot.position.y == button.y,
                "toolbar handoff retains virtual cursor while native return move is in flight");
            Check(fake.requests.back().transparent && !core.InputReleased() && fake.showCalls == 1 && fake.visible,
                "native cursor is shown before the return move while window hits remain transparent");
            Check(fake.packets.size() == 2 && fake.packets.back().message == WM_MOUSEMOVE &&
                fake.packets.back().point.x == button.x && fake.packets.back().point.y == button.y,
                "toolbar movement returns to its destination, not a mapped source control");
            auto stale = core.SurfaceRequest() - 1;
            core.SurfaceAcknowledged(stale, true);
            Check(fake.snapshot.visible, "stale UI ack cannot remove the handoff cursor");
            fake.Complete(core);
            Check(fake.visible && !fake.snapshot.visible && fake.showCalls == 1 && core.InputReleased() &&
                !fake.requests.back().transparent,
                "native show precedes removal of virtual cursor and request for local window hits");
            Check(fake.handoff == std::vector<char>{ 'S', 'M', 'O', 'V' },
                "toolbar handoff shows before its only return move and withdraws virtual cursor afterward");
            Check(!core.Intercept({ WM_LBUTTONDOWN, button }) && !core.Intercept({ WM_LBUTTONUP, button }),
                "toolbar click remains native during surface restore acknowledgement");
            FinishRestore(core, fake);
            Check(!core.Intercept({ WM_MOUSEMOVE, button }), "hovering a button cannot reenter mapping");
            core.Pump();
            Check(core.Phase() == ProxyPhase::Armed && fake.packets.size() == 2 && !fake.snapshot.visible,
                "all toolbar actions stay out of the source input queue");
            Check(!core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }), "content reentry starts from native movement");
            core.Pump();
            core.SurfaceAcknowledged(core.SurfaceRequest(), true);
            Check(core.Phase() == ProxyPhase::Active && fake.snapshot.visible && !fake.visible && fake.hideCalls == 2,
                "moving back from chrome resumes exactly one virtual cursor");
        }
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; auto fullscreen = config; fullscreen.fullscreen = true;
        Enter(core, fake, fullscreen); Event(core, WM_LBUTTONDOWN); Event(core, WM_MOUSEMOVE, -10000, 10000);
        Check(core.Snapshot().position.x == 100 && core.Snapshot().position.y == 699, "fullscreen drag clamps to full image bounds");
        core.Disable(); FinishRestore(core, fake);
        fullscreen.blocked = true; core.Configure(fullscreen);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        Check(core.Phase() == ProxyPhase::Armed, "geometry transition cannot start takeover");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); fake.failShow = true;
        core.Disable(); FinishRestore(core, fake);
        Check(fake.recoveryFailed && fake.cursorPending && !fake.recoveryActive && !fake.inputActiveAtRecoveryFailure &&
            core.InputReleased(), "cursor-only failure keeps its own duty, not already-released input");
        Check(fake.showCalls == 1 && fake.handoff == std::vector<char>{ 'S', 'M', 'O', 'V' },
            "failed show is attempted once before the return move and leaves bounded recovery to guardian");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        fake.failSendAt = fake.sendCount + 1; // Fail the one return move, not initial source movement.
        core.Disable(); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.recoveryFailed && fake.showCalls == 1 &&
            fake.handoff == std::vector<char>{ 'S', 'M', 'O', 'V' },
            "failed return move never replays or postpones the visibility handoff");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config); core.Disable(); core.Disable();
        Check(!fake.showCalls && !fake.hideCalls && fake.packets.empty(), "never-used mapping cannot change global cursor state");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        Event(core, WM_MOUSEMOVE, 1000, 0); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Armed, "ordinary exit remains armed");
        fake.visible = false; // Lost system cursor AFTER the initial API said success.
        auto sent = fake.packets.size(); auto shows = fake.showCalls;
        core.Disable(ERROR_CANCELLED, ProxyFaultSite::HealthCheck);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible && fake.showCalls == shows + 1,
            "global shutdown while armed reasserts visibility even after successful normal exit");
        Check(fake.packets.size() == sent && !fake.recoveryActive && core.InputReleased(),
            "visibility-only shutdown never replays UPs or restores an obsolete position");
        fake.visible = false; core.Disable(ERROR_CANCELLED, ProxyFaultSite::HealthCheck);
        Check(fake.visible && fake.showCalls == shows + 2 && fake.packets.size() == sent,
            "repeat shutdown after mapping failed still repairs cursor without input");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); core.Disable(); FinishRestore(core, fake);
        auto sent = fake.packets.size(); auto shows = fake.showCalls;
        fake.visible = false; core.Disable();
        Check(core.Phase() == ProxyPhase::Off && fake.visible && fake.showCalls == shows + 1 && fake.packets.size() == sent,
            "explicit disable after Off retains cursor hide history without replaying input");
        core.ResetAfterStop(); core.Disable();
        Check(fake.showCalls == shows + 1, "a stopped worker does not inherit an old cursor obligation");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        fake.failHide = true; core.SurfaceAcknowledged(core.SurfaceRequest(), true); FinishRestore(core, fake);
        Check(fake.hideCalls == 1 && fake.showCalls == 1 && fake.visible && !fake.cursorPending,
            "a failed/uncertain hide attempt is explicitly undone");
    }
    Check(ClassifyProxyInput(LLMHF_INJECTED, 77, 77) == ProxyInputOrigin::OwnInjection &&
        ClassifyProxyInput(0, 77, 77) == ProxyInputOrigin::Hardware &&
        ClassifyProxyInput(LLMHF_INJECTED, 88, 77) == ProxyInputOrigin::ForeignInjection,
        "own injected movement bypasses core; unrelated injection is distinct");
 #ifndef REGIONLENS_PRODUCTION_SOURCE
    {
        // Reproduce the observed logical chain without calling native input:
        // initial send fails, then health cancellation, then a cleanup failure.
        Fake fake; fake.failSendAt = 1;
        auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; core.Configure(config);
        core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        fake.settled = false; // The failing initial call leaves input pending, not the Armed entry move.
        core.SurfaceAcknowledged(core.SurfaceRequest(), true);
        Check(core.Phase() == ProxyPhase::Restoring && core.Snapshot().error == ERROR_WRITE_FAULT &&
            core.Snapshot().errorSite == ProxyFaultSite::InitialSend, "initial send failure retains site and error 29");
        fake.healthy = false;
        for (int repeat = 0; repeat < 1000; ++repeat) core.Pump();
        Check(core.Snapshot().error == ERROR_WRITE_FAULT && core.Snapshot().secondaryError == ERROR_CANCELLED &&
            core.Snapshot().secondarySite == ProxyFaultSite::HealthCheck, "1223 cannot overwrite the original send failure");
        Check(fake.faults.size() == 2 && fake.faults[0].first && !fake.faults[1].first,
            "repeated health cancellation does not flood diagnostic records");
        fake.settled = true; core.Pump(); core.SurfaceAcknowledged(core.SurfaceRequest(), false);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible && core.InputReleased(),
            "diagnostic changes preserve fail-closed recovery and cursor release");
        Check(fake.faults.size() == 3 && fake.faults[2].site == ProxyFaultSite::SurfaceRestore &&
            core.Snapshot().error == ERROR_WRITE_FAULT, "additional cleanup errors logged without replacing the first failure");
        fake.healthy = true; core.Configure(Config(2));
        Check(core.Snapshot().error == 0 && core.Snapshot().secondaryError == 0,
            "fresh configuration resets failure history");
    }
#endif
    {
        ULONG_PTR cookie = 77;
        unsigned calls{};
        Check(!CreateProxyInputCookie(cookie, [&](uint32_t& candidate)
            { ++calls; candidate = 99; return false; }) && cookie == 0 && calls == 1,
            "RNG failure discards partial data and stale cookie without a fixed fallback");
        calls = 0; cookie = 77;
        Check(!CreateProxyInputCookie(cookie, [&](uint32_t& candidate)
            { ++calls; candidate = 0; return true; }) && cookie == 0 && calls == 8,
            "repeated zero RNG output fails closed with bounded retries");
        calls = 0;
        Check(CreateProxyInputCookie(cookie, [&](uint32_t& candidate)
            { candidate = ++calls < 3 ? 0 : UINT32_MAX; return true; }) && cookie == UINT32_MAX && calls == 3,
            "zero candidates are retried and unsigned high bit is not sign-extended");
        calls = 0;
        Check(!CreateProxyInputCookie(cookie, [&](uint32_t& candidate)
            { candidate = 0; return ++calls == 1; }) && cookie == 0 && calls == 2,
            "RNG failure during zero retry clears output and stops immediately");
        Check(!ValidProxyInputCookie(0) && ValidProxyInputCookie(1) && ValidProxyInputCookie(UINT32_MAX),
            "cookie validity is exactly the nonzero unsigned 32-bit range");
        Check(ClassifyProxyInput(LLMHF_INJECTED, 0, 0) == ProxyInputOrigin::ForeignInjection,
            "an absent session cookie never matches an untagged injection");
        Check(MakeProxyInputs({}, WM_MOUSEMOVE, 0, config.desktop, 0).count == 0 &&
            MakeProxyRecoveryInputs({}, {}, 31, config.desktop, 0).count == 0,
            "invalid cookie cannot emit normal or guardian packets");
    }
    for (uint32_t sample : { 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu,
        3352636821u, 3557258868u, 4015191543u, 3966832260u, 3902654071u })
    {
        // The last five values are the low halves captured in the five user
        // reproductions. The old tag's high half was 1380741089, received as 0.
        ULONG_PTR cookie{};
        Check(CreateProxyInputCookie(cookie, [=](uint32_t& candidate) { candidate = sample; return true; }) &&
            uint64_t(cookie) == sample, "production cookie generator preserves all 32 random bits and zero upper half");
        if constexpr (sizeof(ULONG_PTR) > sizeof(uint32_t))
        {
            auto oldCookie = static_cast<ULONG_PTR>((uint64_t(1380741089) << 32) | sample);
            Check(ClassifyProxyInput(LLMHF_INJECTED, ULONG_PTR(sample), oldCookie) == ProxyInputOrigin::ForeignInjection,
                "observed legacy full-width tag cannot match its truncated callback");
            Check(ClassifyProxyInput(LLMHF_INJECTED, oldCookie, cookie) == ProxyInputOrigin::ForeignInjection &&
                !ValidProxyInputCookie(oldCookie), "foreign upper bits are rejected, not masked away");
            Check(MakeProxyInputs({}, WM_MOUSEMOVE, 0, config.desktop, oldCookie).count == 0 &&
                MakeProxyRecoveryInputs({}, {}, 31, config.desktop, oldCookie).count == 0,
                "legacy high-bit cookies are rejected by both injection packet builders");
        }
        for (UINT message : { WM_MOUSEMOVE, WM_LBUTTONDOWN, WM_LBUTTONUP, WM_RBUTTONDOWN, WM_RBUTTONUP,
            WM_MBUTTONDOWN, WM_MBUTTONUP, WM_XBUTTONDOWN, WM_XBUTTONUP, WM_MOUSEWHEEL, WM_MOUSEHWHEEL })
        {
            auto batch = MakeProxyInputs({}, message, XBUTTON2 << 16, config.desktop, cookie);
            Check(batch.count == (message == WM_MOUSEMOVE ? 1u : 2u), "wire-tag test creates expected mouse packet count");
            for (UINT i = 0; i < batch.count; ++i)
            {
                auto tag = batch.inputs[i].mi.dwExtraInfo;
                auto received = static_cast<ULONG_PTR>(uint32_t(tag));
                Check(tag == cookie && ClassifyProxyInput(LLMHF_INJECTED, received, cookie) == ProxyInputOrigin::OwnInjection,
                    "every move/button/wheel callback survives observed 32-bit transport with full equality");
                Check(ClassifyProxyInput(0, received, cookie) == ProxyInputOrigin::Hardware &&
                    ClassifyProxyInput(LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED, received, cookie) == ProxyInputOrigin::OwnInjection,
                    "hardware flag distinction and injected flag combinations remain intact");
            }
        }
        ProxyRecoveryShared shared;
        shared.cookie = cookie;
        auto recovery = MakeProxyRecoveryInputs({ -1000, -500 }, { 350, 350 }, 31, config.desktop, shared.cookie);
        Check(shared.magic == 0x524C5037 && recovery.count == 7,
            "v7 guardian state keeps tagged cursor recovery valid without a hotkey");
        for (UINT i = 0; i < recovery.count; ++i)
            Check(recovery.inputs[i].mi.dwExtraInfo == cookie && ClassifyProxyInput(LLMHF_INJECTED,
                ULONG_PTR(uint32_t(recovery.inputs[i].mi.dwExtraInfo)), cookie) == ProxyInputOrigin::OwnInjection,
                "guardian releases and cursor restoration are recognized after 32-bit transport");
        Check(ClassifyProxyInput(LLMHF_INJECTED, cookie ^ 1, cookie) == ProxyInputOrigin::ForeignInjection,
            "unrelated or stale cookie still follows foreign-input protection");

        CookieLoopback fake; fake.cookie = cookie; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage;
        Enter(core, fake);
        Check(fake.pending == 0 && fake.acknowledged == fake.wireInputs && fake.acknowledged == 1 &&
            !fake.foreign && fake.healthy,
            "initial move is acknowledged during fake send without cancelling the session");
        for (int i = 0; i < 100; ++i) Event(core, WM_MOUSEMOVE, 0.25, -0.25);
        Event(core, WM_LBUTTONDOWN); Event(core, WM_LBUTTONUP);
        Event(core, WM_LBUTTONDOWN); Event(core, WM_LBUTTONUP);
        Event(core, WM_MOUSEWHEEL, 0, 0, 120 << 16);
        Event(core, WM_MOUSEHWHEEL, 0, 0, DWORD(WORD(SHORT(-120))) << 16);
        Check(core.Phase() == ProxyPhase::Active && core.Snapshot().position.x == 375 &&
            core.Snapshot().position.y == 325 && fake.pending == 0 && fake.acknowledged == fake.wireInputs &&
            fake.acknowledged < 113 && !fake.foreign,
            "same-source movement is elided while clicks/wheels retain complete tagged input");
        Event(core, WM_XBUTTONDOWN, 0, 0, XBUTTON1 << 16);
        core.Disable(); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Off && fake.visible && !fake.recoveryActive &&
            fake.pending == 0 && fake.acknowledged == fake.wireInputs && fake.foreign == 0 && core.Snapshot().error == 0,
            "side-button release and cursor restoration finish without spurious cancellation or ack timeout");
    }
    {
        ProxyPointerBaseline pointer; pointer.Passed({ 350, 350 });
        double virtualX = 350, virtualY = 350;
        for (LONG i = 0; i < 100; ++i)
        {
            POINT sourcePoint{ -1000 + i, -500 + i };
            pointer.Passed(sourcePoint); // Own injected warp only rebases physical position.
            for (int j = 0; j < 2; ++j)
            {
                auto hardware = pointer.Hardware(WM_MOUSEMOVE, { sourcePoint.x + 1, sourcePoint.y - 1 }, 0);
                virtualX += hardware.dx; virtualY += hardware.dy;
                // The two suppressed hardware moves do not move the OS baseline.
            }
        }
        Check(virtualX == 550 && virtualY == 150, "injected source warps and repeated suppressed points add no drift or speed gain");
        pointer.Passed({ 550, 150 }); // Restoration to the virtual position.
        auto native = pointer.Hardware(WM_MOUSEMOVE, { 551, 149 }, 0);
        Check(native.dx == 1 && native.dy == -1, "restoration rebase preserves normal movement");
    }
    Check(ProxyShouldCancel(false, false, false, true, 500, 500), "parent exit triggers recovery without waiting heartbeat");
    Check(ProxyShouldCancel(true, false, false, true, 1501, 500), "stalled input thread invalidates session");
    Check(ProxyShouldCancel(true, false, true, true, 1502, 1502), "resumed heartbeat cannot revive cancelled session");
    Check(ProxyShouldCancel(true, true, false, true, 500, 500), "guardian shutdown restores active input");
    Check(!ProxyShouldCancel(true, false, false, false, 5000, 500), "bootstrap with no captured input is not a stall");
    auto recovery = MakeProxyRecoveryInputs({ -1000, -500 }, { 350, 350 }, 31, config.desktop, 77);
    Check(recovery.count == 7 && recovery.inputs[1].mi.dwFlags == MOUSEEVENTF_LEFTUP &&
        recovery.inputs[5].mi.dwFlags == MOUSEEVENTF_XUP && recovery.inputs[5].mi.mouseData == XBUTTON2 &&
        recovery.inputs[6].mi.dx == ProxyAbsoluteAxis(350, config.desktop.left, config.desktop.right),
        "guardian recovery packet releases all buttons before restoring destination");
    for (UINT i = 0; i < recovery.count; ++i) Check(recovery.inputs[i].mi.dwExtraInfo == 77, "recovery input is tagged");
    // DPI scale factors deliberately never enter the physical-pixel transform.
    for (double dpi : { 1.0, 1.25, 1.5, 2.0 })
    {
        auto p = ProxyMapPoint(100 + 120 * dpi, 100 + 80 * dpi,
            { 100, 100, LONG(101 + 400 * dpi), LONG(101 + 300 * dpi) }, { -1500, 0, -699, 601 });
        Check(p.x == -1260 && p.y == 160, "mixed DPI physical rectangles yield consistent source coordinates");
    }
    {
        // A software cursor must not wait behind the previous source injection.
        // All operations remain inside the fake asynchronous channel.
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); fake.Complete(core);
        Event(core, WM_MOUSEMOVE, 2, 0);
        auto sends = fake.packets.size();
        Check(core.Intercept({ WM_MOUSEMOVE, {}, 20, -10 }), "queued preview move is consumed");
        core.Pump();
        Check(!fake.InputSettled() && fake.packets.size() == sends &&
            core.Snapshot().position.x == 372 && core.Snapshot().position.y == 340,
            "virtual cursor follows hardware while an older source move is still in flight");
        fake.Complete(core);
        Check(fake.packets.size() == sends + 1,
            "latest coalesced source position follows the completed injection without an event backlog");
        fake.Complete(core);
    }
    {
        Fake fake; auto magnified = config;
        magnified.source = { -1000, -500, -920, -440 };
        auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake, magnified);
        auto sends = fake.packets.size();
        Event(core, WM_MOUSEMOVE, 1, 0);
        Check(core.Snapshot().position.x == 351 && fake.packets.size() == sends,
            "magnified destination motion does not inject a redundant identical source pixel");
    }
    {
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        auto initialChecks = fake.exposureCalls;
        for (int i = 0; i < 100; ++i) Event(core, WM_MOUSEMOVE, 0.01, 0);
        Check(fake.exposureCalls <= initialChecks + 1,
            "top-level window enumeration is throttled during a high-rate movement burst");
        fake.tick = 51; Event(core, WM_MOUSEMOVE, 0.01, 0);
        Check(fake.exposureCalls == initialChecks + 1,
            "occlusion safety is sampled again after the bounded latency interval");
    }
    {
        ProxySendChannel channel;
        auto batch = MakeProxyInputs({ -1500, 100 }, WM_MOUSEWHEEL, 120 << 16, config.desktop, 123);
        ProxySendJob job; job.count = batch.count; job.message = WM_MOUSEWHEEL; job.tick = 100; job.sequence = 1;
        std::copy_n(batch.inputs.begin(), batch.count, job.inputs.begin());
        Check(channel.Submit(job, 456), "bounded async channel accepts a wheel batch");
        Check(!channel.Submit(job, 456), "outstanding send cannot be overwritten by another event");
        std::promise<void> started, resume;
        auto startedFuture = started.get_future(); auto resumeFuture = resume.get_future();
        std::thread sender([&]
        {
            ProxySendJob outgoing;
            bool taken = channel.Take(outgoing);
            started.set_value(); resumeFuture.wait(); // Models a blocked SendInput; NO native APIs.
            channel.Complete({ taken ? outgoing.count : 0u, taken ? 0ul : ERROR_WRITE_FAULT, 313 });
        });
        Check(startedFuture.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
            "fake native call runs on a separate thread");
        ProxyEventQueue arrivals;
        for (int i = 0; i < 100; ++i) Check(arrivals.Push({ WM_MOUSEWHEEL }), "hook-side wheel queue works while sender is blocked");
        ProxySendResult result;
        channel.Observe(WM_MOUSEMOVE, 0, 456);
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 123);
        channel.Observe(WM_MOUSEWHEEL, LLMHF_INJECTED, 456);
        Check(!channel.FenceSeen() && !channel.Poll(413, result), "hardware/payload/wheel cannot acknowledge terminal fence");
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 456);
        Check(channel.FenceSeen() && !channel.Poll(413, result), "fence alone does not complete a still-blocked API call");
        resume.set_value(); sender.join();
        Check(channel.Poll(413, result) && !result.error && result.sent == 3 && !channel.Busy(),
            "missing wheel callbacks do not leak counters once API and terminal move finish");
        Check(!channel.Poll(413, result), "completion is consumed only once");
        for (int i = 0; i < 300; ++i)
        {
            job.tick = 1000 + i * 10;
            Check(channel.Submit(job, 456), "successive wheels do not accumulate acknowledgement debt");
            ProxySendJob outgoing; Check(channel.Take(outgoing), "sender serializes wheel batches");
            Check(outgoing.count == 3 && outgoing.inputs[1].mi.dwFlags == MOUSEEVENTF_WHEEL &&
                outgoing.inputs[2].mi.dwExtraInfo == 456 && !(outgoing.inputs[2].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE),
                "wheel stays between source move and a coalescing-safe independently tagged terminal move");
            channel.Complete({ outgoing.count, 0, 0 });
            Check(!channel.Poll(job.tick + 1, result), "successful SendInput is not sufficient without terminal receipt");
            channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 456);
            Check(channel.Poll(job.tick + 2, result) && !result.error, "delayed terminal receipt completes only its own serial batch");
        }
        job.tick = 10000; Check(channel.Submit(job, 456), "submit missing-fence case");
        ProxySendJob outgoing; channel.Take(outgoing); channel.Complete({ outgoing.count, 0, 0 });
        Check(!channel.Poll(10500, result) && channel.Poll(10501, result) && result.error == ERROR_TIMEOUT,
            "missing fence remains a real bounded failure, not a silently cleared count");
        job.tick = 11000; channel.Submit(job, 456); channel.Take(outgoing);
        channel.Complete({ 1, ERROR_ACCESS_DENIED, 3 });
        Check(channel.Poll(11003, result) && result.error == ERROR_ACCESS_DENIED,
            "partial send propagates the original API error without waiting for an impossible fence");
        job.tick = 12000; channel.Submit(job, 456); channel.Take(outgoing);
        Check(channel.Expired(12501) && !channel.Poll(12501, result) && channel.Busy(),
            "a timed-out running native call cannot be forgotten or overwritten");
        channel.Complete({ 0, ERROR_CANCELLED, 600 });
        Check(channel.Poll(12600, result) && result.error == ERROR_CANCELLED, "late cancelled sender result releases its slot safely");

        auto moveBatch = MakeProxyInputs({ -1400, 120 }, WM_MOUSEMOVE, 0, config.desktop, 123);
        ProxySendJob moveJob; moveJob.count = moveBatch.count; moveJob.message = WM_MOUSEMOVE; moveJob.tick = 13000;
        std::copy_n(moveBatch.inputs.begin(), moveBatch.count, moveJob.inputs.begin());
        MarkRecoveryReturnMove(moveJob);
        Check(channel.Submit(moveJob, 456) && channel.Take(outgoing) && outgoing.count == 1 && !outgoing.requiresFence,
            "ordinary movement omits the duplicate terminal fence");
        channel.Complete({ outgoing.count, 0, 0 });
        Check(!channel.Poll(13000, result),
            "ordinary movement still waits for its own payload callback");
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 123);
        Check(channel.Poll(13000, result) && !result.error,
            "payload receipt frees the low-latency slot without a second injected move");
        Check(!(outgoing.inputs[0].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE),
            "injected movement preserves normal Windows latest-position coalescing");

        auto returnJob = moveJob; returnJob.recovery = true; returnJob.tick = 13100;
        MarkRecoveryReturnMove(returnJob);
        Check(channel.Submit(returnJob, 456) && channel.Take(outgoing) && outgoing.count == 2 &&
            outgoing.requiresFence && (outgoing.inputs[0].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE) &&
            !(outgoing.inputs[1].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE) &&
            outgoing.inputs[0].mi.dx == outgoing.inputs[1].mi.dx &&
            outgoing.inputs[0].mi.dy == outgoing.inputs[1].mi.dy,
            "only the existing recovery return payload is non-coalesced; no new movement or position is added");
        channel.Complete({ outgoing.count, 0, 0 });
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 123);
        Check(!channel.Poll(13100, result), "return payload alone cannot acknowledge the recovery fence");
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 456);
        Check(channel.Poll(13100, result) && !result.error,
            "non-coalesced return still completes through the original ordered fence");

        auto releaseBatch = MakeProxyInputs({ -1400, 120 }, WM_LBUTTONUP, 0, config.desktop, 123);
        ProxySendJob releaseJob; releaseJob.recovery = true; releaseJob.message = WM_LBUTTONUP;
        releaseJob.count = releaseBatch.count; releaseJob.tick = 13200;
        std::copy_n(releaseBatch.inputs.begin(), releaseBatch.count, releaseJob.inputs.begin());
        MarkRecoveryReturnMove(releaseJob);
        Check(channel.Submit(releaseJob, 456) && channel.Take(outgoing) && outgoing.count == 3 &&
            !(outgoing.inputs[0].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE) &&
            !(outgoing.inputs[2].mi.dwFlags & MOUSEEVENTF_MOVE_NOCOALESCE),
            "recovery button release and its fence retain normal coalescing behavior");
        channel.Complete({ outgoing.count, 0, 0 });
        channel.Observe(WM_MOUSEMOVE, LLMHF_INJECTED, 456);
        Check(channel.Poll(13200, result) && !result.error,
            "unchanged recovery button pairing remains acknowledged");
    }
    {
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        for (int i = 0; i < 80; ++i) Event(core, WM_MOUSEWHEEL, 0, 0, DWORD(WORD(SHORT(i % 2 ? -120 : 120))) << 16);
        Check(fake.packets.size() == 1, "no new batch is sent before the initial async move finishes");
        for (int i = 0; i < 81; ++i) fake.Complete(core);
        Check(core.Phase() == ProxyPhase::Active && fake.packets.size() == 81 && fake.InputSettled(),
            "wheel backlog drains in order without false cancellation");
        for (int i = 0; i < 80; ++i)
            Check(SHORT(HIWORD(fake.packets[i + 1].data)) == (i % 2 ? -120 : 120), "queued wheel direction and count are preserved");
        Event(core, WM_LBUTTONDOWN);
        core.Disable();
        Check(fake.recoveryButtons == 1 && !fake.visible, "shutdown retains pending DOWN recovery obligation");
        fake.Complete(core); // DOWN finishes; UP now scheduled.
        Check(fake.packets.back().message == WM_LBUTTONUP && fake.recoveryButtons == 1,
            "release waits behind pending DOWN and remains owned until acknowledged");
        fake.Complete(core); // UP finishes; restore move scheduled.
        Check(fake.packets.back().message == WM_MOUSEMOVE && fake.visible && fake.snapshot.visible,
            "button UP settles before native show and return move while virtual cursor remains");
        auto handoffSize = fake.handoff.size();
        for (int i = 0; i < 8; ++i) { core.Pump(); core.Disable(); }
        Check(fake.handoff.size() == handoffSize && fake.showCalls == 1 && fake.snapshot.visible,
            "busy return movement and repeated shutdown do not replay show or remove virtual cursor");
        fake.Complete(core); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Off && fake.visible && !fake.recoveryActive,
            "async shutdown discharges input before restoring hit testing");
        Check(fake.handoff == std::vector<char>{ 'U', 'S', 'M', 'O', 'V' },
            "asynchronous recovery releases button, shows native, returns once, then removes virtual cursor");
    }
    {
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); fake.Complete(core);
        Event(core, WM_LBUTTONDOWN); fake.Complete(core);
        Event(core, WM_LBUTTONUP); fake.Complete(core, ERROR_WRITE_FAULT);
        Check(core.Phase() == ProxyPhase::Restoring && fake.recoveryButtons == 1 &&
            fake.packets.back().message == WM_LBUTTONUP, "partial async UP retains a matching release obligation");
        fake.Complete(core); fake.Complete(core); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible, "async failure restores input and reports failure");
    }
    {
        MappingGeometryGate gate;
        Fake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        using Action = MappingGeometryGate::Action;
        Check(gate.Update(true) == Action::Pause, "WM_ENTERSIZEMOVE pauses once");
        core.Disable(); FinishRestore(core, fake);
        auto count = fake.packets.size();
        for (int i = 0; i < 1000; ++i)
        {
            Check(gate.Update(true) == Action::Wait, "intermediate geometry does not disable/configure again");
            core.Intercept({ WM_MOUSEMOVE, { 350, 350 } }); core.Pump();
        }
        Check(core.Phase() == ProxyPhase::Off && fake.packets.size() == count,
            "normal resize mouse input is not taken over by a blocked Armed session");
        Check(gate.Update(false) == Action::Resume, "WM_EXITSIZEMOVE submits final geometry once");
        auto resized = Config(2); resized.destination = { 200, 200, 1200, 1000 };
        core.Configure(resized); core.Intercept({ WM_MOUSEMOVE, { 500, 500 } }); core.Pump();
        core.SurfaceAcknowledged(core.SurfaceRequest(), true);
        auto expected = ProxyMapPoint(500, 500, resized.destination, resized.source);
        Check(core.Phase() == ProxyPhase::Active && core.Snapshot().sourcePosition.x == expected.x &&
            core.Snapshot().sourcePosition.y == expected.y, "mapping resumes using the actual final physical rectangle");
        Check(gate.Update(true) == Action::Pause && gate.Update(false) == Action::Resume,
            "programmatic fullscreen geometry has the same pause/commit pair");
        Check(!MappingGeometryCanSettle(2000, 1000, true, false, false) &&
            !MappingGeometryCanSettle(2000, 1000, false, true, false) &&
            !MappingGeometryCanSettle(2000, 1000, false, false, true) &&
            !MappingGeometryCanSettle(1179, 1000, false, false, false) &&
            MappingGeometryCanSettle(1180, 1000, false, false, false),
            "missing exit-message fallback requires stable size, released button, no capture and no native move loop");
        gate.Update(true); gate.Reset();
        Check(gate.Update(false) == Action::Replace, "region switch clears prior geometry pause");
    }
    {
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake); fake.Complete(core);
        Event(core, WM_LBUTTONDOWN); fake.Complete(core);
        Event(core, WM_LBUTTONUP); core.Disable(); // Resize/close arrives before the UP result.
        fake.Complete(core);
        Check(fake.packets.back().message == WM_MOUSEMOVE && fake.recoveryButtons == 0,
            "acknowledged in-flight UP is not sent twice when resize/close intervenes");
        fake.Complete(core); FinishRestore(core, fake);
        Check(std::count_if(fake.packets.begin(), fake.packets.end(), [](auto const& p) { return p.message == WM_LBUTTONUP; }) == 1,
            "async normal UP and recovery have exactly one release");
    }
    {
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        fake.healthy = false; core.Pump();
        Check(!core.InputReleased(), "pending transport is retained before external recovery completes");
        fake.recoveredExternally = true; fake.recoveryActive = false;
        core.Pump(); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible && core.InputReleased() &&
            !core.Intercept({ WM_MOUSEMOVE, { 500, 500 } }) && fake.channel.Busy(),
            "guardian completion restores ordinary input even while old sender remains blocked");
        auto count = fake.packets.size();
        fake.Complete(core, ERROR_CANCELLED);
        Check(fake.packets.size() == count && fake.InputSettled(), "late cancelled result cannot enqueue old-session recovery or reactivate mapping");
        Check(RejectCancelledProxyPacket(LLMHF_INJECTED, 123, 123, 456, false, true) &&
            RejectCancelledProxyPacket(LLMHF_INJECTED, 456, 123, 456, false, true) &&
            !RejectCancelledProxyPacket(LLMHF_INJECTED, 789, 123, 456, false, true) &&
            !RejectCancelledProxyPacket(LLMHF_INJECTED, 456, 123, 456, true, true) &&
            !RejectCancelledProxyPacket(0, 123, 123, 456, false, true) &&
            !RejectCancelledProxyPacket(LLMHF_INJECTED, 123, 123, 456, false, false),
            "cancellation filters only old payload/fence; native hardware and guardian recovery are never filtered");
    }
    {
        AsyncFake fake; auto coreStorage=std::make_unique<MouseProxyCore>(fake); auto& core=*coreStorage; Enter(core, fake);
        for (size_t i = 0; i <= ProxyEventQueue::Capacity; ++i) core.Intercept({ WM_MOUSEWHEEL });
        core.Pump();
        Check(core.Phase() == ProxyPhase::Restoring && core.Snapshot().error == ERROR_BUFFER_OVERFLOW,
            "async sender backlog cannot silently drop an overflowing wheel/button stream");
        fake.Complete(core); fake.Complete(core); FinishRestore(core, fake);
        Check(core.Phase() == ProxyPhase::Failed && fake.visible && !fake.recoveryActive,
            "bounded async queue overflow stops and restores cleanly");
    }
    if (!failures) std::cout << "Mouse proxy tests passed (fake backend; no desktop input).\n";
    return failures;
}
