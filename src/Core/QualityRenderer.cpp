#include "QualityRenderer.h"
#include "QualityShaders.h"
#include <d3d11shader.h>
using Microsoft::WRL::ComPtr;
namespace RegionLens::native
{
    namespace {
        struct alignas(16) Constants { float rect[4], size[2], strength, reserved; };
        HRESULT Compile(char const* text, char const* profile, ComPtr<ID3DBlob>& blob) {
            return D3DCompile(text, strlen(text), nullptr, nullptr, nullptr, "main", profile,
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, nullptr);
        }
    }
    bool QualityRenderer::ValidateShaders() {
        ComPtr<ID3DBlob> blob;
        if (FAILED(Compile(QualityVertexShader,"vs_5_0",blob))) return false;
        for (auto text : { QualityResizeShader, QualityCasShader }) {
            blob.Reset(); if (FAILED(Compile(text,"ps_5_0",blob))) return false;
            ComPtr<ID3D11ShaderReflection> reflection;
            if (FAILED(D3DReflect(blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&reflection)))) return false;
            auto buffer=reflection->GetConstantBufferByName("QualityConstants"); D3D11_SHADER_BUFFER_DESC desc{};
            if (FAILED(buffer->GetDesc(&desc)) || desc.Size!=sizeof(Constants)) return false;
            for (auto field : { std::pair{"SourceRect",0u}, {"OutputSize",16u}, {"Strength",24u} }) {
                D3D11_SHADER_VARIABLE_DESC variable{};
                if (FAILED(buffer->GetVariableByName(field.first)->GetDesc(&variable)) || variable.StartOffset!=field.second) return false;
            }
        }
        return true;
    }
    HRESULT QualityRenderer::Initialize(ID3D11Device* device) {
        if(m_constants) return S_OK;
        ComPtr<ID3DBlob> blob;
        HRESULT hr=Compile(QualityVertexShader,"vs_5_0",blob); if(FAILED(hr)) return hr;
        hr=device->CreateVertexShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,m_vertex.ReleaseAndGetAddressOf()); if(FAILED(hr)) return hr;
        blob.Reset(); hr=Compile(QualityResizeShader,"ps_5_0",blob); if(FAILED(hr)) return hr;
        hr=device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,m_resize.ReleaseAndGetAddressOf()); if(FAILED(hr)) return hr;
        blob.Reset(); hr=Compile(QualityCasShader,"ps_5_0",blob); if(FAILED(hr)) return hr;
        hr=device->CreatePixelShader(blob->GetBufferPointer(),blob->GetBufferSize(),nullptr,m_cas.ReleaseAndGetAddressOf()); if(FAILED(hr)) return hr;
        D3D11_BUFFER_DESC desc{}; desc.ByteWidth=sizeof(Constants); desc.Usage=D3D11_USAGE_DYNAMIC;
        desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER; desc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        return device->CreateBuffer(&desc,nullptr,m_constants.ReleaseAndGetAddressOf());
    }
    HRESULT QualityRenderer::Allocate(ID3D11Device* device, RenderExtent size, bool sharp) {
        if (m_size!=size) {
            m_resizedTarget.Reset(); m_resizedView.Reset(); m_linearView.Reset(); m_sharpTarget.Reset(); m_sharpView.Reset();
            m_valid=m_sharpValid=false; m_size={};
        }
        D3D11_TEXTURE2D_DESC desc{}; desc.Width=size.width; desc.Height=size.height;
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1; desc.Format=DXGI_FORMAT_R8G8B8A8_TYPELESS;
        desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        auto allocate=[&](ComPtr<ID3D11RenderTargetView>& target, ComPtr<ID3D11ShaderResourceView>& view,
            ComPtr<ID3D11ShaderResourceView>* linear)->HRESULT {
            ComPtr<ID3D11Texture2D> texture; HRESULT hr=device->CreateTexture2D(&desc,nullptr,&texture); if(FAILED(hr)) return hr;
            D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
            hr=device->CreateRenderTargetView(texture.Get(),&rtv,target.ReleaseAndGetAddressOf()); if(FAILED(hr)) return hr;
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
            hr=device->CreateShaderResourceView(texture.Get(),&srv,view.ReleaseAndGetAddressOf()); if(FAILED(hr)) return hr;
            if(linear) { srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; hr=device->CreateShaderResourceView(texture.Get(),&srv,linear->ReleaseAndGetAddressOf()); }
            return hr;
        };
        HRESULT hr=S_OK;
        if(!m_resizedView) { hr=allocate(m_resizedTarget,m_resizedView,&m_linearView); if(FAILED(hr)) return hr; m_size=size; }
        if(sharp && !m_sharpView) hr=allocate(m_sharpTarget,m_sharpView,nullptr);
        return hr;
    }
    HRESULT QualityRenderer::Draw(ID3D11DeviceContext* context, ID3D11PixelShader* shader,
        ID3D11RenderTargetView* target, ID3D11ShaderResourceView* source, QualityCacheKey const& key, float strength) {
        Constants values{{float(key.source.x),float(key.source.y),float(key.source.width),float(key.source.height)},
            {float(key.output.width),float(key.output.height)},strength,0};
        D3D11_MAPPED_SUBRESOURCE mapped{}; HRESULT hr=context->Map(m_constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
        if(FAILED(hr)) return hr; memcpy(mapped.pData,&values,sizeof(values)); context->Unmap(m_constants.Get(),0);
        D3D11_VIEWPORT viewport{0,0,float(key.output.width),float(key.output.height),0,1};
        context->RSSetViewports(1,&viewport); context->OMSetRenderTargets(1,&target,nullptr);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(m_vertex.Get(),nullptr,0); context->PSSetShader(shader,nullptr,0);
        auto constants=m_constants.Get(); context->PSSetConstantBuffers(0,1,&constants); context->PSSetShaderResources(0,1,&source);
        context->Draw(3,0);
        ID3D11ShaderResourceView* empty=nullptr; context->PSSetShaderResources(0,1,&empty); context->OMSetRenderTargets(0,nullptr,nullptr);
        return S_OK;
    }
    HRESULT QualityRenderer::Prepare(ID3D11Device* device, ID3D11DeviceContext* context,
        ID3D11ShaderResourceView* source, QualityCacheKey const& key, LensSharpness sharpness) {
        if(!device || !context || !source || key.source.Empty() || !key.output.Valid() ||
            key.output.width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            key.output.height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) return E_INVALIDARG;
        HRESULT hr=Initialize(device); if(FAILED(hr)) return hr;
        hr=Allocate(device,key.output,sharpness!=LensSharpness::Off); if(FAILED(hr)) return hr;
        // A zero stamp is deliberately uncacheable (e.g. callers without capture metadata).
        if(!m_valid || m_key!=key || !key.frame.instance || !key.frame.revision) {
            m_valid=m_sharpValid=false;
            hr=Draw(context,m_resize.Get(),m_resizedTarget.Get(),source,key,0); if(FAILED(hr)) return hr;
            m_key=key; m_valid=true; ++m_resamples;
        }
        m_sharpened=sharpness!=LensSharpness::Off;
        if(m_sharpened && (!m_sharpValid || m_strength!=sharpness)) {
            m_sharpValid=false;
            hr=Draw(context,m_cas.Get(),m_sharpTarget.Get(),m_linearView.Get(),key,SharpnessAmount(sharpness)); if(FAILED(hr)) return hr;
            m_strength=sharpness; m_sharpValid=true; ++m_sharpens;
        }
        if(!m_sharpened) { m_sharpTarget.Reset(); m_sharpView.Reset(); m_sharpValid=false; }
        return S_OK;
    }
    void QualityRenderer::Reset() noexcept {
        m_resizedTarget.Reset(); m_resizedView.Reset(); m_linearView.Reset(); m_sharpTarget.Reset(); m_sharpView.Reset();
        m_valid=m_sharpValid=m_sharpened=false; m_size={}; m_key={};
    }
}
