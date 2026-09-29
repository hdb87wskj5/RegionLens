#include "pch.h"
#include "MonitorCapture.h"

using namespace winrt;
using namespace winrt::Windows::Graphics;
using namespace winrt::Windows::Graphics::Capture;
using namespace winrt::Windows::Graphics::DirectX;
using Microsoft::WRL::ComPtr;

namespace RegionLens::native
{
    MonitorCapture::MonitorCapture(HMONITOR monitor, HWND notifyWindow, UINT frameMessage, std::shared_ptr<D3DDevice> device)
        : m_monitor(monitor), m_notifyWindow(notifyWindow), m_frameMessage(frameMessage), m_device(std::move(device))
    {
    }

    MonitorCapture::~MonitorCapture()
    {
        Stop();
    }

    bool MonitorCapture::Start()
    {
        if (m_running)
        {
            return true;
        }
        if (!GraphicsCaptureSession::IsSupported())
        {
            return false;
        }

        try
        {
            auto factory = get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
            check_hresult(factory->CreateForMonitor(m_monitor, guid_of<GraphicsCaptureItem>(), put_abi(m_item)));
            auto size = m_item.Size();
            if (size.Width <= 0 || size.Height <= 0)
            {
                return false;
            }

            m_framePool = Direct3D11CaptureFramePool::CreateFreeThreaded(
                m_device->WinrtDevice(),
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                size);
            auto weakSelf = weak_from_this();
            m_frameToken = m_framePool.FrameArrived(
                [weakSelf](Direct3D11CaptureFramePool const& sender,
                    winrt::Windows::Foundation::IInspectable const& args)
                {
                    if (auto self = weakSelf.lock())
                    {
                        self->OnFrameArrived(sender, args);
                    }
                });
            m_session = m_framePool.CreateCaptureSession(m_item);
            m_session.IsCursorCaptureEnabled(false);
            try
            {
                m_session.IsBorderRequired(false);
            }
            catch (...)
            {
                // Borderless capture requires consent/identity on some systems.
            }
            m_running = true;
            m_session.StartCapture();
            return true;
        }
        catch (hresult_error const& error)
        {
            OutputDebugStringW((L"RegionLens: failed to start capture: " + error.message() + L"\n").c_str());
            Stop();
            return false;
        }
    }

    void MonitorCapture::Stop()
    {
        m_running = false;
        if (m_framePool)
        {
            try
            {
                m_framePool.FrameArrived(m_frameToken);
            }
            catch (...) {}
        }
        if (m_session)
        {
            m_session.Close();
            m_session = nullptr;
        }
        if (m_framePool)
        {
            m_framePool.Close();
            m_framePool = nullptr;
        }
        m_item = nullptr;

        std::scoped_lock lock(m_pendingMutex);
        if (m_pendingFrame)
        {
            m_pendingFrame.Close();
            m_pendingFrame = nullptr;
        }
        m_framePosted = false;
    }

    void MonitorCapture::OnFrameArrived(
        Direct3D11CaptureFramePool const& sender,
        winrt::Windows::Foundation::IInspectable const&)
    {
        if (!m_running)
        {
            return;
        }

        winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame{nullptr};
        uint64_t drained{};
        // Bounded drain: the frame pool has two buffers. Never chase a producer
        // indefinitely, and never enqueue an older available buffer for display.
        try {
            for(int i=0;i<2;++i) {
                auto next=sender.TryGetNextFrame();
                if(!next) break;
                ++drained;
                if(frame) frame.Close();
                frame=std::move(next);
            }
        } catch (...) { return; } // The pool may close during shutdown.
        if (!frame)
        {
            return;
        }

        bool shouldPost = false;
        {
            std::scoped_lock lock(m_pendingMutex);
            m_arrivals+=drained;
            m_replaced+=drained-1;
            if (m_pendingFrame)
            {
                ++m_replaced;
                m_pendingFrame.Close();
            }
            m_pendingFrame = std::move(frame);
            m_pendingArrived=QpcNow();
            if (!m_framePosted)
            {
                m_framePosted = true;
                shouldPost = true;
            }
        }
        if (shouldPost)
        {
            PostMessageW(m_notifyWindow, m_frameMessage, reinterpret_cast<WPARAM>(this), 0);
        }
    }

    bool MonitorCapture::ProcessPendingFrame()
    {
        Direct3D11CaptureFrame frame{ nullptr };
        uint64_t arrived{};
        {
            std::scoped_lock lock(m_pendingMutex);
            frame = std::move(m_pendingFrame);
            arrived=m_pendingArrived;
            m_pendingFrame = nullptr;
            m_framePosted = false;
        }
        if (!frame)
        {
            return false;
        }

        try
        {
            auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> sourceTexture;
            check_hresult(access->GetInterface(IID_PPV_ARGS(sourceTexture.ReleaseAndGetAddressOf())));

            D3D11_TEXTURE2D_DESC description{};
            sourceTexture->GetDesc(&description);
            bool recreate = !m_latest.texture ||
                m_latest.width != static_cast<int32_t>(description.Width) ||
                m_latest.height != static_cast<int32_t>(description.Height);
            if (recreate)
            {
                m_latest = {};
                description.MipLevels = 1;
                description.ArraySize = 1;
                description.Usage = D3D11_USAGE_DEFAULT;
                description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                description.CPUAccessFlags = 0;
                description.MiscFlags = 0;
                check_hresult(m_device->Device()->CreateTexture2D(&description, nullptr, m_latest.texture.ReleaseAndGetAddressOf()));
                check_hresult(m_device->Device()->CreateShaderResourceView(m_latest.texture.Get(), nullptr, m_latest.view.ReleaseAndGetAddressOf()));
                m_latest.width = static_cast<int32_t>(description.Width);
                m_latest.height = static_cast<int32_t>(description.Height);
            }
            auto before=QpcNow();
            m_device->Context()->CopyResource(m_latest.texture.Get(), sourceTexture.Get());
            m_arrivedQpc=arrived;
            auto relative=frame.SystemRelativeTime().count();
            m_capturedQpc=relative>0 ? uint64_t(static_cast<long double>(relative)*QpcFrequency()/10000000) : 0;
            static std::atomic<uint64_t> nextInstance{ 1 };
            if (!m_captureId) m_captureId = nextInstance.fetch_add(1);
            if(Runtime().diagnostics) {
                m_copyLatency.Add(QpcMicros(QpcNow()-before));
                if(m_capturedQpc && arrived>=m_capturedQpc) m_arrivalLatency.Add(QpcMicros(arrived-m_capturedQpc));
                auto now=GetTickCount64();
                if(now-m_lastTimingReport>=1000) {
                    m_copyLatency.Report(6,m_captureId);m_arrivalLatency.Report(7,m_captureId);
                    uint64_t arrivals{},replaced{};
                    {std::scoped_lock lock(m_pendingMutex);arrivals=std::exchange(m_arrivals,0);replaced=std::exchange(m_replaced,0);}
                    Record(DiagnosticEvent::CaptureTiming,{int64_t(arrivals),int64_t(replaced),m_latest.width,m_latest.height},false,0,m_captureId);
                    m_lastTimingReport=now;
                }
            }
            ++m_revision;
            frame.Close();
            return true;
        }
        catch (hresult_error const& error)
        {
            OutputDebugStringW((L"RegionLens: failed to process capture frame: " + error.message() + L"\n").c_str());
            frame.Close();
            return false;
        }
    }

    CapturedTexture MonitorCapture::Freeze() const
    {
        CapturedTexture frozen;
        if (!m_latest.texture)
        {
            return frozen;
        }

        D3D11_TEXTURE2D_DESC description{};
        m_latest.texture->GetDesc(&description);
        if (FAILED(m_device->Device()->CreateTexture2D(&description, nullptr, frozen.texture.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        m_device->Context()->CopyResource(frozen.texture.Get(), m_latest.texture.Get());
        if (FAILED(m_device->Device()->CreateShaderResourceView(frozen.texture.Get(), nullptr, frozen.view.ReleaseAndGetAddressOf())))
        {
            return {};
        }
        frozen.width = m_latest.width;
        frozen.height = m_latest.height;
        return frozen;
    }
}
