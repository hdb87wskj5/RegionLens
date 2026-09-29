#pragma once
#include <cstdint>

namespace RegionLens::native
{
    enum class MappingRouteResult { Routed, Deferred, Cancelled, Failed };
    enum class MappingOperation { Idle, Starting, Updating, Releasing, Pumping, Stopping };
    // UI-thread reentrancy, NOT the input worker's Off/Armed/Active state. A
    // cursor snapshot must never release this guard while an OS call is pending.
    class InputMappingLifecycle
    {
    public:
        bool Begin(MappingOperation operation) noexcept
        {
            if (Busy() || m_shutdown) return false;
            m_operation = operation; return true;
        }
        void RequestStop(bool shutdown = false) noexcept { m_stop = true; m_shutdown |= shutdown; }
        bool Busy() const noexcept { return m_operation != MappingOperation::Idle; }
        bool Cancelled() const noexcept { return m_stop || m_shutdown; }
        bool StopPending() const noexcept { return m_stop; }
        bool Shutdown() const noexcept { return m_shutdown; }
        MappingOperation Operation() const noexcept { return m_operation; }
        void Stopping() noexcept { m_operation = MappingOperation::Stopping; }
        void Finish() noexcept { m_stop = false; m_operation = MappingOperation::Idle; }
    private:
        MappingOperation m_operation{ MappingOperation::Idle };
        bool m_stop{}, m_shutdown{};
    };
}
