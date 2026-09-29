#pragma once
#include "AppRuntime.h"
#include "ProbeProtocol.h"

namespace RegionLens::weTypeCompat
{
    class WeTypeProbeHost final : public native::IWeTypePreChangeProbe
    {
    public:
        ~WeTypeProbeHost() override;
        bool Start(native::WeTypeProbeMode mode) noexcept override;
        void Stop(native::WeTypeProbeStopReason reason) noexcept override;
        void Pump() noexcept override;
        void UpdateTargets(std::span<native::WeTypeProbeLens const> targets) noexcept override;
        [[nodiscard]] bool Enabled() const noexcept override { return m_hook != nullptr; }
        [[nodiscard]] bool HasTargets() const noexcept override { return m_targetCount != 0; }
        [[nodiscard]] native::WeTypeProbeMode Mode() const noexcept override { return m_mode; }
        [[nodiscard]] HWND InterceptionTarget() const noexcept override;
        void ReportUnexpectedOrder(HWND window) noexcept override;
        native::WeTypeProbeStopReason TakeStopReason() noexcept override
        { auto result = m_stopReason; m_stopReason = native::WeTypeProbeStopReason::None; return result; }

    private:
        bool DrainEvents() noexcept;
        HWND m_target{};
        DWORD m_pid{}, m_thread{};
        uint64_t m_creationTime{};
        uint64_t m_started{};
        uint64_t m_lastIdentityCheck{};
        uint64_t m_lastSummary{};
        uint64_t m_nonce{};
        HMODULE m_localDll{};
        HHOOK m_hook{};
        HANDLE m_mapping{};
        weTypeProbe::ProbeShared* m_shared{};
        LONG m_readSequence{};
        weTypeProbe::ProbeLens m_targets[weTypeProbe::LensCapacity]{};
        LONG m_targetCount{};
        uint64_t m_preCount{}, m_postCount{}, m_recorded{};
        native::WeTypeProbeMode m_mode{ native::WeTypeProbeMode::Observe };
        native::WeTypeProbeStopReason m_stopReason{ native::WeTypeProbeStopReason::None };
    };
}
