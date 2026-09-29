#include "pch.h"
#include "UiCursorOwnership.h"
#include "CursorObservationNative.h"

namespace RegionLens::native
{
    namespace
    {
        class NativeUiCursorBackend final : public IUiCursorBackend
        {
        public:
            HCURSOR Shape() noexcept override { return GetCursor(); }
            HCURSOR Arrow() noexcept override { return LoadCursorW(nullptr, IDC_ARROW); }
            void SetShape(HCURSOR shape) noexcept override { SetCursor(shape); }
            bool Position(POINT& point) noexcept override { return GetCursorPos(&point) != FALSE; }
            HWND Hit(POINT point) noexcept override { return WindowFromPoint(point); }
            HWND Capture() noexcept override { return GetCapture(); }
            CursorObservation Observe() noexcept override { return ObserveSystemCursor(); }
        };
    }

    UiCursorOwnership& UiCursorOwnership::ForCurrentThread()
    {
        thread_local NativeUiCursorBackend backend;
        thread_local UiCursorOwnership ownership(backend);
        return ownership;
    }

    uint64_t UiCursorOwnership::Acquire(HWND owner, uint64_t lens, UiCursorPurpose purpose,
        POINT expected, bool eligible, uint32_t phase) noexcept
    {
        POINT current{};
        if (!owner || !eligible || !m_backend.Position(current) ||
            current.x != expected.x || current.y != expected.y) return 0;
        auto capture = m_backend.Capture();
        if (purpose == UiCursorPurpose::Drag ? capture != owner :
            ((capture && capture != owner) || m_backend.Hit(current) != owner)) return 0;
        auto arrow = m_backend.Arrow();
        if (!arrow) return 0; // Never hide unless a concrete restoration shape is available.
        bool newOwner = m_owner != owner || m_purpose != purpose || !m_epoch;
        if (newOwner)
        {
            if (m_owner && !Release(m_owner, m_epoch, UiCursorRelease::Replaced, phase)) return 0;
            m_owner = owner; m_lens = lens; m_purpose = purpose;
            m_epoch = ++m_nextEpoch; m_restoreArrow = arrow;
        }
        auto before = m_backend.Shape();
        // A native WM_SETCURSOR can set a new shape between two software frames.
        // Reassert only while we still own the live hit/capture, never from an old point.
        if (before) m_backend.SetShape(nullptr);
        auto after = m_backend.Shape();
        if (newOwner || after) Trace(true, uint32_t(purpose), phase, before, after, !after);
        if (after) { Release(owner, m_epoch, UiCursorRelease::StaleMessage, phase); return 0; }
        return m_epoch;
    }

    bool UiCursorOwnership::Release(HWND owner, uint64_t epoch, UiCursorRelease reason, uint32_t phase) noexcept
    {
        if (!Owns(owner, epoch)) return true; // Old cleanup cannot discharge another lens's hide.
        auto before = m_backend.Shape();
        // Undo only our thread's null shape. No WindowFromPoint condition here:
        // by WM_MOUSELEAVE the physical pointer has already reached another app.
        if (!before) m_backend.SetShape(m_restoreArrow);
        auto after = m_backend.Shape();
        Trace(false, uint32_t(reason), phase, before, after, after != nullptr);
        if (!after) return false; // Retain the obligation for explicit cancel/close.
        m_owner = nullptr; m_epoch = m_lens = 0; m_restoreArrow = nullptr;
        return true;
    }

    void UiCursorOwnership::NativeArrow() noexcept
    {
        if (auto arrow = m_backend.Arrow()) m_backend.SetShape(arrow);
    }

    bool UiCursorOwnership::RefreshNativeAfterShow(HWND owner, POINT expected,
        HCURSOR desired, HCURSOR alternate) noexcept
    {
        POINT current{};
        if (!owner || !desired || !alternate || desired == alternate ||
            !m_backend.Position(current) || current.x != expected.x || current.y != expected.y ||
            m_backend.Hit(current) != owner ||
            (m_backend.Capture() && m_backend.Capture() != owner)) return false;

        // MagShowSystemCursor restores visibility, but a stationary pointer in
        // our window may receive no new WM_SETCURSOR. A different *non-null*
        // shape forces a local cursor-image transition while the software
        // overlay still covers the handoff. Never set a foreign thread's shape.
        if (m_backend.Shape() == desired)
        {
            m_backend.SetShape(alternate);
            if (m_backend.Shape() != alternate) return false;
        }
        m_backend.SetShape(desired);
        return m_backend.Shape() == desired;
    }

    void UiCursorOwnership::Trace(bool acquire, uint32_t reason, uint32_t phase,
        HCURSOR before, HCURSOR after, bool success) noexcept
    {
        if (!Runtime().diagnostics) return;
        auto now = GetTickCount64();
        if (now - m_traceWindow >= 1000) { m_traceWindow = now; m_traceCount = m_anomalyCount = 0; }
        auto observation = m_backend.Observe();
        // A null global shape is expected while acquiring a software cursor,
        // but is a separate diagnostic anomaly after releasing our shape.
        // Observation never changes the ownership/recovery completion policy.
        bool anomaly = !success || !observation.queried || (!acquire && !observation.shape);
        auto& count = anomaly ? m_anomalyCount : m_traceCount;
        if (count++ >= (anomaly ? 4u : 32u)) return;
        Record(DiagnosticEvent::UiCursor,
            { acquire, reason, phase, before != nullptr, after != nullptr,
              CursorObservationBits(observation), observation.error, success }, anomaly, m_epoch, m_lens);
    }
}
