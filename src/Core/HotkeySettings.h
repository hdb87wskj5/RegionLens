#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <string>
#include <span>

namespace RegionLens::native
{
    enum class HotkeyAction : size_t
    {
        NewRegion,
        CloseAll,
        HideAll,
        ShowAllTopmost,
        Count,
    };

    constexpr size_t HotkeyActionCount = static_cast<size_t>(HotkeyAction::Count);
    using HotkeyStorageRecord = std::array<DWORD, HotkeyActionCount + 2>;

    struct HotkeyBinding
    {
        UINT modifiers{};
        UINT virtualKey{};

        [[nodiscard]] bool Enabled() const noexcept { return modifiers != 0 || virtualKey != 0; }
        friend bool operator==(HotkeyBinding const&, HotkeyBinding const&) = default;
    };

    struct HotkeySettings
    {
        std::array<HotkeyBinding, HotkeyActionCount> bindings{};

        HotkeyBinding& operator[](HotkeyAction action) noexcept
        {
            return bindings[static_cast<size_t>(action)];
        }
        HotkeyBinding const& operator[](HotkeyAction action) const noexcept
        {
            return bindings[static_cast<size_t>(action)];
        }
        friend bool operator==(HotkeySettings const&, HotkeySettings const&) = default;
    };

    enum class HotkeyValidationError
    {
        None,
        InvalidBinding,
        DuplicateBinding,
    };

    struct HotkeyValidationResult
    {
        HotkeyValidationError error{};
        HotkeyAction action{};
        HotkeyAction duplicate{};
        [[nodiscard]] bool Valid() const noexcept { return error == HotkeyValidationError::None; }
    };

    struct HotkeyRegistrationItem
    {
        bool requested{};
        bool succeeded{ true };
        DWORD error{};
    };

    struct HotkeyRegistrationResult
    {
        std::array<HotkeyRegistrationItem, HotkeyActionCount> items{};
        [[nodiscard]] bool AllSucceeded() const noexcept;
        [[nodiscard]] bool HasConflict() const noexcept;
    };

    struct HotkeyTransactionResult
    {
        bool applied{};
        HotkeyRegistrationResult candidate{};
        HotkeyRegistrationResult rollback{};
        bool rollbackSucceeded{ true };
    };

    HotkeySettings DefaultHotkeySettings() noexcept;
    bool IsSupportedHotkey(HotkeyBinding binding) noexcept;
    HotkeyValidationResult ValidateHotkeySettings(HotkeySettings const& settings) noexcept;
    DWORD PackHotkeyBinding(HotkeyBinding binding) noexcept;
    HotkeyBinding UnpackHotkeyBinding(DWORD value) noexcept;
    HotkeyStorageRecord EncodeHotkeySettings(HotkeySettings const& settings) noexcept;
    bool DecodeHotkeySettings(std::span<DWORD const> record, HotkeySettings& settings) noexcept;
    wchar_t const* HotkeyActionName(HotkeyAction action) noexcept;
    std::wstring FormatHotkey(HotkeyBinding binding);
    HotkeySettings LoadHotkeySettings() noexcept;
    bool SaveHotkeySettings(HotkeySettings const& settings) noexcept;

    template<typename Release, typename Acquire>
    HotkeyTransactionResult ApplyHotkeysTransactional(
        HotkeySettings const& previous,
        HotkeySettings const& candidate,
        Release&& release,
        Acquire&& acquire)
    {
        HotkeyTransactionResult result;
        release();
        result.candidate = acquire(candidate);
        if (result.candidate.AllSucceeded())
        {
            result.applied = true;
            return result;
        }

        release();
        result.rollback = acquire(previous);
        result.rollbackSucceeded = result.rollback.AllSucceeded();
        return result;
    }
}
