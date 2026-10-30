// SPDX-License-Identifier: MIT
#include "MacSyscalls.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Utils/LogManager.h>

#include <cerrno>
#include <cstdio>

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
constexpr uint32_t MDEP_THREAD_FAST_SET_CTHREAD_SELF = 3;
constexpr uint32_t X86_EFLAGS_CARRY = 1U << 0;

struct HostSyscallResult {
  uint64_t Value;
  uint64_t SecondValue;
  bool Failed;
};

HostSyscallResult DoHostSyscall(int64_t Number, const uint64_t (&Args)[6]) {
  register uint64_t x0 asm("x0") = Args[0];
  register uint64_t x1 asm("x1") = Args[1];
  register uint64_t x2 asm("x2") = Args[2];
  register uint64_t x3 asm("x3") = Args[3];
  register uint64_t x4 asm("x4") = Args[4];
  register uint64_t x5 asm("x5") = Args[5];
  register int64_t x16 asm("x16") = Number;
  uint64_t CarrySet {};
  asm volatile(
    "svc #0x80\n"
    "cset %[CarrySet], cs\n"
    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x16), [CarrySet] "=&r"(CarrySet)
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
  const uint64_t Args[6] = {
    State.gregs[REG_RDI], State.gregs[REG_RSI], State.gregs[REG_RDX], State.gregs[REG_R10], State.gregs[REG_R8], State.gregs[REG_R9],
  };

  switch (Class) {
  case SYSCALL_CLASS_UNIX: {
    if (Number == UNIX_SYSCALL_EXIT) {
      fflush(nullptr);
    }
    const auto Result = DoHostSyscall(Number, Args);
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
    const auto Result = DoHostSyscall(-static_cast<int64_t>(Number), Args);
    State.gregs[REG_RAX] = Result.Value;
    break;
  }
  case SYSCALL_CLASS_MDEP: {
    if (Number == MDEP_THREAD_FAST_SET_CTHREAD_SELF) {
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
