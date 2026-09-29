#include "pch.h"
#include "SoftwareCursorWindow.h"

#include <iostream>

using namespace RegionLens::native;

int RunSoftwareCursorWindowTests()
{
    int failures{};
    auto check = [&](bool value, char const* name) {
        if (!value)
        {
            ++failures;
            std::cerr << "FAILED software cursor: " << name << '\n';
        }
    };

    SoftwareCursorWindow cursor;
    check(cursor.Create(nullptr) && cursor.Ready(), "cursor window is created without an owner");
    HWND window = cursor.Window();
    if (window)
    {
        check(!IsWindowVisible(window), "cursor surface starts hidden");
        auto style = GetWindowLongPtrW(window, GWL_EXSTYLE);
        check((style & (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST)) ==
            (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST),
            "cursor surface is click-through, nonactivating and topmost");
        DWORD affinity{};
        check(GetWindowDisplayAffinity(window, &affinity) && affinity == WDA_EXCLUDEFROMCAPTURE,
            "software cursor is excluded from screen capture");

        // The entire exercise remains far outside the virtual desktop. It
        // changes neither the system cursor nor any real desktop input.
        POINT outside{ -30000, -30000 };
        check(cursor.PrepareAt(outside, LoadCursorW(nullptr, IDC_ARROW)) &&
            !IsWindowVisible(window), "cursor sprite is prepared before native hide");
        check(cursor.ShowAt(outside, LoadCursorW(nullptr, IDC_ARROW)) && IsWindowVisible(window),
            "offscreen cursor shape can be presented");
        check(cursor.EnsureTopmost(), "topmost order can be reaffirmed");
        check(cursor.ShowAt({ -29980, -29990 }, LoadCursorW(nullptr, IDC_ARROW)),
            "same-shape movement does not require repainting");
        check(cursor.ShowAt({ -29960, -29970 }, LoadCursorW(nullptr, IDC_IBEAM)),
            "shape and hotspot can change");
        cursor.Hide();
        check(!IsWindowVisible(window) && cursor.EnsureTopmost(),
            "hidden cursor does not enter the topmost maintenance path");
    }
    cursor.Destroy();
    check(!cursor.Window() && !cursor.Ready(), "cursor window and bitmap are released");
    if (!failures) std::cout << "Software cursor window tests passed (offscreen only).\n";
    return failures;
}
