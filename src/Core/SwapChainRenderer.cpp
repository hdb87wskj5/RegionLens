#include "pch.h"
#include "AppRuntime.h"
#include "SwapChainRenderer.h"
#include "D3DDevice.h"
#include "RegionTransform.h"
#include "LensChrome.h"
#include <d3d11shader.h>

using Microsoft::WRL::ComPtr;

namespace
{
    constexpr char VertexShaderSource[] = R"(
struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput main(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    output.uv = uv;
    return output;
}
)";

    constexpr char PixelShaderSource[] = R"(
Texture2D SourceTexture : register(t0);
Texture2D CursorTexture : register(t1);
SamplerState LinearSampler : register(s0);

cbuffer Constants : register(b0)
{
    float4 SourceUv;
    float4 Selection;
    float2 DestinationSize;
    float OverlayMode;
    float ChromeMode;
    float TopmostMode;
    float FullscreenMode;
    float InputMappingMode;
    float PointerSpeedMode;
    float4 CursorRect;
    float CursorMode;
    float3 CursorPadding;
    float4 SourceClamp;
    float ChromeCenterY;
    float ChromePadding;
    float ChromeRadius;
    float ScreenshotFeedback;
    float CloseX; float PinX; float FullscreenX; float RestoreX;
    float MappingX; float PointerSpeedX; float ChromePadding2; float ScreenshotX;
    float4 ContentRect;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float2 pixel = input.uv * DestinationSize;
    float2 contentUv = (pixel - ContentRect.xy) / max(ContentRect.zw, float2(1.0f, 1.0f));
    bool insideContent = ContentRect.z > 0.0f && ContentRect.w > 0.0f &&
                         all(contentUv >= 0.0f) && all(contentUv < 1.0f);
    float2 sourceUv = SourceUv.xy + saturate(contentUv) * SourceUv.zw;
    float4 color = insideContent
        ? SourceTexture.Sample(LinearSampler, clamp(sourceUv, SourceClamp.xy, SourceClamp.zw))
        : float4(0.0f, 0.0f, 0.0f, 1.0f);
    if (OverlayMode > 0.5f)
    {
        bool hasSelection = Selection.z > Selection.x && Selection.w > Selection.y;
        bool inside = hasSelection && input.uv.x >= Selection.x && input.uv.x <= Selection.z &&
                      input.uv.y >= Selection.y && input.uv.y <= Selection.w;
        if (!inside)
        {
            color.rgb *= 0.32f;
        }
        if (hasSelection)
        {
            float2 border = 2.0f / max(DestinationSize, float2(1.0f, 1.0f));
            bool onVertical = abs(input.uv.x - Selection.x) <= border.x || abs(input.uv.x - Selection.z) <= border.x;
            bool onHorizontal = abs(input.uv.y - Selection.y) <= border.y || abs(input.uv.y - Selection.w) <= border.y;
            bool withinX = input.uv.x >= Selection.x - border.x && input.uv.x <= Selection.z + border.x;
            bool withinY = input.uv.y >= Selection.y - border.y && input.uv.y <= Selection.w + border.y;
            if ((onVertical && withinY) || (onHorizontal && withinX))
            {
                color = float4(0.0f, 0.47f, 0.84f, 1.0f);
            }
        }
    }

    if (ChromeMode > 0.5f)
    {
        float2 size = DestinationSize;
        const float4 accent = float4(0.0f, 0.47f, 0.84f, 1.0f);
        const float4 button = float4(0.12f, 0.14f, 0.16f, 0.92f);
        const float4 pinnedButton = float4(0.0f, 0.38f, 0.72f, 0.95f);

        bool border = pixel.x < 2.0f || pixel.y < 2.0f ||
                      pixel.x >= size.x - 2.0f || pixel.y >= size.y - 2.0f;

        // Resizing still uses the entire native edge hit target, so the eight
        // decorative handles are unnecessary. Fullscreen suppresses the frame
        // as well and retains only invisible button-reveal hot zones.
        if (FullscreenMode < 0.5f && border)
        {
            color = accent;
        }

        // Positions come from the exact same CPU layout used by hit-testing,
        // mapping exclusion and full-screen reveal, including narrow lenses.
        float2 closeCenter = float2(CloseX, ChromeCenterY);
        float2 pinCenter = float2(PinX, ChromeCenterY);
        float2 fullscreenCenter = float2(FullscreenX, ChromeCenterY);
        float2 restoreSizeCenter = float2(RestoreX, ChromeCenterY);
        float2 inputMappingCenter = float2(MappingX, ChromeCenterY);
        float2 pointerSpeedCenter = float2(PointerSpeedX, ChromeCenterY);
        float2 screenshotCenter = float2(ScreenshotX, ChromeCenterY);
        float closeDistance = length(pixel - closeCenter);
        float pinDistance = length(pixel - pinCenter);
        float fullscreenDistance = length(pixel - fullscreenCenter);
        float restoreSizeDistance = length(pixel - restoreSizeCenter);
        float inputMappingDistance = length(pixel - inputMappingCenter);
        if (length(pixel - screenshotCenter) <= ChromeRadius)
            color = ScreenshotFeedback > 0.5 ? float4(0.0, 0.47, 0.84, 1.0) : button;
        if (length(pixel - pointerSpeedCenter) <= ChromeRadius)
            color = PointerSpeedMode > 0.5f ? pinnedButton : button;
        if (inputMappingDistance <= ChromeRadius)
        {
            color = InputMappingMode > 0.5f ? pinnedButton : button;
        }
        if (fullscreenDistance <= ChromeRadius)
        {
            color = FullscreenMode > 0.5f ? pinnedButton : button;
        }
        if (restoreSizeDistance <= ChromeRadius)
        {
            color = button;
        }
        if (pinDistance <= ChromeRadius)
        {
            color = TopmostMode > 0.5f ? pinnedButton : button;
        }
        if (closeDistance <= ChromeRadius)
        {
            color = button;
        }

        float2 closeDelta = abs(pixel - closeCenter);
        bool closeGlyph = max(closeDelta.x, closeDelta.y) <= 7.0f &&
                          abs(closeDelta.x - closeDelta.y) <= 1.5f;
        float2 pinDelta = pixel - pinCenter;
        bool pinHead = abs(pinDelta.y + 4.0f) <= 1.4f && abs(pinDelta.x) <= 6.0f;
        bool pinBody = abs(pinDelta.x) <= 1.4f && pinDelta.y >= -3.0f && pinDelta.y <= 7.0f;
        bool pinShoulder = abs(pinDelta.y - 1.0f) <= 1.4f && abs(pinDelta.x) <= 4.0f;
        float2 fullscreenDelta = pixel - fullscreenCenter;
        bool fullscreenHorizontal = abs(abs(fullscreenDelta.y) - 6.0f) <= 1.3f &&
                                    abs(fullscreenDelta.x) >= 2.0f && abs(fullscreenDelta.x) <= 6.5f;
        bool fullscreenVertical = abs(abs(fullscreenDelta.x) - 6.0f) <= 1.3f &&
                                  abs(fullscreenDelta.y) >= 2.0f && abs(fullscreenDelta.y) <= 6.5f;
        float2 restoreBack = abs(pixel - (restoreSizeCenter + float2(-2.0f, -2.0f)));
        bool restoreBackGlyph = max(restoreBack.x, restoreBack.y) <= 5.0f &&
                                (abs(restoreBack.x - 4.0f) <= 1.1f || abs(restoreBack.y - 4.0f) <= 1.1f);
        float2 restoreFront = abs(pixel - (restoreSizeCenter + float2(2.0f, 2.0f)));
        bool restoreFrontGlyph = max(restoreFront.x, restoreFront.y) <= 5.0f &&
                                 (abs(restoreFront.x - 4.0f) <= 1.1f || abs(restoreFront.y - 4.0f) <= 1.1f);
        bool restoreSizeGlyph = restoreBackGlyph || restoreFrontGlyph;
        float2 inputMappingDelta = pixel - inputMappingCenter;
        float2 arrowA = float2(-7.0f, -8.0f);
        float2 arrowB = float2(-5.0f, 6.0f);
        float2 arrowC = float2(5.0f, 2.0f);
        float2 arrowV0 = arrowC - arrowA;
        float2 arrowV1 = arrowB - arrowA;
        float2 arrowV2 = inputMappingDelta - arrowA;
        float arrowD00 = dot(arrowV0, arrowV0);
        float arrowD01 = dot(arrowV0, arrowV1);
        float arrowD11 = dot(arrowV1, arrowV1);
        float arrowD20 = dot(arrowV2, arrowV0);
        float arrowD21 = dot(arrowV2, arrowV1);
        float arrowInverse = 1.0f / (arrowD00 * arrowD11 - arrowD01 * arrowD01);
        float arrowU = (arrowD11 * arrowD20 - arrowD01 * arrowD21) * arrowInverse;
        float arrowV = (arrowD00 * arrowD21 - arrowD01 * arrowD20) * arrowInverse;
        bool arrowHead = arrowU >= 0.0f && arrowV >= 0.0f && arrowU + arrowV <= 1.0f;
        float2 arrowStemA = float2(-1.0f, 2.0f);
        float2 arrowStemB = float2(4.0f, 8.0f);
        float2 arrowStemVector = arrowStemB - arrowStemA;
        float arrowStemT = saturate(dot(inputMappingDelta - arrowStemA, arrowStemVector) /
                                    dot(arrowStemVector, arrowStemVector));
        bool arrowStem = length(inputMappingDelta - (arrowStemA + arrowStemT * arrowStemVector)) <= 1.7f;
        bool inputMappingGlyph = arrowHead || arrowStem;
        float2 camera = pixel - screenshotCenter;
        float2 cameraAbs = abs(camera);
        bool cameraGlyph = (cameraAbs.x <= 8 && cameraAbs.y <= 5 && (cameraAbs.x >= 6.5 || cameraAbs.y >= 3.5)) ||
            abs(length(camera) - 2.5) < 0.9 || (abs(camera.y + 6) <= 1 && cameraAbs.x <= 3);
        float2 speed = pixel - pointerSpeedCenter;
        bool speedHead = speed.x >= -2 && speed.x <= 6 && abs(speed.y + 2) <= (6 - speed.x) * 0.65;
        bool speedStem = abs(speed.x - 1) <= 1 && speed.y >= 1 && speed.y <= 7;
        bool speedLines = speed.x >= -8 && speed.x <= -4 &&
            (abs(speed.y + 4) <= 0.8 || abs(speed.y) <= 0.8 || abs(speed.y - 4) <= 0.8);
        if (speedHead || speedStem || speedLines || cameraGlyph || closeGlyph || pinHead || pinBody || pinShoulder || fullscreenHorizontal || fullscreenVertical ||
            restoreSizeGlyph || inputMappingGlyph)
        {
            color = float4(1.0f, 1.0f, 1.0f, 1.0f);
        }
    }
    // DestinationSize is the physical client size, not the fixed back-buffer
    // resolution used during live resizing. Keep chrome and cursor hit geometry
    // in client pixels while DirectComposition scales the rendered video.
    float2 cp = input.uv * DestinationSize - CursorRect.xy;
    if (CursorMode > 0.5f && all(cp >= 0.0f) && all(cp < CursorRect.zw))
    {
        float4 cursor = CursorTexture.Load(int3(int2(cp), 0));
        if (CursorMode < 1.5f)
            color.rgb = cursor.rgb + color.rgb * (1.0f - cursor.a);
        else
        {
            uint3 background = (uint3)round(saturate(color.rgb) * 255.0f);
            uint mask = cursor.a > 0.5f ? 255u : 0u;
            uint3 xorMask = (uint3)round(cursor.rgb * 255.0f);
            color.rgb = float3((background & mask) ^ xorMask) / 255.0f;
        }
    }
    color.a = 1.0f;
    return color;
}
)";

    bool CompileShader(char const* source, char const* target, ComPtr<ID3DBlob>& blob)
    {
        ComPtr<ID3DBlob> errors;
        auto hr = D3DCompile(
            source,
            strlen(source),
            nullptr,
            nullptr,
            nullptr,
            "main",
            target,
            D3DCOMPILE_ENABLE_STRICTNESS,
            0,
            blob.ReleaseAndGetAddressOf(),
            errors.ReleaseAndGetAddressOf());
        if (FAILED(hr) && errors)
        {
            OutputDebugStringA(static_cast<char const*>(errors->GetBufferPointer()));
        }
        return SUCCEEDED(hr);
    }
}

namespace RegionLens::native
{
    void SwapChainRenderer::SetChromeLayout(ShaderConstants& values, RECT client) noexcept
    {
        auto x = [&](int id) { return float(ChromeButtonCenterFor(id, client).x); };
        values.chromeCenterY = float(ChromeButtonCenter(0, client).y);
        values.chromeRadius = float(ChromeRadius(client.right - client.left));
        values.closeX = x(CloseButtonId); values.pinX = x(PinButtonId);
        values.fullscreenX = x(FullscreenButtonId); values.restoreX = x(RestoreSizeButtonId);
        values.mappingX = x(InputMappingButtonId); values.pointerSpeedX = x(PointerSpeedButtonId);
        values.screenshotX = x(ScreenshotButtonId);
    }

    bool SwapChainRenderer::CompileFrameShaders(ComPtr<ID3DBlob>& vertex, ComPtr<ID3DBlob>& pixel)
    {
        return CompileShader(VertexShaderSource, "vs_5_0", vertex) && CompileShader(PixelShaderSource, "ps_5_0", pixel);
    }
    bool SwapChainRenderer::ValidateShaders()
    {
        if (!QualityRenderer::ValidateShaders()) return false;
        static_assert(sizeof(ShaderConstants) == 176, "HLSL constant buffer layout must match");
        ComPtr<ID3DBlob> vertex, pixel;
        if (!CompileFrameShaders(vertex, pixel)) return false;
        ComPtr<ID3D11ShaderReflection> reflection;
        if (FAILED(D3DReflect(pixel->GetBufferPointer(), pixel->GetBufferSize(), IID_PPV_ARGS(&reflection)))) return false;
        auto constants = reflection->GetConstantBufferByName("Constants");
        D3D11_SHADER_BUFFER_DESC buffer{};
        if (FAILED(constants->GetDesc(&buffer)) || buffer.Size != sizeof(ShaderConstants)) return false;
        auto matches = [constants](char const* name, size_t offset, UINT size) {
            D3D11_SHADER_VARIABLE_DESC value{};
            return SUCCEEDED(constants->GetVariableByName(name)->GetDesc(&value)) && value.StartOffset == offset && value.Size == size;
        };
        return matches("DestinationSize", offsetof(ShaderConstants, destinationSize), 8) &&
            matches("CursorRect", offsetof(ShaderConstants, cursorRect), 16) &&
            matches("CursorMode", offsetof(ShaderConstants, cursorMode), 4) &&
            matches("SourceClamp", offsetof(ShaderConstants, sourceClamp), 16) &&
            matches("ChromeCenterY", offsetof(ShaderConstants, chromeCenterY), 4) &&
            matches("PointerSpeedMode", offsetof(ShaderConstants, pointerSpeedMode), 4) &&
            matches("CloseX", offsetof(ShaderConstants, closeX), 4) &&
            matches("PointerSpeedX", offsetof(ShaderConstants, pointerSpeedX), 4) &&
            matches("ScreenshotX", offsetof(ShaderConstants, screenshotX), 4) &&
            matches("ChromeRadius", offsetof(ShaderConstants, chromeRadius), 4) &&
            matches("ScreenshotFeedback", offsetof(ShaderConstants, screenshotFeedback), 4) &&
            matches("ContentRect", offsetof(ShaderConstants, contentRect), 16);
    }
    bool SwapChainRenderer::Initialize(HWND window, std::shared_ptr<D3DDevice> device, bool composition, uint64_t lensId)
    {
        m_window = window;
        m_device = std::move(device);
        m_composition = composition;
        m_lensId = lensId;
        if (composition)
        {
            ComPtr<IDXGIDevice> dxgi;
            if (FAILED(m_device->Device()->QueryInterface(IID_PPV_ARGS(&dxgi))) ||
                FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&m_compositionDevice))) ||
                FAILED(m_compositionDevice->CreateTargetForHwnd(window, TRUE, &m_compositionTarget)) ||
                FAILED(m_compositionDevice->CreateVisual(&m_compositionVisual))) return false;
        }
        return CreateDeviceResources() && Resize();
    }

    bool SwapChainRenderer::CreateDeviceResources()
    {
        ComPtr<ID3DBlob> vertexBlob;
        ComPtr<ID3DBlob> pixelBlob;
        if (!CompileFrameShaders(vertexBlob, pixelBlob))
        {
            return false;
        }

        auto device = m_device->Device();
        if (FAILED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(), nullptr, m_vertexShader.ReleaseAndGetAddressOf())) ||
            FAILED(device->CreatePixelShader(pixelBlob->GetBufferPointer(), pixelBlob->GetBufferSize(), nullptr, m_pixelShader.ReleaseAndGetAddressOf())))
        {
            return false;
        }

        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sampler, m_sampler.ReleaseAndGetAddressOf())))
        {
            return false;
        }

        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(ShaderConstants);
        constants.Usage = D3D11_USAGE_DYNAMIC;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&constants, nullptr, m_constants.ReleaseAndGetAddressOf()));
    }

    bool SwapChainRenderer::CreateWindowSizeResources(RenderExtent size)
    {
        if (!size.Valid()) return false;
        if (m_compositionSwapChain && m_renderTarget && FitsCompositionStorage(size, m_bufferStorage))
        {
            // Keep the displayed resource attached at mouse release. Only the
            // raster viewport changes, with SetSourceSize submitted after drawing.
            m_width = size.width; m_height = size.height;
            RecordResize(3);
            return StretchToClient();
        }
        auto width = size.width;
        auto height = size.height;
        auto storage = size;
        if (m_composition)
        {
            MONITORINFO monitor{sizeof(monitor)};
            RenderExtent display{};
            if (GetMonitorInfoW(MonitorFromWindow(m_window, MONITOR_DEFAULTTONEAREST), &monitor))
                display = {UINT(monitor.rcMonitor.right - monitor.rcMonitor.left),
                    UINT(monitor.rcMonitor.bottom - monitor.rcMonitor.top)};
            storage = ReserveCompositionStorage(size, display, m_bufferStorage);
        }
        constexpr UINT swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        auto descriptionFor = [&](bool composition)
        {
            DXGI_SWAP_CHAIN_DESC1 description{};
            description.Width = storage.width;
            description.Height = storage.height;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            description.BufferCount = 2;
            description.Scaling = DXGI_SCALING_STRETCH;
            description.SwapEffect = composition ? DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL : DXGI_SWAP_EFFECT_FLIP_DISCARD;
            description.AlphaMode = composition ? DXGI_ALPHA_MODE_PREMULTIPLIED : DXGI_ALPHA_MODE_IGNORE;
            description.Flags = swapChainFlags;
            return description;
        };

        bool const initial = !m_swapChain;
        bool const replaceComposition = m_composition && !initial;
        ComPtr<IDXGISwapChain1> replacement;
        HRESULT hr{};
        if (replaceComposition)
        {
            auto description = descriptionFor(true);
            hr = m_device->Factory()->CreateSwapChainForComposition(
                m_device->Device(), &description, nullptr, replacement.ReleaseAndGetAddressOf());
        }
        else if (!initial)
        {
            m_device->Context()->OMSetRenderTargets(0, nullptr, nullptr);
            m_renderTarget.Reset();
            m_presentationWait.Suspend();
            hr = m_swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, swapChainFlags);
        }
        else
        {
            auto description = descriptionFor(m_composition);
            hr = m_composition ? m_device->Factory()->CreateSwapChainForComposition(
                m_device->Device(), &description, nullptr, replacement.ReleaseAndGetAddressOf()) :
                m_device->Factory()->CreateSwapChainForHwnd(
                    m_device->Device(), m_window, &description, nullptr, nullptr, replacement.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr) && m_composition && storage != size)
        {
            // Reservation is optional under memory pressure. Preserve resolution
            // and try exact-size storage, retaining the old chain during handoff.
            storage = size;
            auto description = descriptionFor(true);
            hr = m_device->Factory()->CreateSwapChainForComposition(
                m_device->Device(), &description, nullptr, replacement.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) return false;

        auto resourceSwapChain = replacement ? replacement.Get() : m_swapChain.Get();

        ComPtr<IDXGISwapChain2> lowLatencySwapChain;
        HANDLE frameLatencyWaitable{};
        auto queryResult = resourceSwapChain->QueryInterface(IID_PPV_ARGS(&lowLatencySwapChain));
        if (m_composition && FAILED(queryResult)) return false;
        if (SUCCEEDED(queryResult))
        {
            if (FAILED(lowLatencySwapChain->SetMaximumFrameLatency(1)) && m_composition) return false;
            if (replacement || initial) frameLatencyWaitable = lowLatencySwapChain->GetFrameLatencyWaitableObject();
            // A live composition surface must not silently bypass backpressure
            // when the expected DXGI readiness object cannot be obtained.
            if (m_composition && (replacement || initial) && !frameLatencyWaitable) return false;
        }

        ComPtr<ID3D11Texture2D> backBuffer;
        ComPtr<ID3D11RenderTargetView> renderTarget;
        if (FAILED(resourceSwapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.ReleaseAndGetAddressOf()))) ||
            FAILED(m_device->Device()->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTarget.ReleaseAndGetAddressOf()))) {
            if (frameLatencyWaitable) CloseHandle(frameLatencyWaitable);
            return false;
        }

        m_device->Context()->OMSetRenderTargets(0, nullptr, nullptr);
        m_renderTarget = std::move(renderTarget);
        if ((replacement || initial) && !m_presentationWait.Attach(frameLatencyWaitable)) return false;
        if (m_composition) m_compositionSwapChain = lowLatencySwapChain;
        m_bufferStorage = storage;
        if (replacement)
        {
            if (replaceComposition)
            {
                // If a second final size arrives before the first handoff, keep
                // retaining the swap chain that the visual actually displays.
                if (!m_resizeHandoff.Pending()) m_retiredSwapChain = m_swapChain;
                m_swapChain = std::move(replacement);
                m_resizeHandoff.Prepare(storage);
            }
            else
            {
                m_swapChain = std::move(replacement);
                m_device->Factory()->MakeWindowAssociation(m_window, DXGI_MWA_NO_ALT_ENTER);
            }
        }
        m_width = width;
        m_height = height;
        RecordResize(2);

        if (replaceComposition) return true; // Old presented content remains attached and stretched.
        if (initial && m_composition)
        {
            if (FAILED(m_compositionVisual->SetContent(m_swapChain.Get())) ||
                FAILED(m_compositionTarget->SetRoot(m_compositionVisual.Get())) ||
                FAILED(m_compositionDevice->Commit())) return false;
            m_resizeHandoff.Initialize(storage);
        }
        m_transformedClient = {}; // The displayed buffer changed its scale denominator.
        return StretchToClient();
    }

    bool SwapChainRenderer::StretchToClient()
    {
        if (!m_compositionVisual) return true;
        RECT client{};
        if (!GetClientRect(m_window, &client)) return false;
        if (client.right <= 0 || client.bottom <= 0) return true;
        return StretchToExtent({ UINT(client.right), UINT(client.bottom) });
    }

    bool SwapChainRenderer::PrepareClientExtent(RenderExtent extent)
    {
        if (!m_compositionVisual || (!m_resizePolicy.Holding() && !m_programmaticResize)) return true;
        return StretchToExtent(extent);
    }

    bool SwapChainRenderer::StretchToExtent(RenderExtent extent)
    {
        // Composition swap chains use DXGI_SCALING_STRETCH: source cropping
        // still presents at the allocation's extent. Normalize by that actual
        // storage, NOT the raster viewport. No additional swap-chain matrix.
        auto displayed = m_resizeHandoff.Displayed();
        auto denominator = m_composition ? displayed : RenderExtent{ m_width, m_height };
        if (!extent.Valid() || !denominator.Valid()) return false;
        if (m_transformedClient == extent) return true; // No per-frame composition commits.
        D2D_MATRIX_3X2_F transform{ float(extent.width) / denominator.width, 0, 0,
            float(extent.height) / denominator.height, 0, 0 };
        if (FAILED(m_compositionVisual->SetTransform(transform)) || FAILED(m_compositionDevice->Commit())) return false;
        m_transformedClient = extent;
        return true;
    }

    bool SwapChainRenderer::Resize()
    {
        if (!m_device || !m_window) return false;
        RECT client{};
        if (!GetClientRect(m_window, &client)) return false;
        if (client.right <= 0 || client.bottom <= 0) return true; // Keep resources while minimized.
        auto target = m_resizePolicy.BufferTarget({ UINT(client.right), UINT(client.bottom) }, { m_width, m_height });
        if (target != RenderExtent{ m_width, m_height } || !m_renderTarget)
            return CreateWindowSizeResources(target);
        return StretchToClient();
    }

    void SwapChainRenderer::BeginLiveResize()
    {
        if (m_resizePolicy.Begin(m_composition)) RecordResize(0);
    }

    bool SwapChainRenderer::EndLiveResize()
    {
        if (m_resizePolicy.End()) RecordResize(1);
        // Do not destroy displayed buffers here. Render() prepares the final
        // size while this last stretched frame remains continuously visible.
        return StretchToClient();
    }

    bool SwapChainRenderer::AttachPreparedCompositionFrame()
    {
        if (!m_resizeHandoff.Pending()) return true;
        RECT client{};
        if (!GetClientRect(m_window, &client) || client.right <= 0 || client.bottom <= 0) return false;
        D2D_MATRIX_3X2_F transform{ float(client.right) / m_bufferStorage.width, 0, 0,
            float(client.bottom) / m_bufferStorage.height, 0, 0 };
        // The replacement already owns a complete presented frame. Content and
        // transform enter the visual tree in the same DirectComposition commit.
        if (FAILED(m_compositionVisual->SetContent(m_swapChain.Get())) ||
            FAILED(m_compositionVisual->SetTransform(transform)) ||
            FAILED(m_compositionDevice->Commit())) return false;
        m_resizeHandoff.Commit();
        m_retiredSwapChain.Reset();
        m_transformedClient = { UINT(client.right), UINT(client.bottom) };
        return true;
    }

    void SwapChainRenderer::RecordResize(int phase)
    {
        if (!Runtime().diagnostics) return;
        RECT client{}; GetClientRect(m_window, &client);
        Record(DiagnosticEvent::RenderResize, { phase, m_width, m_height, client.right, client.bottom,
            m_resizePolicy.Holding(), m_bufferStorage.width, m_bufferStorage.height }, false, 0, m_lensId);
    }

    RenderResult SwapChainRenderer::PreparePresentation()
    {
        if (!Resize() || !m_swapChain || !m_renderTarget) return RenderResult::Failed;
        auto ready=m_presentationWait.Poll();
        if (ready==PresentationWait::Result::Failed) {
            Record(DiagnosticEvent::Fault,{m_presentationWait.Error(),31},true,0,m_lensId);
            return RenderResult::Failed;
        }
        m_presentPending=ready==PresentationWait::Result::Pending;
        if (m_presentPending) {
            if (!m_waitStarted && Runtime().diagnostics) m_waitStarted=QpcNow();
            return RenderResult::Deferred;
        }
        if (m_waitStarted) {m_waitLatency.Add(QpcMicros(QpcNow()-m_waitStarted));m_waitStarted=0;}
        return RenderResult::Presented;
    }

    RenderResult SwapChainRenderer::Render(
        ID3D11ShaderResourceView* source,
        PixelRect sourceRect,
        int32_t textureWidth,
        int32_t textureHeight,
        std::optional<PixelRect> selection,
        bool lensChrome,
        bool lensTopmost,
        bool lensFullscreen,
        bool lensInputMapping,
        CursorSnapshot const* cursor, LensQualitySettings quality, CaptureStamp frame, bool lensPointerSpeedAdjusted,
        bool lensScreenshotFeedback, std::optional<PixelRect> contentRect)
    {
        if (!source || !m_device || !m_window)
        {
            return RenderResult::Failed;
        }

        RECT client{};
        if (!GetClientRect(m_window, &client)) return RenderResult::Failed;
        if (client.right <= 0 || client.bottom <= 0) { m_presentPending = false; return RenderResult::Presented; }
        auto width = static_cast<UINT>(std::max<LONG>(1, client.right - client.left));
        auto height = static_cast<UINT>(std::max<LONG>(1, client.bottom - client.top));
        auto clampContent = [width, height](PixelRect value) noexcept {
            if (value.width <= 0 || value.height <= 0) return PixelRect{};
            auto left = (std::clamp)(int64_t(value.x), int64_t(0), int64_t(width));
            auto top = (std::clamp)(int64_t(value.y), int64_t(0), int64_t(height));
            auto right = (std::clamp)(int64_t(value.x) + value.width, int64_t(0), int64_t(width));
            auto bottom = (std::clamp)(int64_t(value.y) + value.height, int64_t(0), int64_t(height));
            return PixelRect{ int32_t(left), int32_t(top), int32_t((std::max)(left, right) - left),
                int32_t((std::max)(top, bottom) - top) };
        };
        auto content = clampContent(contentRect.value_or(PixelRect{ 0, 0, int32_t(width), int32_t(height) }));
        auto ready=PreparePresentation();
        if (ready.state!=RenderResult::Presented) return ready;

        sourceRect = ClampRect(sourceRect, textureWidth, textureHeight);
        if (sourceRect.Empty()) return RenderResult::Failed;
        if (quality.mode == LensQualityMode::Smooth) {
            m_quality.Reset(); m_qualityUnavailable = false;
        }
        RenderExtent contentExtent{ UINT((std::max)(0, content.width)), UINT((std::max)(0, content.height)) };
        bool clear = !m_qualityUnavailable && UseClearQuality(quality, sourceRect,
            contentExtent, m_resizePolicy.Holding(), m_composition);
        if (clear) {
            auto before = m_quality.ResampleCount();
            LARGE_INTEGER start{}, end{};
            if (Runtime().diagnostics) QueryPerformanceCounter(&start);
            auto hr = m_quality.Prepare(m_device->Device(), m_device->Context(), source,
                { frame, sourceRect, contentExtent }, quality.sharpness);
            if (Runtime().diagnostics) {
                QueryPerformanceCounter(&end);
                m_qualityCpuTicks += end.QuadPart - start.QuadPart;
                m_qualityCacheHits += SUCCEEDED(hr) && before == m_quality.ResampleCount();
            }
            if (FAILED(hr)) {
                m_quality.Reset();
                // A removed device cannot safely display a virtual cursor.
                if (FAILED(m_device->Device()->GetDeviceRemovedReason())) return RenderResult::Failed;
                m_qualityUnavailable = true; clear = false;
                if (!m_qualityFailureNotified) { m_qualityFailure = hr; m_qualityFailureNotified = true; }
                Record(DiagnosticEvent::RenderQuality, { 2, hr }, true, 0, m_lensId);
            }
        }
        if (clear) {
            source = m_quality.View();
            // The quality stage is allocated for the visible content only,
            // not for the surrounding letterbox bars.
            textureWidth = static_cast<int32_t>(contentExtent.width);
            textureHeight = static_cast<int32_t>(contentExtent.height);
            sourceRect = { 0, 0, textureWidth, textureHeight };
        }
        if (Runtime().diagnostics && GetTickCount64() - m_qualityLastReport >= 1000) {
            LARGE_INTEGER frequency{}; QueryPerformanceFrequency(&frequency);
            Record(DiagnosticEvent::RenderQuality, { 0, int64_t(quality.mode), clear,
                int64_t(m_qualityCacheHits), int64_t(m_quality.ResampleCount()),
                int64_t(m_quality.SharpenCount()), int64_t(m_quality.ResourceBytes()),
                int64_t(m_qualityCpuTicks * 1000000 / frequency.QuadPart) }, false, 0, m_lensId);
            m_qualityLastReport = GetTickCount64(); m_qualityCacheHits = m_qualityCpuTicks = 0;
        }
        auto uv = ToUv(sourceRect, textureWidth, textureHeight);
        ShaderConstants values{};
        values.sourceUv[0] = uv.left;
        values.sourceUv[1] = uv.top;
        values.sourceUv[2] = uv.width;
        values.sourceUv[3] = uv.height;
        values.destinationSize[0] = static_cast<float>(width);
        values.destinationSize[1] = static_cast<float>(height);
        values.contentRect[0] = static_cast<float>(content.x);
        values.contentRect[1] = static_cast<float>(content.y);
        values.contentRect[2] = static_cast<float>(content.width);
        values.contentRect[3] = static_cast<float>(content.height);
        values.sourceClamp[0] = (sourceRect.x + 0.5f) / textureWidth;
        values.sourceClamp[1] = (sourceRect.y + 0.5f) / textureHeight;
        values.sourceClamp[2] = (sourceRect.Right() - 0.5f) / textureWidth;
        values.sourceClamp[3] = (sourceRect.Bottom() - 0.5f) / textureHeight;
        SetChromeLayout(values, client);
        values.pointerSpeedMode = lensPointerSpeedAdjusted ? 1.0f : 0.0f;
        values.screenshotFeedback = lensScreenshotFeedback ? 1.0f : 0.0f;
        values.overlayMode = selection.has_value() ? 1.0f : 0.0f;
        values.chromeMode = lensChrome ? 1.0f : 0.0f;
        values.topmostMode = lensTopmost ? 1.0f : 0.0f;
        values.fullscreenMode = lensFullscreen ? 1.0f : 0.0f;
        values.inputMappingMode = lensInputMapping ? 1.0f : 0.0f;
        if (cursor && cursor->visible)
        {
            if (!m_cursorTexture.Update(cursor->shape, m_device->Device())) return RenderResult::Failed;
            POINT origin{}; ClientToScreen(m_window, &origin);
            auto hotspot = m_cursorTexture.Hotspot(); auto size = m_cursorTexture.Size();
            values.cursorRect[0] = float(cursor->position.x - origin.x - hotspot.x);
            values.cursorRect[1] = float(cursor->position.y - origin.y - hotspot.y);
            values.cursorRect[2] = float(size.cx); values.cursorRect[3] = float(size.cy);
            values.cursorMode = m_cursorTexture.Mode();
        }
        if (selection)
        {
            values.selection[0] = static_cast<float>(selection->x) / m_width;
            values.selection[1] = static_cast<float>(selection->y) / m_height;
            values.selection[2] = static_cast<float>(selection->Right()) / m_width;
            values.selection[3] = static_cast<float>(selection->Bottom()) / m_height;
        }

        auto context = m_device->Context();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            return RenderResult::Failed;
        }
        memcpy(mapped.pData, &values, sizeof(values));
        context->Unmap(m_constants.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(m_width);
        viewport.Height = static_cast<float>(m_height);
        viewport.MaxDepth = 1.0f;
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, m_renderTarget.GetAddressOf(), nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
        context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &source);
        auto cursorView = values.cursorMode > 0 ? m_cursorTexture.View() : nullptr;
        context->PSSetShaderResources(1, 1, &cursorView);
        auto sampler = m_sampler.Get();
        context->PSSetSamplers(0, 1, &sampler);
        auto constants = m_constants.Get();
        context->PSSetConstantBuffers(0, 1, &constants);
        context->Draw(3, 0);

        ID3D11ShaderResourceView* empty = nullptr;
        context->PSSetShaderResources(0, 1, &empty);
        context->PSSetShaderResources(1, 1, &empty);
        // The source rectangle is presented by DXGI at m_bufferStorage size.
        // DComp's single client/storage transform already maps it to the HWND.
        // Do not add SetMatrixTransform(1/viewport): it clips the final UI.
        if (m_compositionSwapChain && FAILED(m_compositionSwapChain->SetSourceSize(m_width, m_height))) return RenderResult::Failed;
        auto presentStart = Runtime().diagnostics ? QpcNow() : 0;
        auto result = m_swapChain->Present(0, DXGI_PRESENT_DO_NOT_WAIT);
        m_presentationWait.Consume();
        if (Runtime().diagnostics) {
            auto now = GetTickCount64(); ++m_performanceFrames;
            m_performanceBusy += result == DXGI_ERROR_WAS_STILL_DRAWING;
            auto elapsed=QpcMicros(QpcNow()-presentStart);
            m_presentLatency.Add(elapsed);
            m_performanceLongest = (std::max)(m_performanceLongest, elapsed/1000);
            if (SUCCEEDED(result) && frame.capturedQpc && frame.revision!=m_lastTimedRevision) {
                auto current=QpcNow();
                if(current>=frame.capturedQpc) m_frameAge.Add(QpcMicros(current-frame.capturedQpc));
                m_lastTimedRevision=frame.revision;
            }
            if (now - m_performanceLast >= 1000) {
                Record(DiagnosticEvent::Performance, { int64_t(m_performanceFrames), int64_t(m_performanceBusy),
                    int64_t(m_performanceLongest), m_width, m_height, result, width, height }, false, 0, m_lensId);
                m_waitLatency.Report(3,m_lensId);
                m_presentLatency.Report(4,m_lensId);
                m_frameAge.Report(5,m_lensId);
                m_performanceFrames = m_performanceBusy = m_performanceLongest = 0; m_performanceLast = now;
            }
        }
        m_presentPending = result == DXGI_ERROR_WAS_STILL_DRAWING;
        if (SUCCEEDED(result) && !AttachPreparedCompositionFrame()) return RenderResult::Failed;
        if (SUCCEEDED(result)) {m_presentedFrame=frame;++m_presentedCount;}
        return SUCCEEDED(result) ? RenderResult::Presented :
            result == DXGI_ERROR_WAS_STILL_DRAWING ? RenderResult::Deferred : RenderResult::Failed;
    }
}
