// SPDX-License-Identifier: MIT
#include "MacSyscalls.h"

#include "GuestThread.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/LogManager.h>

#include <FEXCore/Utils/MathUtils.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mach/mach.h>
#include <mach/ndr.h>
#include <sys/mman.h>
#include <unistd.h>

namespace FEXMacx {
namespace {
using namespace FEXCore::X86State;

enum SyscallClass : uint32_t {
  SYSCALL_CLASS_MACH = 1,
  SYSCALL_CLASS_UNIX = 2,
  SYSCALL_CLASS_MDEP = 3,
};

constexpr uint64_t SYSCALL_INSTRUCTION_SIZE = 2;
constexpr uint32_t UNIX_SYSCALL_EXIT = 1;
constexpr uint32_t UNIX_SYSCALL_MUNMAP = 73;
constexpr uint32_t UNIX_SYSCALL_MPROTECT = 74;
constexpr uint32_t UNIX_SYSCALL_SHARED_REGION_CHECK_NP = 294;
constexpr uint32_t UNIX_SYSCALL_BSDTHREAD_CREATE = 360;
constexpr uint32_t UNIX_SYSCALL_BSDTHREAD_TERMINATE = 361;
constexpr uint32_t UNIX_SYSCALL_BSDTHREAD_REGISTER = 366;
constexpr uint32_t MACH_TRAP_VM_DEALLOCATE = 12;
constexpr uint32_t MACH_TRAP_VM_PROTECT = 14;
constexpr uint32_t MACH_TRAP_MSG2 = 47;
constexpr uint32_t MIG_TASK_RESTARTABLE_RANGES_REGISTER = 8000;
constexpr uint32_t MIG_TASK_RESTARTABLE_RANGES_SYNCHRONIZE = 8001;
constexpr uint32_t MIG_REPLY_ID_OFFSET = 100;
constexpr uint64_t MACH64_SEND_MSG = 0x1;
constexpr uint64_t MACH64_RCV_MSG = 0x2;
constexpr uint32_t MDEP_THREAD_FAST_SET_CTHREAD_SELF = 3;
constexpr uint32_t X86_EFLAGS_CARRY = 1U << 0;

struct HostSyscallResult {
  uint64_t Value;
  uint64_t SecondValue;
  bool Failed;
};

bool TracingEnabled() {
  static const bool Enabled = getenv("FEXMACX_TRACE") != nullptr;
  return Enabled;
}

const char* UnixSyscallName(uint32_t Number) {
  switch (Number) {
  case 1: return "exit";
  case 3: return "read";
  case 4: return "write";
  case 5: return "open";
  case 6: return "close";
  case 20: return "getpid";
  case 33: return "access";
  case 46: return "sigaction";
  case 48: return "sigprocmask";
  case 54: return "ioctl";
  case 73: return "munmap";
  case 74: return "mprotect";
  case 75: return "madvise";
  case 92: return "fcntl";
  case 116: return "gettimeofday";
  case 153: return "pread";
  case 169: return "csops";
  case 177: return "kdebug_typefilter";
  case 179: return "kdebug_trace64";
  case 197: return "mmap";
  case 202: return "sysctl";
  case 220: return "getattrlist";
  case 274: return "sysctlbyname";
  case 294: return "shared_region_check_np";
  case 327: return "issetugid";
  case 336: return "proc_info";
  case 360: return "bsdthread_create";
  case 361: return "bsdthread_terminate";
  case 338: return "stat64";
  case 339: return "fstat64";
  case 340: return "lstat64";
  case 344: return "getdirentries64";
  case 366: return "bsdthread_register";
  case 367: return "workq_open";
  case 372: return "thread_selfid";
  case 381: return "__mac_syscall";
  case 396: return "read_nocancel";
  case 397: return "write_nocancel";
  case 398: return "open_nocancel";
  case 399: return "close_nocancel";
  case 463: return "openat";
  case 470: return "fstatat64";
  case 483: return "csrctl";
  case 500: return "getentropy";
  case 515: return "ulock_wait";
  case 516: return "ulock_wake";
  case 520: return "terminate_with_payload";
  case 521: return "abort_with_payload";
  case 536: return "shared_region_map_and_slide_2_np";
  default: return "?";
  }
}

const char* MachTrapName(uint32_t Number) {
  switch (Number) {
  case 10: return "mach_vm_allocate";
  case 12: return "mach_vm_deallocate";
  case 14: return "mach_vm_protect";
  case 15: return "mach_vm_map";
  case 18: return "mach_port_deallocate";
  case 26: return "mach_reply_port";
  case 27: return "thread_self";
  case 28: return "task_self";
  case 29: return "host_self";
  case 31: return "mach_msg";
  case 32: return "mach_msg_overwrite";
  case 47: return "mach_msg2";
  case 89: return "mach_timebase_info";
  case 90: return "mach_wait_until";
  default: return "?";
  }
}

void TraceSyscall(const char* ClassName, uint32_t Number, const char* Name, const uint64_t (&Args)[8], uint64_t Result, bool Failed) {
  fprintf(stderr, "[trace] %s %u %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx) = 0x%llx%s\n", ClassName, Number, Name,
          (unsigned long long)Args[0], (unsigned long long)Args[1], (unsigned long long)Args[2], (unsigned long long)Args[3],
          (unsigned long long)Args[4], (unsigned long long)Result, Failed ? " (failed)" : "");
}

int HostProtectionFor(int GuestProtection) {
  int HostProtection = GuestProtection & (PROT_READ | PROT_WRITE);
  if (GuestProtection & PROT_EXEC) {
    HostProtection |= PROT_READ;
  }
  return HostProtection;
}

// this bum ahh function exists cuz x86 apps think pages are 4K and apple silicon pages are 16K
int ProtectGuestRange(uint64_t Address, uint64_t Size, int GuestProtection) {
  if (Size == 0) {
    return 0;
  }
  const uint64_t HostPageSize = getpagesize();
  const uint64_t Start = Address;
  const uint64_t End = Address + Size;
  const int HostProtection = HostProtectionFor(GuestProtection);

  const uint64_t FullPagesStart = FEXCore::AlignUp(Start, HostPageSize);
  const uint64_t FullPagesEnd = FEXCore::AlignDown(End, HostPageSize);
  if (FullPagesStart < FullPagesEnd) {
    if (mprotect(reinterpret_cast<void*>(FullPagesStart), FullPagesEnd - FullPagesStart, HostProtection) != 0) {
      return errno;
    }
  }

  if (HostProtection == PROT_NONE) {
    return 0;
  }
  const uint64_t FirstPage = FEXCore::AlignDown(Start, HostPageSize);
  const uint64_t LastPage = FEXCore::AlignDown(End - 1, HostPageSize);
  for (uint64_t Page : {FirstPage, LastPage}) {
    const bool FullyCovered = Page >= Start && Page + HostPageSize <= End;
    if (!FullyCovered) {
      mprotect(reinterpret_cast<void*>(Page), HostPageSize, PROT_READ | PROT_WRITE);
    }
  }
  return 0;
}

struct MigEmptyReply {
  mach_msg_header_t Header;
  NDR_record_t NDR;
  kern_return_t ReturnCode;
};

// objc registers x86 code as restartable ranges. the kernel cant restart guest code (the real pc is in the jit)
// and says no, so this answers the request locally with a success reply
bool AnswerRestartableRangesLocally(const uint64_t (&Args)[8], uint64_t& MachResult) {
  const uint64_t Options = Args[1];
  const uint32_t RemotePort = static_cast<uint32_t>(Args[3]);
  const uint32_t LocalPort = static_cast<uint32_t>(Args[3] >> 32);
  const uint32_t MessageID = static_cast<uint32_t>(Args[4] >> 32);
  const bool IsRestartableRanges = MessageID == MIG_TASK_RESTARTABLE_RANGES_REGISTER || MessageID == MIG_TASK_RESTARTABLE_RANGES_SYNCHRONIZE;
  if (!IsRestartableRanges || RemotePort != mach_task_self() || (Options & (MACH64_SEND_MSG | MACH64_RCV_MSG)) != (MACH64_SEND_MSG | MACH64_RCV_MSG)) {
    return false;
  }

  auto* Reply = reinterpret_cast<MigEmptyReply*>(Args[0]);
  memset(Reply, 0, sizeof(*Reply));
  Reply->Header.msgh_bits = MACH_MSGH_BITS(0, MACH_MSG_TYPE_MOVE_SEND_ONCE);
  Reply->Header.msgh_size = sizeof(MigEmptyReply);
  Reply->Header.msgh_remote_port = MACH_PORT_NULL;
  Reply->Header.msgh_local_port = LocalPort;
  Reply->Header.msgh_id = MessageID + MIG_REPLY_ID_OFFSET;
  Reply->NDR = NDR_record;
  Reply->ReturnCode = KERN_SUCCESS;
  MachResult = MACH_MSG_SUCCESS;
  return true;
}

HostSyscallResult DoHostSyscall(int64_t Number, const uint64_t (&Args)[8]) {
  register uint64_t x0 asm("x0") = Args[0];
  register uint64_t x1 asm("x1") = Args[1];
  register uint64_t x2 asm("x2") = Args[2];
  register uint64_t x3 asm("x3") = Args[3];
  register uint64_t x4 asm("x4") = Args[4];
  register uint64_t x5 asm("x5") = Args[5];
  register uint64_t x6 asm("x6") = Args[6];
  register uint64_t x7 asm("x7") = Args[7];
  register int64_t x16 asm("x16") = Number;
  uint64_t CarrySet {};
  asm volatile(
    "svc #0x80\n"
    "cset %[CarrySet], cs\n"
    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7), "+r"(x16), [CarrySet] "=&r"(CarrySet)
    :
    : "memory", "cc");
  return {x0, x1, CarrySet != 0};
}
} // namespace

void MacSyscallHandler::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) {
  auto& State = Frame->State;
  State.rip += SYSCALL_INSTRUCTION_SIZE;
  const uint64_t RawNumber = State.gregs[REG_RAX];
  const uint32_t Class = (RawNumber >> 24) & 0xFF;
  const uint32_t Number = RawNumber & 0xFF'FFFF;
  // args 7 and 8 sit on the guest stack above the return address of the libsystem_kernel stub
  const auto* GuestStack = reinterpret_cast<const uint64_t*>(State.gregs[REG_RSP]);
  const uint64_t Args[8] = {
    State.gregs[REG_RDI], State.gregs[REG_RSI], State.gregs[REG_RDX], State.gregs[REG_R10],
    State.gregs[REG_R8],  State.gregs[REG_R9],  GuestStack[1],        GuestStack[2],
  };

  switch (Class) {
  case SYSCALL_CLASS_UNIX: {
    if (Number == UNIX_SYSCALL_EXIT) {
      if (TracingEnabled()) {
        TraceSyscall("unix", Number, UnixSyscallName(Number), Args, 0, false);
      }
      fflush(nullptr);
    }
    HostSyscallResult Result {};
    if (Number == UNIX_SYSCALL_MPROTECT) {
      const int Error = ProtectGuestRange(Args[0], Args[1], static_cast<int>(Args[2]));
      Result = {static_cast<uint64_t>(Error), 0, Error != 0};
    } else if (Number == UNIX_SYSCALL_MUNMAP) {
      ReleaseGuestRange(Args[0], Args[1]);
      Result = {0, 0, false};
    } else if (Number == UNIX_SYSCALL_BSDTHREAD_REGISTER) {
      Threads.RecordRegistration(Args[0], Args[3], Args[4]);
      Result = {static_cast<uint64_t>(EINVAL), 0, true};
    } else if (Number == UNIX_SYSCALL_BSDTHREAD_CREATE) {
      int Error = 0;
      const uint64_t GuestPthread = Threads.Create(Args[0], Args[1], Args[2], Args[3], static_cast<uint32_t>(Args[4]), Error);
      Result = Error ? HostSyscallResult {static_cast<uint64_t>(Error), 0, true} : HostSyscallResult {GuestPthread, 0, false};
    } else if (Number == UNIX_SYSCALL_BSDTHREAD_TERMINATE) {
      if (TracingEnabled()) {
        TraceSyscall("unix", Number, "bsdthread_terminate", Args, 0, false);
      }
      Threads.Terminate(Args[0], Args[1], static_cast<uint32_t>(Args[2]), static_cast<uint32_t>(Args[3]));
    } else if (Number == UNIX_SYSCALL_SHARED_REGION_CHECK_NP) {
      // the kernel would point dyld at the arm64 cache. say theres none so x86_64 dyld maps its own
      Result = {static_cast<uint64_t>(EINVAL), 0, true};
    } else {
      Result = DoHostSyscall(Number, Args);
    }
    if (TracingEnabled()) {
      TraceSyscall("unix", Number, UnixSyscallName(Number), Args, Result.Value, Result.Failed);
      const bool TakesPathFirst = Number == 5 || Number == 33 || Number == 338 || Number == 340 || Number == 220;
      const bool TakesPathSecond = Number == 463 || Number == 470;
      if ((TakesPathFirst || TakesPathSecond) && (TakesPathFirst ? Args[0] : Args[1]) != 0) {
        fprintf(stderr, "[trace]     path \"%s\"\n", reinterpret_cast<const char*>(TakesPathFirst ? Args[0] : Args[1]));
      }
    }
    State.gregs[REG_RAX] = Result.Value;
    if (!Result.Failed) {
      State.gregs[REG_RDX] = Result.SecondValue;
    }

    uint32_t EFLAGS = CTX->ReconstructCompactedEFLAGS(Frame->Thread, false, nullptr, 0);
    EFLAGS = Result.Failed ? (EFLAGS | X86_EFLAGS_CARRY) : (EFLAGS & ~X86_EFLAGS_CARRY);
    CTX->SetFlagsFromCompactedEFLAGS(Frame->Thread, EFLAGS);
    break;
  }
  case SYSCALL_CLASS_MACH: {
    HostSyscallResult Result {};
    const bool IsOwnTask = Args[0] == mach_task_self();
    uint64_t LocalAnswer {};
    if (Number == MACH_TRAP_MSG2 && AnswerRestartableRangesLocally(Args, LocalAnswer)) {
      Result = {LocalAnswer, 0, false};
    } else if (Number == MACH_TRAP_VM_DEALLOCATE && IsOwnTask) {
      ReleaseGuestRange(Args[1], Args[2]);
      Result = {KERN_SUCCESS, 0, false};
    } else if (Number == MACH_TRAP_VM_PROTECT && IsOwnTask) {
      const bool SetMaximum = Args[3] != 0;
      int Error = 0;
      if (!SetMaximum) {
        Error = ProtectGuestRange(Args[1], Args[2], static_cast<int>(Args[4]));
      }
      Result = {static_cast<uint64_t>(Error == 0 ? KERN_SUCCESS : KERN_PROTECTION_FAILURE), 0, false};
    } else {
      Result = DoHostSyscall(-static_cast<int64_t>(Number), Args);
    }
    if (TracingEnabled()) {
      TraceSyscall("mach", Number, MachTrapName(Number), Args, Result.Value, false);
    }
    State.gregs[REG_RAX] = Result.Value;
    break;
  }
  case SYSCALL_CLASS_MDEP: {
    if (Number == MDEP_THREAD_FAST_SET_CTHREAD_SELF) {
      if (TracingEnabled()) {
        TraceSyscall("mdep", Number, "thread_fast_set_cthread_self", Args, 0, false);
      }
      State.gs_cached = Args[0];
      State.gregs[REG_RAX] = 0;
    } else {
      LogMan::Msg::EFmt("unhandled machdep syscall {}", Number);
      State.gregs[REG_RAX] = ENOSYS;
    }
    break;
  }
  default:
    LogMan::Msg::EFmt("unhandled syscall class {} number {}", Class, Number);
    State.gregs[REG_RAX] = ENOSYS;
    break;
  }
}
} // namespace FEXMacx
