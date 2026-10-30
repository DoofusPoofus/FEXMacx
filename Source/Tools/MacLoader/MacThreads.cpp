// SPDX-License-Identifier: MIT
#include "MacThreads.h"
#include "MacGuestFaults.h"

#include "GuestThread.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>

#include <cerrno>
#include <csetjmp>
#include <cstring>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <memory>
#include <pthread.h>

namespace FEXMacx {
namespace {
using namespace FEXCore::X86State;

constexpr uint32_t PTHREAD_START_CUSTOM = 0x0100'0000;
constexpr uint32_t PTHREAD_START_TSD_BASE_SET = 0x1000'0000;
constexpr uint64_t X86_64_RED_ZONE_SIZE = 128;
constexpr uint64_t REGISTRATION_TSD_OFFSET_FIELD = 24;
constexpr uint64_t REGISTRATION_THREAD_SELF_OFFSET_FIELD = 32;
constexpr size_t HOST_THREAD_STACK_SIZE = 8 * 1024 * 1024;

struct PendingGuestThread {
  GuestThreads* Owner;
  uint64_t StartFunction;
  uint64_t StartArgument;
  uint64_t Stack;
  uint64_t Pthread;
  uint32_t Flags;
};

thread_local std::jmp_buf* ThreadExitJump {};

void* RunGuestThread(void* RawPending) {
  std::unique_ptr<PendingGuestThread> Pending {static_cast<PendingGuestThread*>(RawPending)};
  auto* CTX = Pending->Owner->GetContext();

  auto* Thread = CTX->CreateThread();
  auto ThreadSetup = std::make_unique<GuestThreadSetup>(Thread);
  if (!ThreadSetup->IsReady()) {
    CTX->DestroyThread(Thread);
    return nullptr;
  }

  const mach_port_t KernelPort = mach_thread_self();
  if (const uint32_t ThreadSelfOffset = Pending->Owner->GetThreadSelfOffset()) {
    const uint64_t PortSlotValue = KernelPort;
    const uint64_t PortSlot = Pending->Pthread + Pending->Owner->GetTsdOffset() + ThreadSelfOffset;
    memcpy(reinterpret_cast<void*>(PortSlot), &PortSlotValue, sizeof(PortSlotValue));
  }

  auto& State = Thread->CurrentFrame->State;
  State.rip = Pending->Owner->GetThreadStart();
  State.gregs[REG_RDI] = Pending->Pthread;
  State.gregs[REG_RSI] = KernelPort;
  State.gregs[REG_RDX] = Pending->StartFunction;
  State.gregs[REG_RCX] = Pending->StartArgument;
  State.gregs[REG_R8] = Pending->Stack;
  State.gregs[REG_R9] = Pending->Flags | PTHREAD_START_TSD_BASE_SET;
  State.gregs[REG_RSP] = Pending->Stack - X86_64_RED_ZONE_SIZE;
  State.gs_cached = Pending->Pthread + Pending->Owner->GetTsdOffset();
  Pending.reset();

  std::jmp_buf ExitJump;
  if (setjmp(ExitJump) == 0) {
    ThreadExitJump = &ExitJump;
    SetCurrentGuestThread(Thread);
    CTX->ExecuteThread(Thread);
  }

  ThreadExitJump = nullptr;
  SetCurrentGuestThread(nullptr);
  CTX->DestroyThread(Thread);
  return nullptr;
}
} // namespace

void GuestThreads::RecordRegistration(uint64_t ThreadStart, uint64_t RegistrationData, uint64_t RegistrationDataSize) {
  ThreadStartAddress = ThreadStart;
  if (RegistrationData && RegistrationDataSize >= REGISTRATION_TSD_OFFSET_FIELD + sizeof(uint32_t)) {
    uint32_t Offset {};
    memcpy(&Offset, reinterpret_cast<const void*>(RegistrationData + REGISTRATION_TSD_OFFSET_FIELD), sizeof(Offset));
    TsdOffset = Offset;
  }
  if (RegistrationData && RegistrationDataSize >= REGISTRATION_THREAD_SELF_OFFSET_FIELD + sizeof(uint32_t)) {
    uint32_t Offset {};
    memcpy(&Offset, reinterpret_cast<const void*>(RegistrationData + REGISTRATION_THREAD_SELF_OFFSET_FIELD), sizeof(Offset));
    ThreadSelfOffset = Offset;
  }
}

uint64_t GuestThreads::Create(uint64_t StartFunction, uint64_t StartArgument, uint64_t Stack, uint64_t Pthread, uint32_t Flags, int& Error) {
  if (!(Flags & PTHREAD_START_CUSTOM) || !ThreadStartAddress.load()) {
    Error = ENOTSUP;
    return 0;
  }

  auto* Pending = new PendingGuestThread {this, StartFunction, StartArgument, Stack, Pthread, Flags};

  pthread_attr_t Attributes;
  pthread_attr_init(&Attributes);
  pthread_attr_setstacksize(&Attributes, HOST_THREAD_STACK_SIZE);
  pthread_attr_setdetachstate(&Attributes, PTHREAD_CREATE_DETACHED);
  pthread_t HostThread;
  const int CreateResult = pthread_create(&HostThread, &Attributes, RunGuestThread, Pending);
  pthread_attr_destroy(&Attributes);

  if (CreateResult != 0) {
    delete Pending;
    Error = CreateResult;
    return 0;
  }

  Error = 0;
  return Pthread;
}

void GuestThreads::Terminate(uint64_t StackAddress, uint64_t FreeSize, uint32_t KernelPort, uint32_t JoinSemaphore) {
  if (FreeSize) {
    ReleaseGuestRange(StackAddress, FreeSize);
  }
  if (JoinSemaphore) {
    semaphore_signal(JoinSemaphore);
  }
  if (KernelPort) {
    mach_port_deallocate(mach_task_self(), KernelPort);
  }

  if (ThreadExitJump) {
    std::longjmp(*ThreadExitJump, 1);
  }
  pthread_exit(nullptr);
}
} // namespace FEXMacx
