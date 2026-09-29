#include "resource.h"
#include "ProductBuild.h"
#include "AppRuntime.h"
#include "FileTransaction.h"

#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wintrust.lib")

namespace
{
    #ifdef REGIONLENS_DEV
    constexpr auto Identity = RegionLens::native::DevIdentity;
#else
    constexpr auto Identity = RegionLens::native::StableIdentity;
#endif
    constexpr auto AppName = Identity.name;
    constexpr auto AppVersion = RL_VERSION;
    constexpr auto AppExeName = Identity.executable;
    constexpr auto SetupExeName = Identity.setupExecutable;
    constexpr auto WeTypeDllName = Identity.channel == RegionLens::native::AppChannel::Dev ?
        L"RegionLens-Dev-WeTypeProbe.dll" : L"RegionLens-WeTypeCompat.dll";
    constexpr auto CertSubjectExpected = Identity.certificateSubject;
    const std::wstring UninstallKeyText = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\") + Identity.folder;
    const auto UninstallKey = UninstallKeyText.c_str();
    constexpr wchar_t AutoStartKey[] =
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr auto AutoStartValue = Identity.folder;

    struct HandleCloser
    {
        void operator()(HANDLE value) const noexcept
        {
            if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        }
    };
    using unique_handle = std::unique_ptr<void, HandleCloser>;

    struct LocalFreeCloser
    {
        void operator()(void* value) const noexcept { if (value) LocalFree(value); }
    };
    using unique_local = std::unique_ptr<void, LocalFreeCloser>;

    struct CoTaskMemCloser
    {
        void operator()(wchar_t* value) const noexcept { if (value) CoTaskMemFree(value); }
    };
    using unique_cotask_string = std::unique_ptr<wchar_t, CoTaskMemCloser>;

    struct CertContextCloser
    {
        void operator()(PCCERT_CONTEXT value) const noexcept { if (value) CertFreeCertificateContext(value); }
    };
    using unique_cert = std::unique_ptr<const CERT_CONTEXT, CertContextCloser>;

    struct CertStoreCloser
    {
        void operator()(HCERTSTORE value) const noexcept { if (value) CertCloseStore(value, 0); }
    };
    using unique_cert_store = std::unique_ptr<void, CertStoreCloser>;

    struct CryptMessageCloser
    {
        void operator()(HCRYPTMSG value) const noexcept { if (value) CryptMsgClose(value); }
    };
    using unique_crypt_message = std::unique_ptr<void, CryptMessageCloser>;

    struct TempFile
    {
        std::wstring path;
        ~TempFile() { if (!path.empty()) DeleteFileW(path.c_str()); }
    };

    struct PayloadInfo
    {
        std::vector<BYTE> app;
        std::vector<BYTE> certificate;
        std::vector<BYTE> probe;
        std::vector<BYTE> weTypeDll;
        std::array<BYTE, 32> probeHash{};
        std::array<BYTE, 32> weTypeDllHash{};
        std::array<BYTE, 32> appHash{};
        std::wstring certSubject;
        std::wstring certThumbprint;
    };

    struct InstallOptions
    {
        bool autoStart{ Identity.defaultAutoStart };
        bool desktopShortcut{ true };
    };

    struct InstallDialogContext
    {
        PayloadInfo const* payload{};
        std::wstring installDirectory;
        InstallOptions options;
    };

    std::wstring FormatError(DWORD error)
    {
        wchar_t* raw{};
        auto length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
            reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
        unique_local holder(raw);
        if (!length) return L"错误 " + std::to_wstring(error);
        std::wstring result(raw, length);
        while (!result.empty() && iswspace(result.back())) result.pop_back();
        return result + L" (" + std::to_wstring(error) + L")";
    }

    bool Fail(std::wstring const& message, UINT flags = MB_ICONERROR)
    {
        MessageBoxW(nullptr, message.c_str(), AppName, flags | MB_OK);
        return false;
    }

    std::vector<BYTE> LoadBinaryResource(WORD id)
    {
        auto module = GetModuleHandleW(nullptr);
        auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
        if (!resource) return {};
        auto size = SizeofResource(module, resource);
        auto loaded = LoadResource(module, resource);
        auto bytes = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        return bytes && size ? std::vector<BYTE>(bytes, bytes + size) : std::vector<BYTE>{};
    }

    bool ReadFileBytes(std::wstring const& path, std::vector<BYTE>& bytes)
    {
        unique_handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.get(), &size) || size.QuadPart < 0 || size.QuadPart > MAXDWORD) return false;
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read{};
        return (bytes.empty() || ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) &&
            read == bytes.size();
    }

    bool WriteFileBytes(std::wstring const& path, std::vector<BYTE> const& bytes)
    {
        unique_handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.get() == INVALID_HANDLE_VALUE) return false;
        DWORD written{};
        auto ok = (bytes.empty() || WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr)) && written == bytes.size() && FlushFileBuffers(file.get());
        if (!ok)
        {
            file.reset();
            DeleteFileW(path.c_str());
        }
        return ok;
    }

    std::wstring ModulePath()
    {
        std::wstring path(32768, L'\0');
        auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length == path.size()) return {};
        path.resize(length);
        return path;
    }

    bool ComputeSha256(const BYTE* data, size_t size, std::array<BYTE, 32>& result)
    {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE hash{};
        DWORD objectSize{}, copied{}, hashSize{};
        std::vector<BYTE> object;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &copied, 0) >= 0 &&
            BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                reinterpret_cast<PUCHAR>(&hashSize), sizeof(hashSize), &copied, 0) >= 0 &&
            hashSize == result.size();
        if (ok)
        {
            object.resize(objectSize);
            ok = BCryptCreateHash(algorithm, &hash, object.data(), objectSize, nullptr, 0, 0) >= 0 &&
                BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) >= 0 &&
                BCryptFinishHash(hash, result.data(), static_cast<ULONG>(result.size()), 0) >= 0;
        }
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return ok;
    }

    int HexValue(wchar_t value)
    {
        if (value >= L'0' && value <= L'9') return value - L'0';
        value = static_cast<wchar_t>(towupper(value));
        return value >= L'A' && value <= L'F' ? value - L'A' + 10 : -1;
    }

    bool ParseSha256(std::vector<BYTE> const& text, std::array<BYTE, 32>& hash)
    {
        std::wstring hex;
        for (auto byte : text)
        {
            auto value = static_cast<wchar_t>(byte);
            if (!iswspace(value)) hex.push_back(value);
        }
        if (hex.size() != hash.size() * 2) return false;
        for (size_t i = 0; i < hash.size(); ++i)
        {
            auto high = HexValue(hex[i * 2]);
            auto low = HexValue(hex[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            hash[i] = static_cast<BYTE>((high << 4) | low);
        }
        return true;
    }

    std::wstring HexString(const BYTE* bytes, DWORD size)
    {
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        std::wstring result;
        result.reserve(size * 2);
        for (DWORD i = 0; i < size; ++i)
        {
            result.push_back(digits[bytes[i] >> 4]);
            result.push_back(digits[bytes[i] & 0x0f]);
        }
        return result;
    }

    std::wstring CertificateThumbprint(PCCERT_CONTEXT certificate)
    {
        std::array<BYTE, 64> hash{};
        DWORD size = static_cast<DWORD>(hash.size());
        if (!CertGetCertificateContextProperty(certificate, CERT_HASH_PROP_ID, hash.data(), &size)) return {};
        return HexString(hash.data(), size);
    }

    std::wstring CertificateSubject(PCCERT_CONTEXT certificate)
    {
        auto length = CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
        if (length <= 1) return {};
        std::wstring name(length, L'\0');
        CertGetNameStringW(certificate, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), length);
        name.resize(length - 1);
        return name;
    }

    bool CreateTemporaryExe(std::vector<BYTE> const& bytes, TempFile& temp, wchar_t const* extension = L".exe")
    {
        wchar_t directory[MAX_PATH]{};
        wchar_t base[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"RLN", 0, base)) return false;
        DeleteFileW(base);
        temp.path = std::wstring(base) + extension;
        if (WriteFileBytes(temp.path, bytes)) return true;
        temp.path.clear(); return false; // Do not delete a pre-existing colliding file.
    }

    bool IsAmd64Pe(std::vector<BYTE> const& bytes)
    {
        if (bytes.size() < sizeof(IMAGE_DOS_HEADER)) return false;
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(bytes.data());
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        auto offset = static_cast<size_t>(dos->e_lfanew);
        if (offset > bytes.size() || bytes.size() - offset < sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER)) return false;
        auto signature = *reinterpret_cast<const DWORD*>(bytes.data() + offset);
        auto header = reinterpret_cast<const IMAGE_FILE_HEADER*>(bytes.data() + offset + sizeof(DWORD));
        return signature == IMAGE_NT_SIGNATURE && header->Machine == IMAGE_FILE_MACHINE_AMD64;
    }

    bool HasExpectedVersion(std::wstring const& path, wchar_t const* expectedFile = AppExeName)
    {
        DWORD ignored{};
        auto size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
        if (!size) return false;
        std::vector<BYTE> data(size);
        if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return false;
        VS_FIXEDFILEINFO* info{};
        UINT infoSize{};
        if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
            infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE) return false;
        constexpr unsigned parts[]{ RL_VERSION_TUPLE };
        if (HIWORD(info->dwFileVersionMS) != parts[0] || LOWORD(info->dwFileVersionMS) != parts[1] ||
            HIWORD(info->dwFileVersionLS) != parts[2] || LOWORD(info->dwFileVersionLS) != parts[3]) return false;
        auto matches = [&](wchar_t const* field, wchar_t const* expected) {
            wchar_t* actual{}; UINT length{};
            auto key = std::wstring(L"\\StringFileInfo\\080404B0\\") + field;
            return VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&actual), &length) &&
                actual && length && wcscmp(actual, expected) == 0;
        };
        return matches(L"OriginalFilename", expectedFile) && matches(L"SourceCommit", RL_COMMIT) &&
            matches(L"InternalName", Identity.channel == RegionLens::native::AppChannel::Dev ? L"Dev" : L"Stable");
    }

    bool VerifyEmbeddedSignature(std::wstring const& path, std::wstring const& expectedThumbprint, wchar_t const* expectedSubject = nullptr)
    {
        HCERTSTORE rawStore{};
        HCRYPTMSG rawMessage{};
        DWORD encoding{}, content{}, format{};
        if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
            CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0,
            &encoding, &content, &format, &rawStore, &rawMessage, nullptr)) return false;
        unique_cert_store store(rawStore);
        unique_crypt_message message(rawMessage);

        DWORD signerSize{};
        if (!CryptMsgGetParam(rawMessage, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &signerSize)) return false;
        std::vector<BYTE> signerData(signerSize);
        if (!CryptMsgGetParam(rawMessage, CMSG_SIGNER_INFO_PARAM, 0, signerData.data(), &signerSize)) return false;
        auto signer = reinterpret_cast<PCMSG_SIGNER_INFO>(signerData.data());
        CERT_INFO identity{};
        identity.Issuer = signer->Issuer;
        identity.SerialNumber = signer->SerialNumber;
        unique_cert certificate(CertFindCertificateInStore(rawStore, encoding, 0,
            CERT_FIND_SUBJECT_CERT, &identity, nullptr));
        if (!certificate || _wcsicmp(CertificateThumbprint(certificate.get()).c_str(), expectedThumbprint.c_str()) != 0)
            return false;
        if (expectedSubject && _wcsicmp(CertificateSubject(certificate.get()).c_str(), expectedSubject)) return false;
        return CryptMsgControl(rawMessage, 0, CMSG_CTRL_VERIFY_SIGNATURE,
            const_cast<PCERT_INFO>(certificate->pCertInfo)) != FALSE;
    }

    bool VerifyWindowsTrust(std::wstring const& path)
    {
        WINTRUST_FILE_INFO file{};
        file.cbStruct = sizeof(file);
        file.pcwszFilePath = path.c_str();
        WINTRUST_DATA trust{};
        trust.cbStruct = sizeof(trust);
        trust.dwUIChoice = WTD_UI_NONE;
        trust.fdwRevocationChecks = WTD_REVOKE_NONE;
        trust.dwUnionChoice = WTD_CHOICE_FILE;
        trust.pFile = &file;
        trust.dwStateAction = WTD_STATEACTION_VERIFY;
        trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
        GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        auto status = WinVerifyTrust(nullptr, &policy, &trust);
        trust.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &policy, &trust);
        return status == ERROR_SUCCESS;
    }

    bool LoadAndValidatePayload(PayloadInfo& payload, std::wstring& error, int& errorCode)
    {
        errorCode = 10;
        payload.app = LoadBinaryResource(IDR_REGIONLENS_EXE);
        payload.certificate = LoadBinaryResource(IDR_REGIONLENS_CERT);
        auto hashText = LoadBinaryResource(IDR_REGIONLENS_HASH);
        if (payload.app.empty() || payload.certificate.empty() || hashText.empty())
        {
            error = L"安装包缺少内嵌程序、证书或摘要资源。";
            return false;
        }
        if (!IsAmd64Pe(payload.app))
        {
            errorCode = 11;
            error = L"内嵌程序不是有效的 x64 Windows 程序。";
            return false;
        }
        if (!ParseSha256(hashText, payload.appHash))
        {
            errorCode = 12;
            error = L"安装包中的 SHA-256 摘要格式无效。";
            return false;
        }
        std::array<BYTE, 32> actual{};
        if (!ComputeSha256(payload.app.data(), payload.app.size(), actual) || actual != payload.appHash)
        {
            errorCode = 13;
            error = L"内嵌 RegionLens.exe 的 SHA-256 校验失败。";
            return false;
        }
        unique_cert certificate(CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            payload.certificate.data(), static_cast<DWORD>(payload.certificate.size())));
        if (!certificate)
        {
            errorCode = 14;
            error = L"安装包中的公钥证书无效。";
            return false;
        }
        payload.certSubject = CertificateSubject(certificate.get());
        payload.certThumbprint = CertificateThumbprint(certificate.get());
        if (_wcsicmp(payload.certSubject.c_str(), CertSubjectExpected) != 0 || payload.certThumbprint.empty())
        {
            errorCode = 15;
            error = L"安装包中的证书身份不符合 RegionLens 发布要求。";
            return false;
        }
        TempFile app;
        if (!CreateTemporaryExe(payload.app, app) || !HasExpectedVersion(app.path))
        {
            errorCode = 16;
            error = std::wstring(L"内嵌程序版本与预期不一致：") + AppVersion + L"。";
            return false;
        }
        if (!VerifyEmbeddedSignature(app.path, payload.certThumbprint))
        {
            errorCode = 17;
            error = L"内嵌程序的数字签名与安装包证书不匹配或签名已损坏。";
            return false;
        }
#ifdef REGIONLENS_DEV
        payload.probe = LoadBinaryResource(IDR_REGIONLENS_PROBE);
        auto probeHash = LoadBinaryResource(IDR_REGIONLENS_PROBE_HASH);
        TempFile probe;
        std::array<BYTE, 32> actualProbe{};
        if (payload.probe.empty() || !IsAmd64Pe(payload.probe) || !ParseSha256(probeHash, payload.probeHash) ||
            !ComputeSha256(payload.probe.data(), payload.probe.size(), actualProbe) || actualProbe != payload.probeHash ||
            !CreateTemporaryExe(payload.probe, probe) || !HasExpectedVersion(probe.path, L"RegionLens-InputProbe.exe") ||
            !VerifyEmbeddedSignature(probe.path, payload.certThumbprint))
        { errorCode = 19; error = L"测试接收器的版本、渠道、摘要或签名校验失败。"; return false; }
#endif
        payload.weTypeDll = LoadBinaryResource(IDR_WETYPE_PROBE_DLL);
        auto dllHash = LoadBinaryResource(IDR_WETYPE_PROBE_HASH);
        TempFile dll;
        std::array<BYTE, 32> actualDll{};
        if (payload.weTypeDll.empty() || !IsAmd64Pe(payload.weTypeDll) ||
            !ParseSha256(dllHash, payload.weTypeDllHash) ||
            !ComputeSha256(payload.weTypeDll.data(), payload.weTypeDll.size(), actualDll) ||
            actualDll != payload.weTypeDllHash ||
            !CreateTemporaryExe(payload.weTypeDll, dll, L".dll") ||
            !HasExpectedVersion(dll.path, WeTypeDllName) ||
            !VerifyEmbeddedSignature(dll.path, payload.certThumbprint))
        { errorCode = 20; error = L"微信输入法兼容组件的版本、渠道、摘要或签名校验失败。"; return false; }
        auto setupPath = ModulePath();
        if (setupPath.empty() || !HasExpectedVersion(setupPath, SetupExeName) || !VerifyEmbeddedSignature(setupPath, payload.certThumbprint))
        {
            errorCode = 18;
            error = L"安装程序自身的数字签名与内嵌证书不匹配或签名已损坏。";
            return false;
        }
        return true;
    }

    std::wstring KnownFolder(REFKNOWNFOLDERID id)
    {
        PWSTR raw{};
        if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
        unique_cotask_string value(raw);
        return raw;
    }

    std::wstring InstallDirectory()
    {
        auto programFiles = KnownFolder(FOLDERID_ProgramFiles);
        return programFiles.empty() ? std::wstring{} : programFiles + L"\\" + Identity.folder;
    }

    std::wstring StartMenuShortcut()
    {
        auto programs = KnownFolder(FOLDERID_CommonPrograms);
        return programs.empty() ? std::wstring{} : programs + L"\\" + Identity.folder + L".lnk";
    }

    std::wstring DesktopShortcut()
    {
        auto desktop = KnownFolder(FOLDERID_PublicDesktop);
        return desktop.empty() ? std::wstring{} : desktop + L"\\" + Identity.folder + L".lnk";
    }

    void CenterDialogOnCurrentMonitor(HWND dialog)
    {
        RECT bounds{};
        POINT pointer{};
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        if (!GetWindowRect(dialog, &bounds) || !GetCursorPos(&pointer)) return;
        auto monitor = MonitorFromPoint(pointer, MONITOR_DEFAULTTONEAREST);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo)) return;

        auto width = bounds.right - bounds.left;
        auto height = bounds.bottom - bounds.top;
        auto workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
        auto workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
        auto x = monitorInfo.rcWork.left + (workWidth - width) / 2;
        auto y = monitorInfo.rcWork.top + (workHeight - height) / 2;
        SetWindowPos(dialog, nullptr, x, y, 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    INT_PTR CALLBACK InstallOptionsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto context = reinterpret_cast<InstallDialogContext*>(
            GetWindowLongPtrW(dialog, DWLP_USER));
        if (message == WM_INITDIALOG)
        {
            context = reinterpret_cast<InstallDialogContext*>(lParam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
            if (!context || !context->payload) return FALSE;

            auto icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_REGIONLENS));
            SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
            SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));
            SetWindowTextW(dialog, AppName);
            auto productTitle = std::wstring(AppName) + L" " + AppVersion;
            SetDlgItemTextW(dialog, IDC_PRODUCT_TITLE, productTitle.c_str());
            SetDlgItemTextW(dialog, IDC_INSTALL_PATH, context->installDirectory.c_str());
            SetDlgItemTextW(dialog, IDC_CERTIFICATE, context->payload->certSubject.c_str());
            SetDlgItemTextW(dialog, IDC_THUMBPRINT, context->payload->certThumbprint.c_str());
            SendDlgItemMessageW(dialog, IDC_AUTOSTART, BM_SETCHECK,
                context->options.autoStart ? BST_CHECKED : BST_UNCHECKED, 0);
            SendDlgItemMessageW(dialog, IDC_DESKTOP_SHORTCUT, BM_SETCHECK,
                context->options.desktopShortcut ? BST_CHECKED : BST_UNCHECKED, 0);
            CenterDialogOnCurrentMonitor(dialog);
            return TRUE;
        }
        if (message == WM_COMMAND)
        {
            if (LOWORD(wParam) == IDOK && context)
            {
                context->options.autoStart =
                    SendDlgItemMessageW(dialog, IDC_AUTOSTART, BM_GETCHECK, 0, 0) == BST_CHECKED;
                context->options.desktopShortcut =
                    SendDlgItemMessageW(dialog, IDC_DESKTOP_SHORTCUT, BM_GETCHECK, 0, 0) == BST_CHECKED;
                EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (LOWORD(wParam) == IDCANCEL)
            {
                EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
        }
        return FALSE;
    }

    bool ShowInstallOptions(PayloadInfo const& payload, InstallOptions& options)
    {
        InstallDialogContext context{ &payload, InstallDirectory(), options };
        auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_INSTALL_OPTIONS),
            nullptr, InstallOptionsDialogProc, reinterpret_cast<LPARAM>(&context));
        if (result == -1)
        {
            Fail(L"Unable to open the RegionLens installation options.\n" + FormatError(GetLastError()));
            return false;
        }
        if (result != IDOK) return false;
        options = context.options;
        return true;
    }

    bool IsWindows11X64()
    {
        SYSTEM_INFO info{};
        GetNativeSystemInfo(&info);
        if (info.wProcessorArchitecture != PROCESSOR_ARCHITECTURE_AMD64) return false;
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        auto ntdll = GetModuleHandleW(L"ntdll.dll");
        auto function = ntdll ? reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")) : nullptr;
        RTL_OSVERSIONINFOW version{ sizeof(version) };
        return function && function(&version) == 0 && version.dwMajorVersion >= 10 && version.dwBuildNumber >= 22000;
    }

    bool IsElevated()
    {
        HANDLE raw{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw)) return false;
        unique_handle token(raw);
        TOKEN_ELEVATION elevation{};
        DWORD size{};
        return GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &size) &&
            elevation.TokenIsElevated;
    }

    int RelaunchElevated(std::wstring_view arguments)
    {
        auto path = ModulePath();
        SHELLEXECUTEINFOW info{ sizeof(info) };
        info.fMask = SEE_MASK_NOCLOSEPROCESS;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        std::wstring parameters(arguments);
        info.lpParameters = parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&info))
        {
            auto error = GetLastError();
            if (error != ERROR_CANCELLED) Fail(L"无法请求管理员权限：\n" + FormatError(error));
            return error == ERROR_CANCELLED ? ERROR_CANCELLED : 1;
        }
        unique_handle process(info.hProcess);
        WaitForSingleObject(process.get(), INFINITE);
        DWORD exitCode{ 1 };
        GetExitCodeProcess(process.get(), &exitCode);
        return static_cast<int>(exitCode);
    }

    bool SamePath(std::wstring left, std::wstring right)
    {
        auto normalize = [](std::wstring& value)
        {
            std::replace(value.begin(), value.end(), L'/', L'\\');
            while (!value.empty() && value.back() == L'\\') value.pop_back();
        };
        normalize(left);
        normalize(right);
        return _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    bool RegionLensIsRunning(std::wstring const& installedExe)
    {
        unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.get() == INVALID_HANDLE_VALUE) return true;
        PROCESSENTRY32W entry{ sizeof(entry) };
        if (!Process32FirstW(snapshot.get(), &entry)) return true;
        do
        {
            auto basename = installedExe.substr(installedExe.find_last_of(L"\\/") + 1);
            if (_wcsicmp(entry.szExeFile, basename.c_str()) != 0) continue;
            unique_handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
            if (!process) return true;
            std::wstring path(32768, L'\0');
            DWORD size = static_cast<DWORD>(path.size());
            if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)) return true;
            path.resize(size);
            if (SamePath(path, installedExe)) return true;
        } while (Process32NextW(snapshot.get(), &entry));
        return false;
    }

    bool PathExists(std::wstring const& path);

    bool WeTypeProbeIsLoaded(std::wstring const& installedDll)
    {
        if (!PathExists(installedDll)) return false;
        unique_handle processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (processes.get() == INVALID_HANDLE_VALUE) return true;
        PROCESSENTRY32W entry{ sizeof(entry) };
        for (BOOL more = Process32FirstW(processes.get(), &entry); more;
            more = Process32NextW(processes.get(), &entry))
        {
            if (_wcsicmp(entry.szExeFile, L"wetype_renderer.exe")) continue;
            unique_handle modules(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
                entry.th32ProcessID));
            if (modules.get() == INVALID_HANDLE_VALUE) return true;
            MODULEENTRY32W module{ sizeof(module) };
            for (BOOL hasModule = Module32FirstW(modules.get(), &module); hasModule;
                hasModule = Module32NextW(modules.get(), &module))
                if (SamePath(module.szExePath, installedDll)) return true;
        }
        return false;
    }

    bool FindCertificate(std::wstring const& storeName, std::wstring const& thumbprint)
    {
        if (thumbprint.size() != 40) return false;
        unique_cert_store store(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
            CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, storeName.c_str()));
        if (!store) return false;
        std::vector<BYTE> hash(thumbprint.size() / 2);
        for (size_t i = 0; i < hash.size(); ++i)
        {
            auto high = HexValue(thumbprint[i * 2]);
            auto low = HexValue(thumbprint[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            hash[i] = static_cast<BYTE>((high << 4) | low);
        }
        CRYPT_HASH_BLOB blob{ static_cast<DWORD>(hash.size()), hash.data() };
        unique_cert found(CertFindCertificateInStore(store.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0, CERT_FIND_HASH, &blob, nullptr));
        return static_cast<bool>(found);
    }

    bool AddCertificate(std::wstring const& storeName, std::vector<BYTE> const& encoded,
        std::wstring const& thumbprint, bool& added)
    {
        added = false;
        if (FindCertificate(storeName, thumbprint)) return true;
        unique_cert_store store(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
            CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName.c_str()));
        if (!store) return false;
        PCCERT_CONTEXT result{};
        if (!CertAddEncodedCertificateToStore(store.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            encoded.data(), static_cast<DWORD>(encoded.size()), CERT_STORE_ADD_NEW, &result)) return false;
        unique_cert holder(result);
        added = true;
        return true;
    }

    bool RemoveCertificate(std::wstring const& storeName, std::wstring const& thumbprint)
    {
        if (thumbprint.size() != 40) return false;
        unique_cert_store store(CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
            CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG, storeName.c_str()));
        if (!store) return GetLastError() == ERROR_FILE_NOT_FOUND;
        std::vector<BYTE> hash(thumbprint.size() / 2);
        for (size_t i = 0; i < hash.size(); ++i)
        {
            auto high = HexValue(thumbprint[i * 2]);
            auto low = HexValue(thumbprint[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            hash[i] = static_cast<BYTE>((high << 4) | low);
        }
        CRYPT_HASH_BLOB blob{ static_cast<DWORD>(hash.size()), hash.data() };
        auto found = CertFindCertificateInStore(store.get(), X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0, CERT_FIND_HASH, &blob, nullptr);
        return !found || CertDeleteCertificateFromStore(found);
    }

    DWORD ReadRegistryDword(HKEY key, wchar_t const* name)
    {
        DWORD value{}, size = sizeof(value), type{};
        return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) == ERROR_SUCCESS &&
            type == REG_DWORD ? value : 0;
    }

    std::wstring ReadRegistryString(HKEY key, wchar_t const* name)
    {
        DWORD size{}, type{};
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || size < sizeof(wchar_t)) return {};
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) != ERROR_SUCCESS)
            return {};
        while (!value.empty() && value.back() == L'\0') value.pop_back();
        return value;
    }

    bool SetRegistryString(HKEY key, wchar_t const* name, std::wstring const& value)
    {
        return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    }

    bool SetRegistryDword(HKEY key, wchar_t const* name, DWORD value)
    {
        return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value),
            sizeof(value)) == ERROR_SUCCESS;
    }

    bool ConfigureAutoStart(bool enabled, std::wstring const& appPath)
    {
        HKEY raw{};
        if (enabled)
        {
            if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, AutoStartKey, 0, nullptr, 0, KEY_SET_VALUE,
                nullptr, &raw, nullptr) != ERROR_SUCCESS) return false;
            std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw, RegCloseKey);
            auto command = L"\"" + appPath + L"\" --startup";
            return SetRegistryString(raw, AutoStartValue, command);
        }

        auto status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, AutoStartKey, 0, KEY_SET_VALUE, &raw);
        if (status == ERROR_FILE_NOT_FOUND) return true;
        if (status != ERROR_SUCCESS) return false;
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw, RegCloseKey);
        status = RegDeleteValueW(raw, AutoStartValue);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }

    bool RegisterUninstaller(std::wstring const& installDir, std::wstring const& thumbprint,
        bool ownsRoot, bool ownsPublisher, InstallOptions const& options)
    {
        HKEY raw{};
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, UninstallKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &raw, nullptr) != ERROR_SUCCESS) return false;
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw, RegCloseKey);
        auto app = installDir + L"\\" + AppExeName;
        auto setup = installDir + L"\\" + SetupExeName;
        auto uninstall = L"\"" + setup + L"\" /uninstall";
        return SetRegistryString(raw, L"DisplayName", AppName) &&
            SetRegistryString(raw, L"DisplayVersion", AppVersion) &&
            SetRegistryString(raw, L"Publisher", CertSubjectExpected) &&
            SetRegistryString(raw, L"InstallLocation", installDir) &&
            SetRegistryString(raw, L"DisplayIcon", app) &&
            SetRegistryString(raw, L"UninstallString", uninstall) &&
            SetRegistryString(raw, L"CertThumbprint", thumbprint) &&
            SetRegistryDword(raw, L"NoModify", 1) && SetRegistryDword(raw, L"NoRepair", 1) &&
            SetRegistryDword(raw, L"AutoStart", options.autoStart ? 1 : 0) &&
            SetRegistryDword(raw, L"DesktopShortcut", options.desktopShortcut ? 1 : 0) &&
            SetRegistryDword(raw, L"OwnsRootCertificate", ownsRoot ? 1 : 0) &&
            SetRegistryDword(raw, L"OwnsPublisherCertificate", ownsPublisher ? 1 : 0);
    }

    bool CreateShortcut(std::wstring const& shortcutPath, std::wstring const& appPath,
        std::wstring const& installDir)
    {
        auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto shouldUninitialize = SUCCEEDED(initialized);
        IShellLinkW* rawLink{};
        auto result = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&rawLink));
        if (FAILED(result))
        {
            if (shouldUninitialize) CoUninitialize();
            return false;
        }
        std::unique_ptr<IShellLinkW, void(*)(IShellLinkW*)> link(rawLink,
            [](IShellLinkW* value) { value->Release(); });
        link->SetPath(appPath.c_str());
        link->SetWorkingDirectory(installDir.c_str());
        link->SetDescription(L"RegionLens 屏幕区域镜");
        IPersistFile* rawPersist{};
        result = link->QueryInterface(IID_PPV_ARGS(&rawPersist));
        if (SUCCEEDED(result))
        {
            std::unique_ptr<IPersistFile, void(*)(IPersistFile*)> persist(rawPersist,
                [](IPersistFile* value) { value->Release(); });
            result = persist->Save(shortcutPath.c_str(), TRUE);
        }
        if (shouldUninitialize) CoUninitialize();
        return SUCCEEDED(result);
    }

    void LaunchInstalledApp(std::wstring const& appPath)
    {
        auto windows = KnownFolder(FOLDERID_Windows);
        auto explorer = windows.empty() ? L"explorer.exe" : windows + L"\\explorer.exe";
        auto arguments = L"\"" + appPath + L"\"";
        ShellExecuteW(nullptr, L"open", explorer.c_str(), arguments.c_str(), nullptr, SW_SHOWNORMAL);
    }

    bool PathExists(std::wstring const& path)
    {
        return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    struct NativeFileOps
    {
        DWORD error{}, rollbackError{};
        bool Exists(std::wstring const& path) { return PathExists(path); }
        bool Move(std::wstring const& from, std::wstring const& to)
        {
            if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_WRITE_THROUGH)) return true;
            if (!error) error = GetLastError(); return false;
        }
        bool Remove(std::wstring const& path)
        { return DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND; }
        void RollbackFailed() { rollbackError = GetLastError(); }
    };

    bool ValidatePreviousInstall(std::wstring const& directory, std::wstring const& app,
        std::wstring const& setup, std::wstring const& probe, std::wstring const& weTypeDll)
    {
        HKEY raw{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UninstallKey, 0, KEY_QUERY_VALUE, &raw) == ERROR_SUCCESS)
        {
            auto location = ReadRegistryString(raw, L"InstallLocation");
            auto thumb = ReadRegistryString(raw, L"CertThumbprint"); RegCloseKey(raw);
            if (!SamePath(location, directory) || thumb.empty()) return false;
            for (auto const& path : { app, setup })
                if (PathExists(path) && (!VerifyEmbeddedSignature(path, thumb, CertSubjectExpected) || !VerifyWindowsTrust(path))) return false;
            if (PathExists(weTypeDll) &&
                (!VerifyEmbeddedSignature(weTypeDll, thumb, CertSubjectExpected) || !VerifyWindowsTrust(weTypeDll)))
                return false;
            return Identity.channel != RegionLens::native::AppChannel::Dev ||
                !PathExists(probe) ||
                (VerifyEmbeddedSignature(probe, thumb, CertSubjectExpected) && VerifyWindowsTrust(probe));
        }
        if (!PathExists(app) && !PathExists(setup) && !PathExists(probe) && !PathExists(weTypeDll)) return true;
        if (Identity.channel != RegionLens::native::AppChannel::Dev || PathExists(setup) ||
            PathExists(probe) || PathExists(weTypeDll)) return false;
        std::vector<BYTE> bytes;
        if (!ReadFileBytes(directory + L"\\dev-certificate-thumbprint.txt", bytes) || bytes.size() > 128) return false;
        std::wstring thumb(bytes.begin(), bytes.end());
        while (!thumb.empty() && iswspace(thumb.back())) thumb.pop_back();
        if (thumb.size() != 40 || !std::all_of(thumb.begin(), thumb.end(), [](wchar_t c) { return iswxdigit(c); })) return false;
        return VerifyEmbeddedSignature(app, thumb, L"RegionLens Development UIAccess") && VerifyWindowsTrust(app);
    }

    bool Install(PayloadInfo const& payload, InstallOptions const& options)
    {
        auto installDir = InstallDirectory();
        auto shortcut = StartMenuShortcut();
        auto desktopShortcut = DesktopShortcut();
        if (installDir.empty() || shortcut.empty()) return Fail(L"无法确定系统安装路径。请确认 Windows 配置正常。"), false;
        auto appPath = installDir + L"\\" + AppExeName;
        auto setupPath = installDir + L"\\" + SetupExeName;
        auto probePath = installDir + L"\\RegionLens-InputProbe.exe";
        auto weTypeDllPath = installDir + L"\\" + WeTypeDllName;
        if (SamePath(ModulePath(), setupPath))
            return Fail(L"不能从安装目录内直接覆盖正在运行的安装器。\n\n请使用下载目录中的 RegionLens-Setup.exe 更新或修复。"), false;
        if (RegionLensIsRunning(appPath) || (Identity.channel == RegionLens::native::AppChannel::Dev && RegionLensIsRunning(probePath)))
            return Fail(L"检测到已安装的 RegionLens 或其安全守护仍在运行。\n\n"
                L"请从托盘正常退出 RegionLens，等待片刻后重新运行安装程序。安装器不会强制终止输入恢复进程。"), false;
        if (WeTypeProbeIsLoaded(weTypeDllPath))
            return Fail(L"微信输入法仍加载区域镜兼容组件。请正常退出并重新打开微信输入法后重试安装。"), false;

        if (!ValidatePreviousInstall(installDir, appPath, setupPath, probePath, weTypeDllPath))
            return Fail(L"已有文件的目录、签名或安装记录不匹配。请先使用对应旧版本的卸载方式处理，安装器不会覆盖未知文件。"), false;
        HKEY oldRaw{};
        DWORD oldRoot{}, oldPublisher{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UninstallKey, 0, KEY_QUERY_VALUE, &oldRaw) == ERROR_SUCCESS)
        {
            std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> old(oldRaw, RegCloseKey);
            if (ReadRegistryString(oldRaw, L"CertThumbprint") == payload.certThumbprint) {
                oldRoot = ReadRegistryDword(oldRaw, L"OwnsRootCertificate");
                oldPublisher = ReadRegistryDword(oldRaw, L"OwnsPublisherCertificate");
            }
        }

        bool addedRoot{}, addedPublisher{};
        if (!AddCertificate(L"ROOT", payload.certificate, payload.certThumbprint, addedRoot) ||
            !AddCertificate(L"TrustedPublisher", payload.certificate, payload.certThumbprint, addedPublisher))
        {
            auto error = GetLastError();
            if (addedRoot) RemoveCertificate(L"ROOT", payload.certThumbprint);
            if (addedPublisher) RemoveCertificate(L"TrustedPublisher", payload.certThumbprint);
            return Fail(L"无法将 RegionLens 公钥证书安装到本机信任存储：\n" + FormatError(error)), false;
        }

        TempFile signedApp;
        if (!CreateTemporaryExe(payload.app, signedApp) || !VerifyWindowsTrust(signedApp.path) ||
            !VerifyWindowsTrust(ModulePath()))
        {
            if (addedRoot) RemoveCertificate(L"ROOT", payload.certThumbprint);
            if (addedPublisher) RemoveCertificate(L"TrustedPublisher", payload.certThumbprint);
            return Fail(L"Windows 无法验证 RegionLens 或安装器的完整数字签名。安装已取消。"), false;
        }

        if (!CreateDirectoryW(installDir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            if (addedRoot) RemoveCertificate(L"ROOT", payload.certThumbprint);
            if (addedPublisher) RemoveCertificate(L"TrustedPublisher", payload.certThumbprint);
            return Fail(L"无法创建安装目录：\n" + installDir + L"\n\n" + FormatError(GetLastError())), false;
        }
        auto suffix = L".new." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64());
        auto stagedApp = appPath + suffix;
        auto stagedSetup = setupPath + suffix;
        auto stagedProbe = probePath + suffix;
        auto stagedWeTypeDll = weTypeDllPath + suffix;
        if (PathExists(stagedApp) || PathExists(stagedSetup) || PathExists(stagedProbe) || PathExists(stagedWeTypeDll))
        {
            if (addedRoot) RemoveCertificate(L"ROOT", payload.certThumbprint);
            if (addedPublisher) RemoveCertificate(L"TrustedPublisher", payload.certThumbprint);
            return Fail(L"临时文件名冲突，未覆盖已有文件。请重试安装。"), false;
        }
        std::vector<BYTE> setupBytes;
        auto ok = ReadFileBytes(ModulePath(), setupBytes) && WriteFileBytes(stagedApp, payload.app) &&
            WriteFileBytes(stagedSetup, setupBytes) && VerifyWindowsTrust(stagedApp) &&
            VerifyWindowsTrust(stagedSetup);
        if (ok && !payload.probe.empty())
            ok = WriteFileBytes(stagedProbe, payload.probe) && VerifyWindowsTrust(stagedProbe);
        if (ok && !payload.weTypeDll.empty())
            ok = WriteFileBytes(stagedWeTypeDll, payload.weTypeDll) && VerifyWindowsTrust(stagedWeTypeDll);
        std::vector<RegionLens::setup::TransactionFile> files{
            { stagedApp, appPath, appPath + L".previous." + std::to_wstring(GetCurrentProcessId()) },
            { stagedSetup, setupPath, setupPath + L".previous." + std::to_wstring(GetCurrentProcessId()) } };
        if (!payload.probe.empty()) files.push_back({ stagedProbe, probePath,
            probePath + L".previous." + std::to_wstring(GetCurrentProcessId()) });
        if (!payload.weTypeDll.empty()) files.push_back({ stagedWeTypeDll, weTypeDllPath,
            weTypeDllPath + L".previous." + std::to_wstring(GetCurrentProcessId()) });
        NativeFileOps io;
        if (ok) ok = RegionLens::setup::CommitFiles(files, io);
        if (!ok && io.error) SetLastError(io.error);
        if (!ok)
        {
            auto error = GetLastError();
            DeleteFileW(stagedApp.c_str());
            DeleteFileW(stagedSetup.c_str());
            if (!payload.probe.empty()) DeleteFileW(stagedProbe.c_str());
            if (!payload.weTypeDll.empty()) DeleteFileW(stagedWeTypeDll.c_str());
            if (addedRoot) RemoveCertificate(L"ROOT", payload.certThumbprint);
            if (addedPublisher) RemoveCertificate(L"TrustedPublisher", payload.certThumbprint);
            auto detail = L"写入安装文件失败：\n" + FormatError(error);
            if (io.rollbackError) detail += L"\n\n部分原文件恢复失败，备份保留在安装目录的 .previous.* 文件中。请勿删除这些备份。恢复错误：\n" + FormatError(io.rollbackError);
            return Fail(detail), false;
        }

        auto ownsRoot = oldRoot != 0 || addedRoot;
        auto ownsPublisher = oldPublisher != 0 || addedPublisher;
        auto desktopShortcutReady = options.desktopShortcut
            ? (!desktopShortcut.empty() && CreateShortcut(desktopShortcut, appPath, installDir))
            : (desktopShortcut.empty() || DeleteFileW(desktopShortcut.c_str()) ||
                GetLastError() == ERROR_FILE_NOT_FOUND);
        if (!CreateShortcut(shortcut, appPath, installDir) || !desktopShortcutReady ||
            !ConfigureAutoStart(options.autoStart, appPath) ||
            !RegisterUninstaller(installDir, payload.certThumbprint, ownsRoot, ownsPublisher, options) ||
            !VerifyWindowsTrust(appPath) || !VerifyWindowsTrust(setupPath) ||
            (!payload.probe.empty() && !VerifyWindowsTrust(probePath)) ||
            (!payload.weTypeDll.empty() && !VerifyWindowsTrust(weTypeDllPath)))
        {
            return Fail(L"程序文件已写入，但开始菜单、卸载信息或最终签名验证失败。\n"
                L"请重新运行安装程序进行修复。"), false;
        }
        MessageBoxW(nullptr,
            (std::wstring(AppName) + L" " + AppVersion + L" 已成功安装。\n\n程序将从受保护的 Program Files 目录启动，完整鼠标映射可用。").c_str(),
            AppName, MB_OK | MB_ICONINFORMATION);
        LaunchInstalledApp(appPath);
        return true;
    }

    bool VerifyInstalledFile(std::wstring const& path, std::wstring const& thumbprint)
    {
        return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ||
            (VerifyEmbeddedSignature(path, thumbprint) && VerifyWindowsTrust(path));
    }

    bool Uninstall()
    {
        HKEY raw{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UninstallKey, 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS)
            return Fail(std::wstring(L"没有找到 ") + AppName + L" 的安装记录。", MB_ICONINFORMATION), false;
        std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)> key(raw, RegCloseKey);
        auto installDir = ReadRegistryString(raw, L"InstallLocation");
        auto thumbprint = ReadRegistryString(raw, L"CertThumbprint");
        auto ownsRoot = ReadRegistryDword(raw, L"OwnsRootCertificate") != 0;
        auto ownsPublisher = ReadRegistryDword(raw, L"OwnsPublisherCertificate") != 0;
        if (installDir.empty() || thumbprint.empty()) return Fail(L"安装记录不完整，卸载已取消。"), false;
        auto expectedDir = InstallDirectory();
        if (!SamePath(installDir, expectedDir))
            return Fail(L"安装记录中的目录不是预期的安全位置，卸载器不会删除该路径。"), false;
        auto appPath = installDir + L"\\" + AppExeName;
        auto setupPath = installDir + L"\\" + SetupExeName;
        auto probePath = installDir + L"\\RegionLens-InputProbe.exe";
        auto weTypeDllPath = installDir + L"\\" + WeTypeDllName;
        if (RegionLensIsRunning(appPath) || (Identity.channel == RegionLens::native::AppChannel::Dev && RegionLensIsRunning(probePath)))
            return Fail(L"RegionLens 或其安全守护仍在运行。\n\n请先从托盘正常退出，等待片刻后再次卸载。"), false;
        if (WeTypeProbeIsLoaded(weTypeDllPath))
            return Fail(L"微信输入法仍加载区域镜兼容组件。请正常退出并重新打开微信输入法后重试卸载。"), false;
        if (!VerifyInstalledFile(appPath, thumbprint) || !VerifyInstalledFile(setupPath, thumbprint) ||
            !VerifyInstalledFile(weTypeDllPath, thumbprint) ||
            (Identity.channel == RegionLens::native::AppChannel::Dev &&
                !VerifyInstalledFile(probePath, thumbprint)))
            return Fail(L"安装目录中的文件签名与安装记录不一致。为避免误删，卸载已停止。"), false;
        if (MessageBoxW(nullptr, (std::wstring(L"确定要卸载 ") + AppName + L" " + AppVersion + L" 吗？").c_str(), AppName,
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return false;

        key.reset();
        auto shortcut = StartMenuShortcut();
        if (!shortcut.empty()) DeleteFileW(shortcut.c_str());
        auto desktopShortcut = DesktopShortcut();
        if (!desktopShortcut.empty()) DeleteFileW(desktopShortcut.c_str());
        ConfigureAutoStart(false, appPath);
        if (PathExists(appPath) && !DeleteFileW(appPath.c_str()))
            return Fail(L"无法删除已验证的 RegionLens.exe：\n" + FormatError(GetLastError())), false;
        if (Identity.channel == RegionLens::native::AppChannel::Dev && PathExists(probePath) && !DeleteFileW(probePath.c_str()))
            return Fail(L"无法删除测试接收器，请关闭它后重试卸载。"), false;
        if (PathExists(weTypeDllPath) && !DeleteFileW(weTypeDllPath.c_str()))
            return Fail(L"无法删除微信输入法兼容组件，请退出微信输入法后重试卸载。"), false;
        if (ownsRoot) RemoveCertificate(L"ROOT", thumbprint);
        if (ownsPublisher) RemoveCertificate(L"TrustedPublisher", thumbprint);
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, UninstallKey);

        // The running uninstaller cannot delete itself. Windows removes these exact paths at reboot.
        MoveFileExW(setupPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MoveFileExW(installDir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        MessageBoxW(nullptr,
            L"RegionLens 已卸载。\n\n卸载器自身和空安装目录将在下次 Windows 启动时清除。",
            AppName, MB_OK | MB_ICONINFORMATION);
        return true;
    }

    int VerifyOnly(PayloadInfo const& payload)
    {
        std::wostringstream message;
        message << L"RegionLens 安装包验证通过。\n\n"
            << L"版本：" << AppVersion << L"\n"
            << L"架构：x64\n"
            << L"内嵌程序：" << payload.app.size() << L" 字节\n"
            << L"SHA-256：" << HexString(payload.appHash.data(), static_cast<DWORD>(payload.appHash.size())) << L"\n"
            << L"证书：" << payload.certSubject << L"\n"
            << L"指纹：" << payload.certThumbprint << L"\n\n"
            << L"此模式没有安装证书，也没有修改系统。";
        MessageBoxW(nullptr, message.str().c_str(), L"RegionLens 只读验证", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    std::vector<std::wstring> Arguments()
    {
        int count{};
        auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
        unique_local holder(raw);
        std::vector<std::wstring> result;
        for (int i = 1; raw && i < count; ++i) result.emplace_back(raw[i]);
        return result;
    }

    bool HasArgument(std::vector<std::wstring> const& arguments, wchar_t const* expected)
    {
        return std::any_of(arguments.begin(), arguments.end(), [expected](std::wstring const& value)
        {
            return _wcsicmp(value.c_str(), expected) == 0;
        });
    }

    InstallOptions InitialOptions()
    {
        InstallOptions options; HKEY key{};
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, UninstallKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return options;
        DWORD value{}, bytes = sizeof(value);
        if (RegGetValueW(key, nullptr, L"AutoStart", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS)
            options.autoStart = value != 0;
        else {
            HKEY run{};
            if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, AutoStartKey, 0, KEY_QUERY_VALUE, &run) == ERROR_SUCCESS) {
                options.autoStart = !ReadRegistryString(run, AutoStartValue).empty(); RegCloseKey(run);
            } else options.autoStart = false;
        }
        bytes = sizeof(value);
        if (RegGetValueW(key, nullptr, L"DesktopShortcut", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS)
            options.desktopShortcut = value != 0;
        else options.desktopShortcut = PathExists(DesktopShortcut());
        RegCloseKey(key); return options;
    }

    std::wstring InstallArguments(InstallOptions const& options)
    {
        std::wstring result = L"--install";
        result += options.autoStart ? L" --autostart" : L" --no-autostart";
        if (!options.desktopShortcut) result += L" --no-desktop-shortcut";
        return result;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    auto arguments = Arguments();
    auto verifyQuiet = !arguments.empty() && _wcsicmp(arguments[0].c_str(), L"--verify-quiet") == 0;
    auto uninstall = !arguments.empty() && (_wcsicmp(arguments[0].c_str(), L"/uninstall") == 0 ||
        _wcsicmp(arguments[0].c_str(), L"--uninstall") == 0);
    if (uninstall)
    {
        if (!IsElevated()) return RelaunchElevated(L"/uninstall");
        return Uninstall() ? 0 : 1;
    }
    if (!IsWindows11X64())
        return verifyQuiet ? 9 : (Fail(L"RegionLens 仅支持 x64 Windows 11（内部版本 22000 或更高）。"), 1);

    PayloadInfo payload;
    std::wstring validationError;
    int validationCode{};
    if (!LoadAndValidatePayload(payload, validationError, validationCode))
        return verifyQuiet ? validationCode : (Fail(validationError), validationCode);
    auto verify = !arguments.empty() && _wcsicmp(arguments[0].c_str(), L"--verify") == 0;
    if (verify) return VerifyOnly(payload);
    if (verifyQuiet) return 0;
    auto install = !arguments.empty() && _wcsicmp(arguments[0].c_str(), L"--install") == 0;
    if (install)
    {
        InstallOptions options;
        options.autoStart = HasArgument(arguments, L"--autostart") || (Identity.defaultAutoStart && !HasArgument(arguments, L"--no-autostart"));
        options.desktopShortcut = !HasArgument(arguments, L"--no-desktop-shortcut");
        if (!IsElevated()) return RelaunchElevated(InstallArguments(options));
        return Install(payload, options) ? 0 : 1;
    }

    auto options = InitialOptions();
    if (!ShowInstallOptions(payload, options)) return ERROR_CANCELLED;
    return RelaunchElevated(InstallArguments(options));
}
