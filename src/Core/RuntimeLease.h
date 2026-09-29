#pragma once
#include <windows.h>
namespace RegionLens::native
{
    class RuntimeLease
    {
    public:
        RuntimeLease() = default;
        RuntimeLease(RuntimeLease const&) = delete;
        ~RuntimeLease() { if (m_handle) CloseHandle(m_handle); }
        bool Acquire(wchar_t const* name) noexcept
        {
            if (m_handle) return false;
            auto handle = CreateMutexW(nullptr, FALSE, name);
            auto error = GetLastError();
            if (!handle) return false;
            if (error == ERROR_ALREADY_EXISTS) { CloseHandle(handle); SetLastError(ERROR_BUSY); return false; }
            m_handle = handle; return true;
        }
        HANDLE Handle() const noexcept { return m_handle; }
    private:
        HANDLE m_handle{};
    };
}
