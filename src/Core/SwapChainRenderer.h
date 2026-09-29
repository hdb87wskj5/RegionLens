#pragma once

#include "RegionTypes.h"
#include "MouseProxyCore.h"
#include "CursorTexture.h"
#include "LiveResizePolicy.h"
#include "QualityRenderer.h"
#include "PresentationWait.h"
#include "LatencyMetrics.h"
#include <dcomp.h>

namespace RegionLens::native
{
    class D3DDevice;

    class SwapChainRenderer
    {
    public:
        ~SwapChainRenderer() { m_presentationWait.Reset(); }
        void SetPresentationWake(HWND window, UINT message) { m_presentationWait.Target(window,message); }
        bool AcknowledgePresentationWake(uint64_t generation) { return m_presentationWait.Acknowledge(generation); }
        void SuspendPresentationWait() { m_presentationWait.Suspend(); }
        bool EventDrivenPresentation() const noexcept { return m_presentationWait.EventDriven(); }
        RenderResult PreparePresentation();
        static bool ValidateShaders();
        static bool CompileFrameShaders(Microsoft::WRL::ComPtr<ID3DBlob>& vertex, Microsoft::WRL::ComPtr<ID3DBlob>& pixel);
        bool Initialize(HWND window, std::shared_ptr<D3DDevice> device, bool composition = false, uint64_t lensId = 0);
        bool Resize();
        bool StretchToClient();
        bool PrepareClientExtent(RenderExtent extent);
        void BeginLiveResize();
        bool EndLiveResize();
        void BeginProgrammaticResize() noexcept { m_programmaticResize = true; }
        void EndProgrammaticResize() noexcept { m_programmaticResize = false; }
        bool NeedsPresent() const noexcept { return m_presentPending; }
        HRESULT TakeQualityFailure() noexcept { return std::exchange(m_qualityFailure, S_OK); }
        void ResetQuality() noexcept { m_quality.Reset(); m_qualityUnavailable = false; }
        RenderResult Render(
            ID3D11ShaderResourceView* source,
            PixelRect sourceRect,
            int32_t textureWidth,
            int32_t textureHeight,
            std::optional<PixelRect> selection = std::nullopt,
            bool lensChrome = false,
            bool lensTopmost = false,
            bool lensFullscreen = false,
            bool lensInputMapping = false,
            CursorSnapshot const* cursor = nullptr,
            LensQualitySettings quality = {}, CaptureStamp frame = {}, bool lensPointerSpeedAdjusted = false,
            bool lensScreenshotFeedback = false,
            std::optional<PixelRect> contentRect = std::nullopt);

        // Shared with the clean screenshot pass: chrome/cursor flags stay zero there.
        struct alignas(16) ShaderConstants
        {
            float sourceUv[4]{};
            float selection[4]{};
            float destinationSize[2]{};
            float overlayMode{};
            float chromeMode{};
            float topmostMode{};
            float fullscreenMode{};
            float inputMappingMode{};
            float pointerSpeedMode{};
            float cursorRect[4]{};
            float cursorMode{};
            float cursorPadding[3]{};
            float sourceClamp[4]{};
            float chromeCenterY{};
            float chromePadding{};
            float chromeRadius{};
            float screenshotFeedback{};
            float closeX{}, pinX{}, fullscreenX{}, restoreX{};
            float mappingX{}, pointerSpeedX{}, chromePadding2{}, screenshotX{};
            // Client-physical x/y/width/height. The frame shader maps source
            // pixels only inside this rectangle and paints the remainder black.
            float contentRect[4]{};
        };
        static void SetChromeLayout(ShaderConstants& values, RECT client) noexcept;

    private:
        friend struct CompositionPresentationTestAccess;
        friend struct PresentationSchedulerTestAccess;
        bool CreateDeviceResources();
        bool CreateWindowSizeResources(RenderExtent size);
        bool AttachPreparedCompositionFrame();
        bool StretchToExtent(RenderExtent extent);
        void RecordResize(int phase);

        HWND m_window{};
        bool m_presentPending{};
        CaptureStamp m_presentedFrame;
        uint64_t m_presentedCount{};
        PresentationWait m_presentationWait;
        uint64_t m_waitStarted{}, m_lastTimedRevision{};
        LatencySamples m_waitLatency, m_presentLatency, m_frameAge;
        std::shared_ptr<D3DDevice> m_device;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
        Microsoft::WRL::ComPtr<IDXGISwapChain2> m_compositionSwapChain;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> m_retiredSwapChain;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTarget;
        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_constants;
        UINT m_width{};
        UINT m_height{};
        // Storage and raster viewport are deliberately distinct. DXGI stretches
        // SetSourceSize's viewport to storage; DComp scales storage to the HWND.
        RenderExtent m_bufferStorage{};
        bool m_composition{};
        bool m_programmaticResize{};
        LiveResizePolicy m_resizePolicy;
        CompositionResizeHandoff m_resizeHandoff;
        RenderExtent m_transformedClient{};
        uint64_t m_lensId{};
        uint64_t m_performanceLast{}, m_performanceFrames{}, m_performanceBusy{}, m_performanceLongest{};
        CursorTexture m_cursorTexture;
        QualityRenderer m_quality;
        bool m_qualityUnavailable{}, m_qualityFailureNotified{};
        HRESULT m_qualityFailure{ S_OK };
        uint64_t m_qualityLastReport{}, m_qualityCacheHits{}, m_qualityCpuTicks{};
        Microsoft::WRL::ComPtr<IDCompositionDevice> m_compositionDevice;
        Microsoft::WRL::ComPtr<IDCompositionTarget> m_compositionTarget;
        Microsoft::WRL::ComPtr<IDCompositionVisual> m_compositionVisual;
    };
}
