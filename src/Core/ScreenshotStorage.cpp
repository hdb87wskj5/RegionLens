#include "pch.h"
#include "ScreenshotStorage.h"
#include <shlobj.h>
#include <wincodec.h>
#include <filesystem>
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;
namespace RegionLens::native
{
    namespace
    {
        HRESULT LastFailure() { auto error = GetLastError(); return HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE); }
        bool ValidDirectory(std::wstring const& path)
        { return !path.empty() && path.find(L'\0') == std::wstring::npos && std::filesystem::path(path).is_absolute(); }
        std::wstring SettingsKey(AppIdentity const& identity) { return std::wstring(identity.registry) + L"\\Screenshots"; }
        class NativeClipboard final : public IScreenshotClipboard
        {
        public:
            HRESULT Publish(HWND owner, HGLOBAL& memory) noexcept override
            {
                if (!owner || !memory) return E_INVALIDARG;
                if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(ERROR_BUSY);
                HRESULT hr = S_OK;
                if (!EmptyClipboard() || !SetClipboardData(CF_DIB, memory)) hr = LastFailure();
                else memory = nullptr;
                CloseClipboard();
                return hr;
            }
        };
    }

    std::wstring DefaultScreenshotDirectory(AppIdentity const& identity)
    {
        PWSTR folder = nullptr;
        auto hr = SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DONT_VERIFY, nullptr, &folder);
        if (FAILED(hr)) return {}; // Do not fall back silently to a different location.
        std::wstring directory = (std::filesystem::path(folder) / identity.folder).wstring();
        CoTaskMemFree(folder);
        return directory;
    }

    std::wstring LoadScreenshotDirectory(AppIdentity const& identity)
    {
        DWORD bytes{};
        auto key = SettingsKey(identity);
        if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Directory", RRF_RT_REG_SZ, nullptr, nullptr, &bytes) == ERROR_SUCCESS &&
            bytes >= sizeof(wchar_t) && bytes <= 65536) {
            std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1);
            if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"Directory", RRF_RT_REG_SZ, nullptr, value.data(), &bytes) == ERROR_SUCCESS) {
                std::wstring path(value.data());
                if (ValidDirectory(path)) return path;
            }
        }
        return DefaultScreenshotDirectory(identity);
    }

    HRESULT SaveScreenshotDirectory(AppIdentity const& identity, std::wstring const& directory)
    {
        if (!directory.empty() && !ValidDirectory(directory)) return E_INVALIDARG;
        auto key = SettingsKey(identity);
        HKEY registry{};
        auto result = RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &registry, nullptr);
        if (result != ERROR_SUCCESS) return HRESULT_FROM_WIN32(result);
        if (directory.empty()) result = RegDeleteValueW(registry, L"Directory");
        else result = RegSetValueExW(registry, L"Directory", 0, REG_SZ, reinterpret_cast<BYTE const*>(directory.c_str()),
            DWORD((directory.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(registry);
        return result == ERROR_FILE_NOT_FOUND ? S_OK : HRESULT_FROM_WIN32(result);
    }

    HRESULT EnsureScreenshotDirectory(std::wstring const& directory) noexcept
    {
        try {
            if (!ValidDirectory(directory)) return E_INVALIDARG;
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) return HRESULT_FROM_WIN32(error.value());
            if (!std::filesystem::is_directory(directory, error)) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            return S_OK;
        } catch (...) { return E_FAIL; }
    }

    std::wstring NewScreenshotFileName()
    {
        SYSTEMTIME time{}; GetLocalTime(&time);
        GUID id{};
        if (FAILED(CoCreateGuid(&id))) return {};
        wchar_t guid[40]{}; StringFromGUID2(id, guid, 40);
        wchar_t prefix[64]{};
        swprintf_s(prefix, L"RegionLens_%04u%02u%02u_%02u%02u%02u_%03u_", time.wYear, time.wMonth,
            time.wDay, time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
        return std::wstring(prefix) + std::wstring(guid + 1, 36) + L".png";
    }

    HRESULT WriteScreenshotPng(std::wstring const& file, RenderExtent size, std::span<uint8_t const> bgra) noexcept
    {
        std::wstring temporary;
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool created = false;
        auto cleanup = [&] {
            if (handle != INVALID_HANDLE_VALUE) { CloseHandle(handle); handle = INVALID_HANDLE_VALUE; }
            if (created) DeleteFileW(temporary.c_str()); // Only the exact file created by this invocation.
        };
        try {
            if (!ValidDirectory(file) || !ValidScreenshotExtent(size) || bgra.size() != uint64_t(size.width) * size.height * 4) return E_INVALIDARG;
            ComPtr<IWICImagingFactory> factory;
            HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
            if (FAILED(hr)) return hr;
            ComPtr<IStream> stream;
            if (FAILED(hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return hr;
            ComPtr<IWICBitmapEncoder> encoder;
            if (FAILED(hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))) return hr;
            if (FAILED(hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return hr;
            ComPtr<IWICBitmapFrameEncode> frame;
            if (FAILED(hr = encoder->CreateNewFrame(&frame, nullptr))) return hr;
            if (FAILED(hr = frame->Initialize(nullptr))) return hr;
            if (FAILED(hr = frame->SetSize(size.width, size.height))) return hr;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (FAILED(hr = frame->SetPixelFormat(&format))) return hr;
            if (format != GUID_WICPixelFormat32bppBGRA) return WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
            if (FAILED(hr = frame->WritePixels(size.height, size.width * 4, UINT(bgra.size()), const_cast<BYTE*>(bgra.data())))) return hr;
            if (FAILED(hr = frame->Commit()) || FAILED(hr = encoder->Commit())) return hr;
            if (FAILED(hr = EnsureScreenshotDirectory(std::filesystem::path(file).parent_path().wstring()))) return hr;
            temporary = file + L".tmp";
            handle = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE) return LastFailure();
            created = true;
            LARGE_INTEGER zero{};
            if (SUCCEEDED(hr = stream->Seek(zero, STREAM_SEEK_SET, nullptr))) {
                std::array<BYTE, 65536> block{};
                for (;;) {
                    ULONG read{}; hr = stream->Read(block.data(), ULONG(block.size()), &read);
                    if (FAILED(hr) || !read) break;
                    DWORD written{};
                    if (!WriteFile(handle, block.data(), read, &written, nullptr) || written != read) { hr = LastFailure(); break; }
                }
            }
            if (SUCCEEDED(hr) && !FlushFileBuffers(handle)) hr = LastFailure();
            CloseHandle(handle); handle = INVALID_HANDLE_VALUE;
            if (SUCCEEDED(hr)) {
                // No REPLACE_EXISTING: even a name collision must not destroy a user's image.
                if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH)) hr = LastFailure();
                else created = false;
            }
            cleanup(); return hr;
        } catch (...) { cleanup(); return E_OUTOFMEMORY; }
    }

    HRESULT BuildScreenshotDib(RenderExtent size, std::span<uint8_t const> bgra, HGLOBAL& memory) noexcept
    {
        memory = nullptr;
        if (!ValidScreenshotExtent(size) || bgra.size() != uint64_t(size.width) * size.height * 4) return E_INVALIDARG;
        auto allocation = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bgra.size());
        if (!allocation) return E_OUTOFMEMORY;
        auto target = static_cast<uint8_t*>(GlobalLock(allocation));
        if (!target) { GlobalFree(allocation); return E_OUTOFMEMORY; }
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header); header.biWidth = LONG(size.width); header.biHeight = -LONG(size.height);
        header.biPlanes = 1; header.biBitCount = 32; header.biCompression = BI_RGB; header.biSizeImage = DWORD(bgra.size());
        memcpy(target, &header, sizeof(header)); memcpy(target + sizeof(header), bgra.data(), bgra.size());
        GlobalUnlock(allocation); memory = allocation; return S_OK;
    }

    IScreenshotClipboard& SystemScreenshotClipboard() noexcept { static NativeClipboard backend; return backend; }

    void ScreenshotClipboardDelivery::Tick(uint64_t now, HWND owner, HGLOBAL& memory, IScreenshotClipboard& backend) noexcept
    {
        if (complete || now < nextAttempt) return;
        result = backend.Publish(owner, memory); ++attempts;
        complete = result != HRESULT_FROM_WIN32(ERROR_BUSY) || attempts >= 10;
        nextAttempt = now + 50;
    }
}
