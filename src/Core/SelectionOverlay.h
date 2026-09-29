#pragma once

#include "MonitorCapture.h"
#include "RegionTypes.h"
#include "SwapChainRenderer.h"

namespace RegionLens::native
{
    class SelectionOverlay
    {
    public:
        using ConfirmCallback = std::function<void(PixelRect)>;
        using CancelCallback = std::function<void()>;

        SelectionOverlay(
            std::shared_ptr<D3DDevice> device,
            HMONITOR monitor,
            RECT monitorBounds,
            CapturedTexture frozen,
            ConfirmCallback confirm,
            CancelCallback cancel);
        ~SelectionOverlay();

        bool Show();
        void Close();
        [[nodiscard]] HWND Window() const noexcept { return m_window; }

    private:
        enum class HitRegion
        {
            NewSelection,
            Move,
            Left,
            Right,
            Top,
            Bottom,
            TopLeft,
            TopRight,
            BottomLeft,
            BottomRight,
        };

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        static LRESULT CALLBACK HandleProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        bool RegisterClasses();
        void Render();
        void RequestVisualUpdate(bool immediate = false, bool controlsChanged = true);
        void FlushVisualUpdate(bool forceControls = false);
        void ArmVisualTimer(UINT delay);
        void QueueDragPoint(POINT point, bool immediate = false);
        void ApplyPendingDragPoint();
        void UpdateSelection(POINT current);
        void UpdateControls();
        void SetControlsVisible(bool visible);
        [[nodiscard]] HitRegion HitTest(POINT point) const noexcept;
        [[nodiscard]] HCURSOR CursorFor(HitRegion hit) const noexcept;

        std::shared_ptr<D3DDevice> m_device;
        HMONITOR m_monitor{};
        RECT m_monitorBounds{};
        CapturedTexture m_frozen;
        ConfirmCallback m_confirm;
        CancelCallback m_cancel;
        HWND m_window{};
        HWND m_toolbar{};
        HWND m_dimension{};
        HWND m_confirmButton{};
        HWND m_cancelButton{};
        std::array<HWND, 8> m_handles{};
        SwapChainRenderer m_renderer;
        PixelRect m_selection{};
        PixelRect m_startSelection{};
        POINT m_dragOrigin{};
        POINT m_pendingDragPoint{};
        HitRegion m_hit{ HitRegion::NewSelection };
        bool m_dragging{};
        bool m_callbackSent{};
        bool m_visualDirty{};
        bool m_controlsDirty{};
        bool m_visualTimerArmed{};
        bool m_dragPointPending{};
        uint64_t m_lastVisualTick{};
        SIZE m_lastDimension{ -1, -1 };
    };
}
