#pragma once

#include "MouseProxyCore.h"
#include <array>
#include <cstddef>
#include <cstdint>

namespace RegionLens::native
{
    enum class MappingRouteDecision
    {
        Reject,
        Keep,
        Change
    };

    class MappingStandbyPolicy
    {
    public:
        static constexpr size_t Capacity = 16;

        bool Arm(uint64_t id) noexcept
        {
            if (!id || IsArmed(id)) return id != 0;
            if (m_count == m_armed.size()) return false;
            m_armed[m_count++] = id;
            return true;
        }

        bool Disarm(uint64_t id) noexcept
        {
            for (size_t i = 0; i < m_count; ++i)
            {
                if (m_armed[i] != id) continue;
                for (size_t j = i + 1; j < m_count; ++j) m_armed[j - 1] = m_armed[j];
                m_armed[--m_count] = 0;
                if (m_routed == id) m_routed = 0;
                if (m_resumeCandidate == id) m_resumeCandidate = 0;
                return true;
            }
            return false;
        }

        void Clear() noexcept
        {
            m_armed.fill(0);
            m_count = 0;
            m_routed = 0;
            m_resumeCandidate = 0;
            m_suspended = false;
        }

        void Suspend() noexcept
        {
            if (m_suspended) return;
            m_resumeCandidate = m_routed;
            m_routed = 0;
            m_suspended = true;
        }

        [[nodiscard]] uint64_t Resume() noexcept
        {
            if (!m_suspended) return 0;
            m_suspended = false;
            auto candidate = IsArmed(m_resumeCandidate) ? m_resumeCandidate : 0;
            m_resumeCandidate = 0;
            return candidate;
        }

        [[nodiscard]] bool IsArmed(uint64_t id) const noexcept
        {
            for (size_t i = 0; i < m_count; ++i) if (m_armed[i] == id) return true;
            return false;
        }

        [[nodiscard]] bool Empty() const noexcept { return m_count == 0; }
        [[nodiscard]] size_t Count() const noexcept { return m_count; }
        [[nodiscard]] uint64_t Routed() const noexcept { return m_routed; }
        [[nodiscard]] bool Suspended() const noexcept { return m_suspended; }

        [[nodiscard]] MappingRouteDecision Evaluate(
            uint64_t candidate, ProxyPhase phase, bool transitionPending) const noexcept
        {
            if (m_suspended || !IsArmed(candidate)) return MappingRouteDecision::Reject;
            if (candidate == m_routed) return MappingRouteDecision::Keep;
            if (transitionPending || phase == ProxyPhase::Failed) return MappingRouteDecision::Reject;
            if (!m_routed) return MappingRouteDecision::Change;
            return phase == ProxyPhase::Armed
                ? MappingRouteDecision::Change
                : MappingRouteDecision::Reject;
        }

        bool CommitRoute(uint64_t id) noexcept
        {
            if (m_suspended || !IsArmed(id)) return false;
            m_routed = id;
            return true;
        }

        void ClearRoute() noexcept { m_routed = 0; }

    private:
        std::array<uint64_t, Capacity> m_armed{};
        size_t m_count{};
        uint64_t m_routed{};
        uint64_t m_resumeCandidate{};
        bool m_suspended{};
    };
}
