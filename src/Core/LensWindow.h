#pragma once

#include "D3DDevice.h"
#include "RegionTypes.h"
#include "SwapChainRenderer.h"
#include "MouseProxyCore.h"
#include "TopmostPolicy.h"
#include "ScreenshotRenderer.h"
#include "ScreenshotFeedback.h"
#include "PointerSpeedPopup.h"
#include "LensResizePolicy.h"
#include "UiCursorOwnership.h"

namespace RegionLens::native
{
    class LensWindow
    {
    public:
        using CloseCallback = std::function<void(uint64_t)>;
        using MappingToggleCallback = std::function<void(uint64_t)>;
        using MappingGeometryChangedCallback = std::function<void(uint64_t)>;
        using MappingHoverCallback = std::function<void(uint64_t)>;
        using InteractionCallback = std::function<void(uint64_t)>;

        LensWindow(
            LensDescriptor descriptor,
            std::shared_ptr<D3DDevice> device,
            CloseCallback closeCallback,
            MappingToggleCallback mappingToggleCallback,
            MappingGeometryChangedCallback mappingGeometryChangedCallback,
            MappingHoverCallback mappingHoverCallback,
            InteractionCallback interactionCallback, CloseCallback screenshotCallback = {},
            CloseCallback popupChangedCallback = {}, UiCursorOwnership* uiCursor = nullptr);
        ~LensWindow();

        bool Show(bool hidden = false);
        void Hide();
        bool ShowRaisedPreservingTopmost();
        void Close();
        void Render(ID3D11ShaderResourceView* source, int32_t textureWidth, int32_t textureHeight, CaptureStamp stamp = {});
        void SetPresentationRequest(std::function<void()> request) { m_presentationRequest=std::move(request); }
        void RequestPresentation();
        bool ReadyForPresentation();
        void FlushPresentation();
        HRESULT CaptureScreenshot(ScreenshotRenderer& renderer, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging);
        void NotifyScreenshotSuccess();
        void SetQualityLevel(LensSharpness level);
        void SetFullscreenAspectFitEnabled(bool enabled);
        void RefreshLanguage();

        [[nodiscard]] uint64_t Id() const noexcept { return m_descriptor.id; }
        [[nodiscard]] HMONITOR SourceMonitor() const noexcept { return m_descriptor.monitor; }
        [[nodiscard]] HWND Window() const noexcept { return m_window; }
        [[nodiscard]] bool Hidden() const noexcept { return m_hidden; }
        [[nodiscard]] bool IsFullscreen() const noexcept { return m_fullscreen; }
        [[nodiscard]] HWND TopmostHintWindow() const noexcept { return m_fullscreenHint; }
        [[nodiscard]] HWND TopmostTooltipWindow() const noexcept { return m_chromeTooltip; }
        [[nodiscard]] bool KeepsTopmost() const noexcept
        { return !m_hidden && ShouldMaintainLensTopmost(m_descriptor.topmost, m_fullscreen, m_closeRequested); }
        [[nodiscard]] RECT MappingSourceRect() const noexcept;
        [[nodiscard]] RECT MappingDestinationRect() const noexcept;
        [[nodiscard]] bool ShouldSuspendInputMapping(POINT screenPoint) const noexcept;
        void SetInputMappingEnabled(bool enabled);
        void SetUiCursorPaused(bool paused);
        bool SetInputPassThrough(bool enabled);
        bool RefreshNativeCursorForHandoff(POINT expected);
        bool RaiseForInteraction();
        void SetVirtualCursor(CursorSnapshot const& cursor);
        MappingSessionConfig MappingConfig() const;
        bool SpeedPopupOpen() const noexcept { return m_speedPopupOpen; }
        void CloseSpeedPopup(bool resetSpeed = false);

    private:
        friend struct LensSettingsTestAccess;
        friend struct PresentationSchedulerTestAccess;
        friend struct LensFullscreenTransitionTestAccess;
        friend struct UiCursorTestAccess;
        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        bool RegisterClasses();
        void SetChromeVisible(bool visible);
        void UpdateChromeFromCursor();
        void UpdateFullscreenChromeFromClientPoint(POINT clientPoint);
        void UpdateFullscreenChromeFromScreenPoint(POINT screenPoint);
        void UpdateChromeHover(POINT clientPoint);
        void RefreshNativeChromePointer();
        bool SetChromeCursor(POINT clientPoint);
        void ClearChromeCursor(UiCursorRelease reason = UiCursorRelease::PointerLeft);
        void ShowChromeTooltip(int button);
        void HideChromeTooltip();
        void ToggleTopmost();
        void TogglePointerSpeed();
        void ToggleFullscreen();
        void RestoreInitialSize();
        void EnterFullscreen();
        void ExitFullscreen();
        void BeginFullscreenTransition(RECT targetBounds);
        void TryFullscreenTransition();
        void CancelFullscreenTransition(bool redraw, bool notifyMapping);
        bool CommitFullscreenTransition();
        void ShowFullscreenHint();
        void ShowMappingHint();
        bool ShowHint(wchar_t const* text, bool fullscreenPlacement);
        void HideFullscreenHint();
        void BeginMappedMove(bool inputEngineConfirmedGesture);
        void UpdateMappedMove();
        void EndMappedMove(bool publishGeometry);
        void UpdateMappedMoveCursor(POINT screenPoint);
        void RenderLatestFrame();
        void FinishGeometryChange();
        void CheckGeometrySettled();
        [[nodiscard]] RECT ClientRectInScreen() const noexcept;
        [[nodiscard]] RECT ContentRectInClient() const noexcept;
        [[nodiscard]] int ChromeButtonAt(POINT clientPoint, bool requireVisible = true) const noexcept;
        LRESULT HitTestWindow(POINT screenPoint) const noexcept;

        LensDescriptor m_descriptor;
        std::shared_ptr<D3DDevice> m_device;
        CloseCallback m_closeCallback;
        MappingToggleCallback m_mappingToggleCallback;
        MappingGeometryChangedCallback m_mappingGeometryChangedCallback;
        MappingHoverCallback m_mappingHoverCallback;
        InteractionCallback m_interactionCallback;
        CloseCallback m_screenshotCallback;
        CloseCallback m_popupChangedCallback;
        HWND m_window{};
        std::function<void()> m_presentationRequest;
        bool m_presentationDirty{};
        static constexpr UINT PresentationReadyMessage=WM_APP+48;
        SwapChainRenderer m_renderer;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_latestView;
        int32_t m_textureWidth{};
        int32_t m_textureHeight{};
        CaptureStamp m_captureStamp;
        ScreenshotFeedback m_screenshotFeedback;
        LensQualitySettings m_qualitySettings{ NewLensQuality };
        LensSharpness m_qualityRequested{ LensSharpness::Medium };
        PointerSpeed m_pointerSpeed;
        PointerSpeedPopup m_speedPopup;
        bool m_speedPopupOpen{};
        bool m_chromeVisible{};
        bool m_closeRequested{};
        bool m_hidden{};
        bool m_inSizeMove{};
        LensResizePolicy m_resizePolicy;
        bool m_geometryChanging{};
        bool m_mappedMove{};
        bool m_mappedMovePositioning{};
        uint64_t m_lastGeometryChange{};
        bool m_renderFailurePosted{};
        bool m_frameRetryArmed{};
        bool m_fullscreen{};
        bool m_fullscreenAspectFit{};
        FullscreenTransitionGate m_fullscreenTransition;
        RECT m_fullscreenTransitionBounds{};
        bool m_inputMappingEnabled{};
        bool m_inputPassThrough{};
        CursorSnapshot m_virtualCursor;
        CursorSnapshot m_chromeCursor;
        UiCursorOwnership& m_uiCursor;
        uint64_t m_uiCursorEpoch{};
        bool m_uiCursorPaused{};
        HWND m_chromeTooltip{};
        int m_hoverButton{};
        std::wstring m_tooltipText;
        bool m_escapeHotkeyRegistered{};
        bool m_restoreEscapeOnShow{};
        RECT m_restoreBounds{};
        SIZE m_initialSize{};
        POINT m_mappedMoveAnchor{};
        RECT m_mappedMoveBounds{};
        HWND m_fullscreenHint{};
        bool m_mappingHint{};
        bool m_mappingHintShown{};
        bool m_fullscreenHintShown{};
        std::chrono::steady_clock::time_point m_hintDeadline{};
    };
}
