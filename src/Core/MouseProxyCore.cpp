#include "pch.h"
#include "MouseProxyCore.h"

namespace RegionLens::native
{
    POINT MouseProxyCore::Position() const noexcept
    {
        return { static_cast<LONG>(std::llround(m_x)), static_cast<LONG>(std::llround(m_y)) };
    }
    POINT MouseProxyCore::DispatchPosition() const noexcept
    {
        return { static_cast<LONG>(std::llround(m_dispatchX)), static_cast<LONG>(std::llround(m_dispatchY)) };
    }
    POINT MouseProxyCore::Source() const noexcept
    {
        return ProxyMapPoint(m_dispatchX, m_dispatchY, m_config.destination, m_config.source);
    }
    bool MouseProxyCore::Content(POINT p) const noexcept
    {
        return !m_config.blocked && PtInRect(&m_config.destination, p) && !ProxyLocalControl(m_config, p);
    }
    CursorSnapshot MouseProxyCore::Snapshot() const noexcept
    {
        CursorSnapshot snapshot{ m_config.generation, m_config.lensId, m_phase, Position(), Source(), nullptr,
            // The return-to-desktop move is asynchronous. Keep drawing until
            // it has settled and CursorVisible(true) has actually been issued;
            // entering chrome must not remove both cursors at once.
            m_failures.first, m_buttons, m_hidden &&
                (m_phase == ProxyPhase::Active || m_phase == ProxyPhase::Restoring),
            m_failures.secondary, m_failures.firstSite, m_failures.secondarySite };
        snapshot.interactionSequence = m_interactionSequence;
        return snapshot;
    }
    void MouseProxyCore::Publish(bool localMoveRequested)
    {
        auto snapshot = Snapshot();
        snapshot.localMoveRequested = localMoveRequested;
        m_backend.Publish(snapshot);
    }
    void MouseProxyCore::ResetAfterStop() noexcept
    {
        m_config = {}; m_nextConfig.reset(); m_phase = ProxyPhase::Off; m_queue.Clear();
        m_pointerGain = {}; m_invalidMotion = false;
        m_x = m_y = m_dispatchX = m_dispatchY = 0;
        m_buttons = m_physicalButtons = 0; m_failures = {};
        ++m_surfaceRequest; m_gate = m_overflow = m_hidden = m_surfaces = m_restoreRequested = m_rearm = false;
        m_cleanupFailed = m_restoreMove = m_restoreVisibilityAttempted = m_cursorRestoreFailed =
            m_cursorTouched = m_localMovePending = false;
        m_lastSentSource = {}; m_lastSentSourceValid = false;
        m_lastExposureCheck = 0; m_exposureCheckValid = false;
        m_obligations = m_restoreButtons = 0; m_interactionSequence = 0;
    }
    void MouseProxyCore::Configure(MappingSessionConfig const& config)
    {
        if (!config.generation || !ProxyRectValid(config.source) || !ProxyRectValid(config.destination) ||
            !ProxyRectValid(config.desktop)) { Disable(ERROR_INVALID_PARAMETER, ProxyFaultSite::Configuration); return; }
        if (m_phase == ProxyPhase::Active || m_phase == ProxyPhase::Preparing || m_phase == ProxyPhase::Restoring)
        {
            m_nextConfig = config;
            Restore(false);
            return;
        }
        m_config = config; m_failures = {}; m_gate = false; m_queue.Clear();
        m_pointerGain = PointerGainFor(config); m_invalidMotion = false;
        m_cleanupFailed = false; m_lastSentSourceValid = false; m_exposureCheckValid = false;
        m_interactionSequence = 0;
        m_phase = m_backend.Healthy() ? ProxyPhase::Armed : ProxyPhase::Failed;
        if (m_phase == ProxyPhase::Failed) NoteFailure(ERROR_NOT_READY, ProxyFaultSite::InitialHealth);
        Publish();
    }
    void MouseProxyCore::NoteFailure(DWORD error, ProxyFaultSite site)
    {
        m_failures.Note(error, site);
    }
    void MouseProxyCore::Disable(DWORD error, ProxyFaultSite site)
    {
        m_nextConfig.reset();
        Restore(false, error, site);
    }
    void MouseProxyCore::InjectionFailed(DWORD error, ProxyFaultSite site)
    {
        if (m_phase == ProxyPhase::Restoring)
        {
            NoteFailure(error, ProxyFaultSite::ReleaseInput);
            m_cleanupFailed = true; m_restoreButtons = 0; m_restoreMove = false;
            m_backend.RecoveryFailed();
        }
        else
        {
            m_buttons |= m_obligations;
            Disable(error, site);
        }
    }
    void MouseProxyCore::InjectionCompleted()
    {
        m_obligations = m_buttons;
        if (m_phase == ProxyPhase::Restoring) m_restoreButtons &= m_buttons;
        if (m_phase == ProxyPhase::Active || m_phase == ProxyPhase::Restoring)
            m_backend.RecoveryState(true, Position(), Source(), m_obligations);
    }
    bool MouseProxyCore::Intercept(ProxyMouseEvent event) noexcept
    {
        auto previousButtons = m_physicalButtons;
        m_physicalButtons = ProxyUpdateButtons(m_physicalButtons, event.message, event.data);
        event.physicalButtons = m_physicalButtons;
        if (m_phase == ProxyPhase::Off || m_phase == ProxyPhase::Failed) return false;
        if (!m_gate && m_phase == ProxyPhase::Armed)
        {
            // Entry movements remain ordinary OS input until the worker verifies
            // that this lens is exposed. Otherwise an occluding foreign window
            // would lose its input merely because it overlaps the lens bounds.
            m_queue.Clear();
            if (event.message != WM_MOUSEMOVE || previousButtons || m_physicalButtons || !Content(event.point)) return false;
            m_queue.Push(event);
            return false;
        }
        if (!m_gate) return false;
        if ((m_phase == ProxyPhase::Preparing || m_phase == ProxyPhase::Active) && event.message == WM_MOUSEMOVE)
        {
            // Preview motion is independent from SendInput acknowledgement. At
            // high magnification several destination pixels can map to the same
            // source pixel; serialising those no-op warps made the drawn cursor
            // trail the hand even though no useful source event was possible.
            if (!std::isfinite(event.dx) || !std::isfinite(event.dy)) { m_invalidMotion = true; return true; }
            double x = ProxyAccumulateAxis(m_x, event.dx, m_pointerGain.x);
            double y = ProxyAccumulateAxis(m_y, event.dy, m_pointerGain.y);
            if (event.physicalButtons)
            {
                auto border = m_config.fullscreen ? 0 : 10;
                x = std::clamp(x, double(m_config.destination.left + border),
                    double(m_config.destination.right - border - 1));
                y = std::clamp(y, double(m_config.destination.top + border),
                    double(m_config.destination.bottom - border - 1));
                if (ProxyLocalControl(m_config, { LONG(std::llround(x)), LONG(std::llround(y)) }))
                { x = m_x; y = m_y; }
            }
            m_x = x; m_y = y;
        }
        if (m_phase == ProxyPhase::Preparing || m_phase == ProxyPhase::Active)
        {
            event.virtualX = m_x; event.virtualY = m_y; event.virtualValid = true;
        }
        if (m_phase != ProxyPhase::Restoring && !m_queue.Push(event)) m_overflow = true;
        return true;
    }
    void MouseProxyCore::Pump()
    {
        if (m_invalidMotion) { m_invalidMotion = false; Disable(ERROR_INVALID_DATA, ProxyFaultSite::ActiveSend); }
        if (m_overflow) { m_overflow = false; Disable(ERROR_BUFFER_OVERFLOW, ProxyFaultSite::QueueOverflow); }
        if (m_phase != ProxyPhase::Off && m_phase != ProxyPhase::Failed && !m_backend.Healthy())
            Disable(ERROR_CANCELLED, ProxyFaultSite::HealthCheck);
        if (m_phase == ProxyPhase::Restoring) { ProgressRestore(); return; }
        if (m_phase == ProxyPhase::Preparing)
        {
            // A hung UI must not trap the physical mouse while we wait for its
            // transparent-window acknowledgement. Restoration never waits on UI.
            if (m_backend.NowTick() - m_prepareStarted > 1000) Disable(ERROR_TIMEOUT, ProxyFaultSite::PrepareTimeout);
            return;
        }
        // Visual motion is allowed to run ahead of the single ordered injection
        // slot. Leaving the content without a held button still starts recovery
        // immediately instead of waiting for an old source warp to settle.
        if (m_phase == ProxyPhase::Active && !m_physicalButtons && !Content(Position()))
        {
            Restore(true);
            return;
        }
        if (m_phase == ProxyPhase::Active && m_backend.InputSettled())
        {
            m_obligations = m_buttons;
            m_backend.RecoveryState(true, Position(), Source(), m_obligations);
        }
        ProxyMouseEvent event;
        // Native sends complete asynchronously. Keep all button/wheel ordering
        // in the existing bounded queue; never issue another batch meanwhile.
        while (m_backend.InputSettled() && m_queue.Pop(event))
        {
            m_backend.EventDequeued(event.queuedQpc);
            Process(event);
            if (m_phase == ProxyPhase::Preparing || m_phase == ProxyPhase::Restoring || m_phase == ProxyPhase::Failed) break;
        }
        if (m_phase == ProxyPhase::Active && !m_buttons)
        {
            auto now = m_backend.NowTick();
            if (!m_exposureCheckValid || now - m_lastExposureCheck >= 50)
            {
                m_lastExposureCheck = now; m_exposureCheckValid = true;
                if (!m_backend.DestinationExposed(m_config, Position())) Restore(true);
            }
        }
        Publish();
    }
    void MouseProxyCore::Process(ProxyMouseEvent const& event)
    {
        if (m_phase == ProxyPhase::Armed)
        {
            m_x = m_dispatchX = event.point.x; m_y = m_dispatchY = event.point.y;
            if (m_physicalButtons || !Content(Position()) || !m_backend.CanEnter(m_config, Position()))
            {
                m_queue.Clear(); m_gate = false; return;
            }
            m_phase = ProxyPhase::Preparing;
            m_gate = true;
            m_prepareStarted = m_backend.NowTick();
            m_surfaces = true;
            m_backend.RequestSurfaces(++m_surfaceRequest, true);
            Publish();
            return;
        }
        if (m_phase != ProxyPhase::Active) return;
        if (event.virtualValid)
        {
            m_dispatchX = event.virtualX; m_dispatchY = event.virtualY;
        }
        if (event.message == WM_LBUTTONDOWN && event.controlDown && !m_config.fullscreen &&
            !m_buttons && event.physicalButtons == 1)
        {
            ++m_interactionSequence;
            // This DOWN is consumed locally and is never delivered to the
            // source. Restore cursor/surfaces first; UI starts moving only
            // after the real destination window owns mouse hits again.
            m_localMovePending = true;
            Restore(true);
            return;
        }
        if (event.message == WM_MOUSEMOVE)
        {
            if (!event.physicalButtons && !Content(DispatchPosition())) { Restore(true); return; }
        }
        if (ProxyButtonDown(event.message)) ++m_interactionSequence;
        // Record the possible DOWN before calling the backend, so a partial
        // SendInput failure still causes its matching UP during recovery.
        auto oldButtons = m_buttons;
        m_buttons = ProxyUpdateButtons(m_buttons, event.message, event.data);
        m_obligations = oldButtons | m_buttons;
        m_backend.RecoveryState(true, Position(), Source(), m_obligations);
        if (!Send(event.message, event.data)) { m_buttons |= oldButtons; Disable(ERROR_WRITE_FAULT, ProxyFaultSite::ActiveSend); return; }
        if (m_backend.InputSettled()) m_obligations = m_buttons;
        m_backend.RecoveryState(true, Position(), Source(), m_obligations);
    }
    bool MouseProxyCore::Send(UINT message, DWORD data)
    {
        auto source = Source();
        if (message == WM_MOUSEMOVE && m_lastSentSourceValid &&
            source.x == m_lastSentSource.x && source.y == m_lastSentSource.y)
            return true;
        if (!m_backend.SendMouse(source, message, data, m_config.desktop)) return false;
        m_lastSentSource = source; m_lastSentSourceValid = true;
        return true;
    }
    void MouseProxyCore::SurfaceAcknowledged(uint64_t request, bool success)
    {
        if (request != m_surfaceRequest) return;
        if (m_phase == ProxyPhase::Preparing)
        {
            if (!success || !m_backend.Healthy() || !m_backend.SourceAccessible(m_config, Source()))
            { Disable(ERROR_ACCESS_DENIED, ProxyFaultSite::SurfacePrepare); return; }
            m_backend.RecoveryState(true, Position(), Source(), 0);
            m_cursorTouched = true; // Even a failed hide must be safely undone.
            if (!m_backend.CursorVisible(false)) { Disable(ERROR_ACCESS_DENIED, ProxyFaultSite::CursorHide); return; }
            m_hidden = true;
            if (!Send()) { Disable(ERROR_WRITE_FAULT, ProxyFaultSite::InitialSend); return; }
            m_phase = ProxyPhase::Active;
            Publish();
            Pump();
        }
        else if (m_phase == ProxyPhase::Restoring && m_restoreRequested)
        {
            m_surfaces = false; m_gate = false; m_restoreRequested = false;
            if (!success) { NoteFailure(ERROR_INVALID_WINDOW_HANDLE, ProxyFaultSite::SurfaceRestore); m_rearm = false; }
            bool startLocalMove = success && !m_failures.first && m_rearm && m_localMovePending &&
                (m_physicalButtons & 1u) != 0;
            m_localMovePending = false;
            m_phase = m_failures.first ? ProxyPhase::Failed : (m_rearm ? ProxyPhase::Armed : ProxyPhase::Off);
            if (!m_cleanupFailed) m_backend.RecoveryState(false, Position(), Source(), 0);
            Publish(startLocalMove);
            auto next = std::exchange(m_nextConfig, std::nullopt);
            if (next && !m_failures.first) Configure(*next);
        }
    }
    void MouseProxyCore::Restore(bool rearm, DWORD error, ProxyFaultSite site)
    {
        m_rearm = rearm;
        if (!rearm || error) m_localMovePending = false;
        if (error) { NoteFailure(error, site); m_nextConfig.reset(); }
        if (m_phase == ProxyPhase::Restoring) return;
        m_queue.Clear();
        if (!m_surfaces && !m_hidden)
        {
            // An earlier successful API return is not proof that the pointer is
            // still visible. Explicit/failure disable also repairs Armed/Off,
            // without re-sending any button UP or warping to an old position.
            RestoreCursorVisibility();
            m_gate = false; m_phase = m_failures.first ? ProxyPhase::Failed : ProxyPhase::Off;
            Publish(); return;
        }
        m_phase = ProxyPhase::Restoring;
        // Invalidate any outstanding prepare acknowledgement before releasing input.
        ++m_surfaceRequest;
        m_restoreButtons = m_buttons | m_obligations;
        m_restoreMove = m_hidden;
        m_restoreVisibilityAttempted = m_cursorRestoreFailed = false;
        Publish();
        ProgressRestore();
    }
    void MouseProxyCore::ProgressRestore()
    {
        if (m_restoreRequested) return;
        // Keep the surfaces transparent until all our queued UP/move events have
        // passed the hook. Otherwise restoring styles could steal the final UP.
        if (!m_backend.InputSettled())
        {
            if (!m_backend.RecoveryCompletedExternally()) return;
            // The watchdog already discharged the obligation. A stuck sender
            // must not keep swallowing hardware input. Its late normal packets
            // remain invalidated; no old-session recovery packets are queued.
            m_restoreButtons = 0; m_restoreMove = false; m_cleanupFailed = true;
        }
        constexpr UINT ups[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP };
        for (unsigned i = 0; i < 5; ++i)
        {
            if (!(m_restoreButtons & (1u << i))) continue;
            m_restoreButtons &= ~(1u << i);
            if (!m_backend.SendMouse(Source(), ups[i], i >= 3 ? DWORD((i == 3 ? XBUTTON1 : XBUTTON2) << 16) : 0,
                m_config.desktop)) InjectionFailed(ERROR_WRITE_FAULT);
            if (!m_backend.InputSettled()) return;
        }
        // A shape change in the destination thread may be the only thing that
        // repaints an invisible native arrow. Show it before the existing
        // return move, so that movement reaches the destination with the native
        // cursor already enabled. Do this once even if the sender is busy.
        if (!m_restoreVisibilityAttempted)
        {
            m_restoreVisibilityAttempted = true;
            RestoreCursorVisibility(true);
        }
        if (m_restoreMove)
        {
            m_restoreMove = false;
            if (!m_backend.SendMouse(Position(), WM_MOUSEMOVE, 0, m_config.desktop)) InjectionFailed(ERROR_WRITE_FAULT);
            if (!m_backend.InputSettled()) return;
        }
        m_buttons = m_obligations = 0;
        // Discharge input BEFORE a cursor API can fail/cancel the guardian;
        // otherwise a visibility-only fault could race a duplicate UP/warp.
        if (!m_cleanupFailed) m_backend.RecoveryState(false, Position(), Source(), 0);
        if (m_cursorRestoreFailed) m_backend.RecoveryFailed();
        m_hidden = false;
        m_restoreRequested = true;
        m_gate = false; // Ordinary desktop input must work even if the UI is hung.
        // The button/position obligation is already discharged. Cursor recovery
        // has its own lease and may continue after ordinary input is released. Do not
        // let a normal shutdown ask the watchdog to send duplicate button UPs
        // merely because the UI has not acknowledged its window styles yet.
        m_backend.RequestSurfaces(++m_surfaceRequest, false);
        Publish();
    }
    void MouseProxyCore::RestoreCursorVisibility(bool deferFailure)
    {
        if (m_cursorTouched && !m_backend.CursorVisible(true))
        {
            NoteFailure(ERROR_ACCESS_DENIED, ProxyFaultSite::CursorRestore);
            // The backend retains the independent cursor lease. Do not retain
            // already-released buttons merely because the show API failed.
            if (deferFailure) m_cursorRestoreFailed = true;
            else m_backend.RecoveryFailed();
        }
    }
}
