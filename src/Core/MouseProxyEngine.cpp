#include "pch.h"
#include "MouseProxyEngine.h"
#include "CursorObservationNative.h"
#include "DiagnosticScope.h"
#include <bcrypt.h>

namespace RegionLens::native
{
    thread_local MouseProxyEngine* MouseProxyEngine::s_current{};
    MouseProxyEngine::MouseProxyEngine(HWND window) : m_notificationWindow(window), m_core(*this) {}
    MouseProxyEngine::~MouseProxyEngine() { Stop(); }
    bool MouseProxyEngine::Start()
    {
        if (m_running && m_watchdog.Healthy()) return true;
        DiagnosticScope starting(DiagnosticStage::InputStart);
        Stop();
        m_startupFailure = {};
        if (!m_watchdog.Start()) { m_startupFailure = m_watchdog.LastFailure(); return false; }
        auto random = [](uint32_t& value) { return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&value),
            sizeof(value), BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0; };
        if (!CreateProxyInputCookie(m_sendCookie, random) || !CreateProxyInputCookie(m_fenceCookie, random) ||
            m_sendCookie == m_watchdog.Cookie() || m_fenceCookie == m_watchdog.Cookie() || m_sendCookie == m_fenceCookie)
        {
            m_startupFailure = { ProxyStartupStage::InputCookie, ERROR_INVALID_DATA }; Stop(); return false;
        }
        auto eventFailed = [this]
        {
            m_startupFailure = { ProxyStartupStage::InputEvents, StartupError() }; Stop(); return false;
        };
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_wake) return eventFailed();
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_ready) return eventFailed();
        m_released = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!m_released) return eventFailed();
        m_cursorLeaseDone = CreateEventW(nullptr, TRUE, TRUE, nullptr);
        if (!m_cursorLeaseDone) return eventFailed();
        m_sendWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_sendWake) return eventFailed();
        m_stop = false;
        m_sendStop = false;
        m_persistentCursorLease = false;
        m_overlayHeartbeatTimedOut = false;
        m_externalPending = false;
        try
        {
            m_sender = std::thread([this] { SendLoop(); });
            m_thread = std::thread([this] { Run(); });
        }
        catch (std::system_error const&)
        {
            m_startupFailure = { ProxyStartupStage::InputThread, ERROR_NOT_ENOUGH_MEMORY }; Stop(); return false;
        }
        auto wait = WaitForSingleObject(m_ready, 3000);
        if (wait != WAIT_OBJECT_0 || !m_running)
        {
            auto error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : StartupError();
            Stop(); // Join before reading or overriding the worker's failure.
            if (wait != WAIT_OBJECT_0) m_startupFailure = { ProxyStartupStage::InputThread, error };
            return false;
        }
        starting.End();
        return true;
    }
    void MouseProxyEngine::Stop()
    {
        DiagnosticScope stopping(DiagnosticStage::InputStop);
        m_stop = true;
        if (m_wake) SetEvent(m_wake);
        if (m_thread.joinable()) m_thread.join();
        m_sendStop = true;
        if (m_sendWake) SetEvent(m_sendWake);
        if (m_sender.joinable()) m_sender.join();
        m_watchdog.Stop();
        if (m_wake) CloseHandle(std::exchange(m_wake, nullptr));
        if (m_ready) CloseHandle(std::exchange(m_ready, nullptr));
        if (m_released) CloseHandle(std::exchange(m_released, nullptr));
        if (m_cursorLeaseDone) CloseHandle(std::exchange(m_cursorLeaseDone, nullptr));
        if (m_sendWake) CloseHandle(std::exchange(m_sendWake, nullptr));
        m_running = false;
        std::lock_guard lock(m_commandMutex); m_commands.clear();
        stopping.End();
    }
    void MouseProxyEngine::Enqueue(Command command)
    {
        if (!m_running) return;
        {
            std::lock_guard lock(m_commandMutex);
            if (command.kind == Command::Configure && !m_commands.empty() && m_commands.back().kind == Command::Configure)
                m_commands.back() = command;
            else if (m_commands.size() < 64) m_commands.push_back(command);
            else { Record(DiagnosticEvent::QueueOverflow, { 64 }, true); m_watchdog.Cancel(ProxyCancelReason::CommandOverflow); }
        }
        SetEvent(m_wake);
    }
    void MouseProxyEngine::Configure(MappingSessionConfig config, bool probeCursor)
    {
        Command c; c.kind = Command::Configure; c.config = config; c.probeCursor = probeCursor; Enqueue(c);
    }
    bool MouseProxyEngine::SwitchSoftwareCursor(bool begin)
    {
        if (!PersistentSoftwareCursor(Runtime())) return true;
        if (!m_running || m_stop || !m_cursorLeaseDone || !m_wake) return !begin;
        if (begin) SoftwareCursorHeartbeat();
        Command command;
        command.kind = begin ? Command::BeginCursorLease : Command::EndCursorLease;
        command.request = ++m_nextCursorLease;
        ResetEvent(m_cursorLeaseDone);
        {
            std::lock_guard lock(m_commandMutex);
            if (m_commands.size() >= 64)
            {
                m_watchdog.Cancel(ProxyCancelReason::CommandOverflow);
                return false;
            }
            m_commands.push_back(command);
        }
        SetEvent(m_wake);
        auto deadline = GetTickCount64() + 750;
        while (m_completedCursorLease < command.request)
        {
            auto now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(m_cursorLeaseDone, DWORD(deadline - now)) != WAIT_OBJECT_0) break;
            ResetEvent(m_cursorLeaseDone);
        }
        if (m_completedCursorLease >= command.request) return m_cursorLeaseResult;
        m_watchdog.Cancel(ProxyCancelReason::UiDrainTimeout);
        return false;
    }
    bool MouseProxyEngine::BeginSoftwareCursor() { return SwitchSoftwareCursor(true); }
    bool MouseProxyEngine::EndSoftwareCursor() { return SwitchSoftwareCursor(false); }
    void MouseProxyEngine::SoftwareCursorHeartbeat() noexcept
    {
        if (PersistentSoftwareCursor(Runtime())) m_softwareCursorHeartbeat = GetTickCount64();
    }
    bool MouseProxyEngine::NativeCursorShowConfirmed() const noexcept
    {
        // Read only after Stop joined the input thread. This reports the local
        // show API acknowledgement, not a visual-desktop guarantee.
        return !PersistentSoftwareCursor(Runtime()) ||
            (!m_persistentCursorLease && (!m_cursorRecovery.Touched() || m_cursorRecovery.ShowSucceeded()));
    }
    uint64_t MouseProxyEngine::Disable()
    {
        Command c; c.kind = Command::Disable; c.request = ++m_nextDisable;
        Enqueue(c); return c.request;
    }
    bool MouseProxyEngine::DisableAndDrain()
    {
        if (!m_running) return true;
        auto request = Disable();
        auto deadline = GetTickCount64() + 750;
        while (m_completedDisable < request)
        {
            ResetEvent(m_released);
            if (m_completedDisable >= request) break;
            auto now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(m_released, DWORD(deadline - now)) != WAIT_OBJECT_0) break;
        }
        if (m_completedDisable >= request) return true;
        m_watchdog.Cancel(ProxyCancelReason::UiDrainTimeout);
        return false;
    }
    void MouseProxyEngine::Acknowledge(uint64_t request, bool success)
    {
        Command c; c.kind = Command::Acknowledge; c.request = request; c.success = success; Enqueue(c);
    }
    void MouseProxyEngine::NotifyUi()
    {
        if (!m_uiPending.exchange(true)) PostMessageW(m_notificationWindow, UiMessage, 0, 0);
    }
    ProxyUiUpdate MouseProxyEngine::TakeUiUpdate()
    {
        m_uiPending = false;
        std::lock_guard lock(m_uiMutex);
        auto value = m_ui;
        m_ui.surfaces.reset();
        m_ui.cursor.localMoveRequested = false;
        m_ui.externalCursorMoved = false;
        return value;
    }
    void MouseProxyEngine::PublishExternalCursor()
    {
        if (!m_externalPending.exchange(false)) return;
        auto position = UnpackProxyPoint(m_externalPoint.load());
        {
            std::lock_guard lock(m_uiMutex);
            m_ui.externalCursorPosition = position;
            m_ui.externalCursorMoved = true;
        }
        NotifyUi();
    }
    bool MouseProxyEngine::SameInputDesktop()
    {
        auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!desktop) return false;
        wchar_t name[256]{}; DWORD required{};
        bool read = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &required) != FALSE;
        CloseDesktop(desktop);
        return read && m_desktopName == name;
    }
    void MouseProxyEngine::Run()
    {
        DiagnosticScope thread(DiagnosticStage::InputThread);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        s_current = this;
        m_core.ResetAfterStop(); m_pendingDisable = 0;
        m_cursorRecovery = {};
        m_healthCheckTick = m_desktopCheckTick = m_cursorShapeTick = 0;
        m_guardCheckOk = m_desktopCheckOk = false; m_cursorShape = nullptr;
        MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        wchar_t desktop[256]{}; DWORD required{};
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &required);
        m_desktopName = desktop;
        SetLastError(ERROR_SUCCESS);
        DiagnosticScope magnification(DiagnosticStage::InputMagnification);
        bool mag = MagInitialize() != FALSE;
        magnification.End(mag ? 0 : GetLastError());
        BOOL existing{}; RECT source{}, destination{}; float scale{}; int x{}, y{};
        if (!mag) m_startupFailure = { ProxyStartupStage::InputMagnification, StartupError() };
        else
        {
            SetLastError(ERROR_SUCCESS);
            DiagnosticScope transform(DiagnosticStage::InputTransform);
            if (!MagGetInputTransform(&existing, &source, &destination) || !MagGetFullscreenTransform(&scale, &x, &y))
                m_startupFailure = { ProxyStartupStage::InputTransform, StartupError() };
            else if (existing || scale != 1.0f) m_startupFailure = { ProxyStartupStage::InputTransform, ERROR_BUSY };
            else if (!SameInputDesktop()) m_startupFailure = { ProxyStartupStage::InputDesktop, ERROR_ACCESS_DENIED };
            transform.End(m_startupFailure.error);
        }
        bool available = m_startupFailure.stage == ProxyStartupStage::None;
        POINT initial{};
        if (available && !GetPhysicalCursorPos(&initial))
        { available = false; m_startupFailure = { ProxyStartupStage::InputCursor, StartupError() }; }
        m_pointer = {}; m_pointer.Passed(initial);
        m_controlState.Seed((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0,
            (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
        if (available)
        {
            DiagnosticScope hook(DiagnosticStage::InputHook);
            m_hook = SetWindowsHookExW(WH_MOUSE_LL, Hook, GetModuleHandleW(nullptr), 0);
            if (!m_hook) m_startupFailure = { ProxyStartupStage::InputHook, StartupError() };
            else
            {
                m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHook, GetModuleHandleW(nullptr), 0);
                if (!m_keyboardHook) m_startupFailure = { ProxyStartupStage::InputHook, StartupError() };
            }
            hook.End(m_startupFailure.error);
        }
        // Seed buttons already held before this thread installed its hook. The
        // hook itself must not use asynchronous key-state polling.
        uint32_t buttons{};
        constexpr int keys[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
        for (unsigned i = 0; i < ARRAYSIZE(keys); ++i)
            if (GetAsyncKeyState(keys[i]) & 0x8000) buttons |= 1u << i;
        m_core.SeedPhysicalButtons(buttons);
        m_running = m_hook != nullptr && m_keyboardHook != nullptr;
        Record(DiagnosticEvent::Startup, { m_running.load(), LONG(m_startupFailure.stage), m_startupFailure.error }, !m_running);
        Record(DiagnosticEvent::Checkpoint, { int64_t(DiagnosticStage::InputReady), 1, m_startupFailure.error });
        SetEvent(m_ready);
        uint64_t sampleTick{};
        while (m_running && !m_stop)
        {
            MsgWaitForMultipleObjectsEx(1, &m_wake, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            for (unsigned n = 0; n < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n)
            {
                TranslateMessage(&message); DispatchMessageW(&message);
            }
            m_watchdog.Heartbeat();
            PollSend();
            std::deque<Command> commands;
            {
                std::lock_guard lock(m_commandMutex);
                commands.swap(m_commands);
            }
            for (auto const& command : commands)
            {
                if (command.kind == Command::BeginCursorLease || command.kind == Command::EndCursorLease)
                {
                    bool result = command.kind == Command::BeginCursorLease ?
                        BeginPersistentCursorLease() : EndPersistentCursorLease();
                    m_cursorLeaseResult = result;
                    m_completedCursorLease = command.request;
                    SetEvent(m_cursorLeaseDone);
                }
                else if (command.kind == Command::Configure)
                {
                    auto const& c = command.config;
                    // Re-seed outside either hook callback. If a secure-desktop
                    // transition swallowed a Ctrl UP, a later mapping session
                    // must not inherit the stale chord.
                    m_controlState.Seed((GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0,
                        (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0);
                    Record(DiagnosticEvent::Session, { c.blocked, c.fullscreen }, false, c.generation, c.lensId);
                    m_core.Configure(c);
                    // The route can arrive after the physical move that entered the lens.
                    // Probe once so a stationary pointer activates without waiting for another move.
                    if (command.probeCursor && m_core.Phase() == ProxyPhase::Armed)
                    {
                        POINT point{};
                        if (GetPhysicalCursorPos(&point))
                        {
                            auto event = m_pointer.Hardware(WM_MOUSEMOVE, point, 0);
                            event.controlDown = m_controlState.Down();
                            if (!m_core.Intercept(event)) m_pointer.Passed(point);
                        }
                    }
                }
                else if (command.kind == Command::Disable)
                {
                    m_pendingDisable = command.request;
                    m_core.Disable();
                }
                else
                {
                    Record(DiagnosticEvent::SurfaceAck, { int64_t(command.request), command.success }, !command.success);
                    m_core.SurfaceAcknowledged(command.request, command.success);
                }
            }
            if (m_persistentCursorLease && !m_overlayHeartbeatTimedOut)
            {
                auto now = GetTickCount64();
                auto heartbeat = m_softwareCursorHeartbeat.load();
                if (!heartbeat || (now > heartbeat && now - heartbeat > 750))
                {
                    m_overlayHeartbeatTimedOut = true;
                    Record(DiagnosticEvent::CursorRecovery, { 3, ERROR_TIMEOUT, int64_t(now - heartbeat) }, true);
                    m_watchdog.Cancel(ProxyCancelReason::OverlayHeartbeatTimeout);
                    m_core.Disable(ERROR_TIMEOUT, ProxyFaultSite::HealthCheck);
                }
            }
            m_core.Pump();
            if (m_persistentCursorLease && m_watchdog.CancellationReason() != ProxyCancelReason::None &&
                m_core.InputReleased()) EndPersistentCursorLease();
            PublishExternalCursor();
            ServiceCursorRecovery();
            if (Runtime().diagnostics && GetTickCount64() - sampleTick >= 500) {
                sampleTick = GetTickCount64();
                if (sampleTick-m_latencyLast>=1000) {
                    m_queueLatency.Report(0);m_senderLatency.Report(1);m_apiLatency.Report(2);m_ackLatency.Report(8);
                    m_latencyLast=sampleTick;
                }
                auto sample = m_core.Snapshot();
                if (sample.phase == ProxyPhase::Active)
                    Record(DiagnosticEvent::Sample, { sample.position.x, sample.position.y, sample.sourcePosition.x, sample.sourcePosition.y,
                        int64_t(m_core.InputQueueDepth()), m_sendChannel.Busy(), sample.buttons }, false, sample.generation, sample.lensId);
            }
        }
        m_core.Disable();
        // Process our outstanding injected UPs before removing the hook. UI
        // surface restoration is also performed by the coordinator on shutdown.
        // The sender may be inside SendInput. Keep its hook thread pumping until
        // it returns; joining it while blocking this pump recreates the stall.
        while (m_sendChannel.Busy() || !m_core.InputReleased())
        {
            MsgWaitForMultipleObjectsEx(1, &m_wake, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            for (unsigned n = 0; n < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++n)
                DispatchMessageW(&message);
            m_watchdog.Heartbeat();
            PollSend();
            m_core.Pump();
        }
        // A normal DeactivateAll/Shutdown can bypass the UI's explicit End.
        // Restore while Magnification remains initialized; the guardian keeps
        // the pending lease if this local attempt cannot complete.
        EndPersistentCursorLease();
        if (auto shared = m_watchdog.Shared(); shared &&
            (SharedRead(&shared->active) || CursorLeasePending(SharedRead64(&shared->cursorLease))))
            m_watchdog.Cancel(ProxyCancelReason::WorkerShutdown);
        if (m_hook) UnhookWindowsHookEx(std::exchange(m_hook, nullptr));
        if (m_keyboardHook) UnhookWindowsHookEx(std::exchange(m_keyboardHook, nullptr));
        if (mag) MagUninitialize();
        m_running = false; s_current = nullptr;
        thread.End(m_startupFailure.error);
    }
    LRESULT CALLBACK MouseProxyEngine::Hook(int code, WPARAM wParam, LPARAM lParam)
    {
        auto self = s_current;
        if (code == HC_ACTION && self)
        {
            auto mouse = reinterpret_cast<MSLLHOOKSTRUCT const*>(lParam);
            auto origin = ClassifyProxyInput(mouse->flags, mouse->dwExtraInfo, self->m_watchdog.Cookie());
            bool senderTag = (mouse->flags & LLMHF_INJECTED) && mouse->dwExtraInfo == self->m_sendCookie;
            bool fenceTag = (mouse->flags & LLMHF_INJECTED) && mouse->dwExtraInfo == self->m_fenceCookie;
            if (senderTag || fenceTag) origin = ProxyInputOrigin::OwnInjection;
            bool consumed = false;
            if (origin == ProxyInputOrigin::OwnInjection)
            {
                // A cancelled normal packet must not revive the old session if
                // the OS call returns late. Guardian/recovery tags remain valid.
                consumed = RejectCancelledProxyPacket(mouse->flags, mouse->dwExtraInfo, self->m_sendCookie,
                    self->m_fenceCookie, self->m_sendChannel.Job().recovery,
                    self->m_watchdog.CancellationReason() != ProxyCancelReason::None);
                if (!consumed) self->m_pointer.Passed(mouse->pt);
                self->m_sendChannel.Observe(UINT(wParam), mouse->flags, mouse->dwExtraInfo);
                SetEvent(self->m_wake);
            }
            else
            {
                if (origin == ProxyInputOrigin::ForeignInjection)
                {
                    // Another accessibility/automation tool takes precedence.
                    self->m_pointer.Passed(mouse->pt);
                    self->m_watchdog.Cancel(ProxyCancelReason::ForeignInput); SetEvent(self->m_wake);
                }
                else
                {
                    auto event = self->m_pointer.Hardware(UINT(wParam), mouse->pt, mouse->mouseData);
                    event.controlDown = self->m_controlState.Down();
                    if (Runtime().diagnostics) event.queuedQpc=QpcNow();
                    consumed = self->m_core.Intercept(event);
                    if (consumed) SetEvent(self->m_wake);
                    else
                    {
                        self->m_pointer.Passed(mouse->pt);
                        if (self->m_persistentCursorLease && wParam == WM_MOUSEMOVE)
                        {
                            self->m_externalPoint = PackProxyPoint(mouse->pt);
                            self->m_externalPending = true;
                            SetEvent(self->m_wake);
                        }
                    }
                }
            }
            if (consumed) return 1;
        }
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    LRESULT CALLBACK MouseProxyEngine::KeyboardHook(int code, WPARAM wParam, LPARAM lParam)
    {
        auto self = s_current;
        if (code == HC_ACTION && self)
        {
            auto key = reinterpret_cast<KBDLLHOOKSTRUCT const*>(lParam);
            self->m_controlState.Update(UINT(wParam), key->vkCode, key->flags);
        }
        // Keyboard remains native. This hook records only Ctrl's held state for
        // the local mouse chord; it never blocks or injects a keyboard event.
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    bool MouseProxyEngine::Healthy()
    {
        auto now = GetTickCount64();
        if (!m_healthCheckTick || now - m_healthCheckTick >= 50)
        {
            m_guardCheckOk = m_watchdog.Healthy();
            m_healthCheckTick = now;
        }
        else if (m_watchdog.CancellationReason() != ProxyCancelReason::None)
        {
            m_guardCheckOk = false;
        }
        bool guard = m_guardCheckOk;
        int desktop = -1;
        if (guard)
        {
            if (!m_desktopCheckTick || now - m_desktopCheckTick >= 50)
            {
                m_desktopCheckOk = SameInputDesktop();
                m_desktopCheckTick = now;
            }
            desktop = int(m_desktopCheckOk);
        }
        return guard && desktop == 1;
    }
    bool MouseProxyEngine::CanEnter(MappingSessionConfig const& config, POINT point)
    {
        // Do not use WindowFromPoint here: it can synchronously send WM_NCHITTEST
        // to the UI thread, deadlocking shutdown while that thread joins us.
        return Healthy() && DestinationExposed(config, point);
    }
    bool MouseProxyEngine::SourceAccessible(MappingSessionConfig const& config, POINT point)
    {
        for (size_t i = 0; i < config.lensCount; ++i)
        {
            if (!IsWindow(config.lenses[i])) return false;
            auto style = GetWindowLongPtrW(config.lenses[i], GWL_EXSTYLE);
            if ((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT)) != (WS_EX_LAYERED | WS_EX_TRANSPARENT)) return false;
        }
        return PtInRect(&config.source, point) && MonitorFromPoint(point, MONITOR_DEFAULTTONULL);
    }
    bool MouseProxyEngine::DestinationExposed(MappingSessionConfig const& config, POINT point)
    {
        struct Search { MappingSessionConfig const& config; POINT point; bool visible{}; } search{ config, point };
        EnumWindows([](HWND window, LPARAM data) -> BOOL
        {
            auto& s = *reinterpret_cast<Search*>(data);
            if (!IsWindowVisible(window) || IsIconic(window)) return TRUE;
            RECT bounds{}; if (!GetWindowRect(window, &bounds) || !PtInRect(&bounds, s.point)) return TRUE;
            if (window == s.config.lensWindow) { s.visible = true; return FALSE; }
            for (size_t i = 0; i < s.config.lensCount; ++i) if (window == s.config.lenses[i]) return FALSE;
            auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
            if ((style & WS_EX_LAYERED) && (style & WS_EX_TRANSPARENT)) return TRUE;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        return search.visible;
    }
    bool MouseProxyEngine::CursorVisible(bool visible)
    {
        if (PersistentSoftwareCursor(Runtime()))
        {
            // The guardian-backed hide is owned by the whole standby lifetime,
            // not an individual Preparing/Restoring mapping session.
            return visible || (m_persistentCursorLease && Healthy());
        }
        if (!visible && !Healthy())
        {
            return false;
        }
        if (visible)
        {
            if (!m_cursorRecovery.BeginRestore(GetTickCount64())) return true;
            if (auto shared = m_watchdog.Shared())
                InterlockedExchange64(&shared->cursorLease, CursorLease(m_cursorRecovery.Epoch()));
            return ShowCursorForRecovery(0);
        }
        m_cursorRecovery.BeginHide();
        if (auto shared = m_watchdog.Shared())
            InterlockedExchange64(&shared->cursorLease, CursorLease(m_cursorRecovery.Epoch()));
        SetLastError(ERROR_SUCCESS);
        bool result = MagShowSystemCursor(visible ? TRUE : FALSE) != FALSE;
        Record(DiagnosticEvent::CursorRecovery, { visible, result, result ? 0 : GetLastError() }, !result);
        // Cancellation may arrive while the native hide is in progress. Undo it
        // before returning; a late old-session call must not leave the cursor hidden.
        if (!Healthy()) { CursorVisible(true); return false; }
        return result;
    }
    bool MouseProxyEngine::BeginPersistentCursorLease()
    {
        if (!PersistentSoftwareCursor(Runtime())) return true;
        if (m_persistentCursorLease) return Healthy();
        if (!Healthy() || !SameInputDesktop()) return false;
        m_cursorRecovery.BeginHide();
        if (auto shared = m_watchdog.Shared())
            InterlockedExchange64(&shared->cursorLease, CursorLease(m_cursorRecovery.Epoch()));
        // Treat even a failed hide as an uncertain visibility mutation. The
        // lease stays pending until either this worker or the guardian shows.
        m_persistentCursorLease = true;
        SetLastError(ERROR_SUCCESS);
        bool hidden = MagShowSystemCursor(FALSE) != FALSE;
        auto error = hidden ? ERROR_SUCCESS : GetLastError();
        Record(DiagnosticEvent::CursorRecovery, { 3, 0, hidden, error }, !hidden);
        if (!hidden || !Healthy())
        {
            EndPersistentCursorLease();
            m_watchdog.Cancel(ProxyCancelReason::RecoveryFailed);
            return false;
        }
        return true;
    }
    bool MouseProxyEngine::EndPersistentCursorLease()
    {
        if (!m_persistentCursorLease)
        {
            if (!m_cursorRecovery.Touched() || m_cursorRecovery.ShowSucceeded()) return true;
            // A previous show call failed. An End acknowledgement is not a
            // success until a local retry succeeds (the guardian still owns
            // its independent pending lease meanwhile).
            if (!m_cursorRecovery.Checking()) m_cursorRecovery.BeginRestore(GetTickCount64());
            bool shown = ShowCursorForRecovery(0);
            if (!shown) m_watchdog.Cancel(ProxyCancelReason::RecoveryFailed);
            return shown;
        }
        if (!m_core.InputReleased())
        {
            m_core.Disable();
            if (!m_core.InputReleased())
            {
                m_watchdog.Cancel(ProxyCancelReason::UiDrainTimeout);
                return false;
            }
        }
        m_persistentCursorLease = false;
        if (!m_cursorRecovery.BeginRestore(GetTickCount64())) return true;
        if (auto shared = m_watchdog.Shared())
            InterlockedExchange64(&shared->cursorLease, CursorLease(m_cursorRecovery.Epoch()));
        bool shown = ShowCursorForRecovery(0);
        Record(DiagnosticEvent::CursorRecovery, { 3, 1, shown }, !shown);
        if (!shown) m_watchdog.Cancel(ProxyCancelReason::RecoveryFailed);
        return shown;
    }
    bool MouseProxyEngine::ShowCursorForRecovery(unsigned step)
    {
        auto epoch = m_cursorRecovery.Epoch();
        bool desktop = SameInputDesktop();
        auto before = Runtime().diagnostics ? ObserveSystemCursor() : CursorObservation{};
        SetLastError(ERROR_SUCCESS);
        bool result = desktop && MagShowSystemCursor(TRUE) != FALSE;
        auto error = result ? ERROR_SUCCESS : (desktop ? GetLastError() : ERROR_ACCESS_DENIED);
        Record(DiagnosticEvent::CursorRecovery, { 1, result, desktop, error }, !result);
        if (Runtime().diagnostics)
        {
            auto after = ObserveSystemCursor();
            RecordCursorVisibility(0, step, epoch, result, error, before, after);
        }
        m_cursorRecovery.ShowResult(epoch, result);
        return result;
    }
    void MouseProxyEngine::ServiceCursorRecovery()
    {
        if (!m_cursorRecovery.Touched()) return;
        auto now = GetTickCount64();
        if (!m_cursorRecovery.Due(now)) return;
        auto observation = ObserveSystemCursor();
        auto check = m_cursorRecovery.Next(now, observation);
        if (m_cursorRecovery.Current(check))
        {
            if (check.show && !ShowCursorForRecovery(check.step)) RecoveryFailed();
            if (check.last && m_cursorRecovery.ShowSucceeded())
                if (auto shared = m_watchdog.Shared())
                    CompleteCursorLease(&shared->cursorLease, CursorLease(check.epoch));
        }
    }
    bool MouseProxyEngine::SendMouse(POINT point, UINT message, DWORD data, RECT desktop)
    {
        auto sendId = ++m_sendSequence;
        auto now = GetTickCount64();
        bool guardBefore = m_watchdog.CancellationReason() == ProxyCancelReason::None;
        bool desktopOk = m_desktopCheckOk && m_desktopCheckTick && now - m_desktopCheckTick < 100;
        if (!desktopOk || (!guardBefore && m_core.Phase() != ProxyPhase::Restoring))
            return false;
        bool recovery = m_core.Phase() == ProxyPhase::Restoring;
        auto batch = MakeProxyInputs(point, message, data, desktop, recovery ? m_watchdog.Cookie() : m_sendCookie);
        auto count = batch.count;
        if (!count) return false;
        ProxySendJob job;
        std::copy_n(batch.inputs.begin(), count, job.inputs.begin());
        job.count = count; job.message = message; job.point = point;
        job.sequence = sendId; job.tick = GetTickCount64(); job.recovery = recovery;
        MarkRecoveryReturnMove(job);
        if (Runtime().diagnostics) job.submittedQpc=QpcNow();
        job.site = recovery ? ProxyFaultSite::ReleaseInput : m_core.Phase() == ProxyPhase::Preparing ?
            ProxyFaultSite::InitialSend : ProxyFaultSite::ActiveSend;
        if (!m_sendChannel.Submit(job, m_fenceCookie)) return false;
        m_sendTimedOut = false;
        SetEvent(m_sendWake);
        return true; // Accepted by the bounded transport, not yet delivered.
    }
    void MouseProxyEngine::SendLoop()
    {
        DiagnosticScope thread(DiagnosticStage::SenderThread);
        // This thread never installs a hook, accesses the core, UI, or either
        // application state. Its only publication is a release-store result.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        uint64_t desktopCheckTick{};
        uint64_t guardCheckTick{};
        bool desktopOk{};
        bool guardOk{};
        while (!m_sendStop)
        {
            WaitForSingleObject(m_sendWake, INFINITE);
            ProxySendJob job;
            if (!m_sendChannel.Take(job)) continue;
            ProxySendResult result;
            if (Runtime().diagnostics) result.startedQpc=QpcNow();
            auto start = GetTickCount64();
            if (job.recovery || !desktopCheckTick || start - desktopCheckTick >= 50)
            {
                desktopOk = SameInputDesktop();
                desktopCheckTick = start;
            }
            if (job.recovery || !guardCheckTick || start - guardCheckTick >= 50)
            {
                guardOk = m_watchdog.Healthy();
                guardCheckTick = start;
            }
            else if (m_watchdog.CancellationReason() != ProxyCancelReason::None)
            {
                guardOk = false;
            }
            if (!desktopOk || (!job.recovery && (m_stop || !guardOk)))
                result.error = ERROR_CANCELLED;
            else
            {
                SetLastError(ERROR_SUCCESS);
                if (Runtime().diagnostics) result.apiStartedQpc=QpcNow();
                result.sent = SendInput(job.count, job.inputs.data(), sizeof(INPUT));
                if (result.sent != job.count) result.error = result.apiError = GetLastError();
            }
            result.duration = GetTickCount64() - start;
            if (Runtime().diagnostics) result.completedQpc=QpcNow();
            m_sendChannel.Complete(result);
            SetEvent(m_wake);
        }
        thread.End();
    }
    void MouseProxyEngine::PollSend()
    {
        if (!m_sendChannel.Busy()) return;
        auto job = m_sendChannel.Job();
        ProxySendResult result;
        if (m_sendChannel.Poll(GetTickCount64(), result))
        {
            if (Runtime().diagnostics && job.submittedQpc && result.startedQpc && result.completedQpc) {
                m_senderLatency.Add(QpcMicros(result.startedQpc-job.submittedQpc));
                if(result.apiStartedQpc) m_apiLatency.Add(QpcMicros(result.completedQpc-result.apiStartedQpc));
                m_ackLatency.Add(QpcMicros(QpcNow()-job.submittedQpc));
            }
            auto guardAfter = m_guardCheckOk && m_watchdog.CancellationReason() == ProxyCancelReason::None;
            if (!job.recovery && !guardAfter && !result.error) result.error = ERROR_CANCELLED;
            if (result.error == ERROR_TIMEOUT) m_watchdog.Cancel(ProxyCancelReason::InjectionAckTimeout);
            if (result.error || job.message != WM_MOUSEMOVE || result.duration >= 16)
                Record(DiagnosticEvent::Send, { int64_t(job.sequence), job.message, job.count, result.sent, result.error, result.apiError,
                    int64_t(result.duration), int64_t(GetTickCount64() - job.tick) }, result.error != 0);
            if (result.error) m_core.InjectionFailed(result.error, job.site);
            else if (!job.recovery) m_core.InjectionCompleted();
        }
        else if (!m_sendTimedOut && m_sendChannel.Expired(GetTickCount64()))
        {
            Record(DiagnosticEvent::Send, { int64_t(job.sequence), job.message, ERROR_TIMEOUT, int64_t(GetTickCount64() - job.tick) }, true);
            m_sendTimedOut = true;
            m_watchdog.Cancel(ProxyCancelReason::InjectionAckTimeout);
            m_core.InjectionFailed(ERROR_TIMEOUT, job.site);
            // Never clear a running slot. The hook keeps dispatching, discards
            // cancelled normal input, and permits the guardian's recovery tag.
        }
    }
    void MouseProxyEngine::EventDequeued(uint64_t qpc) noexcept
    { if(qpc && Runtime().diagnostics) m_queueLatency.Add(QpcMicros(QpcNow()-qpc)); }

    bool MouseProxyEngine::InputSettled()
    {
        return !m_sendChannel.Busy();
    }
    bool MouseProxyEngine::RecoveryCompletedExternally()
    {
        auto shared = m_watchdog.Shared();
        return shared && SharedRead(&shared->cancelled) && !SharedRead(&shared->active);
    }
    void MouseProxyEngine::RequestSurfaces(uint64_t number, bool transparent)
    {
        { std::lock_guard lock(m_uiMutex); m_ui.surfaces = ProxySurfaceRequest{ number, transparent }; }
        NotifyUi();
    }
    void MouseProxyEngine::RecoveryState(bool active, POINT virtualPoint, POINT sourcePoint, uint32_t buttons)
    {
        m_watchdog.SetRecovery(active, virtualPoint, sourcePoint, buttons);
    }
    void MouseProxyEngine::RecoveryFailed()
    {
        m_watchdog.Cancel(ProxyCancelReason::RecoveryFailed);
    }
    void MouseProxyEngine::Publish(CursorSnapshot const& value)
    {
        if (m_released && m_pendingDisable && m_core.InputReleased())
        {
            m_completedDisable = m_pendingDisable; m_pendingDisable = 0;
            SetEvent(m_released);
        }
        auto snapshot = value;
        snapshot.cancelReason = m_watchdog.CancellationReason();
        if (snapshot.visible)
        {
            if (snapshot.phase == ProxyPhase::Restoring)
            {
                // This pointer is returning to our chrome/desktop, not hovering
                // the source's last text field or auto-hidden video cursor.
                snapshot.shape = LoadCursorW(nullptr, IDC_ARROW);
            }
            else
            {
                auto now = GetTickCount64();
                if (!m_cursorShapeTick || now - m_cursorShapeTick >= 16)
                {
                    CURSORINFO cursor{ sizeof(cursor) };
                    if (GetCursorInfo(&cursor)) m_cursorShape = cursor.hCursor;
                    m_cursorShapeTick = now;
                }
                snapshot.shape = m_cursorShape ? m_cursorShape : LoadCursorW(nullptr, IDC_ARROW);
            }
        }
        bool changed;
        {
            std::lock_guard lock(m_uiMutex);
            auto const& old = m_ui.cursor;
            if (old.generation != snapshot.generation || old.phase != snapshot.phase || old.error != snapshot.error ||
                old.secondaryError != snapshot.secondaryError || old.cancelReason != snapshot.cancelReason)
                Record(DiagnosticEvent::State, { int64_t(snapshot.phase), snapshot.error, int64_t(snapshot.errorSite),
                    snapshot.secondaryError, int64_t(snapshot.secondarySite), int64_t(snapshot.cancelReason), snapshot.visible },
                    snapshot.error != 0, snapshot.generation, snapshot.lensId);
            changed = old.generation != snapshot.generation || old.phase != snapshot.phase || old.visible != snapshot.visible ||
                old.shape != snapshot.shape || old.position.x != snapshot.position.x || old.position.y != snapshot.position.y || old.error != snapshot.error ||
                old.secondaryError != snapshot.secondaryError || old.cancelReason != snapshot.cancelReason ||
                old.interactionSequence != snapshot.interactionSequence || snapshot.localMoveRequested;
            bool pendingLocalMove = old.localMoveRequested &&
                old.generation == snapshot.generation && old.lensId == snapshot.lensId;
            snapshot.localMoveRequested = snapshot.localMoveRequested || pendingLocalMove;
            m_ui.cursor = snapshot;
        }
        if (changed) NotifyUi();
    }
}
