#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <string>
#include <span>

namespace RegionLens::native
{
    enum class AppChannel { Stable, Dev };
    struct AppIdentity
    {
        AppChannel channel;
        wchar_t const* name;
        wchar_t const* folder;
        wchar_t const* executable;
        wchar_t const* setupExecutable;
        wchar_t const* registry;
        wchar_t const* hotkeys;
        wchar_t const* controllerClass;
        wchar_t const* instanceName;
        wchar_t const* certificateSubject;
        bool defaultAutoStart;
    };
    inline constexpr AppIdentity StableIdentity{
        AppChannel::Stable, L"区域镜 RegionLens", L"RegionLens", L"RegionLens.exe", L"RegionLens-Setup.exe",
        L"Software\\RegionLens", L"Software\\RegionLens\\Hotkeys", L"RegionLens.Controller",
        L"Local\\RegionLens.Application.{9C56152E-2A28-4C10-A697-749F8A53050A}", L"RegionLens Local Publisher", true };
    inline constexpr AppIdentity DevIdentity{
        AppChannel::Dev, L"区域镜开发版 RegionLens Dev", L"RegionLens-Dev", L"RegionLens-Dev.exe", L"RegionLens-Dev-Setup.exe",
        L"Software\\RegionLens-Dev", L"Software\\RegionLens-Dev\\Hotkeys", L"RegionLens.Dev.Controller",
        L"Local\\RegionLens.Dev.Application.{9C56152E-2A28-4C10-A697-749F8A53050A}", L"RegionLens Dev Local Publisher", false };
    inline std::wstring WindowClassName(AppIdentity const& identity, wchar_t const* role)
    { return std::wstring(identity.folder) + L"." + role; }
    inline constexpr wchar_t RuntimeLeaseName[] = L"Local\\RegionLens.InputRuntime.{60592129-1C95-4401-970C-016DA717FF2C}";

    enum class DiagnosticEvent : uint32_t
    {
        Startup, Shutdown, Notification, StartupFailure, Session, State, Send,
        CursorRecovery, SurfaceAck, QueueOverflow, Performance, Guard, Fault, Sample,
        Checkpoint, ProcessExit, Exception, Lifecycle, RenderResize, RenderQuality, Screenshot, WindowLayer,
        Latency, CaptureTiming, Adapter, CaptureLifecycle, UiCursor, CursorObservation
    };
    enum class DiagnosticStage : uint32_t
    {
        Entry, InstanceCheck, RuntimeCheck, ControllerInit, MessageLoop, ControllerStop,
        GuardStart, PipeCreate, ShellLaunch, ChildIdentity, PipeConnect, HandleTransfer,
        GuardReady, GuardReceive, GuardInit, GuardStop, InputStart, InputStop, InputThread,
        SenderThread, Capture, InputMagnification, InputTransform, InputHook, InputReady
    };
    struct DiagnosticRecord
    {
        uint64_t tick{}, generation{}, lens{};
        DiagnosticEvent event{};
        std::array<int64_t, 8> values{};
        bool critical{};
        DWORD thread{};
        uint64_t sequence{};
    };
    class IDiagnosticSink
    {
    public:
        virtual ~IDiagnosticSink() = default;
        virtual bool TryRecord(DiagnosticRecord const&) noexcept = 0;
        virtual std::wstring Directory() const = 0;
        virtual DWORD Error() const noexcept = 0;
    };
    enum class WeTypeProbeStopReason : int64_t
    {
        None, Timeout, AttachFailed, WindowChanged, EventOverflow, Manual, Shutdown,
        SettingsPause, HeartbeatExpired, InterceptFailed, NoTargets
    };
    enum class WeTypeProbeMode { Observe, Intercept };
    struct WeTypeProbeLens
    {
        HWND window{};
        RECT bounds{};
    };
    class IWeTypePreChangeProbe
    {
    public:
        virtual ~IWeTypePreChangeProbe() = default;
        virtual bool Start(WeTypeProbeMode mode) noexcept = 0;
        virtual void Stop(WeTypeProbeStopReason reason) noexcept = 0;
        virtual void Pump() noexcept = 0;
        virtual void UpdateTargets(std::span<WeTypeProbeLens const> targets) noexcept = 0;
        [[nodiscard]] virtual bool Enabled() const noexcept = 0;
        [[nodiscard]] virtual bool HasTargets() const noexcept = 0;
        [[nodiscard]] virtual WeTypeProbeMode Mode() const noexcept = 0;
        [[nodiscard]] virtual HWND InterceptionTarget() const noexcept = 0;
        virtual void ReportUnexpectedOrder(HWND window) noexcept = 0;
        virtual WeTypeProbeStopReason TakeStopReason() noexcept = 0;
    };
    struct AppRuntimeConfig
    {
        AppIdentity const* identity{ &StableIdentity };
        wchar_t const* version{ L"unconfigured" };
        wchar_t const* commit{ L"uncommitted" };
        IDiagnosticSink* diagnostics{};
        IWeTypePreChangeProbe* weTypeProbe{}; // Functional compatibility; optional when unavailable.
        // A single guardian-backed native hide spans all standby routes.
        bool persistentSoftwareCursor{};
        HANDLE runtimeLease{}; // Borrowed; the entry point and watchdog own their handles.
        void (*launchProbe)(HWND){};
        wchar_t const* probeLabel{};
        DWORD diagnosticStartupError{};
    };
    inline bool PersistentSoftwareCursor(AppRuntimeConfig const& config) noexcept
    {
        return config.persistentSoftwareCursor;
    }
    // Set once before starting any core threads; destroyed after all threads stop.
    AppRuntimeConfig const& Runtime() noexcept;
    void SetRuntime(AppRuntimeConfig const&) noexcept;
    inline void Record(DiagnosticEvent event, std::array<int64_t, 8> values = {},
        bool critical = false, uint64_t generation = 0, uint64_t lens = 0) noexcept
    {
        if (auto sink = Runtime().diagnostics) {
            auto savedError = GetLastError();
            sink->TryRecord({ GetTickCount64(), generation, lens, event, values, critical });
            SetLastError(savedError);
        }
    }
    std::wstring BuildIdentityText();
}
