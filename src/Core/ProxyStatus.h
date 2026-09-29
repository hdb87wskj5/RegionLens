#pragma once

#include "Localization.h"
#include <windows.h>
#include <cstdint>

namespace RegionLens::native
{
    enum class ProxyCancelReason : LONG
    {
        None, ForeignInput, InjectionAckTimeout, CommandOverflow, UiDrainTimeout,
        RecoveryFailed, WorkerShutdown, ParentExit = 8, WatchdogShutdown,
        HeartbeatTimeout, OverlayHeartbeatTimeout
    };

    inline bool SetProxyCancellation(volatile LONG* flag, ProxyCancelReason reason) noexcept
    {
        return reason != ProxyCancelReason::None && InterlockedCompareExchange(flag, LONG(reason), 0) == 0;
    }

    inline wchar_t const* ProxyCancelName(ProxyCancelReason reason) noexcept
    {
        switch (reason)
        {
        case ProxyCancelReason::None: return Localized(L"未记录取消", L"no cancellation recorded");
        case ProxyCancelReason::ForeignInput: return Localized(L"检测到其他输入工具", L"another input tool was detected");
        case ProxyCancelReason::InjectionAckTimeout: return Localized(L"输入确认超时", L"input acknowledgement timed out");
        case ProxyCancelReason::CommandOverflow: return Localized(L"输入队列已满", L"the input queue is full");
        case ProxyCancelReason::UiDrainTimeout: return Localized(L"输入停止超时", L"input shutdown timed out");
        case ProxyCancelReason::RecoveryFailed: return Localized(L"输入恢复失败", L"input recovery failed");
        case ProxyCancelReason::WorkerShutdown: return Localized(L"输入线程已停止", L"the input worker stopped");
        case ProxyCancelReason::ParentExit: return Localized(L"主程序已退出", L"the main application exited");
        case ProxyCancelReason::WatchdogShutdown: return Localized(L"安全守护已停止", L"the safety watchdog stopped");
        case ProxyCancelReason::HeartbeatTimeout: return Localized(L"输入线程失去响应", L"the input worker stopped responding");
        case ProxyCancelReason::OverlayHeartbeatTimeout: return Localized(L"软件光标停止响应", L"the software pointer stopped responding");
        default: return Localized(L"未知取消原因", L"unknown cancellation reason");
        }
    }

    enum class ProxyFaultSite : uint32_t
    {
        None, Configuration, InitialHealth, QueueOverflow, HealthCheck, PrepareTimeout,
        SurfacePrepare, CursorHide, InitialSend, ActiveSend, SurfaceRestore, ReleaseInput, CursorRestore
    };

    inline wchar_t const* ProxyFaultName(ProxyFaultSite site) noexcept
    {
        switch (site)
        {
        case ProxyFaultSite::Configuration: return Localized(L"区域配置", L"region configuration");
        case ProxyFaultSite::InitialHealth: return Localized(L"初始健康检查", L"initial health check");
        case ProxyFaultSite::QueueOverflow: return Localized(L"鼠标事件队列", L"mouse event queue");
        case ProxyFaultSite::HealthCheck: return Localized(L"运行健康检查", L"runtime health check");
        case ProxyFaultSite::PrepareTimeout: return Localized(L"窗口穿透准备超时", L"window pass-through preparation timeout");
        case ProxyFaultSite::SurfacePrepare: return Localized(L"窗口穿透确认", L"window pass-through acknowledgement");
        case ProxyFaultSite::CursorHide: return Localized(L"隐藏系统光标", L"hide system pointer");
        case ProxyFaultSite::InitialSend: return Localized(L"首次源位置移动", L"initial source-position move");
        case ProxyFaultSite::ActiveSend: return Localized(L"映射输入发送", L"mapped input send");
        case ProxyFaultSite::SurfaceRestore: return Localized(L"窗口恢复确认", L"window restore acknowledgement");
        case ProxyFaultSite::ReleaseInput: return Localized(L"释放输入", L"release input");
        case ProxyFaultSite::CursorRestore: return Localized(L"恢复系统光标", L"restore system pointer");
        default: return Localized(L"未指定阶段", L"unspecified stage");
        }
    }

    struct ProxyFailureHistory
    {
        DWORD first{}, secondary{};
        ProxyFaultSite firstSite{}, secondarySite{};

        bool Note(DWORD error, ProxyFaultSite site) noexcept
        {
            if (!error) return false;
            if (!first) { first = error; firstSite = site; return true; }
            if (error == first && site == firstSite) return false;
            if (!secondary) { secondary = error; secondarySite = site; return true; }
            return false;
        }
    };
}
