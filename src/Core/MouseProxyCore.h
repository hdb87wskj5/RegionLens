#pragma once
#include <windows.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <limits>
#include "LensChrome.h"
#include "ProxyStatus.h"
#include "ProxyInputCookie.h"
#include "PointerSpeed.h"

namespace RegionLens::native
{
    enum class ProxyPhase { Off, Armed, Preparing, Active, Restoring, Failed };
    struct MappingSessionConfig
    {
        uint64_t generation{}, lensId{};
        HWND lensWindow{};
        RECT source{}, destination{}, lensClient{}, desktop{};
        bool fullscreen{}, blocked{};
        std::array<HWND, 16> lenses{};
        size_t lensCount{};
        PointerSpeed pointerSpeed{}; // In-process route preference, not watchdog shared state.
    };
    struct CursorSnapshot
    {
        uint64_t generation{}, lensId{};
        ProxyPhase phase{ ProxyPhase::Off };
        POINT position{}, sourcePosition{};
        HCURSOR shape{};
        DWORD error{};
        uint32_t buttons{};
        bool visible{};
        DWORD secondaryError{};
        ProxyFaultSite errorSite{}, secondarySite{};
        ProxyCancelReason cancelReason{};
        bool localMoveRequested{};
        // Monotonic within one routed lens. This carries only pointer interaction state.
        uint64_t interactionSequence{};
    };
    struct ProxyMouseEvent
    {
        UINT message{};
        POINT point{};
        double dx{}, dy{};
        DWORD data{};
        uint32_t physicalButtons{};
        bool controlDown{};
        // The hook stamps the virtual destination reached by this event.  This
        // lets the visible pointer follow hardware immediately while source
        // injection remains strictly ordered behind an outstanding batch.
        double virtualX{}, virtualY{};
        bool virtualValid{};
        uint64_t queuedQpc{}; // Diagnostics only; never a scheduling clock.
    };
    inline bool ProxyRectValid(RECT r) noexcept { return r.right > r.left && r.bottom > r.top; }
    inline RECT ProxyLensClientRect(MappingSessionConfig const& config) noexcept
    {
        // Older callers and configurations use destination for the complete
        // lens.  Aspect-fit fullscreen routes provide lensClient separately so
        // chrome remains anchored to the window rather than to the video.
        return ProxyRectValid(config.lensClient) ? config.lensClient : config.destination;
    }
    struct ProxyPointerGain { double x{ 1.0 }, y{ 1.0 }; };
    inline ProxyPointerGain PointerGainFor(MappingSessionConfig const& config) noexcept
    {
        // User-selected gain replaces automatic picture-scale matching. Source
        // geometry still controls inverse mapping, never a second acceleration.
        auto gain = config.pointerSpeed.Multiplier();
        return { gain, gain };
    }
    inline double ProxyAccumulateAxis(double position, double delta, double gain) noexcept
    {
        // Keep the same addition for gain=1. Saturation only protects extreme
        // input from escaping the LONG coordinates used by snapshots/SendInput.
        return std::clamp(position + delta * gain, double((std::numeric_limits<LONG>::min)()),
            double((std::numeric_limits<LONG>::max)()));
    }
    inline LONG ProxyMapAxis(double p, LONG from, LONG fromEnd, LONG to, LONG toEnd) noexcept
    {
        if (double(fromEnd) - from <= 1 || double(toEnd) - to <= 1) return to;
        double u = std::clamp((p - from) / (static_cast<double>(fromEnd) - from - 1), 0.0, 1.0);
        return static_cast<LONG>(int64_t(to) + std::llround(u * (static_cast<double>(toEnd) - to - 1)));
    }
    inline POINT ProxyMapPoint(double x, double y, RECT from, RECT to) noexcept
    {
        return { ProxyMapAxis(x, from.left, from.right, to.left, to.right),
                 ProxyMapAxis(y, from.top, from.bottom, to.top, to.bottom) };
    }
    inline LONG ProxyAbsoluteAxis(LONG p, LONG start, LONG end) noexcept
    {
        // Pixel centres avoid landing on the preceding pixel after integer conversion.
        if (end <= start) return 0;
        return static_cast<LONG>(std::clamp((static_cast<double>(p) - start + 0.5) * 65536.0 /
            (static_cast<double>(end) - start), 0.0, 65535.0));
    }
    inline bool ProxyLocalControl(MappingSessionConfig const& c, POINT p) noexcept
    {
        auto lens = ProxyLensClientRect(c);
        if (!PtInRect(&lens, p)) return false;
        POINT local{ p.x - lens.left, p.y - lens.top };
        RECT client{ 0, 0, lens.right - lens.left, lens.bottom - lens.top };
        return HitTestChromeButton(local, client) != 0 || (!c.fullscreen &&
            (local.x < 10 || local.y < 10 || local.x >= client.right - 10 || local.y >= client.bottom - 10));
    }
    inline uint32_t ProxyButtonBit(UINT message, DWORD data) noexcept
    {
        switch (message)
        {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: return 1;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: return 2;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: return 4;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: return HIWORD(data) == XBUTTON1 ? 8 : 16;
        default: return 0;
        }
    }
    inline bool ProxyButtonDown(UINT message) noexcept
    {
        return message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN ||
            message == WM_MBUTTONDOWN || message == WM_XBUTTONDOWN;
    }
    inline uint32_t ProxyUpdateButtons(uint32_t buttons, UINT message, DWORD data) noexcept
    {
        auto bit = ProxyButtonBit(message, data);
        return ProxyButtonDown(message) ? buttons | bit : buttons & ~bit;
    }

    class ProxyControlState
    {
    public:
        void Seed(bool left, bool right) noexcept { m_buttons = (left ? 1u : 0u) | (right ? 2u : 0u); }
        void Update(UINT message, DWORD key, DWORD flags) noexcept
        {
            if (flags & LLKHF_INJECTED) return;
            uint32_t bit = key == VK_RCONTROL ? 2u :
                (key == VK_LCONTROL || key == VK_CONTROL ? 1u : 0u);
            if (!bit) return;
            if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN) m_buttons |= bit;
            else if (message == WM_KEYUP || message == WM_SYSKEYUP) m_buttons &= ~bit;
        }
        [[nodiscard]] bool Down() const noexcept { return m_buttons != 0; }
    private:
        uint32_t m_buttons{};
    };

    struct ProxyInputBatch { std::array<INPUT, 2> inputs{}; UINT count{}; };
    enum class ProxyInputOrigin { Hardware, OwnInjection, ForeignInjection };
    inline ProxyInputOrigin ClassifyProxyInput(DWORD flags, ULONG_PTR cookie, ULONG_PTR ownCookie) noexcept
    {
        if (!(flags & LLMHF_INJECTED)) return ProxyInputOrigin::Hardware;
        return ValidProxyInputCookie(ownCookie) && cookie == ownCookie ?
            ProxyInputOrigin::OwnInjection : ProxyInputOrigin::ForeignInjection;
    }
    class ProxyPointerBaseline
    {
    public:
        // Only events actually passed to Windows advance the physical baseline.
        // Suppressed hardware moves are relative to the same physical position;
        // tagged injected moves rebase it but never enter virtual accumulation.
        void Passed(POINT point) noexcept { m_point = point; m_known = true; }
        ProxyMouseEvent Hardware(UINT message, POINT point, DWORD data) const noexcept
        {
            return { message, point, m_known ? double(point.x) - m_point.x : 0,
                m_known ? double(point.y) - m_point.y : 0, data };
        }
    private:
        POINT m_point{};
        bool m_known{};
    };
    inline ProxyInputBatch MakeProxyInputs(POINT point, UINT message, DWORD data, RECT desktop, ULONG_PTR cookie) noexcept
    {
        ProxyInputBatch batch;
        if (!ProxyRectValid(desktop) || !ValidProxyInputCookie(cookie)) return batch;
        auto& move = batch.inputs[0]; move.type = INPUT_MOUSE;
        move.mi.dx = ProxyAbsoluteAxis(point.x, desktop.left, desktop.right);
        move.mi.dy = ProxyAbsoluteAxis(point.y, desktop.top, desktop.bottom);
        // Preserve the ordinary Windows latest-position semantics. Explicitly
        // disabling move coalescing can flood a target's WM_MOUSEMOVE queue at
        // high polling rates, making a slider fall progressively behind until
        // the application drains that queue. Buttons and wheels remain distinct
        // INPUT records and are never coalesced by our ordered event queue.
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        move.mi.dwExtraInfo = cookie; batch.count = 1;
        if (message == WM_MOUSEMOVE) return batch;
        auto& button = batch.inputs[1]; button.type = INPUT_MOUSE; button.mi.dwExtraInfo = cookie;
        switch (message)
        {
        case WM_LBUTTONDOWN: button.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; break;
        case WM_LBUTTONUP: button.mi.dwFlags = MOUSEEVENTF_LEFTUP; break;
        case WM_RBUTTONDOWN: button.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; break;
        case WM_RBUTTONUP: button.mi.dwFlags = MOUSEEVENTF_RIGHTUP; break;
        case WM_MBUTTONDOWN: button.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; break;
        case WM_MBUTTONUP: button.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; break;
        case WM_XBUTTONDOWN: button.mi.dwFlags = MOUSEEVENTF_XDOWN; button.mi.mouseData = HIWORD(data); break;
        case WM_XBUTTONUP: button.mi.dwFlags = MOUSEEVENTF_XUP; button.mi.mouseData = HIWORD(data); break;
        case WM_MOUSEWHEEL: button.mi.dwFlags = MOUSEEVENTF_WHEEL; button.mi.mouseData = DWORD(SHORT(HIWORD(data))); break;
        case WM_MOUSEHWHEEL: button.mi.dwFlags = MOUSEEVENTF_HWHEEL; button.mi.mouseData = DWORD(SHORT(HIWORD(data))); break;
        default: batch.count = 0; return batch;
        }
        batch.count = 2; return batch;
    }

    struct ProxyRecoveryBatch { std::array<INPUT, 7> inputs{}; UINT count{}; };
    inline ProxyRecoveryBatch MakeProxyRecoveryInputs(POINT source, POINT destination, uint32_t buttons,
        RECT desktop, ULONG_PTR cookie) noexcept
    {
        ProxyRecoveryBatch result;
        if (!ProxyRectValid(desktop) || !ValidProxyInputCookie(cookie)) return result;
        result.inputs[result.count++] = MakeProxyInputs(source, WM_MOUSEMOVE, 0, desktop, cookie).inputs[0];
        constexpr UINT ups[] = { WM_LBUTTONUP, WM_RBUTTONUP, WM_MBUTTONUP, WM_XBUTTONUP, WM_XBUTTONUP };
        for (unsigned i = 0; i < 5; ++i)
        {
            if (!(buttons & (1u << i))) continue;
            auto data = i >= 3 ? DWORD((i == 3 ? XBUTTON1 : XBUTTON2) << 16) : 0;
            result.inputs[result.count++] = MakeProxyInputs(source, ups[i], data, desktop, cookie).inputs[1];
        }
        result.inputs[result.count++] = MakeProxyInputs(destination, WM_MOUSEMOVE, 0, desktop, cookie).inputs[0];
        return result;
    }

    class ProxyEventQueue
    {
    public:
        static constexpr size_t Capacity = 256;
        bool Push(ProxyMouseEvent const& event) noexcept
        {
            if (m_size && event.message == WM_MOUSEMOVE)
            {
                auto& previous = m_events[(m_head + m_size - 1) % Capacity];
                if (previous.message == WM_MOUSEMOVE && previous.physicalButtons == event.physicalButtons)
                {
                    previous.dx += event.dx; previous.dy += event.dy; previous.point = event.point;
                    previous.virtualX = event.virtualX; previous.virtualY = event.virtualY;
                    previous.virtualValid = event.virtualValid;
                    previous.queuedQpc = event.queuedQpc;
                    return true;
                }
            }
            if (m_size == Capacity) return false;
            m_events[(m_head + m_size++) % Capacity] = event;
            return true;
        }
        bool Pop(ProxyMouseEvent& event) noexcept
        {
            if (!m_size) return false;
            event = m_events[m_head]; m_head = (m_head + 1) % Capacity; --m_size;
            return true;
        }
        void Clear() noexcept { m_head = m_size = 0; }
        size_t Size() const noexcept { return m_size; }
    private:
        std::array<ProxyMouseEvent, Capacity> m_events{};
        size_t m_head{}, m_size{};
    };

    // Only this interface can reach real input APIs. Core tests substitute a fake.
    class IProxyBackend
    {
    public:
        virtual ~IProxyBackend() = default;
        virtual bool Healthy() = 0;
        virtual uint64_t NowTick() = 0;
        virtual bool CanEnter(MappingSessionConfig const&, POINT) = 0;
        virtual bool SourceAccessible(MappingSessionConfig const&, POINT) = 0;
        virtual bool DestinationExposed(MappingSessionConfig const&, POINT) = 0;
        virtual bool CursorVisible(bool) = 0;
        // True means accepted. Production completes asynchronously and reports
        // InjectionCompleted/InjectionFailed before InputSettled permits reuse.
        virtual bool SendMouse(POINT, UINT message, DWORD data, RECT desktop) = 0;
        virtual bool InputSettled() = 0;
        virtual void EventDequeued(uint64_t) noexcept {}
        virtual bool RecoveryCompletedExternally() { return false; }
        virtual void RequestSurfaces(uint64_t request, bool transparent) = 0;
        virtual void RecoveryState(bool active, POINT virtualPoint, POINT sourcePoint, uint32_t buttons) = 0;
        virtual void RecoveryFailed() = 0;
        virtual void Publish(CursorSnapshot const&) = 0;
    };

    class MouseProxyCore
    {
    public:
        explicit MouseProxyCore(IProxyBackend& backend) : m_backend(backend) {}
        void Configure(MappingSessionConfig const& config);
        void Disable(DWORD error = 0, ProxyFaultSite site = ProxyFaultSite::None);
        void InjectionFailed(DWORD error, ProxyFaultSite site = ProxyFaultSite::ActiveSend);
        void InjectionCompleted(); // Successful normal batch, never a recovery batch.
        // Hook-safe: bounded arithmetic/queue operations only, no backend calls.
        bool Intercept(ProxyMouseEvent event) noexcept;
        void Pump();
        void SurfaceAcknowledged(uint64_t request, bool success);
        void ExternalInterruption() { Disable(ERROR_CANCELLED, ProxyFaultSite::HealthCheck); }
        ProxyPhase Phase() const noexcept { return m_phase; }
        MappingSessionConfig const& Configuration() const noexcept { return m_config; }
        CursorSnapshot Snapshot() const noexcept;
        size_t InputQueueDepth() const noexcept { return m_queue.Size(); }
        uint64_t SurfaceRequest() const noexcept { return m_surfaceRequest; }
        void ResetAfterStop() noexcept;
        void SeedPhysicalButtons(uint32_t buttons) noexcept { m_physicalButtons = buttons; }
        bool InputReleased() const noexcept { return !m_gate || (m_phase == ProxyPhase::Restoring && m_restoreRequested); }
        int CursorArea(POINT point) const noexcept
        {
            return (PtInRect(&m_config.source, point) ? 1 : 0) | (PtInRect(&m_config.destination, point) ? 2 : 0);
        }
    private:
        void Process(ProxyMouseEvent const& event);
        void Restore(bool rearm, DWORD error = 0, ProxyFaultSite site = ProxyFaultSite::None);
        void NoteFailure(DWORD error, ProxyFaultSite site);
        void ProgressRestore();
        void RestoreCursorVisibility(bool deferFailure = false);
        void Publish(bool localMoveRequested = false);
        POINT Position() const noexcept;
        POINT DispatchPosition() const noexcept;
        POINT Source() const noexcept;
        bool Content(POINT p) const noexcept;
        bool Send(UINT message = WM_MOUSEMOVE, DWORD data = 0);
        IProxyBackend& m_backend;
        MappingSessionConfig m_config{};
        ProxyPointerGain m_pointerGain;
        std::optional<MappingSessionConfig> m_nextConfig;
        ProxyPhase m_phase{ ProxyPhase::Off };
        ProxyEventQueue m_queue;
        double m_x{}, m_y{};
        double m_dispatchX{}, m_dispatchY{};
        POINT m_lastSentSource{};
        uint32_t m_buttons{}, m_physicalButtons{}, m_obligations{}, m_restoreButtons{};
        uint64_t m_surfaceRequest{};
        uint64_t m_prepareStarted{};
        uint64_t m_lastExposureCheck{};
        uint64_t m_interactionSequence{};
        ProxyFailureHistory m_failures;
        bool m_gate{}, m_overflow{}, m_invalidMotion{}, m_hidden{}, m_surfaces{}, m_restoreRequested{}, m_rearm{};
        bool m_cleanupFailed{}, m_restoreMove{}, m_restoreVisibilityAttempted{}, m_cursorRestoreFailed{};
        bool m_localMovePending{};
        bool m_lastSentSourceValid{};
        bool m_exposureCheckValid{};
        bool m_cursorTouched{}; // Survives Armed/Off; failure recovery still owes a show.
    };
}
