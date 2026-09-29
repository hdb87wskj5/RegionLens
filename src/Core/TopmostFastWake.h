#pragma once

#include <windows.h>
#include <atomic>

namespace RegionLens::native
{
    // Lazily started, bounded wake source for a tracked full-screen WeType
    // overlap. It only posts a coalesced message; the UI thread owns all
    // window inspection and z-order work.
    class TopmostFastWake final
    {
    public:
        TopmostFastWake(HWND controller, UINT message) noexcept;
        ~TopmostFastWake();
        TopmostFastWake(TopmostFastWake const&) = delete;
        TopmostFastWake& operator=(TopmostFastWake const&) = delete;

        bool SetEnabled(bool enabled) noexcept;
        void Acknowledge() noexcept;
        [[nodiscard]] bool Enabled() const noexcept;
        [[nodiscard]] DWORD Error() const noexcept;

    private:
        static DWORD WINAPI Run(void* context) noexcept;
        bool Start() noexcept;
        void Fail(DWORD error) noexcept;
        void Shutdown() noexcept;

        HWND m_controller{};
        UINT m_message{};
        HANDLE m_stop{}, m_timer{}, m_thread{};
        std::atomic<bool> m_enabled{}, m_posted{}, m_failed{};
        std::atomic<DWORD> m_error{};
    };
}
