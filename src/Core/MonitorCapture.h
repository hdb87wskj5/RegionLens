#pragma once

#include "D3DDevice.h"
#include "LensQuality.h"
#include "LatencyMetrics.h"

namespace RegionLens::native
{
    struct CapturedTexture
    {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        int32_t width{};
        int32_t height{};

        [[nodiscard]] explicit operator bool() const noexcept { return texture && view; }
    };

    class MonitorCapture : public std::enable_shared_from_this<MonitorCapture>
    {
    public:
        MonitorCapture(HMONITOR monitor, HWND notifyWindow, UINT frameMessage, std::shared_ptr<D3DDevice> device);
        ~MonitorCapture();

        MonitorCapture(MonitorCapture const&) = delete;
        MonitorCapture& operator=(MonitorCapture const&) = delete;

        CaptureStamp Stamp() const noexcept { return { m_captureId, m_revision, m_capturedQpc, m_arrivedQpc }; }
        bool HasPendingFrame() const {std::scoped_lock lock(m_pendingMutex);return bool(m_pendingFrame);}
        bool Start();
        void Stop();
        bool ProcessPendingFrame();
        [[nodiscard]] CapturedTexture Freeze() const;

        [[nodiscard]] HMONITOR Monitor() const noexcept { return m_monitor; }
        [[nodiscard]] bool HasFrame() const noexcept { return m_latest.view != nullptr; }
        [[nodiscard]] ID3D11ShaderResourceView* LatestView() const noexcept { return m_latest.view.Get(); }
        [[nodiscard]] int32_t Width() const noexcept { return m_latest.width; }
        [[nodiscard]] int32_t Height() const noexcept { return m_latest.height; }

    private:
        void OnFrameArrived(
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool const& sender,
            winrt::Windows::Foundation::IInspectable const&);

        HMONITOR m_monitor{};
        HWND m_notifyWindow{};
        UINT m_frameMessage{};
        std::shared_ptr<D3DDevice> m_device;
        winrt::Windows::Graphics::Capture::GraphicsCaptureItem m_item{ nullptr };
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool m_framePool{ nullptr };
        winrt::Windows::Graphics::Capture::GraphicsCaptureSession m_session{ nullptr };
        winrt::event_token m_frameToken{};
        mutable std::mutex m_pendingMutex;
        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame m_pendingFrame{ nullptr };
        bool m_framePosted{};
        uint64_t m_pendingArrived{},m_capturedQpc{},m_arrivedQpc{};
        uint64_t m_arrivals{},m_replaced{},m_lastTimingReport{};
        LatencySamples m_copyLatency,m_arrivalLatency;
        std::atomic_bool m_running{};
        CapturedTexture m_latest;
        uint64_t m_captureId{}, m_revision{};
    };
}
