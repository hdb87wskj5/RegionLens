#pragma once

#include <windows.h>
#include <cstdint>

namespace RegionLens::weTypeProbe
{
    inline constexpr DWORD ProtocolMagic = 0x42505457; // WTPB
    inline constexpr DWORD ProtocolVersion = 4;
    inline constexpr size_t EventCapacity = 256;
    inline constexpr LONG InterceptEventLimit = 96;
    inline constexpr size_t LensCapacity = 16;
    inline constexpr wchar_t BootstrapMessage[] = L"RegionLens.Stable.WeTypeCompat.Bootstrap.V4";
    inline constexpr wchar_t DetachMessage[] = L"RegionLens.Stable.WeTypeCompat.Detach.V4";
    inline constexpr wchar_t MappingPrefix[] = L"Local\\RegionLens.Stable.WeTypeCompat.";
    inline constexpr wchar_t DllName[] = L"RegionLens-WeTypeCompat.dll";

    enum class ProbeMode : DWORD { Observe = 0, Intercept = 1 };
    inline constexpr bool ProbeTimedOut(ProbeMode mode, uint64_t ageMs) noexcept
    { return mode == ProbeMode::Observe && ageMs >= 30000; }

    struct ProbeEvent
    {
        volatile LONG committed{};
        LONG sequence{};
        DWORD message{};
        DWORD flags{};
        DWORD originalFlags{};
        DWORD rewritten{};
        uint64_t window{};
        uint64_t insertAfter{};
        uint64_t originalInsertAfter{};
        uint64_t anchor{};
        int32_t x{}, y{}, width{}, height{};
        int64_t qpc{};
        int64_t durationQpc{};
        uint64_t tick{};
        uint64_t previous{};
        DWORD topmost{};
        DWORD overlapLensCount{};
        DWORD aboveLensCount{};
    };

    struct ProbeLens
    {
        uint64_t window{};
        RECT bounds{};
    };

    struct ProbeShared
    {
        DWORD magic{ ProtocolMagic };
        DWORD version{ ProtocolVersion };
        DWORD hostProcess{};
        DWORD targetProcess{};
        DWORD targetThread{};
        ProbeMode mode{ ProbeMode::Observe };
        uint64_t nonce{};
        uint64_t window{};
        uint64_t targetCreationTime{};
        volatile LONG64 hostHeartbeatTick{};
        volatile LONG active{ 1 };
        volatile LONG attached{};
        volatile LONG detached{};
        volatile LONG writeSequence{};
        volatile LONG attachError{};
        volatile LONG interceptCount{};
        volatile LONG preChangeCount{};
        volatile LONG postChangeCount{};
        volatile LONG failureCode{};
        volatile LONG targetGeneration{};
        LONG targetCount{};
        ProbeLens targets[LensCapacity]{};
        ProbeEvent events[EventCapacity]{};
    };
}
