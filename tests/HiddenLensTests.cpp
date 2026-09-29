#include "pch.h"
#include "LensWindow.h"
#include "LensChrome.h"
#include <iostream>

using namespace RegionLens::native;

namespace RegionLens::native
{
    struct LensFullscreenTransitionTestAccess
    {
        static void Begin(LensWindow& lens) { lens.m_fullscreenTransition.Begin(); }
        static bool Active(LensWindow const& lens) { return lens.m_fullscreenTransition.Active(); }
    };
}

int RunHiddenLensTests()
{
    int failures{};
    auto check = [&](bool value, char const* name) {
        if (!value) { ++failures; std::cerr << "FAILED hidden lens: " << name << '\n'; }
    };
    auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        auto device = std::make_shared<D3DDevice>();
        check(device->Initialize(), "production D3D device initializes");
        if (device->Device()) {
            int closed{}, toggled{}, screenshots{};
            LensDescriptor descriptor{ 123, MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY),
                { 0, 0, 100, 80 }, { -20000, -20000, -19700, -19800 }, true };
            LensWindow lens(descriptor, device, [&](uint64_t) { ++closed; },
                [&](uint64_t) { ++toggled; }, {}, {}, {}, [&](uint64_t) { ++screenshots; });
            // A hidden production window only. Never show/activate a desktop
            // window, install a hook, alter the clipboard, or inject input.
            check(lens.Show(true), "new regions can initialize without a visible flash");
            auto window = lens.Window();
            if (window) {
                auto normalStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
                check((normalStyle & WS_EX_LAYERED) != 0 &&
                    (normalStyle & WS_EX_TRANSPARENT) == 0 &&
                    (GetClassLongPtrW(window, GCL_STYLE) & (CS_HREDRAW | CS_VREDRAW)) == 0,
                    "composition lens does not request redundant full-surface resize invalidation");
                auto enteredPassThrough = lens.SetInputPassThrough(true);
                check(enteredPassThrough, "hidden lens can enter native cross-process hit-through");
                auto passiveStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
                check((passiveStyle & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) ==
                    (WS_EX_LAYERED | WS_EX_TRANSPARENT),
                    "mapping uses layered transparent cross-process hit-through");
                check(lens.SetInputPassThrough(false), "hidden lens restores interactive window mode");
                auto restoredStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
                check((restoredStyle & WS_EX_LAYERED) != 0 &&
                    (restoredStyle & WS_EX_TRANSPARENT) == 0,
                    "mapping recovery removes hit-through without changing the render surface");
                check(!IsWindowVisible(window) && !lens.KeepsTopmost() && lens.MappingConfig().blocked,
                    "hidden region is excluded from input routing and topmost maintenance");
                RECT before{}; GetWindowRect(window, &before);
                lens.SetInputMappingEnabled(true);
                RECT client{}; GetClientRect(window, &client);
                for (int i = 0; i < ChromeButtonCount; ++i) {
                    auto point = ChromeButtonCenter(i, client);
                    // Direct messages to our own hidden HWND exercise stale UI
                    // dispatch; they do not reach the desktop input stream.
                    SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
                    SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
                }
                SendMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
                auto previousCapture = GetCapture();
                CursorSnapshot stale{};
                stale.visible = true; stale.localMoveRequested = true;
                lens.SetVirtualCursor(stale);
                lens.Hide(); lens.Hide();
                lens.NotifyScreenshotSuccess(); // Async completion must not reopen a hidden source region.
                RECT after{}; GetWindowRect(window, &after);
                check(!closed && !toggled && !screenshots && !lens.SpeedPopupOpen() &&
                    !IsWindowVisible(window) && EqualRect(&before, &after) && GetCapture() == previousCapture,
                    "queued controls/Esc/cursor snapshots/screenshot completion cannot reopen, move or capture a hidden region");
                check(lens.MappingConfig().blocked && lens.ShouldSuspendInputMapping({-19900,-19900}),
                    "armed hidden region remains blocked even over its retained geometry");
                lens.Close(); check(!IsWindow(window), "hidden region closes normally");
                lens.NotifyScreenshotSuccess(); // Completion after destruction is harmless as well.
            }

            LensDescriptor newerDescriptor{ 124, descriptor.monitor, descriptor.source,
                { -19600, -20000, -19300, -19800 }, false };
            LensWindow newer(newerDescriptor, device, {}, {}, {}, {}, {});
            check(newer.Show() && !newer.Hidden() && IsWindowVisible(newer.Window()),
                "a region created after hide uses the normal visible state");
            RECT initial{}; GetWindowRect(newer.Window(), &initial);
            SendMessageW(newer.Window(), WM_ENTERSIZEMOVE, 0, 0);
            RECT proposal{ initial.left, initial.top, initial.right + 300, initial.bottom + 400 };
            check(SendMessageW(newer.Window(), WM_SIZING, WMSZ_BOTTOMRIGHT,
                reinterpret_cast<LPARAM>(&proposal)) == TRUE &&
                int64_t(proposal.right-proposal.left) * (initial.bottom-initial.top) ==
                    int64_t(proposal.bottom-proposal.top) * (initial.right-initial.left),
                "production LensWindow routes corner WM_SIZING through proportional policy");
            SendMessageW(newer.Window(), WM_EXITSIZEMOVE, 0, 0);
            LensFullscreenTransitionTestAccess::Begin(newer);
            check(newer.MappingConfig().blocked && newer.ShouldSuspendInputMapping({-19450,-19900}),
                "pending fullscreen transition blocks input before the HWND is enlarged");
            newer.Hide();
            check(!LensFullscreenTransitionTestAccess::Active(newer),
                "hiding a lens cancels an uncommitted fullscreen transition");
            check(newer.ShowRaisedPreservingTopmost() && IsWindowVisible(newer.Window()) &&
                !newer.KeepsTopmost() &&
                (GetWindowLongPtrW(newer.Window(), GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
                "show-all raises an ordinary region without permanently pinning it");

            LensDescriptor pinnedDescriptor{ 125, descriptor.monitor, descriptor.source,
                { -19200, -20000, -18900, -19800 }, true };
            LensWindow pinned(pinnedDescriptor, device, {}, {}, {}, {}, {});
            check(pinned.Show(true) && pinned.ShowRaisedPreservingTopmost() && pinned.KeepsTopmost() &&
                (GetWindowLongPtrW(pinned.Window(), GWL_EXSTYLE) & WS_EX_TOPMOST) != 0,
                "show-all preserves an explicitly pinned region");
            check(newer.ShowRaisedPreservingTopmost() && !newer.KeepsTopmost() &&
                (GetWindowLongPtrW(newer.Window(), GWL_EXSTYLE) & WS_EX_TOPMOST) == 0,
                "raising an ordinary region beside a pinned one does not promote its Z-order tier");
            pinned.Close();
            newer.Close();
        }
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    if (!failures) std::cout << "Hidden/offscreen production lens and queued-control tests passed (no desktop input).\n";
    return failures;
}
