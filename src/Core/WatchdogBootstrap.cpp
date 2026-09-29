#include "pch.h"
#include "WatchdogBootstrap.h"
#include "DiagnosticScope.h"
#include <bcrypt.h>
#include <sddl.h>
#include <shellapi.h>
#include <limits>
#include <vector>

namespace RegionLens::native
{
    namespace
    {
        struct OwnedHandle
        {
            HANDLE value{};
            ~OwnedHandle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
        };
        struct LocalMemory
        {
            HLOCAL value{};
            ~LocalMemory() { if (value) LocalFree(value); }
        };
        bool Fail(ProxyStartupFailure& failure, ProxyStartupStage stage, DWORD error)
        {
            failure = { stage, error ? error : ERROR_GEN_FAILURE };
            Record(DiagnosticEvent::StartupFailure, { LONG(stage), failure.error }, true);
            return false;
        }
        std::wstring PipeName(DWORD pid, std::wstring_view nonce)
        {
            return L"\\\\.\\pipe\\RegionLens." + std::to_wstring(pid) + L"." + std::wstring(nonce);
        }
        bool SameImage(HANDLE process)
        {
            std::vector<wchar_t> own(32768), other(32768);
            DWORD size = static_cast<DWORD>(other.size());
            auto length = GetModuleFileNameW(nullptr, own.data(), static_cast<DWORD>(own.size()));
            return length && length < own.size() &&
                QueryFullProcessImageNameW(process, 0, other.data(), &size) && _wcsicmp(own.data(), other.data()) == 0;
        }
        bool SameProcess(HANDLE first, HANDLE second)
        {
            FILETIME a{}, b{}, exit{}, kernel{}, user{};
            return GetProcessTimes(first, &a, &exit, &kernel, &user) &&
                GetProcessTimes(second, &b, &exit, &kernel, &user) &&
                CompareFileTime(&a, &b) == 0 && GetProcessId(first) == GetProcessId(second);
        }
        bool PipeSecurity(LocalMemory& descriptor)
        {
            OwnedHandle token;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return false;
            DWORD bytes{};
            GetTokenInformation(token.value, TokenLogonSid, nullptr, 0, &bytes);
            if (!bytes) return false;
            std::vector<BYTE> storage(bytes);
            if (!GetTokenInformation(token.value, TokenLogonSid, storage.data(), bytes, &bytes)) return false;
            auto groups = reinterpret_cast<TOKEN_GROUPS*>(storage.data());
            if (groups->GroupCount != 1) { SetLastError(ERROR_INVALID_SID); return false; }
            LPWSTR sid{};
            if (!ConvertSidToStringSidW(groups->Groups[0].Sid, &sid)) return false;
            LocalMemory sidMemory{ sid };
            // Restrict to this logon session and LocalSystem, not Everyone or
            // all sessions of this account. Remote clients are also rejected.
            auto sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;" + std::wstring(sid) + L")";
            return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                reinterpret_cast<PSECURITY_DESCRIPTOR*>(&descriptor.value), nullptr) != FALSE;
        }
        // Cancel and drain before OVERLAPPED/event storage can be destroyed.
        bool CompleteIo(HANDLE pipe, OVERLAPPED& operation, HANDLE peer, DWORD timeout, DWORD& transferred)
        {
            HANDLE waits[]{ operation.hEvent, peer };
            auto wait = WaitForMultipleObjects(peer ? 2 : 1, waits, FALSE, timeout);
            if (wait == WAIT_OBJECT_0)
                return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
            DWORD error = wait == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                (wait == WAIT_OBJECT_0 + 1 ? ERROR_PROCESS_ABORTED : StartupError());
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &transferred, TRUE);
            SetLastError(error);
            return false;
        }
        bool Transfer(HANDLE pipe, HANDLE peer, void* packet, DWORD bytes, bool write, DWORD timeout)
        {
            OwnedHandle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (!event.value) return false;
            OVERLAPPED operation{}; operation.hEvent = event.value;
            DWORD transferred{};
            bool complete = (write ? WriteFile(pipe, packet, bytes, &transferred, &operation) :
                ReadFile(pipe, packet, bytes, &transferred, &operation)) != FALSE;
            if (!complete && GetLastError() == ERROR_IO_PENDING)
                complete = CompleteIo(pipe, operation, peer, timeout, transferred);
            if (!complete) return false;
            if (transferred != bytes) { SetLastError(ERROR_INVALID_DATA); return false; }
            return true;
        }
    }
    bool ParseBootstrapIdentity(std::wstring_view pid, std::wstring_view nonce, DWORD& parentId) noexcept
    {
        parentId = 0;
        if (pid.empty() || pid.size() > 10 || nonce.size() != 32) return false;
        uint64_t value{};
        for (auto c : pid)
        {
            if (c < L'0' || c > L'9') return false;
            value = value * 10 + (c - L'0');
        }
        for (auto c : nonce) if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f'))) return false;
        if (!value || value > (std::numeric_limits<DWORD>::max)()) return false;
        parentId = static_cast<DWORD>(value);
        return true;
    }
    bool ValidBootstrapPacket(BootstrapPacket const& packet, DWORD parentId, DWORD childId) noexcept
    {
        if (packet.magic != 0x524C4233 || packet.version != 3 || packet.parentId != parentId ||
            packet.childId != childId || !parentId || !childId || parentId == childId) return false;
        for (size_t i = 0; i < packet.handles.size(); ++i)
        {
            if (!packet.handles[i] || packet.handles[i] >= (std::numeric_limits<uintptr_t>::max)()) return false;
            for (size_t j = 0; j < i; ++j) if (packet.handles[i] == packet.handles[j]) return false;
        }
        return true;
    }
    void CloseBootstrapHandles(BootstrapHandles& handles) noexcept
    {
        for (auto& handle : handles) if (handle) CloseHandle(std::exchange(handle, nullptr));
    }
    bool LaunchWatchdogBootstrap(BootstrapHandles const& handles, HANDLE& childProcess,
        ProxyStartupFailure& failure, wchar_t const* mode, DWORD timeout)
    {
        using Stage = ProxyStartupStage;
        DiagnosticScope start(DiagnosticStage::GuardStart);
        failure = {}; childProcess = nullptr;
        if (!mode || wcscmp(mode, L"--input-watchdog"))
            return Fail(failure, Stage::ShellLaunch, ERROR_INVALID_PARAMETER);
        for (auto handle : handles)
        {
            DWORD flags{};
            if (!handle || !GetHandleInformation(handle, &flags)) return Fail(failure, Stage::HandleTransfer, ERROR_INVALID_HANDLE);
        }
        std::array<BYTE, 16> random{};
        auto status = BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) return Fail(failure, Stage::PipeSecurity, ERROR_GEN_FAILURE);
        std::wstring nonce;
        for (auto byte : random) { nonce += L"0123456789abcdef"[byte >> 4]; nonce += L"0123456789abcdef"[byte & 15]; }
        auto parentId = GetCurrentProcessId();
        auto name = PipeName(parentId, nonce);
        LocalMemory descriptor;
        if (!PipeSecurity(descriptor)) return Fail(failure, Stage::PipeSecurity, StartupError());
        SECURITY_ATTRIBUTES security{ sizeof(security), descriptor.value, FALSE };
        DiagnosticScope pipeCreate(DiagnosticStage::PipeCreate);
        OwnedHandle pipe{ CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, sizeof(BootstrapPacket), sizeof(BootstrapPacket), 0, &security) };
        if (pipe.value == INVALID_HANDLE_VALUE) return Fail(failure, Stage::PipeCreate, StartupError());
        pipeCreate.End();
        // Activation may dispatch nested UI messages. Keep large path buffers
        // off the UI stack in addition to the coordinator lifecycle guard.
        std::vector<wchar_t> module(32768);
        auto moduleLength = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
        if (!moduleLength || moduleLength >= module.size()) return Fail(failure, Stage::ShellLaunch, StartupError(ERROR_INSUFFICIENT_BUFFER));
        auto parameters = std::wstring(mode) + L" " + std::to_wstring(parentId) + L" " + nonce;
        SHELLEXECUTEINFOW launch{ sizeof(launch) };
        launch.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI | SEE_MASK_NO_CONSOLE;
        launch.lpVerb = L"open"; launch.lpFile = module.data(); launch.lpParameters = parameters.c_str(); launch.nShow = SW_HIDE;
        // Never use runas: retain asInvoker + UIAccess and let the Windows
        // activation broker establish the correct token for the signed image.
        DiagnosticScope shell(DiagnosticStage::ShellLaunch);
        if (!ShellExecuteExW(&launch)) { auto error = StartupError(); shell.End(error); return Fail(failure, Stage::ShellLaunch, error); }
        shell.End(0, launch.hProcess ? GetProcessId(launch.hProcess) : 0);
        childProcess = launch.hProcess;
        if (!childProcess) return Fail(failure, Stage::ShellLaunch, ERROR_INVALID_HANDLE);
        DWORD childId = GetProcessId(childProcess);
        DiagnosticScope identity(DiagnosticStage::ChildIdentity, childId);
        OwnedHandle duplicateTarget{ OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, childId) };
        if (!duplicateTarget.value) return Fail(failure, Stage::ProcessIdentity, StartupError());
        if (!SameProcess(childProcess, duplicateTarget.value) || !SameImage(duplicateTarget.value))
            return Fail(failure, Stage::ProcessIdentity, ERROR_ACCESS_DENIED);
        identity.End();
        DiagnosticScope connect(DiagnosticStage::PipeConnect, childId);
        OwnedHandle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!event.value) return Fail(failure, Stage::PipeConnect, StartupError());
        OVERLAPPED operation{}; operation.hEvent = event.value;
        bool connected = ConnectNamedPipe(pipe.value, &operation) != FALSE;
        if (!connected)
        {
            DWORD error = GetLastError(), transferred{};
            if (error == ERROR_PIPE_CONNECTED) connected = true;
            else if (error == ERROR_IO_PENDING) connected = CompleteIo(pipe.value, operation, childProcess, timeout, transferred);
        }
        if (!connected) return Fail(failure, Stage::PipeConnect, StartupError());
        ULONG connectedId{};
        if (!GetNamedPipeClientProcessId(pipe.value, &connectedId) || connectedId != childId)
            return Fail(failure, Stage::ProcessIdentity, ERROR_ACCESS_DENIED);
        connect.End();
        DiagnosticScope transfer(DiagnosticStage::HandleTransfer, childId);
        BootstrapPacket packet; packet.parentId = parentId; packet.childId = childId;
        for (size_t i = 0; i < handles.size(); ++i)
        {
            HANDLE remote{};
            if (!DuplicateHandle(GetCurrentProcess(), handles[i], duplicateTarget.value, &remote, 0, FALSE, DUPLICATE_SAME_ACCESS))
                return Fail(failure, Stage::HandleTransfer, StartupError());
            // On failure the child observes a broken pipe and exits; the OS
            // closes any handles transferred before that failure.
            packet.handles[i] = reinterpret_cast<uintptr_t>(remote);
        }
        if (!Transfer(pipe.value, childProcess, &packet, sizeof(packet), true, timeout))
            return Fail(failure, Stage::HandleTransfer, StartupError());
        // Keep the server alive until the child has read the handles and
        // acknowledged readiness. No unbounded FlushFileBuffers or pipe close
        // race with the child's first ReadFile.
        transfer.End();
        DiagnosticScope readyStage(DiagnosticStage::GuardReady, childId);
        HANDLE waits[]{ handles[3], childProcess };
        auto ready = WaitForMultipleObjects(2, waits, FALSE, timeout);
        auto waitError = GetLastError();
        RecordProcessExit(childProcess, false, WaitForSingleObject(childProcess, 0));
        readyStage.End(ready == WAIT_FAILED ? waitError : ready); SetLastError(waitError);
        if (ready != WAIT_OBJECT_0)
        {
            DWORD error = ready == WAIT_TIMEOUT ? ERROR_TIMEOUT : StartupError();
            if (ready == WAIT_OBJECT_0 + 1)
            {
                error = ERROR_PROCESS_ABORTED;
                GetExitCodeProcess(childProcess, &error);
                return Fail(failure, Stage::GuardExit, error);
            }
            return Fail(failure, Stage::GuardReady, error);
        }
        start.End(0, childId);
        return true;
    }
    bool ReceiveWatchdogBootstrap(int argc, wchar_t** argv, BootstrapHandles& handles,
        ProxyStartupFailure& failure, DWORD timeout)
    {
        using Stage = ProxyStartupStage;
        DiagnosticScope receive(DiagnosticStage::GuardReceive);
        failure = {}; handles = {};
        DWORD parentId{};
        if (argc != 4 || !ParseBootstrapIdentity(argv[2], argv[3], parentId) || parentId == GetCurrentProcessId())
            return Fail(failure, Stage::ProcessIdentity, ERROR_INVALID_PARAMETER);
        OwnedHandle parent{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parentId) };
        if (!parent.value) return Fail(failure, Stage::ProcessIdentity, StartupError());
        if (!SameImage(parent.value) || WaitForSingleObject(parent.value, 0) != WAIT_TIMEOUT)
            return Fail(failure, Stage::ProcessIdentity, ERROR_ACCESS_DENIED);
        DiagnosticScope connect(DiagnosticStage::PipeConnect, parentId);
        auto name = PipeName(parentId, argv[3]);
        OwnedHandle pipe{ CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr) };
        if (pipe.value == INVALID_HANDLE_VALUE) return Fail(failure, Stage::PipeConnect, StartupError());
        ULONG serverId{};
        if (!GetNamedPipeServerProcessId(pipe.value, &serverId) || serverId != parentId)
            return Fail(failure, Stage::ProcessIdentity, ERROR_ACCESS_DENIED);
        connect.End();
        DiagnosticScope transfer(DiagnosticStage::HandleTransfer, parentId);
        BootstrapPacket packet{};
        if (!Transfer(pipe.value, parent.value, &packet, sizeof(packet), false, timeout))
            return Fail(failure, Stage::HandleTransfer, StartupError());
        if (!ValidBootstrapPacket(packet, parentId, GetCurrentProcessId()))
            return Fail(failure, Stage::HandleTransfer, ERROR_INVALID_DATA);
        for (size_t i = 0; i < handles.size(); ++i)
        {
            handles[i] = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(packet.handles[i]));
            DWORD flags{};
            if (!GetHandleInformation(handles[i], &flags) || (flags & HANDLE_FLAG_INHERIT))
            { CloseBootstrapHandles(handles); return Fail(failure, Stage::HandleTransfer, ERROR_INVALID_HANDLE); }
        }
        if (!SameProcess(parent.value, handles[1]))
        { CloseBootstrapHandles(handles); return Fail(failure, Stage::ProcessIdentity, ERROR_ACCESS_DENIED); }
        transfer.End(); receive.End(0, parentId);
        return true;
    }
}
