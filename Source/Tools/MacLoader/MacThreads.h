// SPDX-License-Identifier: MIT
/*
 guest pthreads on macOS
 x86 libpthread asks the kernel for threads with bsdthread_register / bsdthread_create / bsdthread_terminate
 the kernel cant start x86 code so FEXMacx does those 3 itself
 register   remember the x86 thread_start entry and the tsd offset (the guest still gets EINVAL back like b4
            cuz our own arm64 libpthread alredy registered and x86 libpthread is fine with that)
 create     spin up a host thread with a FEX guest thread and start it in thread_start with the registers
            the xnu kernel would set: rdi pthread rsi thread port rdx func rcx arg r8 stack r9 flags rsp stack-128
 the kernel also sets PTHREAD_START_TSD_BASE_SET in the flags and puts the thread port in the
 MACH_THREAD_SELF tsd slot (pthread + tsd offset + slot offset) so that happens here too
 terminate  free the guest stack, wake pthread_join, drop the port and end the host thread
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

  void RecordRegistration(uint64_t ThreadStart, uint64_t RegistrationData, uint64_t RegistrationDataSize);

  // returns the pthread pointer for the guest or an errno in Error
  uint64_t Create(uint64_t StartFunction, uint64_t StartArgument, uint64_t Stack, uint64_t Pthread, uint32_t Flags, int& Error);

  [[noreturn]]
  void Terminate(uint64_t StackAddress, uint64_t FreeSize, uint32_t KernelPort, uint32_t JoinSemaphore);

  FEXCore::Context::Context* GetContext() const {
    return CTX;
  }
  uint64_t GetThreadStart() const {
    return ThreadStartAddress.load();
  }
  uint32_t GetTsdOffset() const {
    return TsdOffset.load();
  }
  uint32_t GetThreadSelfOffset() const {
    return ThreadSelfOffset.load();
  }

private:
  FEXCore::Context::Context* CTX;
  std::atomic<uint64_t> ThreadStartAddress {};
  std::atomic<uint32_t> TsdOffset {};
  std::atomic<uint32_t> ThreadSelfOffset {};
};
} // namespace FEXMacx
