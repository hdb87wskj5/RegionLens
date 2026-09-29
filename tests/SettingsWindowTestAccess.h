#pragma once
#include "SettingsWindow.h"

namespace RegionLens::native
{
    struct SettingsWindowTestAccess
    {
        static bool Create(SettingsWindow& w,UINT fontDpi=0)
        {
            if(!fontDpi)return w.Create(nullptr,false);
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,96);
            auto points=std::max(8,MulDiv(abs(metrics.lfMessageFont.lfHeight),72,96));
            // Build real native dialog templates at scaled font metrics.
            // Do not fabricate WM_DPICHANGED without changing OS monitor DPI.
            return w.Create(nullptr,false,UINT(MulDiv(points,fontDpi,GetDpiForSystem())));
        }
        static HWND Window(SettingsWindow& w) { return w.m_window; }
        static HWND Page(SettingsWindow& w,int i) { return w.m_pages[i]; }
        static AppSettings& Draft(SettingsWindow& w) { return w.m_draft.editing; }
        static void Dispatch(SettingsWindow& w,UINT message,WPARAM wp=0,LPARAM lp=0)
        {
            MSG item{};item.hwnd=w.m_window;item.message=message;item.wParam=wp;item.lParam=lp;
            w.ProcessMessage(item);
        }
        static void Confirm(SettingsWindow& w) { Dispatch(w,WM_COMMAND,IDOK,reinterpret_cast<LPARAM>(GetDlgItem(w.m_window,IDOK))); }
        static void Cancel(SettingsWindow& w) { Dispatch(w,WM_COMMAND,IDCANCEL,reinterpret_cast<LPARAM>(GetDlgItem(w.m_window,IDCANCEL))); }
        static void PumpPosted(SettingsWindow& w)
        {
            auto deadline=GetTickCount64()+250;MSG item{};
            while(GetTickCount64()<deadline && w.m_window && PeekMessageW(&item,w.m_window,0,0,PM_REMOVE))
                w.ProcessMessage(item);
        }
        static bool ClosedCleanly(SettingsWindow& w)
        {
            return w.m_finished && !w.m_window && !w.m_tabs &&
                std::all_of(w.m_pages.begin(),w.m_pages.end(),[](HWND h){return !h;}) &&
                std::all_of(w.m_status.begin(),w.m_status.end(),[](HWND h){return !h;}) &&
                std::all_of(w.m_values.begin(),w.m_values.end(),[](HWND h){return !h;}) &&
                std::all_of(w.m_items.begin(),w.m_items.end(),[](auto const& item){return !item.window;});
        }
        static void SelectPage(SettingsWindow& w,int i) { w.SelectPage(i); }
        static void Refresh(SettingsWindow& w) { w.RefreshAvailability(); }
        static bool Conflict(SettingsWindow& w,size_t i) { return w.ValueHasConflict(i); }
        static bool Dirty(SettingsWindow& w) { return w.m_draft.Dirty(); }
        static bool Recording(SettingsWindow& w) { return w.m_recording.has_value(); }
        static int CurrentPage(SettingsWindow& w) { return w.m_page; }
        static bool Escape(SettingsWindow& w) { return w.HandleEscape(); }
        static void NotifyLanguage(SettingsWindow& w,int notification)
        { SendMessageW(w.m_pages[0],WM_COMMAND,MAKEWPARAM(4502,notification),reinterpret_cast<LPARAM>(GetDlgItem(w.m_pages[0],4502))); }
        static void Language(SettingsWindow& w,AppLanguage language)
        {
            auto combo=GetDlgItem(w.m_pages[0],4502);
            NotifyLanguage(w,CBN_DROPDOWN);
            auto count=SendMessageW(combo,CB_GETCOUNT,0,0);
            for(LRESULT i=0;i<count;++i) if(SendMessageW(combo,CB_GETITEMDATA,WPARAM(i),0)==LRESULT(language))
                SendMessageW(combo,CB_SETCURSEL,WPARAM(i),0);
            // Include the ordering that used to overwrite the user's choice.
            NotifyLanguage(w,CBN_SELENDOK); NotifyLanguage(w,CBN_CLOSEUP); NotifyLanguage(w,CBN_SELCHANGE);
        }
        static std::wstring Text(HWND window)
        { auto n=GetWindowTextLengthW(window); std::wstring text(size_t(n)+1,L'\0'); GetWindowTextW(window,text.data(),n+1); text.resize(n); return text; }
        static bool ShowOffscreen(SettingsWindow& w)
        {
            // The native sheet may center its hidden HWND after PSCB_INITIALIZED.
            // Reposition only while hidden, then verify before allowing a show.
            if(w.m_window && !IsWindowVisible(w.m_window)) SetWindowPos(w.m_window,nullptr,
                GetSystemMetrics(SM_XVIRTUALSCREEN)-20000,GetSystemMetrics(SM_YVIRTUALSCREEN)-20000,0,0,
                SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            if(!w.m_window || MonitorFromWindow(w.m_window,MONITOR_DEFAULTTONULL)) return false;
            w.m_allowShow=true;
            ShowWindow(w.m_window,SW_SHOWNOACTIVATE); UpdateWindow(w.m_window); return true;
        }
        static bool LayoutValid(SettingsWindow& w)
        {
            RECT client{}; GetClientRect(w.m_window,&client);
            for(int id:{IDOK,IDCANCEL}) {
                auto button=GetDlgItem(w.m_window,id); if(!button) return false;
                RECT b{}; GetWindowRect(button,&b); MapWindowPoints(nullptr,w.m_window,reinterpret_cast<POINT*>(&b),2);
                if(b.left<0 || b.right>client.right || b.bottom>client.bottom || b.top<0) return false;
            }
            auto dc=GetDC(w.m_pages[1]); auto old=SelectObject(dc,reinterpret_cast<HFONT>(SendMessageW(w.m_pages[1],WM_GETFONT,0,0)));
            TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics); bool valid=true;
            for(auto window:w.m_values) {
                RECT b{}; GetClientRect(window,&b);
                valid &= b.bottom>metrics.tmHeight && (GetWindowLongPtrW(window,GWL_STYLE)&SS_CENTERIMAGE)!=0;
            }
            SelectObject(dc,old); ReleaseDC(w.m_pages[1],dc);
            return valid;
        }
        static bool AboutRowsFit(SettingsWindow& w)
        {
            auto page=w.m_pages[2];RECT client{};GetClientRect(page,&client);
            LONG previousBottom=-1;int count{};
            for(auto const& item:w.m_items) if(item.page==2 && item.window && item.chinese && item.chinese[0]==L'•') {
                RECT rect{};GetWindowRect(item.window,&rect);
                MapWindowPoints(nullptr,page,reinterpret_cast<POINT*>(&rect),2);
                if(rect.left<0 || rect.right>client.right || rect.top<=previousBottom || rect.bottom>client.bottom) return false;
                previousBottom=rect.bottom;++count;
            }
            return count==5;
        }
    };
}
