#include "pch.h"
#include "SettingsWindowTestAccess.h"
#include <iostream>

using namespace RegionLens::native;

namespace
{
    void DispatchNative(SettingsWindow& sheet,UINT message,WPARAM wp=0,LPARAM lp=0)
    {
        // Use the shipping message-loop step, including native completion.
        SettingsWindowTestAccess::Dispatch(sheet,message,wp,lp);
    }
    HotkeyRegistrationResult Available(HotkeySettings const& keys)
    {
        HotkeyRegistrationResult result;
        for(size_t i=0;i<HotkeyActionCount;++i)result.items[i]={keys.bindings[i].Enabled(),true,0};
        return result;
    }
    void Pump(SettingsWindow& sheet)
    {
        SettingsWindowTestAccess::PumpPosted(sheet);
    }
}

int RunSettingsCommandTests()
{
    int failures{};auto check=[&](bool ok,char const* label){if(!ok){++failures;std::cerr<<"FAILED settings native command: "<<label<<'\n';}};
    auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    auto priorLanguage=CurrentAppLanguage();SetAppLanguage(AppLanguage::SimplifiedChinese);
    {
        std::vector<bool> recording;
        SettingsWindow sheet(AppSettings{},Available,[](auto const&){return SettingsApplyResult{};},
            [&](bool active){recording.push_back(active);});
        check(SettingsWindowTestAccess::Create(sheet),"create modeless recording test");
        auto root=SettingsWindowTestAccess::Window(sheet);
        if(root) {
            MSG unrelated{};unrelated.message=WM_HOTKEY;unrelated.wParam=3;
            check(!sheet.ProcessMessage(unrelated),"thread/controller hotkeys bypass the modeless sheet");
            SettingsWindowTestAccess::SelectPage(sheet,1);
            auto page=SettingsWindowTestAccess::Page(sheet,1);
            auto record=[&]{SendMessageW(page,WM_COMMAND,MAKEWPARAM(4100,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(page,4100)));};
            record();SettingsWindowTestAccess::Escape(sheet);
            record();SendMessageW(root,WM_ACTIVATE,WA_INACTIVE,0);
            record();SettingsWindowTestAccess::SelectPage(sheet,0);
            SettingsWindowTestAccess::SelectPage(sheet,1);record();sheet.Close();
            check(recording==std::vector<bool>({true,false,true,false,true,false,true,false}),
                "recording, Escape, focus loss, page change and close balance shortcut release/restoration");
        }
    }
    for(int page=0;page<3;++page)for(int path=0;path<8;++path) {
        int commits{};AppSettings applied;
        SettingsWindow sheet(applied,Available,[&](auto const& value){++commits;applied=value;return SettingsApplyResult{};});
        check(SettingsWindowTestAccess::Create(sheet),"hidden native sheet creates");
        HWND native=SettingsWindowTestAccess::Window(sheet);
        if(!native)continue;
        SettingsWindowTestAccess::Language(sheet,AppLanguage::English);
        // Match the focus transition seen with a physical OK click.  A late
        // cancel notification after SELENDOK cannot revive Chinese.
        SettingsWindowTestAccess::NotifyLanguage(sheet,CBN_SELENDCANCEL);
        SettingsWindowTestAccess::SelectPage(sheet,page);
        switch(path) {
        case 0: DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);break;
        case 1: DispatchNative(sheet,WM_COMMAND,MAKEWPARAM(IDOK,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(native,IDOK)));break;
        case 2: DispatchNative(sheet,WM_KEYDOWN,VK_RETURN);break;
        case 3: DispatchNative(sheet,WM_SYSCOMMAND,SC_CLOSE);break;
        case 4: DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_CANCEL);break;
        case 5: DispatchNative(sheet,WM_CLOSE);break;
        case 6: DispatchNative(sheet,WM_KEYDOWN,VK_ESCAPE);break;
        case 7: SendMessageW(GetDlgItem(native,IDOK),BM_CLICK,0,0);break;
        }
        Pump(sheet); // PropSheet_PressButton posts its command for the next loop step.
        if(page==1 && path==2) {
            // Native dialog navigation focuses Record on this page. Enter
            // activates that focused button, not the global OK action.
            check(commits==0 && IsWindow(native) && SettingsWindowTestAccess::Recording(sheet),"Enter respects the focused Record button");
            DispatchNative(sheet,WM_KEYDOWN,VK_ESCAPE);
            check(!SettingsWindowTestAccess::Recording(sheet) && IsWindow(native),"first Esc cancels recording only");
            DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);
        }
        if(path<3 || path==7) {
            check(commits==1 && applied.language==AppLanguage::English,"native OK commits English exactly once");
            check(!IsWindow(native) && SettingsWindowTestAccess::ClosedCleanly(sheet),"native OK closes the modeless sheet");
        } else {
            check(commits==0 && applied.language==AppLanguage::SimplifiedChinese,"native title-bar X never saves");
            check(!IsWindow(native) && SettingsWindowTestAccess::ClosedCleanly(sheet),"native title-bar X closes the modeless sheet");
        }
        if(IsWindow(native)) std::cout<<"Unclosed sheet: page="<<page<<", path="<<path<<", current page="<<PropSheet_GetCurrentPageHwnd(native)<<", commits="<<commits<<'\n';
    }
    // Save failure, invalid bindings, callback exception, and retry all use the
    // native property-sheet pipeline. No path may partially close the pages.
    for(int failure=0;failure<3;++failure) {
        int attempts{};bool fail=true;AppSettings applied;
        SettingsWindow sheet(applied,Available,[&](auto const& value) {
            ++attempts;SettingsApplyResult result;result.availability=Available(value.hotkeys);
            if(fail) {
                if(failure==2)throw std::runtime_error("test settings save failure");
                result.issue=failure==0?SettingsApplyIssue::Storage:SettingsApplyIssue::Hotkey;
                result.error=E_ACCESSDENIED;
                if(failure==1)result.availability.items[1]={true,false,ERROR_HOTKEY_ALREADY_REGISTERED};
            }else applied=value;
            return result;
        });
        check(SettingsWindowTestAccess::Create(sheet),"create failed-save test");
        HWND root=SettingsWindowTestAccess::Window(sheet);if(!root)continue;
        SettingsWindowTestAccess::Language(sheet,AppLanguage::English);
        DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);Pump(sheet);
        check(IsWindow(root) && attempts==1 && applied.language==AppLanguage::SimplifiedChinese &&
            SettingsWindowTestAccess::Draft(sheet).language==AppLanguage::English,"failure keeps the window, draft and old language");
        for(int i=0;i<3;++i)check(IsWindow(SettingsWindowTestAccess::Page(sheet,i)),"failed OK retains all native pages");
        DispatchNative(sheet,PSM_APPLY);
        DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_APPLYNOW);
        check(attempts==1 && IsWindow(root),"neither Apply entry point can commit settings");
        fail=false;DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);
        check(attempts==2 && applied.language==AppLanguage::English && SettingsWindowTestAccess::ClosedCleanly(sheet),"retry saves exactly once and closes cleanly");
    }
    {
        int attempts{};AppSettings applied;SettingsWindow* current{};
        SettingsWindow sheet(applied,Available,[&](auto const& value) {
            ++attempts;
            DispatchNative(*current,PSM_PRESSBUTTON,PSBTN_OK);
            DispatchNative(*current,WM_SYSCOMMAND,SC_CLOSE);
            check(IsWindow(SettingsWindowTestAccess::Window(*current)),"nested OK/X cannot destroy a validating page");
            applied=value;return SettingsApplyResult{};
        });current=&sheet;
        check(SettingsWindowTestAccess::Create(sheet),"create reentrant command test");
        if(SettingsWindowTestAccess::Window(sheet)) {
            SettingsWindowTestAccess::Language(sheet,AppLanguage::English);
            DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);
            check(attempts==1 && applied.language==AppLanguage::English && SettingsWindowTestAccess::ClosedCleanly(sheet),"nested commands do not duplicate commits");
        }
    }
    {
        SettingsWindow* current{};int attempts{};
        SettingsWindow sheet(AppSettings{},Available,[&](auto const&) {
            ++attempts;current->Close();
            check(IsWindow(SettingsWindowTestAccess::Window(*current)),"shutdown defers destruction until native notifications unwind");
            return SettingsApplyResult{SettingsApplyIssue::Cancelled,HRESULT_FROM_WIN32(ERROR_CANCELLED),{}};
        });current=&sheet;
        check(SettingsWindowTestAccess::Create(sheet),"create shutdown-during-save test");
        if(SettingsWindowTestAccess::Window(sheet)) {
            SettingsWindowTestAccess::Language(sheet,AppLanguage::English);
            DispatchNative(sheet,PSM_PRESSBUTTON,PSBTN_OK);
            check(attempts==1 && SettingsWindowTestAccess::ClosedCleanly(sheet),"shutdown finishes after native callback safely returns");
        }
    }
    for(int iteration=0;iteration<12;++iteration) {
        int commits{};
        SettingsWindow sheet(AppSettings{},Available,[&](auto const&){++commits;return SettingsApplyResult{};});
        check(SettingsWindowTestAccess::Create(sheet),"repeated settings creation");
        auto root=SettingsWindowTestAccess::Window(sheet);if(!root)continue;
        SettingsWindowTestAccess::Language(sheet,AppLanguage::English);
        // A sent native command can complete outside the GetMessage dispatch;
        // the finish-check message must wake the loop and destroy the shell.
        SendMessageW(root,PSM_PRESSBUTTON,PSBTN_CANCEL,0);Pump(sheet);
        check(commits==0 && SettingsWindowTestAccess::ClosedCleanly(sheet),"sent Cancel leaves no live shell or stale control handles");
    }
    SetAppLanguage(priorLanguage);if(SUCCEEDED(com))CoUninitialize();
    if(!failures)std::cout<<"Native property-sheet command and close protocol tests passed.\n";
    return failures;
}
