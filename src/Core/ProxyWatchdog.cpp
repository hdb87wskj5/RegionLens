#include "pch.h"
#include "ProxyWatchdog.h"
#include "MouseProxyCore.h"
#include "WatchdogBootstrap.h"
#include "DiagnosticScope.h"
#include "CursorObservationNative.h"
#include <bcrypt.h>
#include <string>
#include <new>

namespace RegionLens::native
{
    namespace
    {
        void CloseOwned(HANDLE& handle) { if (handle) CloseHandle(std::exchange(handle, nullptr)); }
        bool OnOriginalDesktop(std::wstring const& expected)
        {
            if (expected.empty()) return false;
            auto desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
            if (!desktop) return false;
            wchar_t name[256]{}; DWORD bytes{};
            bool read = GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &bytes) != FALSE;
            CloseDesktop(desktop);
            return read && expected == name;
        }
        void RecoverInput(ProxyRecoveryShared* state, std::wstring const& desktopName)
        {
            if (!SharedRead(&state->active)) return;
            // Do not try input injection on the lock/UAC desktop, even if a
            // particular access token can open it. Wait for our original desktop.
            if (!OnOriginalDesktop(desktopName)) return;
            POINT source = UnpackProxyPoint(SharedRead64(&state->sourcePoint));
            POINT destination = UnpackProxyPoint(SharedRead64(&state->virtualPoint));
            auto buttons = SharedRead(&state->buttons);
            RECT desktop{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN), 0, 0 };
            desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
            desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
            auto batch = MakeProxyRecoveryInputs(source, destination, uint32_t(buttons), desktop, state->cookie);
            // Retry on the ordinary input desktop if recovery coincides with a
            // secure-desktop transition; never switch into the secure desktop.
            auto sent = SendInput(batch.count, batch.inputs.data(), sizeof(INPUT));
            Record(DiagnosticEvent::CursorRecovery, { sent, batch.count, buttons }, true);
            if (batch.count && sent == batch.count)
            {
                InterlockedExchange(&state->buttons, 0);
                InterlockedExchange(&state->active, 0);
            }
        }
    }

    ProxyWatchdog::~ProxyWatchdog() { Stop(); ReleaseResources(); }
    bool ProxyWatchdog::Start()
    {
        if (!Stop())
        {
            m_failure = { ProxyStartupStage::GuardExit, ERROR_BUSY };
            return false; // Never run a new mapping beside an old recovering helper.
        }
        m_failure = {};
        auto fail = [this](ProxyStartupStage stage, DWORD error)
        {
            m_failure = { stage, error }; Record(DiagnosticEvent::StartupFailure, { LONG(stage), error }, true); Stop(); return false;
        };
        // These handles are never inherited by unrelated child processes.
        m_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(ProxyRecoveryShared), nullptr);
        if (!m_mapping) return fail(ProxyStartupStage::RecoveryState, StartupError());
        m_parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId());
        if (!m_parent) return fail(ProxyStartupStage::RecoveryState, StartupError());
        m_quit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_quit) return fail(ProxyStartupStage::RecoveryState, StartupError());
        m_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!m_ready) return fail(ProxyStartupStage::RecoveryState, StartupError());
        m_shared = static_cast<ProxyRecoveryShared*>(MapViewOfFile(m_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ProxyRecoveryShared)));
        if (!m_shared) return fail(ProxyStartupStage::RecoveryState, StartupError());
        new (m_shared) ProxyRecoveryShared{};
        m_shared->parentId = GetCurrentProcessId();
        // Generate once for this guardian lifetime. Normal input and guardian
        // recovery read the same zero-extended value from anonymous shared state.
        if (!CreateProxyInputCookie(m_shared->cookie, [](uint32_t& candidate)
            {
                return BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&candidate), sizeof(candidate),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
            })) return fail(ProxyStartupStage::InputCookie, ERROR_GEN_FAILURE);
        Heartbeat();
        if (!LaunchWatchdogBootstrap({ m_mapping, m_parent, m_quit, m_ready, Runtime().runtimeLease }, m_process, m_failure))
        { Stop(); return false; }
        if (SharedRead(&m_shared->ready) != 1)
        {
            auto stage = static_cast<ProxyStartupStage>(SharedRead(&m_shared->startupStage));
            auto error = DWORD(SharedRead(&m_shared->startupError));
            return fail(stage == ProxyStartupStage::None ? ProxyStartupStage::GuardReady : stage,
                error ? error : ERROR_INVALID_DATA);
        }
        if (!Healthy()) return fail(ProxyStartupStage::GuardReady, ERROR_PROCESS_ABORTED);
        return true;
    }
    bool ProxyWatchdog::Stop()
    {
        DiagnosticScope stopping(DiagnosticStage::GuardStop, m_process ? GetProcessId(m_process) : 0);
        if (m_quit) SetEvent(m_quit);
        auto wait = m_process ? WaitForSingleObject(m_process, 1500) : WAIT_OBJECT_0;
        auto error = GetLastError();
        RecordProcessExit(m_process, false, wait);
        stopping.End(wait == WAIT_FAILED ? error : wait); SetLastError(error);
        if (!GuardMayReleaseResources(m_process != nullptr, wait)) return false;
        ReleaseResources();
        return true;
    }
    void ProxyWatchdog::ReleaseResources()
    {
        CloseOwned(m_process);
        if (m_shared) { UnmapViewOfFile(m_shared); m_shared = nullptr; }
        CloseOwned(m_mapping); CloseOwned(m_parent); CloseOwned(m_quit); CloseOwned(m_ready);
    }
    bool ProxyWatchdog::Healthy() const noexcept
    {
        return m_shared && m_process && SharedRead(&m_shared->ready) == 1 &&
            !SharedRead(&m_shared->cancelled) && WaitForSingleObject(m_process, 0) == WAIT_TIMEOUT;
    }
    void ProxyWatchdog::Heartbeat() noexcept
    {
        if (m_shared) InterlockedExchange64(&m_shared->heartbeat, static_cast<LONG64>(GetTickCount64()));
    }
    void ProxyWatchdog::Cancel(ProxyCancelReason reason) noexcept
    { if (m_shared && SetProxyCancellation(&m_shared->cancelled, reason)) Record(DiagnosticEvent::Guard, { LONG(reason) }, true); }
    ProxyCancelReason ProxyWatchdog::CancellationReason() const noexcept
    { return m_shared ? static_cast<ProxyCancelReason>(SharedRead(&m_shared->cancelled)) : ProxyCancelReason::None; }
    ProxyGuardStatus ProxyWatchdog::Status() const noexcept
    {
        ProxyGuardStatus status;
        if (m_process)
        {
            status.pid = GetProcessId(m_process);
            status.waitResult = WaitForSingleObject(m_process, 0);
            if (status.waitResult == WAIT_FAILED) status.queryError = GetLastError();
            if (status.waitResult == WAIT_OBJECT_0 && !GetExitCodeProcess(m_process, &status.exitCode)) status.queryError = GetLastError();
        }
        if (m_shared)
        {
            status.ready = SharedRead(&m_shared->ready); status.active = SharedRead(&m_shared->active);
            status.reason = CancellationReason();
            auto now = GetTickCount64(), heartbeat = uint64_t(SharedRead64(&m_shared->heartbeat));
            status.heartbeatAge = now >= heartbeat ? now - heartbeat : 0;
        }
        return status;
    }
    void ProxyWatchdog::SetRecovery(bool active, POINT virtualPoint, POINT sourcePoint, uint32_t buttons) noexcept
    {
        if (!m_shared) return;
        InterlockedExchange64(&m_shared->virtualPoint, PackProxyPoint(virtualPoint));
        InterlockedExchange64(&m_shared->sourcePoint, PackProxyPoint(sourcePoint));
        InterlockedExchange(&m_shared->buttons, LONG(buttons));
        InterlockedExchange(&m_shared->active, active ? 1 : 0);
    }

    int RunProxyWatchdog(int argc, wchar_t** argv)
    {
        BootstrapHandles handles{};
        ProxyStartupFailure failure{};
        if (!ReceiveWatchdogBootstrap(argc, argv, handles, failure)) { Record(DiagnosticEvent::StartupFailure, { LONG(failure.stage), failure.error }, true); return int(failure.error); }
        DiagnosticScope initialization(DiagnosticStage::GuardInit, GetProcessId(handles[1]));
        auto state = static_cast<ProxyRecoveryShared*>(MapViewOfFile(handles[0], FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ProxyRecoveryShared)));
        if (!state)
        { auto error = StartupError(); CloseBootstrapHandles(handles); return int(error); }
        if (state->magic != 0x524C5037 || state->parentId != GetProcessId(handles[1]) ||
            state->parentId == GetCurrentProcessId() || !ValidProxyInputCookie(state->cookie))
        { UnmapViewOfFile(state); CloseBootstrapHandles(handles); return ERROR_INVALID_DATA; }
        MSG message{}; PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
        wchar_t desktop[256]{}; DWORD bytes{};
        bool desktopRead = GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &bytes) != FALSE;
        std::wstring desktopName = desktop;
        HANDLE token{}; DWORD access{}, returned{};
        SetLastError(ERROR_SUCCESS);
        bool tokenRead = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
            GetTokenInformation(token, TokenUIAccess, &access, sizeof(access), &returned);
        auto tokenError = tokenRead ? ERROR_ACCESS_DENIED : StartupError();
        if (token) CloseHandle(token);
        bool initialized{}, ready{};
        if (!tokenRead || !access) failure = { ProxyStartupStage::GuardUiAccess, tokenError };
        else
        {
            SetLastError(ERROR_SUCCESS);
            initialized = MagInitialize() != FALSE;
            if (!initialized) failure = { ProxyStartupStage::GuardMagnification, StartupError() };
            else if (!desktopRead || !OnOriginalDesktop(desktopName)) failure = { ProxyStartupStage::GuardDesktop, ERROR_ACCESS_DENIED };
            else ready = true;
        }
        InterlockedExchange(&state->startupStage, LONG(failure.stage));
        InterlockedExchange(&state->startupError, LONG(failure.error));
        InterlockedExchange(&state->ready, ready ? 1 : -1);
        Record(DiagnosticEvent::Guard, { ready, LONG(failure.stage), failure.error }, !ready);
        SetEvent(handles[3]);
        initialization.End(failure.error);
        CursorRecoveryPolicy cursorRecovery;
        LONG64 observedLease{};
        bool exitObserved{};
        bool parentExitLogged{}, stopLogged{};
        uint64_t stopStarted{};
        auto showCursor = [&](unsigned step)
        {
            auto before = Runtime().diagnostics ? ObserveSystemCursor() : CursorObservation{};
            SetLastError(ERROR_SUCCESS);
            bool shown = MagShowSystemCursor(TRUE) != FALSE;
            auto error = shown ? ERROR_SUCCESS : GetLastError();
            cursorRecovery.ShowResult(cursorRecovery.Epoch(), shown);
            if (Runtime().diagnostics)
                RecordCursorVisibility(1, step, cursorRecovery.Epoch(), shown, error, before, ObserveSystemCursor());
        };
        bool stop = !ready;
        while (!stop)
        {
            HANDLE waits[] = { handles[1], handles[2] };
            auto result = MsgWaitForMultipleObjects(2, waits, FALSE, 50, QS_ALLINPUT);
            bool exiting = result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1;
            if (result == WAIT_OBJECT_0 && !parentExitLogged)
            { RecordProcessExit(handles[1], true, WAIT_OBJECT_0); parentExitLogged = true; }
            if ((exiting || result == WAIT_FAILED) && !stopLogged)
            {
                stopStarted = GetTickCount64();
                Record(DiagnosticEvent::Checkpoint, { int64_t(DiagnosticStage::GuardStop), 0, result,
                    GetCurrentProcessId(), GetProcessId(handles[1]),
                    0, int64_t(GetTickCount64() - uint64_t(SharedRead64(&state->heartbeat))),
                    SharedRead(&state->active) }, result != WAIT_OBJECT_0 + 1);
                stopLogged = true;
            }
            // Keep the guardian's message queue serviced for magnification.
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
            auto reason = ProxyWatchdogCancelReason(result != WAIT_OBJECT_0, result == WAIT_OBJECT_0 + 1,
                SharedRead(&state->active) != 0 || CursorLeasePending(SharedRead64(&state->cursorLease)),
                GetTickCount64(), uint64_t(SharedRead64(&state->heartbeat)));
            SetProxyCancellation(&state->cancelled, reason);
            auto now = GetTickCount64();
            if (SharedRead(&state->cancelled) && OnOriginalDesktop(desktopName))
            {
                RecoverInput(state, desktopName);
                auto lease = SharedRead64(&state->cursorLease);
                bool forceShow = exiting && !exitObserved;
                if (GuardNeedsCursorRecovery(lease, observedLease, forceShow,
                    cursorRecovery.Checking(), cursorRecovery.ShowSucceeded()))
                {
                    observedLease = lease; exitObserved = exiting;
                    cursorRecovery.AdoptRestore(now);
                    showCursor(0); // No active input required; no position/UP packet here.
                }
                if (cursorRecovery.Due(now))
                {
                    auto check = cursorRecovery.Next(now, ObserveSystemCursor());
                    if (cursorRecovery.Current(check))
                    {
                        if (check.show) showCursor(check.step);
                        if (check.last && cursorRecovery.ShowSucceeded())
                            CompleteCursorLease(&state->cursorLease, observedLease);
                    }
                }
            }
            // If the secure desktop temporarily rejects recovery, keep the helper
            // alive until the ordinary desktop accepts the UPs/show-cursor again.
            if (exiting && GuardRecoveryComplete(SharedRead(&state->active) != 0,
                SharedRead64(&state->cursorLease), cursorRecovery.Unfinished())) stop = true;
            else if (exiting) Sleep(50);
        }
        Record(DiagnosticEvent::Checkpoint, { int64_t(DiagnosticStage::GuardStop), 1,
            0, GetCurrentProcessId(), GetProcessId(handles[1]),
            stopStarted ? int64_t(GetTickCount64() - stopStarted) : 0, LONG(SharedRead(&state->cancelled)) });
        if (initialized) MagUninitialize();
        UnmapViewOfFile(state);
        CloseBootstrapHandles(handles);
        return ready ? 0 : int(failure.error);
    }
}
