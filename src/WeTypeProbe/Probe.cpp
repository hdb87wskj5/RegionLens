#include "ProbeProtocol.h"
#include <commctrl.h>
#include <climits>
#include <cwchar>

using namespace RegionLens::weTypeProbe;

namespace
{
    HANDLE mapping{};
    ProbeShared* shared{};
    HWND target{};
    UINT detachMessage{};
    constexpr UINT_PTR SubclassId = 0x57545042;
    bool pinned{};
    LRESULT CALLBACK CandidateSubclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

    void ReleaseTarget(HWND window) noexcept
    {
        if (window) RemoveWindowSubclass(window, CandidateSubclass, SubclassId);
        if (shared) InterlockedExchange(&shared->detached, 1);
        target = nullptr;
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        shared = nullptr;
        mapping = nullptr;
    }

    bool ReadTargets(ProbeLens (&targets)[LensCapacity], LONG& count) noexcept
    {
        if (!shared) return false;
        auto first = InterlockedCompareExchange(&shared->targetGeneration, 0, 0);
        if (first & 1) return false;
        count = shared->targetCount;
        if (count < 0 || count > LensCapacity) return false;
        for (LONG index = 0; index < count; ++index) targets[index] = shared->targets[index];
        MemoryBarrier();
        return first == InterlockedCompareExchange(&shared->targetGeneration, 0, 0);
    }

    bool ProposedBounds(HWND window, WINDOWPOS const& position, RECT& bounds) noexcept
    {
        RECT current{};
        if (!GetWindowRect(window, &current)) return false;
        auto x = (position.flags & SWP_NOMOVE) ? int64_t(current.left) : position.x;
        auto y = (position.flags & SWP_NOMOVE) ? int64_t(current.top) : position.y;
        auto width = (position.flags & SWP_NOSIZE) ?
            int64_t(current.right) - current.left : position.cx;
        auto height = (position.flags & SWP_NOSIZE) ?
            int64_t(current.bottom) - current.top : position.cy;
        if (width <= 0 || height <= 0 || x < INT_MIN || y < INT_MIN ||
            x + width > INT_MAX || y + height > INT_MAX) return false;
        bounds = { LONG(x), LONG(y), LONG(x + width), LONG(y + height) };
        return true;
    }

    HWND InterceptionAnchor(HWND window, WINDOWPOS const& position) noexcept
    {
        if (!shared || shared->mode != ProbeMode::Intercept ||
            (position.flags & SWP_HIDEWINDOW) || position.hwnd != window ||
            (!(position.flags & SWP_NOZORDER) &&
                (position.hwndInsertAfter == HWND_BOTTOM ||
                 position.hwndInsertAfter == HWND_NOTOPMOST))) return nullptr;
        RECT proposed{};
        if (!ProposedBounds(window, position, proposed)) return nullptr;
        ProbeLens targets[LensCapacity]{};
        LONG count{};
        if (!ReadTargets(targets, count) || !count) return nullptr;
        HWND eligible[LensCapacity]{};
        LONG eligibleCount{};
        for (LONG index = 0; index < count; ++index)
        {
            auto lens = reinterpret_cast<HWND>(targets[index].window);
            DWORD owner{};
            RECT actual{}, overlap{};
            if (lens && lens != window &&
                GetWindowThreadProcessId(lens, &owner) && owner == shared->hostProcess &&
                IsWindowVisible(lens) &&
                (GetWindowLongPtrW(lens, GWL_EXSTYLE) & WS_EX_TOPMOST) &&
                GetWindowRect(lens, &actual) && EqualRect(&actual, &targets[index].bounds) &&
                IntersectRect(&overlap, &proposed, &actual))
                eligible[eligibleCount++] = lens;
        }
        if (!eligibleCount) return nullptr;

        // A complete bounded desktop walk chooses the lowest overlapping lens.
        // Inserting after it preserves the candidate's topmost tier while keeping
        // every overlapping lens in front. Incomplete order fails open.
        HWND anchor{};
        LONG found{};
        bool candidateSeen{};
        bool candidateAboveLens{};
        int steps{};
        for (auto current = GetTopWindow(nullptr); current && steps < 2048;
            current = GetWindow(current, GW_HWNDNEXT), ++steps)
        {
            if (current == window) candidateSeen = true;
            for (LONG index = 0; index < eligibleCount; ++index)
                if (current == eligible[index])
                {
                    anchor = current;
                    ++found;
                    if (candidateSeen) candidateAboveLens = true;
                    break;
                }
            if (found == eligibleCount && candidateSeen) break;
        }
        if (found != eligibleCount || !candidateSeen) return nullptr;
        if ((position.flags & SWP_NOZORDER) && !candidateAboveLens) return nullptr;
        if (!(position.flags & SWP_NOZORDER) && position.hwndInsertAfter == anchor)
            return nullptr;
        return anchor;
    }

    void ClassifyLayer(HWND window, ProbeEvent& event) noexcept
    {
        if (!shared || !IsWindowVisible(window)) return;
        ProbeLens targets[LensCapacity]{};
        LONG count{};
        if (!ReadTargets(targets, count)) return;
        RECT candidate{};
        if (!GetWindowRect(window, &candidate)) return;
        uint64_t overlapping[LensCapacity]{};
        for (LONG index = 0; index < count; ++index)
        {
            RECT intersect{};
            if (IntersectRect(&intersect, &candidate, &targets[index].bounds) &&
                IsWindowVisible(reinterpret_cast<HWND>(targets[index].window)))
                overlapping[event.overlapLensCount++] = targets[index].window;
        }
        if (!event.overlapLensCount) return;
        int steps{};
        for (auto below = GetWindow(window, GW_HWNDNEXT);
            below && steps < 512 && event.aboveLensCount < event.overlapLensCount;
            below = GetWindow(below, GW_HWNDNEXT), ++steps)
            for (DWORD index = 0; index < event.overlapLensCount; ++index)
                if (overlapping[index] == reinterpret_cast<uint64_t>(below))
                { ++event.aboveLensCount; overlapping[index] = 0; }
    }

    void Record(HWND window, UINT message, WINDOWPOS* position) noexcept
    {
        if (!shared || InterlockedCompareExchange(&shared->active, 0, 0) != 1) return;
        auto nowTick = GetTickCount64();
        auto heartbeat = InterlockedCompareExchange64(&shared->hostHeartbeatTick, 0, 0);
        if (nowTick < uint64_t(heartbeat) || nowTick - uint64_t(heartbeat) > 1000)
        {
            InterlockedExchange(&shared->active, 0);
            return;
        }
        LARGE_INTEGER started{};
        QueryPerformanceCounter(&started);
        if (message == WM_WINDOWPOSCHANGING) InterlockedIncrement(&shared->preChangeCount);
        else if (message == WM_WINDOWPOSCHANGED) InterlockedIncrement(&shared->postChangeCount);
        auto originalFlags = position ? position->flags : 0;
        auto originalInsertAfter = position ? reinterpret_cast<uint64_t>(position->hwndInsertAfter) : 0;
        HWND anchor{};
        if (message == WM_WINDOWPOSCHANGING && position &&
            InterlockedCompareExchange(&shared->attached, 0, 0) == 1)
        {
            anchor = InterceptionAnchor(window, *position);
            if (anchor)
            {
                position->hwndInsertAfter = anchor;
                position->flags &= ~SWP_NOZORDER;
                InterlockedIncrement(&shared->interceptCount);
            }
        }
        ProbeEvent layer{};
        bool failed{};
        if (message == WM_WINDOWPOSCHANGED && shared->mode == ProbeMode::Intercept)
        {
            ClassifyLayer(window, layer);
            failed = layer.overlapLensCount && layer.aboveLensCount;
            if (failed)
            {
                InterlockedCompareExchange(&shared->failureCode, 1, 0);
                InterlockedExchange(&shared->active, 0);
            }
        }
        // The long-running mode keeps only its first bounded sample and a
        // possible failure record. Every later message still checks the layer
        // and updates aggregate counters, but cannot overflow the event ring.
        if (shared->mode == ProbeMode::Intercept &&
            InterlockedCompareExchange(&shared->writeSequence, 0, 0) >= InterceptEventLimit && !failed)
            return;
        auto sequence = InterlockedIncrement(&shared->writeSequence);
        auto& event = shared->events[(sequence - 1) % EventCapacity];
        InterlockedExchange(&event.committed, 0);
        event.sequence = sequence;
        event.message = message;
        event.window = reinterpret_cast<uint64_t>(window);
        event.originalFlags = originalFlags;
        event.originalInsertAfter = originalInsertAfter;
        event.anchor = reinterpret_cast<uint64_t>(anchor);
        event.rewritten = anchor ? 1 : 0;
        event.flags = position ? position->flags : 0;
        event.insertAfter = position ? reinterpret_cast<uint64_t>(position->hwndInsertAfter) : 0;
        event.x = event.y = event.width = event.height = 0;
        event.overlapLensCount = event.aboveLensCount = 0;
        if (position)
        {
            event.x = position->x; event.y = position->y;
            event.width = position->cx; event.height = position->cy;
        }
        event.qpc = started.QuadPart;
        event.tick = GetTickCount64();
        event.previous = reinterpret_cast<uint64_t>(GetWindow(window, GW_HWNDPREV));
        event.topmost = (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOPMOST) ? 1u : 0u;
        if (message == WM_WINDOWPOSCHANGED && shared->mode == ProbeMode::Intercept)
        { event.overlapLensCount = layer.overlapLensCount; event.aboveLensCount = layer.aboveLensCount; }
        else ClassifyLayer(window, event);
        LARGE_INTEGER finished{};
        QueryPerformanceCounter(&finished);
        event.durationQpc = finished.QuadPart - started.QuadPart;
        MemoryBarrier();
        InterlockedExchange(&event.committed, sequence);
    }

    LRESULT CALLBACK CandidateSubclass(HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR, DWORD_PTR)
    {
        if (message == WM_WINDOWPOSCHANGING || message == WM_WINDOWPOSCHANGED)
            Record(window, message, reinterpret_cast<WINDOWPOS*>(lParam));
        if ((detachMessage && message == detachMessage) || message == WM_NCDESTROY)
        {
            ReleaseTarget(window);
        }
        return DefSubclassProc(window, message, wParam, lParam);
    }

    void TryAttach(CWPSTRUCT const& call) noexcept
    {
        static UINT const bootstrapMessage = RegisterWindowMessageW(BootstrapMessage);
        if (!bootstrapMessage || call.message != bootstrapMessage ||
            !call.hwnd || !call.wParam || !call.lParam) return;
        // A timed-out host can leave a pass-through subclass on this thread.
        // A later bootstrap replaces it only after its old lease is inactive.
        if (target)
        {
            if (shared && InterlockedCompareExchange(&shared->active, 0, 0) == 1) return;
            ReleaseTarget(target);
        }
        auto host = static_cast<DWORD>(call.wParam);
        auto nonce = static_cast<uint64_t>(call.lParam);
        wchar_t name[160]{};
        if (swprintf_s(name, L"%ls%lu.%lu.%llu", MappingPrefix,
            host, GetCurrentProcessId(), static_cast<unsigned long long>(nonce)) <= 0) return;
        auto opened = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name);
        if (!opened) return;
        auto view = static_cast<ProbeShared*>(MapViewOfFile(opened, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
            sizeof(ProbeShared)));
        if (!view)
        {
            CloseHandle(opened);
            return;
        }
        auto valid = view->magic == ProtocolMagic && view->version == ProtocolVersion &&
            (view->mode == ProbeMode::Observe || view->mode == ProbeMode::Intercept) &&
            view->hostProcess == host && view->targetProcess == GetCurrentProcessId() &&
            view->targetThread == GetCurrentThreadId() && view->nonce == nonce &&
            view->window == reinterpret_cast<uint64_t>(call.hwnd) &&
            InterlockedCompareExchange(&view->active, 0, 0) == 1;
        auto heartbeat = InterlockedCompareExchange64(&view->hostHeartbeatTick, 0, 0);
        auto nowTick = GetTickCount64();
        valid = valid && nowTick >= uint64_t(heartbeat) && nowTick - uint64_t(heartbeat) <= 1000;
        wchar_t className[64]{};
        valid = valid && GetClassNameW(call.hwnd, className, 64) &&
            wcscmp(className, L"wetype.flutter.setting") == 0 && !GetWindow(call.hwnd, GW_OWNER);
        if (!valid)
        {
            UnmapViewOfFile(view); CloseHandle(opened);
            return;
        }
        if (!SetWindowSubclass(call.hwnd, CandidateSubclass, SubclassId, 0))
        {
            InterlockedExchange(&view->attachError, GetLastError() ? GetLastError() : ERROR_INVALID_FUNCTION);
            UnmapViewOfFile(view); CloseHandle(opened);
            return;
        }
        HMODULE pinModule{};
        if (!pinned && !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&CandidateSubclass), &pinModule))
        {
            auto error = GetLastError();
            RemoveWindowSubclass(call.hwnd, CandidateSubclass, SubclassId);
            InterlockedExchange(&view->attachError, error ? error : ERROR_INVALID_FUNCTION);
            UnmapViewOfFile(view); CloseHandle(opened);
            return;
        }
        pinned = true;
        if (shared) UnmapViewOfFile(shared);
        if (mapping) CloseHandle(mapping);
        shared = view; mapping = opened; target = call.hwnd;
        detachMessage = RegisterWindowMessageW(DetachMessage);
        InterlockedExchange(&shared->attached, 1);
    }
}

extern "C" __declspec(dllexport) LRESULT CALLBACK WeTypeProbeHook(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && lParam) TryAttach(*reinterpret_cast<CWPSTRUCT const*>(lParam));
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(instance);
    return TRUE;
}
