#include "pch.h"
#include "AppRuntime.h"
#include "Localization.h"
namespace RegionLens::native
{
    namespace { AppRuntimeConfig configuration; }
    AppRuntimeConfig const& Runtime() noexcept { return configuration; }
    void SetRuntime(AppRuntimeConfig const& value) noexcept { configuration = value; }
    std::wstring BuildIdentityText()
    {
        auto const& value = Runtime();
        std::wstring text = std::wstring(Localized(L"区域镜 RegionLens", L"RegionLens")) + L" " + value.version +
            (value.identity->channel == AppChannel::Dev ? Localized(L"（开发版）", L" (Dev)") : Localized(L"（正式版）", L" (Stable)")) +
            L"\r\n" + value.commit;
        if (value.diagnostics) text += value.diagnostics->Error()
            ? std::wstring(Localized(L"\r\n诊断记录不可用：", L"\r\nDiagnostics unavailable: ")) + std::to_wstring(value.diagnostics->Error())
            : Localized(L"\r\n后台诊断已开启", L"\r\nBackground diagnostics enabled");
        if (value.diagnosticStartupError) text += std::wstring(Localized(L"\r\n诊断初始化失败：", L"\r\nDiagnostics unavailable: ")) + std::to_wstring(value.diagnosticStartupError);
        return text;
    }
}
