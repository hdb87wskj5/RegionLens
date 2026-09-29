#pragma once

#include <windows.h>
#include <atomic>
#include "AppRuntime.h"

namespace RegionLens::native
{
    enum class AppLanguage : DWORD
    {
        SimplifiedChinese = 0,
        English = 1,
    };

    inline std::atomic<AppLanguage> g_appLanguage{ AppLanguage::SimplifiedChinese };

    inline AppLanguage CurrentAppLanguage() noexcept
    {
        return g_appLanguage.load(std::memory_order_relaxed);
    }

    inline bool UsesEnglish() noexcept
    {
        return CurrentAppLanguage() == AppLanguage::English;
    }

    inline void SetAppLanguage(AppLanguage language) noexcept
    {
        g_appLanguage.store(language, std::memory_order_relaxed);
    }

    inline wchar_t const* Localized(wchar_t const* chinese, wchar_t const* english) noexcept
    {
        return UsesEnglish() ? english : chinese;
    }

    void LoadAppLanguagePreference() noexcept;

    inline bool SaveAppLanguagePreference(AppLanguage language) noexcept
    {
        HKEY key{};
        if (RegCreateKeyExW(HKEY_CURRENT_USER, Runtime().identity->registry, 0, nullptr, 0,
            KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            return false;
        }
        DWORD value = DWORD(language);
        auto result = RegSetValueExW(key, L"Language", 0, REG_DWORD,
            reinterpret_cast<BYTE const*>(&value), sizeof(value));
        RegCloseKey(key);
        if (result == ERROR_SUCCESS) SetAppLanguage(language);
        return result == ERROR_SUCCESS;
    }

}
