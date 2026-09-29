#include "WeTypeProbeHost.h"
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstring>

namespace RegionLens::weTypeCompat
{
    namespace
    {
        struct Candidate
        {
            HWND window{};
            DWORD pid{}, thread{};
            uint64_t creation{};
        };

        bool SignedTencentRenderer(std::wstring const& path) noexcept
        {
            HCERTSTORE store{};
            HCRYPTMSG message{};
            try
            {
            WINTRUST_FILE_INFO file{};
            file.cbStruct = sizeof(file);
            file.pcwszFilePath = path.c_str();
            WINTRUST_DATA trust{};
            trust.cbStruct = sizeof(trust);
            trust.dwUIChoice = WTD_UI_NONE;
            trust.fdwRevocationChecks = WTD_REVOKE_NONE;
            trust.dwUnionChoice = WTD_CHOICE_FILE;
            trust.pFile = &file;
            trust.dwStateAction = WTD_STATEACTION_VERIFY;
            trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
            GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
            auto status = WinVerifyTrust(nullptr, &policy, &trust);
            trust.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(nullptr, &policy, &trust);
            if (status != ERROR_SUCCESS) return false;

            DWORD encoding{}, content{}, format{};
            bool queried = CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(),
                CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY,
                0, &encoding, &content, &format, &store, &message, nullptr) != FALSE;
            bool expected{};
            if (queried)
            {
                DWORD size{};
                if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, nullptr, &size) && size && size < 65536)
                {
                    std::vector<BYTE> data(size);
                    if (CryptMsgGetParam(message, CMSG_SIGNER_INFO_PARAM, 0, data.data(), &size))
                    {
                        auto signer = reinterpret_cast<PCMSG_SIGNER_INFO>(data.data());
                        CERT_INFO identity{};
                        identity.Issuer = signer->Issuer;
                        identity.SerialNumber = signer->SerialNumber;
                        auto cert = CertFindCertificateInStore(store, encoding, 0,
                            CERT_FIND_SUBJECT_CERT, &identity, nullptr);
                        if (cert)
                        {
                            wchar_t subject[256]{};
                            CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0,
                                nullptr, subject, static_cast<DWORD>(std::size(subject)));
                            expected = wcscmp(subject, L"Tencent Technology (Shenzhen) Company Limited") == 0;
                            CertFreeCertificateContext(cert);
                        }
                    }
                }
            }
            if (message) CryptMsgClose(message);
            if (store) CertCloseStore(store, 0);
            return expected;
            }
            catch (...)
            {
                if (message) CryptMsgClose(message);
                if (store) CertCloseStore(store, 0);
                return false;
            }
        }

        bool VerifyProcess(DWORD pid, uint64_t& created) noexcept
        {
            try
            {
            auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return false;
            wchar_t image[32768]{};
            DWORD length = static_cast<DWORD>(std::size(image));
            FILETIME start{}, end{}, kernel{}, user{};
            USHORT machine{}, nativeMachine{};
            bool okay = QueryFullProcessImageNameW(process, 0, image, &length) &&
                GetProcessTimes(process, &start, &end, &kernel, &user) &&
                IsWow64Process2(process, &machine, &nativeMachine) &&
                machine == IMAGE_FILE_MACHINE_UNKNOWN && nativeMachine == IMAGE_FILE_MACHINE_AMD64;
            CloseHandle(process);
            if (!okay) return false;
            constexpr wchar_t suffix[] = L"\\Tencent\\WeType\\2.1.4.6\\wetype_renderer.exe";
            constexpr size_t suffixSize = std::size(suffix) - 1;
            if (length < suffixSize || _wcsicmp(image + length - suffixSize, suffix)) return false;
            DWORD ignored{};
            auto size = GetFileVersionInfoSizeW(image, &ignored);
            if (!size || size > 65536) return false;
            std::vector<BYTE> version(size);
            if (!GetFileVersionInfoW(image, 0, size, version.data())) return false;
            VS_FIXEDFILEINFO* info{}; UINT infoSize{};
            if (!VerQueryValueW(version.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
                infoSize < sizeof(VS_FIXEDFILEINFO) || info->dwSignature != VS_FFI_SIGNATURE ||
                HIWORD(info->dwFileVersionMS) != 2 || LOWORD(info->dwFileVersionMS) != 1 ||
                HIWORD(info->dwFileVersionLS) != 4 || LOWORD(info->dwFileVersionLS) != 6 ||
                !SignedTencentRenderer(image)) return false;
            created = (uint64_t(start.dwHighDateTime) << 32) | start.dwLowDateTime;
            return true;
            }
            catch (...) { return false; }
        }

        struct CandidateList { Candidate values[2]{}; size_t count{}; bool rejected{}; };

        BOOL CALLBACK LocateCandidate(HWND window, LPARAM context)
        {
            auto* candidates = reinterpret_cast<CandidateList*>(context);
            wchar_t name[64]{};
            if (!GetClassNameW(window, name, static_cast<int>(std::size(name))) ||
                wcscmp(name, L"wetype.flutter.setting") || GetWindow(window, GW_OWNER)) return TRUE;
            constexpr auto passive = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
            if ((GetWindowLongPtrW(window, GWL_EXSTYLE) & passive) != passive) return TRUE;
            DWORD pid{};
            auto thread = GetWindowThreadProcessId(window, &pid);
            uint64_t creation{};
            if (thread && VerifyProcess(pid, creation))
                candidates->values[candidates->count++] = { window, pid, thread, creation };
            else candidates->rejected = true;
            return candidates->count < 2;
        }

        std::wstring ProbePath()
        {
            wchar_t path[32768]{};
            auto length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
            if (!length || length == std::size(path)) return {};
            std::wstring result(path, length);
            auto slash = result.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring{} :
                result.substr(0, slash + 1) + weTypeProbe::DllName;
        }

        bool SameProcessCreation(DWORD pid, uint64_t expected) noexcept
        {
            auto process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!process) return false;
            FILETIME start{}, end{}, kernel{}, user{};
            auto okay = GetProcessTimes(process, &start, &end, &kernel, &user) != FALSE;
            CloseHandle(process);
            return okay && (((uint64_t(start.dwHighDateTime) << 32) | start.dwLowDateTime) == expected);
        }
    }

    WeTypeProbeHost::~WeTypeProbeHost() { Stop(native::WeTypeProbeStopReason::Shutdown); }

    void WeTypeProbeHost::UpdateTargets(std::span<native::WeTypeProbeLens const> targets) noexcept
    {
        weTypeProbe::ProbeLens next[weTypeProbe::LensCapacity]{};
        auto count = static_cast<LONG>(std::min(targets.size(), weTypeProbe::LensCapacity));
        for (LONG index = 0; index < count; ++index)
        {
            next[index].window = reinterpret_cast<uint64_t>(targets[index].window);
            next[index].bounds = targets[index].bounds;
        }
        if (count == m_targetCount && !std::memcmp(m_targets, next, count * sizeof(next[0]))) return;
        std::memcpy(m_targets, next, count * sizeof(next[0]));
        m_targetCount = count;
        if (m_shared)
        {
            InterlockedIncrement(&m_shared->targetGeneration); // Odd: writer owns snapshot.
            m_shared->targetCount = count;
            std::memcpy(m_shared->targets, next, count * sizeof(next[0]));
            MemoryBarrier();
            InterlockedIncrement(&m_shared->targetGeneration); // Even: snapshot published.
        }
    }

    HWND WeTypeProbeHost::InterceptionTarget() const noexcept
    {
        return m_mode == native::WeTypeProbeMode::Intercept && m_hook && m_shared &&
            InterlockedCompareExchange(&m_shared->attached, 0, 0) == 1 &&
            InterlockedCompareExchange(&m_shared->active, 0, 0) == 1 &&
            InterlockedCompareExchange(&m_shared->failureCode, 0, 0) == 0 ? m_target : nullptr;
    }

    void WeTypeProbeHost::ReportUnexpectedOrder(HWND window) noexcept
    {
        if (window != InterceptionTarget()) return;
        InterlockedCompareExchange(&m_shared->failureCode, 2, 0);
        InterlockedExchange(&m_shared->active, 0);
    }

    bool WeTypeProbeHost::Start(native::WeTypeProbeMode mode) noexcept
    {
        if (Enabled()) { SetLastError(ERROR_ALREADY_EXISTS); return false; }
        if (!HasTargets()) { SetLastError(ERROR_NOT_READY); return false; }
        if (mode != native::WeTypeProbeMode::Observe && mode != native::WeTypeProbeMode::Intercept)
        { SetLastError(ERROR_INVALID_PARAMETER); return false; }
        try
        {
            CandidateList candidates;
            EnumWindows(LocateCandidate, reinterpret_cast<LPARAM>(&candidates));
            if (candidates.count != 1)
            {
                SetLastError(candidates.count > 1 ? ERROR_DUP_NAME :
                    candidates.rejected ? ERROR_NOT_SUPPORTED : ERROR_NOT_FOUND);
                return false;
            }
            auto candidate = candidates.values[0];
            auto dll = ProbePath();
            if (dll.empty()) { SetLastError(ERROR_PATH_NOT_FOUND); return false; }
            uint64_t nonce{};
            if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&nonce), sizeof(nonce),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
            { SetLastError(ERROR_GEN_FAILURE); return false; }
            auto name = std::wstring(weTypeProbe::MappingPrefix) +
                std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(candidate.pid) +
                L"." + std::to_wstring(nonce);
            auto localDll = LoadLibraryW(dll.c_str());
            if (!localDll) return false;
            auto callback = reinterpret_cast<HOOKPROC>(GetProcAddress(localDll, "WeTypeProbeHook"));
            if (!callback) { FreeLibrary(localDll); SetLastError(ERROR_PROC_NOT_FOUND); return false; }
            auto mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                sizeof(weTypeProbe::ProbeShared), name.c_str());
            auto existing = GetLastError();
            if (!mapping || existing == ERROR_ALREADY_EXISTS)
            { if (mapping) CloseHandle(mapping); FreeLibrary(localDll); SetLastError(existing); return false; }
            auto shared = static_cast<weTypeProbe::ProbeShared*>(MapViewOfFile(mapping,
                FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(weTypeProbe::ProbeShared)));
            if (!shared)
            { auto error = GetLastError(); CloseHandle(mapping); FreeLibrary(localDll); SetLastError(error); return false; }
            shared->magic = weTypeProbe::ProtocolMagic;
            shared->version = weTypeProbe::ProtocolVersion;
            shared->hostProcess = GetCurrentProcessId();
            shared->targetProcess = candidate.pid;
            shared->targetThread = candidate.thread;
            shared->mode = mode == native::WeTypeProbeMode::Intercept ?
                weTypeProbe::ProbeMode::Intercept : weTypeProbe::ProbeMode::Observe;
            shared->nonce = nonce;
            shared->window = reinterpret_cast<uint64_t>(candidate.window);
            shared->targetCreationTime = candidate.creation;
            InterlockedExchange64(&shared->hostHeartbeatTick, GetTickCount64());
            shared->targetCount = m_targetCount;
            std::memcpy(shared->targets, m_targets, m_targetCount * sizeof(m_targets[0]));
            InterlockedExchange(&shared->active, 1);
            auto hook = SetWindowsHookExW(WH_CALLWNDPROC, callback, localDll, candidate.thread);
            if (!hook)
            {
                auto error = GetLastError();
                UnmapViewOfFile(shared); CloseHandle(mapping); FreeLibrary(localDll);
                SetLastError(error); return false;
            }
            m_target = candidate.window; m_pid = candidate.pid; m_thread = candidate.thread;
            m_creationTime = candidate.creation; m_started = GetTickCount64(); m_nonce = nonce;
            m_lastSummary = m_started;
            m_lastIdentityCheck = m_started;
            m_localDll = localDll; m_hook = hook; m_mapping = mapping; m_shared = shared;
            m_readSequence = 0; m_preCount = m_postCount = m_recorded = 0;
            m_stopReason = native::WeTypeProbeStopReason::None;
            m_mode = mode;
            auto bootstrap = RegisterWindowMessageW(weTypeProbe::BootstrapMessage);
            // WH_CALLWNDPROC observes sent messages, not messages posted to the queue.
            // The candidate belongs to another thread, so SendNotifyMessageW returns
            // without waiting for its window procedure to run.
            if (!bootstrap || !SendNotifyMessageW(candidate.window, bootstrap,
                GetCurrentProcessId(), static_cast<LPARAM>(nonce)))
            {
                auto error = GetLastError(); Stop(native::WeTypeProbeStopReason::AttachFailed);
                m_stopReason = native::WeTypeProbeStopReason::None;
                SetLastError(error ? error : ERROR_GEN_FAILURE); return false;
            }
            native::Record(native::DiagnosticEvent::WindowLayer,
                { 11, 1, candidate.pid, candidate.thread,
                  int64_t(reinterpret_cast<uintptr_t>(candidate.window)),
                  int64_t(mode == native::WeTypeProbeMode::Intercept) }, true);
            return true;
        }
        catch (...) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    }

    void WeTypeProbeHost::Stop(native::WeTypeProbeStopReason reason) noexcept
    {
        if (!m_hook) return;
        if (m_shared) InterlockedExchange(&m_shared->active, 0);
        DWORD currentPid{};
        if (IsWindow(m_target) && GetWindowThreadProcessId(m_target, &currentPid) == m_thread &&
            currentPid == m_pid && SameProcessCreation(currentPid, m_creationTime))
        {
            DWORD_PTR ignored{};
            auto detach = RegisterWindowMessageW(weTypeProbe::DetachMessage);
            if (detach) SendMessageTimeoutW(m_target, detach, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK,
                100, &ignored);
        }
        if (!DrainEvents()) reason = native::WeTypeProbeStopReason::EventOverflow;
        auto detached = m_shared && InterlockedCompareExchange(&m_shared->detached, 0, 0) == 1;
        native::Record(native::DiagnosticEvent::WindowLayer,
            { 12, int64_t(reason),
              m_shared ? InterlockedCompareExchange(&m_shared->preChangeCount, 0, 0) : int64_t(m_preCount),
              m_shared ? InterlockedCompareExchange(&m_shared->postChangeCount, 0, 0) : int64_t(m_postCount),
              int64_t(detached ? 1 : 0), int64_t(m_recorded),
              m_shared ? InterlockedCompareExchange(&m_shared->interceptCount, 0, 0) : 0,
              m_shared ? InterlockedCompareExchange(&m_shared->failureCode, 0, 0) : 0 }, true);
        UnhookWindowsHookEx(m_hook); m_hook = nullptr;
        if (m_localDll) FreeLibrary(m_localDll); m_localDll = nullptr;
        if (m_shared) UnmapViewOfFile(m_shared); m_shared = nullptr;
        if (m_mapping) CloseHandle(m_mapping); m_mapping = nullptr;
        m_target = nullptr; m_pid = m_thread = 0;
        m_creationTime = m_nonce = m_started = m_lastIdentityCheck = m_lastSummary = 0;
        m_stopReason = reason;
    }

    bool WeTypeProbeHost::DrainEvents() noexcept
    {
        if (!m_shared) return true;
        static auto qpcFrequency = [] { LARGE_INTEGER value{};
            return QueryPerformanceFrequency(&value) && value.QuadPart > 0 ? value.QuadPart : int64_t(1); }();
        auto latest = InterlockedCompareExchange(&m_shared->writeSequence, 0, 0);
        if (latest - m_readSequence > weTypeProbe::EventCapacity) return false;
        while (m_readSequence < latest)
        {
            auto next = m_readSequence + 1;
            auto& event = m_shared->events[(next - 1) % weTypeProbe::EventCapacity];
            if (InterlockedCompareExchange(&event.committed, 0, 0) != next) break;
            ++m_readSequence;
            if (event.message == WM_WINDOWPOSCHANGING) ++m_preCount;
            else if (event.message == WM_WINDOWPOSCHANGED) ++m_postCount;
            if (++m_recorded <= 512)
            {
                auto critical = m_recorded <= 96 && !(event.flags & SWP_NOZORDER);
                auto durationMicros = event.durationQpc >= 0 && event.durationQpc < qpcFrequency * 60 ?
                    event.durationQpc * 1000000 / qpcFrequency : int64_t(-1);
                native::Record(native::DiagnosticEvent::WindowLayer,
                    { 11, 2, event.sequence, event.message, event.flags,
                      int64_t(event.insertAfter), event.topmost, int64_t(event.tick) },
                    critical, durationMicros, event.window);
                native::Record(native::DiagnosticEvent::WindowLayer,
                    { 11, 4, event.sequence, event.overlapLensCount, event.aboveLensCount,
                      int64_t(event.previous), event.topmost, int64_t(event.tick) },
                    critical, durationMicros, event.window);
                if (event.message == WM_WINDOWPOSCHANGING)
                {
                    native::Record(native::DiagnosticEvent::WindowLayer,
                        { 11, 3, event.sequence, event.x, event.y, event.width,
                          event.height, int64_t(event.previous) },
                        false, durationMicros, event.window);
                    if (event.rewritten)
                        native::Record(native::DiagnosticEvent::WindowLayer,
                            { 11, 5, event.sequence, event.originalFlags,
                              int64_t(event.originalInsertAfter), int64_t(event.anchor),
                              event.flags, int64_t(event.insertAfter) },
                            true, durationMicros, event.window);
                }
            }
        }
        return true;
    }

    void WeTypeProbeHost::Pump() noexcept
    {
        if (!m_hook || !m_shared) return;
        if (!HasTargets()) { Stop(native::WeTypeProbeStopReason::NoTargets); return; }
        if (InterlockedCompareExchange(&m_shared->detached, 0, 0) == 1)
        { Stop(native::WeTypeProbeStopReason::WindowChanged); return; }
        if (InterlockedCompareExchange(&m_shared->active, 0, 0) != 1)
        {
            Stop(InterlockedCompareExchange(&m_shared->failureCode, 0, 0) ?
                native::WeTypeProbeStopReason::InterceptFailed :
                native::WeTypeProbeStopReason::HeartbeatExpired);
            return;
        }
        InterlockedExchange64(&m_shared->hostHeartbeatTick, GetTickCount64());
        auto age = GetTickCount64() - m_started;
        if (weTypeProbe::ProbeTimedOut(m_shared->mode, age))
        { Stop(native::WeTypeProbeStopReason::Timeout); return; }
        if (GetTickCount64() - m_lastIdentityCheck >= 250)
        {
            m_lastIdentityCheck = GetTickCount64();
            DWORD pid{};
            wchar_t name[64]{};
            if (!IsWindow(m_target) || GetWindowThreadProcessId(m_target, &pid) != m_thread ||
                pid != m_pid || !GetClassNameW(m_target, name, 64) ||
                wcscmp(name, L"wetype.flutter.setting") || !SameProcessCreation(pid, m_creationTime))
            { Stop(native::WeTypeProbeStopReason::WindowChanged); return; }
        }
        if (age > 2000 && InterlockedCompareExchange(&m_shared->attached, 0, 0) != 1)
        { Stop(native::WeTypeProbeStopReason::AttachFailed); return; }
        if (!DrainEvents()) { Stop(native::WeTypeProbeStopReason::EventOverflow); return; }
        if (m_mode == native::WeTypeProbeMode::Intercept &&
            GetTickCount64() - m_lastSummary >= 60000)
        {
            m_lastSummary = GetTickCount64();
            native::Record(native::DiagnosticEvent::WindowLayer,
                { 11, 6,
                  InterlockedCompareExchange(&m_shared->preChangeCount, 0, 0),
                  InterlockedCompareExchange(&m_shared->postChangeCount, 0, 0),
                  InterlockedCompareExchange(&m_shared->interceptCount, 0, 0),
                  InterlockedCompareExchange(&m_shared->failureCode, 0, 0) });
        }
    }
}
