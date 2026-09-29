#pragma once
#include "MouseProxyCore.h"
#include "IInputMappingEngine.h"
#include "ProxyWatchdog.h"
#include "ProxySendChannel.h"
#include "CursorRecoveryPolicy.h"
#include "LatencyMetrics.h"
#include <thread>
#include <mutex>
#include <deque>
#include <atomic>

namespace RegionLens::native
{
    class MouseProxyEngine final : public IInputMappingEngine, private IProxyBackend
    {
    public:
        static constexpr UINT UiMessage = WM_APP + 8;
        explicit MouseProxyEngine(HWND notificationWindow);
        ~MouseProxyEngine() override;
        bool Start() override;
        void Stop() override;
        void Configure(MappingSessionConfig config, bool probeCursor = false) override;
        uint64_t Disable();
        bool DisableAndDrain() override;
        void Acknowledge(uint64_t request, bool success) override;
        ProxyUiUpdate TakeUiUpdate() override;
        bool BeginSoftwareCursor() override;
        bool EndSoftwareCursor() override;
        void SoftwareCursorHeartbeat() noexcept override;
        bool NativeCursorShowConfirmed() const noexcept override;
        bool Running() const noexcept { return m_running; }
        bool GuardHealthy() const noexcept override { return m_running && m_watchdog.Healthy(); }
        // Only inspected by the UI after Start returns (failed worker joined).
        ProxyStartupFailure LastFailure() const noexcept override { return m_startupFailure; }
    private:
        struct Command
        {
            enum Kind { Configure, Disable, Acknowledge, BeginCursorLease, EndCursorLease } kind{};
            MappingSessionConfig config{};
            uint64_t request{};
            bool success{};
            bool probeCursor{};
        };
        void Enqueue(Command command);
        void Run();
        void SendLoop();
        void PollSend();
        void NotifyUi();
        void ServiceCursorRecovery();
        bool SwitchSoftwareCursor(bool begin);
        bool BeginPersistentCursorLease();
        bool EndPersistentCursorLease();
        void PublishExternalCursor();
        bool ShowCursorForRecovery(unsigned step);
        bool SameInputDesktop();
        static LRESULT CALLBACK Hook(int, WPARAM, LPARAM);
        static LRESULT CALLBACK KeyboardHook(int, WPARAM, LPARAM);
        bool Healthy() override;
        uint64_t NowTick() override { return GetTickCount64(); }
        bool CanEnter(MappingSessionConfig const&, POINT) override;
        bool SourceAccessible(MappingSessionConfig const&, POINT) override;
        bool DestinationExposed(MappingSessionConfig const&, POINT) override;
        bool CursorVisible(bool) override;
        bool SendMouse(POINT, UINT, DWORD, RECT) override;
        bool InputSettled() override;
        void EventDequeued(uint64_t qpc) noexcept override;
        bool RecoveryCompletedExternally() override;
        void RequestSurfaces(uint64_t, bool) override;
        void RecoveryState(bool, POINT, POINT, uint32_t) override;
        void RecoveryFailed() override;
        void Publish(CursorSnapshot const&) override;
        HWND m_notificationWindow{};
        ProxyWatchdog m_watchdog;
        MouseProxyCore m_core;
        ProxyPointerBaseline m_pointer; // Input thread only, including its hook.
        std::thread m_thread, m_sender;
        HANDLE m_sendWake{};
        std::atomic<bool> m_sendStop{};
        ProxySendChannel m_sendChannel;
        ULONG_PTR m_sendCookie{}, m_fenceCookie{};
        bool m_sendTimedOut{};
        HANDLE m_wake{}, m_ready{}, m_released{}, m_cursorLeaseDone{};
        HHOOK m_hook{}, m_keyboardHook{};
        ProxyControlState m_controlState; // Input thread; tracks only the Ctrl modifier.
        std::atomic<bool> m_running{}, m_stop{}, m_uiPending{};
        std::atomic<uint64_t> m_nextDisable{}, m_completedDisable{};
        std::atomic<uint64_t> m_nextCursorLease{}, m_completedCursorLease{};
        std::atomic<bool> m_cursorLeaseResult{};
        std::atomic<uint64_t> m_softwareCursorHeartbeat{};
        std::atomic<LONG64> m_externalPoint{};
        std::atomic<bool> m_externalPending{};
        bool m_persistentCursorLease{}; // Hook and worker share the input thread.
        bool m_overlayHeartbeatTimedOut{};
        uint64_t m_pendingDisable{}; // Worker-owned acknowledgement, not a UI flag.
        std::mutex m_commandMutex, m_uiMutex;
        std::deque<Command> m_commands;
        ProxyUiUpdate m_ui;
        CursorRecoveryPolicy m_cursorRecovery;
        uint64_t m_sendSequence{};
        uint64_t m_latencyLast{};
        LatencySamples m_queueLatency,m_senderLatency,m_apiLatency,m_ackLatency;
        uint64_t m_healthCheckTick{}, m_desktopCheckTick{}, m_cursorShapeTick{};
        HCURSOR m_cursorShape{};
        bool m_guardCheckOk{}, m_desktopCheckOk{};
        std::wstring m_desktopName;
        ProxyStartupFailure m_startupFailure{};
        static thread_local MouseProxyEngine* s_current;
    };
}
