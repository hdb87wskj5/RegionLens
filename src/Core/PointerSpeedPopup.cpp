#include "pch.h"
#include "PointerSpeedPopup.h"
#include "Localization.h"
#include "AppRuntime.h"
#include <dwmapi.h>
#pragma comment(lib, "comctl32.lib")

namespace RegionLens::native
{
    PointerSpeedTrack ConfigurePointerSpeedSlider(HWND slider, PointerSpeed value)
    {
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(PointerSpeed::Minimum, PointerSpeed::Maximum));
        SendMessageW(slider, TBM_SETLINESIZE, 0, 1);
        SendMessageW(slider, TBM_SETPAGESIZE, 0, 1);
        // Run before showing the popup. Redraw=TRUE also updates the native
        // thumb geometry; FALSE leaves TBM_GETTHUMBRECT at its previous layout.
        // Cache actual endpoints once, so a later click has only one position
        // change and never briefly displays a minimum/maximum measurement.
        RECT minimum{}, maximum{};
        SendMessageW(slider, TBM_SETPOS, TRUE, PointerSpeed::Minimum);
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&minimum));
        SendMessageW(slider, TBM_SETPOS, TRUE, PointerSpeed::Maximum);
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&maximum));
        SendMessageW(slider, TBM_SETPOS, TRUE, value.Tick());
        return { minimum.left + (minimum.right - minimum.left) / 2,
            maximum.left + (maximum.right - maximum.left) / 2 };
    }
    bool SeekPointerSpeedSlider(HWND slider, POINT& press, PointerSpeedTrack track)
    {
        if (track.last <= track.first) return false;
        RECT thumb{};
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        if (PtInRect(&thumb, press)) return false; // Keep the ordinary thumb grab offset.
        auto speed = PointerSpeedAtTrackPosition(press.x, track.first, track.last);
        SendMessageW(slider, TBM_SETPOS, TRUE, speed.Tick());
        SendMessageW(slider, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
        if (thumb.right <= thumb.left || thumb.bottom <= thumb.top) return false;
        // Forward this local press INSIDE the new thumb. Native handling then
        // starts a drag, never its page-step/repeat timer. Mouse moves/releases
        // and capture cancellation continue to use the native control path.
        press.x = std::clamp(press.x, thumb.left, thumb.right - 1);
        press.y = thumb.top + (thumb.bottom - thumb.top) / 2;
        return true;
    }
    PointerSpeedPopup::~PointerSpeedPopup()
    {
        m_closed = {}; m_changed = {};
        Close();
    }

    bool PointerSpeedPopup::Show(HWND owner, POINT anchor, PointerSpeed value,
        std::function<void(PointerSpeed)> changed, std::function<void()> closed)
    {
        if (m_window) { SetForegroundWindow(m_window); return true; }
        m_value = { value.Tick() }; m_changed = std::move(changed); m_closed = std::move(closed);
        m_wheelRemainder = 0;
        m_dpi = static_cast<int>(GetDpiForWindow(owner));
        auto px = [this](int v) { return MulDiv(v, m_dpi, 96); };
        INITCOMMONCONTROLSEX controls{ sizeof(controls), ICC_BAR_CLASSES };
        if (!InitCommonControlsEx(&controls)) return false;
        auto name = WindowClassName(*Runtime().identity, L"PointerSpeed");
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.lpfnWndProc = WindowProc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW); wc.lpszClassName = name.c_str();
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        MONITORINFO monitor{ sizeof(monitor) };
        if (!GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        auto bounds = PointerSpeedPopupBounds(anchor, { px(300), px(128) }, monitor.rcWork);
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW, name.c_str(), Localized(L"鼠标速度", L"Mouse speed"),
            WS_POPUP | WS_BORDER | WS_CLIPCHILDREN, bounds.left, bounds.top,
            bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr, wc.hInstance, this);
        if (!m_window) return false;
        // A lens can straddle monitors: size the popup using the DPI of its
        // own monitor rather than assuming the owner's DPI is identical.
        m_dpi = static_cast<int>(GetDpiForWindow(m_window));
        bounds = PointerSpeedPopupBounds(anchor, { px(300), px(128) }, monitor.rcWork);
        SetWindowPos(m_window, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
            bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
        // Fail closed: never briefly show a popup that can feed back into capture.
        if (!SetWindowDisplayAffinity(m_window, WDA_EXCLUDEFROMCAPTURE)) { Close(); return false; }
        DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(m_window, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        m_font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        RECT client{}; GetClientRect(m_window, &client);
        m_slider = CreateWindowExW(0, TRACKBAR_CLASSW, Localized(L"鼠标速度", L"Mouse speed"),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
            px(16), px(49), client.right - px(32), px(32), m_window, nullptr, wc.hInstance, nullptr);
        if (!m_font || !m_slider || !SetWindowSubclass(m_slider, SliderProc, 1, reinterpret_cast<DWORD_PTR>(this)))
        { Close(); return false; }
        m_track = ConfigurePointerSpeedSlider(m_slider, m_value);
        if (m_track.last <= m_track.first) { Close(); return false; }
        m_shown = true;
        ShowWindow(m_window, SW_SHOWNORMAL);
        SetForegroundWindow(m_window);
        SetFocus(m_slider);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return true;
    }

    void PointerSpeedPopup::Close()
    {
        if (m_window) {
            // Drain the trackbar's native capture before restoring mapped input.
            if (GetCapture() == m_slider) ReleaseCapture();
            DestroyWindow(m_window);
        }
    }

    void PointerSpeedPopup::UpdateValue()
    {
        PointerSpeed next{ static_cast<int>(SendMessageW(m_slider, TBM_GETPOS, 0, 0)) };
        if (next == m_value) return;
        m_value = next;
        InvalidateRect(m_window, nullptr, FALSE);
        if (m_changed) m_changed(next);
    }

    LRESULT CALLBACK PointerSpeedPopup::SliderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR id, DWORD_PTR data)
    {
        auto self = reinterpret_cast<PointerSpeedPopup*>(data);
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK) {
            message = WM_LBUTTONDOWN;
            POINT press{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (SeekPointerSpeedSlider(window, press, self->m_track)) {
                lParam = MAKELPARAM(press.x, press.y);
                self->UpdateValue(); // TBM_SETPOS itself does not send WM_HSCROLL.
            }
        }
        if (message == WM_KEYDOWN && (wParam == VK_ESCAPE || wParam == VK_RETURN)) {
            PostMessageW(self->m_window, WM_CLOSE, 0, 0); return 0;
        }
        if (message == WM_MOUSEWHEEL) {
            // One notch = one 0.05 step, irrespective of system scroll-line settings.
            self->m_wheelRemainder += GET_WHEEL_DELTA_WPARAM(wParam);
            auto steps = self->m_wheelRemainder / WHEEL_DELTA;
            self->m_wheelRemainder %= WHEEL_DELTA;
            auto tick = std::clamp(self->m_value.Tick() + steps,
                PointerSpeed::Minimum, PointerSpeed::Maximum);
            SendMessageW(window, TBM_SETPOS, TRUE, tick); self->UpdateValue(); return 0;
        }
        if (message == WM_NCDESTROY) RemoveWindowSubclass(window, SliderProc, id);
        return DefSubclassProc(window, message, wParam, lParam);
    }

    LRESULT CALLBACK PointerSpeedPopup::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto self = reinterpret_cast<PointerSpeedPopup*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<PointerSpeedPopup*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
            self->m_window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CTLCOLORSTATIC:
            SetBkColor(reinterpret_cast<HDC>(wParam), GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && self->m_shown) PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        case WM_THEMECHANGED: case WM_DPICHANGED: case WM_DISPLAYCHANGE:
            if (self->m_shown) PostMessageW(window, WM_CLOSE, 0, 0);
            return 0;
        case WM_CANCELMODE: case WM_CLOSE:
            self->Close(); return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE || wParam == VK_RETURN) { self->Close(); return 0; }
            break;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lParam) == self->m_slider) self->UpdateValue();
            return 0;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{}; auto dc = BeginPaint(window, &paint);
            RECT client{}; GetClientRect(window, &client); FillRect(dc, &client, GetSysColorBrush(COLOR_WINDOW));
            auto old = SelectObject(dc, self->m_font ? self->m_font : GetStockObject(DEFAULT_GUI_FONT));
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
            auto px = [self](int v) { return MulDiv(v, self->m_dpi, 96); };
            RECT title{ px(18), px(10), client.right - px(18), px(43) };
            DrawTextW(dc, Localized(L"鼠标速度", L"Mouse speed"), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            auto value = PointerSpeedText(self->m_value);
            DrawTextW(dc, value.c_str(), -1, &title, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            RECT label{ px(20), px(87), client.right - px(20), px(116) };
            DrawTextW(dc, L"0.50×", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
            DrawTextW(dc, L"2.50×", -1, &label, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
            SelectObject(dc, old); EndPaint(window, &paint); return 0;
        }
        case WM_NCDESTROY:
        {
            self->m_window = self->m_slider = nullptr; self->m_shown = false;
            if (self->m_font) DeleteObject(std::exchange(self->m_font, nullptr));
            auto closed = std::move(self->m_closed);
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            auto result = DefWindowProcW(window, message, wParam, lParam);
            if (closed) closed();
            return result;
        }
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}
