#pragma once

#include "InputMappingCoordinator.h"
#include "LensWindow.h"
#include "MappingStandbyPolicy.h"
#include "SoftwareCursorWindow.h"
#include "TopmostGuard.h"

namespace RegionLens::native
{
    class LensManager
    {
    public:
        using CloseRequestCallback = std::function<void(uint64_t)>;

        LensManager(
            std::shared_ptr<D3DDevice> device,
            std::shared_ptr<InputMappingCoordinator> inputMapping,
            CloseRequestCallback closeRequest, CloseRequestCallback screenshotRequest = {});
        ~LensManager();

        std::optional<uint64_t> Create(HMONITOR monitor, PixelRect source, RECT windowBounds);
        void Remove(uint64_t id);
        void CloseAll();
        void HideAll();
        bool ShowAllRaised();
        void RenderMonitor(HMONITOR monitor, ID3D11ShaderResourceView* source, int32_t width, int32_t height, CaptureStamp stamp = {});
        void SetPresentationRequest(std::function<void()> request) {m_presentationRequest=std::move(request);}
        void MarkMonitorDirty(HMONITOR monitor);
        bool MonitorReady(HMONITOR monitor);
        void FlushPresentations();
        void ToggleInputMapping(uint64_t id);
        void UpdateInputMapping(uint64_t id);
        void SuspendInputMappings();
        void ResumeInputMappings();
        void SetSettingsUiOpen(bool open);
        void SetQualityLevel(LensSharpness level);
        void SetFullscreenAspectFitEnabled(bool enabled);
        void SetNewWindowTopmost(bool enabled) noexcept { m_newWindowTopmost = enabled; }
        void RefreshLanguage();
        HRESULT CaptureScreenshot(uint64_t id, ScreenshotRenderer& renderer, Microsoft::WRL::ComPtr<ID3D11Texture2D>& staging);
        void NotifyScreenshotSuccess(uint64_t id);
        HMONITOR MonitorForLens(uint64_t id) const noexcept;
        [[nodiscard]] size_t CountForMonitor(HMONITOR monitor) const noexcept;
        void DisableAllInputMappings();
        void RefreshInputMappingFromCursor();
        bool StartTopmostEvents(HWND controller) { return m_topmostGuard.StartEvents(controller); }
        void StopTopmostEvents() { m_topmostGuard.StopEvents(); }
        void RefreshTopmost(bool suspended, bool eventWake = false);
        bool SetWeTypeDemotionExperiment(bool enabled) noexcept
        { return m_topmostGuard.SetWeTypeDemotionExperiment(enabled); }
        [[nodiscard]] bool WeTypeDemotionExperimentEnabled() const noexcept
        { return m_topmostGuard.WeTypeDemotionExperimentEnabled(); }
        [[nodiscard]] bool WeTypeDemotionIdle() const noexcept
        { return m_topmostGuard.WeTypeDemotionIdle(); }
        [[nodiscard]] WeTypeTrialStopReason TakeWeTypeDemotionStopReason() noexcept
        { return m_topmostGuard.TakeWeTypeDemotionStopReason(); }
        [[nodiscard]] bool TopmostFastWakeNeeded() const noexcept { return m_topmostGuard.FastWakeNeeded(); }
        [[nodiscard]] size_t Count() const noexcept { return m_lenses.size(); }
        [[nodiscard]] bool MappingSuspended() const noexcept { return m_mappingStandby.Suspended(); }

    private:
        friend struct LensSettingsTestAccess;
        friend struct PresentationSchedulerTestAccess;
        void ApplyUiPause();
        void ResumeRoutes();
        MappingSessionConfig MappingConfig(LensWindow const& lens) const;
        bool RaiseForInteraction(uint64_t id, bool force = false);
        void NotifyMappingHover(uint64_t id);
        bool RouteInputMapping(uint64_t id);
        void TryRouteInputMappingAtCursor(uint64_t preferredId = 0);
        std::optional<uint64_t> MappingCandidateAtCursor() const;
        bool BeginSoftwareCursor();
        void EndSoftwareCursor();
        void RefreshSoftwareCursor(std::optional<POINT> physical = std::nullopt);
        void SoftwareCursorFailed(DWORD error);
        static constexpr size_t MaximumLenses = 16;
        std::shared_ptr<D3DDevice> m_device;
        std::shared_ptr<InputMappingCoordinator> m_inputMapping;
        CloseRequestCallback m_closeRequest;
        CloseRequestCallback m_screenshotRequest;
        std::function<void()> m_presentationRequest;
        uint64_t m_lastPresentedLens{};
        std::unordered_map<uint64_t, std::unique_ptr<LensWindow>> m_lenses;
        uint64_t m_nextId{ 1 };
        MappingStandbyPolicy m_mappingStandby;
        LensInteractionOrder m_interactionOrder;
        uint64_t m_lastInputInteractionGeneration{};
        uint64_t m_lastInputInteractionSequence{};
        TopmostGuard m_topmostGuard;
        SoftwareCursorWindow m_softwareCursor;
        CursorSnapshot m_softwareCursorSnapshot;
        bool m_softwareCursorActive{};
        bool m_softwareCursorFailing{};
        bool m_softwareCursorRestoreUnconfirmed{};
        uint64_t m_lastSoftwareCursorHeartbeat{};
        MappingUiPauseState m_uiPause;
        LensSharpness m_qualityLevel{ LensSharpness::Medium };
        bool m_fullscreenAspectFit{};
        bool m_newWindowTopmost{ true };
    };
}
