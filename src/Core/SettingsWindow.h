#pragma once
#include "AppSettings.h"
#include <commctrl.h>
#include <prsht.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <optional>

namespace RegionLens::native
{
    class SettingsWindow
    {
    public:
        using Probe = std::function<HotkeyRegistrationResult(HotkeySettings const&)>;
        using Apply = std::function<SettingsApplyResult(AppSettings const&)>;
        using RecordingChanged = std::function<void(bool)>;
        SettingsWindow(AppSettings const& current, Probe probe, Apply apply, RecordingChanged recordingChanged = {});
        ~SettingsWindow();
        bool Show(HWND owner, bool visible = true);
        bool ProcessMessage(MSG& message);
        bool Finished() const noexcept { return m_finished; }
        void Activate();
        void Close();
    private:
        friend struct SettingsWindowTestAccess;
        struct PageContext { SettingsWindow* owner{}; int index{}; };
        struct Item { HWND window{}; int page{}, x{}, y{}, width{}, height{}; wchar_t const* chinese{}; wchar_t const* english{}; };
        static int CALLBACK SheetCallback(HWND, UINT, LPARAM);
        static LRESULT CALLBACK SheetProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
        static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);
        INT_PTR PageMessage(HWND, int, UINT, WPARAM, LPARAM);
        bool Create(HWND owner, bool show, UINT fontPoints=0);
        void SetRecording(std::optional<size_t> recording);
        void FinishNativeClose();
        void RequestFinishCheck();
        void CreateControls(int page);
        HWND Control(int page, int id, wchar_t const* kind, DWORD style,
            int x, int y, int width, int height, wchar_t const* chinese=L"", wchar_t const* english=L"");
        void RefreshText();
        void RefreshValues();
        void RefreshKeyValues();
        void RefreshAvailability();
        void SelectPage(int index);
        void LayoutPage(int index);
        void CaptureSheetLayout();
        void LayoutSheet();
        void FitToWorkArea(bool center);
        void Scroll(int page,int bar,WPARAM command);
        void Command(int id,int notification);
        void CaptureKey(WPARAM key,LPARAM flags);
        bool HandleEscape();
        bool ConfirmChanges();
        void ChooseFolder();
        void OpenFolder();
        void SetStatus(std::wstring const&);
        bool ValueHasConflict(size_t index) const;
        void SelectLanguage(AppLanguage language);
        void ReadLanguage();
        void ReadQuality();
        SettingsDraft m_draft;
        HotkeySettings m_checked;
        HotkeyRegistrationResult m_availability;
        Probe m_probe;
        Apply m_apply;
        RecordingChanged m_recordingChanged;
        HWND m_owner{}, m_window{}, m_tabs{};
        std::array<HWND,3> m_pages{}, m_status{};
        std::array<PageContext,3> m_pageContexts{};
        std::array<POINT,3> m_scroll{};
        std::vector<Item> m_items;
        std::vector<WORD> m_template;
        std::array<HWND,HotkeyActionCount> m_values{};
        std::array<RECT,6> m_frameRects{};
        SIZE m_frameSize{};
        bool m_finished{}, m_busy{}, m_refreshing{}, m_folderOpening{}, m_initializing{}, m_layoutBusy{}, m_dpiChanging{};
        bool m_allowShow{}, m_confirmed{}, m_creationFailed{}, m_ending{}, m_destroying{}, m_finishCheckPosted{}, m_forceClose{};
        unsigned m_notificationDepth{};
        std::optional<size_t> m_recording;
        std::optional<AppLanguage> m_languageBeforeDrop;
        std::optional<LensSharpness> m_qualityBeforeDrop;
        int m_page{};
        Microsoft::WRL::ComPtr<IFileOpenDialog> m_folderDialog;
    };
}
