#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>

namespace RegionLens::native
{
    // The callback never touches a renderer or invokes UI code. Its sole job
    // is to retain one readiness token and post a generation-tagged wake.
    class PresentationWait
    {
    public:
        enum class Result { Ready, Pending, Failed };
        ~PresentationWait() { Reset(); }
        PresentationWait() = default;
        PresentationWait(PresentationWait const&) = delete;
        PresentationWait& operator=(PresentationWait const&) = delete;
        void Target(HWND window, UINT message) noexcept { m_window=window; m_message=message; }
        bool Attach(HANDLE handle) noexcept
        {
            Reset(); m_handle=handle;
            if (handle && m_message) {
                m_wait=CreateThreadpoolWait(Callback,this,nullptr);
                if (!m_wait) {m_error=GetLastError();return false;}
            }
            return true;
        }
        void Suspend() noexcept
        {
            if (m_wait) {SetThreadpoolWait(m_wait,nullptr,nullptr);WaitForThreadpoolWaitCallbacks(m_wait,TRUE);}
            m_armed=false;
            ++m_generation;
        }
        void Reset() noexcept
        {
            Suspend();
            if (m_wait) {CloseThreadpoolWait(m_wait);m_wait=nullptr;}
            if (m_handle) {CloseHandle(m_handle);m_handle=nullptr;}
            m_ready=false;m_error=ERROR_SUCCESS;
        }
        Result Poll() noexcept
        {
            if (m_error.load()) return Result::Failed;
            if (!m_handle || m_ready.load(std::memory_order_acquire)) return Result::Ready;
            if (m_armed) return Result::Pending;
            auto result=WaitForSingleObject(m_handle,0);
            if (result==WAIT_OBJECT_0) {m_ready=true;return Result::Ready;}
            if (result!=WAIT_TIMEOUT) {m_error=GetLastError();return Result::Failed;}
            if (m_wait) {m_armed=true;SetThreadpoolWait(m_wait,m_handle,nullptr);}
            return Result::Pending;
        }
        bool Acknowledge(uint64_t generation) noexcept
        {
            if (generation!=m_generation) return false;
            // Join the tiny callback before reusing its wait object.
            if (m_wait) WaitForThreadpoolWaitCallbacks(m_wait,FALSE);
            m_armed=false;
            return true;
        }
        void Consume() noexcept {m_ready=false;}
        bool EventDriven() const noexcept {return m_wait!=nullptr;}
        uint64_t Generation() const noexcept {return m_generation;}
        DWORD Error() const noexcept {return m_error.load();}
    private:
        static void CALLBACK Callback(PTP_CALLBACK_INSTANCE,void* context,PTP_WAIT,TP_WAIT_RESULT result) noexcept
        {
            auto& self=*static_cast<PresentationWait*>(context);
            if (result==WAIT_OBJECT_0) self.m_ready.store(true,std::memory_order_release);
            else self.m_error=ERROR_INVALID_HANDLE;
            if (!PostMessageW(self.m_window,self.m_message,WPARAM(self.m_generation),0))
                self.m_error=GetLastError();
        }
        HANDLE m_handle{};
        PTP_WAIT m_wait{};
        HWND m_window{};
        UINT m_message{};
        uint64_t m_generation{};
        bool m_armed{};
        std::atomic_bool m_ready{};
        std::atomic<DWORD> m_error{};
    };

    struct RenderResult
    {
        enum State { Failed, Presented, Deferred };
        State state;
        constexpr RenderResult(State value) noexcept : state(value) {}
        // Compatibility for selection/offscreen callers; live scheduling checks state.
        constexpr operator bool() const noexcept {return state!=Failed;}
    };
}
