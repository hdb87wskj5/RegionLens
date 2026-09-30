#include "pch.h"
#include "LensManager.h"

namespace RegionLens::native
{
    LensManager::LensManager(
        std::shared_ptr<D3DDevice> device,
        std::shared_ptr<InputMappingCoordinator> inputMapping,
        CloseRequestCallback closeRequest, CloseRequestCallback screenshotRequest)
        : m_device(std::move(device)),
          m_inputMapping(std::move(inputMapping)),
          m_closeRequest(std::move(closeRequest)), m_screenshotRequest(std::move(screenshotRequest))
    {
        m_inputMapping->SetUiCallbacks(
            [this](bool transparent)
            {
                bool success = true;
                for (auto& [id, lens] : m_lenses) success = lens->SetInputPassThrough(transparent) && success;
                if (!success) for (auto& [id, lens] : m_lenses) lens->SetInputPassThrough(false);
                return success;
            },
            [this](CursorSnapshot const& cursor)
            {
                bool interacted = cursor.lensId && cursor.phase == ProxyPhase::Active &&
                    cursor.interactionSequence &&
                    (cursor.generation != m_lastInputInteractionGeneration ||
                        cursor.interactionSequence != m_lastInputInteractionSequence);
                if (cursor.lensId)
                {
                    m_lastInputInteractionGeneration = cursor.generation;
                    m_lastInputInteractionSequence = cursor.interactionSequence;
                }
                else
                {
                    m_lastInputInteractionGeneration = 0;
                    m_lastInputInteractionSequence = 0;
                }
                if (interacted) RaiseForInteraction(cursor.lensId);
                for (auto& [id, lens] : m_lenses) lens->SetVirtualCursor(id == cursor.lensId ? cursor : CursorSnapshot{});
                if (Runtime().persistentSoftwareCursor)
                {
                    m_softwareCursorSnapshot = cursor;
                    RefreshSoftwareCursor();
                }
            },
            [this](POINT physical)
            {
                if (Runtime().persistentSoftwareCursor) RefreshSoftwareCursor(physical);
            });
    }

    LensManager::~LensManager()
    {
        StopTopmostEvents();
        CloseAll();
        m_inputMapping->Shutdown();
        EndSoftwareCursor();
    }

    bool LensManager::BeginSoftwareCursor()
    {
        if (!Runtime().persistentSoftwareCursor) return true;
        if (m_softwareCursorFailing || m_softwareCursorRestoreUnconfirmed) return false;
        if (m_softwareCursorActive) return true;
        if (!m_inputMapping || !m_softwareCursor.Create(nullptr))
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        if (!m_topmostGuard.SetPassiveOverlay(m_softwareCursor.Window()))
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        POINT initial{};
        CURSORINFO current{ sizeof(current) };
        if (!GetPhysicalCursorPos(&initial))
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        HCURSOR shape = GetCursorInfo(&current) && current.hCursor ? current.hCursor :
            LoadCursorW(nullptr, IDC_ARROW);
        if (!m_softwareCursor.PrepareAt(initial, shape))
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        // The compositor and watchdog must both exist before the one global
        // hide.  Session enters/exits never repeat that visibility toggle.
        if (!m_inputMapping->BeginSoftwareCursor())
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        m_softwareCursorActive = true;
        m_lastSoftwareCursorHeartbeat = GetTickCount64();
        m_inputMapping->SoftwareCursorHeartbeat();
        if (!m_softwareCursor.ShowAt(initial, shape))
        {
            SoftwareCursorFailed(GetLastError());
            return false;
        }
        Record(DiagnosticEvent::CursorRecovery, { 3, m_softwareCursorActive, 1 });
        return m_softwareCursorActive && !m_softwareCursorFailing;
    }

    void LensManager::EndSoftwareCursor()
    {
        if (!Runtime().persistentSoftwareCursor) return;
        // A reentrant fault may defer worker shutdown until the current UI
        // callback unwinds. Keep the overlay only until that final stop has
        // actually confirmed a native show.
        if (m_softwareCursorRestoreUnconfirmed)
        {
            if (!m_inputMapping || !m_inputMapping->SoftwareCursorRestoreConfirmed()) return;
            m_softwareCursorRestoreUnconfirmed = false;
        }
        bool restored = !m_inputMapping || m_inputMapping->EndSoftwareCursor();
        if (!restored)
        {
            // Retain the drawn cursor while the guardian still owns a pending
            // native-visibility lease. Never leave a live desktop pointer blank.
            m_softwareCursorFailing = true;
            m_softwareCursorRestoreUnconfirmed = true;
            Record(DiagnosticEvent::CursorRecovery, { 4, 0, GetLastError() }, true);
            return;
        }
        bool surfacesReady = true;
        for (auto& [id, lens] : m_lenses)
            surfacesReady = lens->SetInputPassThrough(false) && surfacesReady;
        // The return point may still hit the colored software cursor window,
        // even though that window is marked transparent. Remove that surface
        // from hit testing only AFTER the worker confirms a native show, then
        // refresh the actual owning lens before destroying the surface.
        m_softwareCursor.Hide();
        bool localHandoff = false;
        POINT handoffPoint{};
        HWND handoffHit{};
        if (m_softwareCursorActive)
        {
            if (GetPhysicalCursorPos(&handoffPoint))
            {
                handoffHit = WindowFromPoint(handoffPoint);
                for (auto& [id, lens] : m_lenses)
                    if (lens->Window() == handoffHit)
                    {
                        localHandoff = lens->RefreshNativeCursorForHandoff(handoffPoint);
                        break;
                    }
            }
        }
        Record(DiagnosticEvent::CursorRecovery,
            { 4, 2, localHandoff, handoffPoint.x, handoffPoint.y,
              int64_t(uintptr_t(handoffHit)), surfacesReady }, !surfacesReady);
        m_softwareCursorActive = false;
        m_softwareCursorSnapshot = {};
        m_lastSoftwareCursorHeartbeat = 0;
        m_topmostGuard.SetPassiveOverlay(nullptr);
        m_softwareCursor.Destroy();
        m_softwareCursorFailing = false;
        m_softwareCursorRestoreUnconfirmed = false;
        Record(DiagnosticEvent::CursorRecovery, { 4, 1, 0 });
    }

    void LensManager::RefreshSoftwareCursor(std::optional<POINT> physical)
    {
        if (!Runtime().persistentSoftwareCursor || !m_softwareCursorActive ||
            (m_softwareCursorFailing && !m_softwareCursorRestoreUnconfirmed)) return;
        POINT point{};
        HCURSOR shape{};
        bool mapped = !m_softwareCursorRestoreUnconfirmed && m_inputMapping && m_softwareCursorSnapshot.visible &&
            m_softwareCursorSnapshot.lensId == m_inputMapping->RoutedLensId() &&
            m_softwareCursorSnapshot.generation == m_inputMapping->SessionGeneration() &&
            m_lenses.contains(m_softwareCursorSnapshot.lensId);
        if (mapped)
        {
            point = m_softwareCursorSnapshot.position;
            shape = m_softwareCursorSnapshot.shape;
        }
        else
        {
            if (physical) point = *physical;
            else if (!GetPhysicalCursorPos(&point))
            {
                SoftwareCursorFailed(GetLastError());
                return;
            }
            CURSORINFO current{ sizeof(current) };
            if (GetCursorInfo(&current)) shape = current.hCursor;
        }
        if (!shape) shape = LoadCursorW(nullptr, IDC_ARROW);
        if (!m_softwareCursor.ShowAt(point, shape)) SoftwareCursorFailed(GetLastError());
    }

    void LensManager::SoftwareCursorFailed(DWORD error)
    {
        if (m_softwareCursorFailing) return;
        m_softwareCursorFailing = true;
        error = error ? error : ERROR_GEN_FAILURE;
        Record(DiagnosticEvent::Fault, { error, 1 }, true);
        bool restored = !m_inputMapping || m_inputMapping->ReportSoftwareCursorFailure(error);
        if (!restored)
        {
            // A failed local show leaves a guardian-backed recovery obligation.
            // Keep a pointer image available until process exit rather than
            // withdrawing it solely because the input worker stopped.
            m_softwareCursorRestoreUnconfirmed = true;
            if (m_softwareCursor.Ready())
            {
                POINT point{};
                CURSORINFO cursor{ sizeof(cursor) };
                if (GetPhysicalCursorPos(&point))
                {
                    HCURSOR shape = GetCursorInfo(&cursor) && cursor.hCursor ? cursor.hCursor :
                        LoadCursorW(nullptr, IDC_ARROW);
                    m_softwareCursorActive = m_softwareCursor.ShowAt(point, shape);
                }
            }
        }
        DisableAllInputMappings();
        // ReportSoftwareCursorFailure drains before this call. A failed show
        // retains the overlay and the watchdog lease, never an inferred success.
        EndSoftwareCursor();
    }

    std::optional<uint64_t> LensManager::Create(HMONITOR monitor, PixelRect source, RECT windowBounds)
    {
        if (m_lenses.size() >= MaximumLenses)
        {
            return std::nullopt;
        }
        auto id = m_nextId++;
        LensDescriptor descriptor{ id, monitor, source, windowBounds, m_newWindowTopmost };
        auto lens = std::make_unique<LensWindow>(
            descriptor,
            m_device,
            m_closeRequest,
            [this](uint64_t lensId)
            {
                ToggleInputMapping(lensId);
            },
            [this](uint64_t lensId)
            {
                UpdateInputMapping(lensId);
            },
            [this](uint64_t lensId)
            {
                NotifyMappingHover(lensId);
            },
            [this](uint64_t lensId)
            {
                RaiseForInteraction(lensId);
            }, m_screenshotRequest, [this](uint64_t) { ApplyUiPause(); });
        lens->SetPresentationRequest(m_presentationRequest);
        lens->SetQualityLevel(m_qualityLevel);
        lens->SetFullscreenAspectFitEnabled(m_fullscreenAspectFit);
        // Hide all is a snapshot operation. A region created afterwards is a
        // new visible window, even while older regions remain hidden.
        if (!lens->Show())
        {
            return std::nullopt;
        }
        m_lenses.emplace(id, std::move(lens));
        m_interactionOrder.MarkFront(id);
        if (m_softwareCursorActive && !m_softwareCursor.EnsureTopmost())
            SoftwareCursorFailed(GetLastError());
        return id;
    }

    void LensManager::Remove(uint64_t id)
    {
        auto found = m_lenses.find(id);
        if (found == m_lenses.end())
        {
            return;
        }
        bool wasArmed = m_mappingStandby.IsArmed(id);
        bool wasRouted = m_mappingStandby.Routed() == id || (m_inputMapping && m_inputMapping->TargetsLens(id));
        if (wasArmed)
        {
            m_mappingStandby.Disarm(id);
            found->second->SetInputMappingEnabled(false);
        }
        if (wasRouted && m_inputMapping && !m_inputMapping->ReleaseRoute())
        {
            DisableAllInputMappings();
        }
        found->second->Close();
        m_lenses.erase(found);
        m_interactionOrder.Remove(id);
        if (m_lenses.empty()) m_uiPause.Hidden(false);
        ApplyUiPause();
        if (m_mappingStandby.Empty())
        {
            if (m_inputMapping && Runtime().persistentSoftwareCursor) m_inputMapping->ReleaseRoute();
            EndSoftwareCursor();
            if (m_inputMapping) m_inputMapping->DeactivateAll();
        }
        else if (auto routed = m_mappingStandby.Routed())
        {
            UpdateInputMapping(routed);
        }
        else
        {
            TryRouteInputMappingAtCursor();
        }
    }

    void LensManager::CloseAll()
    {
        DisableAllInputMappings();
        for (auto& [id, lens] : m_lenses)
        {
            lens->Close();
        }
        m_lenses.clear();
        m_uiPause.Hidden(false);
        ApplyUiPause();
        m_interactionOrder.Clear();
    }

    void LensManager::HideAll()
    {
        m_uiPause.Hidden(true);
        ApplyUiPause(); // Drain injected buttons/cursor and remove transparency first.
        for (auto& [id, lens] : m_lenses) lens->Hide();
        // Hidden is only a batch-transition pause. Individual LensWindow state
        // keeps old regions excluded while later-created regions remain usable.
        m_uiPause.Hidden(false);
        ApplyUiPause();
    }

    bool LensManager::ShowAllRaised()
    {
        m_uiPause.Hidden(true); // Also pause if Show is invoked while already visible.
        ApplyUiPause();
        // Snapshot native front-to-back order before raising any window. Show
        // bottom-first so overlapping regions do not shuffle on every shortcut.
        std::array<LensWindow*, MaximumLenses> order{};
        size_t count{};
        std::array<HWND, 2048> visited{};
        size_t visitedCount{};
        for (auto window = GetTopWindow(nullptr); window && count < m_lenses.size(); window = GetWindow(window, GW_HWNDNEXT)) {
            if (visitedCount == visited.size() || !IsWindow(window) ||
                std::find(visited.begin(), visited.begin() + visitedCount, window) != visited.begin() + visitedCount) break;
            visited[visitedCount++] = window;
            for (auto& [id, lens] : m_lenses)
                if (lens->Window() == window && count < order.size()) order[count++] = lens.get();
        }
        // A changing foreign desktop list must not omit any of our windows.
        for (auto& [id, lens] : m_lenses)
            if (std::find(order.begin(), order.begin() + count, lens.get()) == order.begin() + count)
                order[count++] = lens.get();
        bool success = true;
        while (count) success = order[--count]->ShowRaisedPreservingTopmost() && success;
        m_uiPause.Hidden(false);
        ApplyUiPause(); // Never undo a concurrent settings pause or global mapping shutdown.
        return success;
    }

    void LensManager::RenderMonitor(HMONITOR monitor, ID3D11ShaderResourceView* source, int32_t width, int32_t height, CaptureStamp stamp)
    {
        for (auto& [id, lens] : m_lenses)
        {
            if (lens->SourceMonitor() == monitor)
            {
                lens->Render(source, width, height, stamp);
            }
        }
    }

    HMONITOR LensManager::MonitorForLens(uint64_t id) const noexcept
    {auto found=m_lenses.find(id);return found==m_lenses.end()?nullptr:found->second->SourceMonitor();}

    size_t LensManager::CountForMonitor(HMONITOR monitor) const noexcept
    {
        size_t count{};
        for (auto const& [id, lens] : m_lenses)
            if (lens->SourceMonitor() == monitor) ++count;
        return count;
    }

    void LensManager::MarkMonitorDirty(HMONITOR monitor)
    { for(auto& [id,lens]:m_lenses) if(lens->SourceMonitor()==monitor) lens->RequestPresentation(); }

    bool LensManager::MonitorReady(HMONITOR monitor)
    {
        bool ready=false;
        for(auto& [id,lens]:m_lenses) if(lens->SourceMonitor()==monitor)
            ready=lens->ReadyForPresentation() || ready;
        return ready;
    }

    void LensManager::FlushPresentations()
    {
        std::array<uint64_t,MaximumLenses> order{};size_t count{};
        for(auto const& [id,lens]:m_lenses) order[count++]=id;
        std::sort(order.begin(),order.begin()+count);
        auto middle=std::upper_bound(order.begin(),order.begin()+count,m_lastPresentedLens);
        std::rotate(order.begin(),middle,order.begin()+count);
        auto active=m_inputMapping && m_inputMapping->Phase()==ProxyPhase::Active ? m_inputMapping->RoutedLensId() : 0;
        auto draw=[&](uint64_t id) {
            auto found=m_lenses.find(id);
            if(found!=m_lenses.end() && found->second->ReadyForPresentation()) found->second->FlushPresentation();
        };
        if(active) draw(active);
        for(size_t i=0;i<count;++i) if(order[i]!=active) draw(order[i]);
        if(count) m_lastPresentedLens=order[0];
    }

    void LensManager::ToggleInputMapping(uint64_t id)
    {
        auto found = m_lenses.find(id);
        if (found == m_lenses.end() || !m_inputMapping)
        {
            return;
        }
        auto lens = found->second.get();
        if (m_mappingStandby.IsArmed(id))
        {
            bool wasRouted = m_mappingStandby.Routed() == id || (m_inputMapping && m_inputMapping->TargetsLens(id));
            m_mappingStandby.Disarm(id);
            lens->SetInputMappingEnabled(false);
            if (m_mappingStandby.Empty())
            {
                if (Runtime().persistentSoftwareCursor) m_inputMapping->ReleaseRoute();
                EndSoftwareCursor();
                m_inputMapping->DeactivateAll();
            }
            else if (wasRouted)
            {
                if (!m_inputMapping->ReleaseRoute())
                {
                    DisableAllInputMappings();
                    return;
                }
                TryRouteInputMappingAtCursor();
            }
            return;
        }

        if (!m_mappingStandby.Arm(id)) return;
        lens->SetInputMappingEnabled(true);
        if (m_uiPause.Paused()) { m_mappingStandby.Suspend(); return; }
        if (m_mappingStandby.Count() == 1)
        {
            if (!BeginSoftwareCursor() || !RouteInputMapping(id)) DisableAllInputMappings();
        }
        else
        {
            TryRouteInputMappingAtCursor(id);
        }
    }

    void LensManager::UpdateInputMapping(uint64_t id)
    {
        if (!m_mappingStandby.IsArmed(id) || !m_inputMapping || !m_inputMapping->TargetsLens(id))
        {
            return;
        }
        auto found = m_lenses.find(id);
        if (found == m_lenses.end() ||
            !m_inputMapping->Update(MappingConfig(*found->second)))
        {
            DisableAllInputMappings();
        }
    }

    MappingSessionConfig LensManager::MappingConfig(LensWindow const& lens) const
    {
        auto config = lens.MappingConfig();
        for (auto const& [id, entry] : m_lenses) config.lenses[config.lensCount++] = entry->Window();
        return config;
    }

    bool LensManager::RaiseForInteraction(uint64_t id, bool force)
    {
        // Other lenses deliberately use MA_NOACTIVATE. Explicitly dismiss a
        // different lens's settings on click, since WM_ACTIVATE will not fire.
        std::array<uint64_t, MaximumLenses> popups{};
        size_t popupCount{};
        for (auto const& [otherId, lens] : m_lenses)
            if (otherId != id && lens->SpeedPopupOpen()) popups[popupCount++] = otherId;
        for (size_t index = 0; index < popupCount; ++index) {
            auto popup = m_lenses.find(popups[index]);
            if (popup != m_lenses.end()) popup->second->CloseSpeedPopup();
        }
        auto found = m_lenses.find(id);
        if (found == m_lenses.end() || m_uiPause.Hidden()) return false;

        bool lensAbove = false;
        for (auto above = GetWindow(found->second->Window(), GW_HWNDPREV); above && !lensAbove;
            above = GetWindow(above, GW_HWNDPREV))
        {
            for (auto const& [otherId, lens] : m_lenses)
            {
                if (otherId != id && lens->Window() == above && IsWindowVisible(above))
                {
                    lensAbove = true;
                    break;
                }
            }
        }
        if (!force && !lensAbove)
        {
            m_interactionOrder.MarkFront(id);
            return true;
        }
        if (!found->second->RaiseForInteraction()) return false;
        if (m_softwareCursorActive && !m_softwareCursor.EnsureTopmost())
            SoftwareCursorFailed(GetLastError());
        m_interactionOrder.MarkFront(id);
        return true;
    }

    void LensManager::NotifyMappingHover(uint64_t id)
    {
        TryRouteInputMappingAtCursor(id);
    }

    std::optional<uint64_t> LensManager::MappingCandidateAtCursor() const
    {
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return std::nullopt;
        auto hit = WindowFromPoint(cursor);
        if (!hit) return std::nullopt;
        auto root = GetAncestor(hit, GA_ROOT);
        if (root) hit = root;
        for (auto const& [id, lens] : m_lenses)
        {
            if (!m_mappingStandby.IsArmed(id) || lens->Window() != hit ||
                lens->ShouldSuspendInputMapping(cursor) || lens->MappingConfig().blocked)
            {
                continue;
            }
            return id;
        }
        return std::nullopt;
    }

    bool LensManager::RouteInputMapping(uint64_t id)
    {
        if (!m_inputMapping) return false;
        auto decision = m_mappingStandby.Evaluate(id, m_inputMapping->Phase(), m_inputMapping->RoutePending());
        if (decision == MappingRouteDecision::Reject) return true;
        if (decision == MappingRouteDecision::Keep) return true;
        auto found = m_lenses.find(id);
        if (found == m_lenses.end()) return false;
        auto result = m_inputMapping->Route(MappingConfig(*found->second));
        if (result == MappingRouteResult::Failed) return false;
        if (result != MappingRouteResult::Routed || m_inputMapping->RoutedLensId() != id) return true;
        // Shell activation can dispatch close/cancel/geometry messages. Never
        // commit a route based on the pre-activation iterator or standby state.
        if (!m_lenses.contains(id) || !m_mappingStandby.IsArmed(id) || m_mappingStandby.Suspended())
        { m_inputMapping->ReleaseRoute(); return true; }
        return m_mappingStandby.CommitRoute(id);
    }

    void LensManager::TryRouteInputMappingAtCursor(uint64_t preferredId)
    {
        if (!m_inputMapping || m_mappingStandby.Empty() || m_mappingStandby.Suspended()) return;
        if (m_inputMapping->RoutePending() ||
            (m_inputMapping->Phase() != ProxyPhase::Armed && m_inputMapping->Phase() != ProxyPhase::Off)) return;
        auto candidate = MappingCandidateAtCursor();
        if (!candidate || (preferredId && *candidate != preferredId)) return;
        if (!RouteInputMapping(*candidate)) DisableAllInputMappings();
    }

    void LensManager::SuspendInputMappings()
    {
        m_uiPause.Selection(true);
        ApplyUiPause();
    }

    void LensManager::SetSettingsCommitInProgress(bool committing)
    {
        m_uiPause.Settings(committing);
        if (committing) {
            for (auto& [id, lens] : m_lenses) lens->SetUiCursorPaused(true);
            m_mappingStandby.Suspend();
            // Only pause while committing runtime settings, never merely
            // because the modeless sheet is open. Preserve standby buttons.
            if (m_inputMapping && Runtime().persistentSoftwareCursor) m_inputMapping->ReleaseRoute();
            EndSoftwareCursor();
            if (m_inputMapping && !m_inputMapping->DeactivateAll()) DisableAllInputMappings();
        } else ApplyUiPause();
    }

    void LensManager::SetQualityLevel(LensSharpness level)
    {
        if (m_qualityLevel == level) return;
        m_qualityLevel = level;
        for (auto& [id, lens] : m_lenses) lens->SetQualityLevel(level);
    }

    void LensManager::SetFullscreenAspectFitEnabled(bool enabled)
    {
        if (m_fullscreenAspectFit == enabled) return;
        m_fullscreenAspectFit = enabled;
        for (auto& [id, lens] : m_lenses) lens->SetFullscreenAspectFitEnabled(enabled);
    }

    void LensManager::RefreshLanguage()
    { for (auto& [id, lens] : m_lenses) lens->RefreshLanguage(); }

    HRESULT LensManager::CaptureScreenshot(uint64_t id, ScreenshotRenderer& renderer, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging)
    {
        auto found = m_lenses.find(id);
        if (found == m_lenses.end()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return found->second->CaptureScreenshot(renderer, staging);
    }

    void LensManager::NotifyScreenshotSuccess(uint64_t id)
    {
        auto found = m_lenses.find(id);
        if (found != m_lenses.end()) found->second->NotifyScreenshotSuccess();
    }

    void LensManager::ResumeInputMappings()
    {
        m_uiPause.Selection(false);
        ApplyUiPause();
    }

    void LensManager::ResumeRoutes()
    {
        if (!m_mappingStandby.Suspended()) return;
        auto previousRoute = m_mappingStandby.Resume();
        if (!m_inputMapping || m_mappingStandby.Empty()) return;

        auto candidate = MappingCandidateAtCursor();
        uint64_t target = candidate.value_or(0);
        if (!target && previousRoute)
        {
            auto previous = m_lenses.find(previousRoute);
            if (previous != m_lenses.end() && !previous->second->Hidden()) target = previousRoute;
        }
        if (!target)
        {
            for (auto const& entry : m_lenses)
            {
                auto id = entry.first;
                if (m_mappingStandby.IsArmed(id) && !entry.second->Hidden())
                {
                    target = id;
                    break;
                }
            }
        }
        if (target && (!BeginSoftwareCursor() || !RouteInputMapping(target))) DisableAllInputMappings();
    }

    void LensManager::DisableAllInputMappings()
    {
        m_mappingStandby.Clear();
        if (m_uiPause.Paused()) m_mappingStandby.Suspend();
        for (auto& [id, lens] : m_lenses)
        {
            lens->SetInputMappingEnabled(false);
        }
        if (m_inputMapping && Runtime().persistentSoftwareCursor) m_inputMapping->ReleaseRoute();
        // Global shutdown must not retire the software cursor while any lens
        // remains transparent, even if a surface-restore notice is pending.
        for (auto& [id, lens] : m_lenses) lens->SetInputPassThrough(false);
        EndSoftwareCursor();
        if (m_inputMapping) m_inputMapping->DeactivateAll();
    }

    void LensManager::ApplyUiPause()
    {
        m_uiPause.SpeedPopup(std::any_of(m_lenses.begin(), m_lenses.end(),
            [](auto const& item) { return item.second->SpeedPopupOpen(); }));
        for (auto& [id, lens] : m_lenses) lens->SetUiCursorPaused(m_uiPause.Paused());
        if (m_uiPause.Paused()) {
            if (!m_mappingStandby.Suspended()) {
                m_mappingStandby.Suspend();
                if (m_inputMapping) {
                    if (m_uiPause.SelectionActive())
                    {
                        if (Runtime().persistentSoftwareCursor) m_inputMapping->ReleaseRoute();
                        EndSoftwareCursor();
                        m_inputMapping->DeactivateAll();
                    }
                    else if (!m_inputMapping->ReleaseRoute()) DisableAllInputMappings();
                }
            }
            EndSoftwareCursor();
        } else ResumeRoutes();
    }

    void LensManager::RefreshTopmost(bool suspended, bool eventWake)
    {
        std::array<TopmostLensTarget, MaximumLenses> targets{};
        std::array<WeTypeProbeLens, MaximumLenses> probeTargets{};
        size_t count{};
        size_t probeCount{};
        for (auto const& [id, lens] : m_lenses)
        {
            targets[count++] = { lens->Window(), lens->KeepsTopmost(), lens->IsFullscreen(),
                lens->TopmostHintWindow(), lens->TopmostTooltipWindow() };
            RECT bounds{};
            if (Runtime().weTypeProbe && lens->KeepsTopmost() &&
                IsWindowVisible(lens->Window()) && !IsIconic(lens->Window()) &&
                GetWindowRect(lens->Window(), &bounds))
                probeTargets[probeCount++] = { lens->Window(), bounds };
        }
        if (auto probe = Runtime().weTypeProbe)
            probe->UpdateTargets({ probeTargets.data(), probeCount });
        m_topmostGuard.Refresh({ targets.data(), count }, suspended || m_uiPause.Hidden(), eventWake);
        if (m_softwareCursorActive && !m_softwareCursor.EnsureTopmost())
            SoftwareCursorFailed(GetLastError());
    }

    void LensManager::RefreshInputMappingFromCursor()
    {
        if (m_softwareCursorRestoreUnconfirmed && m_inputMapping &&
            m_inputMapping->SoftwareCursorRestoreConfirmed()) EndSoftwareCursor();
        if (m_softwareCursorActive)
        {
            auto now = GetTickCount64();
            if (!m_softwareCursorFailing && now - m_lastSoftwareCursorHeartbeat >= 100)
            {
                m_lastSoftwareCursorHeartbeat = now;
                if (m_inputMapping) m_inputMapping->SoftwareCursorHeartbeat();
            }
            // Cursor-shape changes need no physical movement (for example,
            // entering a text field beneath a stationary pointer).
            RefreshSoftwareCursor();
        }
        if (m_mappingStandby.Suspended() && m_inputMapping && !m_inputMapping->LifecycleBusy() &&
            !m_inputMapping->InputRuntimeHealthy()) {
            Record(DiagnosticEvent::Fault, { ERROR_CANCELLED, int64_t(ProxyFaultSite::HealthCheck) }, true);
            DisableAllInputMappings(); // Never auto-restart a failed paused engine on settings close.
        }
        if (m_inputMapping && !m_inputMapping->LifecycleBusy() && !m_mappingStandby.Suspended())
        {
            m_inputMapping->RefreshFromCursor();
            auto routed = m_mappingStandby.Routed();
            if (routed && m_inputMapping->RoutedLensId() != routed)
            {
                DisableAllInputMappings();
                return;
            }
            TryRouteInputMappingAtCursor();
        }
    }
}
