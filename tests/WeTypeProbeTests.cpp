#include "ProductBuild.h"
#include "../src/WeTypeProbe/ProbeProtocol.h"
#include <windows.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    struct FakeControl
    {
        volatile LONG ready{};
        volatile LONG stop{};
        volatile LONG bootstrapReceived{};
        DWORD process{};
        DWORD thread{};
        uint64_t window{};
    };

    FakeControl* childControl{};

    LRESULT CALLBACK FakeCandidate(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (childControl && message == RegisterWindowMessageW(
            RegionLens::weTypeProbe::BootstrapMessage))
            InterlockedIncrement(&childControl->bootstrapReceived);
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void Check(bool condition, char const* description, int& failures)
    {
        if (!condition) { std::cerr << "FAILED WeType probe: " << description << '\n'; ++failures; }
    }

    void CheckCrossProcess(HMODULE dll, wchar_t const* executable, int& failures)
    {
        using namespace RegionLens::weTypeProbe;
        auto name = L"Local\\RegionLens.FakeCandidate." + std::to_wstring(GetCurrentProcessId()) +
            L"." + std::to_wstring(GetTickCount64());
        auto controlMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(FakeControl), name.c_str());
        auto control = controlMapping ? static_cast<FakeControl*>(MapViewOfFile(controlMapping,
            FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(FakeControl))) : nullptr;
        Check(control != nullptr, "cross-process control mapping", failures);
        if (!control) { if (controlMapping) CloseHandle(controlMapping); return; }
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --wetype-probe-child \"" + name + L"\"";
        STARTUPINFOW startup{ sizeof(startup) };
        PROCESS_INFORMATION child{};
        bool launched = CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE;
        Check(launched, "launch independent fake candidate process", failures);
        if (launched)
        {
            auto deadline = GetTickCount64() + 5000;
            while (InterlockedCompareExchange(&control->ready, 0, 0) != 1 && GetTickCount64() < deadline &&
                WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT) Sleep(5);
            Check(InterlockedCompareExchange(&control->ready, 0, 0) == 1 &&
                control->process == child.dwProcessId, "fake process owns candidate window", failures);
            if (control->ready == 1)
            {
                constexpr uint64_t nonce = 0x22CCDDEEFF331144ull;
                auto probeName = std::wstring(MappingPrefix) +
                    std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(child.dwProcessId) + L"." + std::to_wstring(nonce);
                auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                    0, sizeof(ProbeShared), probeName.c_str());
                auto shared = mapping ? static_cast<ProbeShared*>(MapViewOfFile(mapping,
                    FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ProbeShared))) : nullptr;
                Check(shared != nullptr, "cross-process event mapping", failures);
                auto window = reinterpret_cast<HWND>(control->window);
                if (shared)
                {
                    shared->magic = ProtocolMagic; shared->version = ProtocolVersion;
                    shared->hostProcess = GetCurrentProcessId();
                    shared->targetProcess = child.dwProcessId;
                    shared->targetThread = control->thread;
                    shared->window = control->window;
                    shared->nonce = nonce;
                    InterlockedExchange64(&shared->hostHeartbeatTick, GetTickCount64());
                    InterlockedExchange(&shared->active, 1);
                }
                auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "WeTypeProbeHook"));
                auto hook = shared ? SetWindowsHookExW(WH_CALLWNDPROC, callback, dll, control->thread) : nullptr;
                Check(hook != nullptr, "inject only into fake candidate thread", failures);
                if (hook)
                {
                    auto bootstrap = RegisterWindowMessageW(BootstrapMessage);
                    Check(PostMessageW(window, bootstrap, GetCurrentProcessId(),
                        static_cast<LPARAM>(nonce)) != FALSE, "post fake bootstrap", failures);
                    deadline = GetTickCount64() + 1000;
                    while (InterlockedCompareExchange(&control->bootstrapReceived, 0, 0) == 0 &&
                        GetTickCount64() < deadline) Sleep(5);
                    Check(control->bootstrapReceived == 1 && shared->attached == 0,
                        "posted bootstrap reaches candidate but not WH_CALLWNDPROC", failures);
                    Check(SendNotifyMessageW(window, bootstrap, GetCurrentProcessId(),
                        static_cast<LPARAM>(nonce)) != FALSE,
                        "send nonblocking bootstrap to fake candidate", failures);
                    deadline = GetTickCount64() + 2000;
                    while (InterlockedCompareExchange(&shared->attached, 0, 0) != 1 &&
                        GetTickCount64() < deadline) Sleep(5);
                    Check(shared->attached == 1,
                        "sent bootstrap attaches subclass in independent process", failures);
                    if (shared->attached == 1)
                    {
                        SetWindowPos(window, HWND_TOPMOST, 60, 70, 160, 90, SWP_NOACTIVATE);
                        deadline = GetTickCount64() + 2000;
                        while (InterlockedCompareExchange(&shared->writeSequence, 0, 0) < 2 &&
                            GetTickCount64() < deadline) Sleep(5);
                        auto latest = InterlockedCompareExchange(&shared->writeSequence, 0, 0);
                        bool pre{}, post{};
                        for (LONG index = 0; index < latest && index < EventCapacity; ++index)
                        {
                            auto& event = shared->events[index];
                            if (InterlockedCompareExchange(&event.committed, 0, 0) != index + 1) continue;
                            pre |= event.message == WM_WINDOWPOSCHANGING;
                            post |= event.message == WM_WINDOWPOSCHANGED;
                        }
                        Check(pre && post, "independent process reports pre/post without changing WINDOWPOS", failures);

                        auto lens = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            L"STATIC", L"", WS_POPUP | WS_VISIBLE, -30000, -30000, 200, 120,
                            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
                        Check(lens != nullptr, "create independent offscreen topmost lens", failures);
                        if (lens)
                        {
                            RECT bounds{};
                            GetWindowRect(lens, &bounds);
                            InterlockedIncrement(&shared->targetGeneration);
                            shared->targetCount = 1;
                            shared->targets[0] = { reinterpret_cast<uint64_t>(lens), bounds };
                            MemoryBarrier();
                            InterlockedIncrement(&shared->targetGeneration);
                            shared->mode = ProbeMode::Intercept;
                            SetWindowPos(window, HWND_TOPMOST, -30000, -30000, 100, 80,
                                SWP_SHOWWINDOW | SWP_NOACTIVATE);
                            Check(shared->interceptCount > 0 && shared->failureCode == 0 &&
                                GetWindow(window, GW_HWNDPREV) == lens,
                                "independent candidate's first show stays behind topmost lens", failures);
                            auto firstCount = shared->interceptCount;
                            SetWindowPos(window, HWND_TOPMOST, -28000, -28000, 100, 80,
                                SWP_NOACTIVATE);
                            Check(shared->interceptCount == firstCount && shared->active == 1,
                                "independent candidate outside the lens remains unaffected", failures);
                            SetWindowPos(window, HWND_TOP, -30000, -30000, 100, 80,
                                SWP_NOZORDER | SWP_NOACTIVATE);
                            Check(shared->interceptCount > firstCount && shared->failureCode == 0 &&
                                GetWindow(window, GW_HWNDPREV) == lens,
                                "independent candidate entering overlap is intercepted", failures);
                            DestroyWindow(lens);
                        }
                    }
                    InterlockedExchange(&shared->active, 0);
                    DWORD_PTR ignored{};
                    SendMessageTimeoutW(window, RegisterWindowMessageW(DetachMessage), 0, 0,
                        SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
                    Check(shared->detached == 1, "independent process detaches target subclass", failures);
                    UnhookWindowsHookEx(hook);
                }
                if (shared) UnmapViewOfFile(shared);
                if (mapping) CloseHandle(mapping);
            }
            InterlockedExchange(&control->stop, 1);
            Check(WaitForSingleObject(child.hProcess, 3000) == WAIT_OBJECT_0,
                "fake process exits normally", failures);
            CloseHandle(child.hThread);
            CloseHandle(child.hProcess);
        }
        UnmapViewOfFile(control);
        CloseHandle(controlMapping);
    }
}

int RunWeTypeProbeTestChild(int argc, wchar_t** argv)
{
    if (argc < 3) return ERROR_INVALID_PARAMETER;
    auto mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, argv[2]);
    auto control = mapping ? static_cast<FakeControl*>(MapViewOfFile(mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(FakeControl))) : nullptr;
    if (!control) { if (mapping) CloseHandle(mapping); return ERROR_INVALID_HANDLE; }
    WNDCLASSW type{};
    type.lpfnWndProc = FakeCandidate;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"wetype.flutter.setting";
    auto atom = RegisterClassW(&type);
    auto window = atom ? CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        type.lpszClassName, L"fake cross-process candidate", WS_POPUP,
        30, 40, 100, 50, nullptr, nullptr, type.hInstance, nullptr) : nullptr;
    if (window)
    {
        childControl = control;
        control->process = GetCurrentProcessId();
        control->thread = GetCurrentThreadId();
        control->window = reinterpret_cast<uint64_t>(window);
        InterlockedExchange(&control->ready, 1);
        auto deadline = GetTickCount64() + 12000;
        MSG message{};
        while (InterlockedCompareExchange(&control->stop, 0, 0) == 0 && GetTickCount64() < deadline)
        {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&message); DispatchMessageW(&message); }
            Sleep(1);
        }
        DestroyWindow(window);
        childControl = nullptr;
    }
    if (atom) UnregisterClassW(type.lpszClassName, type.hInstance);
    UnmapViewOfFile(control);
    CloseHandle(mapping);
    return window ? 0 : ERROR_INVALID_WINDOW_HANDLE;
}

int RunWeTypeProbeTests()
{
    using namespace RegionLens::weTypeProbe;
    int failures{};
    Check(std::wstring_view(DllName) == L"RegionLens-WeTypeCompat.dll" &&
        std::wstring_view(MappingPrefix) == L"Local\\RegionLens.Stable.WeTypeCompat.",
        "stable compatibility resources use the production namespace", failures);
    Check(ProbeTimedOut(ProbeMode::Observe, 30000) &&
        !ProbeTimedOut(ProbeMode::Observe, 29999) &&
        !ProbeTimedOut(ProbeMode::Intercept, 24ull * 60 * 60 * 1000),
        "only observation has a 30-second lifetime", failures);
    wchar_t executable[32768]{};
    auto length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    Check(length && length < std::size(executable), "test executable path", failures);
    if (failures) return failures;
    std::wstring dllPath(executable, length);
    dllPath.resize(dllPath.find_last_of(L"\\/") + 1);
    dllPath += DllName;
    auto dll = LoadLibraryW(dllPath.c_str());
    Check(dll != nullptr, "load built x64 observation DLL", failures);
    if (!dll) return failures;
    auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "WeTypeProbeHook"));
    Check(callback != nullptr, "export targeted hook", failures);
    if (!callback) { FreeLibrary(dll); return failures; }

    WNDCLASSW type{};
    type.lpfnWndProc = FakeCandidate;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"wetype.flutter.setting";
    auto atom = RegisterClassW(&type);
    Check(atom != 0, "register fake candidate class", failures);
    auto window = atom ? CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        type.lpszClassName, L"fake candidate", WS_POPUP, 25, 35, 120, 60,
        nullptr, nullptr, type.hInstance, nullptr) : nullptr;
    Check(window != nullptr, "create fake candidate window", failures);
    if (!window) { FreeLibrary(dll); return failures; }

    constexpr uint64_t nonce = 0x16AABBCCDD223344ull;
    std::wstring name = std::wstring(MappingPrefix) +
        std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetCurrentProcessId()) +
        L"." + std::to_wstring(nonce);
    auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, sizeof(ProbeShared), name.c_str());
    auto shared = mapping ? static_cast<ProbeShared*>(MapViewOfFile(mapping,
        FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(ProbeShared))) : nullptr;
    Check(shared != nullptr, "create fixed event mapping", failures);
    if (shared)
    {
        shared->magic = ProtocolMagic;
        shared->version = ProtocolVersion;
        shared->hostProcess = GetCurrentProcessId();
        shared->targetProcess = GetCurrentProcessId();
        shared->targetThread = GetCurrentThreadId();
        shared->nonce = nonce;
        shared->window = reinterpret_cast<uint64_t>(window);
        InterlockedExchange64(&shared->hostHeartbeatTick, GetTickCount64());
        InterlockedExchange(&shared->active, 1);
    }
    auto hook = shared ? SetWindowsHookExW(WH_CALLWNDPROC, callback, dll,
        GetCurrentThreadId()) : nullptr;
    Check(hook != nullptr, "install thread-targeted hook", failures);
    if (hook)
    {
        auto bootstrap = RegisterWindowMessageW(BootstrapMessage);
        SendMessageW(window, bootstrap, GetCurrentProcessId(), static_cast<LPARAM>(nonce));
        Check(InterlockedCompareExchange(&shared->attached, 0, 0) == 1,
            "target-thread subclass attached", failures);
        SetWindowPos(window, HWND_TOPMOST, 40, 50, 125, 65, SWP_NOACTIVATE);
        SetWindowPos(window, HWND_NOTOPMOST, 45, 55, 130, 70, SWP_NOACTIVATE);
        bool sawPre{}, sawPost{}, sawGeometry{};
        auto latest = InterlockedCompareExchange(&shared->writeSequence, 0, 0);
        Check(latest >= 4 && latest < EventCapacity, "bounded pre/post messages", failures);
        for (LONG index = 0; index < latest && index < EventCapacity; ++index)
        {
            auto& event = shared->events[index];
            Check(InterlockedCompareExchange(&event.committed, 0, 0) == index + 1,
                "complete ordered record", failures);
            sawPre |= event.message == WM_WINDOWPOSCHANGING;
            sawPost |= event.message == WM_WINDOWPOSCHANGED;
            sawGeometry |= event.x == 40 && event.y == 50 && event.width == 125 && event.height == 65;
            Check(event.window == reinterpret_cast<uint64_t>(window) && event.tick && event.qpc,
                "identified window and timestamp", failures);
        }
        Check(sawPre && sawPost && sawGeometry, "read-only prechange and final records", failures);
        SetWindowPos(window, HWND_TOPMOST, 47, 57, 135, 75,
            SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        auto skippedLatest = InterlockedCompareExchange(&shared->writeSequence, 0, 0);
        bool skippedPre{}, skippedPost{};
        for (LONG index = latest; index < skippedLatest && index < EventCapacity; ++index)
        {
            auto& event = shared->events[index];
            skippedPre |= event.message == WM_WINDOWPOSCHANGING;
            skippedPost |= event.message == WM_WINDOWPOSCHANGED;
        }
        Check(!skippedPre && skippedPost,
            "SWP_NOSENDCHANGING bypasses prechange observation", failures);
        InterlockedExchange64(&shared->hostHeartbeatTick, GetTickCount64() - 2000);
        SetWindowPos(window, HWND_TOPMOST, 50, 60, 140, 80, SWP_NOACTIVATE);
        Check(shared->active == 0 && shared->writeSequence == skippedLatest,
            "expired host heartbeat disables observation without changing the window", failures);
        InterlockedExchange(&shared->active, 0);
        SendMessageW(window, RegisterWindowMessageW(DetachMessage), 0, 0);
        Check(InterlockedCompareExchange(&shared->detached, 0, 0) == 1,
            "subclass detached on target thread", failures);

        WNDCLASSW lensType{};
        lensType.lpfnWndProc = FakeCandidate;
        lensType.hInstance = type.hInstance;
        lensType.lpszClassName = L"RegionLens.OffscreenTestLens";
        auto lensAtom = RegisterClassW(&lensType);
        auto lens = lensAtom ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            lensType.lpszClassName, L"", WS_POPUP, -30000, -30000, 200, 120,
            nullptr, nullptr, type.hInstance, nullptr) : nullptr;
        auto candidate = lens ? CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            type.lpszClassName, L"", WS_POPUP, -30000, -30000, 100, 80,
            nullptr, nullptr, type.hInstance, nullptr) : nullptr;
        Check(candidate != nullptr, "create offscreen overlapping fake windows", failures);
        if (candidate)
        {
            SetWindowPos(lens, HWND_TOPMOST, -30000, -30000, 200, 120,
                SWP_SHOWWINDOW | SWP_NOACTIVATE);
            RECT bounds{}; GetWindowRect(lens, &bounds);
            shared->window = reinterpret_cast<uint64_t>(candidate);
            shared->targetCount = 1;
            shared->targets[0] = { reinterpret_cast<uint64_t>(lens), bounds };
            InterlockedExchange(&shared->attached, 0);
            InterlockedExchange(&shared->detached, 0);
            InterlockedExchange(&shared->writeSequence, 0);
            InterlockedExchange64(&shared->hostHeartbeatTick, GetTickCount64());
            InterlockedExchange(&shared->active, 1);
            SendMessageW(candidate, bootstrap, GetCurrentProcessId(), static_cast<LPARAM>(nonce));
            Check(shared->attached == 1, "reattach exact second candidate", failures);
            SetWindowPos(candidate, HWND_TOPMOST, -30000, -30000, 100, 80,
                SWP_SHOWWINDOW | SWP_NOACTIVATE);
            bool sawRise{};
            auto count = InterlockedCompareExchange(&shared->writeSequence, 0, 0);
            for (LONG index = 0; index + 1 < count && index + 1 < EventCapacity; ++index)
            {
                auto& before = shared->events[index];
                auto& after = shared->events[index + 1];
                sawRise |= before.message == WM_WINDOWPOSCHANGING && before.aboveLensCount == 0 &&
                    after.message == WM_WINDOWPOSCHANGED && after.overlapLensCount == 1 &&
                    after.aboveLensCount == 1;
            }
            Check(sawRise, "read-only snapshot detects an actual overlapping z-order rise", failures);

            // The next experiment changes only the exact candidate's proposed
            // WINDOWPOS. A fake topmost lens must stay in front even when the
            // candidate asks to rise or moves into its bounds with NOZORDER.
            SetWindowPos(candidate, lens, -30000, -30000, 100, 80,
                SWP_NOACTIVATE);
            shared->mode = ProbeMode::Intercept;
            InterlockedExchange(&shared->failureCode, 0);
            InterlockedExchange(&shared->interceptCount, 0);
            SetWindowPos(candidate, HWND_TOPMOST, -30000, -30000, 100, 80,
                SWP_NOACTIVATE);
            Check(shared->interceptCount > 0 && shared->failureCode == 0 &&
                GetWindow(candidate, GW_HWNDPREV) == lens,
                "pre-change rewrite keeps overlapping candidate below topmost lens", failures);

            auto beforeOutside = shared->interceptCount;
            SetWindowPos(candidate, HWND_TOPMOST, -28000, -28000, 100, 80,
                SWP_NOACTIVATE);
            Check(shared->interceptCount == beforeOutside && shared->active == 1,
                "candidate outside lens remains normally topmost", failures);
            SetWindowPos(candidate, candidate, -30000, -30000, 100, 80,
                SWP_NOZORDER | SWP_NOACTIVATE);
            Check(shared->interceptCount > beforeOutside && shared->failureCode == 0 &&
                GetWindow(candidate, GW_HWNDPREV) == lens,
                "moving into overlap rewrites NOZORDER before display", failures);

            auto secondLens = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                lensType.lpszClassName, L"", WS_POPUP, -30000, -30000, 200, 120,
                nullptr, nullptr, type.hInstance, nullptr);
            Check(secondLens != nullptr, "create second offscreen lens", failures);
            if (secondLens)
            {
                SetWindowPos(secondLens, HWND_TOPMOST, -30000, -30000, 200, 120,
                    SWP_SHOWWINDOW | SWP_NOACTIVATE);
                RECT secondBounds{}; GetWindowRect(secondLens, &secondBounds);
                InterlockedIncrement(&shared->targetGeneration);
                shared->targetCount = 2;
                shared->targets[1] = { reinterpret_cast<uint64_t>(secondLens), secondBounds };
                MemoryBarrier();
                InterlockedIncrement(&shared->targetGeneration);
                SetWindowPos(candidate, HWND_TOPMOST, -30000, -30000, 100, 80,
                    SWP_NOACTIVATE);
                auto const& latestEvent = shared->events[(shared->writeSequence - 1) % EventCapacity];
                Check(shared->failureCode == 0 && latestEvent.overlapLensCount == 2 &&
                    latestEvent.aboveLensCount == 0 && GetWindow(candidate, GW_HWNDPREV) == lens,
                    "candidate is placed behind both overlapping lenses", failures);
                InterlockedIncrement(&shared->targetGeneration);
                shared->targetCount = 1;
                MemoryBarrier();
                InterlockedIncrement(&shared->targetGeneration);
                DestroyWindow(secondLens);
            }

            auto beforeLongRun = shared->interceptCount;
            for (int index = 0; index < 200; ++index)
                SetWindowPos(candidate, HWND_TOPMOST, -30000 + (index & 1), -30000,
                    100, 80, SWP_NOACTIVATE);
            Check(shared->interceptCount > beforeLongRun && shared->active == 1 &&
                shared->failureCode == 0 &&
                shared->preChangeCount > InterceptEventLimit &&
                shared->postChangeCount > InterceptEventLimit &&
                shared->writeSequence <= InterceptEventLimit,
                "long-running interception aggregates events without filling the ring", failures);

            // A target that bypasses WINDOWPOSCHANGING must fail the trial;
            // the host then resumes ordinary post-change topmost maintenance.
            SetWindowPos(candidate, HWND_TOPMOST, -30000, -30000, 100, 80,
                SWP_NOACTIVATE | SWP_NOSENDCHANGING);
            Check(shared->failureCode == 1 && shared->active == 0,
                "bypassed pre-change message fails interception closed", failures);

            constexpr uint64_t replacementNonce = nonce + 1;
            auto replacementName = std::wstring(MappingPrefix) +
                std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(GetCurrentProcessId()) + L"." +
                std::to_wstring(replacementNonce);
            auto replacementMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE, 0, sizeof(ProbeShared), replacementName.c_str());
            auto replacement = replacementMapping ? static_cast<ProbeShared*>(MapViewOfFile(
                replacementMapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                sizeof(ProbeShared))) : nullptr;
            Check(replacement != nullptr, "create replacement lease for a stale subclass", failures);
            if (replacement)
            {
                replacement->magic = ProtocolMagic;
                replacement->version = ProtocolVersion;
                replacement->hostProcess = GetCurrentProcessId();
                replacement->targetProcess = GetCurrentProcessId();
                replacement->targetThread = GetCurrentThreadId();
                replacement->mode = ProbeMode::Intercept;
                replacement->nonce = replacementNonce;
                replacement->window = reinterpret_cast<uint64_t>(candidate);
                replacement->targetCount = 1;
                replacement->targets[0] = { reinterpret_cast<uint64_t>(lens), bounds };
                InterlockedExchange64(&replacement->hostHeartbeatTick, GetTickCount64());
                InterlockedExchange(&replacement->active, 1);
                SendMessageW(candidate, bootstrap, GetCurrentProcessId(),
                    static_cast<LPARAM>(replacementNonce));
                Check(shared->detached == 1 && replacement->attached == 1,
                    "inactive target-thread subclass is replaced by a fresh lease", failures);
                SetWindowPos(candidate, HWND_TOPMOST, -30000, -30000, 100, 80,
                    SWP_NOACTIVATE);
                Check(replacement->interceptCount > 0 && replacement->failureCode == 0,
                    "replacement lease resumes interception", failures);
            }
            DestroyWindow(candidate);
            Check((replacement ? replacement->detached : shared->detached) == 1,
                "destroyed candidate automatically removes its target-thread subclass", failures);
            if (replacement) UnmapViewOfFile(replacement);
            if (replacementMapping) CloseHandle(replacementMapping);
        }
        if (lens) DestroyWindow(lens);
        if (lensAtom) UnregisterClassW(lensType.lpszClassName, lensType.hInstance);
        UnhookWindowsHookEx(hook);
    }
    DestroyWindow(window);
    UnregisterClassW(type.lpszClassName, type.hInstance);
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    CheckCrossProcess(dll, executable, failures);
    FreeLibrary(dll);
    if (!failures) std::cout << "WeType observation and interception hook tests passed (fake same/cross-process windows; no desktop input).\n";
    return failures;
}
