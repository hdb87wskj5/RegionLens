#pragma once
#include "CursorRecoveryPolicy.h"
#include "AppRuntime.h"

namespace RegionLens::native
{
    // Preserve raw CURSORINFO flags plus query/shape validity, without storing a
    // screen coordinate or a process-dependent cursor handle in diagnostics.
    inline int64_t CursorObservationBits(CursorObservation const& value) noexcept
    { return int64_t(value.flags) | (value.queried ? 0x100000000LL : 0) | (value.shape ? 0x200000000LL : 0); }

    // Read-only. Never change another thread's ShowCursor count, force a cursor
    // shape, synthesize movement, or change the user's mouse-vanish preference.
    inline CursorObservation ObserveSystemCursor() noexcept
    {
        CURSORINFO info{ sizeof(info) };
        bool ok = GetCursorInfo(&info) != FALSE;
        return { ok, info.flags, ok ? ERROR_SUCCESS : GetLastError(),
            reinterpret_cast<uintptr_t>(info.hCursor), info.ptScreenPos };
    }

    inline void RecordCursorVisibility(unsigned actor, unsigned step, uint64_t epoch,
        bool success, DWORD error, CursorObservation const& before, CursorObservation const& after) noexcept
    {
        if (!Runtime().diagnostics) return;
        struct Budget { uint64_t window{}; unsigned detail{}, anomaly{}; };
        thread_local Budget budget;
        auto now = GetTickCount64();
        if (now - budget.window >= 1000) { budget = {}; budget.window = now; }
        bool anomaly = !success || !after.queried || !after.shape;
        auto& count = anomaly ? budget.anomaly : budget.detail;
        if (count++ >= (anomaly ? 4u : 16u)) return;
        Record(DiagnosticEvent::CursorObservation,
            { actor, step, success, error, CursorObservationBits(before), CursorObservationBits(after),
              before.error, after.error }, anomaly, epoch);
    }
}
