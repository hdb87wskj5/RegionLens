#include "RegionTransform.h"
#include "ProductBuild.h"
#include "LensChrome.h"
#include "MappingStandbyPolicy.h"
#include "HotkeySettings.h"
#include "AppSettings.h"
#include <iostream>
#include <string_view>

// Exercise the same native control implementation as the shipping UI,
// without opting the test executable into UIAccess or elevating it.
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace RegionLens::native;

int RunMouseProxyTests();
int RunCursorRecoveryTests();
int RunShaderTests();
int RunBootstrapTests();
int RunBootstrapTestChild(int argc, wchar_t** argv);
int RunTopmostTests();
int RunHotkeyTests();
int RunSettingsTests();
int RunSettingsCommandTests();
int RunSettingsPreview();
int RunSettingsPresentationTests();
int RunHiddenLensTests();
int RunMappingLifecycleTests();
int RunLiveResizeTests();
int RunCompositionPresentationTests();
int RunPresentationSchedulerTests();
int RunQualityTests();
int RunPointerSpeedTests();
int RunQualityBenchmark();
int RunScreenshotTests();
int RunCaptureLifecycleTests();
int RunUiCursorOwnershipTests();
int RunSoftwareCursorWindowTests();
int RunWeTypeProbeTests();
int RunWeTypeProbeTestChild(int argc, wchar_t** argv);

namespace
{
    int failures{};
    void Check(bool condition, std::string_view name)
    {
        if (!condition)
        {
            std::cerr << "FAILED production: " << name << '\n';
            ++failures;
        }
    }
}

int wmain(int argc, wchar_t** argv)
{
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);
    AppRuntimeConfig config;
    config.version = RL_VERSION; config.commit = RL_COMMIT; SetRuntime(config);
    if (argc > 1)
    {
        if (std::wstring_view(argv[1]) == L"--settings-preview") return RunSettingsPreview();
        if (std::wstring_view(argv[1]) == L"--settings-test") return RunSettingsTests()+RunSettingsCommandTests()+RunSettingsPresentationTests();
        if (std::wstring_view(argv[1]) == L"--settings-command-test") return RunSettingsCommandTests();
        if (std::wstring_view(argv[1]) == L"--composition-test") return RunCompositionPresentationTests();
        if (std::wstring_view(argv[1]) == L"--scheduling-test") return RunPresentationSchedulerTests();
        if (std::wstring_view(argv[1]) == L"--cursor-test") return RunUiCursorOwnershipTests() + RunCursorRecoveryTests() + RunMouseProxyTests() + RunSoftwareCursorWindowTests();
        if (std::wstring_view(argv[1]) == L"--quality-benchmark") return RunQualityBenchmark();
        if (std::wstring_view(argv[1]) == L"--wetype-probe-child") return RunWeTypeProbeTestChild(argc, argv);
        if (std::wstring_view(argv[1]) == L"--input-watchdog")
            return RunBootstrapTestChild(argc, argv);
        return ERROR_INVALID_PARAMETER;
    }

    Check(NormalizeRect({ 200, 160 }, { 40, 20 }) == PixelRect{ 40, 20, 160, 140 },
        "selection coordinates normalize in reverse");
    Check(IsPointInResizeBorder({ 100, 300 }, { 100, 200, 500, 600 }, 10),
        "resize border remains excluded from mapping activation");

    MappingStandbyPolicy standby;
    Check(standby.Arm(1) && standby.Arm(2) && standby.Count() == 2,
        "multiple lenses can remain armed");
    Check(standby.Evaluate(1, ProxyPhase::Off, false) == MappingRouteDecision::Change && standby.CommitRoute(1),
        "one armed lens owns the sole route");
    Check(standby.Evaluate(2, ProxyPhase::Active, false) == MappingRouteDecision::Reject,
        "active input cannot be interrupted");
    Check(standby.Evaluate(2, ProxyPhase::Armed, false) == MappingRouteDecision::Change && standby.CommitRoute(2),
        "an armed idle route can switch safely");
    standby.Suspend();
    Check(standby.Count() == 2 && standby.Routed() == 0 && standby.Resume() == 2,
        "selection suspends and resumes without clearing standby");

    SetAppLanguage(AppLanguage::English);
    auto about = BuildAboutText();
    Check(about.find(std::wstring(L"RegionLens ") + RL_VERSION) != std::wstring::npos &&
        about.find(L"Ctrl+Alt+M") == std::wstring::npos,
        "About exposes the version without a shortcut inventory");

    failures += RunMouseProxyTests();
    failures += RunCursorRecoveryTests();
    failures += RunTopmostTests();
    failures += RunShaderTests();
    failures += RunBootstrapTests();
    failures += RunHotkeyTests();
    failures += RunSettingsTests();
    failures += RunSettingsCommandTests();
    failures += RunSettingsPresentationTests();
    failures += RunHiddenLensTests();
    failures += RunMappingLifecycleTests();
    failures += RunLiveResizeTests();
    failures += RunCompositionPresentationTests();
    failures += RunPresentationSchedulerTests();
    failures += RunQualityTests();
    failures += RunPointerSpeedTests();
    failures += RunScreenshotTests();
    failures += RunCaptureLifecycleTests();
    failures += RunUiCursorOwnershipTests();
    failures += RunSoftwareCursorWindowTests();
    failures += RunWeTypeProbeTests();

    if (!failures) std::cout << "RegionLens production core tests passed.\n";
    return failures ? 1 : 0;
}
