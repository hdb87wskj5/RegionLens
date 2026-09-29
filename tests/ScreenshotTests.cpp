#include "pch.h"
#include "ScreenshotService.h"
#include "ScreenshotFeedback.h"
#include "LensChrome.h"
#include "LensQuality.h"
#include <wincodec.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace RegionLens::native;
using Microsoft::WRL::ComPtr;
namespace
{
    int failures{};
    void Check(bool value, char const* label) { if (!value) { ++failures; std::cerr << "FAILED screenshot: " << label << '\n'; } }
    void Require(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("HRESULT=" + std::to_string(uint32_t(hr))); }
    using Pixel = std::array<uint8_t, 4>; // BGRA, including asymmetric red/blue samples.
    struct FakeClipboard final : IScreenshotClipboard
    {
        unsigned calls{}, busyCount{};
        HRESULT failure{ S_OK };
        std::vector<uint8_t> received;
        HRESULT Publish(HWND, HGLOBAL& memory) noexcept override
        {
            ++calls;
            if (calls <= busyCount) return HRESULT_FROM_WIN32(ERROR_BUSY);
            if (FAILED(failure)) return failure;
            try {
                auto data = static_cast<uint8_t const*>(GlobalLock(memory));
                if (!data) return E_FAIL;
                auto header = reinterpret_cast<BITMAPINFOHEADER const*>(data);
                received.assign(data, data + sizeof(BITMAPINFOHEADER) + header->biSizeImage);
                GlobalUnlock(memory); GlobalFree(memory); memory = nullptr; return S_OK;
            } catch (...) { GlobalUnlock(memory); return E_OUTOFMEMORY; }
        }
    };
    struct Fixture
    {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> source;
        ComPtr<ID3D11ShaderResourceView> view;
        ScreenshotRenderer renderer;
        RenderExtent size{};
        Fixture()
        {
            Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
            Require(renderer.Initialize(device.Get()));
        }
        void Upload(std::vector<Pixel> const& pixels, RenderExtent extent)
        {
            if (!source || size != extent) {
                source.Reset(); view.Reset(); size = extent;
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = size.width; desc.Height = size.height;
                desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                Require(device->CreateTexture2D(&desc, nullptr, &source));
                Require(device->CreateShaderResourceView(source.Get(), nullptr, &view));
            }
            context->UpdateSubresource(source.Get(), 0, nullptr, pixels.data(), size.width * 4, 0);
        }
        ComPtr<ID3D11Texture2D> Capture(PixelRect crop, RenderExtent output, LensQualitySettings settings = {})
        {
            ComPtr<ID3D11Texture2D> staging;
            Require(renderer.Capture(device.Get(), context.Get(), view.Get(), crop, size, output, settings, { 42, 3 }, staging));
            return staging;
        }
        std::vector<Pixel> Read(ID3D11Texture2D* staging)
        {
            D3D11_TEXTURE2D_DESC desc{}; staging->GetDesc(&desc);
            std::vector<Pixel> pixels(size_t(desc.Width) * desc.Height);
            D3D11_MAPPED_SUBRESOURCE map{};
            Require(context->Map(staging, 0, D3D11_MAP_READ, 0, &map)); // Waiting is test-only.
            for (UINT y = 0; y < desc.Height; ++y)
                memcpy(pixels.data() + size_t(y) * desc.Width, static_cast<uint8_t*>(map.pData) + size_t(y) * map.RowPitch, size_t(desc.Width) * 4);
            context->Unmap(staging, 0); return pixels;
        }
    };
    std::span<uint8_t const> Bytes(std::vector<Pixel> const& pixels)
    { return { reinterpret_cast<uint8_t const*>(pixels.data()), pixels.size() * 4 }; }
    std::vector<Pixel> Decode(std::wstring const& file, RenderExtent expected)
    {
        ComPtr<IWICImagingFactory> factory;
        Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
        ComPtr<IWICBitmapDecoder> decoder;
        Require(factory->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &decoder));
        ComPtr<IWICBitmapFrameDecode> frame; Require(decoder->GetFrame(0, &frame));
        UINT width{}, height{}; Require(frame->GetSize(&width, &height));
        Check(width == expected.width && height == expected.height, "PNG records physical output pixels, not source size or DIPs");
        ComPtr<IWICFormatConverter> converter; Require(factory->CreateFormatConverter(&converter));
        Require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
        std::vector<Pixel> pixels(size_t(width) * height);
        Require(converter->CopyPixels(nullptr, width * 4, UINT(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())));
        return pixels;
    }
    bool Near(std::vector<Pixel> const& left, std::vector<Pixel> const& right, int tolerance = 1)
    {
        if (left.size() != right.size()) return false;
        for (size_t i = 0; i < left.size(); ++i) for (int c = 0; c < 4; ++c)
            if (abs(int(left[i][c]) - int(right[i][c])) > tolerance) return false;
        return true;
    }
}

int RunScreenshotTests()
{
    {
        ScreenshotFeedback first, second;
        Check(!first.Active(0) && !first.Expire(0), "screenshot button starts without success feedback");
        first.Start(0);
        Check(first.Active(0) && first.Active(399) && !second.Active(399), "success pulse lasts 400ms and is per-region");
        Check(!first.Active(400) && first.Expire(400) && !first.Expire(401), "pulse expires once at its deadline");
        first.Start(1000); first.Start(1300);
        Check(!first.Expire(1400) && first.Active(1699), "old queued timer cannot cancel a repeated screenshot pulse");
        Check(first.Expire(1700), "repeated screenshot extends only its own deadline");
        first.Start(3000); first.Reset();
        Check(!first.Active(3001) && !first.Expire(3800), "hiding or closing clears success without stale timer work");
        first.Start(UINT64_MAX - 100);
        Check(first.Active(50) && first.Expire(700), "pulse uses monotonic unsigned elapsed time");
    }
    auto apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::filesystem::path temporary;
    std::filesystem::path temporaryParent;
    bool ownsTemporary{};
    std::wstring registry;
    try {
        Check(ValidScreenshotExtent({ 3840, 2160 }) && !ValidScreenshotExtent({ 0, 10 }) &&
            !ValidScreenshotExtent({ 16384, 16384 }) && !ValidScreenshotExtent({ UINT_MAX, UINT_MAX }), "allocation limits reject zero and oversized captures");
        wchar_t temp[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, temp)) throw std::runtime_error("temp path");
        auto unique = NewScreenshotFileName();
        if (unique.empty()) throw std::runtime_error("unique test directory");
        temporaryParent = std::filesystem::weakly_canonical(temp);
        temporary = std::filesystem::path(temp) / (L"RegionLens-ScreenshotTests-" + unique);
        ownsTemporary = std::filesystem::create_directory(temporary);
        if (!ownsTemporary) throw std::runtime_error("test directory already exists");

        // Persist only under a unique disposable test key, never the user's real settings.
        registry = L"Software\\RegionLens.Tests\\" + unique;
        auto identity = DevIdentity; identity.registry = registry.c_str();
        auto defaultDirectory = DefaultScreenshotDirectory(identity);
        Check(!defaultDirectory.empty() && std::filesystem::path(defaultDirectory).filename() == L"RegionLens-Dev", "default is Pictures/channel, not Program Files");
        Check(DefaultScreenshotDirectory(StableIdentity) != defaultDirectory, "channels have separate picture folders");
        Check(LoadScreenshotDirectory(identity) == defaultDirectory, "absent setting uses Pictures default");
        auto custom = (temporary / L"中文 截图").wstring();
        Require(SaveScreenshotDirectory(identity, custom));
        Check(LoadScreenshotDirectory(identity) == custom, "Unicode custom folder persists");
        Check(FAILED(SaveScreenshotDirectory(identity, L"relative\\path")) && LoadScreenshotDirectory(identity) == custom, "invalid setting retains previous folder");
        Require(SaveScreenshotDirectory(identity, {}));
        Check(LoadScreenshotDirectory(identity) == defaultDirectory, "restore default does not remove files");
        Check(NewScreenshotFileName() != NewScreenshotFileName(), "rapid screenshots cannot overwrite one another");
        HKEY testKey{};
        auto screenshotKey = registry + L"\\Screenshots";
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, screenshotKey.c_str(), 0, KEY_SET_VALUE, &testKey) == ERROR_SUCCESS, "open isolated test key");
        if (testKey) {
            DWORD corrupt = 42;
            RegSetValueExW(testKey, L"Directory", 0, REG_DWORD, reinterpret_cast<BYTE*>(&corrupt), sizeof(corrupt));
            RegCloseKey(testKey);
            Check(LoadScreenshotDirectory(identity) == defaultDirectory, "corrupt storage type falls back to channel default");
        }

        Fixture f;
        std::vector<Pixel> pixels(12 * 8);
        for (UINT y = 0; y < 8; ++y) for (UINT x = 0; x < 12; ++x)
            pixels[y * 12 + x] = { uint8_t(10 + 11 * x), uint8_t(15 + 17 * y), uint8_t(205 - 9 * x - y), 255 };
        f.Upload(pixels, { 12, 8 });
        auto original = f.Read(f.Capture({ 0, 0, 12, 8 }, { 12, 8 }).Get());
        Check(Near(original, pixels, 0), "1:1 clean pass preserves colors/orientation and never draws controls");
        PixelRect crop{ 2, 1, 6, 4 };
        for (auto output : { RenderExtent{ 10, 7 }, RenderExtent{ 24, 12 }, RenderExtent{ 3, 9 } }) {
            auto smooth = f.Read(f.Capture(crop, output).Get());
            std::vector<Pixel> expected(size_t(output.width) * output.height);
            for (UINT y = 0; y < output.height; ++y) for (UINT x = 0; x < output.width; ++x) {
                auto sx = std::clamp(crop.x + (x + .5) * crop.width / output.width - .5, double(crop.x), double(crop.Right() - 1));
                auto sy = std::clamp(crop.y + (y + .5) * crop.height / output.height - .5, double(crop.y), double(crop.Bottom() - 1));
                int x0 = int(sx), y0 = int(sy), x1 = std::min(x0 + 1, crop.Right() - 1), y1 = std::min(y0 + 1, crop.Bottom() - 1);
                for (int c = 0; c < 4; ++c)
                    expected[size_t(y) * output.width + x][c] = uint8_t(std::lround(
                        (pixels[y0 * 12 + x0][c] * (1 - (sx - x0)) + pixels[y0 * 12 + x1][c] * (sx - x0)) * (1 - (sy - y0)) +
                        (pixels[y1 * 12 + x0][c] * (1 - (sx - x0)) + pixels[y1 * 12 + x1][c] * (sx - x0)) * (sy - y0)));
            }
            Check(Near(smooth, expected), "bilinear displayed screenshot supports nonuniform scales, crop offsets and shrinking");
        }
        for (auto sharp : { LensSharpness::Off, LensSharpness::Low, LensSharpness::Medium, LensSharpness::High }) {
            RenderExtent output{ 24, 16 };
            auto actual = f.Read(f.Capture(crop, output, { LensQualityMode::Clear, sharp }).Get());
            QualityRenderer quality;
            Require(quality.Prepare(f.device.Get(), f.context.Get(), f.view.Get(), { { 42, 3 }, crop, output }, sharp));
            ComPtr<ID3D11Resource> resource; quality.View()->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture; Require(resource.As(&texture));
            D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
            desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.BindFlags = 0;
            ComPtr<ID3D11Texture2D> staging; Require(f.device->CreateTexture2D(&desc, nullptr, &staging));
            f.context->CopyResource(staging.Get(), texture.Get());
            auto expected = f.Read(staging.Get());
            for (auto& p : expected) std::swap(p[0], p[2]); // Quality target stores RGBA, PNG/clipboard use BGRA.
            Check(Near(actual, expected), "screenshot includes the production Catmull-Rom/CAS result");
        }
        auto toggledOn = QualityForLevel(LensSharpness::Medium);
        Check(Near(f.Read(f.Capture(crop, {24,16}, toggledOn).Get()),
            f.Read(f.Capture(crop, {24,16}, {LensQualityMode::Clear,LensSharpness::Medium}).Get()),0),
            "globally sharpened screenshot uses the selected medium setting");
        Check(Near(f.Read(f.Capture(crop, {24,16}, QualityForLevel(LensSharpness::Off)).Get()),
            f.Read(f.Capture(crop, {24,16}).Get()),0), "quality off screenshot is the unsharpened original at the same size");
        auto frozen = f.Capture(crop, { 6, 4 });
        auto before = f.Read(frozen.Get());
        std::vector<Pixel> changed(12 * 8, Pixel{ 40, 85, 180, 255 });
        f.Upload(changed, { 12, 8 });
        Check(Near(f.Read(frozen.Get()), before, 0), "capture request freezes GPU content despite subsequent frame reuse");
        auto flat = f.Read(f.Capture({ 3, 3, 1, 1 }, { 200, 128 }, { LensQualityMode::Clear, LensSharpness::High }).Get());
        Check(std::all_of(flat.begin(), flat.end(), [&](Pixel p) { return p == changed[0]; }), "single pixel crop fills output without pointer, frame, camera icon or neighbors");
        {
            auto fullScreen = f.Read(f.Capture({ 0, 0, 12, 8 }, { 3840, 2160 }).Get());
            auto fullScreenFile = (temporary / L"4k.png").wstring();
            Require(WriteScreenshotPng(fullScreenFile, { 3840, 2160 }, Bytes(fullScreen)));
            Check(Decode(fullScreenFile, { 3840, 2160 }) == fullScreen &&
                std::all_of(fullScreen.begin(), fullScreen.end(), [&](Pixel p) { return p == changed[0]; }),
                "4K fullscreen output keeps exact dimensions, color and PNG payload");
        }

        std::vector<Pixel> samplePixels{ { 1, 25, 241, 255 }, { 23, 100, 9, 255 }, { 110, 22, 3, 255 }, { 20, 6, 199, 255 } };
        auto file = (temporary / L"中文 截图" / L"test.png").wstring();
        Require(WriteScreenshotPng(file, { 2, 2 }, Bytes(samplePixels)));
        Check(Decode(file, { 2, 2 }) == samplePixels, "WIC PNG round trip is lossless BGRA and top-down");
        auto other = samplePixels; other[0] = { 255, 0, 0, 255 };
        Check(FAILED(WriteScreenshotPng(file, { 2, 2 }, Bytes(other))) && Decode(file, { 2, 2 }) == samplePixels,
            "file collision preserves the existing screenshot");
        Check(!std::filesystem::exists(file + L".tmp"), "failed commit removes only its own temporary file");

        HGLOBAL dib{}; Require(BuildScreenshotDib({ 2, 2 }, Bytes(samplePixels), dib));
        auto header = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib));
        Check(header->biWidth == 2 && header->biHeight == -2 && header->biBitCount == 32 && header->biSizeImage == 16,
            "clipboard DIB has correct top-down dimensions and pixel layout");
        GlobalUnlock(dib);
        FakeClipboard retry; retry.busyCount = 2;
        ScreenshotClipboardDelivery delivery;
        delivery.Tick(0, nullptr, dib, retry); delivery.Tick(1, nullptr, dib, retry);
        Check(!delivery.complete && retry.calls == 1 && dib, "busy clipboard leaves data intact and retries without blocking");
        delivery.Tick(50, nullptr, dib, retry); delivery.Tick(100, nullptr, dib, retry); delivery.Tick(200, nullptr, dib, retry);
        Check(delivery.complete && SUCCEEDED(delivery.result) && retry.calls == 3 && !dib, "clipboard succeeds once and transfers ownership exactly once");
        Require(BuildScreenshotDib({ 2, 2 }, Bytes(samplePixels), dib));
        FakeClipboard busy; busy.busyCount = 100; delivery = {};
        for (uint64_t time = 0; time < 1000; time += 50) delivery.Tick(time, nullptr, dib, busy);
        Check(delivery.complete && FAILED(delivery.result) && busy.calls == 10 && dib, "persistent clipboard contention is bounded and reported");
        GlobalFree(dib);

        for (bool denyClipboard : { false, true }) {
            FakeClipboard backend; if (denyClipboard) backend.failure = E_ACCESSDENIED;
            ScreenshotService service(backend);
            auto capture = f.Capture({ 0, 0, 12, 8 }, { 120, 80 });
            Require(service.Start(capture, temporary.wstring()));
            Check(service.Start(capture, temporary.wstring()) == HRESULT_FROM_WIN32(ERROR_BUSY), "in-flight work is bounded to one screenshot");
            std::optional<ScreenshotResult> result;
            auto deadline = GetTickCount64() + 10000;
            while (!result && GetTickCount64() < deadline) { result = service.Poll(f.context.Get(), nullptr); Sleep(1); }
            Check(result.has_value(), "asynchronous production readback/encoding finishes");
            if (result) {
                Check(SUCCEEDED(result->file) && (denyClipboard ? FAILED(result->clipboard) : SUCCEEDED(result->clipboard)),
                    "PNG saving succeeds independently of clipboard failure");
                Check(Decode(result->path, { 120, 80 }) == std::vector<Pixel>(120 * 80, changed[0]), "service saves the frozen displayed result");
                if (!denyClipboard) {
                    auto decoded = Decode(result->path, { 120, 80 });
                    auto bytes = Bytes(decoded);
                    Check(backend.received.size() == sizeof(BITMAPINFOHEADER) + bytes.size() &&
                        memcmp(backend.received.data() + sizeof(BITMAPINFOHEADER), bytes.data(), bytes.size()) == 0,
                        "clipboard and PNG contain identical pixels");
                }
            }
            Check(!service.Busy(), "completion releases snapshot and permits next request");
        }
        // A real encoder failure must not prevent publishing the independently prepared DIB.
        {
            FakeClipboard backend; ScreenshotService service(backend);
            Require(service.Start(f.Capture({ 0, 0, 12, 8 }, { 12, 8 }), file)); // Parent is an existing PNG, not a directory.
            std::optional<ScreenshotResult> result; auto deadline = GetTickCount64() + 10000;
            while (!result && GetTickCount64() < deadline) { result = service.Poll(f.context.Get(), nullptr); Sleep(1); }
            Check(result && SUCCEEDED(result->clipboard) && FAILED(result->file), "unwritable save location does not lose clipboard image");
        }
        MappingUiPauseState pause;
        pause.Settings(true); pause.Selection(true); pause.Settings(false);
        Check(pause.Paused(), "folder picker closing cannot resume an active selection");
        pause.Settings(true); pause.Selection(false);
        Check(pause.Paused(), "selection closing cannot resume an active folder picker");
        pause.Settings(false); Check(!pause.Paused(), "settings pause clears independently");
    } catch (std::exception const& error) { ++failures; std::cerr << "Screenshot tests: " << error.what() << '\n'; }
    if (!registry.empty()) RegDeleteTreeW(HKEY_CURRENT_USER, registry.c_str());
    // Only the fresh unique test child of the system temp folder; never touch Pictures or app settings.
    if (ownsTemporary && !temporary.empty() && temporary.filename().wstring().starts_with(L"RegionLens-ScreenshotTests-") &&
        std::filesystem::weakly_canonical(temporary).parent_path() == temporaryParent) {
        std::error_code ignored; std::filesystem::remove_all(temporary, ignored);
    }
    if (SUCCEEDED(apartment)) CoUninitialize();
    if (!failures) std::cout << "Screenshot GPU/PNG/fake-clipboard/settings tests passed.\n";
    return failures;
}
