#include "pch.h"
#include "LensWindow.h"
#include "LensChrome.h"
#include "CursorObservationNative.h"
#include <iostream>

using namespace RegionLens::native;

namespace RegionLens::native
{
    struct UiCursorTestAccess
    {
        static POINT Arm(LensWindow& lens)
        {
            // The real HWND remains hidden/offscreen. Only the fake hit backend
            // treats it as exposed; no desktop cursor/capture API is exercised.
            lens.m_hidden = false; lens.m_inputMappingEnabled = true; lens.m_chromeVisible = true;
            RECT client{}; GetClientRect(lens.Window(), &client);
            auto point = ChromeButtonCenterFor(FullscreenButtonId, client);
            ClientToScreen(lens.Window(), &point); return point;
        }
        static bool Chrome(LensWindow& lens) { return lens.SetChromeCursor({}); }
        static void ForgetVisual(LensWindow& lens) { lens.m_chromeCursor = {}; }
        static bool Visual(LensWindow const& lens) { return lens.m_chromeCursor.visible; }
        static uint64_t Epoch(LensWindow const& lens) { return lens.m_uiCursorEpoch; }
        static HWND Tooltip(LensWindow const& lens) { return lens.m_chromeTooltip; }
        static void Clear(LensWindow& lens) { lens.ClearChromeCursor(); }
        static void Drag(LensWindow& lens, POINT point)
        { lens.m_mappedMove = true; lens.UpdateMappedMoveCursor(point); }
    };
}

namespace
{
    int failures{};
    bool TooltipTextPainted(HWND window)
    {
        RECT rect{}; GetClientRect(window, &rect);
        if (rect.right <= 0 || rect.bottom <= 0) return false;
        auto dc = CreateCompatibleDC(nullptr);
        if (!dc) return false;
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = rect.right; info.bmiHeader.biHeight = -rect.bottom;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits{};
        auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap) { DeleteDC(dc); return false; }
        auto old = SelectObject(dc, bitmap);
        // WM_PRINTCLIENT invokes the same production painter as WM_PAINT.
        // Offscreen windows have no visible paint clip; no desktop is read.
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
        GdiFlush(); // Complete GDI writes before reading the owned DIB on the CPU.
        auto pixels = static_cast<uint32_t*>(bits);
        LONG left = rect.right, right = -1; unsigned white{};
        for (LONG y = 0; y < rect.bottom; ++y) for (LONG x = 0; x < rect.right; ++x)
        {
            auto color = pixels[size_t(y) * rect.right + x];
            if ((color & 255) > 200 && ((color >> 8) & 255) > 200 && ((color >> 16) & 255) > 200)
            { ++white; left = std::min(left, x); right = std::max(right, x); }
        }
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc);
        return white > 25 && right - left > rect.right / 3 && std::abs(left + right - rect.right) < 16;
    }
    void Check(bool value, char const* text)
    { if (!value) { ++failures; std::cerr << "FAILED UI cursor ownership: " << text << '\n'; } }
    template<class T> T Handle(uintptr_t value) { return reinterpret_cast<T>(value); }
    class FakeCursor final : public IUiCursorBackend
    {
    public:
        HCURSOR arrow{ Handle<HCURSOR>(10) }, shape{ arrow };
        HWND hit{ Handle<HWND>(1) }, capture{};
        POINT position{ 100, 100 };
        bool readable{ true }, failRestore{}, failHide{};
        unsigned hides{}, shows{};
        HCURSOR Shape() noexcept override { return shape; }
        HCURSOR Arrow() noexcept override { return arrow; }
        void SetShape(HCURSOR value) noexcept override
        {
            if (value) { ++shows; if (failRestore) return; }
            else { ++hides; if (failHide) return; }
            shape = value;
        }
        bool Position(POINT& value) noexcept override { value = position; return readable; }
        HWND Hit(POINT) noexcept override { return hit; }
        HWND Capture() noexcept override { return capture; }
        CursorObservation Observe() noexcept override
        { return { readable, shape ? CURSOR_SHOWING : 0u, readable ? 0u : ERROR_ACCESS_DENIED,
            reinterpret_cast<uintptr_t>(shape), position }; }
    };
    class Sink final : public IDiagnosticSink
    {
    public:
        unsigned observations{}, ownership{}, anomalies{};
        bool TryRecord(DiagnosticRecord const& record) noexcept override
        {
            if (record.event == DiagnosticEvent::CursorObservation) ++observations;
            if (record.event == DiagnosticEvent::UiCursor) ++ownership;
            if (record.critical) ++anomalies;
            return true;
        }
        std::wstring Directory() const override { return L"memory"; }
        DWORD Error() const noexcept override { return 0; }
    };
}

int RunUiCursorOwnershipTests()
{
    failures = 0;
    auto a = Handle<HWND>(1), b = Handle<HWND>(2), outside = Handle<HWND>(3);
    {
        FakeCursor cursor; UiCursorOwnership owner(cursor);
        auto untouched = owner.Inspect();
        Check(!untouched.owner && !untouched.epoch && !untouched.lens && !cursor.hides && !cursor.shows,
            "fault snapshot is read-only before software cursor ownership");
        Check(owner.Release(a, 0, UiCursorRelease::MappingDisabled, 0) && !cursor.shows,
            "never-owned cancellation does not alter a native shape");
        auto epoch = owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1);
        Check(epoch && !cursor.shape, "button software arrow owns a null native shape");
        auto owned = owner.Inspect();
        Check(owned.owner == a && owned.epoch == epoch && owned.lens == 11 &&
            owned.purpose == UiCursorPurpose::Chrome && cursor.hides == 1 && !cursor.shows,
            "fault snapshot identifies the owner without changing cursor shape");
        for (int i = 0; i < 100; ++i)
            Check(owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1) == epoch,
                "ordinary movement retains one generation");
        Check(cursor.hides == 1, "steady software motion does not repeat native hides");
        cursor.hit = outside;
        Check(owner.Release(a, epoch, UiCursorRelease::MappingDisabled, 0) && cursor.shape == cursor.arrow,
            "cancel after pointer moved outside restores shape without another move");
        Check(!owner.Inspect().owner && !owner.Inspect().epoch && !owner.Inspect().lens &&
            owner.Inspect().purpose == UiCursorPurpose{},
            "fault snapshot reports cleared ownership after recovery");
        Check(owner.Release(a, epoch, UiCursorRelease::Closed, 0) && cursor.shows == 1,
            "release is idempotent");
    }
    {
        FakeCursor cursor; UiCursorOwnership owner(cursor);
        auto old = owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1);
        cursor.hit = b;
        auto current = owner.Acquire(b, 12, UiCursorPurpose::Chrome, cursor.position, true, 1);
        auto shows = cursor.shows;
        Check(current > old && owner.Release(a, old, UiCursorRelease::PointerLeft, 1) &&
            owner.Owns(b, current) && !cursor.shape && cursor.shows == shows,
            "late release by old lens cannot reveal a second arrow on the new lens");
        for (auto shape : { Handle<HCURSOR>(20), Handle<HCURSOR>(21) })
        {
            cursor.shape = shape;
            Check(owner.Release(b, current, UiCursorRelease::PointerLeft, 1) && cursor.shape == shape,
                "native hand and text shapes are preserved during cleanup");
            current = owner.Acquire(b, 12, UiCursorPurpose::Chrome, cursor.position, true, 1);
        }
        cursor.hit = outside;
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1),
            "obsolete window mouse message cannot hide over a foreign hit");
        owner.Release(b, current, UiCursorRelease::PointerLeft, 1);
    }
    {
        FakeCursor cursor; UiCursorOwnership owner(cursor);
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, false, 0),
            "hidden, closed, paused and transparent callers cannot acquire");
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Chrome, { 99, 100 }, true, 0),
            "cursor moving after hit-test rejects stale expected coordinates");
        cursor.capture = outside;
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 0),
            "another capture owner blocks local software cursor");
        cursor.capture = a; cursor.hit = outside;
        auto drag = owner.Acquire(a, 11, UiCursorPurpose::Drag, cursor.position, true, 1);
        Check(drag && !cursor.shape, "local drag uses capture ownership even outside its rectangle");
        cursor.capture = b;
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Drag, cursor.position, true, 1),
            "capture loss cannot rehide from a queued drag update");
        cursor.failRestore = true;
        Check(!owner.Release(a, drag, UiCursorRelease::CaptureLost, 1) && owner.Owns(a, drag),
            "failed restoration retains its own obligation");
        cursor.failRestore = false;
        Check(owner.Release(a, drag, UiCursorRelease::MappingDisabled, 0) && cursor.shape,
            "later explicit cancel retries retained obligation");
        cursor.hit = a; cursor.capture = nullptr; cursor.failHide = true;
        Check(!owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1) && cursor.shape,
            "failed hide cannot advertise a software cursor");
    }
    {
        FakeCursor cursor; UiCursorOwnership owner(cursor);
        auto alternate = Handle<HCURSOR>(20);
        Check(owner.RefreshNativeAfterShow(a, cursor.position, cursor.arrow, alternate) &&
            cursor.shape == cursor.arrow && cursor.shows == 2,
            "stationary native handoff forces a non-null shape transition before overlay retirement");
        auto shows = cursor.shows;
        cursor.hit = outside;
        Check(!owner.RefreshNativeAfterShow(a, cursor.position, cursor.arrow, alternate) && cursor.shows == shows,
            "handoff never changes another window's cursor shape");
        cursor.hit = a; cursor.capture = outside;
        Check(!owner.RefreshNativeAfterShow(a, cursor.position, cursor.arrow, alternate) && cursor.shows == shows,
            "handoff respects another window's capture");
        cursor.capture = nullptr;
        Check(!owner.RefreshNativeAfterShow(a, { cursor.position.x + 1, cursor.position.y },
            cursor.arrow, alternate) && cursor.shows == shows,
            "stale cursor coordinates cannot refresh an old lens");
        cursor.failRestore = true;
        Check(!owner.RefreshNativeAfterShow(a, cursor.position, cursor.arrow, alternate),
            "failed intermediate shape cannot acknowledge a visual handoff");
        cursor.failRestore = false;
        cursor.shape = alternate;
        Check(owner.RefreshNativeAfterShow(a, cursor.position, cursor.arrow, alternate) &&
            cursor.shape == cursor.arrow,
            "a distinct current native shape needs only the final shape update");
    }
    {
        auto runtime = Runtime(); Sink sink; auto diagnosticRuntime = runtime;
        diagnosticRuntime.diagnostics = &sink; SetRuntime(diagnosticRuntime);
        FakeCursor cursor; UiCursorOwnership owner(cursor);
        for (int i = 0; i < 1000; ++i)
        {
            auto epoch = owner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1);
            owner.Release(a, epoch, UiCursorRelease::PointerLeft, 1);
        }
        CursorObservation empty{ true, 0, 0, 0, {} };
        RecordCursorVisibility(0, 4, 22, true, 0, empty, empty);
        Check(sink.ownership > 0 && sink.ownership <= 64 && sink.observations == 1 && sink.anomalies,
            "transition diagnostics are bounded and API success does not imply a visible shape");
        UiCursorOwnership observationOwner(cursor);
        auto epoch = observationOwner.Acquire(a, 11, UiCursorPurpose::Chrome, cursor.position, true, 1);
        auto anomalies = sink.anomalies;
        cursor.readable = false;
        Check(observationOwner.Release(a, epoch, UiCursorRelease::PointerLeft, 1) &&
            !observationOwner.Owns(a, epoch) && sink.anomalies == anomalies + 1,
            "observation failure is recorded but does not retain a restored shape indefinitely");
        SetRuntime(runtime);
    }

    // Exercise real LensWindow clearing/notification code with a hidden HWND
    // and fake cursor API. No desktop pointer, input hooks or SendInput calls.
    auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        auto device = std::make_shared<D3DDevice>();
        Check(device->Initialize(), "production device initializes for hidden window regression");
        if (device->Device())
        {
            FakeCursor cursor; UiCursorOwnership owner(cursor);
            LensDescriptor descriptor{ 1001, MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY),
                {0, 0, 100, 80}, {-22000, -22000, -21600, -21700}, true };
            unsigned mappingClicks{}, screenshotClicks{}, closeClicks{};
            LensWindow* target{};
            LensWindow lens(descriptor, device, [&](uint64_t) { ++closeClicks; },
                [&](uint64_t) { ++mappingClicks; target->SetInputMappingEnabled(false); }, {}, {}, {},
                [&](uint64_t) { ++screenshotClicks; }, {}, &owner);
            target = &lens;
            Check(lens.Show(true), "production lens created hidden");
            RECT offscreen{}; GetWindowRect(lens.Window(), &offscreen);
            Check(!MonitorFromRect(&offscreen, MONITOR_DEFAULTTONULL), "native test lens is entirely offscreen");
            ShowWindow(lens.Window(), SW_SHOWNOACTIVATE); // Enable real popup painting, never desktop input.
            auto arm = [&] {
                cursor.hit = lens.Window(); cursor.capture = nullptr;
                cursor.position = UiCursorTestAccess::Arm(lens);
                Check(UiCursorTestAccess::Chrome(lens) && !cursor.shape,
                    "production button path acquires fake native shape");
            };
            arm();
            UiCursorTestAccess::ForgetVisual(lens); cursor.hit = outside;
            lens.SetInputMappingEnabled(false);
            Check(cursor.shape && !UiCursorTestAccess::Epoch(lens),
                "mapping cancellation repairs ownership even after visual state disappeared");
            cursor.hit = lens.Window();
            auto handoffShows = cursor.shows;
            Check(lens.RefreshNativeCursorForHandoff(cursor.position) &&
                cursor.shape == cursor.arrow && cursor.shows == handoffShows + 2,
                "production lens refreshes its own stationary pointer after native show");
            cursor.hit = outside;
            handoffShows = cursor.shows;
            Check(!lens.RefreshNativeCursorForHandoff(cursor.position) && cursor.shows == handoffShows,
                "production lens does not refresh a foreign hit window");
            arm(); cursor.hit = outside;
            SendMessageW(lens.Window(), WM_MOUSELEAVE, 0, 0);
            Check(cursor.shape && !UiCursorTestAccess::Visual(lens),
                "production mouse leave restores outside arrow");
            auto hides = cursor.hides;
            SendMessageW(lens.Window(), WM_SETCURSOR, reinterpret_cast<WPARAM>(lens.Window()),
                MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            Check(cursor.hides == hides, "stale production WM_SETCURSOR never reacquires");
            arm();
            auto epoch = UiCursorTestAccess::Epoch(lens);
            auto shows = cursor.shows;
            SendMessageW(lens.Window(), WM_MOUSELEAVE, 0, 0);
            Check(!cursor.shape && UiCursorTestAccess::Visual(lens) &&
                UiCursorTestAccess::Epoch(lens) == epoch && cursor.shows == shows,
                "late leave after reentry retains the current button lease without flicker");
            // A leave notification must not itself re-register tracking. With
            // the real HWND offscreen, USER32 can immediately post another
            // leave; a bounded native message pump detects that feedback loop.
            unsigned leaves{}; MSG leave{};
            while (leaves < 32 && PeekMessageW(&leave, lens.Window(), WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE))
            { ++leaves; DispatchMessageW(&leave); }
            std::cout << "Native queued leave count: " << leaves << '\n';
            Check(leaves < 32, "leave handling cannot endlessly replenish the native message queue");

            // Exercise actual tooltip HWND/paint dispatch and queued button
            // notifications, not just the ownership object or shader labels.
            auto clientPoint = [&](int button) {
                RECT client{}; GetClientRect(lens.Window(), &client);
                auto point = ChromeButtonCenterFor(button, client);
                cursor.position = point; ClientToScreen(lens.Window(), &cursor.position);
                return point;
            };
            auto hover = [&](int button) {
                auto point = clientPoint(button);
                SendMessageW(lens.Window(), WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
            };
            hover(RestoreSizeButtonId);
            MSG firstLeave{};
            unsigned initialLeaves{};
            while (initialLeaves < 32 && PeekMessageW(&firstLeave, lens.Window(), WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE))
            { ++initialLeaves; DispatchMessageW(&firstLeave); }
            Check(initialLeaves < 32, "initial tooltip hover leaves a finite queue");
            HWND tooltip = UiCursorTestAccess::Tooltip(lens);
            Check(tooltip != nullptr, "production hover creates tooltip HWND");
            if (tooltip)
            {
                RECT bounds{}; GetWindowRect(tooltip, &bounds);
                Check(!MonitorFromRect(&bounds, MONITOR_DEFAULTTONULL), "test tooltip stays outside desktop monitors");
                auto language = CurrentAppLanguage();
                for (auto current : { AppLanguage::SimplifiedChinese, AppLanguage::English })
                {
                    SetAppLanguage(current);
                    for (int button : { PinButtonId, ScreenshotButtonId, PointerSpeedButtonId, InputMappingButtonId })
                    {
                        hover(button);
                        wchar_t text[96]{}; GetWindowTextW(tooltip, text, int(std::size(text)));
                        Check(std::wstring(text) == ChromeTooltipText(button, true, false, true),
                            "tooltip label follows the current button rather than an older hover");
                        Check(TooltipTextPainted(tooltip), "production tooltip paints visible centered glyphs after changing size/text");
                        unsigned queued{}; MSG item{};
                        while (queued < 32 && PeekMessageW(&item, lens.Window(), WM_MOUSELEAVE, WM_MOUSELEAVE, PM_REMOVE))
                        { ++queued; DispatchMessageW(&item); }
                        Check(queued <= 1, "hover tracking has at most one immediate leave, not a feedback loop");
                    }
                }
                SetAppLanguage(language);
            }
            auto click = [&](int button) {
                auto point = clientPoint(button);
                PostMessageW(lens.Window(), WM_MOUSEMOVE, 0, MAKELPARAM(point.x, point.y));
                PostMessageW(lens.Window(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(point.x, point.y));
                PostMessageW(lens.Window(), WM_LBUTTONUP, 0, MAKELPARAM(point.x, point.y));
                unsigned count{}; MSG item{};
                while (count < 64 && PeekMessageW(&item, lens.Window(), 0, 0, PM_REMOVE))
                { ++count; DispatchMessageW(&item); }
                Check(count < 64, "queued click and hover messages drain without starving the native pump");
            };
            click(ScreenshotButtonId);
            Check(screenshotClicks == 1, "one queued screenshot click invokes one action");
            click(InputMappingButtonId);
            Check(mappingClicks == 1 && !UiCursorTestAccess::Epoch(lens) && cursor.shape,
                "one queued mapping-button click disables and restores the cursor");
            for (int i = 0; i < 50; ++i)
            {
                cursor.hit = outside;
                SendMessageW(lens.Window(), WM_MOUSELEAVE, 0, 0);
                Check(cursor.shape && !UiCursorTestAccess::Epoch(lens),
                    "repeated leave restores without accumulating null-shape obligations");
                arm();
            }
            cursor.hit = outside;
            lens.SetInputMappingEnabled(false);
            shows = cursor.shows;
            lens.SetInputMappingEnabled(false);
            Check(cursor.shape && cursor.shows == shows,
                "repeated global cancel does not overwrite an already restored cursor");
            arm();
            lens.SetUiCursorPaused(true);
            Check(cursor.shape && !UiCursorTestAccess::Chrome(lens), "settings/selection pause clears and blocks software cursor");
            hides = cursor.hides;
            SendMessageW(lens.Window(), WM_SETCURSOR, reinterpret_cast<WPARAM>(lens.Window()),
                MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            Check(cursor.hides == hides, "queued cursor message cannot rehide during settings pause");
            cursor.hit = outside; lens.SetUiCursorPaused(false);
            arm(); lens.SetInputPassThrough(true);
            Check(cursor.shape && !UiCursorTestAccess::Epoch(lens), "pass-through clears native obligation before style handoff");
            hides = cursor.hides;
            SendMessageW(lens.Window(), WM_SETCURSOR, reinterpret_cast<WPARAM>(lens.Window()),
                MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            Check(cursor.hides == hides, "queued cursor message cannot rehide a transparent lens");
            cursor.hit = outside; lens.SetInputPassThrough(false);
            arm(); cursor.capture = lens.Window();
            UiCursorTestAccess::Drag(lens, cursor.position);
            cursor.capture = outside; cursor.hit = outside;
            SendMessageW(lens.Window(), WM_CAPTURECHANGED, 0, reinterpret_cast<LPARAM>(outside));
            Check(cursor.shape && !UiCursorTestAccess::Visual(lens), "capture loss restores drag cursor without another mouse event");
            arm(); cursor.hit = outside; lens.Hide();
            Check(cursor.shape && !UiCursorTestAccess::Epoch(lens), "hide restores outside shape");
            arm(); click(CloseButtonId);
            Check(closeClicks == 1, "one queued close click invokes one close request");
            cursor.hit = outside; lens.Close();
            Check(cursor.shape && !UiCursorTestAccess::Epoch(lens), "close restores before destroying HWND");
        }
    }
    {
        // In the Dev unified-cursor mode, button and local-drag paths must
        // leave the thread cursor shape intact. The only visible sprite is
        // supplied by SoftwareCursorWindow; no real pointer is moved here.
        auto previous = Runtime();
        auto dev = previous;
        dev.identity = &DevIdentity;
        dev.persistentSoftwareCursor = true;
        SetRuntime(dev);
        auto device = std::make_shared<D3DDevice>();
        if (device->Initialize())
        {
            FakeCursor cursor;
            UiCursorOwnership owner(cursor);
            LensDescriptor descriptor{ 1002, MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY),
                { 0, 0, 100, 80 }, { -22500, -22000, -22100, -21700 }, true };
            LensWindow lens(descriptor, device, {}, {}, {}, {}, {}, {}, {}, &owner);
            Check(lens.Show(true), "Dev cursor test creates an offscreen lens");
            ShowWindow(lens.Window(), SW_SHOWNOACTIVATE);
            cursor.hit = lens.Window();
            cursor.position = UiCursorTestAccess::Arm(lens);
            Check(UiCursorTestAccess::Chrome(lens) && cursor.hides == 0 &&
                !UiCursorTestAccess::Epoch(lens) && !UiCursorTestAccess::Visual(lens),
                "Dev button never clears native shape or draws a second arrow");
            UiCursorTestAccess::Drag(lens, cursor.position);
            Check(cursor.hides == 0 && !UiCursorTestAccess::Visual(lens),
                "Dev Ctrl drag leaves cursor drawing to the sole overlay");
            lens.Close();
        }
        else Check(false, "Dev cursor test initializes the production D3D device");
        SetRuntime(previous);
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (!failures) std::cout << "UI cursor ownership and production handoff tests passed (fake cursor; no desktop input).\n";
    return failures;
}
