#include "QualityRenderer.h"
#include "QualityShaders.h"
#include "SwapChainRenderer.h"
#include "LensChrome.h"
#include "MappingStandbyPolicy.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace RegionLens::native;
using Microsoft::WRL::ComPtr;
namespace
{
    int failures{};
    void Check(bool ok,char const* name) { if(!ok) { ++failures; std::cerr<<"FAILED quality: "<<name<<'\n'; } }
    void Require(HRESULT hr) { if(FAILED(hr)) throw std::runtime_error("D3D HRESULT="+std::to_string(static_cast<unsigned>(hr))); }
    using Pixel=std::array<uint8_t,4>;
    struct Fixture {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11Texture2D> source;
        ComPtr<ID3D11ShaderResourceView> view;
        QualityRenderer renderer;
        UINT width{},height{};
        explicit Fixture(bool hardware=false) {
            Require(D3D11CreateDevice(nullptr,hardware?D3D_DRIVER_TYPE_HARDWARE:D3D_DRIVER_TYPE_WARP,nullptr,
                D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context));
        }
        void Upload(std::vector<Pixel> const& pixels,UINT w,UINT h) {
            if(!source || width!=w || height!=h) {
                source.Reset(); view.Reset();
                D3D11_TEXTURE2D_DESC d{}; d.Width=w;d.Height=h;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;
                d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                Require(device->CreateTexture2D(&d,nullptr,&source)); Require(device->CreateShaderResourceView(source.Get(),nullptr,&view));
                width=w;height=h;
            }
            context->UpdateSubresource(source.Get(),0,nullptr,pixels.data(),w*sizeof(Pixel),0);
        }
        void Prepare(QualityCacheKey const& key,LensSharpness sharp) {
            Require(renderer.Prepare(device.Get(),context.Get(),view.Get(),key,sharp));
        }
        std::vector<Pixel> Read() {
            ComPtr<ID3D11Resource> resource; renderer.View()->GetResource(&resource);
            ComPtr<ID3D11Texture2D> texture; Require(resource.As(&texture));
            D3D11_TEXTURE2D_DESC d{};texture->GetDesc(&d); d.Usage=D3D11_USAGE_STAGING;
            d.CPUAccessFlags=D3D11_CPU_ACCESS_READ; d.BindFlags=0; d.MiscFlags=0;
            ComPtr<ID3D11Texture2D> staging;Require(device->CreateTexture2D(&d,nullptr,&staging));
            context->CopyResource(staging.Get(),texture.Get());
            D3D11_MAPPED_SUBRESOURCE map{};Require(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
            std::vector<Pixel> output(size_t(d.Width)*d.Height);
            for(UINT y=0;y<d.Height;++y) memcpy(output.data()+size_t(y)*d.Width,
                static_cast<uint8_t*>(map.pData)+size_t(y)*map.RowPitch,size_t(d.Width)*sizeof(Pixel));
            context->Unmap(staging.Get(),0);return output;
        }
    };
    double Weight(int i,double t) {
        switch(i) { case 0:return -.5*t+t*t-.5*t*t*t;case 1:return 1-2.5*t*t+1.5*t*t*t;
        case 2:return .5*t+2*t*t-1.5*t*t*t;default:return -.5*t*t+.5*t*t*t; }
    }
    std::vector<Pixel> Reference(std::vector<Pixel> const& source,UINT width,PixelRect crop,RenderExtent output) {
        std::vector<Pixel> result(size_t(output.width)*output.height);
        for(UINT y=0;y<output.height;++y) for(UINT x=0;x<output.width;++x) {
            double sx=crop.x+(x+.5)*crop.width/output.width-.5, sy=crop.y+(y+.5)*crop.height/output.height-.5;
            int bx=int(floor(sx)),by=int(floor(sy)); Pixel value{0,0,0,255};
            for(int c=0;c<3;++c) {
                double sum=0,lo=255,hi=0;
                for(int j=0;j<4;++j) for(int i=0;i<4;++i) {
                    int px=std::clamp(bx+i-1,crop.x,crop.Right()-1),py=std::clamp(by+j-1,crop.y,crop.Bottom()-1);
                    auto sample=source[size_t(py)*width+px][c]; sum+=sample*Weight(i,sx-bx)*Weight(j,sy-by);
                    if(i>=1&&i<=2&&j>=1&&j<=2) { lo=std::min(lo,double(sample));hi=std::max(hi,double(sample)); }
                }
                value[c]=uint8_t(std::lround(std::clamp(sum,lo,hi)));
            }
            result[size_t(y)*output.width+x]=value;
        }
        return result;
    }
    double Decode(double c) { return c<=.04045?c/12.92:pow((c+.055)/1.055,2.4); }
    double Encode(double c) { return c<=.0031308?c*12.92:1.055*pow(c,1/2.4)-.055; }
    std::vector<Pixel> CasReference(std::vector<Pixel> const& input,RenderExtent size,float strength) {
        std::vector<Pixel> output(input.size());
        for(UINT y=0;y<size.height;++y) for(UINT x=0;x<size.width;++x) {
            std::array<std::array<double,3>,5> c{};
            const POINT offsets[]{{0,-1},{-1,0},{0,0},{1,0},{0,1}};
            double mn=1,mx=0;
            for(int i=0;i<5;++i) {
                int px=std::clamp(int(x)+offsets[i].x,0L,LONG(size.width)-1),py=std::clamp(int(y)+offsets[i].y,0L,LONG(size.height)-1);
                for(int channel=0;channel<3;++channel) c[i][channel]=Decode(input[size_t(py)*size.width+px][channel]/255.0);
                mn=std::min(mn,c[i][1]);mx=std::max(mx,c[i][1]);
            }
            double weight=-sqrt(std::clamp(std::min(mn,1-mx)/std::max(mx,1e-6),0.0,1.0))/(8-3*strength);
            Pixel pixel{0,0,0,255};
            for(int i=0;i<3;++i) {
                double linear=std::clamp(((c[0][i]+c[1][i]+c[3][i]+c[4][i])*weight+c[2][i])/(1+4*weight),0.0,1.0);
                pixel[i]=uint8_t(std::lround(Encode(linear)*255));
            }
            output[size_t(y)*size.width+x]=pixel;
        } return output;
    }
    int Difference(std::vector<Pixel> const& a,std::vector<Pixel> const& b) {
        if(a.size()!=b.size()) return 1000;
        int error=0;for(size_t i=0;i<a.size();++i) for(int c=0;c<4;++c) error=std::max(error,std::abs(int(a[i][c])-b[i][c]));return error;
    }
}
int RunQualityTests()
{
    auto first=QualityForLevel(LensSharpness::Medium);
    LensQualitySettings second;
    Check(first==NewLensQuality && first.sharpness==LensSharpness::Medium,"default quality selects 0.4 sharpening");
    Check(QualityForLevel(LensSharpness::Off)==second,"global off selects the safe original path");
    Check(second.mode==LensQualityMode::Smooth && second.sharpness==LensSharpness::Off,"empty settings still mean safe failure fallback");
    Check(!UseClearQuality(first,{0,0,100,80},{100,80},false,true),"1:1 bypasses quality processing");
    Check(!UseClearQuality(first,{0,0,100,80},{200,70},false,true),"mixed shrink/upscale bypasses");
    Check(UseClearQuality(first,{0,0,100,80},{200,80},false,true),"one enlarged axis is supported");
    Check(!UseClearQuality(first,{0,0,100,80},{200,160},true,true) &&
        !UseClearQuality(first,{0,0,100,80},{200,160},false,false),"resize and selection keep lightweight path");
    Check(UseClearQuality(first,{0,0,1,1},{7,3},false,true),"one-pixel source can enlarge");
    Check(SharpnessAmount(LensSharpness::Off)==0 && SharpnessAmount(LensSharpness::High)==.6f,"fixed sharpness levels");
    for(LONG w:{200L,219L,220L,3840L}) {
        RECT rect{0,0,w,200};
        SwapChainRenderer::ShaderConstants chrome{}; SwapChainRenderer::SetChromeLayout(chrome,rect);
        std::array renderedX{chrome.closeX,chrome.pinX,chrome.fullscreenX,chrome.restoreX,
            chrome.mappingX,chrome.pointerSpeedX,chrome.screenshotX};
        for(int i=0;i<ChromeButtonCount;++i) {
            auto p=ChromeButtonCenter(i,rect);
            Check(p.x>=ChromeRadius(w) && p.x+ChromeRadius(w)<w && HitTestChromeButton(p,rect)==ChromeButtonIds[i],"seven visible and distinct hit targets");
            Check(renderedX[i]==p.x && chrome.chromeCenterY==p.y,"GPU constants use actual shared hit-test coordinates");
            if(i) Check(ChromeButtonCenter(i-1,rect).x-p.x>2*ChromeRadius(w),"compressed buttons never overlap");
            Check(FullscreenChromeRevealAt(p,rect),"fullscreen reveals all seven button hot zones");
            MappingSessionConfig config{};config.destination=rect;config.fullscreen=true;
            Check(ProxyLocalControl(config,p),"new controls exit mapped input before native click");
        }
        Check(!FullscreenChromeRevealAt({w/2,100},rect),"fullscreen center remains chrome-free");
    }
    MappingUiPauseState pause; MappingStandbyPolicy standby;
    standby.Arm(1);standby.Arm(2);standby.CommitRoute(1);
    pause.Settings(true);standby.Suspend();pause.Selection(true);pause.Settings(false);
    Check(pause.Paused() && standby.Count()==2,"closing settings cannot unpause selection or discard standby");
    pause.Settings(true);pause.Selection(false);Check(pause.Paused(),"settings own an independent pause");
    standby.Clear();pause.Settings(false);
    Check(!pause.Paused() && standby.Resume()==0 && standby.Empty(),"global deactivation cannot be undone by settings close");
    standby.Arm(1); standby.Arm(2); standby.CommitRoute(2);
    pause.Hidden(true); standby.Suspend(); standby.Suspend();
    pause.Settings(true); pause.SpeedPopup(true); pause.Selection(true);
    pause.SpeedPopup(false); pause.Settings(false); pause.Selection(false);
    Check(pause.Paused() && pause.Hidden() && standby.Count()==2 && standby.Routed()==0 &&
        standby.Evaluate(1,ProxyPhase::Armed,false)==MappingRouteDecision::Reject,
        "hide is idempotent and independent of settings/selection/popup closure; hidden standby cannot route");
    pause.Hidden(false);
    Check(!pause.Paused() && standby.Resume()==2 && standby.Count()==2,
        "show resumes the previous candidate without losing any standby buttons");
    pause.Hidden(true);standby.Suspend();standby.Clear();standby.Suspend();pause.Hidden(false);
    Check(standby.Resume()==0 && standby.Empty(),"show cannot undo global deactivation while hidden");
    pause.Settings(true);pause.Hidden(true);pause.Hidden(false);
    Check(pause.Paused(),"show never unpauses a still-open settings dialog");
    pause.Settings(false);
    // The manager clears Hidden after the batch; only existing LensWindow
    // instances remain hidden. A later window is therefore independently usable.
    pause.Hidden(true); standby.Arm(3); standby.CommitRoute(3); standby.Suspend(); pause.Hidden(false);
    Check(!pause.Paused() && standby.Resume()==3,
        "ending a hide batch permits a subsequently created visible region to route");
    Check(QualityRenderer::ValidateShaders(),"production quality shaders compile and reflect constant layout");
    try {
        Fixture f;std::vector<Pixel> pixels(32*24);
        uint64_t revision=0;
        for(int pattern=0;pattern<5;++pattern) {
            for(int y=0;y<24;++y) for(int x=0;x<32;++x) {
                uint8_t v=uint8_t(pattern==0?90:pattern==1?((x+y)%2)*255:pattern==2?x*8:
                    pattern==3?(x>=y?210:30):((x%4==0 || y%7==0)?235:45));
                pixels[size_t(y)*32+x]={v,uint8_t(pattern==2?y*10:v),v,255};
            }
            f.Upload(pixels,32,24);
            for(auto size:{RenderExtent{10,10},RenderExtent{16,16},RenderExtent{32,32},RenderExtent{31,13}}) {
                QualityCacheKey key{{1,++revision},{3,4,8,8},size};
                f.Prepare(key,LensSharpness::Off);auto resized=f.Read();
                Check(Difference(resized,Reference(pixels,32,key.source,size))<=1,"4x4 production resample matches CPU reference");
                auto count=f.renderer.ResampleCount();
                for(auto level:{LensSharpness::Low,LensSharpness::Medium,LensSharpness::High}) {
                    f.Prepare(key,level);
                    Check(f.renderer.ResampleCount()==count,"sharpness-only changes reuse resampling");
                    Check(Difference(f.Read(),CasReference(resized,size,SharpnessAmount(level)))<=2,"linear-light CAS agrees with upstream CPU arithmetic");
                }
            }
        }
        // A hostile color surrounds the crop. Neither cubic nor CAS may leak it.
        pixels.assign(32*24,{255,0,255,255});pixels[7*32+5]={32,96,160,255};f.Upload(pixels,32,24);
        QualityCacheKey key{{2,1},{5,7,1,1},{13,17}};
        for(auto sharp:{LensSharpness::Off,LensSharpness::Low,LensSharpness::High}) {
            f.Prepare(key,sharp);Check(Difference(f.Read(),std::vector<Pixel>(13*17,{32,96,160,255}))<=1,"single-pixel crop is constant and never reads outside");
        }
        auto resamples=f.renderer.ResampleCount(),sharpens=f.renderer.SharpenCount(),bytes=f.renderer.ResourceBytes();
        for(int i=0;i<5000;++i) f.Prepare(key,LensSharpness::High);
        Check(f.renderer.ResampleCount()==resamples && f.renderer.SharpenCount()==sharpens && f.renderer.ResourceBytes()==bytes,
            "5000 cursor-only updates reuse both stages without allocating");
        auto sameView=f.view.Get();pixels[7*32+5]={190,80,10,255};f.Upload(pixels,32,24);++key.frame.revision;
        f.Prepare(key,LensSharpness::Off);
        Check(sameView==f.view.Get() && Difference(f.Read(),std::vector<Pixel>(13*17,{190,80,10,255}))==0,"new frame in SAME texture invalidates cached output");
        resamples=f.renderer.ResampleCount();++key.frame.instance;f.Prepare(key,LensSharpness::Off);
        Check(f.renderer.ResampleCount()==resamples+1,"capture instance change invalidates even with same revision");
        key.source={0,0,1,1};f.Prepare(key,LensSharpness::Off);
        Check(Difference(f.Read(),std::vector<Pixel>(13*17,{255,0,255,255}))==0,"crop change invalidates cache");
        key.output={1,1};f.Prepare(key,LensSharpness::Low);Check(f.Read().size()==1,"one-pixel output safely clamps CAS neighbors");
        key.output={D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION+1,1};
        Check(FAILED(f.renderer.Prepare(f.device.Get(),f.context.Get(),f.view.Get(),key,LensSharpness::Low)),
            "invalid GPU allocation returns failure without drawing or retaining partial cache");
        f.renderer.Reset(); key.output={7,3}; f.Prepare(key,LensSharpness::Low);
        Check(f.Read().size()==21,"recovery after resource failure can render a valid extent");
        f.renderer.Reset();Check(f.renderer.ResourceBytes()==0 && !f.renderer.View(),"turning off quality releases textures");
        key.output={};Check(f.renderer.Prepare(f.device.Get(),f.context.Get(),f.view.Get(),key,LensSharpness::Low)==E_INVALIDARG,"invalid geometry fails before GPU submission");
    } catch(std::exception const& error) {++failures;std::cerr<<"Quality GPU test: "<<error.what()<<'\n';}
    if(!failures) std::cout<<"Quality policy, crop, CAS, seven-control layout and 5000-frame cache tests passed (offscreen WARP; no desktop input).\n";
    return failures;
}

int RunQualityBenchmark()
{
    try {
        Fixture f(true);std::vector<Pixel> pixels(640*360);
        for(size_t i=0;i<pixels.size();++i) pixels[i]={uint8_t(i%251),uint8_t(i%127),uint8_t(i%199),255};
        f.Upload(pixels,640,360);
        ComPtr<ID3DBlob> vb,pb;
        if(!SwapChainRenderer::CompileFrameShaders(vb,pb)) throw std::runtime_error("production compositor compilation failed");
        ComPtr<ID3D11VertexShader> vertex; ComPtr<ID3D11PixelShader> pixel;
        Require(f.device->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vertex));
        Require(f.device->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&pixel));
        ComPtr<ID3D11Buffer> constants;
        D3D11_BUFFER_DESC bd{}; bd.ByteWidth=sizeof(SwapChainRenderer::ShaderConstants); bd.Usage=D3D11_USAGE_DYNAMIC; bd.BindFlags=D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        Require(f.device->CreateBuffer(&bd,nullptr,&constants));
        ComPtr<ID3D11SamplerState> sampler; D3D11_SAMPLER_DESC sd{}; sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP; sd.MaxLOD=D3D11_FLOAT32_MAX;
        Require(f.device->CreateSamplerState(&sd,&sampler));
        for(auto extent:{RenderExtent{1920,1080},RenderExtent{3840,2160}}) {
            QualityCacheKey key{{1,1},{0,0,640,360},extent};
            f.Prepare(key,LensSharpness::High);
            ComPtr<ID3D11Texture2D> output;ComPtr<ID3D11RenderTargetView> target;
            D3D11_TEXTURE2D_DESC td{};td.Width=extent.width;td.Height=extent.height;td.MipLevels=td.ArraySize=td.SampleDesc.Count=1;
            td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.BindFlags=D3D11_BIND_RENDER_TARGET;
            Require(f.device->CreateTexture2D(&td,nullptr,&output));Require(f.device->CreateRenderTargetView(output.Get(),nullptr,&target));
            auto composite=[&](bool clear) {
                SwapChainRenderer::ShaderConstants data{};
                data.sourceUv[2]=data.sourceUv[3]=1;
                data.destinationSize[0]=data.contentRect[2]=float(extent.width);
                data.destinationSize[1]=data.contentRect[3]=float(extent.height);
                data.sourceClamp[0]=.5f/(clear?extent.width:640);
                data.sourceClamp[1]=.5f/(clear?extent.height:360);
                data.sourceClamp[2]=1-data.sourceClamp[0];data.sourceClamp[3]=1-data.sourceClamp[1];
                D3D11_MAPPED_SUBRESOURCE mapped{};Require(f.context->Map(constants.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped));
                memcpy(mapped.pData,&data,sizeof(data));f.context->Unmap(constants.Get(),0);
                D3D11_VIEWPORT vp{0,0,float(extent.width),float(extent.height),0,1};f.context->RSSetViewports(1,&vp);
                f.context->OMSetRenderTargets(1,target.GetAddressOf(),nullptr);f.context->IASetInputLayout(nullptr);
                f.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                f.context->VSSetShader(vertex.Get(),nullptr,0);f.context->PSSetShader(pixel.Get(),nullptr,0);
                f.context->PSSetConstantBuffers(0,1,constants.GetAddressOf());f.context->PSSetSamplers(0,1,sampler.GetAddressOf());
                auto view=clear?f.renderer.View():f.view.Get();f.context->PSSetShaderResources(0,1,&view);f.context->Draw(3,0);
                ID3D11ShaderResourceView* empty=nullptr;f.context->PSSetShaderResources(0,1,&empty);f.context->OMSetRenderTargets(0,nullptr,nullptr);
            };
            for(int mode:{0,1,2}) {
                bool cache=mode==2;
                std::vector<double> times;
                for(int sample=0;sample<32;++sample) {
                    D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};ComPtr<ID3D11Query> disjoint,start,end;
                    Require(f.device->CreateQuery(&d,&disjoint));d.Query=D3D11_QUERY_TIMESTAMP;
                    Require(f.device->CreateQuery(&d,&start));Require(f.device->CreateQuery(&d,&end));
                    f.context->Begin(disjoint.Get());f.context->End(start.Get());if(!cache) ++key.frame.revision;
                    if(mode) f.Prepare(key,LensSharpness::High);
                    composite(mode!=0);f.context->End(end.Get());f.context->End(disjoint.Get());f.context->Flush();
                    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT result{};UINT64 a{},b{};auto deadline=GetTickCount64()+5000;
                    HRESULT ready{};
                    while((ready=f.context->GetData(disjoint.Get(),&result,sizeof(result),0))==S_FALSE && GetTickCount64()<deadline) Sleep(1);
                    if(ready!=S_OK) throw std::runtime_error("GPU query deadline/failed");
                    auto readStamp=[&](ID3D11Query* query, UINT64& value) {
                        HRESULT status;
                        while((status=f.context->GetData(query,&value,sizeof(value),0))==S_FALSE && GetTickCount64()<deadline) Sleep(1);
                        if(status!=S_OK) throw std::runtime_error("timestamp unavailable");
                    };
                    readStamp(start.Get(),a);readStamp(end.Get(),b);
                    if(!result.Disjoint && result.Frequency && b>a && sample>=2) times.push_back(double(b-a)*1000/result.Frequency);
                }
                if(times.empty()) { std::cout << "GPU timestamp unavailable for mode=" << mode << "; no zero-latency claim.\n"; continue; }
                std::sort(times.begin(),times.end());
                std::cout<<"Quality GPU "<<extent.width<<'x'<<extent.height<<(mode==0?" smooth":cache?" clear-cached":" clear-fresh")
                    <<" p50_ms="<<times[times.size()/2]<<" p95_ms="<<times[size_t((times.size()-1)*.95)]
                    <<" p99_ms="<<times[size_t((times.size()-1)*.99)]<<" texture_bytes="<<(mode?f.renderer.ResourceBytes():0)<<'\n';
            }
        }
        return 0;
    } catch(std::exception const& e) {std::cerr<<"Quality benchmark failed: "<<e.what()<<'\n';return 1;}
}
