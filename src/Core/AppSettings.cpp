#include "pch.h"
#include "AppSettings.h"
#include "ScreenshotStorage.h"
#include <cstring>

namespace RegionLens::native
{
    namespace
    {
        constexpr wchar_t ValueName[] = L"SettingsV3";
        constexpr wchar_t Version2ValueName[] = L"SettingsV2";
        constexpr wchar_t PreviousValueName[] = L"SettingsV1";
        constexpr DWORD Magic = 0x33534c52; // RLS3
        constexpr DWORD PreviousMagic = 0x31534c52; // RLS1
        constexpr size_t PreviousHotkeyActionCount = 5;
        constexpr size_t HeaderWords = 8 + HotkeyActionCount;
        constexpr size_t OldHeaderWords = 8 + PreviousHotkeyActionCount;
        constexpr size_t PreviousHeaderWords = 7 + PreviousHotkeyActionCount;
        constexpr size_t MaximumBytes = OldHeaderWords * sizeof(DWORD) + 32767 * sizeof(wchar_t);
        constexpr size_t PreviousMaximumBytes = PreviousHeaderWords * sizeof(DWORD) + 32767 * sizeof(wchar_t);
        AppSettings LoadLegacy(AppIdentity const& identity)
        {
            AppSettings result;
            DWORD language{}, size = sizeof(language);
            if (RegGetValueW(HKEY_CURRENT_USER, identity.registry, L"Language", RRF_RT_REG_DWORD,
                nullptr, &language, &size) == ERROR_SUCCESS && language <= DWORD(AppLanguage::English))
                result.language = AppLanguage(language);
            std::array<DWORD, PreviousHotkeyActionCount + 2> hotkeys{}; size = sizeof(hotkeys);
            if (RegGetValueW(HKEY_CURRENT_USER, identity.hotkeys, L"Bindings", RRF_RT_REG_BINARY,
                nullptr, hotkeys.data(), &size) == ERROR_SUCCESS && size % sizeof(DWORD) == 0)
                DecodeHotkeySettings({hotkeys.data(), size / sizeof(DWORD)}, result.hotkeys);
            result.screenshotDirectory = LoadScreenshotDirectory(identity);
            if (result.screenshotDirectory == DefaultScreenshotDirectory(identity)) result.screenshotDirectory.clear();
            return result;
        }

        bool DecodePreviousSettings(std::span<BYTE const> bytes, AppSettings& settings)
        {
            std::array<DWORD, PreviousHeaderWords> header{};
            if (bytes.size() < sizeof(header) || bytes.size() > PreviousMaximumBytes) return false;
            memcpy(header.data(), bytes.data(), sizeof(header));
            if (header[0] != PreviousMagic || header[1] != 1 || header[2] > DWORD(AppLanguage::English) ||
                header[3] > 1 || header[4] > 1 || header[5] != PreviousHotkeyActionCount ||
                header[6] > 32767 || bytes.size() != sizeof(header) + size_t(header[6]) * sizeof(wchar_t)) return false;
            AppSettings decoded;
            decoded.language = AppLanguage(header[2]);
            decoded.quality = header[3] ? LensSharpness::Medium : LensSharpness::Off;
            decoded.newWindowTopmost = header[4] != 0;
            decoded.fullscreenAspectFit = false;
            for (size_t i = 0; i < PreviousHotkeyActionCount; ++i)
            {
                if (i == 2) continue; // Removed cancel-mapping action.
                decoded.hotkeys.bindings[i > 2 ? i - 1 : i] = UnpackHotkeyBinding(header[7 + i]);
            }
            decoded.screenshotDirectory.resize(header[6]);
            if (header[6]) memcpy(decoded.screenshotDirectory.data(), bytes.data() + sizeof(header), size_t(header[6]) * sizeof(wchar_t));
            if (!ValidAppSettings(decoded)) return false;
            settings = std::move(decoded);
            return true;
        }

        enum class ReadRecordResult { Missing, Valid, Invalid };
        template<typename Decode>
        ReadRecordResult ReadRecord(AppIdentity const& identity, wchar_t const* name, size_t maximum,
            Decode&& decode, AppSettings& settings)
        {
            DWORD size{};
            auto error = RegGetValueW(HKEY_CURRENT_USER, identity.registry, name, RRF_RT_REG_BINARY, nullptr, nullptr, &size);
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) return ReadRecordResult::Missing;
            if (error != ERROR_SUCCESS || size > maximum) return ReadRecordResult::Invalid;
            std::vector<BYTE> bytes(size);
            error = RegGetValueW(HKEY_CURRENT_USER, identity.registry, name, RRF_RT_REG_BINARY,
                nullptr, bytes.data(), &size);
            if (error != ERROR_SUCCESS || !decode(std::span<BYTE const>(bytes.data(), size), settings))
                return ReadRecordResult::Invalid;
            return ReadRecordResult::Valid;
        }
    }

    bool ValidAppSettings(AppSettings const& settings) noexcept
    {
        auto const& path = settings.screenshotDirectory;
        bool absolute = path.empty() || (path.size() >= 3 &&
            (((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
                path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))) ||
            (path.size() > 2 && path[0] == L'\\' && path[1] == L'\\');
        return DWORD(settings.language) <= DWORD(AppLanguage::English) && DWORD(settings.quality) <= DWORD(LensSharpness::High) &&
            ValidateHotkeySettings(settings.hotkeys).Valid() && path.size() <= 32767 && absolute &&
            path.find(L'\0') == std::wstring::npos;
    }

    std::vector<BYTE> EncodeAppSettings(AppSettings const& settings)
    {
        if (!ValidAppSettings(settings)) return {};
        std::array<DWORD, HeaderWords> header{Magic, 3, DWORD(settings.language), DWORD(settings.quality),
            DWORD(settings.newWindowTopmost), DWORD(settings.fullscreenAspectFit), DWORD(HotkeyActionCount),
            DWORD(settings.screenshotDirectory.size())};
        for (size_t i = 0; i < HotkeyActionCount; ++i) header[8+i] = PackHotkeyBinding(settings.hotkeys.bindings[i]);
        std::vector<BYTE> bytes(sizeof(header) + settings.screenshotDirectory.size() * sizeof(wchar_t));
        memcpy(bytes.data(), header.data(), sizeof(header));
        if (!settings.screenshotDirectory.empty()) memcpy(bytes.data()+sizeof(header), settings.screenshotDirectory.data(), bytes.size()-sizeof(header));
        return bytes;
    }

    bool DecodeAppSettings(std::span<BYTE const> bytes, AppSettings& settings)
    {
        std::array<DWORD, OldHeaderWords> header{};
        if (bytes.size() < 8 * sizeof(DWORD) || bytes.size() > MaximumBytes) return false;
        memcpy(header.data(), bytes.data(), 8 * sizeof(DWORD));
        auto keyCount = size_t(header[6]);
        if (keyCount != HotkeyActionCount && keyCount != PreviousHotkeyActionCount) return false;
        auto headerBytes = (8 + keyCount) * sizeof(DWORD);
        if (bytes.size() < headerBytes) return false;
        memcpy(header.data() + 8, bytes.data() + 8 * sizeof(DWORD), keyCount * sizeof(DWORD));
        if (header[0] != Magic || header[1] != 3 || header[2] > DWORD(AppLanguage::English) ||
            header[3] > DWORD(LensSharpness::High) || header[4] > 1 || header[5] > 1 ||
            header[7] > 32767 || bytes.size() != headerBytes + size_t(header[7])*sizeof(wchar_t)) return false;
        AppSettings decoded;
        decoded.language = AppLanguage(header[2]); decoded.quality = LensSharpness(header[3]);
        decoded.newWindowTopmost = header[4] != 0; decoded.fullscreenAspectFit = header[5] != 0;
        for (size_t i = 0; i < keyCount; ++i)
        {
            if (keyCount == PreviousHotkeyActionCount && i == 2) continue;
            auto destination = keyCount == PreviousHotkeyActionCount && i > 2 ? i - 1 : i;
            decoded.hotkeys.bindings[destination] = UnpackHotkeyBinding(header[8 + i]);
        }
        decoded.screenshotDirectory.resize(header[7]);
        if (header[7]) memcpy(decoded.screenshotDirectory.data(), bytes.data()+headerBytes, size_t(header[7])*sizeof(wchar_t));
        if (!ValidAppSettings(decoded)) return false;
        settings = std::move(decoded);
        return true;
    }

    AppSettings LoadAppSettings(AppIdentity const& identity) noexcept
    {
        try {
            AppSettings result;
            auto current = ReadRecord(identity, ValueName, MaximumBytes, DecodeAppSettings, result);
            if (current == ReadRecordResult::Valid) return result;
            if (current == ReadRecordResult::Invalid) return {};
            auto version2 = ReadRecord(identity, Version2ValueName, MaximumBytes,
                [](std::span<BYTE const> bytes, AppSettings& decoded) {
                    if (bytes.size() < 8 * sizeof(DWORD)) return false;
                    std::array<DWORD, 4> fields{};
                    memcpy(fields.data(), bytes.data(), sizeof(fields));
                    if (fields[0] != 0x32534c52 || fields[1] != 2 || fields[3] > 1) return false;
                    fields[0] = Magic; fields[1] = 3;
                    fields[3] = DWORD(fields[3] ? LensSharpness::Medium : LensSharpness::Off);
                    std::vector<BYTE> migrated(bytes.begin(), bytes.end());
                    memcpy(migrated.data(), fields.data(), sizeof(fields));
                    return DecodeAppSettings(migrated, decoded);
                }, result);
            if (version2 == ReadRecordResult::Valid) return result;
            if (version2 == ReadRecordResult::Invalid) return {};
            auto previous = ReadRecord(identity, PreviousValueName, PreviousMaximumBytes, DecodePreviousSettings, result);
            if (previous == ReadRecordResult::Valid) return result;
            if (previous == ReadRecordResult::Invalid) return {};
            return LoadLegacy(identity);
        } catch (...) { return {}; }
    }

    HRESULT SaveAppSettings(AppIdentity const& identity, AppSettings const& settings) noexcept
    {
        try {
            auto bytes = EncodeAppSettings(settings);
            if (bytes.empty()) return E_INVALIDARG;
            HKEY key{};
            auto error = RegCreateKeyExW(HKEY_CURRENT_USER, identity.registry, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (error != ERROR_SUCCESS) return HRESULT_FROM_WIN32(error);
            error = RegSetValueExW(key, ValueName, 0, REG_BINARY, bytes.data(), DWORD(bytes.size()));
            RegCloseKey(key);
            return HRESULT_FROM_WIN32(error);
        } catch (...) { return E_OUTOFMEMORY; }
    }

    std::wstring ResolveScreenshotDirectory(AppSettings const& settings, AppIdentity const& identity)
    { return settings.screenshotDirectory.empty() ? DefaultScreenshotDirectory(identity) : settings.screenshotDirectory; }

    void LoadAppLanguagePreference() noexcept
    { SetAppLanguage(LoadAppSettings(*Runtime().identity).language); }

    std::wstring BuildAboutText()
    {
        return BuildIdentityText() + L"\r\n\r\n" + Localized(
            L"• 框选屏幕内容，创建持续更新的实时区域。\r\n\r\n"
            L"• 拖动画面移动窗口；四角等比例缩放，四边单向拉伸。\r\n\r\n"
            L"• 开启鼠标映射后，可在实时区域操作原窗口。\r\n\r\n"
            L"• 截图同时复制到剪贴板并保存为 PNG。\r\n\r\n"
            L"• 右上角按钮可调节鼠标速度、恢复尺寸、全屏、置顶或关闭区域。",
            L"• Select screen content to create a continuously updated live region.\r\n\r\n"
            L"• Drag the picture to move; drag corners proportionally or edges in one direction.\r\n\r\n"
            L"• Enable mouse mapping to operate the original window through the live region.\r\n\r\n"
            L"• Screenshots are copied to the clipboard and saved as PNG.\r\n\r\n"
            L"• Top-right controls adjust mouse speed, restore size, enter full screen, pin or close a region.");
    }
}
