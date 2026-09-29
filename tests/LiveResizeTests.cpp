#include "LiveResizePolicy.h"
#include "LensChrome.h"
#include "MappingGeometryGate.h"
#include "LensResizePolicy.h"
#include "RegionTransform.h"
#include <cmath>
#include <iostream>

using namespace RegionLens::native;
namespace
{
    int failures{};
    void Check(bool ok, char const* name)
    { if (!ok) { ++failures; std::cerr << "FAILED live resize: " << name << '\n'; } }
    bool SameRect(RECT const& left, RECT const& right) { return EqualRect(&left, &right) != FALSE; }
    struct FakeRenderSurface
    {
        LiveResizePolicy policy;
        RenderExtent buffer{ 640, 360 };
        unsigned allocations{}, frames{};
        void Draw(RenderExtent client)
        {
            auto target = policy.BufferTarget(client, buffer);
            if (target != buffer) { buffer = target; ++allocations; }
            if (client.Valid()) ++frames;
        }
    };
}
int RunLiveResizeTests()
{
    {
        Check(SameRect(FitAspectRect({0,0,16,9},{0,0,1920,1200}),RECT{0,60,1920,1140}),
            "landscape source is centred with horizontal black bars");
        Check(SameRect(FitAspectRect({0,0,9,16},{-1920,0,0,1080}),RECT{-1264,0,-657,1080}),
            "portrait source is centred on a negative-coordinate monitor with deterministic odd bars");
        Check(SameRect(FitAspectRect({0,0,1,1},{0,0,1919,1080}),RECT{419,0,1499,1080}),
            "square and odd-width layouts put the spare pixel on the right");
        Check(SameRect(FitAspectRect({0,0,1,4096},{0,0,2,1}),RECT{0,0,1,1}),
            "extreme and single-pixel source axes retain a non-empty fitted image");
        auto empty = FitAspectRect({}, {0,0,1920,1080});
        Check(IsRectEmpty(&empty), "invalid source geometry cannot produce a fullscreen destination");

        Check(ProjectContentRect({120,0,480,360},600,360,300,200)==PixelRect{60,0,180,200},
            "square fullscreen content projects into the current transition frame");
        Check(ProjectContentRect({0,60,1920,1140},1920,1200,853,479)==PixelRect{0,24,853,431},
            "horizontal bars project inward without leaking source pixels");
        Check(ProjectContentRect({656,0,1263,1080},1919,1080,333,777)==PixelRect{114,0,105,777},
            "portrait content and odd side bars preserve deterministic inward rounding");
        Check(ProjectContentRect({0,0,1,1},2,1,1,4096)==PixelRect{0,0,1,4096},
            "single-pixel target content remains visible in an extreme transition");
        Check(ProjectContentRect({},1920,1080,640,360).Empty(),
            "empty target content cannot create a transition rectangle");
    }
    {
        FullscreenTransitionGate gate;
        Check(!gate.Active() && !gate.Ready(10), "fullscreen transition gate starts inactive");
        gate.Begin();
        Check(gate.Active() && gate.Ready(10), "fullscreen transition can present immediately");
        Check(gate.OnBusy(10) && !gate.Ready(25) && gate.Ready(26) && gate.BusyCount()==1,
            "first busy result schedules exactly one sixteen millisecond retry");
        Check(!gate.OnBusy(26) && gate.Active(),
            "second busy result cancels instead of exposing a stretched fallback");
        gate.Begin(); gate.Finish();
        Check(!gate.Active(), "hide, close or competing geometry can cancel a pending transition");
    }
    {
        CompositionResizeHandoff handoff;
        handoff.Initialize({640,360});
        Check(!handoff.Pending() && handoff.Displayed()==RenderExtent{640,360},
            "initial composition buffer is the displayed buffer");
        Check(handoff.Prepare({1280,720}) && handoff.Pending() &&
            handoff.Displayed()==RenderExtent{640,360} && handoff.Prepared()==RenderExtent{1280,720},
            "final-size allocation retains the old displayed frame until presentation");
        handoff.Prepare({1920,1080});
        Check(handoff.Displayed()==RenderExtent{640,360} && handoff.Prepared()==RenderExtent{1920,1080},
            "a newer final size replaces only the unpublished buffer");
        handoff.Commit();
        Check(!handoff.Pending() && handoff.Displayed()==RenderExtent{1920,1080},
            "one successful composition commit atomically publishes content and transform");
        handoff.Prepare({1280,720});
        Check(handoff.Prepare({1920,1080}) && handoff.Pending(),
            "replacing with the original dimensions still publishes the new resource");
        handoff.Commit();
    }
    {
        auto capacity=ReserveCompositionStorage({640,360},{1920,1080});
        Check(capacity==RenderExtent{1920,1080},"reserve display-sized composition storage");
        for(auto size : {RenderExtent{200,128},{1280,720},{1920,1080},{640,360}})
            Check(FitsCompositionStorage(size,capacity),"ordinary drag/release/fullscreen can reuse the allocation");
        Check(!FitsCompositionStorage({},capacity) && !FitsCompositionStorage({2400,700},capacity),
            "invalid or over-capacity viewports are not reused");
        Check(ReserveCompositionStorage({2400,700},{1920,1080},capacity)==RenderExtent{2400,1080},
            "exceptional growth preserves storage on the other axis");
    }
    {
        LensResizePolicy resize;
        Check(resize.Begin({100,100,500,300}), "corner resize captures the current 2:1 ratio");
        RECT bottomRight{100,100,700,500};
        Check(resize.Adjust(WMSZ_BOTTOMRIGHT,bottomRight,200,128) &&
            SameRect(bottomRight, RECT{100,100,700,400}),
            "bottom-right corner preserves ratio and the opposite corner");
        RECT topLeft{-100,-100,500,300};
        Check(resize.Adjust(WMSZ_TOPLEFT,topLeft,200,128) &&
            SameRect(topLeft, RECT{-100,0,500,300}),
            "top-left corner preserves ratio and the opposite corner");
        RECT topRight{100,-100,900,300};
        Check(resize.Adjust(WMSZ_TOPRIGHT,topRight,200,128) &&
            SameRect(topRight, RECT{100,-100,900,300}),
            "already proportional top-right proposals remain unchanged");
        RECT bottomLeft{-100,100,500,500};
        Check(resize.Adjust(WMSZ_BOTTOMLEFT,bottomLeft,200,128) &&
            SameRect(bottomLeft, RECT{-100,100,500,400}),
            "bottom-left corner uses the nearest pointer axis without ratio drift");
        for (auto edge : { UINT(WMSZ_LEFT), UINT(WMSZ_RIGHT), UINT(WMSZ_TOP), UINT(WMSZ_BOTTOM) }) {
            RECT side{100,100,900,777}; auto sideOriginal=side;
            Check(!resize.Adjust(edge,side,200,128) && SameRect(side,sideOriginal),
                "each edge drag remains independent one-axis resizing");
        }
        RECT minimum{100,100,150,150};
        Check(resize.Adjust(WMSZ_BOTTOMRIGHT,minimum,200,128) &&
            minimum.right-minimum.left==256 && minimum.bottom-minimum.top==128,
            "corner ratio honors both minimum dimensions");
        resize.End();
        RECT inactive{100,100,900,777}; auto inactiveOriginal=inactive;
        Check(!resize.Adjust(WMSZ_BOTTOMRIGHT,inactive,200,128) && SameRect(inactive,inactiveOriginal),
            "programmatic and selection-style resizing is untouched outside a native lens drag");

        Check(resize.Begin({0,0,300,300}), "a later drag captures the then-current ratio");
        RECT square{0,0,600,400};
        Check(resize.Adjust(WMSZ_BOTTOMRIGHT,square,200,128) &&
            square.right-square.left==600 && square.bottom-square.top==600,
            "corner resize preserves the current shape after a prior edge stretch");
    }
    {
        FakeRenderSurface surface;
        Check(surface.policy.Begin(true) && !surface.policy.Begin(true), "native move/size entry is idempotent");
        RenderExtent finalSize{};
        for (unsigned i = 0; i < 5000; ++i)
        {
            finalSize = { 200 + i % 3600, 128 + (i * 3) % 2000 };
            surface.Draw(finalSize);
            Check(surface.buffer == RenderExtent{ 640, 360 }, "every intermediate frame keeps buffer resolution");
        }
        Check(surface.frames == 5000 && surface.allocations == 0,
            "allocation policy permits fresh frames throughout long, non-uniform resizing");
        Check(surface.policy.End() && !surface.policy.Holding(), "release ends the resolution hold");
        surface.Draw(finalSize);
        for (int i = 0; i < 100; ++i) surface.Draw(finalSize);
        Check(surface.allocations == 1 && surface.buffer == finalSize, "only final resolution is allocated once");
        Check(!surface.policy.End(), "duplicate native exit or fallback settlement is harmless");
    }
    {
        FakeRenderSurface moved;
        moved.policy.Begin(true);
        for (int i = 0; i < 1000; ++i) moved.Draw({ 640, 360 });
        moved.policy.End(); moved.Draw({ 640, 360 });
        Check(moved.frames == 1001 && moved.allocations == 0, "translation remains live and never reallocates");
        moved.policy.Begin(true); moved.Draw({ 1500, 800 });
        moved.Draw({ 640, 360 }); moved.policy.End(); moved.Draw({ 640, 360 });
        Check(!moved.allocations, "cancelled resize returning to original dimensions needs no rebuild");
    }
    {
        FakeRenderSurface first, other;
        first.policy.Begin(true); first.Draw({ 1600, 900 }); other.Draw({ 800, 600 });
        Check(first.allocations == 0 && other.allocations == 1 && !other.policy.Holding(),
            "a resizing lens does not freeze another lens or programmatic resize");
        FakeRenderSurface overlay;
        Check(!overlay.policy.Begin(false), "HWND selection overlay does not opt in");
        overlay.Draw({ 1920, 1080 });
        Check(overlay.buffer == RenderExtent{ 1920, 1080 }, "selection keeps native-resolution rendering");
        first.policy.End(); first.Draw({ 1920, 1080 }); first.Draw({ 640, 360 });
        Check(first.allocations == 2, "fullscreen and restore after the native drag can resize normally");
    }
    {
        LiveResizePolicy policy; policy.Begin(true);
        Check(policy.BufferTarget({ 640, 360 }, {}) == RenderExtent{ 640, 360 }, "missing initial resources can still be created");
        Check(policy.BufferTarget({}, { 640, 360 }) == RenderExtent{ 640, 360 }, "minimization does not destroy held buffers");
        policy.End();
        Check(policy.BufferTarget({}, { 640, 360 }) == RenderExtent{ 640, 360 }, "zero client extent is not allocated after release");
        Check(policy.BufferTarget({ 1, 1 }, { 640, 360 }) == RenderExtent{ 1, 1 }, "single-pixel extent remains valid");
    }
    {
        // The shader uses uv * physical client extent for both chrome and
        // cursor. Non-uniform buffer scaling must not shift native hit targets.
        constexpr RenderExtent buffer{ 640, 360 };
        for (RenderExtent client : { RenderExtent{ 200, 128 }, { 1280, 720 }, { 1919, 333 }, { 420, 1800 } })
        {
            RECT bounds{ 0, 0, LONG(client.width), LONG(client.height) };
            for (auto button : ChromeButtonIds)
            {
                auto center = ChromeButtonCenterFor(button, bounds);
                double clientX = center.x, clientY = center.y;
                double u = clientX / client.width, v = clientY / client.height;
                double bufferX = u * buffer.width, bufferY = v * buffer.height;
                double displayedX = bufferX * client.width / buffer.width;
                double displayedY = bufferY * client.height / buffer.height;
                Check(std::abs(displayedX - clientX) < 0.001 && std::abs(displayedY - clientY) < 0.001 &&
                    HitTestChromeButton({ LONG(std::lround(displayedX)), LONG(std::lround(displayedY)) }, bounds) == button,
                    "scaled presentation retains physical button centers and native hit testing");
            }
            Check(std::abs((14.0 / client.width * buffer.width) * client.width / buffer.width - 14.0) < 0.001 &&
                std::abs((2.0 / client.height * buffer.height) * client.height / buffer.height - 2.0) < 0.001,
                "button radius and border width use client units rather than buffer units");
        }
    }
    {
        LiveResizePolicy video; MappingGeometryGate input;
        video.Begin(true);
        Check(input.Update(true) == MappingGeometryGate::Action::Pause, "live video still pauses source input before resizing");
        for (int i = 0; i < 1000; ++i)
            Check(input.Update(true) == MappingGeometryGate::Action::Wait, "resizing never repeatedly drains input");
        video.End();
        Check(input.Update(false) == MappingGeometryGate::Action::Resume, "source input resumes only at final geometry");
    }
    if (!failures) std::cout << "Live resize allocation policy, physical chrome layout and mapping pause tests passed (no desktop input).\n";
    return failures;
}
