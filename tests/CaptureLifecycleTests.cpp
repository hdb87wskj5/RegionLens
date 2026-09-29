#include "CaptureDemandPolicy.h"
#include <iostream>

using namespace RegionLens::native;

int RunCaptureLifecycleTests()
{
    int failures{};
    auto check = [&](bool value, char const* name)
    {
        if (!value) { ++failures; std::cerr << "FAILED capture lifecycle: " << name << '\n'; }
    };
    auto first = reinterpret_cast<HMONITOR>(uintptr_t(1));
    auto second = reinterpret_cast<HMONITOR>(uintptr_t(2));
    check(!CaptureIsNeeded(nullptr, nullptr, 0), "null monitor never starts capture");
    check(!CaptureIsNeeded(first, nullptr, 0), "last closed lens releases capture");
    check(CaptureIsNeeded(first, first, 0), "pending selection retains its monitor capture");
    check(!CaptureIsNeeded(first, second, 0), "selection on another monitor does not retain capture");
    check(CaptureIsNeeded(first, nullptr, 1), "one live lens retains capture");
    check(CaptureIsNeeded(first, second, 2), "live lenses retain capture regardless of another selection");
    return failures;
}
