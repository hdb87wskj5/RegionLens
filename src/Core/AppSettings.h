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

    // All registrations here are temporary while the input engine is stopped.
    // Reserve through storage commit, then release even on error. The owner
    // restores the latest applied configuration when the settings window exits.
    template<typename Acquire, typename Release, typename Save>
    SettingsApplyResult CommitAppSettings(AppSettings const& previous, AppSettings const& candidate,
        Acquire&& acquire, Release&& release, Save&& save)
    {
        SettingsApplyResult result;
        if (!ValidAppSettings(candidate)) { result.issue = SettingsApplyIssue::Invalid; result.error = E_INVALIDARG; return result; }
        struct Cleanup { Release& callback; ~Cleanup() { callback(); } } cleanup{ release };
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
