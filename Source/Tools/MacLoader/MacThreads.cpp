// SPDX-License-Identifier: MIT
#include "MacThreads.h"
#include "MacGuestFaults.h"
#include "MacTime.h"

#include "GuestThread.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
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
constexpr uint64_t WQOPS_THREAD_KEVENT_RETURN = 0x040;
constexpr uint64_t WQOPS_THREAD_WORKLOOP_RETURN = 0x100;
constexpr uint64_t WQOPS_SETUP_DISPATCH = 0x400;

constexpr uint32_t WQ_FLAG_THREAD_PRIO_QOS = 0x0000'4000;
constexpr uint32_t WQ_FLAG_THREAD_OVERCOMMIT = 0x0001'0000;
constexpr uint32_t WQ_FLAG_THREAD_REUSE = 0x0002'0000;
constexpr uint32_t WQ_FLAG_THREAD_NEWSPI = 0x0004'0000;
constexpr uint32_t WQ_FLAG_THREAD_KEVENT = 0x0008'0000;
constexpr uint32_t WQ_FLAG_THREAD_EVENT_MANAGER = 0x0010'0000;
constexpr uint32_t WQ_FLAG_THREAD_TSD_BASE_SET = 0x0020'0000;
constexpr uint32_t WQ_FLAG_THREAD_WORKLOOP = 0x0040'0000;

constexpr uint64_t PTHREAD_PRIORITY_QOS_CLASS_MASK = 0x003f'ff00;
constexpr uint64_t PTHREAD_PRIORITY_EVENT_MANAGER_FLAG = 0x0200'0000;
constexpr uint64_t PTHREAD_PRIORITY_OVERCOMMIT_FLAG = 0x8000'0000;
constexpr uint32_t THREAD_QOS_DEFAULT = 4;
constexpr pthread_key_t PTHREAD_TSD_SLOT_QOS_CLASS = 4;

constexpr uint64_t X86_64_RED_ZONE_SIZE = 128;
constexpr uint64_t REGISTRATION_TSD_OFFSET_FIELD = 24;
constexpr uint64_t REGISTRATION_THREAD_SELF_OFFSET_FIELD = 32;
constexpr size_t HOST_THREAD_STACK_SIZE = 8 * 1024 * 1024;
constexpr size_t WORKQUEUE_GUEST_STACK_SIZE = 512 * 1024;
constexpr size_t GUEST_PTHREAD_STRUCT_SIZE = 0x2000;
constexpr size_t KEVENT_QOS_SIZE = 72;
constexpr size_t WORKQUEUE_KEVENT_LIST_LENGTH = 16;
constexpr size_t WORKQUEUE_KEVENT_DATA_SIZE = 32 * 1024;
constexpr size_t WORKQUEUE_EVENT_AREA_SIZE = 48 * 1024;

GuestThreads* BridgeOwner {};

uint32_t WorkqueueFlagsFromPriority(uint64_t Priority) {
  const uint64_t QosBits = (Priority & PTHREAD_PRIORITY_QOS_CLASS_MASK) >> 8;
  const uint32_t ThreadQos = QosBits ? static_cast<uint32_t>(__builtin_ffsll(QosBits)) : THREAD_QOS_DEFAULT;
  uint32_t Flags = WQ_FLAG_THREAD_NEWSPI | WQ_FLAG_THREAD_TSD_BASE_SET | WQ_FLAG_THREAD_PRIO_QOS | ThreadQos;
  if (Priority & PTHREAD_PRIORITY_OVERCOMMIT_FLAG) {
    Flags |= WQ_FLAG_THREAD_OVERCOMMIT;
  }
  if (Priority & PTHREAD_PRIORITY_EVENT_MANAGER_FLAG) {
    Flags |= WQ_FLAG_THREAD_EVENT_MANAGER;
  }
  return Flags;
}

void WriteThreadPortSlot(GuestThreads* Owner, uint64_t Pthread, mach_port_t KernelPort) {
  if (const uint32_t ThreadSelfOffset = Owner->GetThreadSelfOffset()) {
    const uint64_t PortSlotValue = KernelPort;
    memcpy(reinterpret_cast<void*>(Pthread + Owner->GetTsdOffset() + ThreadSelfOffset), &PortSlotValue, sizeof(PortSlotValue));
  }
}

struct PthreadEntry {
  GuestThreads* Owner;
  uint64_t EntryPoint;
  uint64_t Pthread;
  uint64_t StartFunction;
  uint64_t StartArgument;
  uint64_t Stack;
  uint32_t Flags;
};

void* RunGuestPthread(void* RawEntry) {
  std::unique_ptr<PthreadEntry> Entry {static_cast<PthreadEntry*>(RawEntry)};
  auto* CTX = Entry->Owner->GetContext();

  auto* Thread = CTX->CreateThread();
  auto ThreadSetup = std::make_unique<GuestThreadSetup>(Thread);
  if (!ThreadSetup->IsReady()) {
    CTX->DestroyThread(Thread);
    return nullptr;
  }

  const mach_port_t KernelPort = mach_thread_self();
  WriteThreadPortSlot(Entry->Owner, Entry->Pthread, KernelPort);

  auto& State = Thread->CurrentFrame->State;
  State.rip = Entry->EntryPoint;
  State.gregs[REG_RDI] = Entry->Pthread;
  State.gregs[REG_RSI] = KernelPort;
  State.gregs[REG_RDX] = Entry->StartFunction;
  State.gregs[REG_RCX] = Entry->StartArgument;
  State.gregs[REG_R8] = Entry->Stack;
  State.gregs[REG_R9] = Entry->Flags | PTHREAD_START_TSD_BASE_SET;
  State.gregs[REG_RSP] = Entry->Stack - X86_64_RED_ZONE_SIZE;
  State.gs_cached = Entry->Pthread + Entry->Owner->GetTsdOffset();
  Entry.reset();

  SetCurrentGuestThread(Thread);
  CTX->ExecuteThread(Thread);
  SetCurrentGuestThread(nullptr);
  CTX->DestroyThread(Thread);
  return nullptr;
}

struct GuestWorker {
  FEXCore::Core::InternalThreadState* Thread {};
  std::unique_ptr<GuestThreadSetup> Setup;
  uint8_t* Allocation {};
  size_t AllocationSize {};
  uint64_t Self {};
  uint64_t StackBottom {};
  uint64_t StackTop {};
  uint64_t KeventList {};
  bool GuestPthreadReady {};
  bool InsideGuest {};
  uint64_t ReturnedEvents {};
  int ReturnedEventCount {};

  bool Prepare() {
    if (Thread) {
      return true;
    }
    const size_t HostPageSize = getpagesize();
    const size_t GuardSize = HostPageSize;
    AllocationSize = GuardSize + WORKQUEUE_GUEST_STACK_SIZE + WORKQUEUE_EVENT_AREA_SIZE + FEXCore::AlignUp(GUEST_PTHREAD_STRUCT_SIZE, HostPageSize);
    Allocation = static_cast<uint8_t*>(mmap(nullptr, AllocationSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
    if (Allocation == MAP_FAILED) {
      Allocation = nullptr;
      return false;
    }
    mprotect(Allocation, GuardSize, PROT_NONE);

    StackBottom = reinterpret_cast<uint64_t>(Allocation) + GuardSize;
    Self = StackBottom + WORKQUEUE_GUEST_STACK_SIZE + WORKQUEUE_EVENT_AREA_SIZE;
    KeventList = Self - WORKQUEUE_KEVENT_LIST_LENGTH * KEVENT_QOS_SIZE;
    StackTop = FEXCore::AlignDown(KeventList - sizeof(uint64_t) - WORKQUEUE_KEVENT_DATA_SIZE - X86_64_RED_ZONE_SIZE, 16);

    auto* CTX = BridgeOwner->GetContext();
    Thread = CTX->CreateThread();
    Setup = std::make_unique<GuestThreadSetup>(Thread);
    return Setup->IsReady();
  }

  ~GuestWorker() {
    if (Thread) {
      Setup.reset();
      BridgeOwner->GetContext()->DestroyThread(Thread);
    }
    if (Allocation) {
      munmap(Allocation, AllocationSize);
    }
  }
};

thread_local GuestWorker Worker;

void TraceWorkerEntry(const char* Kind, uint32_t Flags, int EventCount) {
  static const bool Tracing = getenv("FEXMACX_TRACE") != nullptr;
  if (Tracing) {
    char Line[96];
    const int Length = snprintf(Line, sizeof(Line), "[trace] workqueue %s worker flags 0x%x events %d\n", Kind, Flags, EventCount);
    write(STDERR_FILENO, Line, Length);
  }
}

void RunGuestWorker(uint32_t Flags, void* Events, int EventCount, const uint64_t* WorkloopId) {
  TraceWorkerEntry(WorkloopId ? "workloop" : ((Flags & WQ_FLAG_THREAD_KEVENT) ? "kevent" : "plain"), Flags, EventCount);
  if (!BridgeOwner || !BridgeOwner->GetWorkqueueStart() || !Worker.Prepare()) {
    Worker.ReturnedEvents = 0;
    Worker.ReturnedEventCount = 0;
    return;
  }

  const int CopiedEvents = std::clamp(EventCount, 0, static_cast<int>(WORKQUEUE_KEVENT_LIST_LENGTH));
  if (Events && CopiedEvents) {
    memcpy(reinterpret_cast<void*>(Worker.KeventList), Events, CopiedEvents * KEVENT_QOS_SIZE);
  }
  if (WorkloopId) {
    memcpy(reinterpret_cast<void*>(Worker.KeventList - sizeof(uint64_t)), WorkloopId, sizeof(uint64_t));
  }
  if (Worker.GuestPthreadReady) {
    Flags |= WQ_FLAG_THREAD_REUSE;
  }

  const mach_port_t KernelPort = mach_thread_self();
  WriteThreadPortSlot(BridgeOwner, Worker.Self, KernelPort);

  auto& State = Worker.Thread->CurrentFrame->State;
  State.rip = BridgeOwner->GetWorkqueueStart();
  State.gregs[REG_RDI] = Worker.Self;
  State.gregs[REG_RSI] = KernelPort;
  State.gregs[REG_RDX] = Worker.StackBottom;
  State.gregs[REG_RCX] = (Flags & WQ_FLAG_THREAD_KEVENT) ? Worker.KeventList : 0;
  State.gregs[REG_R8] = Flags;
  State.gregs[REG_R9] = CopiedEvents;
  State.gregs[REG_RSP] = Worker.StackTop;
  State.gs_cached = Worker.Self + BridgeOwner->GetTsdOffset();
  State.callret_sp = GetCallRetStackRange(Worker.Thread).DefaultLocation;

  Worker.ReturnedEvents = 0;
  Worker.ReturnedEventCount = 0;
  Worker.InsideGuest = true;
  SetCurrentGuestThread(Worker.Thread);
  BridgeOwner->GetContext()->ExecuteThread(Worker.Thread);
  SetCurrentGuestThread(nullptr);
  Worker.InsideGuest = false;
  Worker.GuestPthreadReady = true;
  mach_port_deallocate(mach_task_self(), KernelPort);
}

void HostWorkerCallback(uint64_t Priority) {
  RunGuestWorker(WorkqueueFlagsFromPriority(Priority), nullptr, 0, nullptr);
}

void HostKeventCallback(void** Events, int* EventCount) {
  const uint64_t Priority = reinterpret_cast<uint64_t>(pthread_getspecific(PTHREAD_TSD_SLOT_QOS_CLASS));
  RunGuestWorker(WorkqueueFlagsFromPriority(Priority) | WQ_FLAG_THREAD_KEVENT, *Events, *EventCount, nullptr);
  *Events = reinterpret_cast<void*>(Worker.ReturnedEvents);
  *EventCount = Worker.ReturnedEventCount;
}

void HostWorkloopCallback(uint64_t* WorkloopId, void** Events, int* EventCount) {
  const uint64_t Priority = reinterpret_cast<uint64_t>(pthread_getspecific(PTHREAD_TSD_SLOT_QOS_CLASS));
  RunGuestWorker(WorkqueueFlagsFromPriority(Priority) | WQ_FLAG_THREAD_KEVENT | WQ_FLAG_THREAD_WORKLOOP, *Events, *EventCount, WorkloopId);
  *Events = reinterpret_cast<void*>(Worker.ReturnedEvents);
  *EventCount = Worker.ReturnedEventCount;
}

using WorkqueueInitFunction = int (*)(void (*)(uint64_t), void (*)(void**, int*), void (*)(uint64_t*, void**, int*), int, int);
} // namespace

GuestThreads::GuestThreads(FEXCore::Context::Context* CTX)
  : CTX {CTX} {
  const size_t HostPageSize = getpagesize();
  auto* TrampolinePage = static_cast<uint8_t*>(mmap(nullptr, HostPageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (TrampolinePage != MAP_FAILED) {
    constexpr uint8_t X86_HLT = 0xF4;
    TrampolinePage[0] = X86_HLT;
    mprotect(TrampolinePage, HostPageSize, PROT_READ);
    ExitTrampoline = reinterpret_cast<uint64_t>(TrampolinePage);
  }
}

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

  auto* Entry = new PthreadEntry {this, ThreadStartAddress.load(), Pthread, StartFunction, StartArgument, Stack, Flags};
  pthread_attr_t Attributes;
  pthread_attr_init(&Attributes);
  pthread_attr_setstacksize(&Attributes, HOST_THREAD_STACK_SIZE);
  pthread_attr_setdetachstate(&Attributes, PTHREAD_CREATE_DETACHED);
  pthread_t HostThread;
  const int CreateResult = pthread_create(&HostThread, &Attributes, RunGuestPthread, Entry);
  pthread_attr_destroy(&Attributes);
  if (CreateResult != 0) {
    delete Entry;
    Error = CreateResult;
    return 0;
  }

  Error = 0;
  return Pthread;
}

void GuestThreads::Terminate(FEXCore::Core::CpuStateFrame* Frame, uint64_t StackAddress, uint64_t FreeSize, uint32_t KernelPort,
                             uint32_t JoinSemaphore) {
  if (FreeSize) {
    ReleaseGuestRange(StackAddress, FreeSize);
  }
  if (JoinSemaphore) {
    semaphore_signal(JoinSemaphore);
  }
  if (KernelPort) {
    mach_port_deallocate(mach_task_self(), KernelPort);
  }
  Frame->State.rip = ExitTrampoline;
}

bool GuestThreads::HandleWorkqueueCall(FEXCore::Core::CpuStateFrame* Frame, const uint64_t (&Args)[8], uint64_t& Result, bool& Failed) {
  const uint64_t Operation = Args[0];
  Result = 0;
  Failed = false;
  switch (Operation) {
  case WQOPS_QUEUE_NEWSPISUPP:
  case WQOPS_SETUP_DISPATCH: return true;
  case WQOPS_THREAD_RETURN:
  case WQOPS_THREAD_KEVENT_RETURN:
  case WQOPS_THREAD_WORKLOOP_RETURN:
    if (!Worker.InsideGuest) {
      return false;
    }
    Worker.ReturnedEvents = Operation == WQOPS_THREAD_RETURN ? 0 : Args[1];
    Worker.ReturnedEventCount = Operation == WQOPS_THREAD_RETURN ? 0 : static_cast<int>(Args[2]);
    ConvertGuestTimerKevents(Worker.ReturnedEvents, Worker.ReturnedEventCount);
    Frame->State.rip = ExitTrampoline;
    return true;
  default: return false;
  }
}

bool GuestThreads::InstallHostWorkqueueBridge() {
  BridgeOwner = this;
  auto Initialize = reinterpret_cast<WorkqueueInitFunction>(dlsym(RTLD_DEFAULT, "_pthread_workqueue_init_with_workloop"));
  return Initialize && Initialize(HostWorkerCallback, HostKeventCallback, HostWorkloopCallback, 0, 0) == 0;
}
} // namespace FEXMacx
