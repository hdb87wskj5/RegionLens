#include "pch.h"
#include "ScreenshotService.h"
#include <filesystem>
#include <thread>

namespace RegionLens::native
{
    HRESULT ScreenshotService::Start(Microsoft::WRL::ComPtr<ID3D11Texture2D> staging, std::wstring const& directory)
    {
        if (Busy()) return HRESULT_FROM_WIN32(ERROR_BUSY);
        if (!staging) return E_INVALIDARG;
        auto file = NewScreenshotFileName();
        if (file.empty()) return E_FAIL;
        D3D11_TEXTURE2D_DESC desc{}; staging->GetDesc(&desc);
        if (!ValidScreenshotExtent({ desc.Width, desc.Height }) || desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM ||
            desc.Usage != D3D11_USAGE_STAGING || !(desc.CPUAccessFlags & D3D11_CPU_ACCESS_READ)) return E_INVALIDARG;
        auto job = std::make_shared<Job>();
        job->path = directory.empty() ? L"" : (std::filesystem::path(directory) / file).wstring();
        m_size = { desc.Width, desc.Height }; m_delivery = {}; m_start = GetTickCount64();
        m_job = std::move(job); m_staging = std::move(staging);
        return S_OK;
    }

    std::optional<ScreenshotResult> ScreenshotService::Poll(ID3D11DeviceContext* context, HWND owner) noexcept
    {
        if (!Busy()) return {};
        try {
            if (m_staging) {
                D3D11_MAPPED_SUBRESOURCE map{};
                auto hr = context->Map(m_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
                if (hr == DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() - m_start < 5000) return {};
                if (SUCCEEDED(hr)) {
                    try {
                        m_job->pixels.resize(size_t(m_size.width) * m_size.height * 4);
                        for (UINT y = 0; y < m_size.height; ++y)
                            memcpy(m_job->pixels.data() + size_t(y) * m_size.width * 4,
                                static_cast<uint8_t const*>(map.pData) + size_t(y) * map.RowPitch, size_t(m_size.width) * 4);
                    } catch (...) { context->Unmap(m_staging.Get(), 0); throw; }
                    context->Unmap(m_staging.Get(), 0);
                }
                m_staging.Reset();
                if (FAILED(hr)) { auto path = m_job->path; m_job.reset(); return ScreenshotResult{ hr, hr, std::move(path) }; }
                // One bounded job. The worker owns only pixel data, never HWNDs,
                // input state or the D3D context. Closing a lens is safe immediately.
                std::thread([job = m_job, size = m_size] {
                    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                    auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    job->dibResult = BuildScreenshotDib(size, job->pixels, job->dib);
                    job->dibReady.store(true, std::memory_order_release);
                    job->fileResult = SUCCEEDED(apartment) ? WriteScreenshotPng(job->path, size, job->pixels) : apartment;
                    job->pixels.clear();
                    if (SUCCEEDED(apartment)) CoUninitialize();
                    job->finished.store(true, std::memory_order_release);
                }).detach();
            }
            if (m_job->dibReady.load(std::memory_order_acquire)) {
                if (FAILED(m_job->dibResult)) { m_delivery.complete = true; m_delivery.result = m_job->dibResult; }
                else m_delivery.Tick(GetTickCount64(), owner, m_job->dib, m_clipboard);
            }
            if (m_delivery.complete && m_job->finished.load(std::memory_order_acquire)) {
                ScreenshotResult result{ m_delivery.result, m_job->fileResult, m_job->path };
                m_job.reset(); return result;
            }
            return {};
        } catch (...) {
            m_staging.Reset(); m_job.reset();
            return ScreenshotResult{ E_OUTOFMEMORY, E_OUTOFMEMORY, {} };
        }
    }
}
