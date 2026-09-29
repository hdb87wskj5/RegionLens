#pragma once
#include "AppRuntime.h"

namespace RegionLens::native
{
    // Low-frequency milestones only. Sink implementations must never block the
    // input hook: the development incident journal is pre-mapped at startup.
    class DiagnosticScope
    {
    public:
        explicit DiagnosticScope(DiagnosticStage stage, uint64_t peer = 0) noexcept
            : m_stage(stage), m_peer(peer), m_started(GetTickCount64())
        { Emit(0, 0); }
        ~DiagnosticScope() { if (!m_finished) Emit(2, GetLastError()); }
        void End(DWORD result = 0, uint64_t peer = 0) noexcept
        { if (!m_finished) { if (peer) m_peer = peer; Emit(1, result); m_finished = true; } }
    private:
        void Emit(int state, DWORD result) noexcept
        { Record(DiagnosticEvent::Checkpoint, { int64_t(m_stage), state, result,
            int64_t(GetCurrentProcessId()), int64_t(m_peer), int64_t(GetTickCount64() - m_started) }, state == 2); }
        DiagnosticStage m_stage;
        uint64_t m_peer{}, m_started{};
        bool m_finished{};
    };
    inline void RecordProcessExit(HANDLE process, bool parent, DWORD wait) noexcept
    {
        if (!Runtime().diagnostics || !process) return;
        auto saved = GetLastError();
        DWORD code{}, error{};
        if (!GetExitCodeProcess(process, &code)) error = GetLastError();
        Record(DiagnosticEvent::ProcessExit, { GetProcessId(process), parent, wait, code, error },
            wait == WAIT_FAILED || (wait == WAIT_OBJECT_0 && code != 0));
        SetLastError(saved);
    }
}
