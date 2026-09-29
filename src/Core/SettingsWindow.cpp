#include "pch.h"
#include "SettingsWindow.h"
#include "ScreenshotStorage.h"
#include "resource.h"
#include <shlobj.h>
#include <uxtheme.h>
#pragma comment(lib,"comctl32.lib")
#pragma comment(lib,"uxtheme.lib")

namespace RegionLens::native
{
    namespace
    {
        constexpr int SharpenId=4500, TopmostId=4501, LanguageId=4502, DirectoryId=4503;
        constexpr int ChooseId=4504, OpenId=4505, DefaultDirectoryId=4506, RestoreKeysId=4507;
        constexpr int AboutTextId=4509, KeyHintId=4510, StatusId=4511, AspectFitId=4512;
        constexpr int AboutIdentityId=4513;
        constexpr int RecordBase=4100, ClearBase=4200, ValueBase=4300, ActionBase=4400;
        constexpr int ContentWidth=330, ContentHeight=218; // Dialog units, not physical pixels.
        constexpr UINT RelayoutMessage=WM_APP+271;
        constexpr UINT FinishCheckMessage=WM_APP+272, HotkeyErrorPageMessage=WM_APP+273;
        thread_local SettingsWindow* creatingSheet{};
        void Front(HWND window)
        {
            if(!IsWindow(window)) return;
            if(IsIconic(window)) ShowWindow(window,SW_RESTORE);
            BringWindowToTop(window); SetForegroundWindow(window);
        }
        bool Modifier(WPARAM key)
        { return key==VK_CONTROL || key==VK_LCONTROL || key==VK_RCONTROL || key==VK_MENU ||
            key==VK_LMENU || key==VK_RMENU || key==VK_SHIFT || key==VK_LSHIFT || key==VK_RSHIFT; }
        std::vector<WORD> PageTemplate(UINT fontPoints)
        {
            // Standard, DWORD-aligned DIALOGEX: native dialog units and font
            // metrics are shared by the sheet, pages and controls.
            std::vector<WORD> data;
            auto word=[&](WORD value){data.push_back(value);};
            auto dword=[&](DWORD value){word(WORD(value));word(WORD(value>>16));};
            word(1); word(0xffff); dword(0); dword(WS_EX_CONTROLPARENT);
            dword(WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS|DS_CONTROL|DS_SHELLFONT);
            word(0); word(0); word(0); word(ContentWidth); word(ContentHeight);
            word(0); word(0); word(0); // Menu, dialog class and title.
            NONCLIENTMETRICSW metrics{sizeof(metrics)};
            bool systemFont=SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,96)!=FALSE;
            word(WORD(fontPoints?fontPoints:systemFont?std::max(8,MulDiv(abs(metrics.lfMessageFont.lfHeight),72,96)):9));
            word(FW_NORMAL); word(WORD(DEFAULT_CHARSET)<<8); // Italic byte + charset byte.
            auto face=systemFont?metrics.lfMessageFont.lfFaceName:L"Segoe UI";
            do { word(WORD(*face)); } while(*face++);
            return data;
        }
    }

    SettingsWindow::SettingsWindow(AppSettings const& current,Probe probe,Apply apply)
        :m_draft(current),m_probe(std::move(probe)),m_apply(std::move(apply)) {}
    SettingsWindow::~SettingsWindow() { Close(); }

    bool SettingsWindow::Create(HWND owner,bool show,UINT fontPoints)
    {
        m_owner=owner; m_finished=false; m_allowShow=show; m_initializing=true;
        INITCOMMONCONTROLSEX common{sizeof(common),ICC_TAB_CLASSES};
        if(!InitCommonControlsEx(&common)) return false;
        m_template=PageTemplate(fontPoints);
        std::array<PROPSHEETPAGEW,3> pages{};
        std::array<wchar_t const*,3> titles{Localized(L"选项",L"Options"),Localized(L"快捷键",L"Shortcuts"),Localized(L"关于",L"About")};
        for(int i=0;i<3;++i) {
            m_pageContexts[i]={this,i};
            auto& page=pages[i]; page.dwSize=sizeof(page);
            page.dwFlags=PSP_DLGINDIRECT|PSP_USETITLE|PSP_PREMATURE;
            page.hInstance=GetModuleHandleW(nullptr);
            page.pResource=reinterpret_cast<LPCDLGTEMPLATE>(m_template.data());
            page.pszTitle=titles[i]; page.pfnDlgProc=PageProc;
            page.lParam=reinterpret_cast<LPARAM>(&m_pageContexts[i]);
        }
        PROPSHEETHEADERW sheet{sizeof(sheet)};
        sheet.dwFlags=PSH_PROPSHEETPAGE|PSH_MODELESS|PSH_NOAPPLYNOW|PSH_NOCONTEXTHELP|PSH_USECALLBACK|PSH_USEHICON;
        sheet.hwndParent=owner; sheet.hInstance=GetModuleHandleW(nullptr);
        sheet.hIcon=LoadIconW(sheet.hInstance,MAKEINTRESOURCEW(IDI_REGIONLENS));
        sheet.pszCaption=Localized(L"设置",L"Settings"); sheet.nPages=UINT(pages.size());
        sheet.ppsp=pages.data(); sheet.pfnCallback=SheetCallback;
        struct CreatingScope { SettingsWindow* old; ~CreatingScope(){creatingSheet=old;} } restore{creatingSheet};
        creatingSheet=this;
        auto result=PropertySheetW(&sheet);
        m_initializing=false;
        if(result<=0 || m_creationFailed || !IsWindow(m_window)) { Close(); return false; }
        m_tabs=PropSheet_GetTabControl(m_window);
        CaptureSheetLayout();
        RefreshText(); RefreshValues(); RefreshAvailability();
        FitToWorkArea(show);
        if(show) { ShowWindow(m_window,SW_SHOW); UpdateWindow(m_window); }
        return true;
    }

    int CALLBACK SettingsWindow::SheetCallback(HWND window,UINT message,LPARAM)
    {
        if(message!=PSCB_INITIALIZED || !creatingSheet) return 0;
        auto self=creatingSheet; self->m_window=window;
        if(!SetWindowSubclass(window,SheetProc,1,reinterpret_cast<DWORD_PTR>(self))) {
            self->m_creationFailed=true; return 0;
        }
        // Settings is an ordinary capturable dialog. Only live lenses and
        // their chrome retain WDA_EXCLUDEFROMCAPTURE.
        if(!self->m_allowShow) {
            SetWindowLongPtrW(window,GWL_EXSTYLE,GetWindowLongPtrW(window,GWL_EXSTYLE)|WS_EX_NOACTIVATE);
            int x=GetSystemMetrics(SM_XVIRTUALSCREEN)-20000, y=GetSystemMetrics(SM_YVIRTUALSCREEN)-20000;
            SetWindowPos(window,nullptr,x,y,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    }

    void SettingsWindow::Run(HWND owner)
    {
        if(!Create(owner,true)) throw std::runtime_error("Settings property sheet creation failed");
        EnableWindow(owner,FALSE);
        struct EnableOwner {HWND window; ~EnableOwner(){if(IsWindow(window)) EnableWindow(window,TRUE);}} restore{owner};
        MSG message{}; int result=1;
        while(!m_finished && (result=GetMessageW(&message,nullptr,0,0))>0) {
            ProcessMessage(message);
        }
        Close();
        if(result==0) PostQuitMessage(int(message.wParam));
    }

    void SettingsWindow::ProcessMessage(MSG& message)
    {
        if(m_finished || !m_window) return;
        if((message.hwnd==m_window || IsChild(m_window,message.hwnd)) &&
            (message.message==WM_KEYDOWN || message.message==WM_SYSKEYDOWN)) {
            if(m_recording) {CaptureKey(message.wParam,message.lParam);return;}
            if(message.wParam==VK_ESCAPE && HandleEscape()) return;
        }
        if(message.hwnd==m_window && message.message==WM_CLOSE) {
            PropSheet_PressButton(m_window,PSBTN_CANCEL);
        } else if(!PropSheet_IsDialogMessage(m_window,&message)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        // A modeless sheet reports completion by clearing its current page.
        // The native command path can bypass our WM_COMMAND/WM_CLOSE subclass.
        // Destroy only after native notification/page-list traversal unwinds.
        FinishNativeClose();
    }
    void SettingsWindow::RequestFinishCheck()
    {
        if(m_window && !m_finishCheckPosted)
            m_finishCheckPosted=PostMessageW(m_window,FinishCheckMessage,0,0)!=FALSE;
    }
    void SettingsWindow::FinishNativeClose()
    {
        if(!m_initializing && !m_notificationDepth && m_window &&
            (m_forceClose || !PropSheet_GetCurrentPageHwnd(m_window))) Close();
    }

    void SettingsWindow::Activate()
    {
        if(!m_window || m_ending || m_finished) return;
        if(m_folderOpening || m_busy) {Front(GetLastActivePopup(m_window));return;}
        RefreshAvailability(); Front(m_window);
    }
    void SettingsWindow::Close()
    {
        if(m_destroying) return;
        if(m_notificationDepth) {
            // Application shutdown may reenter while a page is validating.
            // Keep the native page list alive until its notification returns.
            m_forceClose=true; m_ending=true; RequestFinishCheck(); return;
        }
        m_destroying=true; m_finished=true; m_ending=true;
        if(m_folderDialog) m_folderDialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        if(m_window) DestroyWindow(m_window);
        m_destroying=false;
    }
    bool SettingsWindow::HandleEscape()
    {
        auto quality=GetDlgItem(m_pages[0],SharpenId);
        if(SendMessageW(quality,CB_GETDROPPEDSTATE,0,0)) {
            auto previous=m_qualityBeforeDrop.value_or(m_draft.editing.quality);
            SendMessageW(quality,CB_SHOWDROPDOWN,FALSE,0);
            m_draft.editing.quality=previous;
            SendMessageW(quality,CB_SETCURSEL,WPARAM(previous),0);m_qualityBeforeDrop.reset();return true;
        }
        if(m_recording) {m_recording.reset();RefreshKeyValues();return true;}
        auto combo=GetDlgItem(m_pages[0],LanguageId);
        if(SendMessageW(combo,CB_GETDROPPEDSTATE,0,0)) {
            auto previous=m_languageBeforeDrop.value_or(m_draft.editing.language);
            SendMessageW(combo,CB_SHOWDROPDOWN,FALSE,0);
            m_draft.editing.language=previous; SelectLanguage(previous); m_languageBeforeDrop.reset();
            return true;
        }
        return false;
    }
    LRESULT CALLBACK SettingsWindow::SheetProc(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR,DWORD_PTR data)
    {
        auto self=reinterpret_cast<SettingsWindow*>(data);
        try {
            if(message==WM_WINDOWPOSCHANGING && !self->m_allowShow) {
                auto position=reinterpret_cast<WINDOWPOS*>(lp);
                position->flags=(position->flags&~SWP_SHOWWINDOW)|SWP_NOACTIVATE;
            }
            // Saving/cancellation belongs to PSN_APPLY/PSN_RESET, not this
            // subclass: native sheet commands need not pass through it.
            if(message==WM_CLOSE) {PropSheet_PressButton(window,PSBTN_CANCEL);return 0;}
            if(message==PSM_APPLY) return FALSE; // No independent Apply operation.
            if(message==FinishCheckMessage) {
                self->m_finishCheckPosted=false; self->FinishNativeClose(); return 0;
            }
            if(message==HotkeyErrorPageMessage) {
                if(!self->m_ending && !self->m_finished) self->SelectPage(1);
                return 0;
            }
            if(message==WM_ACTIVATE && LOWORD(wp)!=WA_INACTIVE && !self->m_initializing)
                self->RefreshAvailability();
            if(message==WM_SIZE && !self->m_initializing && !self->m_dpiChanging) {
                auto result=DefSubclassProc(window,message,wp,lp); self->LayoutSheet();return result;
            }
            if(message==WM_DPICHANGED) {
                self->m_dpiChanging=true;
                auto result=DefSubclassProc(window,message,wp,lp);
                self->m_dpiChanging=false;
                PostMessageW(window,RelayoutMessage,0,0); return result;
            }
            if(message==RelayoutMessage) {
                self->CaptureSheetLayout(); self->FitToWorkArea(false);
                for(int i=0;i<3;++i) self->LayoutPage(i);
                return 0;
            }
            if(message==WM_NCDESTROY) {
                RemoveWindowSubclass(window,SheetProc,1);
                self->m_window=nullptr; self->m_tabs=nullptr; self->m_finished=true;
                self->m_pages={}; self->m_status={}; self->m_values={};
                for(auto& item:self->m_items) item.window=nullptr;
            }
        } catch(...) {
            self->m_busy=false; self->m_refreshing=false;
            self->SetStatus(Localized(L"无法完成操作，请重试。",L"Could not complete the operation. Please retry."));
            return 0;
        }
        return DefSubclassProc(window,message,wp,lp);
    }
    INT_PTR CALLBACK SettingsWindow::PageProc(HWND window,UINT message,WPARAM wp,LPARAM lp)
    {
        auto context=reinterpret_cast<PageContext*>(GetWindowLongPtrW(window,DWLP_USER));
        if(message==WM_INITDIALOG) {
            context=reinterpret_cast<PageContext*>(reinterpret_cast<PROPSHEETPAGEW*>(lp)->lParam);
            SetWindowLongPtrW(window,DWLP_USER,reinterpret_cast<LONG_PTR>(context));
        }
        if(!context) return FALSE;
        struct NotificationScope {
            SettingsWindow& owner;bool active;
            NotificationScope(SettingsWindow& value,bool notify):owner(value),active(notify){if(active)++owner.m_notificationDepth;}
            ~NotificationScope(){if(active)--owner.m_notificationDepth;}
        } notification(*context->owner,message==WM_NOTIFY);
        try {return context->owner->PageMessage(window,context->index,message,wp,lp);}
        catch(...) {
            context->owner->m_creationFailed |= message==WM_INITDIALOG;
            context->owner->m_busy=false; context->owner->m_refreshing=false; context->owner->m_folderOpening=false;
            context->owner->SetStatus(Localized(L"无法完成操作，请重试。",L"Could not complete the operation. Please retry."));
            if(message==WM_NOTIFY && reinterpret_cast<NMHDR*>(lp)->code==PSN_APPLY) {
                SetWindowLongPtrW(window,DWLP_MSGRESULT,PSNRET_INVALID_NOCHANGEPAGE); return TRUE;
            }
            return FALSE;
        }
    }
    INT_PTR SettingsWindow::PageMessage(HWND window,int page,UINT message,WPARAM wp,LPARAM lp)
    {
        switch(message) {
        case WM_INITDIALOG:
            m_pages[page]=window; m_window=GetParent(window);
            EnableThemeDialogTexture(window,ETDT_ENABLETAB);
            CreateControls(page); LayoutPage(page); return TRUE;
        case WM_NCDESTROY:
            m_pages[page]=nullptr; m_status[page]=nullptr;
            if(page==1) m_values={};
            for(auto& item:m_items) if(item.page==page) item.window=nullptr;
            SetWindowLongPtrW(window,DWLP_USER,0); return FALSE;
        case WM_COMMAND: Command(LOWORD(wp),HIWORD(wp)); return TRUE;
        case WM_SIZE: case WM_DPICHANGED_AFTERPARENT:
            LayoutPage(page); return FALSE;
        case WM_VSCROLL: case WM_HSCROLL:
            Scroll(page,message==WM_VSCROLL?SB_VERT:SB_HORZ,wp); return TRUE;
        case WM_MOUSEWHEEL:
            Scroll(page,SB_VERT,GET_WHEEL_DELTA_WPARAM(wp)>0?SB_LINEUP:SB_LINEDOWN); return TRUE;
        case WM_CTLCOLORSTATIC: {
            auto control=reinterpret_cast<HWND>(lp);
            auto id=GetDlgCtrlID(control);
            if(id!=StatusId && (id<ValueBase || id>=ValueBase+int(HotkeyActionCount))) return FALSE;
            auto dc=reinterpret_cast<HDC>(wp);
            if(id==StatusId && !GetWindowTextLengthW(control)) return FALSE;
            bool red=id==StatusId || ValueHasConflict(size_t(id-ValueBase));
            SetTextColor(dc,red?RGB(196,43,28):GetSysColor(COLOR_WINDOWTEXT));
            SetBkColor(dc,GetSysColor(COLOR_WINDOW));
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_NOTIFY:
            if(auto hdr=reinterpret_cast<NMHDR*>(lp);hdr && hdr->hwndFrom==m_window) switch(hdr->code) {
            case PSN_SETACTIVE:
                m_page=page; m_recording.reset();
                if(!m_initializing) {if(page==1) RefreshAvailability(); else RefreshKeyValues();}
                SetWindowLongPtrW(window,DWLP_MSGRESULT,0); return TRUE;
            case PSN_KILLACTIVE:
                SetWindowLongPtrW(window,DWLP_MSGRESULT,m_busy || m_folderOpening || m_refreshing || m_finished);return TRUE;
            case PSN_APPLY: {
                auto notice=reinterpret_cast<PSHNOTIFY*>(lp);
                bool accepted=notice->lParam && !m_busy && !m_folderOpening && !m_refreshing && !m_finished && !m_forceClose;
                if(accepted && !m_confirmed) {
                    accepted=ConfirmChanges();
                    if(accepted) {m_confirmed=true;m_ending=true;RequestFinishCheck();}
                }
                // Every page acknowledges, but the shared draft commits only
                // once per accepted OK. A failed attempt remains retryable.
                SetWindowLongPtrW(window,DWLP_MSGRESULT,accepted?PSNRET_NOERROR:PSNRET_INVALID_NOCHANGEPAGE);return TRUE;
            }
            case PSN_QUERYCANCEL:
                SetWindowLongPtrW(window,DWLP_MSGRESULT,m_busy || m_folderOpening || m_refreshing || m_ending);return TRUE;
            case PSN_RESET:
                if(!m_ending) {m_draft.Cancel();m_recording.reset();m_languageBeforeDrop.reset();m_ending=true;}
                RequestFinishCheck();return TRUE;
            }
            break;
        }
        return FALSE;
    }

    HWND SettingsWindow::Control(int page,int id,wchar_t const* kind,DWORD style,
        int x,int y,int width,int height,wchar_t const* chinese,wchar_t const* english)
    {
        auto window=CreateWindowExW(0,kind,Localized(chinese,english),WS_CHILD|WS_VISIBLE|style,
            0,0,1,1,m_pages[page],reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        if(!window) throw std::runtime_error("Settings control creation failed");
        SendMessageW(window,WM_SETFONT,SendMessageW(m_pages[page],WM_GETFONT,0,0),TRUE);
        m_items.push_back({window,page,x,y,width,height,chinese,english});
        return window;
    }
    void SettingsWindow::CreateControls(int page)
    {
        auto window=m_pages[page];
        SetWindowLongPtrW(window,GWL_STYLE,GetWindowLongPtrW(window,GWL_STYLE)|WS_HSCROLL|WS_VSCROLL);
        if(page==0) {
            Control(0,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,10,7,310,12,L"窗口",L"Windows");
            Control(0,0,L"STATIC",SS_ETCHEDHORZ,10,21,310,1);
            Control(0,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,26,111,17,L"画面质量",L"Image quality");
            auto quality=Control(0,SharpenId,L"COMBOBOX",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,129,26,191,64);
            std::array<wchar_t const*,4> levels{Localized(L"无",L"None"),Localized(L"低",L"Low"),
                Localized(L"中",L"Medium"),Localized(L"高",L"High")};
            for(size_t i=0;i<levels.size();++i) {
                auto index=SendMessageW(quality,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(levels[i]));
                if(index==CB_ERR || index==CB_ERRSPACE) throw std::runtime_error("Quality list creation failed");
                SendMessageW(quality,CB_SETITEMDATA,WPARAM(index),LPARAM(i));
            }
            Control(0,TopmostId,L"BUTTON",WS_TABSTOP|BS_AUTOCHECKBOX|BS_VCENTER,12,49,308,17,L"新窗口默认置顶",L"Keep new windows always on top");
            Control(0,AspectFitId,L"BUTTON",WS_TABSTOP|BS_AUTOCHECKBOX|BS_VCENTER,12,72,308,17,
                L"全屏时保持画面比例",L"Preserve aspect ratio in full screen");
            Control(0,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,10,97,310,12,L"语言",L"Language");
            Control(0,0,L"STATIC",SS_ETCHEDHORZ,10,111,310,1);
            Control(0,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,118,111,17,L"界面语言",L"Display language");
            auto combo=Control(0,LanguageId,L"COMBOBOX",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,129,118,191,58);
            for(auto const& item:std::array<std::pair<wchar_t const*,AppLanguage>,2>{
                std::pair{L"简体中文",AppLanguage::SimplifiedChinese},std::pair{L"English",AppLanguage::English}}) {
                auto index=SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item.first));
                if(index==CB_ERR || index==CB_ERRSPACE) throw std::runtime_error("Language list creation failed");
                SendMessageW(combo,CB_SETITEMDATA,WPARAM(index),LPARAM(item.second));
            }
            Control(0,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,10,141,310,12,L"截图保存位置",L"Screenshot folder");
            Control(0,0,L"STATIC",SS_ETCHEDHORZ,10,154,310,1);
            Control(0,DirectoryId,L"STATIC",WS_BORDER|SS_LEFT|SS_CENTERIMAGE|SS_PATHELLIPSIS,12,160,308,16);
            Control(0,ChooseId,L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,12,182,94,18,L"更改…",L"Change…");
            Control(0,OpenId,L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,113,182,99,18,L"打开文件夹",L"Open folder");
            Control(0,DefaultDirectoryId,L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,219,182,101,18,L"恢复默认",L"Restore default");
        } else if(page==1) {
            Control(1,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,7,308,18,L"单击“录入”后按下组合键；必须包含 Ctrl 或 Alt。",L"Select Record, then press a shortcut with Ctrl or Alt.");
            for(size_t i=0;i<HotkeyActionCount;++i) {
                int y=31+int(i)*27;
                Control(1,ActionBase+int(i),L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,y,100,19);
                m_values[i]=Control(1,ValueBase+int(i),L"STATIC",WS_BORDER|SS_CENTER|SS_CENTERIMAGE,118,y,110,19);
                Control(1,RecordBase+int(i),L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,234,y,40,19,L"录入",L"Record");
                Control(1,ClearBase+int(i),L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,280,y,40,19,L"清除",L"Clear");
            }
            Control(1,RestoreKeysId,L"BUTTON",WS_TABSTOP|BS_PUSHBUTTON,12,139,112,18,L"恢复默认快捷键",L"Restore default shortcuts");
            Control(1,KeyHintId,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,161,308,13);
            Control(1,0,L"STATIC",SS_LEFT,12,180,308,22,
                L"Enter：确认选区；Esc／右键：取消选区；Esc：退出全屏。\r\n映射开启时，Ctrl＋鼠标左键拖动：移动区域。",
                L"Enter: confirm selection; Esc/right-click: cancel; Esc: leave full screen.\r\nWhile mapping is on, Ctrl+left-drag moves the region.");
        } else {
            auto icon=Control(2,0,L"STATIC",SS_ICON,12,11,24,24);
            SendMessageW(icon,STM_SETICON,reinterpret_cast<WPARAM>(LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDI_REGIONLENS))),0);
            Control(2,AboutIdentityId,L"STATIC",SS_LEFT,45,10,275,35);
            Control(2,0,L"STATIC",SS_ETCHEDHORZ,10,48,310,1);
            Control(2,0,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,56,308,14,L"主要操作",L"Basic operations");
            constexpr std::array<std::pair<wchar_t const*,wchar_t const*>,5> operations{{
                {L"• 框选屏幕内容，创建实时区域。",L"• Select part of the screen to create a live region."},
                {L"• 拖动画面移动；四角等比缩放，四边单向拉伸。",L"• Drag to move; corners resize proportionally, edges in one direction."},
                {L"• 开启鼠标映射，在区域内操作原窗口。",L"• Enable mouse mapping to control the original window."},
                {L"• 截图复制到剪贴板，并保存为 PNG。",L"• Capture to the clipboard and save a PNG."},
                {L"• 使用右上角按钮调整、全屏、置顶或关闭。",L"• Use the top-right controls for size, full screen, pinning, or closing."}
            }};
            for(size_t i=0;i<operations.size();++i)
                Control(2,i?0:AboutTextId,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,77+int(i)*25,308,21,
                    operations[i].first,operations[i].second);
        }
        m_status[page]=Control(page,StatusId,L"STATIC",SS_LEFT|SS_CENTERIMAGE,12,205,308,12);
    }
    void SettingsWindow::LayoutPage(int index)
    {
        if(m_layoutBusy || m_dpiChanging || m_ending || !m_pages[index]) return;
        m_layoutBusy=true;
        auto page=m_pages[index];
        RECT content{0,0,ContentWidth,ContentHeight}; MapDialogRect(page,&content);
        for(int pass=0;pass<2;++pass) {
            RECT client{}; GetClientRect(page,&client);
            for(int bar:{SB_HORZ,SB_VERT}) {
                SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS};
                info.nMax=(bar==SB_HORZ?content.right:content.bottom)-1;
                info.nPage=UINT(std::max(1L,bar==SB_HORZ?client.right:client.bottom));
                info.nPos=bar==SB_HORZ?m_scroll[index].x:m_scroll[index].y;
                SetScrollInfo(page,bar,&info,TRUE);
                (bar==SB_HORZ?m_scroll[index].x:m_scroll[index].y)=GetScrollPos(page,bar);
            }
        }
        auto font=SendMessageW(page,WM_GETFONT,0,0);
        for(auto const& item:m_items) if(item.page==index) {
            RECT rect{item.x,item.y,item.x+item.width,item.y+item.height}; MapDialogRect(page,&rect);
            SendMessageW(item.window,WM_SETFONT,font,FALSE);
            SetWindowPos(item.window,nullptr,rect.left-m_scroll[index].x,rect.top-m_scroll[index].y,
                rect.right-rect.left,rect.bottom-rect.top,SWP_NOZORDER|SWP_NOACTIVATE);
        }
        m_layoutBusy=false;
    }
    void SettingsWindow::CaptureSheetLayout()
    {
        if(!m_window) return;
        RECT client{}; GetClientRect(m_window,&client); m_frameSize={client.right,client.bottom};
        m_tabs=PropSheet_GetTabControl(m_window);
        std::array<HWND,6> windows{m_tabs,m_pages[0],m_pages[1],m_pages[2],GetDlgItem(m_window,IDOK),GetDlgItem(m_window,IDCANCEL)};
        for(size_t i=0;i<windows.size();++i) {
            GetWindowRect(windows[i],&m_frameRects[i]);
            MapWindowPoints(nullptr,m_window,reinterpret_cast<POINT*>(&m_frameRects[i]),2);
        }
    }
    void SettingsWindow::LayoutSheet()
    {
        if(m_layoutBusy || m_ending || !m_frameSize.cx || !m_window) return;
        m_layoutBusy=true;
        RECT client{}; GetClientRect(m_window,&client);
        int dx=client.right-m_frameSize.cx,dy=client.bottom-m_frameSize.cy;
        std::array<HWND,6> windows{m_tabs,m_pages[0],m_pages[1],m_pages[2],GetDlgItem(m_window,IDOK),GetDlgItem(m_window,IDCANCEL)};
        for(size_t i=0;i<windows.size();++i) {
            auto rect=m_frameRects[i];
            if(i>=4) OffsetRect(&rect,dx,dy);
            else {rect.right+=dx;rect.bottom+=dy;}
            SetWindowPos(windows[i],nullptr,rect.left,rect.top,std::max(1L,rect.right-rect.left),
                std::max(1L,rect.bottom-rect.top),SWP_NOZORDER|SWP_NOACTIVATE);
        }
        m_layoutBusy=false;
        for(int i=0;i<3;++i) LayoutPage(i);
    }
    void SettingsWindow::FitToWorkArea(bool center)
    {
        if(!m_window || !m_allowShow) return;
        MONITORINFO monitor{sizeof(monitor)};
        POINT pointer{}; GetCursorPos(&pointer);
        auto target=center?MonitorFromPoint(pointer,MONITOR_DEFAULTTONEAREST):MonitorFromWindow(m_window,MONITOR_DEFAULTTONEAREST);
        if(!GetMonitorInfoW(target,&monitor)) return;
        RECT bounds{}; GetWindowRect(m_window,&bounds);
        int width=std::min(bounds.right-bounds.left,monitor.rcWork.right-monitor.rcWork.left-16);
        int height=std::min(bounds.bottom-bounds.top,monitor.rcWork.bottom-monitor.rcWork.top-16);
        int x=center?monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2:bounds.left;
        int y=center?monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2:bounds.top;
        SetWindowPos(m_window,nullptr,x,y,width,height,SWP_NOACTIVATE|SWP_NOZORDER);
    }
    void SettingsWindow::Scroll(int page,int bar,WPARAM command)
    {
        SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(m_pages[page],bar,&info);
        int pos=info.nPos,line=MulDiv(24,GetDpiForWindow(m_window),96);
        switch(LOWORD(command)) {
        case SB_LINEUP:pos-=line;break;case SB_LINEDOWN:pos+=line;break;
        case SB_PAGEUP:pos-=info.nPage;break;case SB_PAGEDOWN:pos+=info.nPage;break;
        case SB_THUMBTRACK:case SB_THUMBPOSITION:pos=info.nTrackPos;break;
        case SB_TOP:pos=0;break;case SB_BOTTOM:pos=info.nMax;break;default:return;
        }
        info.fMask=SIF_POS;info.nPos=pos;SetScrollInfo(m_pages[page],bar,&info,TRUE);
        (bar==SB_HORZ?m_scroll[page].x:m_scroll[page].y)=GetScrollPos(m_pages[page],bar);
        LayoutPage(page);
        RedrawWindow(m_pages[page],nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_ALLCHILDREN);
    }
    void SettingsWindow::SelectPage(int index)
    { if(m_window) PropSheet_SetCurSel(m_window,nullptr,std::clamp(index,0,2)); }
    void SettingsWindow::RefreshText()
    {
        if(!m_window || m_ending) return;
        SetWindowTextW(m_window,Localized(L"设置",L"Settings"));
        SetDlgItemTextW(m_window,IDOK,Localized(L"确定",L"OK"));
        SetDlgItemTextW(m_window,IDCANCEL,Localized(L"取消",L"Cancel"));
        for(auto const& item:m_items) if(item.chinese && *item.chinese)
            SetWindowTextW(item.window,Localized(item.chinese,item.english));
        for(size_t i=0;i<HotkeyActionCount;++i)
            SetDlgItemTextW(m_pages[1],ActionBase+int(i),HotkeyActionName(HotkeyAction(i)));
        SetDlgItemTextW(m_pages[2],AboutIdentityId,BuildIdentityText().c_str());
    }
    void SettingsWindow::SelectLanguage(AppLanguage language)
    {
        auto combo=GetDlgItem(m_pages[0],LanguageId);
        auto count=SendMessageW(combo,CB_GETCOUNT,0,0);
        for(LRESULT i=0;i<count;++i) if(SendMessageW(combo,CB_GETITEMDATA,WPARAM(i),0)==LRESULT(language)) {
            SendMessageW(combo,CB_SETCURSEL,WPARAM(i),0);return;
        }
    }
    void SettingsWindow::ReadLanguage()
    {
        auto combo=GetDlgItem(m_pages[0],LanguageId);
        auto selection=SendMessageW(combo,CB_GETCURSEL,0,0);
        if(selection==CB_ERR) return;
        auto language=SendMessageW(combo,CB_GETITEMDATA,WPARAM(selection),0);
        if(language==LRESULT(AppLanguage::SimplifiedChinese) || language==LRESULT(AppLanguage::English))
            m_draft.editing.language=AppLanguage(language);
    }
    void SettingsWindow::ReadQuality()
    {
        auto combo=GetDlgItem(m_pages[0],SharpenId);
        auto selection=SendMessageW(combo,CB_GETCURSEL,0,0);
        if(selection==CB_ERR) return;
        auto level=SendMessageW(combo,CB_GETITEMDATA,WPARAM(selection),0);
        if(level>=0 && level<=LRESULT(LensSharpness::High)) m_draft.editing.quality=LensSharpness(level);
    }
    void SettingsWindow::RefreshValues()
    {
        SendDlgItemMessageW(m_pages[0],SharpenId,CB_SETCURSEL,WPARAM(m_draft.editing.quality),0);
        CheckDlgButton(m_pages[0],TopmostId,m_draft.editing.newWindowTopmost?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(m_pages[0],AspectFitId,m_draft.editing.fullscreenAspectFit?BST_CHECKED:BST_UNCHECKED);
        SelectLanguage(m_draft.editing.language);
        SetDlgItemTextW(m_pages[0],DirectoryId,ResolveScreenshotDirectory(m_draft.editing,*Runtime().identity).c_str());
        RefreshKeyValues();
    }
    void SettingsWindow::RefreshKeyValues()
    {
        if(m_ending || m_finished) return;
        for(size_t i=0;i<HotkeyActionCount;++i) if(m_values[i]) {
            auto text=m_recording && *m_recording==i?std::wstring(Localized(L"请按下组合键…",L"Press shortcut…")):FormatHotkey(m_draft.editing.hotkeys.bindings[i]);
            if(ValueHasConflict(i)) text+=Localized(L" · 已占用",L" · In use");
            SetWindowTextW(m_values[i],text.c_str());InvalidateRect(m_values[i],nullptr,TRUE);
        }
        SetDlgItemTextW(m_pages[1],KeyHintId,m_availability.AllSucceeded()?L"":Localized(L"红色组合键已被占用。",L"Red shortcuts are in use."));
    }
    bool SettingsWindow::ValueHasConflict(size_t index) const
    {
        if(index>=HotkeyActionCount) return false;
        auto const& item=m_availability.items[index];
        return (!m_recording || *m_recording!=index) && m_checked.bindings[index]==m_draft.editing.hotkeys.bindings[index] &&
            item.requested && !item.succeeded;
    }
    void SettingsWindow::RefreshAvailability()
    {
        if(!m_window || m_initializing || m_busy || m_refreshing || m_folderOpening || m_ending || m_finished || !m_probe) return;
        m_refreshing=true;
        m_checked=m_draft.editing.hotkeys;m_availability=m_probe(m_checked);
        for(size_t i=0;i<HotkeyActionCount;++i) for(size_t j=i+1;j<HotkeyActionCount;++j)
            if(m_checked.bindings[i].Enabled() && m_checked.bindings[i]==m_checked.bindings[j]) {
                m_availability.items[i]={true,false,ERROR_HOTKEY_ALREADY_REGISTERED};
                m_availability.items[j]={true,false,ERROR_HOTKEY_ALREADY_REGISTERED};
            }
        m_refreshing=false;RefreshKeyValues();
    }
    void SettingsWindow::SetStatus(std::wstring const& text)
    { for(auto window:m_status) if(IsWindow(window)) SetWindowTextW(window,text.c_str()); }

    void SettingsWindow::Command(int id,int notification)
    {
        if(m_busy || m_folderOpening || m_refreshing || m_initializing || m_ending || m_finished) return;
        if(id>=RecordBase && id<RecordBase+int(HotkeyActionCount)) {
            m_recording=size_t(id-RecordBase);RefreshKeyValues();SetFocus(m_pages[1]);return;
        }
        if(id>=ClearBase && id<ClearBase+int(HotkeyActionCount)) {
            m_recording.reset();m_draft.editing.hotkeys.bindings[id-ClearBase]={};SetStatus(L"");RefreshAvailability();return;
        }
        switch(id) {
        case SharpenId:
            if(notification==CBN_DROPDOWN) m_qualityBeforeDrop=m_draft.editing.quality;
            else if(notification==CBN_SELCHANGE) ReadQuality();
            else if(notification==CBN_SELENDOK) {ReadQuality();m_qualityBeforeDrop.reset();}
            else if(notification==CBN_SELENDCANCEL && m_qualityBeforeDrop) {
                m_draft.editing.quality=*m_qualityBeforeDrop;
                SendDlgItemMessageW(m_pages[0],SharpenId,CB_SETCURSEL,WPARAM(*m_qualityBeforeDrop),0);
                m_qualityBeforeDrop.reset();
            }
            break;
        case TopmostId:m_draft.editing.newWindowTopmost=IsDlgButtonChecked(m_pages[0],id)==BST_CHECKED;break;
        case AspectFitId:m_draft.editing.fullscreenAspectFit=IsDlgButtonChecked(m_pages[0],id)==BST_CHECKED;break;
        case LanguageId:
            if(notification==CBN_DROPDOWN) m_languageBeforeDrop=m_draft.editing.language;
            else if(notification==CBN_SELCHANGE) ReadLanguage();
            else if(notification==CBN_SELENDOK) {
                // The combo has committed this selection.  Moving focus to
                // OK can produce a later CBN_SELENDCANCEL on some Windows
                // builds; it must not restore the value from before opening.
                ReadLanguage();m_languageBeforeDrop.reset();
            }
            else if(notification==CBN_SELENDCANCEL && m_languageBeforeDrop) {
                m_draft.editing.language=*m_languageBeforeDrop;SelectLanguage(*m_languageBeforeDrop);
                m_languageBeforeDrop.reset();
            }
            // No general RefreshValues here: focus/close notifications must
            // never overwrite the control's pending selection.
            break;
        case ChooseId:ChooseFolder();
            if(m_window) SetDlgItemTextW(m_pages[0],DirectoryId,ResolveScreenshotDirectory(m_draft.editing,*Runtime().identity).c_str());
            break;
        case OpenId:OpenFolder();break;
        case DefaultDirectoryId:m_draft.editing.screenshotDirectory.clear();
            SetDlgItemTextW(m_pages[0],DirectoryId,ResolveScreenshotDirectory(m_draft.editing,*Runtime().identity).c_str());break;
        case RestoreKeysId:m_recording.reset();m_draft.editing.hotkeys=DefaultHotkeySettings();RefreshAvailability();break;
        }
    }
    void SettingsWindow::CaptureKey(WPARAM key,LPARAM flags)
    {
        if(!m_recording || (flags&(LPARAM(1)<<30))) return;
        if(key==VK_ESCAPE) {m_recording.reset();RefreshKeyValues();return;}
        if(Modifier(key)) return;
        UINT modifiers{};
        if(GetKeyState(VK_CONTROL)&0x8000) modifiers|=MOD_CONTROL;
        if(GetKeyState(VK_MENU)&0x8000) modifiers|=MOD_ALT;
        if(GetKeyState(VK_SHIFT)&0x8000) modifiers|=MOD_SHIFT;
        HotkeyBinding binding{modifiers,UINT(key)};
        if(!IsSupportedHotkey(binding)) {SetStatus(Localized(L"请使用 Ctrl 或 Alt 加字母、数字或 F1–F12。",L"Use Ctrl or Alt with a letter, digit, or F1–F12."));return;}
        m_draft.editing.hotkeys.bindings[*m_recording]=binding;m_recording.reset();SetStatus(L"");RefreshAvailability();
    }
    bool SettingsWindow::ConfirmChanges()
    {
        m_recording.reset();
        ReadQuality();m_qualityBeforeDrop.reset();
        ReadLanguage();
        // Confirmation completes any language combo transaction.  A delayed
        // focus-loss notification cannot roll an accepted choice back.
        m_languageBeforeDrop.reset();
        if(!m_draft.Dirty()) return true;
        auto validation=ValidateHotkeySettings(m_draft.editing.hotkeys);
        if(!validation.Valid()) {
            PostMessageW(m_window,HotkeyErrorPageMessage,0,0);RefreshAvailability();
            SetStatus(validation.error==HotkeyValidationError::DuplicateBinding?
                Localized(L"快捷键重复，请修改红色组合。",L"Duplicate shortcut. Change the red bindings."):
                Localized(L"快捷键无效，请重新录入。",L"Invalid shortcut. Please record it again."));
            return false;
        }
        m_busy=true;
        auto result=m_apply(m_draft.editing);
        m_busy=false;
        if(!m_window || m_forceClose) return false;
        if(result.Applied()) {m_draft.Accept();return true;}
        if(result.issue==SettingsApplyIssue::Hotkey) PostMessageW(m_window,HotkeyErrorPageMessage,0,0);
        m_checked=m_draft.editing.hotkeys;m_availability=result.availability;
        SetStatus((result.issue==SettingsApplyIssue::Hotkey?
            std::wstring(Localized(L"快捷键不可用，修改未保存。错误：",L"Shortcut unavailable. Nothing saved. Error: ")):
            std::wstring(Localized(L"保存失败，原设置不变。错误：",L"Save failed. Previous settings retained. Error: ")))+
            std::to_wstring(uint32_t(result.error)));
        RefreshKeyValues();return false;
    }

    void SettingsWindow::ChooseFolder()
    {
        m_folderOpening=true;
        Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
        auto hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog));
        if(SUCCEEDED(hr)) {
            m_folderDialog=dialog;
            hr=dialog->SetOptions(FOS_PICKFOLDERS|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_NOCHANGEDIR);
        }
        if(SUCCEEDED(hr) && m_window) {
            dialog->SetTitle(Localized(L"截图保存位置",L"Screenshot folder"));
            auto path=ResolveScreenshotDirectory(m_draft.editing,*Runtime().identity);
            Microsoft::WRL::ComPtr<IShellItem> initial;
            if(SUCCEEDED(SHCreateItemFromParsingName(path.c_str(),nullptr,IID_PPV_ARGS(&initial)))) dialog->SetFolder(initial.Get());
            hr=dialog->Show(m_window);
            if(SUCCEEDED(hr) && m_window) {
                Microsoft::WRL::ComPtr<IShellItem> item; PWSTR selected{};
                hr=dialog->GetResult(&item);
                if(SUCCEEDED(hr)) hr=item->GetDisplayName(SIGDN_FILESYSPATH,&selected);
                if(SUCCEEDED(hr)) {
                    try { m_draft.editing.screenshotDirectory=selected; }
                    catch(...) { CoTaskMemFree(selected); throw; }
                    CoTaskMemFree(selected);
                }
            }
        }
        m_folderDialog.Reset(); m_folderOpening=false;
        if(FAILED(hr) && hr!=HRESULT_FROM_WIN32(ERROR_CANCELLED)) SetStatus(Localized(L"无法选择文件夹，原设置未改变。",L"Could not select a folder. Settings are unchanged."));
    }

    void SettingsWindow::OpenFolder()
    {
        auto directory=ResolveScreenshotDirectory(m_draft.editing,*Runtime().identity);
        auto hr=EnsureScreenshotDirectory(directory);
        if(SUCCEEDED(hr)) {
            auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(m_window,L"open",directory.c_str(),nullptr,nullptr,SW_SHOWNORMAL));
            if(result>32) { SetStatus(L""); return; }
        }
        SetStatus(Localized(L"无法打开文件夹，请检查保存位置及权限。",L"Could not open the folder. Check its location and permissions."));
    }
}
