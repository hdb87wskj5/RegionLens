#include "pch.h"
#include "D3DDevice.h"
#include "SwapChainRenderer.h"
#include "LensChrome.h"
#include "RegionTransform.h"
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
using namespace RegionLens::native;
using namespace winrt::Windows::Graphics::Capture;

// Test-only access. Deliberately recreate the rejected 2bd43de transforms to
// prove that the FINAL composed-pixel validator detects that regression.
namespace RegionLens::native
{
    struct CompositionPresentationTestAccess
    {
        static IDXGISwapChain2* Chain(SwapChainRenderer& r) { return r.m_compositionSwapChain.Get(); }
        static RenderExtent Viewport(SwapChainRenderer const& r) { return {r.m_width, r.m_height}; }
        static RenderExtent Storage(SwapChainRenderer const& r) { return r.m_bufferStorage; }
        static bool Pending(SwapChainRenderer const& r) { return r.m_resizeHandoff.Pending(); }
        static uint64_t QualityBytes(SwapChainRenderer const& r) { return r.m_quality.ResourceBytes(); }
        static bool ProgrammaticResize(SwapChainRenderer const& r) { return r.m_programmaticResize; }
        static void LegacyScale(SwapChainRenderer& r, bool enabled)
        {
            RECT client{}; GetClientRect(r.m_window, &client);
            DXGI_MATRIX_3X2_F matrix{1,0,0,1,0,0};
            if (enabled) {
                matrix._11 = 1.f / r.m_width; matrix._22 = 1.f / r.m_height;
                D2D_MATRIX_3X2_F visual{float(client.right),0,0,float(client.bottom),0,0};
                winrt::check_hresult(r.m_compositionVisual->SetTransform(visual));
                winrt::check_hresult(r.m_compositionDevice->Commit());
            } else {
                r.m_transformedClient = {};
                if (!r.StretchToClient()) throw std::runtime_error("restore composition transform failed");
            }
            winrt::check_hresult(r.m_compositionSwapChain->SetMatrixTransform(&matrix));
        }
    };
}

namespace
{
    LRESULT CALLBACK PresentationTestProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (message == WM_NCCALCSIZE) return 0;
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
    struct TestWindow
    {
        HWND value{};
        ~TestWindow() { if (value) DestroyWindow(value); }
    };
    struct CaptureScope
    {
        Direct3D11CaptureFramePool pool{nullptr};
        GraphicsCaptureSession session{nullptr};
        ~CaptureScope() { if (session) session.Close(); if (pool) pool.Close(); }
    };
    std::vector<uint32_t> ReadPixels(D3DDevice& device, ID3D11Texture2D* texture, UINT width, UINT height)
    {
        D3D11_TEXTURE2D_DESC desc{}; texture->GetDesc(&desc);
        if (width > desc.Width || height > desc.Height) throw std::runtime_error("capture texture is smaller than content");
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = desc.MiscFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        winrt::check_hresult(device.Device()->CreateTexture2D(&desc, nullptr, &staging));
        device.Context()->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        winrt::check_hresult(device.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        std::vector<uint32_t> pixels(size_t(width) * height);
        for (UINT y = 0; y < height; ++y)
            memcpy(pixels.data() + size_t(y) * width, static_cast<uint8_t const*>(mapped.pData) + size_t(y) * mapped.RowPitch,
                size_t(width) * sizeof(uint32_t));
        device.Context()->Unmap(staging.Get(), 0);
        return pixels;
    }

    constexpr std::array<uint32_t,4> Quadrants{0xff403030, 0xff306030, 0xff303050, 0xff605030};
    bool NearColor(uint32_t actual, uint32_t expected, int tolerance = 3)
    {
        for (unsigned shift : {0u,8u,16u})
            if (std::abs(int((actual >> shift) & 255) - int((expected >> shift) & 255)) > tolerance) return false;
        return true;
    }
    struct PixelCheck { bool borders{true}, buttons{true}, content{true}, cursor{true}, letterbox{true};
        bool All() const { return borders && buttons && content && cursor && letterbox; } };
    PixelCheck ValidatePresentation(std::vector<uint32_t> const& pixels, RenderExtent size,
        bool chrome, bool fullscreen, bool cursor, uint32_t tint, bool feedback,
        std::optional<PixelRect> contentRect = std::nullopt)
    {
        PixelCheck result;
        auto pixel = [&](int x, int y) { return pixels[size_t(y) * size.width + x]; };
        RECT client{0,0,LONG(size.width),LONG(size.height)};
        constexpr uint32_t accent = 0xff0078d6, dark = 0xff1f2429, blue = 0xff0061b8;
        for (int fraction : {1,2,3}) {
            int x = int(size.width) * fraction / 4, y = int(size.height) * fraction / 4;
            for (auto point : {POINT{0,y}, POINT{LONG(size.width)-1,y}, POINT{x,0}, POINT{x,LONG(size.height)-1}})
                result.borders &= (chrome && !fullscreen) ? NearColor(pixel(point.x,point.y),accent) :
                    !NearColor(pixel(point.x,point.y),accent);
        }
        for (int id : ChromeButtonIds) {
            auto center = ChromeButtonCenterFor(id,client);
            bool enabled = id==PinButtonId || id==InputMappingButtonId || id==PointerSpeedButtonId ||
                (id==FullscreenButtonId && fullscreen);
            if (chrome) {
                auto background = feedback && id==ScreenshotButtonId ? accent : enabled?blue:dark;
                result.buttons &= NearColor(pixel(center.x,center.y+9),background) &&
                    HitTestChromeButton(center,client)==id;
                int glyphPixels{};
                for (int dy=-8;dy<=8;++dy) for (int dx=-8;dx<=8;++dx)
                    glyphPixels += NearColor(pixel(center.x+dx,center.y+dy),0xffffffff,12);
                result.buttons &= glyphPixels >= 6;
            } else result.buttons &= !NearColor(pixel(center.x,center.y+9),dark) &&
                !NearColor(pixel(center.x,center.y+9),blue);
        }
        PixelRect content = contentRect.value_or(PixelRect{0,0,int32_t(size.width),int32_t(size.height)});
        // Distinct source quadrants reveal unintended zoom/cropping that a
        // constant-color image cannot detect. Samples follow the actual content
        // rectangle rather than the surrounding fullscreen bars.
        for (int row=0;row<2;++row) for (int col=0;col<2;++col) {
            int x = content.x + content.width*(1+2*col)/4;
            int y = content.y + content.height*(1+2*row)/4;
            result.content &= NearColor(pixel(x,y),Quadrants[row*2+col] ^ tint);
        }
        if (contentRect) {
            constexpr uint32_t black = 0xff000000;
            if (content.x > 2)
                result.letterbox &= NearColor(pixel(content.x/2, content.y+content.height/2), black);
            if (content.Right()+2 < int32_t(size.width))
                result.letterbox &= NearColor(pixel((content.Right()+int32_t(size.width))/2,
                    content.y+content.height/2), black);
            if (content.y > 42)
                result.letterbox &= NearColor(pixel(content.x+content.width/2, content.y/2), black);
            if (content.Bottom()+2 < int32_t(size.height))
                result.letterbox &= NearColor(pixel(content.x+content.width/2,
                    (content.Bottom()+int32_t(size.height))/2), black);
        }
        // Look for a blank/white block in the interior, excluding chrome and
        // cursor glyphs. No source pixel is white in this synthetic image.
        size_t white{};
        for (UINT y=40;y+3<size.height;++y) for (UINT x=3;x+3<size.width;++x)
            white += NearColor(pixel(int(x),int(y)),0xffffffff,6);
        result.content &= white < 1000;
        if (cursor) {
            int cx=int(size.width)*5/8,cy=int(size.height)/2;
            int visible{};
            for(int y=cy;y<std::min(cy+32,int(size.height));++y)
                for(int x=cx;x<std::min(cx+32,int(size.width));++x)
                    visible += NearColor(pixel(x,y),0xffffffff,12) || NearColor(pixel(x,y),0xff000000,12);
            result.cursor = visible >= 8;
        }
        return result;
    }
}

int RunCompositionPresentationTests()
{
    int failures{};
    auto check = [&](bool ok, char const* message) {
        if (!ok) { ++failures; std::cerr << "FAILED composition presentation: " << message << '\n'; }
    };
    auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto previousDpi = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        auto device = std::make_shared<D3DDevice>();
        if (!device->Initialize()) throw std::runtime_error("D3D device unavailable");
        WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = PresentationTestProc; wc.lpszClassName = L"RegionLens.Tests.Presentation";
        RegisterClassW(&wc);
        // A no-activate window wholly outside the virtual desktop. Capture only
        // this owned test window, never a display, source app, or user's pixels.
        int testX = GetSystemMetrics(SM_XVIRTUALSCREEN) - D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION - 1024;
        int testY = GetSystemMetrics(SM_YVIRTUALSCREEN) - D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION - 1024;
        TestWindow window{CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            wc.lpszClassName, L"RegionLens offscreen presentation test", WS_POPUP,
            testX, testY, 300, 200, nullptr, nullptr, wc.hInstance, nullptr)};
        if (!window.value || !SetLayeredWindowAttributes(window.value, 0, 255, LWA_ALPHA))
            throw std::runtime_error("offscreen test HWND unavailable");
        if (MonitorFromWindow(window.value,MONITOR_DEFAULTTONULL))
            throw std::runtime_error("refuse to show a test HWND on a real display");
        SwapChainRenderer renderer;
        if (!renderer.Initialize(window.value, device, true)) throw std::runtime_error("renderer unavailable");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 32;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::array<uint32_t,32*32> sourcePixels{};
        for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
            sourcePixels[y*32+x]=Quadrants[(y>=16?2:0)+(x>=16?1:0)];
        D3D11_SUBRESOURCE_DATA data{sourcePixels.data(), 32*sizeof(uint32_t), 0};
        ComPtr<ID3D11Texture2D> texture; ComPtr<ID3D11ShaderResourceView> view;
        winrt::check_hresult(device->Device()->CreateTexture2D(&desc, &data, &texture));
        winrt::check_hresult(device->Device()->CreateShaderResourceView(texture.Get(), nullptr, &view));
        bool chrome=true, fullscreen=false, sharpen=false, cursor=false, feedback=false;
        std::optional<PixelRect> contentRect;
        uint32_t tint{}; uint64_t revision=1;
        auto draw = [&] {
            RECT client{}; GetClientRect(window.value,&client);
            CursorSnapshot pointer{}; pointer.visible=cursor; pointer.shape=LoadCursorW(nullptr,IDC_ARROW);
            POINT origin{}; ClientToScreen(window.value,&origin);
            pointer.position={origin.x+client.right*5/8,origin.y+client.bottom/2};
            LensQualitySettings quality; quality.mode=sharpen?LensQualityMode::Clear:LensQualityMode::Smooth;
            quality.sharpness=sharpen?LensSharpness::High:LensSharpness::Off;
            if (!renderer.Render(view.Get(), {0,0,32,32}, 32, 32, {}, chrome, true, fullscreen, true,
                &pointer,quality,{1,revision},true,feedback,contentRect)) throw std::runtime_error("production presentation failed");
        };
        draw();
        ShowWindow(window.value, SW_SHOWNOACTIVATE);

        GraphicsCaptureItem item{nullptr};
        auto factory = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(factory->CreateForWindow(window.value, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        CaptureScope capture;
        capture.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(device->WinrtDevice(),
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, {1024,768});
        capture.session = capture.pool.CreateCaptureSession(item);
        capture.session.IsCursorCaptureEnabled(false);
        try { capture.session.IsBorderRequired(false); } catch (...) {}
        capture.session.StartCapture();
        unsigned received{};
        auto observe = [&](RenderExtent expected, bool expectValid=true, bool transition=false,
            bool live=false, bool redraw=true) {
            unsigned accepted{},unexpected{}; bool established{}; PixelCheck last{};
            auto required = redraw ? 6u : 1u;
            auto deadline=GetTickCount64()+4000;
            while(GetTickCount64()<deadline && accepted<required) {
                MSG message{}; while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    TranslateMessage(&message); DispatchMessageW(&message);
                }
                if (redraw) draw();
                if(auto frame=capture.pool.TryGetNextFrame()) {
                    auto size=frame.ContentSize(); ++received;
                    if(size.Width==int(expected.width) && size.Height==int(expected.height)) {
                        auto access=frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                        ComPtr<ID3D11Texture2D> surface;
                        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&surface)));
                        auto pixels=ReadPixels(*device,surface.Get(),expected.width,expected.height);
                        last=ValidatePresentation(pixels,expected,chrome,fullscreen,cursor,tint,feedback,contentRect);
                        // Live resizing deliberately retains a lower raster
                        // resolution. Its 2px chrome may alias at large scales;
                        // exact UI pixels are required after release, not here.
                        bool valid=live ? last.content : last.All();
                        if(!expectValid) valid=last.borders || last.buttons;
                        if(valid==expectValid) { established=true; ++accepted; }
                        else if(established) ++unexpected;
                        // A resize can temporarily retain an older geometry,
                        // but its source image must never become a blank block.
                        if(transition && !last.content) ++unexpected;
                    }
                    frame.Close();
                }
                Sleep(2); // Capture/readback/waits exist only in this test.
            }
            if(accepted<required || unexpected) std::cerr << "presentation " << expected.width << 'x' << expected.height
                << " accepted=" << accepted << " unexpected=" << unexpected << " borders=" << last.borders
                << " buttons=" << last.buttons << " content=" << last.content << " cursor=" << last.cursor
                << " letterbox=" << last.letterbox << '\n';
            return accepted==required && unexpected==0;
        };
        check(observe({300,200}),"four borders, seven buttons/glyphs and full source survive final composition");
        CompositionPresentationTestAccess::LegacyScale(renderer,true);
        check(observe({300,200},false),"negative control rejects the previous source-normalization regression");
        CompositionPresentationTestAccess::LegacyScale(renderer,false);
        check(observe({300,200}),"single storage-to-client transform restores borders/buttons/source");
        auto originalChain=CompositionPresentationTestAccess::Chain(renderer);
        auto storage=CompositionPresentationTestAccess::Storage(renderer);
        for (auto size : {RenderExtent{600,360},{200,128},{800,480},{300,200},{511,239}}) {
            auto held=CompositionPresentationTestAccess::Viewport(renderer);
            renderer.BeginLiveResize();
            SetWindowPos(window.value,nullptr,testX,testY,int(size.width),int(size.height),SWP_NOACTIVATE|SWP_NOZORDER);
            check(renderer.Resize() && CompositionPresentationTestAccess::Viewport(renderer)==held,
                "drag keeps live raster resolution and source updates");
            tint ^= 0x00070707; ++revision;
            for(unsigned y=0;y<32;++y) for(unsigned x=0;x<32;++x)
                sourcePixels[y*32+x]=Quadrants[(y>=16?2:0)+(x>=16?1:0)] ^ tint;
            device->Context()->UpdateSubresource(texture.Get(),0,nullptr,sourcePixels.data(),32*sizeof(uint32_t),0);
            // Confirm real frames still present throughout a native-size change.
            check(observe(size,true,false,true),"continuous lightweight resize keeps the complete source presenting");
            renderer.EndLiveResize();
            check(observe(size,true,true),"mouse release contains no blank source blocks and settles with all UI intact");
            check(CompositionPresentationTestAccess::Chain(renderer)==originalChain &&
                !CompositionPresentationTestAccess::Pending(renderer) &&
                CompositionPresentationTestAccess::Viewport(renderer)==size,
                "release selects final raster resolution without replacing the composition chain");
        }
        sharpen=true; cursor=true;
        check(observe({511,239}),"sharpening leaves all buttons/borders and virtual cursor intact");
        fullscreen=true;
        check(observe({511,239}),"fullscreen hides four borders while preserving revealed controls");
        feedback=true;
        check(observe({511,239}),"screenshot success highlights only the camera on a retained fullscreen frame");
        feedback=false;
        check(observe({511,239}),"screenshot feedback restores without a new capture revision");
        contentRect=PixelRect{55,40,400,180};
        check(observe({511,239}),"fullscreen aspect-fit content has black bars with chrome and cursor above them");
        check(CompositionPresentationTestAccess::QualityBytes(renderer)==uint64_t(400)*180*8,
            "clear-quality cache targets content size rather than the fullscreen client");
        chrome=false;
        check(observe({511,239}),"fullscreen hidden chrome keeps black bars, source and virtual cursor intact");

        // Reproduce the production entry sequence and deliberately withhold the
        // final full-resolution render. The captured 600x360 frame therefore
        // proves that DComp enlarged the prepared 300x200 transition frame,
        // not a formerly full-client image that happens to be corrected later.
        sharpen=false; cursor=false; feedback=false; fullscreen=true; chrome=false;
        SetWindowPos(window.value,nullptr,testX,testY,300,200,SWP_NOACTIVATE|SWP_NOZORDER);
        RECT targetClient{0,0,600,360};
        auto finalFit=FitAspectRect({0,0,1,1},targetClient);
        contentRect=ProjectContentRect(finalFit,600,360,300,200);
        check(observe({300,200}),"programmatic fullscreen transition frame is complete before resize");
        renderer.BeginProgrammaticResize();
        check(CompositionPresentationTestAccess::ProgrammaticResize(renderer) &&
            renderer.PrepareClientExtent({600,360}),
            "programmatic transition permits one bounded pre-size visual transform");
        SetWindowPos(window.value,nullptr,testX,testY,600,360,SWP_NOACTIVATE|SWP_NOZORDER);
        renderer.EndProgrammaticResize();
        contentRect=PixelRect{finalFit.left,finalFit.top,finalFit.right-finalFit.left,finalFit.bottom-finalFit.top};
        check(observe({600,360},true,true,false,false),
            "enlarged retained transition frame already has pure black bars and correct aspect");
        check(!CompositionPresentationTestAccess::ProgrammaticResize(renderer),
            "programmatic transition mode is cleared before the final fullscreen frame");
        check(observe({600,360}),"final fullscreen frame replaces the transition at native resolution");

        contentRect.reset();
        fullscreen=false; chrome=true; cursor=false;
        if(storage.width+16<D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            SetWindowPos(window.value,nullptr,testX,testY,int(storage.width+16),200,SWP_NOACTIVATE|SWP_NOZORDER);
            check(renderer.Resize() && CompositionPresentationTestAccess::Pending(renderer),"exceptional growth retains old content before presenting");
            SetWindowPos(window.value,nullptr,testX,testY,300,200,SWP_NOACTIVATE|SWP_NOZORDER);
            check(renderer.Resize() && CompositionPresentationTestAccess::Pending(renderer),"rapid reversal keeps the replacement handoff pending");
            check(observe({300,200}),"capacity growth and reversal present full frame and UI");
        }
        std::cout << "Final-composition validation sampled " << received << " owned offscreen window frames.\n";
    }
    catch (winrt::hresult_error const& error) { ++failures; std::cerr << "composition capture HRESULT=" << std::hex << unsigned(error.code()) << std::dec << '\n'; }
    catch (std::exception const& error) { ++failures; std::cerr << error.what() << '\n'; }
    if (previousDpi) SetThreadDpiAwarenessContext(previousDpi);
    if (SUCCEEDED(initialized)) CoUninitialize();
    return failures;
}
