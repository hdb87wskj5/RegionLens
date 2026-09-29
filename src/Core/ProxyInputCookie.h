#pragma once
#include <windows.h>
#include <cstdint>

namespace RegionLens::native
{
    // Keep the wire value in the unsigned 32-bit range. The observed input
    // path clears dwExtraInfo's upper half before our low-level hook sees it.
    // Compare the whole received ULONG_PTR; never mask an incoming tag.
    // This is an event-routing marker, not a process-authentication secret.
    inline constexpr bool ValidProxyInputCookie(ULONG_PTR cookie) noexcept
    {
        return cookie != 0 && uint64_t(cookie) <= UINT32_MAX;
    }

    // FillRandom is supplied by the platform at startup, or by a pure fake in
    // tests. Reject zero rather than substituting a predictable shared marker.
    // Bound retries and clear the output on every failure; fail before hooking.
    template<typename FillRandom>
    bool CreateProxyInputCookie(ULONG_PTR& cookie, FillRandom&& fillRandom)
    {
        cookie = 0;
        for (unsigned attempt = 0; attempt < 8; ++attempt)
        {
            uint32_t candidate{};
            if (!fillRandom(candidate)) return false;
            if (candidate)
            {
                cookie = static_cast<ULONG_PTR>(candidate);
                return true;
            }
        }
        return false;
    }
}
