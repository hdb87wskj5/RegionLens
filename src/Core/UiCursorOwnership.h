#pragma once

#include "CursorRecoveryPolicy.h"

namespace RegionLens::native
{
    enum class UiCursorPurpose : uint32_t { Chrome = 1, Drag };
    enum class UiCursorRelease : uint32_t
    {
        PointerLeft = 1, Replaced, MappingDisabled, PassThrough, Hidden,
        Closed, Paused, DragEnded, CaptureLost, Geometry, StaleMessage
    };

    // Thread-local cursor shape and global magnification visibility are separate
    // obligations. This backend is replaceable without changing desktop input.
    class IUiCursorBackend
    {
    public:
        virtual ~IUiCursorBackend() = default;
        virtual HCURSOR Shape() noexcept = 0;
        virtual HCURSOR Arrow() noexcept = 0;
        virtual void SetShape(HCURSOR shape) noexcept = 0;
        virtual bool Position(POINT& point) noexcept = 0;
        virtual HWND Hit(POINT point) noexcept = 0;
        virtual HWND Capture() noexcept = 0;
        virtual CursorObservation Observe() noexcept = 0;
    };

    class UiCursorOwnership
    {
    public:
        struct State
        {
            HWND owner{};
            uint64_t lens{}, epoch{};
            UiCursorPurpose purpose{};
        };
        explicit UiCursorOwnership(IUiCursorBackend& backend) : m_backend(backend) {}
        static UiCursorOwnership& ForCurrentThread();
        [[nodiscard]] State Inspect() const noexcept
        { return { m_owner, m_lens, m_epoch, m_owner ? m_purpose : UiCursorPurpose{} }; }
        uint64_t Acquire(HWND owner, uint64_t lens, UiCursorPurpose purpose,
            POINT expected, bool eligible, uint32_t phase) noexcept;
        bool Release(HWND owner, uint64_t epoch, UiCursorRelease reason, uint32_t phase) noexcept;
        bool Owns(HWND owner, uint64_t epoch) const noexcept
        { return owner && epoch && owner == m_owner && epoch == m_epoch; }
        bool Position(POINT& point) noexcept { return m_backend.Position(point); }
        HWND Hit(POINT point) noexcept { return m_backend.Hit(point); }
        HWND Capture() noexcept { return m_backend.Capture(); }
        HCURSOR Arrow() noexcept { return m_backend.Arrow(); }
        void NativeArrow() noexcept;
        bool RefreshNativeAfterShow(HWND owner, POINT expected,
            HCURSOR desired, HCURSOR alternate) noexcept;
    private:
        void Trace(bool acquire, uint32_t reason, uint32_t phase, HCURSOR before,
            HCURSOR after, bool success) noexcept;
        IUiCursorBackend& m_backend;
        HWND m_owner{};
        uint64_t m_epoch{}, m_nextEpoch{}, m_lens{};
        UiCursorPurpose m_purpose{};
        HCURSOR m_restoreArrow{};
        uint64_t m_traceWindow{};
        unsigned m_traceCount{}, m_anomalyCount{};
    };
}
