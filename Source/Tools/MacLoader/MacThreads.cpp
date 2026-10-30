// SPDX-License-Identifier: MIT
#include "MacThreads.h"
#include "MacGuestFaults.h"

#include "GuestThread.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <cerrno>
#include <csetjmp>
#include <cstring>
#include <mach/mach.h>
#include <memory>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace FEXMacx {
namespace {
using namespace FEXCore::X86State;

constexpr uint32_t PTHREAD_START_CUSTOM = 0x0100'0000;
constexpr uint32_t PTHREAD_START_TSD_BASE_SET = 0x1000'0000;

constexpr uint64_t PTHREAD_FEATURE_DISPATCHFUNC = 0x01;
constexpr uint64_t PTHREAD_FEATURE_FINEPRIO = 0x02;
constexpr uint64_t PTHREAD_FEATURE_BSDTHREADCTL = 0x04;
constexpr uint64_t PTHREAD_FEATURE_SETSELF = 0x08;
constexpr uint64_t PTHREAD_FEATURE_QOS_MAINTENANCE = 0x10;
constexpr uint64_t PTHREAD_FEATURE_KEVENT = 0x40;
constexpr uint64_t PTHREAD_FEATURE_WORKLOOP = 0x80;
constexpr uint64_t PTHREAD_FEATURE_QOS_DEFAULT = 0x4000'0000;

constexpr uint64_t WQOPS_THREAD_RETURN = 0x004;
constexpr uint64_t WQOPS_QUEUE_NEWSPISUPP = 0x010;
constexpr uint64_t WQOPS_QUEUE_REQTHREADS = 0x020;
constexpr uint64_t WQOPS_QUEUE_REQTHREADS2 = 0x030;
constexpr uint64_t WQOPS_THREAD_KEVENT_RETURN = 0x040;
constexpr uint64_t WQOPS_SET_EVENT_MANAGER_PRIORITY = 0x080;
constexpr uint64_t WQOPS_THREAD_WORKLOOP_RETURN = 0x100;
constexpr uint64_t WQOPS_SHOULD_NARROW = 0x200;
constexpr uint64_t WQOPS_SETUP_DISPATCH = 0x400;

constexpr uint32_t WQ_FLAG_THREAD_PRIO_QOS = 0x0000'4000;
constexpr uint32_t WQ_FLAG_THREAD_OVERCOMMIT = 0x0001'0000;
constexpr uint32_t WQ_FLAG_THREAD_NEWSPI = 0x0004'0000;
constexpr uint32_t WQ_FLAG_THREAD_TSD_BASE_SET = 0x0020'0000;

constexpr uint64_t PTHREAD_PRIORITY_QOS_CLASS_MASK = 0x003f'ff00;
constexpr uint64_t PTHREAD_PRIORITY_OVERCOMMIT_FLAG = 0x8000'0000;
constexpr uint32_t THREAD_QOS_DEFAULT = 4;

constexpr uint64_t X86_64_RED_ZONE_SIZE = 128;
constexpr uint64_t REGISTRATION_TSD_OFFSET_FIELD = 24;
constexpr uint64_t REGISTRATION_THREAD_SELF_OFFSET_FIELD = 32;
constexpr size_t HOST_THREAD_STACK_SIZE = 8 * 1024 * 1024;
constexpr size_t WORKQUEUE_GUEST_STACK_SIZE = 512 * 1024;
constexpr size_t GUEST_PTHREAD_STRUCT_SIZE = 0x2000;

struct GuestThreadEntry {
  GuestThreads* Owner;
  uint64_t EntryPoint;
  uint64_t Pthread;
  uint64_t ThirdArgument;
  uint64_t FourthArgument;
  uint64_t FifthArgument;
  uint64_t SixthArgument;
  uint64_t StackPointer;
  uint64_t OwnedAllocation;
  size_t OwnedAllocationSize;
};

thread_local std::jmp_buf* ThreadExitJump {};
thread_local uint64_t ThreadOwnedAllocation {};
thread_local size_t ThreadOwnedAllocationSize {};

void* RunGuestThread(void* RawEntry) {
  std::unique_ptr<GuestThreadEntry> Entry {static_cast<GuestThreadEntry*>(RawEntry)};
  auto* CTX = Entry->Owner->GetContext();

  auto* Thread = CTX->CreateThread();
  auto ThreadSetup = std::make_unique<GuestThreadSetup>(Thread);
  if (!ThreadSetup->IsReady()) {
    CTX->DestroyThread(Thread);
    return nullptr;
  }

  const mach_port_t KernelPort = mach_thread_self();
  const uint64_t TsdBase = Entry->Pthread + Entry->Owner->GetTsdOffset();
  if (const uint32_t ThreadSelfOffset = Entry->Owner->GetThreadSelfOffset()) {
    const uint64_t PortSlotValue = KernelPort;
    memcpy(reinterpret_cast<void*>(TsdBase + ThreadSelfOffset), &PortSlotValue, sizeof(PortSlotValue));
  }

  auto& State = Thread->CurrentFrame->State;
  State.rip = Entry->EntryPoint;
  State.gregs[REG_RDI] = Entry->Pthread;
  State.gregs[REG_RSI] = KernelPort;
  State.gregs[REG_RDX] = Entry->ThirdArgument;
  State.gregs[REG_RCX] = Entry->FourthArgument;
  State.gregs[REG_R8] = Entry->FifthArgument;
  State.gregs[REG_R9] = Entry->SixthArgument;
  State.gregs[REG_RSP] = Entry->StackPointer;
  State.gs_cached = TsdBase;
  ThreadOwnedAllocation = Entry->OwnedAllocation;
  ThreadOwnedAllocationSize = Entry->OwnedAllocationSize;
  Entry.reset();

  std::jmp_buf ExitJump;
  if (setjmp(ExitJump) == 0) {
    ThreadExitJump = &ExitJump;
    SetCurrentGuestThread(Thread);
    CTX->ExecuteThread(Thread);
  }

  ThreadExitJump = nullptr;
  SetCurrentGuestThread(nullptr);
  CTX->DestroyThread(Thread);
  if (ThreadOwnedAllocation) {
    munmap(reinterpret_cast<void*>(ThreadOwnedAllocation), ThreadOwnedAllocationSize);
    ThreadOwnedAllocation = 0;
  }
  return nullptr;
}

bool StartHostThread(GuestThreadEntry* Entry) {
  pthread_attr_t Attributes;
  pthread_attr_init(&Attributes);
  pthread_attr_setstacksize(&Attributes, HOST_THREAD_STACK_SIZE);
  pthread_attr_setdetachstate(&Attributes, PTHREAD_CREATE_DETACHED);
  pthread_t HostThread;
  const int CreateResult = pthread_create(&HostThread, &Attributes, RunGuestThread, Entry);
  pthread_attr_destroy(&Attributes);
  return CreateResult == 0;
}

[[noreturn]]
void LeaveGuestThread() {
  if (ThreadExitJump) {
    std::longjmp(*ThreadExitJump, 1);
  }
  pthread_exit(nullptr);
}

uint32_t WorkqueueFlagsFromPriority(uint64_t Priority) {
  const uint64_t QosBits = (Priority & PTHREAD_PRIORITY_QOS_CLASS_MASK) >> 8;
  const uint32_t ThreadQos = QosBits ? static_cast<uint32_t>(__builtin_ffsll(QosBits)) : THREAD_QOS_DEFAULT;
  uint32_t Flags = WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_TSD_BASE_SET | WQ_FLAG_THREAD_PRIO_QOS | ThreadQos;
  if (Priority & PTHREAD_PRIORITY_OVERCOMMIT_FLAG) {
    Flags |= WQ_FLAG_THREAD_OVERCOMMIT;
  }
  return Flags;
}
} // namespace

uint64_t GuestThreads::RecordRegistration(uint64_t ThreadStart, uint64_t WorkqueueThreadStart, uint64_t RegistrationData,
                                          uint64_t RegistrationDataSize) {
  ThreadStartAddress = ThreadStart;
  WorkqueueStartAddress = WorkqueueThreadStart;
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
  return PTHREAD_FEATURE_DISPATCHFUNC | PTHREAD_FEATURE_FINEPRIO | PTHREAD_FEATURE_BSDTHREADCTL | PTHREAD_FEATURE_SETSELF |
         PTHREAD_FEATURE_QOS_MAINTENANCE | PTHREAD_FEATURE_KEVENT | PTHREAD_FEATURE_WORKLOOP | PTHREAD_FEATURE_QOS_DEFAULT;
}

uint64_t GuestThreads::Create(uint64_t StartFunction, uint64_t StartArgument, uint64_t Stack, uint64_t Pthread, uint32_t Flags, int& Error) {
  if (!(Flags & PTHREAD_START_CUSTOM) || !ThreadStartAddress.load()) {
    Error = ENOTSUP;
    return 0;
  }

  auto* Entry = new GuestThreadEntry {
    .Owner = this,
    .EntryPoint = ThreadStartAddress.load(),
    .Pthread = Pthread,
    .ThirdArgument = StartFunction,
    .FourthArgument = StartArgument,
    .FifthArgument = Stack,
    .SixthArgument = Flags | PTHREAD_START_TSD_BASE_SET,
    .StackPointer = Stack - X86_64_RED_ZONE_SIZE,
    .OwnedAllocation = 0,
    .OwnedAllocationSize = 0,
  };
  if (!StartHostThread(Entry)) {
    delete Entry;
    Error = EAGAIN;
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
  LeaveGuestThread();
}

bool GuestThreads::StartWorkqueueThread(uint64_t Priority) {
  const uint64_t EntryPoint = WorkqueueStartAddress.load();
  if (!EntryPoint) {
    return false;
  }

  const size_t HostPageSize = getpagesize();
  const size_t GuardSize = HostPageSize;
  const size_t AllocationSize = GuardSize + WORKQUEUE_GUEST_STACK_SIZE + FEXCore::AlignUp(GUEST_PTHREAD_STRUCT_SIZE, HostPageSize);
  auto* Allocation = static_cast<uint8_t*>(mmap(nullptr, AllocationSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (Allocation == MAP_FAILED) {
    return false;
  }
  mprotect(Allocation, GuardSize, PROT_NONE);

  const uint64_t StackBottom = reinterpret_cast<uint64_t>(Allocation) + GuardSize;
  const uint64_t Self = StackBottom + WORKQUEUE_GUEST_STACK_SIZE;
  auto* Entry = new GuestThreadEntry {
    .Owner = this,
    .EntryPoint = EntryPoint,
    .Pthread = Self,
    .ThirdArgument = StackBottom,
    .FourthArgument = 0,
    .FifthArgument = WorkqueueFlagsFromPriority(Priority),
    .SixthArgument = 0,
    .StackPointer = FEXCore::AlignDown(Self - X86_64_RED_ZONE_SIZE, 16),
    .OwnedAllocation = reinterpret_cast<uint64_t>(Allocation),
    .OwnedAllocationSize = AllocationSize,
  };
  if (!StartHostThread(Entry)) {
    delete Entry;
    munmap(Allocation, AllocationSize);
    return false;
  }
  return true;
}

int GuestThreads::HandleWorkqueueCall(uint64_t Operation, uint64_t Item, uint64_t Argument, uint64_t Priority) {
  switch (Operation) {
  case WQOPS_QUEUE_NEWSPISUPP:
  case WQOPS_SET_EVENT_MANAGER_PRIORITY:
  case WQOPS_SHOULD_NARROW:
  case WQOPS_SETUP_DISPATCH: return 0;
  case WQOPS_QUEUE_REQTHREADS:
  case WQOPS_QUEUE_REQTHREADS2: {
    const uint64_t ThreadCount = Argument ? Argument : 1;
    for (uint64_t Index = 0; Index < ThreadCount; ++Index) {
      if (!StartWorkqueueThread(Priority)) {
        return EAGAIN;
      }
    }
    return 0;
  }
  case WQOPS_THREAD_RETURN:
  case WQOPS_THREAD_KEVENT_RETURN:
  case WQOPS_THREAD_WORKLOOP_RETURN: LeaveGuestThread();
  default: return ENOTSUP;
  }
}
} // namespace FEXMacx
