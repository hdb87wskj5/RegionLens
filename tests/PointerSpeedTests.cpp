#include "PointerSpeedPopup.h"
#include "LensChrome.h"
#include "MappingStandbyPolicy.h"
#include <iostream>
#include <limits>

using namespace RegionLens::native;
namespace {
    int failures{};
    void Check(bool ok, char const* message) { if (!ok) { ++failures; std::cerr << "FAILED speed: " << message << '\n'; } }
}
int RunPointerSpeedTests()
{
    PointerSpeed first, second;
    Check(first.Multiplier()==1.0 && !first.Adjusted() && PointerSpeedText(first)==L"1.00×", "default is exactly neutral 1.00x");
    first={31}; Check(first.Multiplier()==1.55 && second.Multiplier()==1.0,"per-region speed remains independent");
    Check(PointerSpeedButtonAction({},false)==PointerSpeedClick::OpenPanel &&
        PointerSpeedButtonAction({},true)==PointerSpeedClick::ClosePanel,
        "neutral button opens or closes the slider without changing speed");
    for(int tick=10;tick<=50;++tick) if(tick!=PointerSpeed::Default) {
        for(bool panelOpen:{false,true}) {
            PointerSpeed adjusted{tick}, other{31};
            Check(PointerSpeedButtonAction(adjusted,panelOpen)==PointerSpeedClick::ResetSpeed,
                "every non-neutral speed resets on click, including with the panel open");
            adjusted={};
            Check(!adjusted.Adjusted() && adjusted.Multiplier()==1.0 && other.Tick()==31 &&
                PointerSpeedButtonAction(adjusted,false)==PointerSpeedClick::OpenPanel,
                "reset dims the button, preserves other regions and permits reopening on the next click");
        }
    }
    Check(PointerSpeed{(std::numeric_limits<int>::min)()}.Multiplier()==0.5 &&
        PointerSpeed{(std::numeric_limits<int>::max)()}.Multiplier()==2.5,"invalid/extreme ticks are bounded before gain conversion");
    for(int tick=10;tick<=50;++tick) {
        auto text=PointerSpeedText({tick});
        Check(text.size()==5 && text[1]==L'.' && (text[3]==L'0' || text[3]==L'5'),"all 41 ticks have stable two-decimal labels");
    }
    Check(PointerSpeedText({10})==L"0.50×" && PointerSpeedText({50})==L"2.50×","range endpoint labels are exact");
    Check(PointerSpeedAtTrackPosition(0,0,400).Tick()==10 && PointerSpeedAtTrackPosition(100,0,400).Tick()==20 &&
        PointerSpeedAtTrackPosition(200,0,400).Tick()==30 && PointerSpeedAtTrackPosition(400,0,400).Tick()==50,
        "track endpoints, quarter and centre jump to 0.50x/1.00x/1.50x/2.50x directly");
    Check(PointerSpeedAtTrackPosition(4,0,400).Tick()==10 && PointerSpeedAtTrackPosition(5,0,400).Tick()==11 &&
        PointerSpeedAtTrackPosition(-20,0,400).Tick()==10 && PointerSpeedAtTrackPosition(900,0,400).Tick()==50,
        "position quantizes to nearest 0.05 with deterministic half-step rounding and endpoint clamping");
    Check(PointerSpeedAtTrackPosition(50,20,20).Tick()==20 && PointerSpeedAtTrackPosition(50,30,20).Tick()==20 &&
        PointerSpeedAtTrackPosition((std::numeric_limits<LONG>::max)(),(std::numeric_limits<LONG>::min)(),
            (std::numeric_limits<LONG>::max)()).Tick()==50,
        "degenerate and extreme track coordinates cannot overflow");
    for(auto work:{RECT{0,0,1920,1080},RECT{-1920,-1080,0,0},RECT{-200,0,0,128}}) {
        for(auto anchor:{POINT{work.left,work.top},POINT{work.right,work.bottom}}) {
            auto r=PointerSpeedPopupBounds(anchor,{600,256},work);
            Check(r.left>=work.left && r.top>=work.top && r.right<=work.right && r.bottom<=work.bottom &&
                r.right>r.left && r.bottom>r.top,"popup stays within monitor bounds at corners and mixed DPI sizes");
        }
    }
    MappingUiPauseState pause; MappingStandbyPolicy standby;
    standby.Arm(1);standby.Arm(2);standby.CommitRoute(1);
    pause.SpeedPopup(true); standby.Suspend(); pause.Settings(true); pause.SpeedPopup(false);
    Check(pause.Paused() && standby.Count()==2,"closing slider cannot clear another settings pause or standby");
    pause.SpeedPopup(true);pause.Settings(false);pause.Selection(true);pause.SpeedPopup(false);
    Check(pause.Paused(),"selection retains its own pause when the slider closes");
    pause.Selection(false);pause.SpeedPopup(true);standby.Clear();pause.SpeedPopup(false);
    Check(!pause.Paused() && standby.Resume()==0 && standby.Empty(),"global deactivation while slider is open cannot re-arm anything");
    SetAppLanguage(AppLanguage::SimplifiedChinese);
    Check(std::wstring(ChromeTooltipText(PointerSpeedButtonId,false,false,false))==L"鼠标速度","Chinese button is speed, not matching toggle");
    Check(std::wstring(ChromeTooltipText(PointerSpeedButtonId,false,false,false,true)).find(L"点击恢复 1.00×")!=std::wstring::npos,
        "adjusted speed explains one-click reset in Chinese");
    SetAppLanguage(AppLanguage::English);
    Check(std::wstring(ChromeTooltipText(PointerSpeedButtonId,false,false,false))==L"Mouse speed","English button is localized");
    Check(std::wstring(ChromeTooltipText(PointerSpeedButtonId,false,false,false,true)).find(L"reset to 1.00x")!=std::wstring::npos,
        "adjusted speed explains one-click reset in English");

    // Hidden native control only: no ShowWindow, foreground changes, hooks,
    // physical cursor operations or synthetic desktop mouse/keyboard input.
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES};
    Check(InitCommonControlsEx(&controls)!=FALSE,"native slider class initializes");
    auto host=CreateWindowExW(0,L"STATIC",L"Hidden speed test",WS_POPUP,0,0,300,128,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    auto slider=CreateWindowExW(0,TRACKBAR_CLASSW,L"Speed",WS_CHILD|TBS_NOTICKS,0,0,260,32,host,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(host && slider,"hidden slider fixture created");
    if(slider) {
        ConfigurePointerSpeedSlider(slider,{});
        Check(SendMessageW(slider,TBM_GETRANGEMIN,0,0)==10 && SendMessageW(slider,TBM_GETRANGEMAX,0,0)==50 &&
            SendMessageW(slider,TBM_GETPOS,0,0)==20,"production slider config uses 0.50..2.50 with neutral initial value");
        Check(SendMessageW(slider,TBM_GETLINESIZE,0,0)==1 && SendMessageW(slider,TBM_GETPAGESIZE,0,0)==1,
            "keyboard and track-page increments are both 0.05");
        for(int tick=10;tick<=50;++tick) {
            ConfigurePointerSpeedSlider(slider,{tick});
            Check(SendMessageW(slider,TBM_GETPOS,0,0)==tick,"native slider represents every allowed value");
        }
        // Production seek helper, invoked directly on a hidden control. No
        // WM_*BUTTON injection, focus changes or desktop mouse capture.
        for(int width:{200,260,390,520}) {
            SetWindowPos(slider,nullptr,0,0,width,32,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            RECT firstThumb{}, lastThumb{};
            auto track=ConfigurePointerSpeedSlider(slider,{10});
            SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&firstThumb));
            ConfigurePointerSpeedSlider(slider,{50});
            SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&lastThumb));
            auto firstX=(firstThumb.left+firstThumb.right)/2, lastX=(lastThumb.left+lastThumb.right)/2;
            if(lastX<=firstX || firstThumb.bottom<=firstThumb.top) {
                RECT channel{};SendMessageW(slider,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
                std::cerr<<"Native slider geometry width="<<width<<" min="<<firstThumb.left<<','<<firstThumb.top<<','<<firstThumb.right<<','<<firstThumb.bottom
                    <<" max="<<lastThumb.left<<','<<lastThumb.top<<','<<lastThumb.right<<','<<lastThumb.bottom
                    <<" channel="<<channel.left<<','<<channel.top<<','<<channel.right<<','<<channel.bottom<<'\n';
                Check(false,"native thumb endpoints are measurable without desktop interaction");continue;
            }
            for(int tick=10;tick<=50;++tick) {
                ConfigurePointerSpeedSlider(slider,{tick<=30?50:10});
                POINT press{firstX+MulDiv(tick-10,lastX-firstX,40),(firstThumb.top+firstThumb.bottom)/2};
                Check(SeekPointerSpeedSlider(slider,press,track) && SendMessageW(slider,TBM_GETPOS,0,0)==tick,
                    "clicking any of 41 native track positions jumps directly, independent of old speed and control width");
                RECT thumb{};SendMessageW(slider,TBM_GETTHUMBRECT,0,reinterpret_cast<LPARAM>(&thumb));
                Check(PtInRect(&thumb,press)!=FALSE,"forwarded press hits the thumb so native dragging replaces page repeat");
                auto grab=press;
                Check(!SeekPointerSpeedSlider(slider,grab,track) && grab.x==press.x && grab.y==press.y &&
                    SendMessageW(slider,TBM_GETPOS,0,0)==tick,"grabbing current thumb preserves speed and native drag offset");
            }
            ConfigurePointerSpeedSlider(slider,{});
            POINT edge{0,0}; Check(SeekPointerSpeedSlider(slider,edge,track) && SendMessageW(slider,TBM_GETPOS,0,0)==10,
                "left margin clamps to minimum and enters native thumb drag");
            edge={width-1,31};Check(SeekPointerSpeedSlider(slider,edge,track) && SendMessageW(slider,TBM_GETPOS,0,0)==50,
                "right margin clamps to maximum and enters native thumb drag");
        }
        DestroyWindow(slider);
    }
    if(host) DestroyWindow(host);
    if(!failures) std::cout<<"Mouse speed defaults, range, pause safety and hidden native slider tests passed (no desktop input).\n";
    return failures;
}
