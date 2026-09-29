#include "pch.h"
#include "InputMappingCoordinator.h"
#include "Localization.h"
#include "MouseProxyEngine.h"
#include "AppRuntime.h"

namespace RegionLens::native
{
    InputMappingCoordinator::InputMappingCoordinator(HWND window, NotificationCallback notify,
        InputMappingDependencies dependencies)
        : m_window(window), m_notify(std::move(notify)), m_dependencies(std::move(dependencies)) {}
    InputMappingCoordinator::~InputMappingCoordinator() { Shutdown(); }
    bool InputMappingCoordinator::Initialize()
    {
        if (m_dependencies.hasUiAccess) m_hasUiAccess = m_dependencies.hasUiAccess();
        else
        {
            HANDLE token{}; DWORD access{}, returned{};
            if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            {
                m_hasUiAccess = GetTokenInformation(token, TokenUIAccess, &access, sizeof(access), &returned) && access;
                CloseHandle(token);
            }
        }
        if (m_window) SetTimer(m_window, RefreshTimerId, 16, nullptr);
        return Available();
    }
    void InputMappingCoordinator::SetUiCallbacks(SurfaceCallback surfaces, CursorCallback cursor,
        ExternalCursorCallback external)
    {
        m_surfaces = std::move(surfaces); m_cursor = std::move(cursor);
        m_externalCursor = std::move(external);
    }
    bool InputMappingCoordinator::EnsureEngineStarted()
    {
        if (!m_engine) m_engine = m_dependencies.createEngine ? m_dependencies.createEngine(m_window) :
            std::make_unique<MouseProxyEngine>(m_window);
        if (!m_engine) return false;
        if (m_engine->Start()) return true;
        m_lifecycle.RequestStop();
        auto failure = m_engine->LastFailure();
        if (m_notify) m_notify(Localized(L"鼠标映射未启动", L"Mouse mapping did not start"),
            StartupFailureMessage(failure), NIIF_ERROR);
        return false;
    }
    bool InputMappingCoordinator::BeginSoftwareCursor()
    {
        if (!PersistentSoftwareCursor(Runtime())) return true;
        if (m_softwareCursorRequested) return m_engine && m_engine->GuardHealthy();
        if (!Available() || m_lifecycle.Shutdown() ||
            !m_lifecycle.Begin(MappingOperation::Starting)) return false;
        OperationScope operation{ *this };
        if (!EnsureEngineStarted()) return false;
        if (!m_engine->BeginSoftwareCursor())
        {
            m_lifecycle.RequestStop();
            if (m_notify) m_notify(Localized(L"鼠标映射未启动", L"Mouse mapping did not start"),
                Localized(L"软件光标未能安全启动。", L"The software pointer could not start safely."), NIIF_ERROR);
            return false;
        }
        m_softwareCursorEndFailed = false;
        m_softwareCursorRequested = true;
        return true;
    }
    bool InputMappingCoordinator::EndSoftwareCursor()
    {
        if (!m_softwareCursorRequested) return !m_softwareCursorEndFailed;
        if (!m_engine || m_lifecycle.Busy())
        {
            m_softwareCursorEndFailed = true;
            m_lifecycle.RequestStop();
            return false;
        }
        if (!m_engine->EndSoftwareCursor())
        {
            m_softwareCursorEndFailed = true;
            DeactivateAll();
            // Stop makes one final local restore attempt. Its result, not the
            // earlier End acknowledgement, decides whether the overlay stays.
            return !m_softwareCursorEndFailed;
        }
        m_softwareCursorRequested = false;
        m_softwareCursorEndFailed = false;
        return true;
    }
    void InputMappingCoordinator::SoftwareCursorHeartbeat() noexcept
    {
        if (m_softwareCursorRequested && m_engine) m_engine->SoftwareCursorHeartbeat();
    }
    bool InputMappingCoordinator::ReportSoftwareCursorFailure(DWORD error)
    {
        Record(DiagnosticEvent::CursorRecovery, { 3, 2, error }, true);
        ReleaseRoute();
        bool restored = EndSoftwareCursor();
        DeactivateAll();
        if (m_notify) m_notify(Localized(L"鼠标映射已停止", L"Mouse mapping stopped"),
            Localized(L"软件光标发生故障，已请求恢复系统光标。错误码：",
                L"The software pointer failed; system-pointer recovery was requested. Error: ") +
            std::to_wstring(error ? error : ERROR_GEN_FAILURE), NIIF_ERROR);
        return restored && !m_softwareCursorEndFailed;
    }
    MappingRouteResult InputMappingCoordinator::Route(MappingSessionConfig config)
    {
        if (m_lifecycle.Shutdown()) return MappingRouteResult::Cancelled;
        if (!m_lifecycle.Begin(MappingOperation::Starting)) { ++m_deferredRoutes; return MappingRouteResult::Deferred; }
        OperationScope operation{ *this };
        m_startingLensId = config.lensId;
        // The persistent hide belongs to the existing worker/watchdog pair.
        // Restarting either while the overlay remains visible can expose both
        // the native cursor and the software cursor.
        if (PersistentSoftwareCursor(Runtime()) &&
            (!m_softwareCursorRequested || !m_engine || !m_engine->GuardHealthy()))
        {
            m_lifecycle.RequestStop();
            return MappingRouteResult::Failed;
        }
        if (config.lensId == m_routedLensId) return MappingRouteResult::Routed;
        if (m_routePending || (m_routedLensId && m_phase != ProxyPhase::Armed)) return MappingRouteResult::Deferred;
        Record(DiagnosticEvent::Lifecycle, { LONG(MappingOperation::Starting), 0 }, false, m_generation, config.lensId);
        if (!Available())
        {
            if (m_notify) m_notify(Localized(L"鼠标映射不可用", L"Mouse mapping unavailable"),
                Localized(L"请使用已签名并安装在 Program Files 中的 UIAccess 版本。",
                    L"Use the signed UIAccess build installed under Program Files."), NIIF_WARNING);
            return MappingRouteResult::Failed;
        }
        if (m_lifecycle.Cancelled()) return MappingRouteResult::Cancelled;
        bool started = m_softwareCursorRequested || EnsureEngineStarted();
        if (!started) return MappingRouteResult::Failed;
        if (m_lifecycle.Cancelled()) return MappingRouteResult::Cancelled;
        if (m_deferredGeometry && m_deferredGeometry->lensId == config.lensId)
            config = *std::exchange(m_deferredGeometry, std::nullopt);
        m_routedLensId = config.lensId; config.generation = ++m_generation; m_reportedError = 0;
        m_phase = ProxyPhase::Off;
        m_routePending = true;
        m_geometryGate.Reset();
        m_engine->Configure(config, true);
        return m_lifecycle.Cancelled() ? MappingRouteResult::Cancelled : MappingRouteResult::Routed;
    }
    bool InputMappingCoordinator::Update(MappingSessionConfig config)
    {
        if (m_lifecycle.Busy())
        {
            if (TargetsLens(config.lensId) && !m_lifecycle.Cancelled()) m_deferredGeometry = config;
            return true; // Keep only the latest geometry, without interrupting another engine call.
        }
        if (!m_lifecycle.Begin(MappingOperation::Updating)) return true;
        OperationScope operation{ *this };
        if (!m_engine || !m_routedLensId || config.lensId != m_routedLensId) return false;
        auto action = m_geometryGate.Update(config.blocked);
        if (action == MappingGeometryGate::Action::Wait) return true;
        config.generation = ++m_generation;
        m_phase = ProxyPhase::Off;
        m_routePending = true;
        // Geometry callbacks may precede WM_WINDOWPOSCHANGED. Release source
        // input before the UI moves/resizes any hit-test or render surface.
        if (action != MappingGeometryGate::Action::Resume && !m_engine->DisableAndDrain())
        {
            m_lifecycle.RequestStop();
            return false;
        }
        PumpInputUpdatesCore();
        if (m_lifecycle.Cancelled() || m_deferredGeometry) return true;
        if (!m_routedLensId || !m_engine->GuardHealthy()) return false;
        // Stay truly Off during the resize, not Armed with blocked=true.
        // No repeated generation churn, stale prepare acknowledgements or
        // synchronous drains on intermediate WM_WINDOWPOS messages.
        if (action == MappingGeometryGate::Action::Pause)
        {
            if (m_cursor) m_cursor({});
            return true;
        }
        m_engine->Configure(config, true);
        return true;
    }
    bool InputMappingCoordinator::ReleaseRoute()
    {
        ResetRoute();
        if (m_lifecycle.Busy()) { m_lifecycle.RequestStop(); return true; }
        if (!m_lifecycle.Begin(MappingOperation::Releasing)) return true;
        OperationScope operation{ *this };
        if (!m_engine) return true;
        if (!m_engine->DisableAndDrain())
        {
            m_lifecycle.RequestStop();
            return false;
        }
        PumpInputUpdatesCore();
        return m_lifecycle.Cancelled() || m_engine->GuardHealthy();
    }
    bool InputMappingCoordinator::DeactivateAll()
    {
        if (m_lifecycle.Busy())
        {
            ResetRoute(); m_lifecycle.RequestStop();
            return true;
        }
        // Preserve the ordinary graceful drain before stopping the worker.
        // Reentrant cancellation instead uses Stop's own recovery after the
        // in-flight call returns; it cannot enter that call a second time.
        bool recovered = ReleaseRoute();
        m_lifecycle.RequestStop();
        m_lifecycle.Stopping();
        FinishOperation();
        return recovered;
    }
    void InputMappingCoordinator::PumpInputUpdates()
    {
        if (m_lifecycle.Busy() || m_lifecycle.Shutdown()) return;
        if (m_deferredGeometry)
        {
            auto geometry = *std::exchange(m_deferredGeometry, std::nullopt);
            if (!Update(geometry)) { DeactivateAll(); return; }
        }
        if (!m_engine || !m_lifecycle.Begin(MappingOperation::Pumping)) return;
        OperationScope operation{ *this };
        PumpInputUpdatesCore();
    }
    void InputMappingCoordinator::PumpInputUpdatesCore()
    {
        if (!m_engine || m_lifecycle.Cancelled()) return;
        auto update = m_engine->TakeUiUpdate();
        if (m_lifecycle.Cancelled()) return;
        if (update.externalCursorMoved && m_externalCursor) m_externalCursor(update.externalCursorPosition);
        if (m_lifecycle.Cancelled()) return;
        if (update.surfaces)
        {
            auto request = *update.surfaces;
            bool permit = !request.transparent || (m_routedLensId && update.cursor.generation == m_generation);
            bool success = permit && m_surfaces && m_surfaces(request.transparent);
            if (m_lifecycle.Cancelled()) return;
            m_engine->Acknowledge(request.number, success);
        }
        bool pausedFailure = m_geometryGate.Paused() && update.cursor.lensId == m_routedLensId &&
            (update.cursor.error || update.cursor.cancelReason != ProxyCancelReason::None);
        if (update.cursor.generation != m_generation && !pausedFailure) return;
        m_phase = update.cursor.phase;
        m_routePending = false;
        if (m_cursor) m_cursor(pausedFailure ? CursorSnapshot{} : update.cursor);
        if (m_lifecycle.Cancelled()) return;
        if (pausedFailure || update.cursor.phase == ProxyPhase::Failed || update.cursor.phase == ProxyPhase::Off)
        {
            m_routedLensId = 0;
            m_routePending = false;
            m_geometryGate.Reset();
            if (update.cursor.error && update.cursor.error != m_reportedError)
            {
                m_reportedError = update.cursor.error;
                auto const& cursor = update.cursor;
                Record(DiagnosticEvent::Fault, { cursor.error, int64_t(cursor.errorSite), cursor.secondaryError,
                    int64_t(cursor.secondarySite), int64_t(cursor.cancelReason) }, true, cursor.generation, cursor.lensId);
                std::wstring detail = Localized(L"首次错误 ", L"First error ") + std::to_wstring(cursor.error) +
                    Localized(L"（", L" (") + ProxyFaultName(cursor.errorSite) + Localized(L"）。", L"). ");
                if (cursor.secondaryError) detail += Localized(L"后续错误 ", L"Secondary error ") +
                    std::to_wstring(cursor.secondaryError) + Localized(L"（", L" (") +
                    ProxyFaultName(cursor.secondarySite) + Localized(L"）。", L"). ");
                if (cursor.cancelReason != ProxyCancelReason::None) detail +=
                    Localized(L"取消来源：", L"Cancellation: ") + std::wstring(ProxyCancelName(cursor.cancelReason)) +
                    Localized(L"。", L". ");
                detail += Localized(L"请重新启动 RegionLens；若问题持续出现，请反馈首次错误码与阶段。",
                    L"Restart RegionLens. If the problem continues, report the first error code and stage.");
                if (m_notify) m_notify(Localized(L"鼠标映射已停止", L"Mouse mapping stopped"), detail, NIIF_WARNING);
            }
        }
    }
    void InputMappingCoordinator::ResetRoute()
    {
        m_routedLensId = 0; ++m_generation;
        m_phase = ProxyPhase::Off; m_routePending = false; m_geometryGate.Reset(); m_deferredGeometry.reset();
    }
    void InputMappingCoordinator::FinishOperation()
    {
        auto previous = m_lifecycle.Operation();
        bool stopped = m_lifecycle.StopPending();
        if (stopped)
        {
            m_lifecycle.Stopping(); // Keep entries closed through callbacks and destruction.
            Record(DiagnosticEvent::Lifecycle, { LONG(MappingOperation::Stopping), 0 }, false, m_generation, m_startingLensId);
            ResetRoute();
            if (m_engine)
            {
                m_engine->Stop();
                m_softwareCursorEndFailed = !m_engine->NativeCursorShowConfirmed();
                m_engine.reset();
            }
            m_softwareCursorRequested = false;
            if (m_surfaces) m_surfaces(false);
            if (m_cursor) m_cursor({});
        }
        if (m_lifecycle.Shutdown()) { m_surfaces = {}; m_cursor = {}; m_externalCursor = {}; }
        if (previous == MappingOperation::Starting || stopped)
            Record(DiagnosticEvent::Lifecycle, { LONG(previous), 1, stopped, m_lifecycle.Shutdown(), int64_t(m_deferredRoutes) },
                false, m_generation, m_startingLensId);
        m_startingLensId = 0; m_deferredRoutes = 0;
        m_lifecycle.Finish();
        bool notifyIdle = std::exchange(m_idleNotificationRequested, false);
        if (m_window && (notifyIdle || previous == MappingOperation::Starting || stopped))
            PostMessageW(m_window, LifecycleIdleMessage, 0, 0);
    }
    void InputMappingCoordinator::Shutdown()
    {
        ResetRoute();
        if (m_window) KillTimer(m_window, RefreshTimerId);
        m_lifecycle.RequestStop(true);
        if (m_lifecycle.Busy()) return;
        m_lifecycle.Stopping();
        FinishOperation();
    }
}
