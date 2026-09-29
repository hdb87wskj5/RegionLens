#pragma once
#include "ScreenshotRenderer.h"
#include "AppRuntime.h"
#include <span>

namespace RegionLens::native
{
    std::wstring DefaultScreenshotDirectory(AppIdentity const& identity);
    std::wstring LoadScreenshotDirectory(AppIdentity const& identity);
    HRESULT SaveScreenshotDirectory(AppIdentity const& identity, std::wstring const& directory);
    HRESULT EnsureScreenshotDirectory(std::wstring const& directory) noexcept;
    std::wstring NewScreenshotFileName();
    HRESULT WriteScreenshotPng(std::wstring const& file, RenderExtent size, std::span<uint8_t const> bgra) noexcept;
    HRESULT BuildScreenshotDib(RenderExtent size, std::span<uint8_t const> bgra, HGLOBAL& memory) noexcept;

    struct IScreenshotClipboard
    {
        virtual ~IScreenshotClipboard() = default;
        // Success transfers memory ownership to the clipboard and sets it to null.
        virtual HRESULT Publish(HWND owner, HGLOBAL& memory) noexcept = 0;
    };
    IScreenshotClipboard& SystemScreenshotClipboard() noexcept;
    struct ScreenshotClipboardDelivery
    {
        unsigned attempts{};
        uint64_t nextAttempt{};
        bool complete{};
        HRESULT result{ E_PENDING };
        void Tick(uint64_t now, HWND owner, HGLOBAL& memory, IScreenshotClipboard& backend) noexcept;
    };
}
