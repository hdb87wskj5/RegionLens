#include "CursorRecoveryPolicy.h"
#include "ProxyWatchdog.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using namespace RegionLens::native;
namespace
{
    int failures{};
    void Check(bool condition, char const* name)
    {
        if (!condition) { ++failures; std::cerr << "FAILED cursor recovery: " << name << '\n'; }
    }
    CursorObservation Visible() { return { true, CURSOR_SHOWING, 0, 123, {} }; }

}

int RunCursorRecoveryTests()
{
    failures = 0;
    {
        CursorRecoveryPolicy policy;
        Check(!policy.BeginRestore(0) && !policy.Due(1000), "never-hidden cursor remains untouched");
        policy.BeginHide(); auto hiddenEpoch = policy.Epoch();
        Check(policy.Hidden() && !policy.Due(10000), "active mapping cannot run a visibility retry");
        Check(policy.BeginRestore(1000) && policy.Epoch() > hiddenEpoch, "restore starts a new cursor epoch");
        policy.ShowResult(policy.Epoch(), true);
        Check(!policy.Due(1031), "no busy polling before first delayed check");
        for (unsigned i = 0; i < CursorRecoveryPolicy::CheckTimes.size(); ++i)
        {
            auto check = policy.Next(1000 + CursorRecoveryPolicy::CheckTimes[i], Visible());
            Check(policy.Current(check) && check.step == i + 1, "visible cursor checks run in order");
            Check(check.show == (i == 0), "one delayed reassertion; visible readback avoids extra API calls");
            if (check.show) policy.ShowResult(check.epoch, true);
        }
        Check(!policy.Checking() && !policy.Due(99999) && policy.ShowSucceeded(), "verification ends after half a second");
        policy.BeginRestore(1000); policy.ShowResult(policy.Epoch(), true);
        auto late = policy.Next(9000, Visible());
        Check(late.step == 4 && late.show && !policy.Checking(), "late first check still reasserts once, without replaying expired timers");
    }
    {
        CursorRecoveryPolicy policy; policy.BeginHide(); policy.BeginRestore(0); policy.ShowResult(policy.Epoch(), true);
        CursorObservation hidden{ true, 0, 0, 123, {} };
        for (auto tick : CursorRecoveryPolicy::CheckTimes)
        {
            auto check = policy.Next(tick, hidden);
            Check(check.show, "API success but hidden readback requests a bounded reassertion");
            policy.ShowResult(check.epoch, true);
        }
        Check(!policy.Checking() && !policy.Due(10000), "intentionally hidden app cursors cannot trigger endless forced shows");
        for (auto observation : { CursorObservation{}, CursorObservation{ true, CURSOR_SHOWING, 0, 0, {} },
            CursorObservation{ true, CURSOR_SUPPRESSED, 0, 123, {} } })
        {
            Check(!observation.ReportedVisible(), "failed query, null shape and pen suppression are distinct from visible");
            policy.BeginRestore(0); policy.ShowResult(policy.Epoch(), true);
            Check(policy.Next(512, observation).show && !policy.Checking(), "delayed checks skip expired steps without a burst");
        }
    }
    {
        CursorRecoveryPolicy policy; policy.BeginHide(); policy.BeginRestore(0);
        auto oldCheck = policy.Next(32, Visible()); auto oldLease = CursorLease(policy.Epoch());
        policy.BeginHide(); volatile LONG64 lease = CursorLease(policy.Epoch());
        policy.ShowResult(oldCheck.epoch, true);
        Check(!policy.Current(oldCheck) && policy.Hidden() && !policy.ShowSucceeded(), "reentry invalidates a delayed old-session show/result");
        Check(!CompleteCursorLease(&lease, oldLease) && CursorLeasePending(SharedRead64(&lease)),
            "old guardian/readback cannot acknowledge a new hide lease");
        policy.BeginRestore(100); auto current = CursorLease(policy.Epoch()); InterlockedExchange64(&lease, current);
        Check(CompleteCursorLease(&lease, current) && !CursorLeasePending(SharedRead64(&lease)) && SharedRead64(&lease) != 0,
            "completing cursor work preserves history for a later failure");
        policy.ShowResult(policy.Epoch(), false);
        Check(policy.Next(132, Visible()).show, "failed native show is retried even if readback looks visible");
    }
    {
        LONG64 done = CursorLease(9) & ~LONG64(1);
        Check(GuardNeedsCursorRecovery(done, done, true, false, true), "hotkey after Armed/Off still requests guardian cursor recovery");
        Check(GuardNeedsCursorRecovery(done, done, true, true, true), "repeat hotkey restarts verification after first cancellation");
        CursorRecoveryPolicy guard; guard.AdoptRestore(0); guard.ShowResult(guard.Epoch(), false);
        guard.Next(512, Visible());
        Check(GuardNeedsCursorRecovery(done, done, false, guard.Checking(), guard.ShowSucceeded()) &&
            !GuardRecoveryComplete(false, done, guard.Unfinished()),
            "failed forced show after an already-acknowledged lease retains local guardian duty");
        Check(!GuardNeedsCursorRecovery(0, 0, true, false, false), "unused guardian never changes a cursor");
        Check(!GuardRecoveryComplete(false, CursorLease(9), false) && !GuardRecoveryComplete(false, done, true) &&
            GuardRecoveryComplete(false, done, false), "helper shutdown waits for cursor independently of buttons");
        Check(!GuardRecoveryComplete(true, done, false), "showing cursor cannot discharge pending mouse buttons");
        Check(ProxyWatchdogCancelReason(true, false, CursorLeasePending(CursorLease(9)), 2001, 1000) ==
            ProxyCancelReason::HeartbeatTimeout, "cursor-only pending work is protected by heartbeat");
        Check(!GuardMayReleaseResources(true, WAIT_TIMEOUT) && !GuardMayReleaseResources(true, WAIT_FAILED) &&
            GuardMayReleaseResources(true, WAIT_OBJECT_0) && GuardMayReleaseResources(false, WAIT_TIMEOUT),
            "new mapping cannot outlive an unjoined old recovery helper");
    }
    if (!failures) std::cout << "Cursor recovery tests passed (fake cursor/input; bounded log I/O only).\n";
    return failures;
}
