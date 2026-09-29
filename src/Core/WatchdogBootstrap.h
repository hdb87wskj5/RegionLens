#pragma once
#include "ProxyStartup.h"
#include <array>
#include <cstdint>
#include <string_view>

namespace RegionLens::native
{
    using BootstrapHandles = std::array<HANDLE, 5>; // anonymous mapping, parent, quit, ready, runtime lease
    struct BootstrapPacket
    {
        uint32_t magic{ 0x524C4233 }, version{ 3 }, parentId{}, childId{};
        std::array<uint64_t, 5> handles{};
    };
    bool ParseBootstrapIdentity(std::wstring_view pid, std::wstring_view nonce, DWORD& parentId) noexcept;
    bool ValidBootstrapPacket(BootstrapPacket const& packet, DWORD parentId, DWORD childId) noexcept;
    void CloseBootstrapHandles(BootstrapHandles& handles) noexcept;
    // Shell activation is required for uiAccess binaries. The one-shot pipe only
    // carries duplicated handles after mutual PID / same-image verification.
    // No named recovery mapping, inherited handles, or ongoing command endpoint.
    // childProcess is owned by the caller even on a partial startup failure.
    bool LaunchWatchdogBootstrap(BootstrapHandles const& handles, HANDLE& childProcess,
        ProxyStartupFailure& failure, wchar_t const* mode = L"--input-watchdog", DWORD timeout = 5000);
    bool ReceiveWatchdogBootstrap(int argc, wchar_t** argv, BootstrapHandles& handles,
        ProxyStartupFailure& failure, DWORD timeout = 5000);
}
