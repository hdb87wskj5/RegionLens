#pragma once
#include "Localization.h"
#include <windows.h>
#include <string>

namespace RegionLens::native
{
    enum class ProxyStartupStage : LONG
    {
        None, RecoveryState, PipeSecurity, PipeCreate, ShellLaunch, ProcessIdentity,
        PipeConnect, HandleTransfer, GuardReady, GuardExit, GuardUiAccess,
        GuardMagnification, GuardDesktop, InputEvents = 14, InputThread,
        InputMagnification, InputTransform, InputDesktop, InputCursor, InputHook, InputCookie
    };
    struct ProxyStartupFailure
    {
        ProxyStartupStage stage{};
        DWORD error{};
    };
    inline DWORD StartupError(DWORD fallback = ERROR_GEN_FAILURE) noexcept
    {
        auto error = GetLastError();
        return error ? error : fallback;
    }
    inline wchar_t const* StartupStageName(ProxyStartupStage stage) noexcept
    {
        switch (stage)
        {
        case ProxyStartupStage::None: return L"ready";
        case ProxyStartupStage::RecoveryState: return L"recovery-state";
        case ProxyStartupStage::PipeSecurity: return L"bootstrap-security";
        case ProxyStartupStage::PipeCreate: return L"bootstrap-pipe";
        case ProxyStartupStage::ShellLaunch: return L"watchdog-shell-launch";
        case ProxyStartupStage::ProcessIdentity: return L"watchdog-identity";
        case ProxyStartupStage::PipeConnect: return L"bootstrap-connect";
        case ProxyStartupStage::HandleTransfer: return L"bootstrap-handles";
        case ProxyStartupStage::GuardReady: return L"watchdog-ready";
        case ProxyStartupStage::GuardExit: return L"watchdog-exit";
        case ProxyStartupStage::GuardUiAccess: return L"watchdog-uiaccess";
        case ProxyStartupStage::GuardMagnification: return L"watchdog-magnification";
        case ProxyStartupStage::GuardDesktop: return L"watchdog-desktop";
        case ProxyStartupStage::InputEvents: return L"input-events";
        case ProxyStartupStage::InputThread: return L"input-thread";
        case ProxyStartupStage::InputMagnification: return L"input-magnification";
        case ProxyStartupStage::InputTransform: return L"input-transform";
        case ProxyStartupStage::InputDesktop: return L"input-desktop";
        case ProxyStartupStage::InputCursor: return L"input-cursor";
        case ProxyStartupStage::InputHook: return L"input-hook";
        case ProxyStartupStage::InputCookie: return L"input-cookie";
        default: return L"unknown";
        }
    }
    inline std::wstring StartupFailureMessage(ProxyStartupFailure failure)
    {
        std::wstring message = Localized(L"启动失败：", L"Startup failed: ") +
            std::wstring(StartupStageName(failure.stage)) + Localized(L"（错误 ", L" (error ") +
            std::to_wstring(failure.error) + Localized(L"）。", L"). ");
        if (failure.stage == ProxyStartupStage::GuardUiAccess || failure.error == ERROR_ELEVATION_REQUIRED)
            message += Localized(L"请退出旧版并重新安装已签名的 Program Files 版本。",
                L"Exit the old build and reinstall the signed Program Files version.");
        else if (failure.stage == ProxyStartupStage::InputTransform)
            message += Localized(L"请关闭其他正在使用系统输入变换的放大工具。",
                L"Close other magnifiers that are using the system input transform.");
        else
            message += Localized(L"请重新启动 RegionLens；若问题持续，请记录上述阶段和错误码。",
                L"Restart RegionLens. If the issue persists, record the stage and error code shown above.");
        return message;
    }
}
