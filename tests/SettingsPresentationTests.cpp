#include "pch.h"
#include "SettingsWindowTestAccess.h"
#include "ScreenshotStorage.h"
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace RegionLens::native;

namespace
{
    struct Image { UINT width{},height{}; std::vector<uint8_t> pixels; };
    struct NativeCapture
    {
        explicit NativeCapture(HWND window)
        {
            if(MonitorFromWindow(window,MONITOR_DEFAULTTONULL)) throw std::runtime_error("refusing capture on a real display");
        }
        Image Read(HWND window)
        {
            auto deadline=GetTickCount64()+50;
            do {
                MSG message{};while(GetTickCount64()<deadline && PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
                Sleep(1);
            }while(GetTickCount64()<deadline);
            // Ask Windows to render the complete native window subtree in one
            // call. GDI client areas wholly off-monitor can remain clipped in
            // DWM/WGC even while the nonclient frame is captured normally.
            RECT bounds{};GetWindowRect(window,&bounds);
            Image printed{UINT(bounds.right-bounds.left),UINT(bounds.bottom-bounds.top),{}};
            printed.pixels.resize(size_t(printed.width)*printed.height*4);
            auto screen=GetDC(nullptr);auto dc=CreateCompatibleDC(screen);ReleaseDC(nullptr,screen);
            if(!dc)throw std::runtime_error("native capture DC unavailable");
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=LONG(printed.width);info.bmiHeader.biHeight=-LONG(printed.height);
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            void* bits{};auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
            if(!bitmap){DeleteDC(dc);throw std::runtime_error("native capture bitmap unavailable");}
            auto old=SelectObject(dc,bitmap);
            bool ok=PrintWindow(window,dc,PW_RENDERFULLCONTENT)!=FALSE;
            if(ok){memcpy(printed.pixels.data(),bits,printed.pixels.size());
                for(size_t i=3;i<printed.pixels.size();i+=4)printed.pixels[i]=255;}
            SelectObject(dc,old);DeleteObject(bitmap);DeleteDC(dc);
            if(!ok)throw std::runtime_error("full native window rendering failed");
            return printed;
        }
    };
    RECT CaptureRect(HWND root,HWND control)
    {
        RECT frame{},rect{};GetWindowRect(root,&frame);
        GetClientRect(control,&rect);MapWindowPoints(control,nullptr,reinterpret_cast<POINT*>(&rect),2);
        OffsetRect(&rect,-frame.left,-frame.top);return rect;
    }
    RECT PageRect(HWND page,HWND control)
    {
        RECT rect{};GetWindowRect(control,&rect);
        MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&rect),2);
        return rect;
    }
    bool RowFits(HWND page,std::initializer_list<int> ids)
    {
        RECT client{};GetClientRect(page,&client);
        LONG priorRight=-1, rowTop=-1;
        for(auto id:ids) {
            auto control=GetDlgItem(page,id);if(!control)return false;
            auto rect=PageRect(page,control);
            if(rect.left<0 || rect.right>client.right || rect.top<0 || rect.bottom>client.bottom ||
                rect.left<=priorRight || (rowTop>=0 && abs(rect.top-rowTop)>2)) return false;
            priorRight=rect.right;rowTop=rect.top;
        }
        return true;
    }
    bool LabelFits(HWND control)
    {
        auto value=SettingsWindowTestAccess::Text(control);
        auto dc=GetDC(control);if(!dc)return false;
        auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(control,WM_GETFONT,0,0)));
        SIZE textSize{};auto measured=GetTextExtentPoint32W(dc,value.c_str(),int(value.size()),&textSize)!=FALSE;
        SelectObject(dc,old);ReleaseDC(control,dc);
        RECT client{};GetClientRect(control,&client);
        return measured && textSize.cx<=client.right-client.left-6;
    }
    bool TextVisible(Image const& image,RECT rect,bool red=false,bool centered=false)
    {
        // Ignore control borders; test glyph pixels in Windows' complete
        // native-window rendering, not independently painted child controls.
        rect.left+=3;rect.top+=3;rect.right-=3;rect.bottom-=3;
        if(rect.left<0 || rect.top<0 || rect.right>LONG(image.width) || rect.bottom>LONG(image.height)) return false;
        RECT ink{rect.right,rect.bottom,rect.left,rect.top};int count{};
        for(LONG y=rect.top;y<rect.bottom;++y)for(LONG x=rect.left;x<rect.right;++x) {
            auto p=&image.pixels[(size_t(y)*image.width+x)*4];
            bool glyph=red?p[2]>130 && p[2]>p[1]+50 && p[2]>p[0]+50:p[0]<130 && p[1]<130 && p[2]<130;
            if(glyph){++count;ink.left=std::min(ink.left,x);ink.right=std::max(ink.right,x);ink.top=std::min(ink.top,y);ink.bottom=std::max(ink.bottom,y);}
        }
        if(count<10)return false;
        return !centered || (abs((ink.left+ink.right)-(rect.left+rect.right))<=8 &&
            abs((ink.top+ink.bottom)-(rect.top+rect.bottom))<=10);
    }
    int Run(bool previews)
    {
        int failures{}; auto check=[&](bool ok,char const* message){if(!ok){++failures;std::cerr<<"FAILED native settings presentation: "<<message<<'\n';}};
        auto apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        auto dpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        auto language=CurrentAppLanguage();
        try {
            std::filesystem::path output=std::filesystem::absolute(L"out/settings-native-preview")/std::to_wstring(GetTickCount64());
            if(previews)std::wcout<<L"Preview directory: "<<output.wstring()<<L'\n';
            if(previews)std::filesystem::create_directories(output);
            for(auto current:{AppLanguage::SimplifiedChinese,AppLanguage::English}) for(UINT targetDpi:{96u,144u,192u}) {
                SetAppLanguage(current);AppSettings settings;settings.language=current;
                settings.screenshotDirectory=L"C:\\Pictures\\RegionLens-Dev";
                auto probe=[](HotkeySettings const& keys){HotkeyRegistrationResult result;
                    for(size_t i=0;i<HotkeyActionCount;++i)result.items[i]={keys.bindings[i].Enabled(),i!=1,DWORD(i==1?ERROR_HOTKEY_ALREADY_REGISTERED:ERROR_SUCCESS)};return result;};
                SettingsWindow sheet(settings,probe,[](auto const&){return SettingsApplyResult{};});
                if(!SettingsWindowTestAccess::Create(sheet,targetDpi) || !SettingsWindowTestAccess::ShowOffscreen(sheet))
                    throw std::runtime_error("could not create an entirely offscreen native sheet");
                HWND root=SettingsWindowTestAccess::Window(sheet);NativeCapture capture(root);
                check((GetWindowLongPtrW(root,GWL_EXSTYLE)&WS_EX_TOPMOST)==0,
                    "settings stays in the normal window band");
                std::cout<<"Native settings font/layout metrics "<<targetDpi<<" DPI\n";
                for(int page=0;page<3;++page) {
                    SettingsWindowTestAccess::SelectPage(sheet,page);auto image=capture.Read(root);
                    if(previews) {
                        auto file=output/((current==AppLanguage::English?L"en-":L"zh-")+std::to_wstring(targetDpi)+L"-"+std::to_wstring(page)+L".png");
                        winrt::check_hresult(WriteScreenshotPng(file.wstring(),{image.width,image.height},image.pixels));
                    }
                    check(SettingsWindowTestAccess::LayoutValid(sheet),"dialog-unit layout and footer fit");
                    check(TextVisible(image,CaptureRect(root,GetDlgItem(root,IDOK)),false,true),"OK label visible and centered");
                    check(TextVisible(image,CaptureRect(root,GetDlgItem(root,IDCANCEL)),false,true),"Cancel label visible and centered");
                    auto body=SettingsWindowTestAccess::Page(sheet,page);
                    if(page==0) {
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4500))),"native checkbox label rendered");
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4512))),"aspect-fit checkbox label rendered");
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4502))),"selected language rendered");
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4503))),"path rendered");
                        check(RowFits(body,{4504,4505,4506}),"screenshot actions share a non-overlapping row");
                    }else if(page==1) {
                        for(int i=0;i<int(HotkeyActionCount);++i) {
                            check(RowFits(body,{4400+i,4300+i,4100+i,4200+i}),"shortcut columns align without clipping");
                            check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4300+i)),i==1,true),"shortcut glyphs centered and conflict red");
                            check(LabelFits(GetDlgItem(body,4300+i)),"shortcut value and conflict label fit their cell");
                        }
                        check(SettingsWindowTestAccess::Text(GetDlgItem(body,4301)).find(current==AppLanguage::English?L"In use":L"已占用")!=std::wstring::npos,
                            "occupied shortcut has an explicit label");
                    } else {
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4513))),"About identity rendered by native page");
                        check(TextVisible(image,CaptureRect(root,GetDlgItem(body,4509))),"About operations rendered by native page");
                        check(SettingsWindowTestAccess::AboutRowsFit(sheet),"all five About operations fit without overlap");
                    }
                    RECT client{},status{};GetClientRect(body,&client);status=PageRect(body,GetDlgItem(body,4511));
                    RECT slack{0,0,0,18};MapDialogRect(body,&slack);
                    check(status.bottom<=client.bottom && client.bottom-status.bottom<=slack.bottom,
                        "compact page keeps the status area near its footer");
                }
                // Re-expose an already rendered page and move away/back while
                // staying wholly outside every monitor; no desktop input used.
                SettingsWindowTestAccess::SelectPage(sheet,0);
                ShowWindow(root,SW_HIDE);RECT location{};GetWindowRect(root,&location);
                SetWindowPos(root,nullptr,location.left-1000,location.top-1000,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                SetWindowPos(root,nullptr,location.left,location.top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
                ShowWindow(root,SW_SHOWNOACTIVATE);auto image=capture.Read(root);
                check(TextVisible(image,CaptureRect(root,GetDlgItem(SettingsWindowTestAccess::Page(sheet,0),4503))),"re-exposed page retains text");
                SendMessageW(root,WM_ACTIVATE,WA_INACTIVE,0);
                check(IsWindowVisible(root),"loss of activation does not hide settings");
                DWORD affinity{};check(GetWindowDisplayAffinity(root,&affinity) && affinity==WDA_NONE,"native root remains capturable");
                check((GetWindowLongPtrW(root,GWL_EXSTYLE)&WS_EX_TOPMOST)==0,
                    "settings remains non-topmost after exposure and deactivation");
            }
        }catch(winrt::hresult_error const& e){++failures;std::cerr<<"Native settings capture HRESULT "<<std::hex<<uint32_t(e.code())<<'\n';}
        catch(std::exception const& e){++failures;std::cerr<<"Native settings capture: "<<e.what()<<'\n';}
        SetAppLanguage(language);SetThreadDpiAwarenessContext(dpi);if(SUCCEEDED(apartment))CoUninitialize();
        if(!failures)std::cout<<"Native property-sheet composition, centered text, capture and re-exposure passed.\n";
        return failures;
    }
}
int RunSettingsPresentationTests(){return Run(false);}
int RunSettingsPreview(){return Run(true);}
