#include "pch.h"
#include "TopmostFastWake.h"

namespace RegionLens::native
{
    TopmostFastWake::TopmostFastWake(HWND controller, UINT message) noexcept
        : m_controller(controller), m_message(message) {}

    TopmostFastWake::~TopmostFastWake() { Shutdown(); }

    bool TopmostFastWake::SetEnabled(bool enabled) noexcept
    {
        if (m_failed.load()) return !enabled;
        if (m_enabled.load() == enabled) return true;
        if (!enabled)
        {
            m_enabled.store(false);
            if (m_timer) CancelWaitableTimer(m_timer);
            return true;
        }
        if (!m_thread && !Start()) return false;
        m_enabled.store(true);
        LARGE_INTEGER due{};
        due.QuadPart = -40'000; // first wake in 4 ms, then every 4 ms
        if (!SetWaitableTimer(m_timer, &due, 4, nullptr, nullptr, FALSE))
        {
            Fail(GetLastError());
            return false;
        }
        return true;
    }

    void TopmostFastWake::Acknowledge() noexcept { m_posted.store(false); }
    bool TopmostFastWake::Enabled() const noexcept { return m_enabled.load() && !m_failed.load(); }
    DWORD TopmostFastWake::Error() const noexcept { return m_error.load(); }

    DWORD WINAPI TopmostFastWake::Run(void* context) noexcept
    {
        auto& self = *static_cast<TopmostFastWake*>(context);
        HANDLE handles[]{ self.m_stop, self.m_timer };
        while (true)
        {
            auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0) return 0;
            if (result != WAIT_OBJECT_0 + 1)
            {
                self.Fail(GetLastError());
                return 1;
            }
            if (!self.m_enabled.load() || self.m_posted.exchange(true)) continue;
            if (!PostMessageW(self.m_controller, self.m_message, 0, 0))
            {
                self.m_posted.store(false);
                self.Fail(GetLastError());
                return 1;
            }
        }
    }

    bool TopmostFastWake::Start() noexcept
    {
        m_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!m_stop || !m_timer)
        {
            Fail(GetLastError());
            return false;
        }
        m_thread = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        if (!m_thread)
        {
            Fail(GetLastError());
            return false;
        }
        return true;
    }

    void TopmostFastWake::Fail(DWORD error) noexcept
    {
        m_error.store(error ? error : ERROR_GEN_FAILURE);
        m_failed.store(true);
        m_enabled.store(false);
    }

    void TopmostFastWake::Shutdown() noexcept
    {
        m_enabled.store(false);
        if (m_timer) CancelWaitableTimer(m_timer);
        if (m_stop) SetEvent(m_stop);
        if (m_thread) { WaitForSingleObject(m_thread, INFINITE); CloseHandle(m_thread); m_thread = nullptr; }
        if (m_timer) { CloseHandle(m_timer); m_timer = nullptr; }
        if (m_stop) { CloseHandle(m_stop); m_stop = nullptr; }
    }
}
