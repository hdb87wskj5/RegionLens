#pragma once

#include "HotkeySettings.h"
#include "Localization.h"
#include "LensQuality.h"
#include <functional>
#include <vector>

namespace RegionLens::native
{
    struct AppSettings
    {
        AppLanguage language{ AppLanguage::SimplifiedChinese };
        HotkeySettings hotkeys{ DefaultHotkeySettings() };
        std::wstring screenshotDirectory; // Empty means the channel's Pictures default.
        LensSharpness quality{ LensSharpness::Medium };
        bool newWindowTopmost{ true };
        bool fullscreenAspectFit{ false };
        bool operator==(AppSettings const&) const = default;
    };

    bool ValidAppSettings(AppSettings const& settings) noexcept;
    std::vector<BYTE> EncodeAppSettings(AppSettings const& settings);
    bool DecodeAppSettings(std::span<BYTE const> bytes, AppSettings& settings);
    AppSettings LoadAppSettings(AppIdentity const& identity) noexcept;
    HRESULT SaveAppSettings(AppIdentity const& identity, AppSettings const& settings) noexcept;
    std::wstring ResolveScreenshotDirectory(AppSettings const& settings, AppIdentity const& identity);
    std::wstring BuildAboutText();

    enum class SettingsApplyIssue { None, Invalid, Hotkey, Storage, Cancelled };
    struct SettingsApplyResult
    {
        SettingsApplyIssue issue{};
        HRESULT error{ S_OK };
        HotkeyRegistrationResult availability;
        bool Applied() const noexcept { return issue == SettingsApplyIssue::None; }
    };

    // Working shortcuts stay registered while the modeless sheet is open.
    // Retain the candidate after a successful commit; restore the previous
    // live registrations on validation/storage failure or exceptions.
    template<typename Acquire, typename Release, typename Save>
    SettingsApplyResult CommitAppSettings(AppSettings const& previous, AppSettings const& candidate,
        Acquire&& acquire, Release&& release, Save&& save)
    {
        SettingsApplyResult result;
        if (!ValidAppSettings(candidate)) { result.issue = SettingsApplyIssue::Invalid; result.error = E_INVALIDARG; return result; }
        bool committed{};
        struct Rollback {
            Acquire& acquire; Release& release; HotkeySettings const& previous; bool& committed;
            ~Rollback() { if (!committed) { release(); acquire(previous); } }
        } rollback{ acquire, release, previous.hotkeys, committed };
        release();
        result.availability = acquire(candidate.hotkeys);
        for (size_t i = 0; i < HotkeyActionCount; ++i) {
            auto const& item = result.availability.items[i];
            if (item.requested && !item.succeeded &&
                (candidate.hotkeys.bindings[i] != previous.hotkeys.bindings[i] || item.error != ERROR_HOTKEY_ALREADY_REGISTERED)) {
                result.issue = SettingsApplyIssue::Hotkey;
                result.error = HRESULT_FROM_WIN32(item.error ? item.error : ERROR_GEN_FAILURE);
                return result;
            }
        }
        result.error = save(candidate);
        if (FAILED(result.error)) result.issue = SettingsApplyIssue::Storage;
        else committed = true;
        return result;
    }

    struct SettingsDraft
    {
        AppSettings applied, editing;
        explicit SettingsDraft(AppSettings const& initial) : applied(initial), editing(initial) {}
        bool Dirty() const { return editing != applied; }
        void Accept() { applied = editing; }
        void Cancel() { editing = applied; }
    };
}
