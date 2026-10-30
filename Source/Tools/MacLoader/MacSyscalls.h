// SPDX-License-Identifier: MIT
/*
 macOS syscall bridge
 guest and host are both XNU so most syscalls can go straight to the real kernel
 x86_64 puts the class in the top byte of rax (1 = mach trap 2 = unix 3 = machdep)
 args come in rdi rsi rdx r10 r8 r9 and unix syscalls report errors with the carry flag
 on arm64 the same call is svc #0x80 with the number in x16 (mach traps are negative)
 args 7 and 8 come from the guest stack above the return address of the libsystem_kernel stub
 some calls get answered here instead of by the kernel
 - mprotect munmap mach_vm_protect mach_vm_deallocate: x86 thinks in 4K pages but host pages are 16K
   so only whole host pages change and partly covered ones only ever gain permissions
 - shared_region_check_np says theres no cache so x86_64 dyld maps the x86_64 one itself
 - task_restartable_ranges_register gets a local ok cuz the kernel cant restart x86 code
 - bsdthread and workq calls go to MacThreads
 - mach time (timer kevents mach_wait_until gettimeofday mach_timebase_info) goes thru MacTime
 - sigaction sigprocmask sigaltstack sigsuspend sigreturn go to MacSignals and pending guest
   signals get delivered on the way out of every syscall
*/
#pragma once

#include "MacSignals.h"
#include "MacThreads.h"

#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>

namespace FEXCore::Context {
class Context;
}

namespace FEXMacx {
class MacSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  explicit MacSyscallHandler(FEXCore::Context::Context* CTX)
    : CTX {CTX}
    , Threads {CTX}
    , Signals {CTX} {}

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }

  bool InstallHostWorkqueueBridge() {
    return Threads.InstallHostWorkqueueBridge();
  }

  bool InstallGuestSignals(const FEXCore::SignalDelegator* Delegator) {
    return Signals.Install(Delegator);
  }

private:
  FEXCore::Context::Context* CTX;
  GuestThreads Threads;
  GuestSignals Signals;
};
} // namespace FEXMacx
