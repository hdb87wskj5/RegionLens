#pragma once
#include <windows.h>
#include <cstdint>
#include "ProxyStartup.h"
#include "ProxyStatus.h"
#include "ProxyInputCookie.h"
#include "CursorRecoveryPolicy.h"

namespace RegionLens::native
{
    struct alignas(8) ProxyRecoveryShared
    {
        DWORD magic{ 0x524C5037 }, parentId{}; // v7 guardian shared-state layout.
        ULONG_PTR cookie{};
        volatile LONG ready{}, cancelled{}, active{}, buttons{};
        volatile LONG startupStage{}, startupError{};
        volatile LONG64 heartbeat{}, virtualPoint{}, sourcePoint{};
        volatile LONG64 cursorLease{}; // epoch << 1 | pending; nonzero also preserves hide history.
    };
    inline LONG64 PackProxyPoint(POINT p) noexcept
    {
        return static_cast<LONG64>((uint64_t(uint32_t(p.y)) << 32) | uint32_t(p.x));
    }
    inline POINT UnpackProxyPoint(LONG64 p) noexcept
    {
        return { LONG(uint32_t(p)), LONG(uint32_t(uint64_t(p) >> 32)) };
    }
    inline LONG SharedRead(volatile LONG* p) noexcept { return InterlockedCompareExchange(p, 0, 0); }
    inline LONG64 SharedRead64(volatile LONG64* p) noexcept { return InterlockedCompareExchange64(p, 0, 0); }
    inline bool ProxyHeartbeatExpired(uint64_t now, uint64_t heartbeat) noexcept
    {
        return now > heartbeat && now - heartbeat > 1000;
    }
    inline bool ProxyShouldCancel(bool parentAlive, bool shutdown, bool alreadyCancelled, bool active,
        uint64_t now, uint64_t heartbeat) noexcept
    {
        return !parentAlive || shutdown || alreadyCancelled || (active && ProxyHeartbeatExpired(now, heartbeat));
    }
    inline ProxyCancelReason ProxyWatchdogCancelReason(bool parentAlive, bool shutdown, bool active,
        uint64_t now, uint64_t heartbeat) noexcept
    {
        if (!parentAlive) return ProxyCancelReason::ParentExit;
        if (shutdown) return ProxyCancelReason::WatchdogShutdown;
        if (active && ProxyHeartbeatExpired(now, heartbeat)) return ProxyCancelReason::HeartbeatTimeout;
        return ProxyCancelReason::None;
    }
    struct ProxyGuardStatus
    {
        DWORD pid{}, waitResult{ WAIT_FAILED }, exitCode{}, queryError{};
        LONG ready{}, active{};
        ProxyCancelReason reason{};
        uint64_t heartbeatAge{};
    };
    // Hidden instance of the same signed binary, activated by the Windows shell.
    // Recovery state stays anonymous; authenticated one-shot bootstrap only.
    class ProxyWatchdog
    {
    public:
        ~ProxyWatchdog();
        bool Start();
        bool Stop(); // Retain the old helper until its recovery has actually ended.
        bool Healthy() const noexcept;
        ProxyStartupFailure LastFailure() const noexcept { return m_failure; }
        void Heartbeat() noexcept;
        void Cancel(ProxyCancelReason reason) noexcept;
        ProxyCancelReason CancellationReason() const noexcept;
        ProxyGuardStatus Status() const noexcept;
        void SetRecovery(bool active, POINT virtualPoint, POINT sourcePoint, uint32_t buttons) noexcept;
        ProxyRecoveryShared* Shared() const noexcept { return m_shared; }
        ULONG_PTR Cookie() const noexcept { return m_shared ? m_shared->cookie : 0; }
    private:
        void ReleaseResources();
        HANDLE m_mapping{}, m_parent{}, m_quit{}, m_ready{}, m_process{};
        ProxyRecoveryShared* m_shared{};
        ProxyStartupFailure m_failure{};
    };
    int RunProxyWatchdog(int argc, wchar_t** argv);
}
