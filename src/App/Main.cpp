#include "pch.h"
#include "ProductBuild.h"
#include "AppController.h"
#include "AppRuntime.h"
#include "RuntimeLease.h"
#include "DiagnosticScope.h"
#include "Localization.h"
#include "ProxyWatchdog.h"
#include "../WeTypeProbe/WeTypeProbeHost.h"
#include <tlhelp32.h>

namespace
{
    struct Handle { HANDLE value{}; ~Handle() { if (value) CloseHandle(value); } };
    bool LegacyProcessRunning()
    {
        DWORD ownSession{}; ProcessIdToSessionId(GetCurrentProcessId(), &ownSession);
        auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot == INVALID_HANDLE_VALUE) return true;
        PROCESSENTRY32W entry{ sizeof(entry) };
        bool found{};
        for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry))
        {
            if (entry.th32ProcessID == GetCurrentProcessId() ||
                (_wcsicmp(entry.szExeFile, L"RegionLens.exe") && _wcsicmp(entry.szExeFile, L"RegionLens-Dev.exe"))) continue;
            DWORD session{};
            if (ProcessIdToSessionId(entry.th32ProcessID, &session) && session == ownSession)
            {
                // Updated channels arbitrate atomically using RuntimeLease; do
                // not reject the other contender while it is exiting startup.
                Handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID) };
                wchar_t path[32768]{}; DWORD pathSize = ARRAYSIZE(path);
                bool unified{};
                if (process.value && QueryFullProcessImageNameW(process.value, 0, path, &pathSize))
                {
                    DWORD ignored{}, bytes = GetFileVersionInfoSizeW(path, &ignored);
                    std::vector<BYTE> data(bytes);
                    if (bytes && GetFileVersionInfoW(path, 0, bytes, data.data()))
                    {
                        wchar_t* marker{}; UINT length{};
                        unified = VerQueryValueW(data.data(), L"\\StringFileInfo\\080404B0\\RuntimeProtocol",
                            reinterpret_cast<void**>(&marker), &length) && marker && length && !wcscmp(marker, L"1");
                    }
                }
                if (!unified) { found = true; break; }
            }
        }
        CloseHandle(snapshot); return found;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    using namespace RegionLens::native;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int count{};
    auto arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    bool watchdog = arguments && count > 1 && wcscmp(arguments[1], L"--input-watchdog") == 0;
    bool quietStartup = arguments && count == 2 && wcscmp(arguments[1], L"--startup") == 0;
    AppRuntimeConfig config;
    config.version = RL_VERSION; config.commit = RL_COMMIT;
    config.persistentSoftwareCursor = true;
    auto weTypeProbe = std::make_unique<RegionLens::weTypeCompat::WeTypeProbeHost>();
    config.weTypeProbe = weTypeProbe.get();
    SetRuntime(config);
    auto run = [&]() -> int
    {
        DiagnosticScope entry(DiagnosticStage::Entry);
        LoadAppLanguagePreference();
        Record(DiagnosticEvent::Startup, { watchdog, quietStartup });
        if (watchdog)
        {
            int result = RunProxyWatchdog(count, arguments);
            LocalFree(arguments); entry.End(DWORD(result)); Record(DiagnosticEvent::Shutdown, { result }, true); return result;
        }
        if (!arguments || (count > 1 && !quietStartup))
        { if (arguments) LocalFree(arguments); return ERROR_INVALID_PARAMETER; }
        LocalFree(arguments);

        DiagnosticScope instanceCheck(DiagnosticStage::InstanceCheck);
        Handle instance{ CreateMutexW(nullptr, FALSE, config.identity->instanceName) };
        auto instanceError = GetLastError();
        if (!instance.value) return int(instanceError);
        if (instanceError == ERROR_ALREADY_EXISTS)
        {
            if (!quietStartup)
            {
                bool notified{};
                for (int i = 0; i < 20; ++i)
                {
                    if (auto window = FindWindowW(config.identity->controllerClass, nullptr))
                    { PostMessageW(window, ShowRunningNotificationMessage, 0, 0); notified = true; break; }
                    Sleep(50);
                }
                if (!notified) MessageBoxW(nullptr, L"区域镜已启动或正在恢复输入，请稍后重试。", config.identity->name, MB_OK | MB_ICONINFORMATION);
            }
            return 0;
        }
        instanceCheck.End(instanceError);
        DiagnosticScope runtimeCheck(DiagnosticStage::RuntimeCheck);
        RuntimeLease lease;
        if (!lease.Acquire(RuntimeLeaseName) || LegacyProcessRunning())
        {
            Record(DiagnosticEvent::StartupFailure, { ERROR_BUSY }, true);
            if (!quietStartup) MessageBoxW(nullptr,
                L"另一个区域镜版本或安全守护仍在运行。请从托盘正常退出，等待输入恢复完成后再启动此版本。",
                config.identity->name, MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        runtimeCheck.End();
        config.runtimeLease = lease.Handle(); SetRuntime(config);
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        int result{};
        {
            DiagnosticScope initialize(DiagnosticStage::ControllerInit);
            AppController controller;
            if (!controller.Initialize(quietStartup))
            {
                Record(DiagnosticEvent::StartupFailure, { GetLastError() }, true);
                MessageBoxW(nullptr, Localized(L"区域镜初始化失败。", L"RegionLens failed to initialize."),
                    config.identity->name, MB_OK | MB_ICONERROR);
                return EXIT_FAILURE;
            }
            initialize.End(); entry.End();
            DiagnosticScope loop(DiagnosticStage::MessageLoop);
            MSG message{}; int status{};
            while ((status = GetMessageW(&message, nullptr, 0, 0)) > 0)
            { if (!controller.ProcessMessage(message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
            result = status == -1 ? EXIT_FAILURE : int(message.wParam);
            loop.End(DWORD(result));
            Record(DiagnosticEvent::Checkpoint, { int64_t(DiagnosticStage::ControllerStop), 0 });
        }
        Record(DiagnosticEvent::Checkpoint, { int64_t(DiagnosticStage::ControllerStop), 1 });
        Record(DiagnosticEvent::Shutdown, { result }, true);
        return result;
    };
    auto result = run();
    // Destructors in run() have finished; no input thread can still publish.
    SetRuntime({});
    return result;
}
