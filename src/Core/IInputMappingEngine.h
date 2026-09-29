#pragma once
#include "MouseProxyCore.h"
#include "ProxyStartup.h"

namespace RegionLens::native
{
    struct ProxySurfaceRequest { uint64_t number{}; bool transparent{}; };
    struct ProxyUiUpdate
    {
        CursorSnapshot cursor;
        std::optional<ProxySurfaceRequest> surfaces;
        bool externalCursorMoved{};
        POINT externalCursorPosition{};
    };
    // UI-side lifecycle boundary. Tests supply a passive engine; the real mouse
    // hook/backend remain private to MouseProxyEngine and are not virtualized here.
    class IInputMappingEngine
    {
    public:
        virtual ~IInputMappingEngine() = default;
        virtual bool Start() = 0;
        virtual void Stop() = 0;
        virtual void Configure(MappingSessionConfig, bool probeCursor = false) = 0;
        virtual bool DisableAndDrain() = 0;
        virtual void Acknowledge(uint64_t, bool) = 0;
        virtual ProxyUiUpdate TakeUiUpdate() = 0;
        virtual bool BeginSoftwareCursor() { return true; }
        virtual bool EndSoftwareCursor() { return true; }
        virtual void SoftwareCursorHeartbeat() noexcept {}
        virtual bool NativeCursorShowConfirmed() const noexcept { return true; }
        virtual bool GuardHealthy() const noexcept = 0;
        virtual ProxyStartupFailure LastFailure() const noexcept = 0;
    };
}
