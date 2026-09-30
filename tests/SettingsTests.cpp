#include "pch.h"
#include "AppSettings.h"
#include "AppController.h"
#include "SettingsWindowTestAccess.h"
#include "LensManager.h"
#include "ScreenshotStorage.h"
#include <commctrl.h>
#include <iostream>
#include <filesystem>

using namespace RegionLens::native;
namespace RegionLens::native
{
    struct SettingsControllerTestAccess
    {
        static bool Prepare(AppController& c,std::shared_ptr<D3DDevice> device,std::shared_ptr<InputMappingCoordinator> input)
        {
            c.m_settings.hotkeys.bindings={};c.m_hotkeys=c.m_settings.hotkeys;
            c.m_device=std::move(device);c.m_inputMapping=std::move(input);
            c.m_lensManager=std::make_unique<LensManager>(c.m_device,c.m_inputMapping,LensManager::CloseRequestCallback{});
            return c.CreateControllerWindow(); // Hidden controller; never register real global shortcuts.
        }
        static LensManager& Manager(AppController& c) {return *c.m_lensManager;}
        static HWND Owner(AppController& c) {return c.m_window;}
        static std::shared_ptr<SettingsWindow> Dialog(AppController& c) {return c.m_settingsWindow;}
        static void Open(AppController& c) {c.ShowSettings(false);}
        static void CloseAllShortcut(AppController& c) {c.HandleMessage(WM_HOTKEY,3,0);}
        static void PendingSelection(AppController& c,HMONITOR monitor) {c.m_pendingSelectionMonitor=monitor;}
        static void NewRegionShortcut(AppController& c) {c.HandleMessage(WM_HOTKEY,1,0);}
    };
    struct LensSettingsTestAccess
    {
        static LensWindow& Lens(LensManager& m,uint64_t id) { return *m.m_lenses.at(id); }
        static LensQualitySettings Quality(LensWindow const& w) { return w.m_qualitySettings; }
        static bool Requested(LensWindow const& w) { return w.m_qualityRequested!=LensSharpness::Off; }
        static bool AspectFit(LensWindow const& w) { return w.m_fullscreenAspectFit; }
        static RECT FullscreenContent(LensWindow& w)
        {
            w.m_fullscreen=true;
            auto content=w.ContentRectInClient();
            w.m_fullscreen=false;
            return content;
        }
        static void FailQuality(LensWindow& w) { w.m_qualitySettings={}; }
        static void Arm(LensManager& m,uint64_t id) { m.m_mappingStandby.Arm(id); m.m_mappingStandby.CommitRoute(id); }
        static size_t Armed(LensManager const& m) { return m.m_mappingStandby.Count(); }
    };
}
namespace
{
    int failures{};
    void Check(bool ok,char const* label) { if(!ok) { ++failures; std::cerr<<"FAILED settings: "<<label<<'\n'; } }
    struct InputState { int starts{}, stops{}; bool failDrain{}; };
    class NotificationCounter final : public IDiagnosticSink
    {
    public:
        int notifications{};
        bool TryRecord(DiagnosticRecord const& record) noexcept override
        { if(record.event==DiagnosticEvent::Notification) ++notifications;return true; }
        std::wstring Directory() const override {return {};}
        DWORD Error() const noexcept override {return 0;}
    };
    class FakeEngine final : public IInputMappingEngine
    {
        InputState& s;
    public:
        explicit FakeEngine(InputState& state):s(state){}
        bool Start() override { ++s.starts; return true; }
        void Stop() override { ++s.stops; }
        void Configure(MappingSessionConfig,bool) override {}
        bool DisableAndDrain() override { return !s.failDrain; }
        void Acknowledge(uint64_t,bool) override {}
        ProxyUiUpdate TakeUiUpdate() override { return {}; }
        bool GuardHealthy() const noexcept override { return true; }
        ProxyStartupFailure LastFailure() const noexcept override { return {}; }
    };
    HotkeyRegistrationResult Availability(HotkeySettings const& keys,int conflict=-1)
    {
        HotkeyRegistrationResult result;
        for(size_t i=0;i<HotkeyActionCount;++i) {
            auto& item=result.items[i]; item.requested=keys.bindings[i].Enabled();
            if(int(i)==conflict && item.requested) { item.succeeded=false; item.error=ERROR_HOTKEY_ALREADY_REGISTERED; }
        }
        return result;
    }
    std::vector<BYTE> EncodePreviousSettings(AppSettings const& settings)
    {
        std::array<DWORD,12> header{0x31534c52,1,DWORD(settings.language),DWORD(settings.quality!=LensSharpness::Off),
            DWORD(settings.newWindowTopmost),5,DWORD(settings.screenshotDirectory.size())};
        header[7]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::NewRegion]);
        header[8]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::CloseAll]);
        header[9]=PackHotkeyBinding({MOD_CONTROL|MOD_ALT,'I'}); // Former cancel-mapping slot is ignored on migration.
        header[10]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::HideAll]);
        header[11]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::ShowAllTopmost]);
        std::vector<BYTE> bytes(sizeof(header)+settings.screenshotDirectory.size()*sizeof(wchar_t));
        memcpy(bytes.data(),header.data(),sizeof(header));
        if(!settings.screenshotDirectory.empty()) memcpy(bytes.data()+sizeof(header),settings.screenshotDirectory.data(),
            settings.screenshotDirectory.size()*sizeof(wchar_t));
        return bytes;
    }
    std::vector<BYTE> EncodePreviousV2OrV3Settings(AppSettings const& settings,DWORD version)
    {
        std::array<DWORD,13> header{DWORD(version==2?0x32534c52u:0x33534c52u),version,DWORD(settings.language),
            version==2?DWORD(settings.quality!=LensSharpness::Off):DWORD(settings.quality),
            DWORD(settings.newWindowTopmost),DWORD(settings.fullscreenAspectFit),5u,DWORD(settings.screenshotDirectory.size())};
        header[8]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::NewRegion]);
        header[9]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::CloseAll]);
        header[10]=PackHotkeyBinding({MOD_CONTROL|MOD_ALT,'I'});
        header[11]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::HideAll]);
        header[12]=PackHotkeyBinding(settings.hotkeys[HotkeyAction::ShowAllTopmost]);
        std::vector<BYTE> bytes(sizeof(header)+settings.screenshotDirectory.size()*sizeof(wchar_t));
        memcpy(bytes.data(),header.data(),sizeof(header));
        if(!settings.screenshotDirectory.empty()) memcpy(bytes.data()+sizeof(header),settings.screenshotDirectory.data(),
            settings.screenshotDirectory.size()*sizeof(wchar_t));
        return bytes;
    }
}

int RunSettingsTests()
{
    struct DpiScope { DPI_AWARENESS_CONTEXT previous=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ~DpiScope() { if(previous) SetThreadDpiAwarenessContext(previous); } } dpiScope;
    auto oldLanguage=CurrentAppLanguage();
    auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    AppSettings defaults;
    Check(defaults.quality==LensSharpness::Medium && defaults.newWindowTopmost && !defaults.fullscreenAspectFit &&
        defaults.hotkeys==DefaultHotkeySettings(),"new settings preserve defaults");
    AppSettings custom=defaults; custom.language=AppLanguage::English; custom.quality=LensSharpness::Off; custom.newWindowTopmost=false;
    custom.fullscreenAspectFit=true;
    custom.screenshotDirectory=L"C:\\图片 测试\\captures"; custom.hotkeys[HotkeyAction::NewRegion]={MOD_ALT|MOD_SHIFT,'Q'};
    custom.hotkeys[HotkeyAction::HideAll]={MOD_CONTROL|MOD_SHIFT,'H'};
    custom.hotkeys[HotkeyAction::ShowAllTopmost]={MOD_ALT|MOD_SHIFT,'S'};
    auto encoded=EncodeAppSettings(custom); AppSettings decoded;
    Check(DecodeAppSettings(encoded,decoded) && decoded==custom,"versioned snapshot round trip preserves all pages");
    for(size_t offset:{0u,4u,8u,12u,16u,20u,24u,28u}) {
        auto bad=encoded; memset(bad.data()+offset,255,4); decoded=defaults;
        Check(!DecodeAppSettings(bad,decoded) && decoded==defaults,"malformed records do not partially replace settings");
    }
    auto shortRecord=encoded; shortRecord.pop_back(); Check(!DecodeAppSettings(shortRecord,decoded),"truncated path rejected");
    auto invalid=custom; invalid.screenshotDirectory=L"relative\\folder"; Check(!ValidAppSettings(invalid),"relative paths rejected");
    invalid=custom; invalid.screenshotDirectory.push_back(L'\0'); Check(!ValidAppSettings(invalid),"embedded null rejected");
    invalid=custom; invalid.hotkeys.bindings[1]=invalid.hotkeys.bindings[0]; Check(!ValidAppSettings(invalid),"duplicate shortcuts rejected");
    SettingsDraft draft(defaults); draft.editing=custom; Check(draft.Dirty(),"draft is separate from applied values");
    draft.Cancel(); Check(!draft.Dirty() && draft.editing==defaults,"cancel discards only unapplied edits");
    draft.editing=custom; draft.Accept(); draft.editing.quality=LensSharpness::Medium; draft.Cancel(); Check(draft.editing==custom,"cancel after commit keeps last saved settings");
    int saves{}, releases{}; AppSettings stored=defaults;HotkeySettings live=defaults.hotkeys;
    auto commit=[&](AppSettings const& prior,AppSettings const& next,int conflict,HRESULT write) {
        return CommitAppSettings(prior,next,[&](auto const& keys){live=keys;return Availability(keys,conflict);},[&]{++releases;live.bindings={};},
            [&](auto const& value){++saves; if(SUCCEEDED(write)) stored=value; return write;});
    };
    auto result=commit(defaults,custom,0,S_OK);
    Check(!result.Applied() && saves==0 && releases==2 && stored==defaults && live==defaults.hotkeys,"new conflicting binding restores the old live shortcuts and draft");
    result=commit(defaults,custom,-1,E_ACCESSDENIED);
    Check(result.issue==SettingsApplyIssue::Storage && stored==defaults && releases==4 && live==defaults.hotkeys,"storage failure restores working shortcuts before returning");
    auto optionsOnly=defaults; optionsOnly.quality=LensSharpness::Off;
    result=commit(defaults,optionsOnly,1,S_OK);
    Check(result.Applied() && stored==optionsOnly && live==optionsOnly.hotkeys,"unchanged existing conflict does not block other options or retire shortcuts");
    result=commit(defaults,custom,1,S_OK);
    Check(result.Applied() && stored==custom && live==custom.hotkeys,"successful save retains the newly registered shortcuts immediately");
    try {
        CommitAppSettings(defaults,custom,[&](auto const& keys){live=keys;return Availability(keys);},
            [&]{live.bindings={};},[](auto const&)->HRESULT{throw std::runtime_error("fake storage exception");});
        Check(false,"storage exception must reach caller");
    } catch(std::runtime_error const&) {
        Check(live==defaults.hotkeys,"exception restores prior live registrations");
    }
    auto owned=defaults.hotkeys.bindings;std::array<bool,HotkeyActionCount> registered{};registered.fill(true);
    int externalProbes{};
    auto availability=ProbeHotkeysPreservingRegistrations(defaults.hotkeys,owned,registered,
        [&](size_t,HotkeyBinding){++externalProbes;return DWORD(ERROR_HOTKEY_ALREADY_REGISTERED);});
    Check(availability.AllSucceeded() && externalProbes==0,"applied owned shortcuts are available without unregistering them");
    auto moved=defaults.hotkeys;std::swap(moved.bindings[0],moved.bindings[1]);
    availability=ProbeHotkeysPreservingRegistrations(moved,owned,registered,
        [&](size_t,HotkeyBinding){++externalProbes;return DWORD(ERROR_HOTKEY_ALREADY_REGISTERED);});
    Check(availability.AllSucceeded() && externalProbes==0,"a draft binding owned under another action is not an external conflict");
    availability=ProbeHotkeysPreservingRegistrations(custom.hotkeys,owned,registered,
        [&](size_t,HotkeyBinding){++externalProbes;return DWORD(ERROR_HOTKEY_ALREADY_REGISTERED);});
    Check(!availability.items[0].succeeded && availability.items[1].succeeded && owned==defaults.hotkeys.bindings &&
        std::all_of(registered.begin(),registered.end(),[](bool v){return v;}),"external conflicts are detected without changing live registrations");
    registered.fill(false);externalProbes=0;
    availability=ProbeHotkeysPreservingRegistrations(defaults.hotkeys,owned,registered,
        [&](size_t,HotkeyBinding){++externalProbes;return DWORD(ERROR_SUCCESS);});
    Check(availability.AllSucceeded() && externalProbes==int(HotkeyActionCount),"recording temporarily released shortcuts are probed normally");

    // Isolated HKCU tree only. Neither channel's real preferences are written.
    auto key=L"Software\\RegionLens-SettingsTests-"+NewScreenshotFileName();
    auto otherKey=key+L"\\Other", hotkeysKey=key+L"\\Hotkeys";
    AppIdentity identity=DevIdentity; identity.registry=key.c_str(); identity.hotkeys=hotkeysKey.c_str();
    AppIdentity other=StableIdentity; other.registry=otherKey.c_str();
    auto otherHotkeys=otherKey+L"\\Hotkeys"; other.hotkeys=otherHotkeys.c_str();
    HKEY registry{};
    Check(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_ALL_ACCESS,nullptr,&registry,nullptr)==ERROR_SUCCESS,"create isolated configuration tree");
    if(registry) {
        DWORD language=DWORD(AppLanguage::English);
        RegSetValueExW(registry,L"Language",0,REG_DWORD,reinterpret_cast<BYTE*>(&language),sizeof(language));
        HKEY hotkeys{}; RegCreateKeyExW(registry,L"Hotkeys",0,nullptr,0,KEY_SET_VALUE,nullptr,&hotkeys,nullptr);
        std::array<DWORD,7> legacy{0x314B4852,2,
            PackHotkeyBinding(custom.hotkeys[HotkeyAction::NewRegion]),
            PackHotkeyBinding(custom.hotkeys[HotkeyAction::CloseAll]),
            PackHotkeyBinding({MOD_CONTROL|MOD_ALT,'I'}),
            PackHotkeyBinding(custom.hotkeys[HotkeyAction::HideAll]),
            PackHotkeyBinding(custom.hotkeys[HotkeyAction::ShowAllTopmost])};
        if(hotkeys) { RegSetValueExW(hotkeys,L"Bindings",0,REG_BINARY,reinterpret_cast<BYTE*>(legacy.data()),sizeof(legacy)); RegCloseKey(hotkeys); }
        Check(SUCCEEDED(SaveScreenshotDirectory(identity,custom.screenshotDirectory)),"write isolated legacy screenshot path");
        auto migrated=LoadAppSettings(identity);
        Check(migrated.language==AppLanguage::English && migrated.hotkeys==custom.hotkeys && migrated.screenshotDirectory==custom.screenshotDirectory &&
            migrated.quality==LensSharpness::Medium && migrated.newWindowTopmost && !migrated.fullscreenAspectFit,
            "legacy preferences migrate without replacing new defaults");
        auto previousRecord=EncodePreviousSettings(custom);
        Check(RegSetValueExW(registry,L"SettingsV1",0,REG_BINARY,previousRecord.data(),DWORD(previousRecord.size()))==ERROR_SUCCESS,
            "write isolated previous settings snapshot");
        auto previousMigrated=custom;previousMigrated.fullscreenAspectFit=false;
        Check(LoadAppSettings(identity)==previousMigrated,"SettingsV1 migrates with aspect-fit disabled");
        auto previousEnabled=previousMigrated;previousEnabled.quality=LensSharpness::High;
        auto enabledRecord=EncodePreviousSettings(previousEnabled);
        RegSetValueExW(registry,L"SettingsV1",0,REG_BINARY,enabledRecord.data(),DWORD(enabledRecord.size()));
        previousEnabled.quality=LensSharpness::Medium;
        Check(LoadAppSettings(identity)==previousEnabled,"V1 enabled sharpening becomes medium rather than silently retaining high");
        // V2 retained the same layout as V3 but stored a boolean in word 3.
        for (DWORD enabled : {0u,1u}) {
            auto v2=EncodePreviousV2OrV3Settings(custom,2);
            memcpy(v2.data()+12,&enabled,4);
            RegSetValueExW(registry,L"SettingsV2",0,REG_BINARY,v2.data(),DWORD(v2.size()));
            auto expected=custom;expected.quality=enabled?LensSharpness::Medium:LensSharpness::Off;
            Check(LoadAppSettings(identity)==expected,"V2 preserves all fields and migrates enabled to medium / disabled to none");
        }
        RegDeleteValueW(registry,L"SettingsV2");
        auto oldV3=EncodePreviousV2OrV3Settings(custom,3);
        Check(RegSetValueExW(registry,L"SettingsV3",0,REG_BINARY,oldV3.data(),DWORD(oldV3.size()))==ERROR_SUCCESS &&
            LoadAppSettings(identity)==custom,"old five-shortcut V3 skips only cancel mapping and preserves other settings");
        RegDeleteValueW(registry,L"SettingsV3");
        DWORD bad=42;RegSetValueExW(registry,L"SettingsV1",0,REG_BINARY,reinterpret_cast<BYTE*>(&bad),sizeof(bad));
        Check(LoadAppSettings(identity)==defaults,"corrupt previous snapshot does not reimport stale legacy values");
        RegSetValueExW(registry,L"SettingsV1",0,REG_BINARY,previousRecord.data(),DWORD(previousRecord.size()));
        Check(SUCCEEDED(SaveAppSettings(identity,custom)) && LoadAppSettings(identity)==custom,"single persisted value restores all settings on restart");
        for(auto level:{LensSharpness::Off,LensSharpness::Low,LensSharpness::Medium,LensSharpness::High}) {
            auto value=custom;value.quality=level;
            Check(SUCCEEDED(SaveAppSettings(identity,value)) && LoadAppSettings(identity)==value,"every V3 quality level survives restart");
        }
        SaveAppSettings(identity,custom);
        language=0; RegSetValueExW(registry,L"Language",0,REG_DWORD,reinterpret_cast<BYTE*>(&language),sizeof(language));
        Check(LoadAppSettings(identity)==custom,"new snapshot takes precedence without dual writes");
        DWORD bytes=sizeof(language); Check(RegGetValueW(registry,nullptr,L"Language",RRF_RT_REG_DWORD,nullptr,&language,&bytes)==ERROR_SUCCESS,"legacy values retained");
        Check(LoadAppSettings(other)==defaults,"channel-specific registry roots stay isolated");
        Check(FAILED(SaveAppSettings(identity,invalid)) && LoadAppSettings(identity)==custom,"invalid save preserves prior snapshot");
        RegSetValueExW(registry,L"SettingsV3",0,REG_BINARY,reinterpret_cast<BYTE*>(&bad),sizeof(bad));
        Check(LoadAppSettings(identity)==defaults,"corrupt current snapshot falls back safely rather than reimporting SettingsV1");
        RegCloseKey(registry); RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str());
    }

    // Native property sheet, fake persistence/probes, and owned control
    // notifications only: no real mouse/keyboard input is synthesized.
    int probes{}, applied{}; bool occupied=true, failSave{}; AppSettings accepted=defaults;
    auto probe=[&](auto const& keys){++probes;return Availability(keys,occupied?1:-1);};
    auto apply=[&](auto const& value){++applied;SettingsApplyResult r;r.availability=Availability(value.hotkeys);
        if(failSave){r.issue=SettingsApplyIssue::Storage;r.error=E_ACCESSDENIED;}
        else {
            auto saved=SaveAppSettings(identity,value);
            if(FAILED(saved)){r.issue=SettingsApplyIssue::Storage;r.error=saved;}
            else {accepted=LoadAppSettings(identity);SetAppLanguage(accepted.language);}
        }return r;};
    {
        SettingsWindow window(defaults,probe,apply);
        Check(SettingsWindowTestAccess::Create(window),"native property sheet creates without desktop UI");
        auto native=SettingsWindowTestAccess::Window(window);
        if(native) {
            wchar_t cls[32]{};GetClassNameW(native,cls,32);
            Check(wcscmp(cls,L"#32770")==0 && SettingsWindowTestAccess::CurrentPage(window)==0 && !IsWindowVisible(native),
                "root is a native dialog; initial Options page remains hidden in tests");
            Check(!IsWindowVisible(GetDlgItem(native,0x3021)) && !GetDlgItem(native,4508),"Apply button removed");
            DWORD affinity=99;
            Check(GetWindowDisplayAffinity(native,&affinity) && affinity==WDA_NONE,"settings is capturable");
            Check(SettingsWindowTestAccess::Conflict(window,1),"occupied shortcut is marked red");
            auto options=SettingsWindowTestAccess::Page(window,0);
            auto chooseQuality=[&](LensSharpness level) {
                SendDlgItemMessageW(options,4500,CB_SETCURSEL,WPARAM(level),0);
                SendMessageW(options,WM_COMMAND,MAKEWPARAM(4500,CBN_SELCHANGE),LPARAM(GetDlgItem(options,4500)));
            };
            chooseQuality(LensSharpness::Off);
            Check(SettingsWindowTestAccess::Draft(window).quality==LensSharpness::Off,"native combo changes reach the draft");
            chooseQuality(LensSharpness::Medium);
            for(auto level:{LensSharpness::Low,LensSharpness::High,LensSharpness::Medium}) {
                SendMessageW(options,WM_COMMAND,MAKEWPARAM(4500,CBN_DROPDOWN),LPARAM(GetDlgItem(options,4500)));
                chooseQuality(level);
                SendMessageW(options,WM_COMMAND,MAKEWPARAM(4500,CBN_SELENDOK),LPARAM(GetDlgItem(options,4500)));
                SendMessageW(options,WM_COMMAND,MAKEWPARAM(4500,CBN_SELENDCANCEL),LPARAM(GetDlgItem(options,4500)));
                SettingsWindowTestAccess::Refresh(window);
                Check(SettingsWindowTestAccess::Draft(window).quality==level,"committed quality choice survives focus/cancel notification and conflict refresh");
            }
            SendDlgItemMessageW(options,4512,BM_CLICK,0,0);
            Check(SettingsWindowTestAccess::Draft(window).fullscreenAspectFit,"aspect-fit checkbox changes reach the draft");
            SettingsWindowTestAccess::Language(window,AppLanguage::English);
            Check(SettingsWindowTestAccess::Draft(window).language==AppLanguage::English,
                "SELENDOK/CLOSEUP/SELCHANGE sequence retains English");
            SettingsWindowTestAccess::NotifyLanguage(window,CBN_SELENDCANCEL);
            Check(SettingsWindowTestAccess::Draft(window).language==AppLanguage::English,
                "focus loss after a committed language choice does not restore the old language");
            SettingsWindowTestAccess::NotifyLanguage(window,CBN_KILLFOCUS);
            SettingsWindowTestAccess::NotifyLanguage(window,CBN_SETFOCUS);
            SettingsWindowTestAccess::SelectPage(window,1);
            auto previousProbes=probes;occupied=false;SettingsWindowTestAccess::Refresh(window);
            SendMessageW(native,WM_ACTIVATE,WA_INACTIVE,0);
            SendMessageW(native,WM_ACTIVATE,WA_ACTIVE,0);
            Check(probes>previousProbes && !SettingsWindowTestAccess::Conflict(window,1) &&
                SettingsWindowTestAccess::Draft(window).language==AppLanguage::English &&
                SettingsWindowTestAccess::CurrentPage(window)==1 && SettingsWindowTestAccess::Window(window)==native,
                "native page switching and activation preserve language/draft and refresh conflicts");
            SettingsWindowTestAccess::Draft(window).hotkeys=custom.hotkeys;
            SettingsWindowTestAccess::Draft(window).screenshotDirectory=custom.screenshotDirectory;
            failSave=true;SettingsWindowTestAccess::Confirm(window);
            Check(IsWindow(native) && SettingsWindowTestAccess::Dirty(window) && accepted==defaults,
                "failed OK keeps the sheet, draft and previous settings");
            SettingsWindowTestAccess::Draft(window).hotkeys.bindings[1]=custom.hotkeys.bindings[0];
            int before=applied;SettingsWindowTestAccess::Confirm(window);
            Check(applied==before && SettingsWindowTestAccess::Conflict(window,0) && SettingsWindowTestAccess::Conflict(window,1),
                "duplicate binding rejects OK and marks both rows");
            SettingsWindowTestAccess::Draft(window).hotkeys=custom.hotkeys;
            Check(SettingsWindowTestAccess::LayoutValid(window),"native centered shortcuts and fixed footer fit");
            SetWindowPos(native,nullptr,0,0,570,420,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            SCROLLINFO scroll{sizeof(scroll),SIF_ALL};
            Check(SettingsWindowTestAccess::LayoutValid(window) && GetScrollInfo(SettingsWindowTestAccess::Page(window,1),SB_VERT,&scroll) &&
                scroll.nMax>=int(scroll.nPage),"small work area keeps footer and scrollable content accessible");
            failSave=false;before=applied;
            SendMessageW(GetDlgItem(native,IDOK),BM_CLICK,0,0);
            SettingsWindowTestAccess::PumpPosted(window);
            Check(!IsWindow(native) && applied==before+1 && accepted.language==AppLanguage::English &&
                accepted.hotkeys==custom.hotkeys,"OK saves once across three native pages and closes");
        }
    }
    {
        auto restarted=LoadAppSettings(identity);
        Check(restarted.language==AppLanguage::English,"control-selected English survives persisted restart");
        SettingsWindow window(restarted,probe,apply);
        Check(SettingsWindowTestAccess::Create(window),"reopen saved configuration");
        auto native=SettingsWindowTestAccess::Window(window);
        if(native) {
            Check(SettingsWindowTestAccess::Text(native)==L"Settings","reopened property sheet uses saved English");
            auto options=SettingsWindowTestAccess::Page(window,0);
            Check(IsDlgButtonChecked(options,4512)==BST_CHECKED,"saved aspect-fit preference is displayed");
            Check(SettingsWindowTestAccess::Text(GetDlgItem(options,4503))==custom.screenshotDirectory,"saved path is displayed");
            SettingsWindowTestAccess::Language(window,AppLanguage::SimplifiedChinese);
            auto before=applied;SettingsWindowTestAccess::Cancel(window);
            Check(!IsWindow(native) && applied==before && CurrentAppLanguage()==AppLanguage::English,
                "Cancel discards language draft and does not write");
        }
    }
    {
        SettingsWindow window(accepted,probe,apply);
        Check(SettingsWindowTestAccess::Create(window),"create close/notification test");
        auto native=SettingsWindowTestAccess::Window(window);
        if(native) {
            SettingsWindowTestAccess::NotifyLanguage(window,CBN_DROPDOWN);
            auto combo=GetDlgItem(SettingsWindowTestAccess::Page(window,0),4502);
            SendMessageW(combo,CB_SETCURSEL,0,0);SettingsWindowTestAccess::NotifyLanguage(window,CBN_SELCHANGE);
            SettingsWindowTestAccess::NotifyLanguage(window,CBN_SELENDCANCEL);
            Check(SettingsWindowTestAccess::Draft(window).language==AppLanguage::English,"cancelled dropdown restores its opening language");
            SendMessageW(combo,CB_SETCURSEL,1,0);SettingsWindowTestAccess::NotifyLanguage(window,CBN_SELENDCANCEL);
            Check(SettingsWindowTestAccess::Draft(window).language==AppLanguage::English,
                "a completed dropdown cancellation cannot be replayed from stale state");
            auto shortcuts=SettingsWindowTestAccess::Page(window,1);
            SendMessageW(shortcuts,WM_COMMAND,MAKEWPARAM(4100,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(shortcuts,4100)));
            Check(SettingsWindowTestAccess::Escape(window) && IsWindow(native),"Esc cancels recording before the settings lifetime");
            int before=applied;
            SendMessageW(native,PSM_APPLY,0,0);
            SettingsWindowTestAccess::Dispatch(window,WM_CLOSE);
            SettingsWindowTestAccess::PumpPosted(window);
            Check(!IsWindow(native) && applied==before,"no standalone Apply; title-bar close does not save");
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER,key.c_str()); // This test's randomly named configuration tree only.

    auto device=std::make_shared<D3DDevice>();
    if(device->Initialize()) {
        {
            InputState state;
            auto input=std::make_shared<InputMappingCoordinator>(nullptr,InputMappingCoordinator::NotificationCallback{},
                InputMappingDependencies{[&](HWND){return std::make_unique<FakeEngine>(state);},[]{return true;}});
            input->Initialize();AppController controller;
            Check(SettingsControllerTestAccess::Prepare(controller,device,input),"prepare controller without real hotkeys or input");
            auto& manager=SettingsControllerTestAccess::Manager(controller);
            auto monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
            auto id=manager.Create(monitor,{0,0,80,80},{-22000,-22000,-21700,-21800});
            if(id) {
                LensSettingsTestAccess::Arm(manager,*id);
                input->Route(LensSettingsTestAccess::Lens(manager,*id).MappingConfig());
                auto starts=state.starts,stops=state.stops;
                SettingsControllerTestAccess::Open(controller);
                auto sheet=SettingsControllerTestAccess::Dialog(controller);
                Check(sheet && IsWindow(SettingsWindowTestAccess::Window(*sheet)) &&
                    IsWindowEnabled(SettingsControllerTestAccess::Owner(controller)),"opening settings returns immediately and leaves controller enabled");
                Check(state.starts==starts && state.stops==stops && input->HasRoute() &&
                    LensSettingsTestAccess::Armed(manager)==1,"opening settings preserves the active engine and mapping standby");
                if(sheet) {
                    auto originalRuntime=Runtime();auto observedRuntime=originalRuntime;NotificationCounter notifications;
                    observedRuntime.diagnostics=&notifications;SetRuntime(observedRuntime);
                    // Exercise the production NewRegion dispatch without starting
                    // capture: an already-pending selection must reach its normal
                    // guard instead of redirecting to the settings dialog.
                    SettingsControllerTestAccess::PendingSelection(controller,monitor);
                    SettingsControllerTestAccess::NewRegionShortcut(controller);
                    SettingsControllerTestAccess::PendingSelection(controller,nullptr);
                    SetRuntime(originalRuntime);
                    Check(notifications.notifications==1,"new-region shortcut reaches selection logic while settings is open");
                    MSG hotkey{};hotkey.hwnd=SettingsControllerTestAccess::Owner(controller);hotkey.message=WM_HOTKEY;hotkey.wParam=3;
                    Check(!controller.ProcessMessage(hotkey),"modeless settings does not consume controller hotkey messages");
                    SettingsControllerTestAccess::CloseAllShortcut(controller);
                    Check(manager.Count()==0 && LensSettingsTestAccess::Armed(manager)==0 &&
                        IsWindow(SettingsWindowTestAccess::Window(*sheet)),"close-all shortcut works while settings stays open");
                    SettingsWindowTestAccess::Cancel(*sheet);SettingsWindowTestAccess::PumpPosted(*sheet);
                    controller.ProcessMessage(hotkey);
                    Check(!SettingsControllerTestAccess::Dialog(controller) && LensSettingsTestAccess::Armed(manager)==0,
                        "closing settings retires its instance without rearming closed regions");
                }
            } else Check(false,"create offscreen controller-test region");
        }
        InputState state;
        auto coordinator=std::make_shared<InputMappingCoordinator>(nullptr,InputMappingCoordinator::NotificationCallback{},
            InputMappingDependencies{[&](HWND){return std::make_unique<FakeEngine>(state);},[]{return true;}});
        coordinator->Initialize();
        LensManager manager(device,coordinator,{});
        auto monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
        manager.SetNewWindowTopmost(false); manager.SetQualityLevel(LensSharpness::Off);
        auto first=manager.Create(monitor,{0,0,80,80},{-22000,-22000,-21700,-21800});
        Check(first.has_value(),"create owned offscreen lens with configured defaults");
        if(first) {
            auto& lens=LensSettingsTestAccess::Lens(manager,*first); lens.Hide();
            Check(!LensSettingsTestAccess::Requested(lens) && !(GetWindowLongPtrW(lens.Window(),GWL_EXSTYLE)&WS_EX_TOPMOST),"new lens follows both defaults");
            manager.SetNewWindowTopmost(true); manager.SetQualityLevel(LensSharpness::Medium);
            Check(LensSettingsTestAccess::Quality(lens)==NewLensQuality && lens.Hidden() &&
                !(GetWindowLongPtrW(lens.Window(),GWL_EXSTYLE)&WS_EX_TOPMOST),"apply sharpens hidden region without revealing or pinning it");
            manager.SetFullscreenAspectFitEnabled(true);
            Check(LensSettingsTestAccess::AspectFit(lens) && lens.Hidden(),
                "apply updates aspect-fit on an existing hidden region without revealing it");
            auto content=LensSettingsTestAccess::FullscreenContent(lens);
            Check(content.right-content.left==content.bottom-content.top && content.left>0,
                "fullscreen render, mapping and screenshot share the fitted content extent instead of black bars");
            LensSettingsTestAccess::FailQuality(lens); manager.SetQualityLevel(LensSharpness::Medium);
            Check(LensSettingsTestAccess::Quality(lens)==LensQualitySettings{},"unrelated/repeated Apply does not retry a failed resource");
            manager.SetQualityLevel(LensSharpness::Off); manager.SetQualityLevel(LensSharpness::Medium);
            Check(LensSettingsTestAccess::Quality(lens)==NewLensQuality,"explicit off/on permits a resource retry");
            auto second=manager.Create(monitor,{0,0,80,80},{-21600,-22000,-21300,-21800});
            Check(second && LensSettingsTestAccess::Lens(manager,*second).KeepsTopmost() &&
                LensSettingsTestAccess::AspectFit(LensSettingsTestAccess::Lens(manager,*second)),
                "new defaults pin and enable aspect-fit only on subsequently created region");
            if(second) {
                LensSettingsTestAccess::Arm(manager,*second);
                coordinator->Route(LensSettingsTestAccess::Lens(manager,*second).MappingConfig());
                auto starts=state.starts;
                manager.SetSettingsCommitInProgress(true);
                Check(state.stops==1 && LensSettingsTestAccess::Armed(manager)==1,"settings commit stops the sole engine/guard while preserving standby");
                for(int i=0;i<50;++i) manager.RefreshInputMappingFromCursor();
                Check(LensSettingsTestAccess::Armed(manager)==1,"periodic UI pumps preserve a deliberately stopped settings session");
                manager.SetSettingsCommitInProgress(false);
                Check(state.starts==starts+1 && LensSettingsTestAccess::Armed(manager)==1,"settings close starts a fresh safe route");
                manager.SetSettingsCommitInProgress(true);
                auto& shutdownLens=LensSettingsTestAccess::Lens(manager,*second);
                Check(shutdownLens.SetInputPassThrough(true) &&
                    (GetWindowLongPtrW(shutdownLens.Window(),GWL_EXSTYLE)&WS_EX_TRANSPARENT),
                    "offscreen lens simulates a pending transparent surface at global shutdown");
                manager.DisableAllInputMappings();
                Check(!(GetWindowLongPtrW(shutdownLens.Window(),GWL_EXSTYLE)&WS_EX_TRANSPARENT),
                    "global shutdown restores hit testing before software pointer retirement");
                manager.SetSettingsCommitInProgress(false);
                Check(state.starts==starts+1 && LensSettingsTestAccess::Armed(manager)==0,"global shutdown during settings cannot rearm old sessions");
                LensSettingsTestAccess::Arm(manager,*second);
                coordinator->Route(LensSettingsTestAccess::Lens(manager,*second).MappingConfig());
                starts=state.starts; state.failDrain=true;
                manager.SetSettingsCommitInProgress(true);
                state.failDrain=false; manager.SetSettingsCommitInProgress(false);
                Check(state.starts==starts && LensSettingsTestAccess::Armed(manager)==0,
                    "failed input drain when opening settings clears standby and never restarts it on close");
            }
        }
    } else Check(false,"D3D initialization for owned-window settings tests");
    SetAppLanguage(oldLanguage);
    if(SUCCEEDED(apartment)) CoUninitialize();
    if(!failures) std::cout<<"Unified settings persistence, transactions, hidden UI and fake-engine standby tests passed.\n";
    return failures;
}
