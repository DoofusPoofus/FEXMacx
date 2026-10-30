// SPDX-License-Identifier: MIT
#include "MacGuestFaults.h"

#include "GuestThread.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/ArchHelpers/Arm64.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <signal.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/ucontext.h>

namespace FEXMacx {
namespace {
constexpr uint64_t X86_COMMPAGE_ADDRESS = 0x7fff'ffe0'0000ULL;
constexpr uint64_t X86_COMMPAGE_SIZE = 0x2000;

constexpr uint64_t COMMPAGE_SIGNATURE = 0x000;
constexpr uint64_t COMMPAGE_CPU_CAPABILITIES64 = 0x010;
constexpr uint64_t COMMPAGE_VERSION = 0x01E;
constexpr uint64_t COMMPAGE_CPU_CAPABILITIES = 0x020;
constexpr uint64_t COMMPAGE_NCPUS = 0x022;
constexpr uint64_t COMMPAGE_CACHE_LINESIZE = 0x026;
constexpr uint64_t COMMPAGE_ACTIVE_CPUS = 0x034;
constexpr uint64_t COMMPAGE_PHYSICAL_CPUS = 0x035;
constexpr uint64_t COMMPAGE_LOGICAL_CPUS = 0x036;
constexpr uint64_t COMMPAGE_MEMORY_SIZE = 0x038;
constexpr uint64_t COMMPAGE_KERNEL_PAGE_SHIFT = 0x04D;
constexpr uint64_t COMMPAGE_USER_PAGE_SHIFT_64 = 0x04E;
constexpr uint64_t COMMPAGE_NT_TSC_BASE = 0x050;
constexpr uint64_t COMMPAGE_NT_SCALE = 0x058;
constexpr uint64_t COMMPAGE_NT_SHIFT = 0x05C;
constexpr uint64_t COMMPAGE_NT_NS_BASE = 0x060;
constexpr uint64_t COMMPAGE_NT_GENERATION = 0x068;

constexpr uint16_t COMMPAGE_THIS_VERSION = 14;

enum CPUCapability : uint64_t {
  HAS_MMX = 1ULL << 0,
  HAS_SSE = 1ULL << 1,
  HAS_SSE2 = 1ULL << 2,
  HAS_SSE3 = 1ULL << 3,
  CACHE64 = 1ULL << 5,
  FAST_THREAD_LOCAL_STORAGE = 1ULL << 7,
  HAS_SUPPLEMENTAL_SSE3 = 1ULL << 8,
  IS_64BIT = 1ULL << 9,
  HAS_SSE4_1 = 1ULL << 10,
  HAS_SSE4_2 = 1ULL << 11,
  HAS_AES = 1ULL << 12,
  NUM_CPUS_SHIFT = 16,
};

alignas(16) uint8_t FakeCommPage[X86_COMMPAGE_SIZE] {};
FEXCore::Context::Context* GuestContext {};
thread_local FEXCore::Core::InternalThreadState* CurrentGuestThread {};
struct sigaction PreviousSegvAction {};
struct sigaction PreviousBusAction {};
struct sigaction PreviousIllAction {};

template<typename T>
void WriteField(uint64_t Offset, T Value) {
  memcpy(&FakeCommPage[Offset], &Value, sizeof(T));
}

template<typename T>
T ReadSysctl(const char* Name, T Fallback) {
  T Value {};
  size_t Size = sizeof(Value);
  return sysctlbyname(Name, &Value, &Size, nullptr, 0) == 0 ? Value : Fallback;
}

void FillFakeCommPage() {
  memcpy(&FakeCommPage[COMMPAGE_SIGNATURE], "commpage 64-bit", 16);

  const uint32_t CPUCount = ReadSysctl<uint32_t>("hw.ncpu", 1);
  const uint8_t CPUCountByte = CPUCount > 0xFF ? 0xFF : static_cast<uint8_t>(CPUCount);
  const uint64_t Capabilities = HAS_MMX | HAS_SSE | HAS_SSE2 | HAS_SSE3 | CACHE64 | FAST_THREAD_LOCAL_STORAGE | HAS_SUPPLEMENTAL_SSE3 |
                                IS_64BIT | HAS_SSE4_1 | HAS_SSE4_2 | HAS_AES | (static_cast<uint64_t>(CPUCountByte) << NUM_CPUS_SHIFT);

  WriteField<uint64_t>(COMMPAGE_CPU_CAPABILITIES64, Capabilities);
  WriteField<uint16_t>(COMMPAGE_VERSION, COMMPAGE_THIS_VERSION);
  WriteField<uint32_t>(COMMPAGE_CPU_CAPABILITIES, static_cast<uint32_t>(Capabilities));
  WriteField<uint8_t>(COMMPAGE_NCPUS, CPUCountByte);
  WriteField<uint16_t>(COMMPAGE_CACHE_LINESIZE, 64);
  WriteField<uint8_t>(COMMPAGE_ACTIVE_CPUS, CPUCountByte);
  WriteField<uint8_t>(COMMPAGE_PHYSICAL_CPUS, static_cast<uint8_t>(ReadSysctl<uint32_t>("hw.physicalcpu", CPUCount)));
  WriteField<uint8_t>(COMMPAGE_LOGICAL_CPUS, static_cast<uint8_t>(ReadSysctl<uint32_t>("hw.logicalcpu", CPUCount)));
  WriteField<uint64_t>(COMMPAGE_MEMORY_SIZE, ReadSysctl<uint64_t>("hw.memsize", 0));
  WriteField<uint8_t>(COMMPAGE_KERNEL_PAGE_SHIFT, 12);
  WriteField<uint8_t>(COMMPAGE_USER_PAGE_SHIFT_64, 12);

  uint64_t CounterNow {};
  uint64_t CounterFrequency {};
  __asm volatile("isb; mrs %[Counter], cntvct_el0" : [Counter] "=r"(CounterNow));
  __asm volatile("mrs %[Frequency], cntfrq_el0" : [Frequency] "=r"(CounterFrequency));
  if (CounterFrequency == 0) {
    CounterFrequency = 24'000'000;
  }

  uint32_t Shift = 0;
  while ((1'000'000'000.0 / CounterFrequency) / static_cast<double>(1ULL << Shift) >= 1.0) {
    ++Shift;
  }
  const double Scale = (1'000'000'000.0 / CounterFrequency) / static_cast<double>(1ULL << Shift) * 4294967296.0;
  const uint64_t NanosecondsNow = static_cast<uint64_t>(static_cast<__uint128_t>(CounterNow) * 1'000'000'000ULL / CounterFrequency);

  WriteField<uint64_t>(COMMPAGE_NT_TSC_BASE, CounterNow);
  WriteField<uint32_t>(COMMPAGE_NT_SCALE, static_cast<uint32_t>(Scale));
  WriteField<uint32_t>(COMMPAGE_NT_SHIFT, Shift);
  WriteField<uint64_t>(COMMPAGE_NT_NS_BASE, NanosecondsNow);
  WriteField<uint32_t>(COMMPAGE_NT_GENERATION, 1);
}

bool CommPageTracing() {
  static const bool Enabled = getenv("FEXMACX_TRACE") != nullptr;
  return Enabled;
}

void TraceCommPageRead(uint64_t Offset, uint32_t Size) {
  char Line[64];
  const int Length = snprintf(Line, sizeof(Line), "[trace] commpage read +0x%llx size %u\n", (unsigned long long)Offset, Size);
  write(STDERR_FILENO, Line, Length);
}

uint64_t& HostRegister(ucontext_t* Context, uint32_t Register) {
  auto& ThreadState = Context->uc_mcontext->__ss;
  if (Register == 29) {
    return ThreadState.__fp;
  }
  if (Register == 30) {
    return ThreadState.__lr;
  }
  return ThreadState.__x[Register];
}

uint64_t ReadFakeCommPage(uint64_t Offset, uint32_t Size, bool SignExtend, bool To32Bit) {
  uint64_t Value {};
  memcpy(&Value, &FakeCommPage[Offset], Size);
  if (SignExtend && Size < 8) {
    const uint32_t ShiftAmount = 64 - Size * 8;
    Value = static_cast<uint64_t>(static_cast<int64_t>(Value << ShiftAmount) >> ShiftAmount);
    if (To32Bit) {
      Value &= 0xFFFF'FFFFULL;
    }
  }
  return Value;
}

bool EmulateCommPageLoad(ucontext_t* Context, uint64_t FaultAddress) {
  if (FaultAddress < X86_COMMPAGE_ADDRESS || FaultAddress >= X86_COMMPAGE_ADDRESS + X86_COMMPAGE_SIZE) {
    return false;
  }

  const uint64_t PC = Context->uc_mcontext->__ss.__pc;
  const uint32_t Instruction = *reinterpret_cast<const uint32_t*>(PC);
  const uint32_t Size = 1U << (Instruction >> 30);
  const uint32_t TargetRegister = Instruction & 0x1F;
  const uint32_t BaseRegister = (Instruction >> 5) & 0x1F;
  const uint64_t Offset = FaultAddress - X86_COMMPAGE_ADDRESS;
  if (Offset + Size > X86_COMMPAGE_SIZE) {
    return false;
  }

  bool SignExtend = false;
  bool To32Bit = false;
  bool IsLoad = false;
  std::optional<int64_t> BaseWriteback;

  const bool IsAcquireLoad = (Instruction & 0x3FFF'FC00) == 0x08DF'FC00 || (Instruction & 0x3FFF'FC00) == 0x38BF'C000;
  const bool IsUnsignedOffset = (Instruction & 0x3B00'0000) == 0x3900'0000;
  const bool IsUnscaledOrIndexedOrRegister = (Instruction & 0x3B00'0000) == 0x3800'0000;
  const bool IsRcpcUnscaled = (Instruction & 0x3F20'0C00) == 0x1900'0000;

  if (IsAcquireLoad) {
    IsLoad = true;
  } else if (IsUnsignedOffset || IsUnscaledOrIndexedOrRegister || IsRcpcUnscaled) {
    const uint32_t Opc = (Instruction >> 22) & 3;
    if (Opc == 0) {
      return false;
    }
    IsLoad = true;
    SignExtend = Opc >= 2;
    To32Bit = Opc == 3;

    if (IsUnscaledOrIndexedOrRegister && ((Instruction >> 21) & 1) == 0) {
      const uint32_t IndexMode = (Instruction >> 10) & 3;
      if (IndexMode == 1 || IndexMode == 3) {
        BaseWriteback = static_cast<int64_t>(static_cast<int32_t>(((Instruction >> 12) & 0x1FF) << 23) >> 23);
      }
    }
  }

  if (!IsLoad) {
    return false;
  }

  if (CommPageTracing()) {
    TraceCommPageRead(Offset, Size);
  }
  const uint64_t Value = ReadFakeCommPage(Offset, Size, SignExtend, To32Bit);
  if (TargetRegister != 31) {
    HostRegister(Context, TargetRegister) = Value;
  }
  if (BaseWriteback && BaseRegister != 31) {
    HostRegister(Context, BaseRegister) += *BaseWriteback;
  }
  Context->uc_mcontext->__ss.__pc = PC + 4;
  return true;
}

void ForwardToPrevious(const struct sigaction& Previous, int Signal, siginfo_t* Info, void* RawContext) {
  if (Previous.sa_flags & SA_SIGINFO) {
    if (Previous.sa_sigaction) {
      Previous.sa_sigaction(Signal, Info, RawContext);
      return;
    }
  } else if (Previous.sa_handler != SIG_DFL && Previous.sa_handler != SIG_IGN) {
    Previous.sa_handler(Signal);
    return;
  }
  signal(Signal, SIG_DFL);
  raise(Signal);
}

bool FixUnalignedJITAccess(ucontext_t* Context, siginfo_t* Info) {
  auto* Thread = CurrentGuestThread;
  const uint64_t PC = Context->uc_mcontext->__ss.__pc;
  if (!Thread || !GuestContext || Info->si_code != BUS_ADRALN || !GuestContext->IsAddressInCodeBuffer(Thread, PC)) {
    return false;
  }

  auto* GPRs = &Context->uc_mcontext->__ss.__x[0];
  const auto Result =
    FEXCore::ArchHelpers::Arm64::HandleUnalignedAccess(Thread, FEXCore::ArchHelpers::Arm64::UnalignedHandlerType::HalfBarrier, PC, GPRs);
  if (CommPageTracing()) {
    char Line[96];
    const int Length = snprintf(Line, sizeof(Line), "[trace] unaligned jit access pc 0x%llx addr 0x%llx -> %s %d\n", (unsigned long long)PC,
                                (unsigned long long)Info->si_addr, Result ? "fixed" : "unhandled", Result ? *Result : 0);
    write(STDERR_FILENO, Line, Length);
  }
  if (!Result) {
    return false;
  }
  Context->uc_mcontext->__ss.__pc = PC + *Result;
  return true;
}

void ReportGuestTrap(ucontext_t* Context) {
  auto* Thread = CurrentGuestThread;
  const uint64_t PC = Context->uc_mcontext->__ss.__pc;
  if (!Thread) {
    return;
  }
  const bool InJIT = GuestContext && GuestContext->IsAddressInCodeBuffer(Thread, PC);
  char Line[160];
  const int Length = snprintf(Line, sizeof(Line), "FEXMacx: guest trapped (ud2 / invalid instruction) near rip 0x%llx (host pc 0x%llx%s)\n",
                              (unsigned long long)Thread->CurrentFrame->State.rip, (unsigned long long)PC, InJIT ? " in jit code" : "");
  write(STDERR_FILENO, Line, Length);
}

bool ResetOverflowedCallRetStack(ucontext_t* Context, uint64_t FaultAddress) {
  auto* Thread = CurrentGuestThread;
  if (!Thread || !Thread->CallRetStackBase) {
    return false;
  }
  const auto Range = GetCallRetStackRange(Thread);
  if (FaultAddress < Range.AllocationStart || FaultAddress >= Range.AllocationEnd) {
    return false;
  }
  constexpr uint32_t CALLRET_STACK_REGISTER = 25;
  HostRegister(Context, CALLRET_STACK_REGISTER) = Range.DefaultLocation;
  return true;
}

void GuestFaultHandler(int Signal, siginfo_t* Info, void* RawContext) {
  auto* Context = static_cast<ucontext_t*>(RawContext);
  if (Signal != SIGILL && ResetOverflowedCallRetStack(Context, reinterpret_cast<uint64_t>(Info->si_addr))) {
    return;
  }
  if (Signal == SIGILL) {
    ReportGuestTrap(Context);
    ForwardToPrevious(PreviousIllAction, Signal, Info, RawContext);
    return;
  }
  if (EmulateCommPageLoad(Context, reinterpret_cast<uint64_t>(Info->si_addr))) {
    return;
  }
  if (Signal == SIGBUS && FixUnalignedJITAccess(Context, Info)) {
    return;
  }
  ForwardToPrevious(Signal == SIGBUS ? PreviousBusAction : PreviousSegvAction, Signal, Info, RawContext);
}
} // namespace

bool InstallGuestFaultHandlers(FEXCore::Context::Context* CTX) {
  GuestContext = CTX;
  FillFakeCommPage();

  struct sigaction Action {};
  Action.sa_sigaction = GuestFaultHandler;
  Action.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&Action.sa_mask);
  return sigaction(SIGSEGV, &Action, &PreviousSegvAction) == 0 && sigaction(SIGBUS, &Action, &PreviousBusAction) == 0 &&
         sigaction(SIGILL, &Action, &PreviousIllAction) == 0;
}

void SetCurrentGuestThread(FEXCore::Core::InternalThreadState* Thread) {
  CurrentGuestThread = Thread;
}
} // namespace FEXMacx
