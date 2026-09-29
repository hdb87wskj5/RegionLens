#include "HotkeySettings.h"
#include "AppSettings.h"
#include "Localization.h"
#include <iostream>
#include <string_view>

using namespace RegionLens::native;

namespace
{
    int failures{};

    void Check(bool condition, std::string_view name)
    {
        if (!condition)
        {
            std::cerr << "FAILED: " << name << '\n';
            ++failures;
        }
    }

    HotkeyRegistrationResult Available(HotkeySettings const& settings)
    {
        HotkeyRegistrationResult result;
        for (size_t i = 0; i < HotkeyActionCount; ++i)
            result.items[i].requested = settings.bindings[i].Enabled();
        return result;
    }

    struct FakeRegistrar
    {
        UINT conflictKey{};
        UINT rollbackFailureKey{};
        int releases{};
        int acquisitions{};

        void Release() { ++releases; }
        HotkeyRegistrationResult Acquire(HotkeySettings const& settings)
        {
            ++acquisitions;
            auto result = Available(settings);
            for (size_t i = 0; i < HotkeyActionCount; ++i)
            {
                auto key = settings.bindings[i].virtualKey;
                if (key && key == conflictKey)
                    result.items[i] = { true, false, ERROR_HOTKEY_ALREADY_REGISTERED };
                if (key && acquisitions > 1 && key == rollbackFailureKey)
                    result.items[i] = { true, false, ERROR_ACCESS_DENIED };
            }
            return result;
        }
    };
}

int RunHotkeyTests()
{
    auto defaults = DefaultHotkeySettings();

    Check(HotkeyActionCount == 4 &&
        defaults[HotkeyAction::HideAll] == HotkeyBinding{ MOD_CONTROL | MOD_ALT, 'H' } &&
        defaults[HotkeyAction::ShowAllTopmost] == HotkeyBinding{ MOD_CONTROL | MOD_ALT, 'S' },
        "Hide and show/topmost are separate configurable actions with distinct defaults");
    Check(!IsSupportedHotkey({ MOD_SHIFT, 'A' }) &&
        !IsSupportedHotkey({ MOD_WIN | MOD_CONTROL, 'A' }) &&
        !IsSupportedHotkey({ MOD_CONTROL, VK_DELETE }) &&
        !IsSupportedHotkey({ MOD_CONTROL, 0 }),
        "Shift-only, Win, broad keys and incomplete bindings are rejected");

    auto packed = PackHotkeyBinding({ MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F9 });
    Check(UnpackHotkeyBinding(packed) == HotkeyBinding{ MOD_CONTROL | MOD_ALT | MOD_SHIFT, VK_F9 } &&
        PackHotkeyBinding({}) == 0 && UnpackHotkeyBinding(0) == HotkeyBinding{},
        "Shortcut storage packing round-trips including disabled bindings");

    auto record = EncodeHotkeySettings(defaults);
    HotkeySettings decoded;
    Check(DecodeHotkeySettings(record, decoded) && decoded == defaults,
        "Versioned shortcut persistence round-trips");
    std::array<DWORD, 5> legacy{ record[0], 1,
        PackHotkeyBinding({ MOD_CONTROL | MOD_SHIFT, 'Q' }), 0,
        PackHotkeyBinding({ MOD_CONTROL | MOD_ALT, 'I' }) };
    Check(DecodeHotkeySettings(legacy, decoded) &&
        decoded[HotkeyAction::NewRegion] == HotkeyBinding{ MOD_CONTROL | MOD_SHIFT, 'Q' } &&
        !decoded[HotkeyAction::CloseAll].Enabled() &&
        decoded[HotkeyAction::HideAll] == defaults[HotkeyAction::HideAll] &&
        decoded[HotkeyAction::ShowAllTopmost] == defaults[HotkeyAction::ShowAllTopmost],
        "Version 1 drops the former cancel action while retaining old and new defaults");
    legacy[2] = PackHotkeyBinding(defaults[HotkeyAction::HideAll]);
    legacy[3] = PackHotkeyBinding(defaults[HotkeyAction::ShowAllTopmost]);
    Check(DecodeHotkeySettings(legacy, decoded) && ValidateHotkeySettings(decoded).Valid() &&
        decoded[HotkeyAction::NewRegion] == defaults[HotkeyAction::HideAll] &&
        !decoded[HotkeyAction::HideAll].Enabled() && !decoded[HotkeyAction::ShowAllTopmost].Enabled(),
        "Migration disables new default collisions without overwriting an old binding");
    Check(!DecodeHotkeySettings({}, decoded) && !DecodeHotkeySettings({record.data(), 1}, decoded) &&
        !DecodeHotkeySettings({record.data(), record.size()-1}, decoded),
        "Empty and truncated current records are rejected");
    auto migrated = EncodeHotkeySettings(decoded);
    Check(DecodeHotkeySettings(migrated, decoded) && !decoded[HotkeyAction::ShowAllTopmost].Enabled(),
        "Migrated disabled actions survive a current save and reload");
    std::array<DWORD, 7> previousFive{ record[0], 2,
        PackHotkeyBinding({ MOD_CONTROL | MOD_SHIFT, 'Q' }),
        PackHotkeyBinding({ MOD_CONTROL | MOD_ALT, 'X' }),
        PackHotkeyBinding({ MOD_CONTROL | MOD_ALT, 'I' }),
        PackHotkeyBinding({ MOD_CONTROL | MOD_SHIFT, 'H' }),
        PackHotkeyBinding({ MOD_CONTROL | MOD_SHIFT, 'S' }) };
    Check(DecodeHotkeySettings(previousFive, decoded) &&
        decoded[HotkeyAction::NewRegion] == HotkeyBinding{ MOD_CONTROL | MOD_SHIFT, 'Q' } &&
        decoded[HotkeyAction::CloseAll] == HotkeyBinding{ MOD_CONTROL | MOD_ALT, 'X' } &&
        decoded[HotkeyAction::HideAll] == HotkeyBinding{ MOD_CONTROL | MOD_SHIFT, 'H' } &&
        decoded[HotkeyAction::ShowAllTopmost] == HotkeyBinding{ MOD_CONTROL | MOD_SHIFT, 'S' },
        "Version 2 drops cancel action without shifting custom hide/show shortcuts");
    auto corruptVersion = record;
    corruptVersion[1] ^= 1;
    Check(!DecodeHotkeySettings(corruptVersion, decoded),
        "Unknown shortcut persistence version is rejected");
    auto corruptBinding = record;
    corruptBinding[2] = PackHotkeyBinding({ MOD_SHIFT, 'A' });
    Check(!DecodeHotkeySettings(corruptBinding, decoded),
        "Corrupt persisted shortcut falls back instead of being accepted");

    auto duplicate = defaults;
    duplicate[HotkeyAction::CloseAll] = duplicate[HotkeyAction::NewRegion];
    auto duplicateResult = ValidateHotkeySettings(duplicate);
    Check(duplicateResult.error == HotkeyValidationError::DuplicateBinding &&
        duplicateResult.action == HotkeyAction::CloseAll &&
        duplicateResult.duplicate == HotkeyAction::NewRegion,
        "Duplicate RegionLens shortcuts identify both actions");
    auto disabled = defaults;
    disabled[HotkeyAction::NewRegion] = {};
    Check(ValidateHotkeySettings(disabled).Valid(),
        "New-region shortcut can be disabled");
    disabled[HotkeyAction::HideAll] = {};
    disabled[HotkeyAction::ShowAllTopmost] = {};
    Check(ValidateHotkeySettings(disabled).Valid(), "Visibility shortcuts can both be disabled");
    duplicate = defaults;
    duplicate[HotkeyAction::ShowAllTopmost] = defaults[HotkeyAction::HideAll];
    Check(ValidateHotkeySettings(duplicate).error == HotkeyValidationError::DuplicateBinding,
        "Visibility shortcuts participate in duplicate validation");

    auto candidate = defaults;
    candidate[HotkeyAction::NewRegion] = { MOD_CONTROL | MOD_SHIFT, 'Q' };
    {
        FakeRegistrar registrar;
        auto applied = ApplyHotkeysTransactional(defaults, candidate,
            [&] { registrar.Release(); },
            [&](HotkeySettings const& settings) { return registrar.Acquire(settings); });
        Check(applied.applied && applied.candidate.AllSucceeded() &&
            registrar.releases == 1 && registrar.acquisitions == 1,
            "Successful shortcut transaction registers the candidate once");
    }
    {
        FakeRegistrar registrar;
        registrar.conflictKey = 'Q';
        auto applied = ApplyHotkeysTransactional(defaults, candidate,
            [&] { registrar.Release(); },
            [&](HotkeySettings const& settings) { return registrar.Acquire(settings); });
        Check(!applied.applied && applied.candidate.HasConflict() && applied.rollbackSucceeded &&
            registrar.releases == 2 && registrar.acquisitions == 2,
            "Conflicting candidate is rejected and every previous shortcut is restored");
    }
    {
        FakeRegistrar registrar;
        registrar.conflictKey = 'Q';
        registrar.rollbackFailureKey = 'M';
        auto applied = ApplyHotkeysTransactional(defaults, candidate,
            [&] { registrar.Release(); },
            [&](HotkeySettings const& settings) { return registrar.Acquire(settings); });
        Check(!applied.applied && !applied.rollbackSucceeded &&
            !applied.rollback.AllSucceeded(),
            "Shortcut rollback failure remains visible to the controller");
    }

    SetAppLanguage(AppLanguage::English);
    {
        FakeRegistrar registrar; registrar.conflictKey = 'S';
        auto applied = ApplyHotkeysTransactional(disabled, defaults,
            [&] { registrar.Release(); }, [&](auto const& value) { return registrar.Acquire(value); });
        Check(!applied.applied && applied.candidate.HasConflict() && applied.rollbackSucceeded &&
            registrar.acquisitions == 2 && registrar.releases == 2,
            "Failure registering the fourth action rolls back all four shortcuts");
    }
    auto englishAbout = BuildAboutText();
    Check(englishAbout.find(L"Ctrl+") == std::wstring::npos && englishAbout.find(L"Esc") == std::wstring::npos &&
        englishAbout.find(L"Screenshots") != std::wstring::npos,
        "About keeps basic operations without listing shortcuts");
    SetAppLanguage(AppLanguage::SimplifiedChinese);
    return failures;
}
