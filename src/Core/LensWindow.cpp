#include "pch.h"
#include "LensWindow.h"
#include "Localization.h"
#include "LensChrome.h"
#include "RegionTransform.h"
#include "MappingGeometryGate.h"
#include "MappedMovePolicy.h"
#include "resource.h"

namespace
{
    wchar_t const* LensClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"LensWindow");
        return name.c_str();
    }
    wchar_t const* FullscreenHintClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"FullscreenHint");
        return name.c_str();
    }
    wchar_t const* ChromeTooltipClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"ChromeTooltip");
        return name.c_str();
    }
    constexpr int ResizeBorder = 10;
    constexpr int MinimumLensWidth = 200;
    constexpr int MinimumLensHeight = 128;
    constexpr UINT_PTR ChromeTimerId = 1;
    constexpr UINT_PTR FullscreenHintTimerId = 2;
    constexpr UINT_PTR FrameRetryTimerId = 3;
    constexpr UINT_PTR ScreenshotFeedbackTimerId = 4;
    constexpr int FullscreenEscapeHotkeyId = 77;
    constexpr UINT RenderFailureMessage = WM_APP + 20;

    UINT ChromeTooltipDpi(HWND window)
    {
        auto owner = GetWindow(window, GW_OWNER);
        return GetDpiForWindow(owner ? owner : window);
    }

    HFONT ChromeTooltipFont(HWND window)
    {
        // Measure and paint at the same lens DPI even if a wide lens straddles
        // two monitors and this owned popup lands on the other monitor.
        return CreateFontW(-MulDiv(12, static_cast<int>(ChromeTooltipDpi(window)), 72),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH, L"Microsoft YaHei UI");
    }

    void PaintChromeTooltip(HWND window, HDC dc)
    {
        RECT client{};
        GetClientRect(window, &client);
        auto background = CreateSolidBrush(RGB(28, 32, 40));
        FillRect(dc, &client, background);
        DeleteObject(background);
        auto font = ChromeTooltipFont(window);
        auto oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        wchar_t text[64]{};
        GetWindowTextW(window, text, static_cast<int>(std::size(text)));
        auto padding = MulDiv(8, static_cast<int>(ChromeTooltipDpi(window)), 96);
        InflateRect(&client, -padding, 0);
        DrawTextW(dc, text, -1, &client, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        DeleteObject(font);
    }

    LRESULT CALLBACK ChromeTooltipProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_ERASEBKGND: return 1;
        case WM_PRINTCLIENT:
            PaintChromeTooltip(window, reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            PaintChromeTooltip(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT CALLBACK FullscreenHintProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            auto background = CreateSolidBrush(RGB(18, 39, 64));
            FillRect(dc, &client, background);
            DeleteObject(background);

            auto dpi = GetDpiForWindow(window);
            auto font = CreateFontW(
                -MulDiv(18, static_cast<int>(dpi), 72),
                0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
            auto oldFont = SelectObject(dc, font);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            wchar_t text[96]{};
            GetWindowTextW(window, text, static_cast<int>(std::size(text)));
            DrawTextW(dc, text, -1, &client,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
            SelectObject(dc, oldFont);
            DeleteObject(font);
            EndPaint(window, &paint);
            return 0;
        }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

namespace RegionLens::native
{
    LensWindow::LensWindow(
        LensDescriptor descriptor,
        std::shared_ptr<D3DDevice> device,
        CloseCallback closeCallback,
        MappingToggleCallback mappingToggleCallback,
        MappingGeometryChangedCallback mappingGeometryChangedCallback,
        MappingHoverCallback mappingHoverCallback,
        InteractionCallback interactionCallback, CloseCallback screenshotCallback, CloseCallback popupChangedCallback,
        UiCursorOwnership* uiCursor)
        : m_descriptor(descriptor),
          m_device(std::move(device)),
          m_closeCallback(std::move(closeCallback)),
          m_mappingToggleCallback(std::move(mappingToggleCallback)),
          m_mappingGeometryChangedCallback(std::move(mappingGeometryChangedCallback)),
          m_mappingHoverCallback(std::move(mappingHoverCallback)),
          m_interactionCallback(std::move(interactionCallback)),
          m_screenshotCallback(std::move(screenshotCallback)), m_popupChangedCallback(std::move(popupChangedCallback)),
          m_uiCursor(uiCursor ? *uiCursor : UiCursorOwnership::ForCurrentThread())
    {
    }

    LensWindow::~LensWindow()
    {
        Close();
    }

    bool LensWindow::RegisterClasses()
    {
        auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW lensClass{ sizeof(lensClass) };
        // The client area is owned entirely by DirectComposition. Asking
        // USER32 to invalidate a separate GDI surface on every size change can
        // expose that surface for a frame while DWM applies the visual update.
        lensClass.style = 0;
        lensClass.lpfnWndProc = WindowProc;
        lensClass.hInstance = instance;
        lensClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        lensClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_REGIONLENS));
        lensClass.hIconSm = lensClass.hIcon;
        lensClass.lpszClassName = LensClassName();
        if (!RegisterClassExW(&lensClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW hintClass{ sizeof(hintClass) };
        hintClass.lpfnWndProc = FullscreenHintProc;
        hintClass.hInstance = instance;
        hintClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        hintClass.lpszClassName = FullscreenHintClassName();
        if (!RegisterClassExW(&hintClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW tooltipClass{ sizeof(tooltipClass) };
        tooltipClass.lpfnWndProc = ChromeTooltipProc;
        tooltipClass.hInstance = instance;
        tooltipClass.lpszClassName = ChromeTooltipClassName();
        if (!RegisterClassExW(&tooltipClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

        return true;
    }

    bool LensWindow::Show(bool hidden)
    {
        m_hidden = hidden;
        if (!RegisterClasses())
        {
            return false;
        }

        auto width = std::max<LONG>(MinimumLensWidth, m_descriptor.windowBounds.right - m_descriptor.windowBounds.left);
        auto height = std::max<LONG>(MinimumLensHeight, m_descriptor.windowBounds.bottom - m_descriptor.windowBounds.top);
        DWORD extended = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
        if (m_descriptor.topmost)
        {
            extended |= WS_EX_TOPMOST;
        }
        m_window = CreateWindowExW(
            extended,
            LensClassName(),
            Localized(L"区域镜", L"RegionLens"),
            WS_POPUP | WS_THICKFRAME | WS_CLIPCHILDREN,
            m_descriptor.windowBounds.left,
            m_descriptor.windowBounds.top,
            width,
            height,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            this);
        if (!m_window)
        {
            return false;
        }
        RECT initialBounds{};
        if (!GetWindowRect(m_window, &initialBounds)) { Close(); return false; }
        m_initialSize = { initialBounds.right - initialBounds.left, initialBounds.bottom - initialBounds.top };
        if (!SetLayeredWindowAttributes(m_window, 0, 255, LWA_ALPHA)) { Close(); return false; }
        if (!SetWindowDisplayAffinity(m_window, WDA_EXCLUDEFROMCAPTURE))
        {
            OutputDebugStringW(L"RegionLens: WDA_EXCLUDEFROMCAPTURE failed.\n");
            Close();
            return false;
        }

        m_renderer.SetPresentationWake(m_window, PresentationReadyMessage);
        if (!m_renderer.Initialize(m_window, m_device, true, Id()))
        {
            Close();
            return false;
        }
        // Start discoverable: the first presented frame includes chrome. The timer
        // hides it only after the pointer has left the lens.
        SetChromeVisible(true);
        if (!m_hidden) {
            ShowWindow(m_window, SW_SHOWNOACTIVATE);
            SetTimer(m_window, ChromeTimerId, 180, nullptr);
        }
        return true;
    }

    void LensWindow::Hide()
    {
        if (!m_window || m_closeRequested || m_hidden) return;
        m_hidden = true; // Block callbacks/timers before closing any native popup.
        m_renderer.SuspendPresentationWait();
        CancelFullscreenTransition(false, true);
        m_screenshotFeedback.Reset();
        KillTimer(m_window, ScreenshotFeedbackTimerId);
        CloseSpeedPopup();
        EndMappedMove(false);
        SendMessageW(m_window, WM_CANCELMODE, 0, 0);
        if (GetCapture() == m_window) ReleaseCapture();
        HideChromeTooltip();
        HideFullscreenHint();
        ClearChromeCursor(UiCursorRelease::Hidden);
        m_virtualCursor = {};
        m_restoreEscapeOnShow = m_escapeHotkeyRegistered;
        if (m_escapeHotkeyRegistered) {
            UnregisterHotKey(m_window, FullscreenEscapeHotkeyId);
            m_escapeHotkeyRegistered = false;
        }
        KillTimer(m_window, ChromeTimerId);
        KillTimer(m_window, FrameRetryTimerId);
        m_frameRetryArmed = false;
        ShowWindow(m_window, SW_HIDE);
    }

    bool LensWindow::ShowRaisedPreservingTopmost()
    {
        if (!m_window || m_closeRequested) return false;
        // Raising is transient: only the explicit pin button changes topmost.
        // Fullscreen is topmost for its duration, independent of that pin.
        bool topmost = m_descriptor.topmost || m_fullscreen;
        // Already-visible pinned/fullscreen regions need no Z-order change.
        // Ctrl+Alt+S is primarily a raise for ordinary regions, not a way to
        // reorder existing topmost windows against other applications.
        if (topmost && !m_hidden && IsWindowVisible(m_window)) return true;
        m_hidden = false;
        auto insertAfter = topmost ? HWND_TOPMOST :
            ((GetWindowLongPtrW(m_window, GWL_EXSTYLE) & WS_EX_TOPMOST) ? HWND_NOTOPMOST : HWND_TOP);
        if (!SetWindowPos(m_window, insertAfter, 0, 0, 0, 0, TopmostPositionFlags | SWP_SHOWWINDOW)) {
            m_hidden = !IsWindowVisible(m_window);
            return false;
        }
        SetTimer(m_window, ChromeTimerId, 180, nullptr);
        // Preserve which fullscreen window owned Esc before hiding; the
        // bottom-first Z-order restore must not hand it to a different window.
        if (m_fullscreen && m_restoreEscapeOnShow && !m_escapeHotkeyRegistered) {
            m_escapeHotkeyRegistered = RegisterHotKey(m_window, FullscreenEscapeHotkeyId, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
            if (m_escapeHotkeyRegistered) m_restoreEscapeOnShow = false;
        }
        CheckGeometrySettled();
        UpdateChromeFromCursor();
        RenderLatestFrame(); // Hidden regions retained the latest capture, not a frozen screenshot.
        return true;
    }

    void LensWindow::SetQualityLevel(LensSharpness level)
    {
        if (m_closeRequested || m_qualityRequested == level) return;
        m_qualityRequested = level;
        m_qualitySettings = QualityForLevel(level);
        m_renderer.ResetQuality();
        HideChromeTooltip();
        Record(DiagnosticEvent::RenderQuality, { 1, int64_t(m_qualitySettings.mode), int64_t(m_qualitySettings.sharpness) }, false, 0, Id());
        // Redraw the retained capture even if the source is completely still.
        // Hidden lenses retain this setting for their next visible frame.
        RenderLatestFrame();
    }

    void LensWindow::SetFullscreenAspectFitEnabled(bool enabled)
    {
        if (m_fullscreenAspectFit == enabled) return;
        if (m_fullscreenTransition.Active()) CancelFullscreenTransition(true, true);
        m_fullscreenAspectFit = enabled;
        HideChromeTooltip();
        ClearChromeCursor();
        m_renderer.ResetQuality();
        RenderLatestFrame();
        // Settings owns the normal input pause while this is changed.  Publish
        // the new content geometry as well so any future/non-UI caller cannot
        // leave a routed session using the former full-client destination.
        if (m_fullscreen && m_inputMappingEnabled && m_mappingGeometryChangedCallback)
            m_mappingGeometryChangedCallback(Id());
    }

    void LensWindow::RefreshLanguage()
    {
        HideChromeTooltip();
        HideFullscreenHint();
        CloseSpeedPopup();
    }

    void LensWindow::TogglePointerSpeed()
    {
        if (!m_window || m_closeRequested) return;
        auto action = PointerSpeedButtonAction(m_pointerSpeed, m_speedPopupOpen);
        if (action == PointerSpeedClick::ResetSpeed) {
            HideChromeTooltip();
            if (m_speedPopupOpen) {
                CloseSpeedPopup(true);
            } else {
                m_pointerSpeed = {};
                // Refresh only this lens's current route through the existing
                // recovery/generation fence; other standby lenses are unchanged.
                if (m_mappingGeometryChangedCallback) m_mappingGeometryChangedCallback(Id());
                RenderLatestFrame();
                RefreshNativeChromePointer();
            }
            return;
        }
        if (action == PointerSpeedClick::ClosePanel) { CloseSpeedPopup(); return; }
        HideChromeTooltip();
        ClearChromeCursor();
        m_speedPopupOpen = true;
        // The manager drains takeover before creating any interactive popup.
        // All slider notifications then only store local settings; closing
        // submits one fresh route generation, not dozens of SendInput drains.
        if (m_popupChangedCallback) m_popupChangedCallback(Id());
        if (!m_window || m_closeRequested || m_inputPassThrough || !m_speedPopupOpen) { CloseSpeedPopup(); return; }
        RECT client{}; GetClientRect(m_window, &client);
        auto anchor = ChromeButtonCenterFor(PointerSpeedButtonId, client);
        anchor.y = 46; ClientToScreen(m_window, &anchor);
        if (!m_speedPopup.Show(m_window, anchor, m_pointerSpeed,
            [this](PointerSpeed value) { m_pointerSpeed = value; RenderLatestFrame(); },
            [this] { CloseSpeedPopup(); })) CloseSpeedPopup();
        SetChromeVisible(true);
    }

    void LensWindow::CloseSpeedPopup(bool resetSpeed)
    {
        if (!m_speedPopupOpen) return;
        // Keep the pause set until native slider capture and HWND are gone.
        // NCDESTROY may call us again; clear ownership before draining it.
        m_speedPopupOpen = false;
        m_speedPopup.Close();
        // Native capture loss may deliver a final slider notification. Reset
        // after that notification, but before the manager resumes any route.
        if (resetSpeed) m_pointerSpeed = {};
        if (m_popupChangedCallback) m_popupChangedCallback(Id());
        RenderLatestFrame();
        RefreshNativeChromePointer();
    }

    void LensWindow::Close()
    {
        m_closeRequested=true;
        m_presentationRequest={};
        m_renderer.SuspendPresentationWait();
        CancelFullscreenTransition(false, true);
        CloseSpeedPopup();
        m_renderer.ResetQuality();
        EndMappedMove(false);
        ClearChromeCursor(UiCursorRelease::Closed);
        HideChromeTooltip();
        if (m_chromeTooltip) DestroyWindow(std::exchange(m_chromeTooltip, nullptr));
        HideFullscreenHint();
        if (m_escapeHotkeyRegistered && m_window)
        {
            UnregisterHotKey(m_window, FullscreenEscapeHotkeyId);
            m_escapeHotkeyRegistered = false;
        }
        if (m_window)
        {
            KillTimer(m_window, FrameRetryTimerId);
            m_frameRetryArmed = false;
            auto window = m_window;
            DestroyWindow(window);
            if (m_window == window)
            {
                m_window = nullptr;
            }
        }
    }

    void LensWindow::Render(ID3D11ShaderResourceView* source, int32_t textureWidth, int32_t textureHeight, CaptureStamp stamp)
    {
        if (!m_window || !source)
        {
            return;
        }
        m_latestView = source;
        m_captureStamp = stamp;
        m_textureWidth = textureWidth;
        m_textureHeight = textureHeight;
        CheckGeometrySettled(); // Capture traffic must not starve the fallback timer.
        if (m_fullscreenHint && std::chrono::steady_clock::now() >= m_hintDeadline)
        {
            HideFullscreenHint();
        }
        RenderLatestFrame();
    }

    LRESULT CALLBACK LensWindow::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        LensWindow* self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<LensWindow*>(create->lpCreateParams);
            self->m_window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<LensWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (self)
        {
            return self->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT LensWindow::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        // Mouse-up/move messages already queued before SW_HIDE must not toggle
        // a control, reopen a fullscreen HWND, or hide the native cursor again.
        if (m_hidden && ((message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) ||
            (message >= WM_NCMOUSEMOVE && message <= WM_NCXBUTTONDBLCLK))) return 0;
        switch (message)
        {
        case PresentationReadyMessage:
            if (m_renderer.AcknowledgePresentationWake(uint64_t(wParam))) RenderLatestFrame();
            return 0;
        case WM_NCCALCSIZE:
            if (wParam) return 0;
            break;
        case WM_NCHITTEST:
        {
            if (m_hidden) return HTNOWHERE;
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            return HitTestWindow(point);
        }
        case WM_MOUSEACTIVATE:
            if (m_hidden) return MA_NOACTIVATE;
            if (m_interactionCallback) m_interactionCallback(Id());
            // Keep an open popup active until the button's mouse-up toggles it;
            // otherwise fullscreen activation would close and immediately reopen it.
            return m_fullscreen && !m_speedPopupOpen ? MA_ACTIVATE : MA_NOACTIVATE;
        case WM_GETMINMAXINFO:
        {
            auto info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = { MinimumLensWidth, MinimumLensHeight };
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(m_window, &paint);
            EndPaint(m_window, &paint);
            RenderLatestFrame();
            return 0;
        }
        case WM_ENTERSIZEMOVE:
            if (m_hidden) return 0;
            CancelFullscreenTransition(true, true);
            {
                RECT bounds{};
                if (GetWindowRect(m_window, &bounds)) m_resizePolicy.Begin(bounds);
                else m_resizePolicy.End();
            }
            // Hold allocation size before any geometry callbacks can draw.
            // Translation uses the same path, without a resolution change.
            m_renderer.BeginLiveResize();
            HideChromeTooltip();
            ClearChromeCursor();
            m_lastGeometryChange = GetTickCount64();
            m_inSizeMove = true;
            CloseSpeedPopup();
            if (m_mappingGeometryChangedCallback) m_mappingGeometryChangedCallback(Id());
            return 0;
        case WM_SIZING:
            if (!m_fullscreen && lParam && m_resizePolicy.Adjust(static_cast<UINT>(wParam),
                *reinterpret_cast<RECT*>(lParam), MinimumLensWidth, MinimumLensHeight)) return TRUE;
            break;
        case WM_EXITSIZEMOVE:
        {
            FinishGeometryChange();
            return 0;
        }
        case WM_SIZE:
            m_renderer.Resize();
            RenderLatestFrame();
            return 0;
        case WM_LBUTTONDOWN:
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (ChromeButtonAt(point) != PointerSpeedButtonId) CloseSpeedPopup();
            if (ChromeButtonAt(point) != 0)
            {
                HideChromeTooltip();
                return 0;
            }
            break;
        }
        case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
        case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
            CloseSpeedPopup();
            break;
        case WM_LBUTTONUP:
        {
            if (m_mappedMove)
            {
                EndMappedMove(true);
                return 0;
            }
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            auto button = ChromeButtonAt(point);
            if (button == ScreenshotButtonId) {
                if (m_screenshotCallback) m_screenshotCallback(Id());
                RefreshNativeChromePointer();
                return 0;
            }
            if (button == PointerSpeedButtonId) {
                TogglePointerSpeed();
                return 0;
            }
            if (button == PinButtonId)
            {
                ToggleTopmost();
                RefreshNativeChromePointer();
                return 0;
            }
            if (button == FullscreenButtonId)
            {
                ToggleFullscreen();
                return 0;
            }
            if (button == RestoreSizeButtonId)
            {
                RestoreInitialSize();
                return 0;
            }
            if (button == InputMappingButtonId)
            {
                if (m_mappingToggleCallback)
                {
                    m_mappingToggleCallback(m_descriptor.id);
                }
                RefreshNativeChromePointer();
                return 0;
            }
            if (button == CloseButtonId && !m_closeRequested)
            {
                m_closeRequested = true;
                m_closeCallback(m_descriptor.id);
                return 0;
            }
            break;
        }
        case RenderFailureMessage:
            if (m_inputMappingEnabled && m_mappingToggleCallback) m_mappingToggleCallback(Id());
            return 0;
        case WM_MOUSEMOVE:
        {
            if (m_mappedMove)
            {
                UpdateMappedMove();
                return 0;
            }
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (m_fullscreen) UpdateFullscreenChromeFromClientPoint(point);
            else SetChromeVisible(true);
            // Posted movement may be stale by dispatch time. Do not set the
            // cursor or show a hint over a different window now under it.
            RefreshNativeChromePointer();
            if (m_inputMappingEnabled && m_mappingHoverCallback)
            {
                m_mappingHoverCallback(m_descriptor.id);
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (m_mappedMove) return 0;
            {
                POINT point{};
                if (m_uiCursor.Position(point) && m_uiCursor.Hit(point) == m_window)
                {
                    // Never re-arm TrackMouseEvent from a leave notification.
                    // USER32 can disagree with WindowFromPoint during the
                    // layered/pass-through handoff and post an immediate new
                    // leave. Re-arming here creates a self-sustaining queue
                    // that starves tooltip WM_PAINT and delays button input.
                    // A real move/timer will refresh the tooltip separately.
                    if (SetChromeCursor({})) return 0;
                }
            }
            HideChromeTooltip();
            ClearChromeCursor();
            return 0;
        case WM_CANCELMODE:
            CloseSpeedPopup();
            m_resizePolicy.End();
            if (m_mappedMove) EndMappedMove(true);
            break;
        case WM_CAPTURECHANGED:
            if (m_mappedMove && reinterpret_cast<HWND>(lParam) != m_window) EndMappedMove(true);
            break;
        case WM_NCMOUSEMOVE:
            if (m_mappedMove)
            {
                POINT point{};
                if (GetCursorPos(&point)) UpdateMappedMoveCursor(point);
                return 0;
            }
            // The windowed lens interior deliberately reports HTCAPTION so it
            // can be dragged. Fullscreen always reports HTCLIENT, but keep this
            // branch policy-correct for synthetic/non-client transitions.
            if (m_fullscreen)
            {
                POINT point{};
                if (GetCursorPos(&point)) UpdateFullscreenChromeFromScreenPoint(point);
                else SetChromeVisible(false);
            }
            else SetChromeVisible(true);
            HideChromeTooltip();
            ClearChromeCursor();
            break;
        case WM_SETCURSOR:
        {
            if (m_hidden || m_closeRequested || m_inputPassThrough || m_uiCursorPaused)
            {
                ClearChromeCursor(UiCursorRelease::StaleMessage);
                return TRUE;
            }
            if (m_mappedMove && reinterpret_cast<HWND>(wParam) == m_window)
            {
                POINT point{};
                if (m_uiCursor.Position(point)) UpdateMappedMoveCursor(point);
                else ClearChromeCursor(UiCursorRelease::StaleMessage);
                return TRUE;
            }
            // WM_TIMER is low priority and can be delayed by a continuous
            // capture stream. Apply the fullscreen hot-zone policy here too,
            // so WM_SETCURSOR cannot reveal chrome in the middle of the video.
            POINT cursorPoint{};
            bool haveCursor = m_uiCursor.Position(cursorPoint);
            if (!haveCursor || m_uiCursor.Hit(cursorPoint) != m_window ||
                (m_uiCursor.Capture() && m_uiCursor.Capture() != m_window))
            {
                ClearChromeCursor(UiCursorRelease::StaleMessage);
                return TRUE; // Do not let an obsolete message set our class cursor over another window.
            }
            if (m_fullscreen)
            {
                if (haveCursor) UpdateFullscreenChromeFromScreenPoint(cursorPoint);
                else SetChromeVisible(false);
            }
            else SetChromeVisible(true);
            if (reinterpret_cast<HWND>(wParam) == m_window && LOWORD(lParam) == HTCLIENT)
            {
                POINT point = cursorPoint;
                if (!haveCursor || !ScreenToClient(m_window, &point))
                {
                    ClearChromeCursor();
                    break;
                }
                if (SetChromeCursor(point))
                {
                    UpdateChromeHover(point);
                    return TRUE;
                }
            }
            ClearChromeCursor();
            break;
        }
        case WM_TIMER:
            if (m_hidden) return 0;
            if (wParam == ScreenshotFeedbackTimerId)
            {
                if (m_screenshotFeedback.Expire(GetTickCount64()))
                {
                    KillTimer(m_window, ScreenshotFeedbackTimerId);
                    UpdateChromeFromCursor();
                    RenderLatestFrame(); // Restore even when the source frame is static.
                }
                return 0;
            }
            if (wParam == FrameRetryTimerId)
            {
                // Retry only the newest frame, including the final resize of
                // a static source. Timer messages coalesce; no frame queue grows.
                if (m_frameRetryArmed && (m_fullscreenTransition.Active() || m_renderer.NeedsPresent()))
                    RenderLatestFrame();
                return 0;
            }
            if (wParam == ChromeTimerId)
            {
                if (m_mappedMove)
                {
                    POINT point{};
                    if (GetCursorPos(&point)) UpdateMappedMoveCursor(point);
                    return 0;
                }
                CheckGeometrySettled();
                UpdateChromeFromCursor();
                if (m_renderer.NeedsPresent()) RenderLatestFrame();
                return 0;
            }
            if (wParam == FullscreenHintTimerId)
            {
                HideFullscreenHint();
                return 0;
            }
            break;
        case WM_HOTKEY:
            if (m_hidden) return 0; // Ignore a queued Esc after unregistering on hide.
            if (wParam == FullscreenEscapeHotkeyId && m_speedPopupOpen) { CloseSpeedPopup(); return 0; }
            if (wParam == FullscreenEscapeHotkeyId && m_fullscreen)
            {
                ExitFullscreen();
                return 0;
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (m_hidden) return 0;
            if (wParam == VK_ESCAPE && m_speedPopupOpen) { CloseSpeedPopup(); return 0; }
            if (wParam == VK_ESCAPE && m_fullscreen)
            {
                ExitFullscreen();
                return 0;
            }
            break;
        case WM_WINDOWPOSCHANGING:
        {
            auto position = reinterpret_cast<WINDOWPOS*>(lParam);
            // WM_SIZE is delivered only after USER32 has changed the HWND. Queue
            // the DirectComposition scale here so DWM can apply the visual and
            // the new window rectangle in the same desktop composition pass.
            if (!(position->flags & SWP_NOSIZE) && position->cx > 0 && position->cy > 0)
                m_renderer.PrepareClientExtent({ UINT(position->cx), UINT(position->cy) });
            if (!m_mappedMovePositioning &&
                (!(position->flags & SWP_NOMOVE) || !(position->flags & SWP_NOSIZE)))
            {
                HideChromeTooltip();
                ClearChromeCursor();
            }
            if (!m_mappedMovePositioning && m_inputMappingEnabled &&
                (!((position->flags & SWP_NOMOVE) && (position->flags & SWP_NOSIZE))))
            {
                m_geometryChanging = true;
                m_lastGeometryChange = GetTickCount64();
                if (m_mappingGeometryChangedCallback) m_mappingGeometryChangedCallback(Id());
            }
            break;
        }
        case WM_WINDOWPOSCHANGED:
        {
            RECT bounds{};
            GetWindowRect(m_window, &bounds);
            bool changed = m_geometryChanging || !EqualRect(&bounds, &m_descriptor.windowBounds);
            m_geometryChanging = false;
            m_descriptor.windowBounds = bounds;
            if (changed) m_lastGeometryChange = GetTickCount64();
            // Let DefWindowProc generate WM_SIZE/WM_MOVE and finish stretching
            // before publishing the final physical client rectangle.
            auto result = DefWindowProcW(m_window, message, wParam, lParam);
            if (changed && !m_mappedMovePositioning && m_mappingGeometryChangedCallback)
            {
                m_mappingGeometryChangedCallback(m_descriptor.id);
            }
            return result;
        }
        case WM_CLOSE:
            if (!m_closeRequested)
            {
                m_closeRequested = true;
                m_closeCallback(m_descriptor.id);
            }
            return 0;
        case WM_NCDESTROY:
            ClearChromeCursor(UiCursorRelease::Closed);
            // Owned popups are also destroyed by Windows with their owner.
            m_chromeTooltip = nullptr;
            m_hoverButton = 0;
            m_tooltipText.clear();
            m_screenshotFeedback.Reset();
            KillTimer(m_window, ScreenshotFeedbackTimerId);
            KillTimer(m_window, ChromeTimerId);
            KillTimer(m_window, FullscreenHintTimerId);
            if (m_escapeHotkeyRegistered)
            {
                UnregisterHotKey(m_window, FullscreenEscapeHotkeyId);
                m_escapeHotkeyRegistered = false;
            }
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            m_window = nullptr;
            return 0;
        }
        return DefWindowProcW(m_window, message, wParam, lParam);
    }

    LRESULT LensWindow::HitTestWindow(POINT screenPoint) const noexcept
    {
        RECT bounds{};
        GetWindowRect(m_window, &bounds);
        POINT clientPoint = screenPoint;
        ScreenToClient(m_window, &clientPoint);
        if (ChromeButtonAt(clientPoint) != 0)
        {
            return HTCLIENT;
        }
        if (m_fullscreen)
        {
            return HTCLIENT;
        }
        bool left = screenPoint.x < bounds.left + ResizeBorder;
        bool right = screenPoint.x >= bounds.right - ResizeBorder;
        bool top = screenPoint.y < bounds.top + ResizeBorder;
        bool bottom = screenPoint.y >= bounds.bottom - ResizeBorder;
        if (left && top) return HTTOPLEFT;
        if (right && top) return HTTOPRIGHT;
        if (left && bottom) return HTBOTTOMLEFT;
        if (right && bottom) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
        if (m_inputMappingEnabled) return HTCLIENT;
        return HTCAPTION;
    }

    int LensWindow::ChromeButtonAt(POINT clientPoint, bool requireVisible) const noexcept
    {
        if (!m_window || (requireVisible && !m_chromeVisible))
        {
            return 0;
        }
        RECT client{};
        GetClientRect(m_window, &client);
        return HitTestChromeButton(clientPoint, client);
    }

    RECT LensWindow::MappingSourceRect() const noexcept
    {
        MONITORINFO info{ sizeof(info) };
        if (!m_descriptor.monitor || !GetMonitorInfoW(m_descriptor.monitor, &info))
        {
            return {};
        }
        return ToScreenRect(m_descriptor.source, info.rcMonitor);
    }

    RECT LensWindow::MappingDestinationRect() const noexcept
    {
        auto client = ContentRectInClient();
        if (IsRectEmpty(&client)) return {};
        POINT topLeft{ client.left, client.top };
        POINT bottomRight{ client.right, client.bottom };
        if (!ClientToScreen(m_window, &topLeft) || !ClientToScreen(m_window, &bottomRight))
        {
            return {};
        }
        return { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    }

    RECT LensWindow::ClientRectInScreen() const noexcept
    {
        if (!m_window) return {};
        RECT client{};
        if (!GetClientRect(m_window, &client)) return {};
        POINT topLeft{client.left,client.top}, bottomRight{client.right,client.bottom};
        if (!ClientToScreen(m_window,&topLeft) || !ClientToScreen(m_window,&bottomRight)) return {};
        return {topLeft.x,topLeft.y,bottomRight.x,bottomRight.y};
    }

    RECT LensWindow::ContentRectInClient() const noexcept
    {
        if (!m_window) return {};
        RECT client{};
        if (!GetClientRect(m_window,&client)) return {};
        return m_fullscreen && m_fullscreenAspectFit ? FitAspectRect(m_descriptor.source,client) : client;
    }

    bool LensWindow::ShouldSuspendInputMapping(POINT screenPoint) const noexcept
    {
        if (m_hidden || m_fullscreenTransition.Active()) return true;
        if (!m_window)
        {
            return false;
        }
        RECT bounds{};
        GetWindowRect(m_window, &bounds);
        if (!PtInRect(&bounds, screenPoint))
        {
            return false;
        }

        POINT clientPoint = screenPoint;
        ScreenToClient(m_window, &clientPoint);
        if (ChromeButtonAt(clientPoint, false) != 0)
        {
            return true;
        }
        if (m_fullscreen)
        {
            auto content = MappingDestinationRect();
            return IsRectEmpty(&content) || !PtInRect(&content,screenPoint);
        }
        return IsPointInResizeBorder(screenPoint, bounds, ResizeBorder);
    }

    void LensWindow::SetInputMappingEnabled(bool enabled)
    {
        if (!enabled)
        {
            if (m_mappedMove) EndMappedMove(false);
            ClearChromeCursor(UiCursorRelease::MappingDisabled);
        }
        if (m_inputMappingEnabled == enabled)
        {
            return;
        }
        m_inputMappingEnabled = enabled;
        RefreshNativeChromePointer();
        RenderLatestFrame();
        if (enabled && !m_fullscreen) ShowMappingHint();
        else if (!enabled && m_mappingHint) HideFullscreenHint();
    }

    void LensWindow::SetUiCursorPaused(bool paused)
    {
        bool changed = m_uiCursorPaused != paused;
        m_uiCursorPaused = paused;
        if (paused)
        {
            if (m_mappedMove) EndMappedMove(false);
            ClearChromeCursor(UiCursorRelease::Paused);
        }
        else if (changed) RefreshNativeChromePointer();
    }

    void LensWindow::FinishGeometryChange()
    {
        m_resizePolicy.End();
        m_inSizeMove = m_geometryChanging = false;
        GetWindowRect(m_window, &m_descriptor.windowBounds);
        m_renderer.EndLiveResize();
        RenderLatestFrame();
        if (m_mappingGeometryChangedCallback) m_mappingGeometryChangedCallback(Id());
    }

    void LensWindow::CheckGeometrySettled()
    {
        if (!m_inSizeMove && !m_geometryChanging) return;
        GUITHREADINFO info{ sizeof(info) };
        if (!GetGUIThreadInfo(GetCurrentThreadId(), &info)) return;
        if (MappingGeometryCanSettle(GetTickCount64(), m_lastGeometryChange,
            (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0,
            (info.flags & GUI_INMOVESIZE) != 0, info.hwndCapture != nullptr)) FinishGeometryChange();
    }

    bool LensWindow::SetInputPassThrough(bool enabled)
    {
        if (!m_window) return !enabled;
        // Release before changing native hit ownership, including a repeated
        // transparent request after a previous visual snapshot was cleared.
        if (enabled || m_inputPassThrough != enabled) ClearChromeCursor(UiCursorRelease::PassThrough);
        if (m_inputPassThrough == enabled) return true;
        auto extended = GetWindowLongPtrW(m_window, GWL_EXSTYLE);
        auto updated = enabled
            ? extended | static_cast<LONG_PTR>(WS_EX_TRANSPARENT)
            : extended & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(m_window, GWL_EXSTYLE, updated) && GetLastError() != ERROR_SUCCESS) return false;
        m_inputPassThrough = enabled;
        if (enabled)
        {
            HideChromeTooltip();
            ClearChromeCursor(UiCursorRelease::PassThrough);
        }
        else
        {
            // The return move happened while this HWND was transparent. Windows
            // may have delivered WM_SETCURSOR to the SOURCE instead. Refresh our
            // own control's native cursor now, before the next UI snapshot stops
            // drawing the virtual one; no extra mouse move/click is required.
            RefreshNativeChromePointer();
        }
        return true;
    }

    bool LensWindow::RefreshNativeCursorForHandoff(POINT expected)
    {
        if (!m_window || m_hidden || m_closeRequested || m_inputPassThrough ||
            m_uiCursorPaused || m_mappedMove) return false;
        RECT bounds{};
        if (!GetWindowRect(m_window, &bounds) || !PtInRect(&bounds, expected)) return false;

        auto hit = HitTestWindow(expected);
        HCURSOR desired{};
        switch (hit)
        {
        case HTLEFT: case HTRIGHT: desired = LoadCursorW(nullptr, IDC_SIZEWE); break;
        case HTTOP: case HTBOTTOM: desired = LoadCursorW(nullptr, IDC_SIZENS); break;
        case HTTOPLEFT: case HTBOTTOMRIGHT: desired = LoadCursorW(nullptr, IDC_SIZENWSE); break;
        case HTTOPRIGHT: case HTBOTTOMLEFT: desired = LoadCursorW(nullptr, IDC_SIZENESW); break;
        case HTCLIENT: case HTCAPTION: desired = m_uiCursor.Arrow(); break;
        default: return false;
        }
        auto arrow = m_uiCursor.Arrow();
        auto alternate = desired == arrow ? LoadCursorW(nullptr, IDC_SIZEALL) : arrow;
        return m_uiCursor.RefreshNativeAfterShow(m_window, expected, desired, alternate);
    }

    bool LensWindow::RaiseForInteraction()
    {
        if (!m_window || m_closeRequested || m_hidden) return false;
        return SetWindowPos(m_window, KeepsTopmost() ? HWND_TOPMOST : HWND_TOP,
            0, 0, 0, 0, TopmostPositionFlags) != FALSE;
    }

    MappingSessionConfig LensWindow::MappingConfig() const
    {
        MappingSessionConfig config;
        config.lensId = Id(); config.lensWindow = m_window;
        config.source = MappingSourceRect(); config.destination = MappingDestinationRect();
        config.lensClient = ClientRectInScreen();
        config.fullscreen = m_fullscreen; config.blocked = m_hidden || m_inSizeMove || m_geometryChanging ||
            m_speedPopupOpen || m_fullscreenTransition.Active();
        config.pointerSpeed = m_pointerSpeed;
        config.desktop.left = GetSystemMetrics(SM_XVIRTUALSCREEN); config.desktop.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        config.desktop.right = config.desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        config.desktop.bottom = config.desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        return config;
    }

    void LensWindow::SetVirtualCursor(CursorSnapshot const& cursor)
    {
        if (m_hidden) { m_virtualCursor = {}; return; }
        bool changed = m_virtualCursor.visible != cursor.visible || m_virtualCursor.shape != cursor.shape ||
            m_virtualCursor.position.x != cursor.position.x || m_virtualCursor.position.y != cursor.position.y;
        // Start the local drag while the last virtual cursor is still available
        // for a seamless visual hand-off. The input worker has already verified
        // the physical LEFT state; do not re-check GetAsyncKeyState here because
        // its DOWN was intentionally suppressed by the low-level hook.
        if (cursor.localMoveRequested) BeginMappedMove(true);
        m_virtualCursor = cursor;
        if (cursor.visible)
        {
            if (m_fullscreen) UpdateFullscreenChromeFromScreenPoint(cursor.position);
            else SetChromeVisible(true);
        }
        // The development software-cursor experiment has a single, separate
        // cursor surface. Keep this snapshot for chrome/fullscreen hit policy,
        // but do not spend a video Present on each pointer movement.
        if ((!Runtime().persistentSoftwareCursor && changed) || m_renderer.NeedsPresent()) RenderLatestFrame();
    }

    void LensWindow::SetChromeVisible(bool visible)
    {
        visible = visible || m_speedPopupOpen || m_screenshotFeedback.Active(GetTickCount64());
        if (m_chromeVisible == visible) return;
        m_chromeVisible = visible;
        if (!visible)
        {
            HideChromeTooltip();
            ClearChromeCursor();
        }
        RenderLatestFrame();
    }

    void LensWindow::UpdateChromeFromCursor()
    {
        if (m_virtualCursor.visible)
        {
            if (m_fullscreen) UpdateFullscreenChromeFromScreenPoint(m_virtualCursor.position);
            else SetChromeVisible(true);
            return;
        }
        POINT cursor{};
        RECT bounds{};
        if (!GetCursorPos(&cursor))
        {
            if (m_fullscreen) SetChromeVisible(false);
            return;
        }
        if (m_fullscreen) UpdateFullscreenChromeFromScreenPoint(cursor);
        else
        {
            GetWindowRect(m_window, &bounds);
            SetChromeVisible(PtInRect(&bounds, cursor) != FALSE);
        }
        RefreshNativeChromePointer();
    }

    void LensWindow::UpdateFullscreenChromeFromClientPoint(POINT clientPoint)
    {
        if (!m_fullscreen || !m_window) return;
        RECT client{};
        if (!GetClientRect(m_window, &client))
        {
            SetChromeVisible(false);
            return;
        }
        SetChromeVisible(FullscreenChromeRevealAt(clientPoint, client));
    }

    void LensWindow::UpdateFullscreenChromeFromScreenPoint(POINT screenPoint)
    {
        if (!m_fullscreen || !m_window) return;
        if (!ScreenToClient(m_window, &screenPoint))
        {
            SetChromeVisible(false);
            return;
        }
        UpdateFullscreenChromeFromClientPoint(screenPoint);
    }

    bool LensWindow::SetChromeCursor(POINT clientPoint)
    {
        POINT screenPoint{};
        auto capture = m_uiCursor.Capture();
        if (m_hidden || m_closeRequested || m_uiCursorPaused || !m_window ||
            !m_uiCursor.Position(screenPoint) || m_uiCursor.Hit(screenPoint) != m_window ||
            (capture && capture != m_window))
        { ClearChromeCursor(UiCursorRelease::StaleMessage); return false; }
        // Message coordinates can already be obsolete. Hit-test the same live
        // position that the ownership backend verifies before clearing a shape.
        clientPoint = screenPoint;
        if (!ScreenToClient(m_window, &clientPoint)) { ClearChromeCursor(); return false; }
        auto accepts = ChromeAcceptsNativePointer(
            m_inputPassThrough, m_inSizeMove || m_geometryChanging || m_fullscreenTransition.Active(), m_chromeVisible);
        auto button = accepts ? ChromeButtonAt(clientPoint) : 0;
        if (!button) { ClearChromeCursor(); return false; }
        auto arrow = m_uiCursor.Arrow();
        if (!arrow) return false;

        if (Runtime().persistentSoftwareCursor)
        {
            // The external cursor surface owns *all* pointer pixels in this
            // mode.  In particular, never set a null thread cursor here: a
            // delayed WM_SETCURSOR could otherwise strand the desktop arrow.
            ClearChromeCursor();
            m_uiCursor.NativeArrow();
            return true;
        }

        if (ChromeUsesSoftwareCursor(m_inputMappingEnabled, accepts, button))
        {
            auto epoch = m_uiCursor.Acquire(m_window, Id(), UiCursorPurpose::Chrome,
                screenPoint, accepts, uint32_t(m_virtualCursor.phase));
            if (!epoch) { ClearChromeCursor(UiCursorRelease::StaleMessage); return false; }
            m_uiCursorEpoch = epoch;
            bool changed = !m_chromeCursor.visible || m_chromeCursor.shape != arrow ||
                m_chromeCursor.position.x != screenPoint.x || m_chromeCursor.position.y != screenPoint.y;
            m_chromeCursor.visible = true;
            m_chromeCursor.position = screenPoint;
            m_chromeCursor.shape = arrow;
            if (changed) RenderLatestFrame();
        }
        else
        {
            ClearChromeCursor();
            m_uiCursor.NativeArrow();
        }
        return true;
    }

    void LensWindow::ClearChromeCursor(UiCursorRelease reason)
    {
        if (m_uiCursor.Release(m_window, m_uiCursorEpoch, reason, uint32_t(m_virtualCursor.phase)))
            m_uiCursorEpoch = 0;
        bool visible = m_chromeCursor.visible;
        m_chromeCursor = {};
        if (visible) RenderLatestFrame();
    }

    void LensWindow::UpdateChromeHover(POINT clientPoint)
    {
        if (m_hidden || m_closeRequested || m_uiCursorPaused) { HideChromeTooltip(); return; }
        auto button = ChromeAcceptsNativePointer(m_inputPassThrough,
            m_inSizeMove || m_geometryChanging || m_fullscreenTransition.Active(), m_chromeVisible)
            ? ChromeButtonAt(clientPoint) : 0;
        if (!button) { HideChromeTooltip(); return; }
        TRACKMOUSEEVENT tracking{ sizeof(tracking), TME_LEAVE, m_window, 0 };
        TrackMouseEvent(&tracking);
        ShowChromeTooltip(button);
    }

    void LensWindow::RefreshNativeChromePointer()
    {
        POINT point{};
        // Runs only on the UI thread, never in the hook or input worker. In an
        // overlap only the actual hit window may set a cursor/show its tooltip.
        if (!m_window || m_hidden || m_closeRequested || m_uiCursorPaused || !m_uiCursor.Position(point))
        {
            if (m_fullscreen) SetChromeVisible(false);
            HideChromeTooltip();
            ClearChromeCursor();
            return;
        }
        if (m_fullscreen) UpdateFullscreenChromeFromScreenPoint(point);
        if (m_inputPassThrough || m_uiCursor.Hit(point) != m_window ||
            (m_uiCursor.Capture() && m_uiCursor.Capture() != m_window))
        {
            HideChromeTooltip();
            ClearChromeCursor();
            return;
        }
        ScreenToClient(m_window, &point);
        if (!SetChromeCursor(point)) ClearChromeCursor();
        UpdateChromeHover(point);
    }

    void LensWindow::ShowChromeTooltip(int button)
    {
        if (m_speedPopupOpen) return;
        auto text = ChromeTooltipText(button, m_inputMappingEnabled, m_fullscreen, m_descriptor.topmost,
            m_pointerSpeed.Adjusted());
        if (m_chromeTooltip && m_hoverButton == button && m_tooltipText == text && IsWindowVisible(m_chromeTooltip)) return;
        auto dpi = static_cast<int>(GetDpiForWindow(m_window));
        auto dc = GetDC(m_window);
        if (!dc) return;
        auto font = ChromeTooltipFont(m_window);
        auto oldFont = SelectObject(dc, font);
        SIZE size{};
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &size);
        SelectObject(dc, oldFont);
        DeleteObject(font);
        ReleaseDC(m_window, dc);
        size.cx += MulDiv(20, dpi, 96);
        size.cy += MulDiv(12, dpi, 96);
        auto bounds = ChromeTooltipBounds(ClientRectInScreen(), size, MulDiv(6, dpi, 96));
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) return;
        if (!m_chromeTooltip)
        {
            // Owned, no-activate and layered+transparent: no new hit surface,
            // taskbar entry or focus change. Exclude it before the first show.
            m_chromeTooltip = CreateWindowExW(
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
                ChromeTooltipClassName(), text, WS_POPUP, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                m_window, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!m_chromeTooltip) return;
            if (!SetLayeredWindowAttributes(m_chromeTooltip, 0, 245, LWA_ALPHA) ||
                !SetWindowDisplayAffinity(m_chromeTooltip, WDA_EXCLUDEFROMCAPTURE))
            {
                DestroyWindow(std::exchange(m_chromeTooltip, nullptr));
                return;
            }
        }
        m_hoverButton = button;
        m_tooltipText = text;
        SetWindowTextW(m_chromeTooltip, text);
        SetWindowPos(m_chromeTooltip, HWND_TOP, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        // Commit the complete new label at the new size on a button change.
        // This is bounded to hover/text changes, not every capture/cursor frame.
        RedrawWindow(m_chromeTooltip, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    }

    void LensWindow::HideChromeTooltip()
    {
        if (m_chromeTooltip && IsWindowVisible(m_chromeTooltip)) ShowWindow(m_chromeTooltip, SW_HIDE);
        m_hoverButton = 0;
        m_tooltipText.clear();
    }

    void LensWindow::BeginMappedMove(bool inputEngineConfirmedGesture)
    {
        if (!m_window || m_hidden || m_closeRequested || m_uiCursorPaused || m_inputPassThrough ||
            !CanBeginMappedMove(m_inputMappingEnabled, m_fullscreen,
            inputEngineConfirmedGesture, m_mappedMove)) return;
        if (!GetCursorPos(&m_mappedMoveAnchor) || !GetWindowRect(m_window, &m_mappedMoveBounds)) return;
        HideChromeTooltip();
        ClearChromeCursor();
        if (m_mappingHint) HideFullscreenHint();
        SetCapture(m_window);
        if (GetCapture() != m_window)
        {
            if (m_uiCursor.Hit(m_mappedMoveAnchor) == m_window)
                m_uiCursor.NativeArrow();
            return;
        }
        m_mappedMove = true;
        UpdateMappedMoveCursor(m_mappedMoveAnchor);
    }

    void LensWindow::UpdateMappedMove()
    {
        if (!m_mappedMove || !m_window) return;
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return;
        auto origin = MappedMoveOrigin(m_mappedMoveBounds, m_mappedMoveAnchor, cursor);
        m_mappedMovePositioning = true;
        SetWindowPos(m_window, nullptr, origin.x, origin.y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        m_mappedMovePositioning = false;
        UpdateMappedMoveCursor(cursor);
    }

    void LensWindow::EndMappedMove(bool publishGeometry)
    {
        if (!m_mappedMove) return;
        m_mappedMove = false;
        ClearChromeCursor(m_uiCursor.Capture() == m_window ?
            UiCursorRelease::DragEnded : UiCursorRelease::CaptureLost);
        if (m_window && GetCapture() == m_window)
        {
            ReleaseCapture();
        }
        if (m_window) GetWindowRect(m_window, &m_descriptor.windowBounds);
        m_lastGeometryChange = GetTickCount64();
        if (publishGeometry && m_mappingGeometryChangedCallback)
            m_mappingGeometryChangedCallback(Id());
    }

    void LensWindow::UpdateMappedMoveCursor(POINT screenPoint)
    {
        if (Runtime().persistentSoftwareCursor)
        {
            ClearChromeCursor(UiCursorRelease::DragEnded);
            m_uiCursor.NativeArrow();
            return;
        }
        auto epoch = m_uiCursor.Acquire(m_window, Id(), UiCursorPurpose::Drag, screenPoint,
            m_mappedMove && !m_hidden && !m_closeRequested && !m_inputPassThrough && !m_uiCursorPaused,
            uint32_t(m_virtualCursor.phase));
        if (!epoch) { ClearChromeCursor(UiCursorRelease::CaptureLost); return; }
        m_uiCursorEpoch = epoch;
        auto arrow = m_uiCursor.Arrow();
        bool changed = !m_chromeCursor.visible || m_chromeCursor.shape != arrow ||
            m_chromeCursor.position.x != screenPoint.x || m_chromeCursor.position.y != screenPoint.y;
        m_chromeCursor.visible = true;
        m_chromeCursor.position = screenPoint;
        m_chromeCursor.shape = arrow;
        // During this captured local drag the lens always owns the pointer.
        // Draw the arrow in the composition swap chain so a delayed
        // MagShowSystemCursor transition cannot leave the drag cursor blank.
        if (changed || m_renderer.NeedsPresent()) RenderLatestFrame();
    }

    void LensWindow::ToggleTopmost()
    {
        m_descriptor.topmost = !m_descriptor.topmost;
        SetWindowPos(m_window, m_fullscreen || m_descriptor.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        RenderLatestFrame();
    }

    void LensWindow::ToggleFullscreen()
    {
        if (m_fullscreenTransition.Active())
        {
            CancelFullscreenTransition(true, true);
            return;
        }
        if (m_fullscreen)
        {
            ExitFullscreen();
        }
        else
        {
            EnterFullscreen();
        }
    }

    void LensWindow::RestoreInitialSize()
    {
        if (!m_window || m_initialSize.cx <= 0 || m_initialSize.cy <= 0) return;
        CancelFullscreenTransition(true, true);
        RECT current{};
        if (!GetWindowRect(m_window, &current)) return;
        auto restored = InitialSizeAtCurrentOrigin(current, m_initialSize);

        HideFullscreenHint();
        HideChromeTooltip();
        ClearChromeCursor();
        if (m_escapeHotkeyRegistered)
        {
            UnregisterHotKey(m_window, FullscreenEscapeHotkeyId);
            m_escapeHotkeyRegistered = false;
        }
        // Reset means size only. In fullscreen the current origin is the
        // monitor origin; do not route through ExitFullscreen, which would
        // restore the old pre-fullscreen rectangle and position.
        m_fullscreen = false;
        SetWindowPos(
            m_window,
            m_descriptor.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            restored.left,
            restored.top,
            restored.right - restored.left,
            restored.bottom - restored.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        UpdateChromeFromCursor();
        RenderLatestFrame();
    }

    void LensWindow::EnterFullscreen()
    {
        if (m_fullscreen || !m_window)
        {
            return;
        }

        GetWindowRect(m_window, &m_restoreBounds);
        auto monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            return;
        }

        if (m_fullscreenAspectFit)
        {
            BeginFullscreenTransition(info.rcMonitor);
            return;
        }

        m_fullscreen = true;
        HideChromeTooltip();
        ClearChromeCursor();
        SetChromeVisible(false);
        SetWindowPos(
            m_window,
            HWND_TOPMOST,
            info.rcMonitor.left,
            info.rcMonitor.top,
            info.rcMonitor.right - info.rcMonitor.left,
            info.rcMonitor.bottom - info.rcMonitor.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetForegroundWindow(m_window);
        SetFocus(m_window);
        m_escapeHotkeyRegistered = RegisterHotKey(
            m_window, FullscreenEscapeHotkeyId, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
        ShowFullscreenHint();
        UpdateChromeFromCursor();
        RenderLatestFrame();
    }

    void LensWindow::BeginFullscreenTransition(RECT targetBounds)
    {
        if (!m_window || m_hidden || m_closeRequested || m_fullscreen || m_fullscreenTransition.Active()) return;
        if (!m_latestView)
        {
            ShowHint(Localized(L"尚无可用画面，未进入全屏", L"No frame is available; full screen was not entered"), false);
            return;
        }

        HideChromeTooltip();
        ClearChromeCursor();
        m_chromeVisible = false;
        m_fullscreenTransitionBounds = targetBounds;
        m_fullscreenTransition.Begin();
        // Publish the blocked geometry before rendering. The coordinator drains
        // the sole active takeover but preserves every standby button.
        if (m_inputMappingEnabled && m_mappingGeometryChangedCallback)
            m_mappingGeometryChangedCallback(Id());
        TryFullscreenTransition();
    }

    void LensWindow::TryFullscreenTransition()
    {
        if (!m_fullscreenTransition.Active()) return;
        if (!m_window || m_hidden || m_closeRequested || !m_latestView)
        {
            CancelFullscreenTransition(false, true);
            return;
        }
        auto now = GetTickCount64();
        if (!m_fullscreenTransition.Ready(now)) return;

        RECT current{};
        if (!GetClientRect(m_window, &current) || current.right <= current.left || current.bottom <= current.top)
        {
            CancelFullscreenTransition(true, true);
            ShowHint(Localized(L"无法准备全屏画面，已保持原窗口",
                L"Could not prepare full screen; the original window was kept"), false);
            return;
        }
        auto targetWidth = m_fullscreenTransitionBounds.right - m_fullscreenTransitionBounds.left;
        auto targetHeight = m_fullscreenTransitionBounds.bottom - m_fullscreenTransitionBounds.top;
        RECT targetClient{ 0, 0, targetWidth, targetHeight };
        auto fitted = FitAspectRect(m_descriptor.source, targetClient);
        auto projected = ProjectContentRect(fitted, targetWidth, targetHeight,
            current.right - current.left, current.bottom - current.top);
        if (projected.Empty())
        {
            CancelFullscreenTransition(true, true);
            ShowHint(Localized(L"无法准备全屏画面，已保持原窗口",
                L"Could not prepare full screen; the original window was kept"), false);
            return;
        }

        // The transition is intentionally smooth and UI-free. Its sole job is
        // to put correctly normalized black bars into the retained swap-chain
        // frame before USER32 enlarges the HWND.
        bool rendered = m_renderer.Render(
            m_latestView.Get(), m_descriptor.source, m_textureWidth, m_textureHeight,
            std::nullopt, false, m_descriptor.topmost, true, false, nullptr,
            LensQualitySettings{}, m_captureStamp, false, false, projected);
        if (!rendered)
        {
            CancelFullscreenTransition(true, true);
            ShowHint(Localized(L"无法准备全屏画面，已保持原窗口",
                L"Could not prepare full screen; the original window was kept"), false);
            return;
        }
        if (m_renderer.NeedsPresent())
        {
            if (m_fullscreenTransition.OnBusy(now))
            {
                if (!m_frameRetryArmed)
                    m_frameRetryArmed = SetTimer(m_window, FrameRetryTimerId,
                        FullscreenTransitionGate::RetryDelayMilliseconds, nullptr) != 0;
                if (!m_frameRetryArmed)
                {
                    CancelFullscreenTransition(true, true);
                    ShowHint(Localized(L"显卡繁忙，未进入全屏，请重试",
                        L"GPU busy; full screen was not entered. Try again"), false);
                }
                return;
            }
            CancelFullscreenTransition(true, true);
            ShowHint(Localized(L"显卡繁忙，未进入全屏，请重试",
                L"GPU busy; full screen was not entered. Try again"), false);
            return;
        }

        CommitFullscreenTransition();
    }

    bool LensWindow::CommitFullscreenTransition()
    {
        if (!m_fullscreenTransition.Active() || !m_window) return false;
        auto target = m_fullscreenTransitionBounds;
        m_fullscreenTransition.Finish();
        m_fullscreenTransitionBounds = {};
        m_renderer.BeginProgrammaticResize();
        m_fullscreen = true;
        auto positioned = SetWindowPos(m_window, HWND_TOPMOST,
            target.left, target.top, target.right - target.left, target.bottom - target.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED) != FALSE;
        m_renderer.EndProgrammaticResize();
        if (!positioned)
        {
            m_fullscreen = false;
            m_renderer.StretchToClient();
            RenderLatestFrame();
            if (m_inputMappingEnabled && m_mappingGeometryChangedCallback)
                m_mappingGeometryChangedCallback(Id());
            ShowHint(Localized(L"无法进入全屏，已保持原窗口",
                L"Could not enter full screen; the original window was kept"), false);
            return false;
        }

        SetForegroundWindow(m_window);
        SetFocus(m_window);
        m_escapeHotkeyRegistered = RegisterHotKey(
            m_window, FullscreenEscapeHotkeyId, MOD_NOREPEAT, VK_ESCAPE) != FALSE;
        ShowFullscreenHint();
        UpdateChromeFromCursor();
        RenderLatestFrame();
        return true;
    }

    void LensWindow::CancelFullscreenTransition(bool redraw, bool notifyMapping)
    {
        if (!m_fullscreenTransition.Active()) return;
        m_fullscreenTransition.Finish();
        m_renderer.EndProgrammaticResize();
        m_fullscreenTransitionBounds = {};
        if (notifyMapping && m_inputMappingEnabled && m_mappingGeometryChangedCallback)
            m_mappingGeometryChangedCallback(Id());
        if (redraw && m_window && !m_hidden && !m_closeRequested)
        {
            // A successfully presented preframe followed by SetWindowPos
            // failure must not leave letterbox bars in the original window.
            UpdateChromeFromCursor();
            RenderLatestFrame();
        }
    }

    void LensWindow::ExitFullscreen()
    {
        if (!m_fullscreen || !m_window)
        {
            return;
        }

        HideFullscreenHint();
        HideChromeTooltip();
        ClearChromeCursor();
        if (m_escapeHotkeyRegistered)
        {
            UnregisterHotKey(m_window, FullscreenEscapeHotkeyId);
            m_escapeHotkeyRegistered = false;
        }
        m_fullscreen = false;
        SetWindowPos(
            m_window,
            m_descriptor.topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
            m_restoreBounds.left,
            m_restoreBounds.top,
            m_restoreBounds.right - m_restoreBounds.left,
            m_restoreBounds.bottom - m_restoreBounds.top,
            SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        UpdateChromeFromCursor();
        RenderLatestFrame();
    }

    void LensWindow::ShowFullscreenHint()
    {
        // A still-visible mapping hint must not follow this lens into a later
        // fullscreen transition, even when the fullscreen hint was already
        // consumed for this lens instance.
        if (m_mappingHint) HideFullscreenHint();
        if (!m_fullscreenHintShown && ShowHint(Localized(L"按 Esc 键退出全屏", L"Press Esc to exit full screen"), true))
            m_fullscreenHintShown = true;
    }

    void LensWindow::ShowMappingHint()
    {
        if (!m_mappingHintShown && ShowHint(Localized(
            L"按住 Ctrl + 鼠标左键拖动可移动实时区域",
            L"Hold Ctrl and left-drag to move the live region"), false))
            m_mappingHintShown = true;
    }

    bool LensWindow::ShowHint(wchar_t const* text, bool fullscreenPlacement)
    {
        if (m_hidden) return false;
        HideFullscreenHint();
        if (!m_window)
        {
            return false;
        }

        auto monitor = MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            return false;
        }
        auto dpi = GetDpiForWindow(m_window);
        LONG hintWidth = MulDiv(520, static_cast<int>(dpi), 96);
        LONG hintHeight = MulDiv(64, static_cast<int>(dpi), 96);
        LONG margin = MulDiv(8, static_cast<int>(dpi), 96);
        auto workWidth = info.rcWork.right - info.rcWork.left;
        auto workHeight = info.rcWork.bottom - info.rcWork.top;
        hintWidth = std::min(hintWidth, std::max(1L, workWidth - 2 * margin));
        hintHeight = std::min(hintHeight, std::max(1L, workHeight - 2 * margin));
        LONG hintLeft{}, hintTop{};
        if (fullscreenPlacement)
        {
            hintLeft = info.rcMonitor.left + ((info.rcMonitor.right - info.rcMonitor.left) - hintWidth) / 2;
            hintTop = info.rcMonitor.top + MulDiv(48, static_cast<int>(dpi), 96);
        }
        else
        {
            RECT lens{};
            GetWindowRect(m_window, &lens);
            hintLeft = lens.left + ((lens.right - lens.left) - hintWidth) / 2;
            hintLeft = std::clamp(hintLeft, info.rcWork.left + margin, info.rcWork.right - margin - hintWidth);
            hintTop = lens.top - hintHeight - margin;
            if (hintTop < info.rcWork.top + margin)
                hintTop = std::clamp(lens.top + MulDiv(48, static_cast<int>(dpi), 96),
                    info.rcWork.top + margin, info.rcWork.bottom - margin - hintHeight);
        }

        m_fullscreenHint = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT,
            FullscreenHintClassName(),
            text,
            WS_POPUP,
            hintLeft,
            hintTop,
            hintWidth,
            hintHeight,
            m_window,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (!m_fullscreenHint)
        {
            return false;
        }
        SetLayeredWindowAttributes(m_fullscreenHint, 0, 235, LWA_ALPHA);
        SetWindowRgn(
            m_fullscreenHint,
            CreateRoundRectRgn(0, 0, hintWidth + 1, hintHeight + 1, hintHeight, hintHeight),
            TRUE);
        if (!SetWindowDisplayAffinity(m_fullscreenHint, WDA_EXCLUDEFROMCAPTURE))
        {
            auto hint = std::exchange(m_fullscreenHint, nullptr);
            DestroyWindow(hint);
            return false;
        }
        ShowWindow(m_fullscreenHint, SW_SHOWNOACTIVATE);
        m_mappingHint = !fullscreenPlacement;
        m_hintDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        SetTimer(m_window, FullscreenHintTimerId, 3000, nullptr);
        return true;
    }

    void LensWindow::HideFullscreenHint()
    {
        if (m_window)
        {
            KillTimer(m_window, FullscreenHintTimerId);
        }
        if (m_fullscreenHint)
        {
            auto hint = std::exchange(m_fullscreenHint, nullptr);
            DestroyWindow(hint);
        }
        m_mappingHint = false;
    }

    HRESULT LensWindow::CaptureScreenshot(ScreenshotRenderer& renderer, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging)
    {
        if (!m_window || m_closeRequested || !m_latestView) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
        if (m_inSizeMove || m_geometryChanging) return HRESULT_FROM_WIN32(ERROR_BUSY);
        auto client = ContentRectInClient();
        if (IsRectEmpty(&client)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        return renderer.Capture(m_device->Device(), m_device->Context(), m_latestView.Get(), m_descriptor.source,
            { UINT(m_textureWidth), UINT(m_textureHeight) },
            { UINT(client.right-client.left), UINT(client.bottom-client.top) },
            m_qualitySettings, m_captureStamp, staging);
    }

    void LensWindow::NotifyScreenshotSuccess()
    {
        if (!m_window || m_hidden || m_closeRequested) return;
        // Arm expiry before displaying success, so a timer allocation failure
        // cannot leave a permanently highlighted button.
        // Poll only while the pulse exists. Timer granularity/early delivery
        // must not extend a static frame's pulse by another full pulse period.
        if (!SetTimer(m_window, ScreenshotFeedbackTimerId, 50, nullptr)) return;
        m_screenshotFeedback.Start(GetTickCount64());
        m_chromeVisible = true; // Keep feedback visible briefly in fullscreen too.
        RenderLatestFrame();
    }

    void LensWindow::RenderLatestFrame()
    {
        RequestPresentation();
        if (!m_presentationRequest) FlushPresentation(); // Owned offscreen/test lenses.
    }

    void LensWindow::RequestPresentation()
    {
        m_presentationDirty=true;
        if (!m_hidden && m_window && !m_closeRequested && m_presentationRequest) m_presentationRequest();
    }

    bool LensWindow::ReadyForPresentation()
    {
        if (!m_presentationDirty || m_hidden || !m_window || m_closeRequested) return false;
        // The transition owns its bounded retry/cancellation. Do not gate its
        // second attempt on GPU readiness or it could remain pending forever.
        if (m_fullscreenTransition.Active()) return m_fullscreenTransition.Ready(GetTickCount64());
        auto result=m_renderer.PreparePresentation();
        if (result.state==RenderResult::Deferred && !m_renderer.EventDrivenPresentation() && !m_frameRetryArmed)
            m_frameRetryArmed=SetTimer(m_window,FrameRetryTimerId,16,nullptr)!=0;
        if (result.state==RenderResult::Failed) {
            m_presentationDirty=false;
            if (m_inputMappingEnabled && !m_renderFailurePosted) {
                m_renderFailurePosted=true;PostMessageW(m_window,RenderFailureMessage,0,0);
            }
        }
        return result.state==RenderResult::Presented;
    }

    void LensWindow::FlushPresentation()
    {
        if (!m_presentationDirty || m_hidden || m_closeRequested) return;
        m_presentationDirty=false;
        if (m_fullscreenTransition.Active())
        {
            TryFullscreenTransition();
            return;
        }
        if (!m_hidden && m_window && m_latestView)
        {
            auto cursor = Runtime().persistentSoftwareCursor ? nullptr :
                (m_virtualCursor.visible ? &m_virtualCursor :
                    (m_chromeCursor.visible ? &m_chromeCursor : nullptr));
            std::optional<PixelRect> content;
            if (m_fullscreen && m_fullscreenAspectFit) {
                auto rect=ContentRectInClient();
                if (!IsRectEmpty(&rect)) content=PixelRect{rect.left,rect.top,rect.right-rect.left,rect.bottom-rect.top};
            }
            auto rendered = m_renderer.Render(
                m_latestView.Get(),
                m_descriptor.source,
                m_textureWidth,
                m_textureHeight,
                std::nullopt,
                m_chromeVisible,
                m_descriptor.topmost,
                m_fullscreen,
                m_inputMappingEnabled,
                cursor, m_qualitySettings, m_captureStamp, m_pointerSpeed.Adjusted(),
                m_screenshotFeedback.Active(GetTickCount64()), content);
            if (FAILED(m_renderer.TakeQualityFailure())) {
                m_qualitySettings = {};
                ShowHint(Localized(L"清晰处理不可用，已恢复流畅模式", L"Clear rendering unavailable; using Smooth"), false);
            }
            bool retry = rendered && m_renderer.NeedsPresent();
            m_presentationDirty=retry;
            if (retry && !m_renderer.EventDrivenPresentation() && !m_frameRetryArmed)
                m_frameRetryArmed = SetTimer(m_window, FrameRetryTimerId, 16, nullptr) != 0;
            else if (!retry && m_frameRetryArmed)
            {
                KillTimer(m_window, FrameRetryTimerId);
                m_frameRetryArmed = false;
            }
            if (!rendered && m_inputMappingEnabled && !m_renderFailurePosted)
            {
                // A missing virtual cursor/render device must not leave the
                // hidden real cursor controlling a desktop the user cannot see.
                m_renderFailurePosted = true;
                PostMessageW(m_window, RenderFailureMessage, 0, 0);
            }
            else if (rendered) m_renderFailurePosted = false;
            // Arm the one-shot wait after a nonblocking Present reports busy.
            if (retry && m_renderer.EventDrivenPresentation()) {
                auto next=m_renderer.PreparePresentation();
                if (next.state==RenderResult::Presented && m_presentationRequest) m_presentationRequest();
            }
        }
    }
}
