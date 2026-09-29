#pragma once
#include "ScreenshotStorage.h"

namespace RegionLens::native
{
    struct ScreenshotResult
    {
        HRESULT clipboard{ E_PENDING }, file{ E_PENDING };
        std::wstring path;
    };
    class ScreenshotService
    {
    public:
        explicit ScreenshotService(IScreenshotClipboard& clipboard = SystemScreenshotClipboard()) : m_clipboard(clipboard) {}
        bool Busy() const noexcept { return m_staging || m_job; }
        HRESULT Start(Microsoft::WRL::ComPtr<ID3D11Texture2D> staging, std::wstring const& directory);
        std::optional<ScreenshotResult> Poll(ID3D11DeviceContext* context, HWND owner) noexcept;
    private:
        struct Job
        {
            std::vector<uint8_t> pixels;
            std::wstring path;
            HGLOBAL dib{};
            HRESULT dibResult{ E_PENDING }, fileResult{ E_PENDING };
            std::atomic<bool> dibReady{}, finished{};
            ~Job() { if (dib) GlobalFree(dib); }
        };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_staging;
        std::shared_ptr<Job> m_job;
        RenderExtent m_size{};
        uint64_t m_start{};
        ScreenshotClipboardDelivery m_delivery;
        IScreenshotClipboard& m_clipboard;
    };
}
