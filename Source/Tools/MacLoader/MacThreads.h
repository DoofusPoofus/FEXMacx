// SPDX-License-Identifier: MIT
/*
 guest threads on macOS (pthreads and the GCD workqueue)
 x86 libpthread asks the kernel for threads but the kernel cant start x86 code so FEXMacx does it
 register   remember the x86 thread_start and start_wqthread entries plus the tsd offsets
            and report kernel features so libdispatch is happy
 create     a pthread: host thread + FEX guest thread entering thread_start with the registers xnu sets
            rdi pthread rsi thread port rdx func rcx arg r8 stack r9 flags rsp stack-128
 terminate  free the guest stack, wake pthread_join, drop the port and end the host thread
 workqueue  FEXMacx takes the process workqueue slot with its own host callbacks so the guests requests
            kevents and workloops go straight to the real kernel. when the kernel starts a worker it lands
            in a callback which runs the guests start_wqthread on that thread with the kernel register layout
            rdi self rsi thread port rdx stack bottom rcx kevent list r8 flags r9 kevent count
            (a workloop id sits rite b4 the kevent list). when the guest hands the thread back its re-arm
            list becomes the callbacks result
 a guest thread leaves FEX by jumping to a page with one hlt byte so ExecuteThread returns normally
 evry new thread gets the TSD_BASE_SET flag and the thread port in the MACH_THREAD_SELF tsd slot cuz
 the kernel does that and libpthread checks it
*/
#pragma once

#include <atomic>
#include <cstdint>

namespace FEXCore::Context {
class Context;
}
namespace FEXCore::Core {
struct CpuStateFrame;
}

namespace FEXMacx {
class GuestThreads final {
public:
  explicit GuestThreads(FEXCore::Context::Context* CTX);

  uint64_t RecordRegistration(uint64_t ThreadStart, uint64_t WorkqueueThreadStart, uint64_t RegistrationData, uint64_t RegistrationDataSize);

  uint64_t Create(uint64_t StartFunction, uint64_t StartArgument, uint64_t Stack, uint64_t Pthread, uint32_t Flags, int& Error);

  void Terminate(FEXCore::Core::CpuStateFrame* Frame, uint64_t StackAddress, uint64_t FreeSize, uint32_t KernelPort, uint32_t JoinSemaphore);

  bool HandleWorkqueueCall(FEXCore::Core::CpuStateFrame* Frame, const uint64_t (&Args)[8], uint64_t& Result, bool& Failed);

  bool InstallHostWorkqueueBridge();

  FEXCore::Context::Context* GetContext() const {
    return CTX;
  }
  uint64_t GetWorkqueueStart() const {
    return WorkqueueStartAddress.load();
  }
  uint32_t GetTsdOffset() const {
    return TsdOffset.load();
  }
  uint32_t GetThreadSelfOffset() const {
    return ThreadSelfOffset.load();
  }
  uint64_t GetExitTrampoline() const {
    return ExitTrampoline;
  }

private:
  FEXCore::Context::Context* CTX;
  uint64_t ExitTrampoline {};
  std::atomic<uint64_t> ThreadStartAddress {};
  std::atomic<uint64_t> WorkqueueStartAddress {};
  std::atomic<uint32_t> TsdOffset {};
  std::atomic<uint32_t> ThreadSelfOffset {};
};
} // namespace FEXMacx
