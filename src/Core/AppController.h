#pragma once

#include "D3DDevice.h"
#include "LensManager.h"
#include "Localization.h"
#include "MonitorCapture.h"
#include "SelectionOverlay.h"
#include "HotkeySettings.h"
#include "DeferredWindowActions.h"
#include "ScreenshotService.h"
#include "SettingsWindow.h"
#include "TopmostFastWake.h"
#include <shobjidl.h>

namespace RegionLens::native
{
    inline constexpr UINT ShowRunningNotificationMessage = WM_APP + 10;
    inline constexpr UINT TopmostFastWakeMessage = WM_APP + 15;

    class AppController
    {
    public:
        AppController() = default;
        ~AppController();

        bool Initialize(bool quietStartup = false);
        bool ProcessMessage(MSG& message);

    private:
        friend struct SettingsControllerTestAccess;
        static constexpr UINT FrameReadyMessage = WM_APP + 1;
        static constexpr UINT SelectionConfirmedMessage = WM_APP + 2;
        static constexpr UINT SelectionCancelledMessage = WM_APP + 3;
        static constexpr UINT CloseLensMessage = WM_APP + 4;
        static constexpr UINT TrayMessage = WM_APP + 5;
        static constexpr UINT ScreenshotMessage = WM_APP + 13;
        static constexpr UINT PresentMessage = WM_APP + 14;
        static constexpr UINT_PTR ScreenshotTimerId = 6;
        static constexpr UINT_PTR TopmostTimerId = 5;

        static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
        bool CreateControllerWindow();
        bool AddTrayIcon();
        void RemoveTrayIcon();
        void ShowTrayMenu();
        void RefreshTopmost(bool eventWake = false);
        void Notify(std::wstring const& title, std::wstring const& message, DWORD flags = NIIF_INFO);
        void NotifyRunning();
        void StartNewSelection();
        void BeginSelection(MonitorCapture& capture);
        MonitorCapture* EnsureCapture(HMONITOR monitor);
        enum class CaptureReleaseReason : int64_t
        {
            SelectionEnded = 1,
            LensClosed,
            CloseAll,
            DisplayChanged,
            Shutdown
        };
        void ReleaseUnusedCapture(HMONITOR monitor, CaptureReleaseReason reason);
        void ReleaseUnusedCaptures(CaptureReleaseReason reason);
        void ConfirmSelection();
        void CancelSelection();
        void ResumeCaptureProcessing();
        HotkeyRegistrationResult RegisterConfiguredHotkeys(HotkeySettings const& settings);
        HotkeyRegistrationResult ProbeHotkeyAvailability(HotkeySettings const& settings);
        void ReleaseConfiguredHotkeys();
        void ShowSettings(bool visible = true);
        SettingsApplyResult ApplySettings(AppSettings const& settings);
        void TakeScreenshot(uint64_t lensId);
        void PumpScreenshots();
        void RequestPresentation();
        void PresentLatest();
        void ShowShortcutProblems(HotkeyRegistrationResult const& result);
        void CloseAll(CaptureReleaseReason reason = CaptureReleaseReason::CloseAll);
        void SetRegionsHidden(bool hidden);
        void Shutdown();

        HWND m_window{};
        NOTIFYICONDATAW m_tray{};
        bool m_trayAdded{};
        std::array<bool, HotkeyActionCount> m_registeredHotkeys{};
        std::array<HotkeyBinding, HotkeyActionCount> m_registeredBindings{};
        bool m_visibilityAction{};
        HotkeySettings m_hotkeys{};
        HotkeyRegistrationResult m_hotkeyRegistration{};
        bool m_shuttingDown{};
        bool m_presentPosted{},m_presenting{};
        bool m_deferredExit{};
        DeferredWindowActions m_deferredInputActions;
        bool m_modalUi{};
        AppSettings m_settings;
        std::shared_ptr<SettingsWindow> m_settingsWindow;
        bool m_settingsOpening{};
        bool m_settingsRecording{}, m_settingsApplying{};
        std::wstring m_screenshotDirectory;
        ScreenshotRenderer m_screenshotRenderer;
        ScreenshotService m_screenshots;
        uint64_t m_screenshotLensId{};
        std::shared_ptr<D3DDevice> m_device;
        std::shared_ptr<InputMappingCoordinator> m_inputMapping;
        std::unique_ptr<LensManager> m_lensManager;
        std::unique_ptr<TopmostFastWake> m_topmostFastWake;
        bool m_topmostFastWakeUnavailable{};
        bool m_weTypeCompatibilityEnabled{};
        uint64_t m_nextWeTypeAttachMs{};
        std::unordered_map<HMONITOR, std::shared_ptr<MonitorCapture>> m_captures;
        std::unique_ptr<SelectionOverlay> m_selectionOverlay;
        HMONITOR m_pendingSelectionMonitor{};
        PixelRect m_pendingSelectionRect{};
    };
}
