// SPDX-License-Identifier: MIT
/*
 guest threads on macOS (pthreads and the GCD workqueue)
 x86 libpthread asks the kernel for threads but the kernel cant start x86 code so FEXMacx does it
 register   remember the x86 thread_start and start_wqthread entries plus the tsd offsets
            and report kernel features so libdispatch is happy
 create     a pthread: host thread + FEX guest thread entering thread_start with the registers xnu sets
            rdi pthread rsi thread port rdx func rcx arg r8 stack r9 flags rsp stack-128
 terminate  free the guest stack, wake pthread_join, drop the port and end the host thread
 workqueue  libdispatch asks for n worker threads with workq_kernreturn(REQTHREADS). each one gets a
            [guard | stack | pthread] block like the kernel gives and enters start_wqthread
            rdi self rsi thread port rdx stack bottom rcx kevents r8 flags r9 kevent count
            when the worker is done it calls workq_kernreturn(THREAD_RETURN) and the thread goes away
 evry new thread gets PTHREAD_START_TSD_BASE_SET (or the WQ version) in its flags and the thread port in
 the MACH_THREAD_SELF tsd slot cuz the kernel does that and libpthread checks it
*/
#pragma once

#include <atomic>
#include <cstdint>

namespace FEXCore::Context {
class Context;
}

namespace FEXMacx {
class GuestThreads final {
public:
  explicit GuestThreads(FEXCore::Context::Context* CTX)
    : CTX {CTX} {}

  uint64_t RecordRegistration(uint64_t ThreadStart, uint64_t WorkqueueThreadStart, uint64_t RegistrationData, uint64_t RegistrationDataSize);

  uint64_t Create(uint64_t StartFunction, uint64_t StartArgument, uint64_t Stack, uint64_t Pthread, uint32_t Flags, int& Error);

  [[noreturn]]
  void Terminate(uint64_t StackAddress, uint64_t FreeSize, uint32_t KernelPort, uint32_t JoinSemaphore);

  int HandleWorkqueueCall(uint64_t Operation, uint64_t Item, uint64_t Argument, uint64_t Priority);

  FEXCore::Context::Context* GetContext() const {
    return CTX;
  }
  uint32_t GetTsdOffset() const {
    return TsdOffset.load();
  }
  uint32_t GetThreadSelfOffset() const {
    return ThreadSelfOffset.load();
  }

private:
  bool StartWorkqueueThread(uint64_t Priority);

  FEXCore::Context::Context* CTX;
  std::atomic<uint64_t> ThreadStartAddress {};
  std::atomic<uint64_t> WorkqueueStartAddress {};
  std::atomic<uint32_t> TsdOffset {};
  std::atomic<uint32_t> ThreadSelfOffset {};
};
} // namespace FEXMacx
