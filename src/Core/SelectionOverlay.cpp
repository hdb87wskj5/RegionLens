#include "pch.h"
#include "SelectionOverlay.h"
#include "Localization.h"
#include "RegionTransform.h"
#include "SelectionUpdatePolicy.h"

namespace
{
    wchar_t const* OverlayClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"SelectionOverlay");
        return name.c_str();
    }
    wchar_t const* HandleClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"SelectionHandle");
        return name.c_str();
    }
    wchar_t const* ToolbarClassName() {
        static auto const name = RegionLens::native::WindowClassName(*RegionLens::native::Runtime().identity, L"SelectionToolbar");
        return name.c_str();
    }
    constexpr int ConfirmButtonId = 1001;
    constexpr int CancelButtonId = 1002;
    constexpr int HandleSize = 10;
    constexpr int HitMargin = 10;
    constexpr int MinimumSelection = 16;
    constexpr UINT_PTR VisualTimerId = 1;

    LRESULT CALLBACK ToolbarProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_COMMAND)
        {
            return SendMessageW(GetParent(window), message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

namespace RegionLens::native
{
    SelectionOverlay::SelectionOverlay(
        std::shared_ptr<D3DDevice> device,
        HMONITOR monitor,
        RECT monitorBounds,
        CapturedTexture frozen,
        ConfirmCallback confirm,
        CancelCallback cancel)
        : m_device(std::move(device)),
          m_monitor(monitor),
          m_monitorBounds(monitorBounds),
          m_frozen(std::move(frozen)),
          m_confirm(std::move(confirm)),
          m_cancel(std::move(cancel))
    {
    }

    SelectionOverlay::~SelectionOverlay()
    {
        Close();
    }

    bool SelectionOverlay::RegisterClasses()
    {
        auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW overlayClass{ sizeof(overlayClass) };
        overlayClass.style = CS_HREDRAW | CS_VREDRAW;
        overlayClass.lpfnWndProc = WindowProc;
        overlayClass.hInstance = instance;
        overlayClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        overlayClass.lpszClassName = OverlayClassName();
        if (!RegisterClassExW(&overlayClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW handleClass{ sizeof(handleClass) };
        handleClass.lpfnWndProc = HandleProc;
        handleClass.hInstance = instance;
        handleClass.lpszClassName = HandleClassName();
        if (!RegisterClassExW(&handleClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        WNDCLASSEXW toolbarClass{ sizeof(toolbarClass) };
        toolbarClass.lpfnWndProc = ToolbarProc;
        toolbarClass.hInstance = instance;
        toolbarClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        toolbarClass.lpszClassName = ToolbarClassName();
        if (!RegisterClassExW(&toolbarClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        return true;
    }

    bool SelectionOverlay::Show()
    {
        if (!m_frozen || !RegisterClasses())
        {
            return false;
        }

        auto width = m_monitorBounds.right - m_monitorBounds.left;
        auto height = m_monitorBounds.bottom - m_monitorBounds.top;
        m_window = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            OverlayClassName(),
            Localized(L"区域镜 - 选择区域", L"RegionLens - Select region"),
            WS_POPUP | WS_CLIPCHILDREN,
            m_monitorBounds.left,
            m_monitorBounds.top,
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
        SetWindowDisplayAffinity(m_window, WDA_EXCLUDEFROMCAPTURE);

        m_toolbar = CreateWindowExW(WS_EX_TOOLWINDOW, ToolbarClassName(), L"", WS_CHILD | WS_BORDER | WS_CLIPCHILDREN,
            0, 0, 256, 38, m_window, nullptr, GetModuleHandleW(nullptr), nullptr);
        m_dimension = CreateWindowExW(0, L"STATIC", L"0 × 0", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE | SS_CENTER,
            4, 4, 96, 28, m_toolbar, nullptr, GetModuleHandleW(nullptr), nullptr);
        m_confirmButton = CreateWindowExW(0, L"BUTTON", Localized(L"确定", L"Confirm"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            104, 4, 70, 28, m_toolbar, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ConfirmButtonId)), GetModuleHandleW(nullptr), nullptr);
        m_cancelButton = CreateWindowExW(0, L"BUTTON", Localized(L"取消", L"Cancel"), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            178, 4, 70, 28, m_toolbar, reinterpret_cast<HMENU>(static_cast<INT_PTR>(CancelButtonId)), GetModuleHandleW(nullptr), nullptr);

        for (auto& handle : m_handles)
        {
            handle = CreateWindowExW(WS_EX_TRANSPARENT, HandleClassName(), L"", WS_CHILD,
                0, 0, HandleSize, HandleSize, m_window, nullptr, GetModuleHandleW(nullptr), nullptr);
        }

        if (!m_renderer.Initialize(m_window, m_device))
        {
            Close();
            return false;
        }

        ShowWindow(m_window, SW_SHOW);
        SetForegroundWindow(m_window);
        SetFocus(m_window);
        SetControlsVisible(false);
        RequestVisualUpdate(true);
        return true;
    }

    void SelectionOverlay::Close()
    {
        if (m_window)
        {
            if (m_visualTimerArmed)
            {
                KillTimer(m_window, VisualTimerId);
                m_visualTimerArmed = false;
            }
            auto window = m_window;
            DestroyWindow(window);
            if (m_window == window)
            {
                m_window = nullptr;
            }
        }
    }

    LRESULT CALLBACK SelectionOverlay::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        SelectionOverlay* self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<SelectionOverlay*>(create->lpCreateParams);
            self->m_window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<SelectionOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (self)
        {
            return self->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT CALLBACK SelectionOverlay::HandleProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCHITTEST)
        {
            return HTTRANSPARENT;
        }
        if (message == WM_PAINT)
        {
            PAINTSTRUCT paint{};
            auto dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            auto brush = CreateSolidBrush(RGB(0, 120, 215));
            FillRect(dc, &bounds, brush);
            DeleteObject(brush);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT SelectionOverlay::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        switch (message)
        {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
        {
            PAINTSTRUCT paint{};
            BeginPaint(m_window, &paint);
            EndPaint(m_window, &paint);
            // The flip-model swap chain is retained by DWM. Child-window moves
            // invalidate parts of the parent, but paint validation must not turn
            // those invalidations back into geometry updates and another child
            // move. That feedback loop previously saturated the UI thread after
            // several seconds of dragging.
            return 0;
        }
        case WM_SIZE:
            m_renderer.Resize();
            RequestVisualUpdate(true, true);
            return 0;
        case WM_TIMER:
            if (wParam == VisualTimerId)
            {
                KillTimer(m_window, VisualTimerId);
                m_visualTimerArmed = false;
                ApplyPendingDragPoint();
                FlushVisualUpdate();
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
        {
            SetFocus(m_window);
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            m_dragOrigin = point;
            m_startSelection = m_selection;
            m_hit = HitTest(point);
            SetControlsVisible(false);
            if (m_selection.Empty() || m_hit == HitRegion::NewSelection)
            {
                m_hit = HitRegion::NewSelection;
                m_selection = { point.x, point.y, 0, 0 };
                RequestVisualUpdate(true, false);
            }
            m_dragPointPending = false;
            m_dragging = true;
            SetCapture(m_window);
            return 0;
        }
        case WM_MOUSEMOVE:
        {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            if (m_dragging)
            {
                QueueDragPoint(point);
            }
            else
            {
                SetCursor(CursorFor(HitTest(point)));
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (m_dragging)
            {
                QueueDragPoint({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) }, true);
                m_dragging = false;
                ReleaseCapture();
                m_visualDirty = true;
                m_controlsDirty = true;
                FlushVisualUpdate(true);
                SetControlsVisible(m_selection.width >= MinimumSelection && m_selection.height >= MinimumSelection);
            }
            return 0;
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            if (m_dragging && (message == WM_CANCELMODE || reinterpret_cast<HWND>(lParam) != m_window))
            {
                m_dragging = false;
                m_dragPointPending = false;
                m_visualDirty = true;
                m_controlsDirty = true;
                FlushVisualUpdate(true);
                SetControlsVisible(m_selection.width >= MinimumSelection && m_selection.height >= MinimumSelection);
            }
            return 0;
        case WM_RBUTTONUP:
        case WM_CLOSE:
            if (!m_callbackSent)
            {
                m_callbackSent = true;
                m_cancel();
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                if (!m_callbackSent)
                {
                    m_callbackSent = true;
                    m_cancel();
                }
                return 0;
            }
            if (wParam == VK_RETURN && m_selection.width >= MinimumSelection && m_selection.height >= MinimumSelection)
            {
                if (!m_callbackSent)
                {
                    m_callbackSent = true;
                    m_confirm(m_selection);
                }
                return 0;
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wParam) == ConfirmButtonId && !m_callbackSent)
            {
                m_callbackSent = true;
                m_confirm(m_selection);
                return 0;
            }
            if (LOWORD(wParam) == CancelButtonId && !m_callbackSent)
            {
                m_callbackSent = true;
                m_cancel();
                return 0;
            }
            break;
        case WM_NCDESTROY:
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            m_window = nullptr;
            return 0;
        }
        return DefWindowProcW(m_window, message, wParam, lParam);
    }

    void SelectionOverlay::Render()
    {
        if (m_window && m_frozen.view)
        {
            m_renderer.Render(
                m_frozen.view.Get(),
                { 0, 0, m_frozen.width, m_frozen.height },
                m_frozen.width,
                m_frozen.height,
                m_selection);
        }
    }

    void SelectionOverlay::RequestVisualUpdate(bool immediate, bool controlsChanged)
    {
        if (!m_window) return;
        m_visualDirty = true;
        m_controlsDirty = m_controlsDirty || controlsChanged;
        auto now = GetTickCount64();
        if (immediate || SelectionVisualUpdateDue(m_lastVisualTick, now))
        {
            FlushVisualUpdate();
            return;
        }
        ArmVisualTimer(SelectionVisualUpdateDelay(m_lastVisualTick, now));
    }

    void SelectionOverlay::QueueDragPoint(POINT point, bool immediate)
    {
        m_pendingDragPoint = point;
        m_dragPointPending = true;
        if (immediate)
        {
            ApplyPendingDragPoint();
            return;
        }
        auto now = GetTickCount64();
        if (SelectionVisualUpdateDue(m_lastVisualTick, now))
        {
            ApplyPendingDragPoint();
            FlushVisualUpdate();
        }
        else
        {
            ArmVisualTimer(SelectionVisualUpdateDelay(m_lastVisualTick, now));
        }
    }

    void SelectionOverlay::ApplyPendingDragPoint()
    {
        if (!m_dragPointPending) return;
        m_dragPointPending = false;
        auto previous = m_selection;
        UpdateSelection(m_pendingDragPoint);
        if (!(m_selection == previous)) m_visualDirty = true;
    }

    void SelectionOverlay::FlushVisualUpdate(bool forceControls)
    {
        if (!m_window) return;
        if (m_visualTimerArmed)
        {
            KillTimer(m_window, VisualTimerId);
            m_visualTimerArmed = false;
        }

        auto visualChanged = std::exchange(m_visualDirty, false);
        auto controlsChanged = std::exchange(m_controlsDirty, false);
        if (controlsChanged || forceControls)
        {
            UpdateControls();
        }
        if (visualChanged || forceControls || m_renderer.NeedsPresent())
        {
            Render();
        }
        m_lastVisualTick = GetTickCount64();

        // A nonblocking present can be rejected until DWM consumes the previous
        // frame. Retry only the newest selection instead of replaying history.
        if (m_renderer.NeedsPresent()) ArmVisualTimer(static_cast<UINT>(SelectionVisualIntervalMs));
    }

    void SelectionOverlay::ArmVisualTimer(UINT delay)
    {
        if (!m_window || m_visualTimerArmed) return;
        if (SetTimer(m_window, VisualTimerId, std::max(1u, delay), nullptr))
            m_visualTimerArmed = true;
    }

    SelectionOverlay::HitRegion SelectionOverlay::HitTest(POINT point) const noexcept
    {
        if (m_selection.Empty())
        {
            return HitRegion::NewSelection;
        }
        auto nearLeft = std::abs(point.x - m_selection.x) <= HitMargin;
        auto nearRight = std::abs(point.x - m_selection.Right()) <= HitMargin;
        auto nearTop = std::abs(point.y - m_selection.y) <= HitMargin;
        auto nearBottom = std::abs(point.y - m_selection.Bottom()) <= HitMargin;
        auto withinX = point.x >= m_selection.x - HitMargin && point.x <= m_selection.Right() + HitMargin;
        auto withinY = point.y >= m_selection.y - HitMargin && point.y <= m_selection.Bottom() + HitMargin;
        if (nearLeft && nearTop) return HitRegion::TopLeft;
        if (nearRight && nearTop) return HitRegion::TopRight;
        if (nearLeft && nearBottom) return HitRegion::BottomLeft;
        if (nearRight && nearBottom) return HitRegion::BottomRight;
        if (nearLeft && withinY) return HitRegion::Left;
        if (nearRight && withinY) return HitRegion::Right;
        if (nearTop && withinX) return HitRegion::Top;
        if (nearBottom && withinX) return HitRegion::Bottom;
        if (point.x > m_selection.x && point.x < m_selection.Right() && point.y > m_selection.y && point.y < m_selection.Bottom())
        {
            return HitRegion::Move;
        }
        return HitRegion::NewSelection;
    }

    HCURSOR SelectionOverlay::CursorFor(HitRegion hit) const noexcept
    {
        switch (hit)
        {
        case HitRegion::Left:
        case HitRegion::Right: return LoadCursorW(nullptr, IDC_SIZEWE);
        case HitRegion::Top:
        case HitRegion::Bottom: return LoadCursorW(nullptr, IDC_SIZENS);
        case HitRegion::TopLeft:
        case HitRegion::BottomRight: return LoadCursorW(nullptr, IDC_SIZENWSE);
        case HitRegion::TopRight:
        case HitRegion::BottomLeft: return LoadCursorW(nullptr, IDC_SIZENESW);
        case HitRegion::Move: return LoadCursorW(nullptr, IDC_SIZEALL);
        default: return LoadCursorW(nullptr, IDC_CROSS);
        }
    }

    void SelectionOverlay::UpdateSelection(POINT current)
    {
        auto clientWidth = static_cast<int>(m_monitorBounds.right - m_monitorBounds.left);
        auto clientHeight = static_cast<int>(m_monitorBounds.bottom - m_monitorBounds.top);
        current.x = std::clamp(current.x, 0L, static_cast<LONG>(clientWidth));
        current.y = std::clamp(current.y, 0L, static_cast<LONG>(clientHeight));

        if (m_hit == HitRegion::NewSelection)
        {
            m_selection = ClampRect(NormalizeRect(m_dragOrigin, current), clientWidth, clientHeight);
            return;
        }

        auto left = m_startSelection.x;
        auto top = m_startSelection.y;
        auto right = m_startSelection.Right();
        auto bottom = m_startSelection.Bottom();
        auto dx = static_cast<int>(current.x - m_dragOrigin.x);
        auto dy = static_cast<int>(current.y - m_dragOrigin.y);

        if (m_hit == HitRegion::Move)
        {
            auto newX = std::clamp(m_startSelection.x + dx, 0, clientWidth - m_startSelection.width);
            auto newY = std::clamp(m_startSelection.y + dy, 0, clientHeight - m_startSelection.height);
            m_selection = { newX, newY, m_startSelection.width, m_startSelection.height };
            return;
        }

        if (m_hit == HitRegion::Left || m_hit == HitRegion::TopLeft || m_hit == HitRegion::BottomLeft)
            left = std::clamp(m_startSelection.x + dx, 0, right - MinimumSelection);
        if (m_hit == HitRegion::Right || m_hit == HitRegion::TopRight || m_hit == HitRegion::BottomRight)
            right = std::clamp(m_startSelection.Right() + dx, left + MinimumSelection, clientWidth);
        if (m_hit == HitRegion::Top || m_hit == HitRegion::TopLeft || m_hit == HitRegion::TopRight)
            top = std::clamp(m_startSelection.y + dy, 0, bottom - MinimumSelection);
        if (m_hit == HitRegion::Bottom || m_hit == HitRegion::BottomLeft || m_hit == HitRegion::BottomRight)
            bottom = std::clamp(m_startSelection.Bottom() + dy, top + MinimumSelection, clientHeight);
        m_selection = { left, top, right - left, bottom - top };
    }

    void SelectionOverlay::SetControlsVisible(bool visible)
    {
        ShowWindow(m_toolbar, visible ? SW_SHOWNA : SW_HIDE);
        for (auto handle : m_handles)
        {
            ShowWindow(handle, visible ? SW_SHOWNA : SW_HIDE);
        }
    }

    void SelectionOverlay::UpdateControls()
    {
        if (m_selection.Empty())
        {
            return;
        }

        if (m_lastDimension.cx != m_selection.width || m_lastDimension.cy != m_selection.height)
        {
            std::wstring text = std::to_wstring(m_selection.width) + L" × " + std::to_wstring(m_selection.height);
            SetWindowTextW(m_dimension, text.c_str());
            m_lastDimension = { m_selection.width, m_selection.height };
        }

        auto clientWidth = static_cast<int>(m_monitorBounds.right - m_monitorBounds.left);
        auto clientHeight = static_cast<int>(m_monitorBounds.bottom - m_monitorBounds.top);
        constexpr int toolbarWidth = 256;
        constexpr int toolbarHeight = 38;
        auto toolbarX = std::clamp(m_selection.x, 0, std::max(0, clientWidth - toolbarWidth));
        auto toolbarY = m_selection.Bottom() + 8;
        if (toolbarY + toolbarHeight > clientHeight)
        {
            toolbarY = std::max(0, m_selection.y - toolbarHeight - 8);
        }
        std::array<POINT, 8> points =
        {
            POINT{ m_selection.x, m_selection.y },
            POINT{ m_selection.x + m_selection.width / 2, m_selection.y },
            POINT{ m_selection.Right(), m_selection.y },
            POINT{ m_selection.Right(), m_selection.y + m_selection.height / 2 },
            POINT{ m_selection.Right(), m_selection.Bottom() },
            POINT{ m_selection.x + m_selection.width / 2, m_selection.Bottom() },
            POINT{ m_selection.x, m_selection.Bottom() },
            POINT{ m_selection.x, m_selection.y + m_selection.height / 2 },
        };
        auto positions = BeginDeferWindowPos(static_cast<int>(m_handles.size() + 1));
        if (positions)
        {
            positions = DeferWindowPos(positions, m_toolbar, nullptr, toolbarX, toolbarY,
                toolbarWidth, toolbarHeight, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        for (size_t index = 0; positions && index < m_handles.size(); ++index)
        {
            positions = DeferWindowPos(positions, m_handles[index], nullptr,
                points[index].x - HandleSize / 2, points[index].y - HandleSize / 2,
                HandleSize, HandleSize, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        if (positions) EndDeferWindowPos(positions);
    }
}
