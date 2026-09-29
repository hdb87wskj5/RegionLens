#include "pch.h"
#include "ScreenshotRenderer.h"
#include "SwapChainRenderer.h"
#include "RegionTransform.h"

using Microsoft::WRL::ComPtr;
namespace RegionLens::native
{
    HRESULT ScreenshotRenderer::Initialize(ID3D11Device* device)
    {
        if (m_constants) return S_OK;
        ComPtr<ID3DBlob> vs, ps;
        if (!SwapChainRenderer::CompileFrameShaders(vs, ps)) return E_FAIL;
        HRESULT hr = device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &m_vertex);
        if (FAILED(hr)) return hr;
        hr = device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &m_pixel);
        if (FAILED(hr)) return hr;
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device->CreateSamplerState(&sampler, &m_sampler);
        if (FAILED(hr)) return hr;
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = sizeof(SwapChainRenderer::ShaderConstants);
        buffer.Usage = D3D11_USAGE_DEFAULT; buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        return device->CreateBuffer(&buffer, nullptr, &m_constants);
    }

    HRESULT ScreenshotRenderer::Capture(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* source, PixelRect crop, RenderExtent sourceSize,
        RenderExtent output, LensQualitySettings quality, CaptureStamp stamp, ComPtr<ID3D11Texture2D>& staging)
    {
        staging.Reset();
        if (!source || !ValidScreenshotExtent(output) || crop.Empty() ||
            crop != ClampRect(crop, int32_t(sourceSize.width), int32_t(sourceSize.height))) return E_INVALIDARG;
        auto hr = Initialize(device);
        if (FAILED(hr)) return hr;
        QualityRenderer enhanced;
        if (UseClearQuality(quality, crop, output, false, true)) {
            hr = enhanced.Prepare(device, context, source, { stamp, crop, output }, quality.sharpness);
            if (FAILED(hr)) return hr; // Do not silently save a different quality than requested.
            source = enhanced.View(); sourceSize = output;
            crop = { 0, 0, int32_t(output.width), int32_t(output.height) };
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = output.width; desc.Height = output.height;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        hr = device->CreateTexture2D(&desc, nullptr, &target);
        if (FAILED(hr)) return hr;
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = device->CreateRenderTargetView(target.Get(), nullptr, &rtv);
        if (FAILED(hr)) return hr;
        desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        hr = device->CreateTexture2D(&desc, nullptr, &staging);
        if (FAILED(hr)) return hr;

        SwapChainRenderer::ShaderConstants constants{};
        auto uv = ToUv(crop, int32_t(sourceSize.width), int32_t(sourceSize.height));
        constants.sourceUv[0] = uv.left; constants.sourceUv[1] = uv.top;
        constants.sourceUv[2] = uv.width; constants.sourceUv[3] = uv.height;
        constants.destinationSize[0] = float(output.width); constants.destinationSize[1] = float(output.height);
        constants.contentRect[0] = 0.0f; constants.contentRect[1] = 0.0f;
        constants.contentRect[2] = float(output.width); constants.contentRect[3] = float(output.height);
        constants.sourceClamp[0] = (crop.x + .5f) / sourceSize.width;
        constants.sourceClamp[1] = (crop.y + .5f) / sourceSize.height;
        constants.sourceClamp[2] = (crop.Right() - .5f) / sourceSize.width;
        constants.sourceClamp[3] = (crop.Bottom() - .5f) / sourceSize.height;
        context->UpdateSubresource(m_constants.Get(), 0, nullptr, &constants, 0, 0);
        D3D11_VIEWPORT viewport{ 0, 0, float(output.width), float(output.height), 0, 1 };
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_vertex.Get(), nullptr, 0);
        context->PSSetShader(m_pixel.Get(), nullptr, 0);
        context->PSSetShaderResources(0, 1, &source);
        context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
        context->Draw(3, 0);
        ID3D11ShaderResourceView* empty = nullptr; context->PSSetShaderResources(0, 1, &empty);
        context->OMSetRenderTargets(0, nullptr, nullptr);
        context->CopyResource(staging.Get(), target.Get());
        context->Flush(); // Submit once; polling reads with DO_NOT_WAIT, never waits for GPU completion.
        return device->GetDeviceRemovedReason();
    }
}
