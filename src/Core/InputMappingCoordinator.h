#pragma once
#include "IInputMappingEngine.h"
#include "InputMappingLifecycle.h"
#include "MappingGeometryGate.h"
#include <functional>
#include <memory>

namespace RegionLens::native
{
    struct InputMappingDependencies
    {
        std::function<std::unique_ptr<IInputMappingEngine>(HWND)> createEngine;
        std::function<bool()> hasUiAccess;
    };
    class InputMappingCoordinator
    {
    public:
        using NotificationCallback = std::function<void(std::wstring const&, std::wstring const&, DWORD)>;
        using SurfaceCallback = std::function<bool(bool)>;
        using CursorCallback = std::function<void(CursorSnapshot const&)>;
        using ExternalCursorCallback = std::function<void(POINT)>;
        static constexpr UINT LifecycleIdleMessage = WM_APP + 12;
        static constexpr UINT_PTR RefreshTimerId = 3;
        InputMappingCoordinator(HWND window, NotificationCallback notify,
            InputMappingDependencies dependencies = {});
        ~InputMappingCoordinator();
        bool Initialize();
        void Shutdown();
        void SetUiCallbacks(SurfaceCallback surfaces, CursorCallback cursor, ExternalCursorCallback external = {});
        bool BeginSoftwareCursor();
        bool EndSoftwareCursor();
        bool SoftwareCursorRestoreConfirmed() const noexcept
        { return !m_softwareCursorRequested && !m_softwareCursorEndFailed; }
        void SoftwareCursorHeartbeat() noexcept;
        bool ReportSoftwareCursorFailure(DWORD error);
        MappingRouteResult Route(MappingSessionConfig config);
        bool Update(MappingSessionConfig config);
        bool ReleaseRoute();
        bool DeactivateAll();
        void RefreshFromCursor() { PumpInputUpdates(); }
        void PumpInputUpdates();
        bool Available() const noexcept { return m_hasUiAccess; }
        bool HasRoute() const noexcept { return m_routedLensId != 0; }
        uint64_t RoutedLensId() const noexcept { return m_routedLensId; }
        uint64_t SessionGeneration() const noexcept { return m_generation; }
        ProxyPhase Phase() const noexcept { return m_phase; }
        bool RoutePending() const noexcept { return m_lifecycle.Busy() || m_routePending; }
        bool LifecycleBusy() const noexcept { return m_lifecycle.Busy(); }
        bool InputRuntimeHealthy() const noexcept { return !m_engine || m_engine->GuardHealthy(); }
        void NotifyWhenIdle() noexcept { m_idleNotificationRequested = true; }
        bool TargetsLens(uint64_t id) const noexcept { return id && (id == m_startingLensId || id == m_routedLensId); }
    private:
        struct OperationScope
        {
            InputMappingCoordinator& owner;
            ~OperationScope() { owner.FinishOperation(); }
        };
        void FinishOperation();
        void ResetRoute();
        void PumpInputUpdatesCore();
        bool EnsureEngineStarted();
        HWND m_window{};
        NotificationCallback m_notify;
        SurfaceCallback m_surfaces;
        CursorCallback m_cursor;
        ExternalCursorCallback m_externalCursor;
        InputMappingDependencies m_dependencies;
        std::unique_ptr<IInputMappingEngine> m_engine;
        InputMappingLifecycle m_lifecycle;
        uint64_t m_startingLensId{}, m_deferredRoutes{};
        std::optional<MappingSessionConfig> m_deferredGeometry;
        uint64_t m_generation{}, m_routedLensId{};
        MappingGeometryGate m_geometryGate;
        ProxyPhase m_phase{ ProxyPhase::Off };
        bool m_routePending{};
        bool m_hasUiAccess{};
        bool m_softwareCursorRequested{};
        bool m_softwareCursorEndFailed{};
        bool m_idleNotificationRequested{};
        DWORD m_reportedError{};
    };
}
