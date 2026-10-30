// SPDX-License-Identifier: MIT
/*
 guest signals
 x86 libsystem hands the kernel its own x86 _sigtramp in __sigaction so none of that can reach the
 host kernel. the guest table lives here and the host gets our handler (or SIG_DFL / SIG_IGN)
 sigsegv sigbus sigill and sigtrap always stay ours cuz FEXMacx needs them for itself
 delivery builds the same frame xnu builds for x86_64: mcontext siginfo and ucontext on the guest
 stack (or the guest altstack) then rip = _sigtramp with rdi handler rsi style rdx sig rcx info r8 uctx
 two ways a signal gets in
 1 at syscall exit: the host handler only marks it pending (host syscalls come back with EINTR) and
   the guest frame is built right on top of the synced guest state. sigreturn just loads it back
 2 inside jit code (async signals and guest faults): same trick linux FEX uses. the host context gets
   backed up on the host stack the static regs get spilled and the host jumps to the dispatcher.
   sigreturn then hits the dispatcher hlt and the SIGILL puts the host context back where it was
 guest masks altstacks and pending bits are per host thread. host masks mirror the guest ones
*/
#pragma once

#include <cstdint>
#include <signal.h>
#include <sys/ucontext.h>

namespace FEXCore {
class SignalDelegator;
}
namespace FEXCore::Context {
class Context;
}
namespace FEXCore::Core {
struct CpuStateFrame;
}

namespace FEXMacx {
class GuestSignals {
public:
  explicit GuestSignals(FEXCore::Context::Context* CTX)
    : CTX {CTX} {}

  bool Install(const FEXCore::SignalDelegator* Delegator);

  bool HandleSyscall(FEXCore::Core::CpuStateFrame* Frame, uint32_t Number, const uint64_t* Args, uint64_t& Result, bool& Failed,
                     bool& StateAlreadySet);

  void DeliverPendingSignals(FEXCore::Core::CpuStateFrame* Frame, uint32_t Number, uint64_t RawNumber, bool Interrupted);

private:
  FEXCore::Context::Context* CTX;
};

void PrepareGuestSignalThread();
bool HandleGuestFaultSignal(int Signal, siginfo_t* Info, ucontext_t* Context);
} // namespace FEXMacx
