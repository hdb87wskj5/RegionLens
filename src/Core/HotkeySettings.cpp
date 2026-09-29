#include "pch.h"
#include "HotkeySettings.h"
#include "Localization.h"

namespace RegionLens::native
{
    namespace
    {
        wchar_t const* RegistryPath() noexcept { return Runtime().identity->hotkeys; }
        constexpr wchar_t RegistryValue[] = L"Bindings";
        constexpr DWORD StoredMagic = 0x314B4852; // RHK1
        constexpr DWORD StoredVersion = 3;

        bool IsPrimaryKey(UINT key) noexcept
        {
            return (key >= 'A' && key <= 'Z') ||
                (key >= '0' && key <= '9') ||
                (key >= VK_F1 && key <= VK_F12);
        }
    }

    bool HotkeyRegistrationResult::AllSucceeded() const noexcept
    {
        for (auto const& item : items)
        {
            if (item.requested && !item.succeeded) return false;
        }
        return true;
    }

    bool HotkeyRegistrationResult::HasConflict() const noexcept
    {
        for (auto const& item : items)
        {
            if (item.requested && !item.succeeded && item.error == ERROR_HOTKEY_ALREADY_REGISTERED)
                return true;
        }
        return false;
    }

    HotkeySettings DefaultHotkeySettings() noexcept
    {
        HotkeySettings settings;
        settings[HotkeyAction::NewRegion] = { MOD_CONTROL | MOD_ALT, 'M' };
        settings[HotkeyAction::CloseAll] = { MOD_CONTROL | MOD_ALT, 'X' };
        settings[HotkeyAction::HideAll] = { MOD_CONTROL | MOD_ALT, 'H' };
        settings[HotkeyAction::ShowAllTopmost] = { MOD_CONTROL | MOD_ALT, 'S' };
        return settings;
    }

    bool IsSupportedHotkey(HotkeyBinding binding) noexcept
    {
        if (!binding.Enabled()) return binding.modifiers == 0 && binding.virtualKey == 0;
        constexpr UINT allowed = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
        if ((binding.modifiers & ~allowed) != 0 || (binding.modifiers & (MOD_CONTROL | MOD_ALT)) == 0)
            return false;
        return IsPrimaryKey(binding.virtualKey);
    }

    HotkeyValidationResult ValidateHotkeySettings(HotkeySettings const& settings) noexcept
    {
        for (size_t i = 0; i < HotkeyActionCount; ++i)
        {
            auto action = static_cast<HotkeyAction>(i);
            auto const binding = settings.bindings[i];
            if (!IsSupportedHotkey(binding))
                return { HotkeyValidationError::InvalidBinding, action, action };
            if (!binding.Enabled()) continue;
            for (size_t j = 0; j < i; ++j)
            {
                if (settings.bindings[j] == binding)
                    return { HotkeyValidationError::DuplicateBinding, action, static_cast<HotkeyAction>(j) };
            }
        }
        return {};
    }

    DWORD PackHotkeyBinding(HotkeyBinding binding) noexcept
    {
        if (!binding.Enabled()) return 0;
        return (DWORD(binding.virtualKey & 0xffff) << 16) | DWORD(binding.modifiers & 0xffff);
    }

    HotkeyBinding UnpackHotkeyBinding(DWORD value) noexcept
    {
        if (!value) return {};
        return { UINT(value & 0xffff), UINT(value >> 16) };
    }

    HotkeyStorageRecord EncodeHotkeySettings(HotkeySettings const& settings) noexcept
    {
        HotkeyStorageRecord record{};
        record[0] = StoredMagic;
        record[1] = StoredVersion;
        for (size_t i = 0; i < HotkeyActionCount; ++i)
            record[i + 2] = PackHotkeyBinding(settings.bindings[i]);
        return record;
    }

    bool DecodeHotkeySettings(std::span<DWORD const> record, HotkeySettings& settings) noexcept
    {
        if (record.size() < 2 || record[0] != StoredMagic) return false;
        bool oldThreeActions = record[1] == 1 && record.size() == 5;
        bool oldFiveActions = record[1] == 2 && record.size() == 7;
        bool current = record[1] == StoredVersion && record.size() == HotkeyActionCount + 2;
        if (!oldThreeActions && !oldFiveActions && !current) return false;
        auto decoded = DefaultHotkeySettings();
        auto count = record.size() - 2;
        for (size_t i = 0; i < count; ++i)
        {
            if (i == 2 && !current) continue; // Removed cancel-mapping action.
            auto destination = !current && i > 2 ? i - 1 : i;
            decoded.bindings[destination] = UnpackHotkeyBinding(record[i + 2]);
        }
        // Never replace an existing custom shortcut with a new default. Leave
        // the added action unassigned if its default is already used locally.
        for (size_t i = oldThreeActions ? 2 : HotkeyActionCount; i < HotkeyActionCount; ++i)
            for (size_t j = 0; j < i; ++j)
                if (decoded.bindings[i] == decoded.bindings[j]) decoded.bindings[i] = {};
        if (!ValidateHotkeySettings(decoded).Valid()) return false;
        settings = decoded;
        return true;
    }

    wchar_t const* HotkeyActionName(HotkeyAction action) noexcept
    {
        switch (action)
        {
        case HotkeyAction::NewRegion: return Localized(L"新建区域", L"New region");
        case HotkeyAction::CloseAll: return Localized(L"关闭全部", L"Close all");
        case HotkeyAction::HideAll: return Localized(L"隐藏全部区域", L"Hide all regions");
        case HotkeyAction::ShowAllTopmost: return Localized(L"显示全部并前置", L"Show all and raise");
        default: return Localized(L"未知功能", L"Unknown action");
        }
    }

    std::wstring FormatHotkey(HotkeyBinding binding)
    {
        if (!binding.Enabled()) return Localized(L"未设置", L"Not set");
        std::wstring text;
        auto append = [&text](std::wstring const& value)
        {
            if (!text.empty()) text += L"+";
            text += value;
        };
        if (binding.modifiers & MOD_CONTROL) append(L"Ctrl");
        if (binding.modifiers & MOD_ALT) append(L"Alt");
        if (binding.modifiers & MOD_SHIFT) append(L"Shift");
        if ((binding.virtualKey >= 'A' && binding.virtualKey <= 'Z') ||
            (binding.virtualKey >= '0' && binding.virtualKey <= '9'))
            append(std::wstring(1, wchar_t(binding.virtualKey)));
        else if (binding.virtualKey >= VK_F1 && binding.virtualKey <= VK_F12)
            append(L"F" + std::to_wstring(binding.virtualKey - VK_F1 + 1));
        return text;
    }



    HotkeySettings LoadHotkeySettings() noexcept
    {
        // The previous format contained five actions, including the removed
        // cancel-mapping slot. Read the larger record before migrating it.
        std::array<DWORD, 7> stored{};
        DWORD type{}, bytes = sizeof(stored);
        if (RegGetValueW(HKEY_CURRENT_USER, RegistryPath(), RegistryValue, RRF_RT_REG_BINARY,
            &type, &stored, &bytes) != ERROR_SUCCESS || bytes % sizeof(DWORD) != 0)
            return DefaultHotkeySettings();
        HotkeySettings settings;
        return DecodeHotkeySettings({ stored.data(), bytes / sizeof(DWORD) }, settings) ? settings : DefaultHotkeySettings();
    }

    bool SaveHotkeySettings(HotkeySettings const& settings) noexcept
    {
        if (!ValidateHotkeySettings(settings).Valid()) return false;
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, RegistryPath(), 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return false;
        auto stored = EncodeHotkeySettings(settings);
        auto result = RegSetValueExW(key, RegistryValue, 0, REG_BINARY,
            reinterpret_cast<BYTE const*>(&stored), sizeof(stored));
        RegCloseKey(key);
        return result == ERROR_SUCCESS;
    }
}
