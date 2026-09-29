#include "pch.h"
#include "SwapChainRenderer.h"
#include "LensChrome.h"
#include <iostream>
#include <vector>
#include <stdexcept>

namespace
{
    // Real production shaders on offscreen WARP, not a screenshot or desktop
    // window. Also checks that the new layout cannot leak into clean exports.
    bool TestChromePixels()
    {
        using namespace RegionLens::native;
        using Microsoft::WRL::ComPtr;
        auto require=[](HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("offscreen chrome HRESULT="+std::to_string(unsigned(hr))); };
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        require(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        ComPtr<ID3DBlob> vsCode,psCode;
        if(!SwapChainRenderer::CompileFrameShaders(vsCode,psCode)) return false;
        ComPtr<ID3D11VertexShader> vs;ComPtr<ID3D11PixelShader> ps;
        require(device->CreateVertexShader(vsCode->GetBufferPointer(),vsCode->GetBufferSize(),nullptr,&vs));
        require(device->CreatePixelShader(psCode->GetBufferPointer(),psCode->GetBufferSize(),nullptr,&ps));
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        uint32_t gray=0xff404040;D3D11_SUBRESOURCE_DATA data{&gray,4,0};
        ComPtr<ID3D11Texture2D> source;ComPtr<ID3D11ShaderResourceView> view;
        require(device->CreateTexture2D(&desc,&data,&source));require(device->CreateShaderResourceView(source.Get(),nullptr,&view));
        D3D11_SAMPLER_DESC sampling{};sampling.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU=sampling.AddressV=sampling.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sampling.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;require(device->CreateSamplerState(&sampling,&sampler));
        D3D11_BUFFER_DESC bufferDesc{};bufferDesc.ByteWidth=sizeof(SwapChainRenderer::ShaderConstants);bufferDesc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
        ComPtr<ID3D11Buffer> buffer;require(device->CreateBuffer(&bufferDesc,nullptr,&buffer));
        context->VSSetShader(vs.Get(),nullptr,0);context->PSSetShader(ps.Get(),nullptr,0);
        auto sourceView=view.Get();auto sampleState=sampler.Get();auto constants=buffer.Get();
        context->PSSetShaderResources(0,1,&sourceView);context->PSSetSamplers(0,1,&sampleState);
        context->PSSetConstantBuffers(0,1,&constants);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        bool passed=true;
        for(LONG width:{200L,300L,800L}) {
            desc.Width=width;desc.Height=128;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> target,staging;ComPtr<ID3D11RenderTargetView> rtv;
            require(device->CreateTexture2D(&desc,nullptr,&target));require(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
            desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            require(device->CreateTexture2D(&desc,nullptr,&staging));desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;
            auto renderTarget=rtv.Get();context->OMSetRenderTargets(1,&renderTarget,nullptr);
            D3D11_VIEWPORT viewport{0,0,float(width),128,0,1};context->RSSetViewports(1,&viewport);
            for(bool chrome:{false,true}) for(bool matching:{false,true}) for(bool fullscreen:{false,true})
            for(bool feedback:{false,true,false}) { // Normal -> successful flash -> restored, including fullscreen.
                SwapChainRenderer::ShaderConstants values{};RECT client{0,0,width,128};
                SwapChainRenderer::SetChromeLayout(values,client);
                values.destinationSize[0]=float(width);values.destinationSize[1]=128;
                values.contentRect[2]=float(width);values.contentRect[3]=128;
                for(auto& v:values.sourceClamp) v=0.5f;
                values.sourceUv[2]=values.sourceUv[3]=1;values.chromeMode=chrome?1.0f:0.0f;
                values.pointerSpeedMode=matching?1.0f:0.0f;values.fullscreenMode=fullscreen?1.0f:0.0f;
                values.screenshotFeedback=feedback?1.0f:0.0f;
                context->UpdateSubresource(buffer.Get(),0,nullptr,&values,0,0);context->Draw(3,0);
                context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
                require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped)); // Waiting is test-only.
                auto pixel=[&](LONG x,LONG y){return static_cast<uint8_t const*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;};
                for(int id:ChromeButtonIds) {
                    auto center=ChromeButtonCenterFor(id,client);auto p=pixel(center.x,center.y+9);
                    bool blue=chrome && ((id==PointerSpeedButtonId && matching) || (id==FullscreenButtonId && fullscreen));
                    bool flash=chrome && feedback && id==ScreenshotButtonId;
                    passed &= flash ? p[0]==0 && p[1]>=119 && p[1]<=121 && p[2]>=213 && p[2]<=215 :
                        chrome ? (blue ? p[0]<2 && p[2]>180 : p[0]>=30 && p[0]<=32 && p[2]>=40 && p[2]<=42) :
                        p[0]==64 && p[1]==64 && p[2]==64;
                }
                for (auto point : {POINT{0,70}, POINT{width-1,70}, POINT{width/2,0}, POINT{width/2,127}}) {
                    auto edge=pixel(point.x,point.y);
                    passed &= (!chrome || fullscreen) ? edge[0]==64 && edge[2]==64 : edge[0]==0 && edge[2]>210;
                }
                if(!chrome) for(LONG y=0;y<128;++y) for(LONG x=0;x<width;++x) {
                    auto p=pixel(x,y);passed &= p[0]==64 && p[1]==64 && p[2]==64; }
                context->Unmap(staging.Get(),0);
            }
            // The optional fullscreen content rectangle letterboxes the source,
            // while chrome is still composited in client coordinates above it.
            SwapChainRenderer::ShaderConstants values{};RECT client{0,0,width,128};
            SwapChainRenderer::SetChromeLayout(values,client);
            values.destinationSize[0]=float(width);values.destinationSize[1]=128;
            values.contentRect[0]=float(width/4);values.contentRect[1]=16;
            values.contentRect[2]=float(width/2);values.contentRect[3]=96;
            for(auto& v:values.sourceClamp) v=0.5f;
            values.sourceUv[2]=values.sourceUv[3]=1;
            context->UpdateSubresource(buffer.Get(),0,nullptr,&values,0,0);context->Draw(3,0);
            context->CopyResource(staging.Get(),target.Get());D3D11_MAPPED_SUBRESOURCE mapped{};
            require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            auto pixel=[&](LONG x,LONG y){return static_cast<uint8_t const*>(mapped.pData)+size_t(y)*mapped.RowPitch+size_t(x)*4;};
            auto black=[&](LONG x,LONG y){auto p=pixel(x,y);return p[0]==0&&p[1]==0&&p[2]==0&&p[3]==255;};
            auto grayAt=[&](LONG x,LONG y){auto p=pixel(x,y);return p[0]==64&&p[1]==64&&p[2]==64&&p[3]==255;};
            passed &= black(2,64) && black(width-3,64) && black(width/2,2) && black(width/2,125) && grayAt(width/2,64);
            context->Unmap(staging.Get(),0);

            values.chromeMode=values.fullscreenMode=1;
            context->UpdateSubresource(buffer.Get(),0,nullptr,&values,0,0);context->Draw(3,0);
            context->CopyResource(staging.Get(),target.Get());
            require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&mapped));
            auto close=ChromeButtonCenterFor(CloseButtonId,client);auto p=pixel(close.x,close.y+9);
            passed &= p[0]>=30&&p[0]<=32&&p[2]>=40&&p[2]<=42 && black(2,64);
            context->Unmap(staging.Get(),0);
        }
        return passed;
    }
}

int RunShaderTests()
{
    // Compiles production HLSL in memory. Creates no windows, hooks, devices,
    // cursor changes, or injected input.
    if (!RegionLens::native::SwapChainRenderer::ValidateShaders())
    {
        std::cerr << "FAILED: production video/chrome/cursor shaders\n";
        return 1;
    }
    try {
        if(!TestChromePixels()) { std::cerr<<"FAILED: seven-button GPU layout/state or clean-frame pixels\n";return 1; }
    } catch(std::exception const& error) { std::cerr<<error.what()<<'\n';return 1; }
    std::cout << "Production shader compilation and offscreen seven-button/screenshot-feedback/clean-frame tests passed.\n";
    return 0;
}
