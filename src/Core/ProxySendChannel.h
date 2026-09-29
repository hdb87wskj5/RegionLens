#pragma once
#include "MouseProxyCore.h"
#include <atomic>

namespace RegionLens::native
{
    inline bool RejectCancelledProxyPacket(DWORD flags, ULONG_PTR tag, ULONG_PTR payloadCookie,
        ULONG_PTR fenceCookie, bool recoveryBatch, bool cancelled) noexcept
    {
        return cancelled && (flags & LLMHF_INJECTED) &&
            ((ValidProxyInputCookie(payloadCookie) && tag == payloadCookie) ||
                (!recoveryBatch && ValidProxyInputCookie(fenceCookie) && tag == fenceCookie));
    }
    struct ProxySendJob
    {
        std::array<INPUT, 3> inputs{};
        UINT count{}, message{};
        uint64_t sequence{}, tick{};
        uint64_t submittedQpc{};
        POINT point{};
        bool recovery{};
        bool requiresFence{};
        ProxyFaultSite site{ ProxyFaultSite::ActiveSend };
    };
    inline void MarkRecoveryReturnMove(ProxySendJob& job) noexcept
    {
        // Only the one existing return-position payload should bypass Windows
        // WM_MOUSEMOVE coalescing. High-rate mapped motion keeps its usual
        // latest-position behavior, and buttons/wheels are unchanged.
        if (job.recovery && job.message == WM_MOUSEMOVE && job.count == 1 &&
            job.inputs[0].type == INPUT_MOUSE && (job.inputs[0].mi.dwFlags & MOUSEEVENTF_MOVE))
            job.inputs[0].mi.dwFlags |= MOUSEEVENTF_MOVE_NOCOALESCE;
    }
    struct ProxySendResult { UINT sent{}; DWORD error{}; uint64_t duration{}; DWORD apiError{};
        uint64_t startedQpc{}, completedQpc{}, apiStartedQpc{}; };

    // Single bounded hand-off, not a second unbounded input queue. The hook/core
    // owns Submit/Observe/Poll; only the sender owns Take/Complete. Publication
    // uses acquire/release, and neither side waits for the other or takes a lock.
    // Buttons, wheels and recovery add a terminal, separately tagged MOVE.
    // Ordinary motion uses its own payload callback as the receipt and avoids
    // that duplicate; wheel/button callbacks themselves are not receipts.
    class ProxySendChannel
    {
    public:
        static constexpr uint64_t TimeoutMs = 500;
        enum class State { Idle, Queued, Running, Completed };
        bool Submit(ProxySendJob job, ULONG_PTR fenceCookie) noexcept
        {
            if (Busy() || !job.count || job.count > 2 || !ValidProxyInputCookie(fenceCookie) ||
                job.inputs[0].mi.dwExtraInfo == fenceCookie) return false;
            // Ordinary movement requires both API completion and its own MOVE
            // receipt. Buttons, wheels and recovery use a terminal tagged move.
            job.requiresFence = job.recovery || job.message != WM_MOUSEMOVE;
            if (job.requiresFence)
            {
                auto fence = job.inputs[0]; fence.mi.dwExtraInfo = fenceCookie;
                // The existing receipt fence is not another non-coalesced
                // movement. It only confirms the ordered recovery payload.
                fence.mi.dwFlags &= ~MOUSEEVENTF_MOVE_NOCOALESCE;
                job.inputs[job.count++] = fence;
            }
            m_job = job; m_fenceCookie = fenceCookie; m_fence = false;
            m_state.store(State::Queued, std::memory_order_release);
            return true;
        }
        bool Take(ProxySendJob& job) noexcept
        {
            if (m_state.load(std::memory_order_acquire) != State::Queued) return false;
            job = m_job;
            m_state.store(State::Running, std::memory_order_release);
            return true;
        }
        void Complete(ProxySendResult result) noexcept
        {
            m_result = result;
            m_state.store(State::Completed, std::memory_order_release);
        }
        void Observe(UINT message, DWORD flags, ULONG_PTR cookie) noexcept
        {
            auto state = m_state.load(std::memory_order_acquire);
            if ((state == State::Running || state == State::Completed) &&
                message == WM_MOUSEMOVE && (flags & LLMHF_INJECTED))
            {
                auto receiptCookie = m_job.requiresFence ? m_fenceCookie : m_job.inputs[0].mi.dwExtraInfo;
                if (cookie == receiptCookie) m_fence = true;
            }
        }
        bool Poll(uint64_t now, ProxySendResult& result) noexcept
        {
            if (m_state.load(std::memory_order_acquire) != State::Completed) return false;
            if (m_result.sent == m_job.count && !m_result.error && !m_fence && !Expired(now)) return false;
            result = m_result;
            if (!result.error && result.sent != m_job.count) result.error = ERROR_WRITE_FAULT;
            if (!result.error && !m_fence) result.error = ERROR_TIMEOUT;
            m_state.store(State::Idle, std::memory_order_release);
            return true;
        }
        bool Busy() const noexcept { return m_state.load(std::memory_order_acquire) != State::Idle; }
        bool Expired(uint64_t now) const noexcept { return Busy() && now - m_job.tick > TimeoutMs; }
        bool FenceSeen() const noexcept { return m_fence; }
        ProxySendJob const& Job() const noexcept { return m_job; } // Core/hook only.
    private:
        std::atomic<State> m_state{ State::Idle };
        ProxySendJob m_job{};
        ProxySendResult m_result{};
        ULONG_PTR m_fenceCookie{};
        bool m_fence{};
    };
}
