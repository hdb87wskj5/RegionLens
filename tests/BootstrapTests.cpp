#include "WatchdogBootstrap.h"
#include <objbase.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace RegionLens::native;
namespace
{
    int failures{};
    void Check(bool condition, char const* name)
    {
        if (!condition) { std::cerr << "FAILED: " << name << '\n'; ++failures; }
    }
    enum class ChildMode : LONG { Ready, DelayedReady, FailReady, EarlyExit, NoReady };
    struct TestState
    {
        DWORD magic{ 0x54455354 };
        ChildMode mode{};
        volatile LONG received{}, error{};
    };
    // This receiver never calls input APIs: only the anonymous mapping and
    // process/event handles are exercised, even in failure/timeout scenarios.
    void CheckHandoff(ChildMode mode)
    {
        BootstrapHandles handles{
            CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(TestState), nullptr),
            OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentProcessId()),
            CreateEventW(nullptr, TRUE, FALSE, nullptr), CreateEventW(nullptr, TRUE, FALSE, nullptr),
            CreateMutexW(nullptr, FALSE, nullptr) };
        auto state = static_cast<TestState*>(MapViewOfFile(handles[0], FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestState)));
        Check(state != nullptr, "Bootstrap test mapping created");
        if (!state) { CloseBootstrapHandles(handles); return; }
        new (state) TestState{}; state->mode = mode;
        HANDLE child{}; ProxyStartupFailure failure{};
        bool launched = LaunchWatchdogBootstrap(handles, child, failure, L"--input-watchdog",
            mode == ChildMode::NoReady ? 1500 : 5000);
        if (mode == ChildMode::EarlyExit)
            Check(!launched && failure.stage == ProxyStartupStage::GuardExit && failure.error == ERROR_ACCESS_DENIED,
                "Child exit preserves the real exit error");
        else if (mode == ChildMode::NoReady)
            Check(!launched && failure.stage == ProxyStartupStage::GuardReady && failure.error == ERROR_TIMEOUT,
                "Missing readiness fails closed with a bounded timeout");
        else
        {
            if (!launched) std::wcerr << L"Bootstrap failure: " << StartupStageName(failure.stage) << L" " << failure.error << L'\n';
            Check(launched && failure.stage == ProxyStartupStage::None, "Shell-activated child receives anonymous recovery handles");
            Check(InterlockedCompareExchange(&state->received, 0, 0) == 1, "Child confirms mapping, parent identity and non-inherited handles");
            if (mode == ChildMode::FailReady)
                Check(InterlockedCompareExchange(&state->error, 0, 0) == ERROR_ACCESS_DENIED,
                    "Child readiness failure can be reported through shared state");
        }
        SetEvent(handles[2]);
        if (child)
        {
            Check(WaitForSingleObject(child, 6000) == WAIT_OBJECT_0, "Bootstrap child exits cleanly after quit or startup failure");
            CloseHandle(child);
        }
        UnmapViewOfFile(state); CloseBootstrapHandles(handles);
    }

}
int RunBootstrapTestChild(int argc, wchar_t** argv)
{
    BootstrapHandles handles{}; ProxyStartupFailure failure{};
    if (!ReceiveWatchdogBootstrap(argc, argv, handles, failure)) return int(failure.error);
    auto state = static_cast<TestState*>(MapViewOfFile(handles[0], FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TestState)));
    if (!state || state->magic != 0x54455354)
    { if (state) UnmapViewOfFile(state); CloseBootstrapHandles(handles); return ERROR_INVALID_DATA; }
    auto mode = state->mode;
    InterlockedExchange(&state->received, 1);
    if (mode == ChildMode::DelayedReady) WaitForSingleObject(handles[2], 100);
    if (mode == ChildMode::FailReady) InterlockedExchange(&state->error, ERROR_ACCESS_DENIED);
    if (mode != ChildMode::EarlyExit && mode != ChildMode::NoReady) SetEvent(handles[3]);
    if (mode != ChildMode::EarlyExit)
    {
        HANDLE waits[]{ handles[1], handles[2] };
        WaitForMultipleObjects(2, waits, FALSE, 6000);
    }
    UnmapViewOfFile(state); CloseBootstrapHandles(handles);
    return mode == ChildMode::EarlyExit ? ERROR_ACCESS_DENIED : ERROR_SUCCESS;
}
int RunBootstrapTests()
{
    DWORD parent{};
    constexpr auto nonce = L"0123456789abcdef0123456789abcdef";
    Check(ParseBootstrapIdentity(L"12345", nonce, parent) && parent == 12345, "Strict bootstrap identity parses");
    for (auto pid : { L"", L"0", L"-1", L"+1", L"1 2", L"4294967296", L"99999999999" })
        Check(!ParseBootstrapIdentity(pid, nonce, parent), "Invalid bootstrap PID rejected");
    Check(!ParseBootstrapIdentity(L"1", L"0123", parent) &&
        !ParseBootstrapIdentity(L"1", L"../../../../../../../../abcdefgh", parent), "Invalid pipe nonce rejected");
    BootstrapPacket packet; packet.parentId = 1; packet.childId = 2; packet.handles = { 4, 8, 12, 16, 20 };
    Check(ValidBootstrapPacket(packet, 1, 2), "Versioned bootstrap packet accepted");
    Check(!ValidBootstrapPacket(packet, 1, 3) && !ValidBootstrapPacket(packet, 3, 2), "Unexpected process identities rejected");
    packet.version = 1; Check(!ValidBootstrapPacket(packet, 1, 2), "Unknown bootstrap protocol rejected");
    packet.version = 2; Check(!ValidBootstrapPacket(packet, 1, 2), "Legacy bootstrap without runtime lease rejected"); packet.version = 3;
    packet.handles[3] = 4; Check(!ValidBootstrapPacket(packet, 1, 2), "Duplicate handle slots rejected");
    packet.handles[3] = 0; Check(!ValidBootstrapPacket(packet, 1, 2), "Missing recovery handle rejected");
    auto previousLanguage = CurrentAppLanguage();
    SetAppLanguage(AppLanguage::English);
    auto launchError = StartupFailureMessage({ ProxyStartupStage::ShellLaunch, ERROR_ELEVATION_REQUIRED });
    auto cookieError = StartupFailureMessage({ ProxyStartupStage::InputCookie, ERROR_GEN_FAILURE });
    Check(cookieError.find(L"input-cookie") != std::wstring::npos,
        "Cookie generation failure retains its own stage");
    Check(launchError.find(L"740") != std::wstring::npos,
        "Elevation failure retains its numeric error");
    SetAppLanguage(previousLanguage);
    HANDLE child{}; ProxyStartupFailure failure{};
    Check(!LaunchWatchdogBootstrap({}, child, failure) && !child && failure.stage == ProxyStartupStage::HandleTransfer,
        "Invalid handles fail before launching any process");
    auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    Check(SUCCEEDED(initialized), "STA initialized for Windows shell activation");
    if (SUCCEEDED(initialized))
    {
        for (int repeat = 0; repeat < 3; ++repeat) CheckHandoff(ChildMode::Ready);
        CheckHandoff(ChildMode::DelayedReady);
        CheckHandoff(ChildMode::FailReady);
        CheckHandoff(ChildMode::EarlyExit);
        CheckHandoff(ChildMode::NoReady);
        CoUninitialize();
    }
    if (!failures) std::cout << "Watchdog bootstrap tests passed (no desktop input APIs).\n";
    return failures;
}
