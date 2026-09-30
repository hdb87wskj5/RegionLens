#include "pch.h"
#include "AppController.h"
#include "CaptureDemandPolicy.h"
#include "LatencyMetrics.h"
#include "MouseProxyEngine.h"
#include "TopmostFastWake.h"
#include "resource.h"
#include <shlobj.h>

namespace
{
    constexpr int NewRegionHotkeyId = 1;
    constexpr int CloseAllHotkeyId = 3;
    constexpr int HideAllHotkeyId = 5;
    constexpr int ShowAllHotkeyId = 6;
    constexpr std::array<int, RegionLens::native::HotkeyActionCount> HotkeyIds{
        NewRegionHotkeyId, CloseAllHotkeyId, HideAllHotkeyId, ShowAllHotkeyId };
    constexpr UINT TrayIconId = 1;
    constexpr UINT CommandNew = 3001;
    constexpr UINT CommandCloseAll = 3002;
    constexpr UINT CommandSettings = 3003;
    constexpr UINT CommandExit = 3004;
    constexpr UINT CommandDeviceLostDisableMapping = 3006;
    constexpr UINT CommandDiagnostics = 3010;
    constexpr UINT CommandProbe = 3011;
    constexpr UINT CommandHideAll = 3015;
    constexpr UINT CommandShowAll = 3016;
}

namespace RegionLens::native
{
    AppController::~AppController()
    {
        Shutdown();
    }

    bool AppController::ProcessMessage(MSG& message)
    {
        // Keep the sheet alive while native callbacks may close it or shut
        // down the controller. Only the sheet's own messages are consumed.
        auto dialog = m_settingsWindow;
        if (!dialog) return false;
        auto consumed = dialog->ProcessMessage(message);
        if (dialog->Finished() && m_settingsWindow == dialog) m_settingsWindow.reset();
        return consumed;
    }

    bool AppController::Initialize(bool quietStartup)
    {
        m_device = std::make_shared<D3DDevice>();
        if (!m_device->Initialize() || !CreateControllerWindow())
        {
            return false;
        }

        m_settings = LoadAppSettings(*Runtime().identity);
        m_weTypeCompatibilityEnabled = Runtime().weTypeProbe != nullptr;
        SetAppLanguage(m_settings.language);
        m_hotkeys = m_settings.hotkeys;
        m_screenshotDirectory = ResolveScreenshotDirectory(m_settings, *Runtime().identity);
        m_screenshotRenderer.Initialize(m_device->Device()); // Compile outside the first screenshot's input path.
        m_inputMapping = std::make_shared<InputMappingCoordinator>(
            m_window,
            [this](std::wstring const& title, std::wstring const& message, DWORD flags)
            {
                Notify(title, message, flags);
            });
        m_lensManager = std::make_unique<LensManager>(m_device, m_inputMapping, [this](uint64_t id)
        {
            PostMessageW(m_window, CloseLensMessage, static_cast<WPARAM>(id), 0);
        }, [this](uint64_t id) { PostMessageW(m_window, ScreenshotMessage, static_cast<WPARAM>(id), 0); });
        m_lensManager->SetQualityLevel(m_settings.quality);
        m_lensManager->SetPresentationRequest([this] {RequestPresentation();});
        m_lensManager->SetFullscreenAspectFitEnabled(m_settings.fullscreenAspectFit);
        m_lensManager->SetNewWindowTopmost(m_settings.newWindowTopmost);

        m_hotkeyRegistration = RegisterConfiguredHotkeys(m_hotkeys);
        if (!AddTrayIcon())
        {
            return false;
        }
        m_inputMapping->Initialize();
        if (!m_lensManager->StartTopmostEvents(m_window))
        {
            Notify(Localized(L"置顶事件监听不完整", L"Always-on-top event monitoring is incomplete"),
                Localized(L"部分窗口事件无法监听，将继续使用定时检查补偿。",
                    L"Some window events are unavailable. Timer checks will compensate."), NIIF_WARNING);
        }
        if (!SetTimer(m_window, TopmostTimerId, TopmostRateLimit::TimerIntervalMs, nullptr))
        {
            Notify(Localized(L"置顶维护定时器不可用", L"Always-on-top timer unavailable"),
                Localized(L"实时画面仍可使用；静止画面上的增强置顶可能无法及时更新，请重启后重试。",
                    L"Live regions remain usable, but enhanced topmost ordering may update late on static content. Restart and retry."), NIIF_WARNING);
        }
        if (!quietStartup) NotifyRunning();
        ShowShortcutProblems(m_hotkeyRegistration);
        return true;
    }

    bool AppController::CreateControllerWindow()
    {
        WNDCLASSEXW windowClass{ sizeof(windowClass) };
        windowClass.lpfnWndProc = WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.hIcon = LoadIconW(windowClass.hInstance, MAKEINTRESOURCEW(IDI_REGIONLENS));
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.lpszClassName = Runtime().identity->controllerClass;
        if (!RegisterClassExW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        m_window = CreateWindowExW(WS_EX_TOOLWINDOW, Runtime().identity->controllerClass,
            Localized(L"区域镜", L"RegionLens"), WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        return m_window != nullptr;
    }

    bool AppController::AddTrayIcon()
    {
        m_tray = {};
        m_tray.cbSize = sizeof(m_tray);
        m_tray.hWnd = m_window;
        m_tray.uID = TrayIconId;
        m_tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        m_tray.uCallbackMessage = TrayMessage;
        m_tray.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_REGIONLENS));
        wcscpy_s(m_tray.szTip, Runtime().identity->name);
        m_trayAdded = Shell_NotifyIconW(NIM_ADD, &m_tray) != FALSE;
        if (m_trayAdded)
        {
            m_tray.uVersion = NOTIFYICON_VERSION_4;
            Shell_NotifyIconW(NIM_SETVERSION, &m_tray);
        }
        return m_trayAdded;
    }

    void AppController::RemoveTrayIcon()
    {
        if (m_trayAdded)
        {
            Shell_NotifyIconW(NIM_DELETE, &m_tray);
            m_trayAdded = false;
        }
    }

    void AppController::Notify(std::wstring const& title, std::wstring const& message, DWORD flags)
    {
        Record(DiagnosticEvent::Notification, { flags }, (flags & NIIF_ICON_MASK) == NIIF_ERROR || (flags & NIIF_ICON_MASK) == NIIF_WARNING);
        if (!m_trayAdded)
        {
            return;
        }
        m_tray.uFlags = NIF_INFO;
        m_tray.dwInfoFlags = flags;
        wcsncpy_s(m_tray.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(m_tray.szInfo, message.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &m_tray);
    }

    void AppController::NotifyRunning()
    {
        auto binding = m_hotkeys[HotkeyAction::NewRegion];
        auto const& registration = m_hotkeyRegistration.items[static_cast<size_t>(HotkeyAction::NewRegion)];
        if (binding.Enabled() && registration.succeeded)
        {
            auto shortcut = FormatHotkey(binding);
            auto message = UsesEnglish() ?
                L"Press " + shortcut + L" to freeze the monitor under the pointer and select a region." :
                L"按 " + shortcut + L" 冻结鼠标所在屏幕并框选区域。";
            Notify(Localized(L"区域镜已启动", L"RegionLens is running"), message, NIIF_INFO);
        }
        else
        {
            Notify(Localized(L"区域镜已启动", L"RegionLens is running"),
                Localized(L"可从托盘菜单新建实时区域或设置快捷键。",
                    L"Use the tray menu to create a live region or configure shortcuts."), NIIF_INFO);
        }
    }

    LRESULT CALLBACK AppController::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        AppController* self = nullptr;
        if (message == WM_NCCREATE)
        {
            auto create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<AppController*>(create->lpCreateParams);
            self->m_window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        else
        {
            self = reinterpret_cast<AppController*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        if (self)
        {
            return self->HandleMessage(message, wParam, lParam);
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT AppController::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (m_inputMapping && (m_inputMapping->LifecycleBusy() || m_visibilityAction))
        {
            if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wParam) == CommandExit))
            {
                m_deferredExit = true;
                m_inputMapping->Shutdown();
                return 0; // The outer LensWindow/Route stack must unwind before destroying its owner.
            }
            bool mutating = message == CloseLensMessage || message == ScreenshotMessage || message == WM_DISPLAYCHANGE ||
                message == SelectionConfirmedMessage || message == SelectionCancelledMessage ||
                (message == WM_HOTKEY && (wParam == NewRegionHotkeyId || wParam == CloseAllHotkeyId ||
                    wParam == HideAllHotkeyId || wParam == ShowAllHotkeyId)) ||
                (message == WM_COMMAND && (LOWORD(wParam) == CommandNew || LOWORD(wParam) == CommandCloseAll ||
                    LOWORD(wParam) == CommandSettings ||
                    LOWORD(wParam) == CommandHideAll || LOWORD(wParam) == CommandShowAll)) ||
                (message == TrayMessage && LOWORD(lParam) == WM_LBUTTONDBLCLK);
            if (mutating)
            {
                if (!m_deferredInputActions.Push(message, wParam, lParam))
                { m_deferredInputActions.Clear(); m_deferredInputActions.Push(WM_COMMAND, CommandCloseAll, 0); }
                // Keep the current route/standby consistent until the queued
                // action runs. Exit instead latches a stop immediately;
                // neither path destroys an executing UI call.
                if (!m_visibilityAction) m_inputMapping->NotifyWhenIdle();
                return 0;
            }
        }
        switch (message)
        {
        case InputMappingCoordinator::LifecycleIdleMessage:
        {
            if (m_visibilityAction) return 0; // The outer visibility batch posts the next wake.
            if (m_inputMapping && m_inputMapping->LifecycleBusy())
            {
                m_inputMapping->NotifyWhenIdle();
                return 0; // Preserve the wake without reposting into a nested message loop.
            }
            if (m_deferredExit)
            {
                m_deferredExit = false; m_deferredInputActions.Clear();
                Shutdown(); PostQuitMessage(0); return 0;
            }
            DeferredWindowActions::Action action;
            if (m_deferredInputActions.Pop(action))
            {
                HandleMessage(action.message, action.wParam, action.lParam);
                if (m_window && !m_deferredInputActions.Empty())
                    PostMessageW(m_window, InputMappingCoordinator::LifecycleIdleMessage, 0, 0);
            }
            return 0;
        }
        case ShowRunningNotificationMessage:
            NotifyRunning();
            return 0;
        case MouseProxyEngine::UiMessage:
            if (m_lensManager) m_lensManager->RefreshInputMappingFromCursor();
            return 0;
        case TopmostGuard::WakeMessage:
            RefreshTopmost(true);
            return 0;
        case TopmostFastWakeMessage:
            if (m_topmostFastWake) m_topmostFastWake->Acknowledge();
            RefreshTopmost();
            return 0;
        case WM_CLOSE:
            Shutdown();
            PostQuitMessage(0);
            return 0;
        case WM_HOTKEY:
            if (m_settingsRecording) return 0;
            if (wParam == NewRegionHotkeyId)
            {
                StartNewSelection();
            }
            else if (wParam == CloseAllHotkeyId)
            {
                CloseAll();
            }
            else if (wParam == HideAllHotkeyId) SetRegionsHidden(true);
            else if (wParam == ShowAllHotkeyId) SetRegionsHidden(false);
            return 0;
        case WM_TIMER:
            if (wParam == ScreenshotTimerId) { PumpScreenshots(); return 0; }
            if (wParam == TopmostTimerId)
            {
                RefreshTopmost();
                return 0;
            }
            if (wParam == InputMappingCoordinator::RefreshTimerId && m_lensManager)
            {
                m_lensManager->RefreshInputMappingFromCursor();
                return 0;
            }
            break;
        case PresentMessage:
            m_presentPosted=false;PresentLatest();return 0;
        case FrameReadyMessage:
        {
            PumpScreenshots(); // Keep readback/clipboard completion progressing under a continuous frame stream.
            // Service input acknowledgements and standby routing here as a
            // fallback when continuous frames delay low-priority timers.
            if (m_lensManager) m_lensManager->RefreshInputMappingFromCursor();
            // Frame-driven fallback avoids low-priority timer starvation. The
            // guard shares poll/event budgets across all monitors and wake paths.
            RefreshTopmost();
            if (m_shuttingDown || !m_lensManager) return 0;
            auto capture = reinterpret_cast<MonitorCapture*>(wParam);
            bool known = false;
            for (auto const& [monitor, entry] : m_captures)
            {
                if (entry.get() == capture)
                {
                    known = true;
                    break;
                }
            }
            // The overlay renders a frozen texture. Leave one pending frame in
            // each capture while it is visible so the producer keeps replacing
            // that single slot without flooding the UI thread with full-screen
            // copies and live-region renders behind the selection surface.
            if (known && !m_selectionOverlay && capture->HasPendingFrame())
            {
                m_lensManager->MarkMonitorDirty(capture->Monitor());
                if (m_pendingSelectionMonitor == capture->Monitor() && capture->ProcessPendingFrame())
                {
                    BeginSelection(*capture);
                }
                RequestPresentation();
            }
            return 0;
        }
        case SelectionConfirmedMessage:
            ConfirmSelection();
            return 0;
        case SelectionCancelledMessage:
            CancelSelection();
            return 0;
        case CloseLensMessage:
        {
            auto id = static_cast<uint64_t>(wParam);
            auto monitor = m_lensManager->MonitorForLens(id);
            m_lensManager->Remove(id);
            ReleaseUnusedCapture(monitor, CaptureReleaseReason::LensClosed);
            return 0;
        }
        case ScreenshotMessage:
            TakeScreenshot(static_cast<uint64_t>(wParam));
            return 0;
        case WM_DISPLAYCHANGE:
            CancelSelection();
            CloseAll(CaptureReleaseReason::DisplayChanged);
            Notify(Localized(L"显示配置已更改", L"Display configuration changed"),
                Localized(L"已有区域已关闭，请重新框选。", L"Existing regions were closed. Please select them again."), NIIF_WARNING);
            return 0;
        case TrayMessage:
        {
            auto event = LOWORD(lParam);
            if (event == WM_CONTEXTMENU || event == WM_RBUTTONUP)
            {
                ShowTrayMenu();
            }
            else if (event == WM_LBUTTONDBLCLK)
            {
                StartNewSelection();
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam))
            {
            case CommandNew: StartNewSelection(); break;
            case CommandCloseAll: CloseAll(); break;
            case CommandHideAll: SetRegionsHidden(true); break;
            case CommandShowAll: SetRegionsHidden(false); break;
            case CommandDeviceLostDisableMapping:
                if (m_lensManager) m_lensManager->DisableAllInputMappings();
                break;

            case CommandSettings: ShowSettings(); break;
            case CommandDiagnostics:
                if (auto sink = Runtime().diagnostics) {
                    auto directory = sink->Directory();
                    if (!directory.empty()) ShellExecuteW(m_window, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                }
                return 0;
            case CommandProbe:
                if (Runtime().launchProbe) Runtime().launchProbe(m_window);
                return 0;
            case CommandExit:
                Shutdown();
                PostQuitMessage(0);
                break;
            }
            return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(m_window, GWLP_USERDATA, 0);
            m_window = nullptr;
            return 0;
        }
        return DefWindowProcW(m_window, message, wParam, lParam);
    }

    void AppController::RefreshTopmost(bool eventWake)
    {
        if (auto probe = Runtime().weTypeProbe)
        {
            probe->Pump();
            auto reason = probe->TakeStopReason();
            if (reason == WeTypeProbeStopReason::InterceptFailed ||
                reason == WeTypeProbeStopReason::EventOverflow ||
                reason == WeTypeProbeStopReason::HeartbeatExpired ||
                reason == WeTypeProbeStopReason::AttachFailed)
            {
                m_weTypeCompatibilityEnabled = false;
                Record(DiagnosticEvent::WindowLayer, { 11, 8, int64_t(reason) }, true);
            }
        }
        if (m_lensManager)
            m_lensManager->RefreshTopmost(m_shuttingDown || m_modalUi || m_settingsApplying || m_selectionOverlay || m_pendingSelectionMonitor, eventWake);
        if (auto probe = Runtime().weTypeProbe; probe && m_lensManager)
        {
            auto now = GetTickCount64();
            if (ShouldAttachWeTypeCompatibility(m_weTypeCompatibilityEnabled,
                probe->Enabled(), probe->HasTargets(),
                m_shuttingDown || m_modalUi || m_settingsApplying ||
                    m_selectionOverlay || m_pendingSelectionMonitor,
                m_lensManager->WeTypeDemotionIdle(), now, m_nextWeTypeAttachMs))
            {
                // Missing/recreated candidates are normal. Bound the signed-
                // image check while attaching within about one second.
                m_nextWeTypeAttachMs = now + 1000;
                if (probe->Start(WeTypeProbeMode::Intercept)) m_nextWeTypeAttachMs = 0;
                else
                {
                    auto error = GetLastError();
                    if (FatalWeTypeAttachError(error))
                    {
                        m_weTypeCompatibilityEnabled = false;
                        Record(DiagnosticEvent::WindowLayer, { 11, 7, int64_t(error) }, true);
                    }
                }
            }
        }
        if (m_lensManager && !m_shuttingDown)
        {
            auto reason = m_lensManager->TakeWeTypeDemotionStopReason();
            if (reason != WeTypeTrialStopReason::None)
            {
                auto message = reason == WeTypeTrialStopReason::Reasserted ?
                    Localized(L"微信输入法重新置顶，实验已停止；这说明单次降层不能稳定解决闪烁。",
                        L"WeType reasserted topmost. The trial stopped; one-time demotion is not stable.") :
                    reason == WeTypeTrialStopReason::Timeout ?
                    Localized(L"30 秒实验结束，候选窗层级已尝试恢复。",
                        L"The 30-second trial ended; candidate-window order restoration was requested.") :
                    Localized(L"候选窗层级操作失败，实验已停止。必要时可重启微信输入法恢复其窗口。",
                        L"Candidate-window positioning failed. Restart WeType if its window order remains changed.");
                Notify(Localized(L"微信输入法层级实验", L"WeType layer experiment"), message,
                    reason == WeTypeTrialStopReason::Timeout ? NIIF_INFO : NIIF_WARNING);
            }
        }
        bool fast = !m_shuttingDown && m_lensManager && m_lensManager->TopmostFastWakeNeeded();
        if (fast && !m_topmostFastWake && !m_topmostFastWakeUnavailable)
            m_topmostFastWake = std::make_unique<TopmostFastWake>(m_window, TopmostFastWakeMessage);
        if (!m_topmostFastWake) return;
        bool wasEnabled = m_topmostFastWake->Enabled();
        if (!m_topmostFastWake->SetEnabled(fast) || m_topmostFastWake->Error())
        {
            Record(DiagnosticEvent::WindowLayer, { 5, -1, m_topmostFastWake->Error() }, true);
            m_topmostFastWakeUnavailable = true;
            m_topmostFastWake.reset();
            return; // The existing event/frame/16-ms timer path remains available.
        }
        if (wasEnabled != fast)
            Record(DiagnosticEvent::WindowLayer, { 5, fast ? 1 : 0 });
    }

    void AppController::ShowTrayMenu()
    {
        // TrackPopupMenu owns a nested message loop for an unbounded period.
        // Do not leave the target-thread heartbeat dependent on that loop.
        if (auto probe = Runtime().weTypeProbe)
            probe->Stop(WeTypeProbeStopReason::SettingsPause);
        auto previousModal = m_modalUi;
        m_modalUi = true;
        POINT point{};
        GetCursorPos(&point);
        auto menu = CreatePopupMenu();

        auto commandText = [this](HotkeyAction action)
        {
            auto index = static_cast<size_t>(action);
            std::wstring text = HotkeyActionName(action);
            text += L"\t";
            text += FormatHotkey(m_hotkeys[action]);
            auto const& status = m_hotkeyRegistration.items[index];
            if (status.requested && !status.succeeded)
                text += Localized(L"（不可用）", L" (unavailable)");
            return text;
        };
        auto newText = commandText(HotkeyAction::NewRegion);
        auto closeText = commandText(HotkeyAction::CloseAll);
        auto hideText = commandText(HotkeyAction::HideAll);
        auto showText = commandText(HotkeyAction::ShowAllTopmost);
        AppendMenuW(menu, MF_STRING, CommandNew, newText.c_str());
        AppendMenuW(menu, MF_STRING, CommandCloseAll, closeText.c_str());
        AppendMenuW(menu, MF_STRING, CommandHideAll, hideText.c_str());
        AppendMenuW(menu, MF_STRING, CommandShowAll, showText.c_str());
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

        AppendMenuW(menu, MF_STRING, CommandSettings, Localized(L"设置…", L"Settings…"));
        if (Runtime().diagnostics) AppendMenuW(menu, MF_STRING, CommandDiagnostics, Localized(L"打开诊断目录", L"Open diagnostics folder"));
        if (Runtime().launchProbe) AppendMenuW(menu, MF_STRING, CommandProbe, Runtime().probeLabel);
        AppendMenuW(menu, MF_STRING, CommandExit, Localized(L"退出", L"Exit"));
        SetForegroundWindow(m_window);
        auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
            point.x, point.y, 0, m_window, nullptr);
        DestroyMenu(menu);
        m_modalUi = previousModal;
        if (command)
        {
            PostMessageW(m_window, WM_COMMAND, command, 0);
        }
    }



    HotkeyRegistrationResult AppController::RegisterConfiguredHotkeys(HotkeySettings const& settings)
    {
        HotkeyRegistrationResult result;
        auto registerOne = [this, &settings, &result](HotkeyAction action, int id)
        {
            auto index = static_cast<size_t>(action);
            auto binding = settings[action];
            auto& item = result.items[index];
            item.requested = binding.Enabled();
            if (m_registeredHotkeys[index] && m_registeredBindings[index] == binding) return;
            if (m_registeredHotkeys[index]) {
                UnregisterHotKey(m_window, id);
                m_registeredHotkeys[index] = false;
                m_registeredBindings[index] = {};
            }
            if (!item.requested) return;
            if (!IsSupportedHotkey(binding))
            {
                item.succeeded = false;
                item.error = ERROR_INVALID_PARAMETER;
                return;
            }

            SetLastError(ERROR_SUCCESS);
            if (!RegisterHotKey(m_window, id, binding.modifiers | MOD_NOREPEAT, binding.virtualKey))
            {
                item.succeeded = false;
                item.error = GetLastError();
                if (!item.error) item.error = ERROR_GEN_FAILURE;
                return;
            }

            m_registeredHotkeys[index] = true;
            m_registeredBindings[index] = binding;
        };

        for (size_t i = 0; i < HotkeyActionCount; ++i)
            registerOne(static_cast<HotkeyAction>(i), HotkeyIds[i]);
        return result;
    }

    HotkeyRegistrationResult AppController::ProbeHotkeyAvailability(HotkeySettings const& settings)
    {
        // Reacquire previously unavailable applied bindings if their owner
        // has exited. Probing a draft must never release a working shortcut.
        if (!m_settingsRecording && !m_settingsApplying)
            m_hotkeyRegistration = RegisterConfiguredHotkeys(m_hotkeys);
        return ProbeHotkeysPreservingRegistrations(settings, m_registeredBindings, m_registeredHotkeys,
            [this](size_t index, HotkeyBinding binding) {
                constexpr int ProbeBase = 100;
                SetLastError(ERROR_SUCCESS);
                if (RegisterHotKey(m_window, ProbeBase + int(index), binding.modifiers | MOD_NOREPEAT, binding.virtualKey)) {
                    UnregisterHotKey(m_window, ProbeBase + int(index));
                    return DWORD(ERROR_SUCCESS);
                }
                auto error = GetLastError();
                return error ? error : DWORD(ERROR_GEN_FAILURE);
            });
    }

    void AppController::ReleaseConfiguredHotkeys()
    {
        if (!m_window) return;
        for (size_t i = 0; i < HotkeyActionCount; ++i)
        {
            if (m_registeredHotkeys[i]) UnregisterHotKey(m_window, HotkeyIds[i]);
            m_registeredHotkeys[i] = false;
            m_registeredBindings[i] = {};
        }
    }

    void AppController::ShowShortcutProblems(HotkeyRegistrationResult const& result)
    {
        bool any{}, conflict{};
        std::wstring message = Localized(
            L"以下全局快捷键无法注册：\n\n",
            L"The following global shortcuts could not be registered:\n\n");
        for (size_t i = 0; i < HotkeyActionCount; ++i)
        {
            auto const& item = result.items[i];
            if (!item.requested || item.succeeded) continue;
            any = true;
            conflict = conflict || item.error == ERROR_HOTKEY_ALREADY_REGISTERED;
            auto action = static_cast<HotkeyAction>(i);
            message += L"• ";
            message += HotkeyActionName(action);
            message += Localized(L"：", L": ");
            message += FormatHotkey(m_hotkeys[action]);
            if (item.error == ERROR_HOTKEY_ALREADY_REGISTERED)
                message += Localized(L"（已被其他程序占用）", L" (already in use)");
            else
                message += Localized(L"（错误 ", L" (error ") + std::to_wstring(item.error) +
                    Localized(L"）", L")");
            message += L"\n";
        }
        if (!any) return;
        for (auto const& item : result.items) if (item.requested && !item.succeeded)
            Record(DiagnosticEvent::StartupFailure, { item.error }, true);
        message += Localized(
            L"\n程序仍可通过托盘菜单操作。请在“设置 → 快捷键”中修改。",
            L"\nThe tray commands remain available. Change bindings in Settings → Shortcuts.");
        auto previousModal = m_modalUi;
        m_modalUi = true;
        MessageBoxW(nullptr, message.c_str(),
            conflict ? Localized(L"快捷键冲突", L"Shortcut conflict") :
                Localized(L"快捷键注册失败", L"Shortcut registration failed"),
            MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        m_modalUi = previousModal;
    }

    void AppController::TakeScreenshot(uint64_t lensId)
    {
        if (!m_lensManager || m_shuttingDown) return;
        if (m_screenshots.Busy()) {
            Notify(Localized(L"正在保存截图", L"Screenshot in progress"),
                Localized(L"请等待上一张截图完成后再试。", L"Please wait for the previous screenshot to finish."));
            return;
        }
        HRESULT hr = E_FAIL;
        try {
            // Arm completion pumping before taking ownership of the GPU snapshot.
            if (!SetTimer(m_window, ScreenshotTimerId, 20, nullptr)) hr = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
            else {
                Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
                auto monitor=m_lensManager->MonitorForLens(lensId);
                auto found=m_captures.find(monitor);
                if(found!=m_captures.end() && !m_selectionOverlay && found->second->ProcessPendingFrame()) {
                    auto& capture=*found->second;
                    m_lensManager->RenderMonitor(monitor,capture.LatestView(),capture.Width(),capture.Height(),capture.Stamp());
                }
                hr = m_lensManager->CaptureScreenshot(lensId, m_screenshotRenderer, staging);
                if (SUCCEEDED(hr)) hr = m_screenshots.Start(std::move(staging), m_screenshotDirectory);
                if (SUCCEEDED(hr)) m_screenshotLensId = lensId;
            }
        } catch (...) { hr = E_OUTOFMEMORY; }
        Record(DiagnosticEvent::Screenshot, { 0, hr }, FAILED(hr), 0, lensId);
        if (FAILED(hr)) {
            KillTimer(m_window, ScreenshotTimerId);
            if (FAILED(m_device->Device()->GetDeviceRemovedReason()))
                PostMessageW(m_window, WM_COMMAND, CommandDeviceLostDisableMapping, 0);
            Notify(Localized(L"截图未完成", L"Screenshot failed"),
                Localized(L"画面尚未就绪、尺寸正在变化或资源不可用。错误：", L"The frame is not ready, is resizing, or resources are unavailable. Error: ") +
                    std::to_wstring(static_cast<uint32_t>(hr)), NIIF_WARNING);
        }
    }

    void AppController::PumpScreenshots()
    {
        if (!m_device || !m_window || m_shuttingDown) return;
        auto completed = m_screenshots.Poll(m_device->Context(), m_window);
        if (!completed) return;
        KillTimer(m_window, ScreenshotTimerId);
        auto lensId = std::exchange(m_screenshotLensId, uint64_t{});
        bool copied = SUCCEEDED(completed->clipboard), saved = SUCCEEDED(completed->file);
        // Confirm only the originating region after both destinations succeed.
        // It may have closed or been hidden while PNG/clipboard work completed.
        if (copied && saved && m_lensManager) m_lensManager->NotifyScreenshotSuccess(lensId);
        if (FAILED(m_device->Device()->GetDeviceRemovedReason()))
            PostMessageW(m_window, WM_COMMAND, CommandDeviceLostDisableMapping, 0);
        Record(DiagnosticEvent::Screenshot, { 1, completed->clipboard, completed->file }, !copied || !saved, 0, lensId);
        // Successful screenshots use only the brief local button feedback.
        // Keep tray warnings for partial/total failure and the diagnostic result.
        if (copied && saved) return;
        auto title = copied ? Localized(L"截图已复制，保存失败", L"Copied, but saving failed") :
            saved ? Localized(L"截图已保存，复制失败", L"Saved, but copying failed") :
            Localized(L"截图失败", L"Screenshot failed");
        std::wstring detail;
        if (!copied) detail += Localized(L"剪贴板错误：", L"Clipboard error: ") + std::to_wstring(uint32_t(completed->clipboard)) + L". ";
        if (!saved) detail += Localized(L"文件错误：", L"File error: ") + std::to_wstring(uint32_t(completed->file)) + L". ";
        detail += Localized(L"请检查剪贴板占用、保存位置及写入权限。", L"Check clipboard availability, save location and write permissions.");
        Notify(title, detail, NIIF_WARNING);
    }


    void AppController::ShowSettings(bool visible)
    {
        if (!m_window || m_shuttingDown) return;
        if (m_settingsWindow && m_settingsWindow->Finished()) m_settingsWindow.reset();
        if (m_settingsWindow) { m_settingsWindow->Activate(); return; }
        if (m_settingsOpening) return;
        if (m_selectionOverlay || m_pendingSelectionMonitor) {
            Notify(Localized(L"正在框选", L"Selection in progress"),
                Localized(L"请先完成或取消框选，再打开设置。", L"Finish or cancel the selection before opening Settings."));
            return;
        }
        m_settingsOpening = true;
        try {
            if (!m_shuttingDown) {
                auto dialog = std::make_shared<SettingsWindow>(m_settings,
                    [this](HotkeySettings const& settings) { return ProbeHotkeyAvailability(settings); },
                    [this](AppSettings const& settings) { return ApplySettings(settings); },
                    [this](bool recording) {
                        m_settingsRecording = recording;
                        if (recording) ReleaseConfiguredHotkeys();
                        else if (!m_shuttingDown) m_hotkeyRegistration = RegisterConfiguredHotkeys(m_hotkeys);
                    });
                m_settingsWindow = dialog;
                if (!dialog->Show(m_window, visible)) throw std::runtime_error("Settings property sheet creation failed");
            }
        } catch (...) {
            if (m_settingsWindow) m_settingsWindow->Close();
            m_settingsWindow.reset();
            if (!m_shuttingDown) Notify(Localized(L"设置不可用", L"Settings unavailable"),
                Localized(L"无法打开设置，原配置未改变。", L"Could not open Settings. Previous configuration retained."), NIIF_WARNING);
        }
        m_settingsOpening = false;
    }

    SettingsApplyResult AppController::ApplySettings(AppSettings const& settings)
    {
        if (m_shuttingDown || !m_settingsWindow || m_settingsApplying)
            return { SettingsApplyIssue::Cancelled, HRESULT_FROM_WIN32(ERROR_CANCELLED), {} };
        m_settingsApplying = true;
        struct Resume {
            AppController& owner;
            ~Resume() {
                owner.m_settingsApplying = false;
                if (!owner.m_shuttingDown && owner.m_lensManager) owner.m_lensManager->SetSettingsCommitInProgress(false);
            }
        } resume{ *this };
        // The sheet itself stays modeless. Only committing runtime geometry
        // briefly drains input before updating existing regions.
        if (m_lensManager) m_lensManager->SetSettingsCommitInProgress(true);
        if (m_shuttingDown) return { SettingsApplyIssue::Cancelled, HRESULT_FROM_WIN32(ERROR_CANCELLED), {} };
        // Allocate new runtime values before committing the single registry value.
        auto candidate = settings;
        auto directory = ResolveScreenshotDirectory(candidate, *Runtime().identity);
        auto result = CommitAppSettings(m_settings, candidate,
            [this](HotkeySettings const& keys) { return RegisterConfiguredHotkeys(keys); },
            [this] { ReleaseConfiguredHotkeys(); },
            [this](AppSettings const& value) { return SaveAppSettings(*Runtime().identity, value); });
        if (!result.Applied()) {
            m_hotkeyRegistration = RegisterConfiguredHotkeys(m_hotkeys);
            return result;
        }
        bool qualityChanged = m_settings.quality != candidate.quality;
        bool fullscreenAspectChanged = m_settings.fullscreenAspectFit != candidate.fullscreenAspectFit;
        bool languageChanged = m_settings.language != candidate.language;
        m_settings = std::move(candidate);
        m_hotkeys = m_settings.hotkeys;
        m_screenshotDirectory = std::move(directory);
        m_hotkeyRegistration = result.availability;
        SetAppLanguage(m_settings.language);
        if (m_lensManager) {
            m_lensManager->SetNewWindowTopmost(m_settings.newWindowTopmost);
            if (qualityChanged) m_lensManager->SetQualityLevel(m_settings.quality);
            if (fullscreenAspectChanged) m_lensManager->SetFullscreenAspectFitEnabled(m_settings.fullscreenAspectFit);
            if (languageChanged) m_lensManager->RefreshLanguage();
        }
        if (m_window) SetWindowTextW(m_window, Localized(L"区域镜", L"RegionLens"));
        return result;
    }

    MonitorCapture* AppController::EnsureCapture(HMONITOR monitor)
    {
        auto found = m_captures.find(monitor);
        if (found != m_captures.end()) return found->second.get();

        auto capture = std::make_shared<MonitorCapture>(monitor, m_window, FrameReadyMessage, m_device);
        auto started = Runtime().diagnostics ? QpcNow() : 0;
        bool success = capture->Start();
        Record(DiagnosticEvent::CaptureLifecycle,
            { 0, success, started ? int64_t(QpcMicros(QpcNow() - started)) : 0,
              int64_t(reinterpret_cast<intptr_t>(monitor)), int64_t(m_captures.size() + (success ? 1 : 0)) },
            !success);
        if (!success) return nullptr;
        auto inserted = m_captures.emplace(monitor, std::move(capture)).first;
        return inserted->second.get();
    }

    void AppController::ReleaseUnusedCapture(HMONITOR monitor, CaptureReleaseReason reason)
    {
        if (!monitor) return;
        auto found = m_captures.find(monitor);
        if (found == m_captures.end()) return;
        auto lensCount = m_lensManager ? m_lensManager->CountForMonitor(monitor) : 0;
        if (CaptureIsNeeded(monitor, m_pendingSelectionMonitor, lensCount)) return;

        auto capture = std::move(found->second);
        m_captures.erase(found); // Reject already-posted frame messages before stopping the producer.
        auto started = Runtime().diagnostics ? QpcNow() : 0;
        capture->Stop();
        capture.reset(); // Include GPU texture/view release in the lifecycle boundary.
        Record(DiagnosticEvent::CaptureLifecycle,
            { 1, int64_t(reason), started ? int64_t(QpcMicros(QpcNow() - started)) : 0,
              int64_t(reinterpret_cast<intptr_t>(monitor)), int64_t(m_captures.size()), int64_t(lensCount) });
    }

    void AppController::ReleaseUnusedCaptures(CaptureReleaseReason reason)
    {
        for (auto current = m_captures.begin(); current != m_captures.end();)
        {
            auto monitor = current->first;
            ++current; // Erasing the previously visited element keeps this iterator valid.
            ReleaseUnusedCapture(monitor, reason);
        }
    }

    void AppController::StartNewSelection()
    {
        if (m_selectionOverlay || m_pendingSelectionMonitor)
        {
            Notify(Localized(L"正在框选", L"Selection in progress"),
                Localized(L"请先确认或取消当前选区。", L"Confirm or cancel the current selection first."), NIIF_INFO);
            return;
        }
        if (m_lensManager->Count() >= 16)
        {
            Notify(Localized(L"区域数量已达上限", L"Region limit reached"),
                Localized(L"最多同时创建 16 个区域。", L"A maximum of 16 live regions can be open at once."), NIIF_WARNING);
            return;
        }

        // A new modal selection invalidates the trial's visible-lens snapshot.
        if (auto probe = Runtime().weTypeProbe)
            probe->Stop(WeTypeProbeStopReason::Manual);
        m_lensManager->SuspendInputMappings();
        POINT cursor{};
        GetCursorPos(&cursor);
        auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        auto capture = EnsureCapture(monitor);
        if (!capture)
        {
            m_lensManager->ResumeInputMappings();
            Notify(Localized(L"无法捕获屏幕", L"Screen capture unavailable"),
                Localized(L"Windows Graphics Capture 启动失败。", L"Windows Graphics Capture failed to start."), NIIF_ERROR);
            return;
        }
        m_pendingSelectionMonitor = monitor;
        capture->ProcessPendingFrame(); // Explicit snapshot consumer, including after all lenses were hidden.
        if (capture->HasFrame())
        {
            BeginSelection(*capture);
        }
    }

    void AppController::RequestPresentation()
    {
        if(m_shuttingDown || m_presentPosted || m_presenting || !m_window) return;
        m_presentPosted=PostMessageW(m_window,PresentMessage,0,0)!=FALSE;
    }

    void AppController::PresentLatest()
    {
        if(m_shuttingDown || m_presenting || m_selectionOverlay || !m_lensManager) return;
        m_presenting=true;
        struct Restore {bool& flag;~Restore(){flag=false;}} restore{m_presenting};
        // Surface acknowledgements remain immediate; their visual consequences
        // are retained until the newest capture has been copied below.
        m_lensManager->RefreshInputMappingFromCursor();
        if(m_shuttingDown || !m_lensManager) return;
        for(auto& [monitor,capture]:m_captures) {
            if(capture->HasPendingFrame() && m_lensManager->MonitorReady(monitor) && capture->ProcessPendingFrame())
                m_lensManager->RenderMonitor(monitor,capture->LatestView(),capture->Width(),capture->Height(),capture->Stamp());
        }
        m_lensManager->FlushPresentations();
        m_presenting=false;
        // A rare DO_NOT_WAIT rejection can already be ready again. Schedule
        // another batch, never spin or render it twice in the current batch.
        for(auto const& [monitor,capture]:m_captures)
            if(m_lensManager->MonitorReady(monitor)) {RequestPresentation();break;}
    }

    void AppController::BeginSelection(MonitorCapture& capture)
    {
        auto frozen = capture.Freeze();
        if (!frozen)
        {
            Notify(Localized(L"无法冻结屏幕", L"Could not freeze the screen"),
                Localized(L"尚未获得有效画面，请稍后重试。", L"No valid frame is available yet. Try again shortly."), NIIF_WARNING);
            CancelSelection();
            return;
        }
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(capture.Monitor(), &info))
        {
            CancelSelection();
            return;
        }

        m_selectionOverlay = std::make_unique<SelectionOverlay>(
            m_device,
            capture.Monitor(),
            info.rcMonitor,
            std::move(frozen),
            [this](PixelRect selection)
            {
                m_pendingSelectionRect = selection;
                PostMessageW(m_window, SelectionConfirmedMessage, 0, 0);
            },
            [this]()
            {
                PostMessageW(m_window, SelectionCancelledMessage, 0, 0);
            });
        if (!m_selectionOverlay->Show())
        {
            m_selectionOverlay.reset();
            Notify(Localized(L"无法显示框选层", L"Could not show the selection overlay"),
                Localized(L"创建静止框选窗口失败。", L"The frozen selection window could not be created."), NIIF_ERROR);
            CancelSelection();
        }
    }

    void AppController::ConfirmSelection()
    {
        auto monitor = m_pendingSelectionMonitor;
        if (!monitor || m_pendingSelectionRect.Empty())
        {
            CancelSelection();
            return;
        }
        MONITORINFO info{ sizeof(info) };
        if (!GetMonitorInfoW(monitor, &info))
        {
            CancelSelection();
            return;
        }

        if (m_selectionOverlay)
        {
            m_selectionOverlay->Close();
            m_selectionOverlay.reset();
            ResumeCaptureProcessing();
        }
        RECT windowBounds
        {
            info.rcMonitor.left + m_pendingSelectionRect.x,
            info.rcMonitor.top + m_pendingSelectionRect.y,
            info.rcMonitor.left + m_pendingSelectionRect.Right(),
            info.rcMonitor.top + m_pendingSelectionRect.Bottom()
        };
        auto id = m_lensManager->Create(monitor, m_pendingSelectionRect, windowBounds);
        if (!id)
        {
            Notify(Localized(L"无法创建区域", L"Could not create the region"),
                Localized(L"窗口创建失败或已达到 16 个区域上限。",
                    L"Window creation failed or the 16-region limit has been reached."), NIIF_ERROR);
        }
        else
        {
            auto found = m_captures.find(monitor);
            if (found != m_captures.end() && found->second->HasFrame())
            {
                m_lensManager->RenderMonitor(monitor, found->second->LatestView(), found->second->Width(), found->second->Height(), found->second->Stamp());
            }
        }
        m_pendingSelectionMonitor = nullptr;
        m_pendingSelectionRect = {};
        ReleaseUnusedCapture(monitor, CaptureReleaseReason::SelectionEnded);
        m_lensManager->ResumeInputMappings();
    }

    void AppController::CancelSelection()
    {
        auto monitor = m_pendingSelectionMonitor;
        bool hadSelection = m_selectionOverlay != nullptr || m_pendingSelectionMonitor != nullptr;
        bool resumeCapture = m_selectionOverlay != nullptr;
        if (m_selectionOverlay)
        {
            m_selectionOverlay->Close();
            m_selectionOverlay.reset();
        }
        m_pendingSelectionMonitor = nullptr;
        m_pendingSelectionRect = {};
        ReleaseUnusedCapture(monitor, CaptureReleaseReason::SelectionEnded);
        if (resumeCapture && !m_shuttingDown) ResumeCaptureProcessing();
        if (hadSelection && !m_shuttingDown && m_lensManager)
            m_lensManager->ResumeInputMappings();
    }

    void AppController::ResumeCaptureProcessing()
    {
        if (!m_lensManager) return;
        for (auto& [monitor, capture] : m_captures)
        {
            if (capture->HasPendingFrame()) m_lensManager->MarkMonitorDirty(monitor);
        }
        RequestPresentation();
    }

    void AppController::SetRegionsHidden(bool hidden)
    {
        if (!m_lensManager || m_shuttingDown) return;
        m_visibilityAction = true;
        // Hide establishes its independent pause before dismissing selection;
        // otherwise selection close could briefly reactivate a hidden route.
        if (hidden) m_lensManager->HideAll();
        else if (!m_lensManager->ShowAllRaised())
            Notify(Localized(L"部分区域未能显示或前置", L"Some regions could not be shown or raised"),
                Localized(L"请再次执行“显示全部并前置”。", L"Please try Show all and raise again."), NIIF_WARNING);
        CancelSelection();
        m_visibilityAction = false;
        if (m_window && (m_deferredExit || !m_deferredInputActions.Empty()))
            PostMessageW(m_window, InputMappingCoordinator::LifecycleIdleMessage, 0, 0);
    }

    void AppController::CloseAll(CaptureReleaseReason reason)
    {
        if (m_lensManager)
        {
            m_lensManager->CloseAll();
        }
        ReleaseUnusedCaptures(reason);
    }

    void AppController::Shutdown()
    {
        if (Runtime().weTypeProbe) Runtime().weTypeProbe->Stop(WeTypeProbeStopReason::Shutdown);
        if (m_shuttingDown) return;
        m_shuttingDown = true;
        m_topmostFastWake.reset();
        if (m_settingsWindow) m_settingsWindow->Close();
        if (m_window) KillTimer(m_window, ScreenshotTimerId);
        if (m_lensManager) m_lensManager->StopTopmostEvents();
        if (m_window) KillTimer(m_window, TopmostTimerId);
        CancelSelection();
        CloseAll(CaptureReleaseReason::Shutdown);
        m_captures.clear();
        m_lensManager.reset();
        if (m_inputMapping)
        {
            m_inputMapping->Shutdown();
            m_inputMapping.reset();
        }
        ReleaseConfiguredHotkeys();
        RemoveTrayIcon();
        if (m_window)
        {
            auto window = m_window;
            DestroyWindow(window);
            if (m_window == window)
            {
                m_window = nullptr;
            }
        }
    }
}
