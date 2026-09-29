#include "pch.h"
#include "LensManager.h"
#include "PresentationWait.h"
#include <iostream>
#include <thread>

using namespace RegionLens::native;
namespace RegionLens::native
{
    struct PresentationSchedulerTestAccess
    {
        static LensWindow& Lens(LensManager& manager,uint64_t id) {return *manager.m_lenses.at(id);}
        static uint64_t Count(LensWindow& lens) {return lens.m_renderer.m_presentedCount;}
        static CaptureStamp Frame(LensWindow& lens) {return lens.m_renderer.m_presentedFrame;}
        static void Pointer(LensWindow& lens) {lens.m_chromeVisible=!lens.m_chromeVisible;lens.RenderLatestFrame();}
        static bool BusyTransition(LensWindow& lens)
        {
            auto previous=std::move(lens.m_mappingGeometryChangedCallback);
            auto enabled=lens.m_inputMappingEnabled;
            int restored{};
            lens.m_inputMappingEnabled=true;
            lens.m_mappingGeometryChangedCallback=[&](uint64_t) {++restored;};
            lens.m_fullscreenTransition.Begin();lens.m_fullscreenTransition.OnBusy(1);lens.m_fullscreenTransition.OnBusy(17);
            lens.CancelFullscreenTransition(false,true);
            lens.CancelFullscreenTransition(false,true);
            lens.m_inputMappingEnabled=enabled;
            lens.m_mappingGeometryChangedCallback=std::move(previous);
            return restored==1 && !lens.m_fullscreenTransition.Active();
        }
    };
}
namespace
{
    int failures{};
    void Check(bool value,char const* text) {if(!value){++failures;std::cerr<<"FAILED presentation scheduler: "<<text<<'\n';}}
    class FakeEngine final : public IInputMappingEngine
    {
    public:
        bool Start() override {return true;}
        void Stop() override {}
        void Configure(MappingSessionConfig,bool) override {}
        bool DisableAndDrain() override {return true;}
        void Acknowledge(uint64_t,bool) override {}
        ProxyUiUpdate TakeUiUpdate() override {return {};}
        bool GuardHealthy() const noexcept override {return true;}
        ProxyStartupFailure LastFailure() const noexcept override {return {};}
    };
    void Pump()
    {MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
}
int RunPresentationSchedulerTests()
{
    auto apartment=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    constexpr UINT readyMessage=WM_APP+601;
    HWND messageWindow=CreateWindowExW(0,L"STATIC",L"wait-test",0,0,0,0,0,HWND_MESSAGE,nullptr,nullptr,nullptr);
    {
        PresentationWait wait;wait.Target(messageWindow,readyMessage);
        HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Check(wait.Attach(event),"wait owns one signal handle");
        auto generation=wait.Generation();
        Check(wait.Poll()==PresentationWait::Result::Pending,"unsignalled handle arms once without blocking UI");
        SetEvent(event);
        MSG message{};auto deadline=GetTickCount64()+1000;
        while(GetTickCount64()<deadline && !PeekMessageW(&message,messageWindow,readyMessage,readyMessage,PM_REMOVE)) Sleep(1);
        Check(message.message==readyMessage && wait.Acknowledge(message.wParam),"worker wake identifies current generation");
        Check(wait.Poll()==PresentationWait::Result::Ready,"callback preserves consumed auto-reset signal as one token");
        wait.Consume();Check(wait.Poll()==PresentationWait::Result::Pending,"next frame cannot reuse consumed token");
        wait.Suspend();Check(!wait.Acknowledge(generation),"hidden/recreated surface rejects old wake");
    }
    {
        // A real, owned event without SYNCHRONIZE rights fails the zero-time
        // readiness poll; no invalid-handle race or desktop input is involved.
        auto original=CreateEventW(nullptr,FALSE,FALSE,nullptr);HANDLE restricted{};
        Check(DuplicateHandle(GetCurrentProcess(),original,GetCurrentProcess(),&restricted,
            EVENT_MODIFY_STATE,FALSE,0)!=FALSE,"create non-waitable test handle");
        CloseHandle(original);
        PresentationWait wait;Check(wait.Attach(restricted),"attach owned restricted handle");
        Check(wait.Poll()==PresentationWait::Result::Failed && wait.Error()==ERROR_ACCESS_DENIED,
            "wait failure is not mistaken for busy or presentation-ready");
        wait.Reset();Check(wait.Poll()==PresentationWait::Result::Ready,"reset clears prior wait error");
    }
    // Warm up the Windows thread pool before measuring owned-handle lifetime.
    DWORD before{},after{};GetProcessHandleCount(GetCurrentProcess(),&before);
    for(int i=0;i<150;++i) {
        PresentationWait wait;wait.Target(messageWindow,readyMessage);
        auto event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        Check(wait.Attach(event),"repeated attachment succeeds");wait.Poll();
        if(i%2) SetEvent(event);
        wait.Reset(); // Joins callbacks before closing handles, including signalled races.
    }
    GetProcessHandleCount(GetCurrentProcess(),&after);
    Check(after<=before+2,"wait create/cancel/replacement does not leak one handle per frame/surface");
    DestroyWindow(messageWindow);Pump();
    {
        // Queue a synthetic thread message and explicitly mark it observed.
        // This is not keyboard/mouse input and cannot reach a desktop app.
        MSG msg{};PeekMessageW(&msg,nullptr,0,0,PM_NOREMOVE);
        PostThreadMessageW(GetCurrentThreadId(),WM_APP+602,0,0);
        PeekMessageW(&msg,nullptr,WM_APP+602,WM_APP+602,PM_NOREMOVE);
        Check(MsgWaitForMultipleObjectsEx(0,nullptr,0,QS_ALLINPUT,MWMO_INPUTAVAILABLE)==WAIT_OBJECT_0,
            "previously observed pending message wakes without a 16ms timeout");
        PeekMessageW(&msg,nullptr,WM_APP+602,WM_APP+602,PM_REMOVE);
    }
    try {
        auto device=std::make_shared<D3DDevice>();Check(device->Initialize(),"test D3D device");
        auto coordinator=std::make_shared<InputMappingCoordinator>(nullptr,InputMappingCoordinator::NotificationCallback{},
            InputMappingDependencies{[](HWND){return std::make_unique<FakeEngine>();},[]{return true;}});
        coordinator->Initialize();
        LensManager manager(device,coordinator,{});bool requested{};
        manager.SetPresentationRequest([&]{requested=true;});manager.SetQualityLevel(LensSharpness::Off);
        auto monitor=MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY);
        std::array<uint64_t,3> ids{};
        for(int i=0;i<3;++i) {
            auto id=manager.Create(monitor,{0,0,32,32},{-24000+i*400,-24000,-23700+i*400,-23800});
            Check(id.has_value(),"owned offscreen scheduled lens created");if(!id) throw std::runtime_error("lens failed");ids[i]=*id;
        }
        D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=32;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        winrt::check_hresult(device->Device()->CreateTexture2D(&desc,nullptr,&texture));
        winrt::check_hresult(device->Device()->CreateShaderResourceView(texture.Get(),nullptr,&view));
        std::array<uint32_t,1024> pixels{};
        for(uint64_t revision=1;revision<=90;++revision) {
            pixels.fill(0xff000000u|uint32_t(revision*193));
            device->Context()->UpdateSubresource(texture.Get(),0,nullptr,pixels.data(),128,0);
            auto& lens=PresentationSchedulerTestAccess::Lens(manager,ids[0]);
            auto beforeCount=PresentationSchedulerTestAccess::Count(lens);
            for(int input=0;input<30;++input) PresentationSchedulerTestAccess::Pointer(lens);
            manager.RenderMonitor(monitor,view.Get(),32,32,{771,revision});
            Check(PresentationSchedulerTestAccess::Count(lens)==beforeCount,"cursor and fresh capture do not submit separately");
            Check(requested,"dirty state requests a batch");requested=false;
            manager.FlushPresentations();
            Check(PresentationSchedulerTestAccess::Count(lens)<=beforeCount+1,"a dirty region submits at most once per batch");
            Pump();
        }
        auto deadline=GetTickCount64()+2000;
        bool latest{};
        while(GetTickCount64()<deadline && !latest) {
            Pump();manager.FlushPresentations();latest=true;
            for(auto id:ids) latest &= PresentationSchedulerTestAccess::Frame(PresentationSchedulerTestAccess::Lens(manager,id)).revision==90;
            if(!latest) Sleep(1);
        }
        Check(latest,"all three regions eventually present latest colour frame without historical backlog");
        auto& lens=PresentationSchedulerTestAccess::Lens(manager,ids[0]);
        Check(PresentationSchedulerTestAccess::BusyTransition(lens),
            "second busy fullscreen attempt restores mapping exactly once");
        auto oldCount=PresentationSchedulerTestAccess::Count(lens);
        PresentationSchedulerTestAccess::Pointer(lens);
        deadline=GetTickCount64()+1000;
        while(GetTickCount64()<deadline && PresentationSchedulerTestAccess::Count(lens)==oldCount) {Pump();manager.FlushPresentations();Sleep(1);}
        Check(PresentationSchedulerTestAccess::Count(lens)>oldCount,"static content updates chrome/cursor without a capture event");
        manager.HideAll();manager.MarkMonitorDirty(monitor);manager.FlushPresentations();
        oldCount=PresentationSchedulerTestAccess::Count(lens);
        for(int i=0;i<10;++i){Pump();manager.FlushPresentations();}
        Check(PresentationSchedulerTestAccess::Count(lens)==oldCount,"hidden regions do not submit queued callbacks");
        manager.CloseAll();Pump();
    } catch(std::exception const& e) {++failures;std::cerr<<"scheduler exception: "<<e.what()<<'\n';}
    if(SUCCEEDED(apartment)) CoUninitialize();
    if(!failures) std::cout<<"Presentation coalescing, latest frames, static updates, wait lifetime and input wake tests passed.\n";
    return failures;
}
