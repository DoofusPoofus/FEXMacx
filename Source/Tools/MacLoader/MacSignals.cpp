// SPDX-License-Identifier: MIT
#include "MacSignals.h"

#include "MacGuestFaults.h"

#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/SignalDelegator.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

namespace FEXMacx {
namespace {
using namespace FEXCore::X86State;

constexpr int GUEST_SIGNAL_COUNT = 32;
constexpr uint64_t GUEST_SIG_DFL = 0;
constexpr uint64_t GUEST_SIG_IGN = 1;

constexpr uint32_t UNIX_SIGACTION = 46;
constexpr uint32_t UNIX_SIGPROCMASK = 48;
constexpr uint32_t UNIX_SIGPENDING = 52;
constexpr uint32_t UNIX_SIGALTSTACK = 53;
constexpr uint32_t UNIX_SIGSUSPEND = 111;
constexpr uint32_t UNIX_SIGRETURN = 184;
constexpr uint32_t UNIX_PTHREAD_SIGMASK = 329;
constexpr uint32_t UNIX_SIGSUSPEND_NOCANCEL = 410;

constexpr int32_t UC_TRAD = 1;
constexpr int32_t UC_FLAVOR = 30;
constexpr int32_t UC_SET_ALT_STACK = 0x40000000;
constexpr int32_t UC_RESET_ALT_STACK = static_cast<int32_t>(0x80000000);

constexpr uint64_t X86_RED_ZONE = 128;
constexpr uint64_t X86_SIGTRAMP_LINKAGE = 112;
constexpr uint64_t USER64_CS = 0x2b;
constexpr uint16_t X86_TRAP_PAGE_FAULT = 14;
constexpr uint32_t X86_EFLAGS_TRAP = 1U << 8;
constexpr uint32_t X86_EFLAGS_DIRECTION = 1U << 10;
constexpr uint32_t X86_EFLAGS_USER_BITS = 0x0CD5;
constexpr uint32_t X86_EFLAGS_ALWAYS_SET = 0x0202;

constexpr uint64_t HOST_ALT_STACK_SIZE = 128 * 1024;
constexpr uint32_t STATE_REGISTER = 28;
constexpr uint32_t FILL_SRA_SINGLE_INST_REGISTER = 1;

constexpr uint32_t SignalBit(int Signal) {
  return 1U << (Signal - 1);
}

constexpr uint32_t FEXMACX_OWNED_SIGNALS = SignalBit(SIGSEGV) | SignalBit(SIGBUS) | SignalBit(SIGILL) | SignalBit(SIGTRAP);
constexpr uint32_t NEVER_BLOCKED_SIGNALS = SignalBit(SIGKILL) | SignalBit(SIGSTOP);
constexpr uint32_t IGNORED_BY_DEFAULT = SignalBit(SIGURG) | SignalBit(SIGCONT) | SignalBit(SIGCHLD) | SignalBit(SIGIO) | SignalBit(SIGWINCH) |
                                        SignalBit(SIGINFO);

struct GuestXnuMContext {
  uint16_t TrapNumber;
  uint16_t Cpu;
  uint32_t ErrorCode;
  uint64_t FaultAddress;
  uint64_t Registers[16];
  uint64_t Rip;
  uint64_t Rflags;
  uint64_t Cs;
  uint64_t Fs;
  uint64_t Gs;
  uint32_t FpuReserved[2];
  uint16_t Fcw;
  uint16_t Fsw;
  uint8_t Ftw;
  uint8_t FpuReserved1;
  uint16_t Fop;
  uint32_t FpuIp;
  uint16_t FpuCs;
  uint16_t FpuReserved2;
  uint32_t FpuDp;
  uint16_t FpuDs;
  uint16_t FpuReserved3;
  uint32_t Mxcsr;
  uint32_t MxcsrMask;
  uint8_t X87Registers[8][16];
  uint8_t Xmm[16][16];
  uint8_t FpuReserved4[96];
  uint32_t FpuReserved5;
  uint32_t TailPadding;
};
static_assert(offsetof(GuestXnuMContext, Rip) == 144);
static_assert(offsetof(GuestXnuMContext, Fcw) == 192);
static_assert(offsetof(GuestXnuMContext, Mxcsr) == 216);
static_assert(offsetof(GuestXnuMContext, X87Registers) == 224);
static_assert(offsetof(GuestXnuMContext, Xmm) == 352);
static_assert(sizeof(GuestXnuMContext) == 712);

struct GuestXnuUContext {
  int32_t OnStack;
  uint32_t SignalMask;
  uint64_t StackPointer;
  uint64_t StackSize;
  int32_t StackFlags;
  int32_t Padding;
  uint64_t Link;
  uint64_t MContextSize;
  uint64_t MContext;
};
static_assert(sizeof(GuestXnuUContext) == 56);
static_assert(sizeof(siginfo_t) == 104);

struct GuestXnuSigAction {
  uint64_t Handler;
  uint64_t Trampoline;
  uint32_t Mask;
  int32_t Flags;
};

struct GuestXnuOldSigAction {
  uint64_t Handler;
  uint32_t Mask;
  int32_t Flags;
};

struct GuestXnuStack {
  uint64_t Base;
  uint64_t Size;
  int32_t Flags;
  int32_t Padding;
};

constexpr int XnuToFexRegister[16] = {
  REG_RAX, REG_RBX, REG_RCX, REG_RDX, REG_RDI, REG_RSI, REG_RBP, REG_RSP, REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15,
};

struct GuestAction {
  uint64_t Handler {GUEST_SIG_DFL};
  uint64_t Trampoline {};
  uint32_t Mask {};
  int32_t Flags {};
};

struct alignas(64) HostContextBackup {
  uint64_t Registers[29];
  uint64_t FramePointer;
  uint64_t LinkRegister;
  uint64_t StackPointer;
  uint64_t ProgramCounter;
  uint32_t Cpsr;
  uint32_t Fpsr;
  uint32_t Fpcr;
  __uint128_t VectorRegisters[32];
  FEXCore::Core::CPUState GuestState;
  uint64_t OriginalRip;
  uint64_t GuestUContext;
  uint64_t InSyscallInfo;
  bool FaultToTop;
  HostContextBackup* Older;
};

struct SignalThreadState {
  std::atomic<uint32_t> PendingMask {};
  siginfo_t PendingInfo[GUEST_SIGNAL_COUNT] {};
  uint32_t GuestMask {};
  bool SuspendActive {};
  uint32_t MaskBeforeSuspend {};
  uint64_t AltStackBase {};
  uint64_t AltStackSize {};
  bool AltStackEnabled {};
  bool OnAltStack {};
  HostContextBackup* NewestBackup {};
  HostContextBackup* ReturningBackup {};
  uint64_t ReturningUContext {};
  void* HostAltStack {};
};

FEXCore::Context::Context* SignalContext {};
FEXCore::SignalDelegatorConfig DispatcherConfig {};
GuestAction Actions[GUEST_SIGNAL_COUNT] {};
std::mutex ActionLock;
pthread_key_t ThreadStateKey {};
pthread_t MainGuestThread {};
bool SignalsInstalled {};

SignalThreadState* CurrentSignalState() {
  return SignalsInstalled ? static_cast<SignalThreadState*>(pthread_getspecific(ThreadStateKey)) : nullptr;
}

sigset_t HostMaskFor(uint32_t GuestMask) {
  return static_cast<sigset_t>(GuestMask & ~FEXMACX_OWNED_SIGNALS);
}

void ApplyHostMask(uint32_t GuestMask) {
  const sigset_t HostMask = HostMaskFor(GuestMask);
  pthread_sigmask(SIG_SETMASK, &HostMask, nullptr);
}

void FreeSignalThreadState(void* RawState) {
  auto* State = static_cast<SignalThreadState*>(RawState);
  if (State->HostAltStack) {
    stack_t Disable {};
    Disable.ss_flags = SS_DISABLE;
    sigaltstack(&Disable, nullptr);
    munmap(State->HostAltStack, HOST_ALT_STACK_SIZE);
  }
  delete State;
}

void ForwardToMainGuestThread(int Signal) {
  if (MainGuestThread && !pthread_equal(pthread_self(), MainGuestThread)) {
    pthread_kill(MainGuestThread, Signal);
  }
}

uint32_t ReconstructFsw(const FEXCore::Core::CPUState& State) {
  return (State.flags[X87FLAG_TOP_LOC] << 11) | (State.flags[X87FLAG_C0_LOC] << 8) | (State.flags[X87FLAG_C1_LOC] << 9) |
         (State.flags[X87FLAG_C2_LOC] << 10) | (State.flags[X87FLAG_C3_LOC] << 14) | State.flags[X87FLAG_IE_LOC];
}

void SpillStaticRegisters(FEXCore::Core::InternalThreadState* Thread, ucontext_t* HostContext, uint32_t AlreadySpilledMask) {
  auto& HostRegisters = HostContext->uc_mcontext->__ss;
  auto& HostVectors = HostContext->uc_mcontext->__ns;
  auto& State = Thread->CurrentFrame->State;
  State.rip = SignalContext->RestoreRIPFromHostPC(Thread, HostRegisters.__pc);

  const uint64_t* HostGPRs = &HostRegisters.__x[0];
  for (size_t Index = 0; Index < DispatcherConfig.SRAGPRCount; ++Index) {
    const uint8_t HostIndex = DispatcherConfig.SRAGPRMapping[Index];
    if (AlreadySpilledMask & (1U << HostIndex)) {
      continue;
    }
    State.gregs[Index] = HostGPRs[HostIndex];
  }
  for (size_t Index = 0; Index < DispatcherConfig.SRAFPRCount; ++Index) {
    memcpy(&State.xmm.sse.data[Index][0], &HostVectors.__v[DispatcherConfig.SRAFPRMapping[Index]], sizeof(__uint128_t));
  }

  const uint32_t EFlags = SignalContext->ReconstructCompactedEFLAGS(Thread, true, HostGPRs, HostRegisters.__cpsr);
  SignalContext->SetFlagsFromCompactedEFLAGS(Thread, EFlags);
}

void LoadGuestMContext(FEXCore::Core::InternalThreadState* Thread, const GuestXnuMContext& MContext) {
  auto& State = Thread->CurrentFrame->State;
  for (int Index = 0; Index < 16; ++Index) {
    State.gregs[XnuToFexRegister[Index]] = MContext.Registers[Index];
  }
  State.rip = MContext.Rip;
  SignalContext->SetFlagsFromCompactedEFLAGS(Thread, (static_cast<uint32_t>(MContext.Rflags) & X86_EFLAGS_USER_BITS) | X86_EFLAGS_ALWAYS_SET);
  __uint128_t XmmValues[16];
  memcpy(XmmValues, MContext.Xmm, sizeof(XmmValues));
  SignalContext->SetXMMRegistersFromState(Thread, XmmValues, nullptr);
  State.mxcsr = MContext.Mxcsr & 0xFFFF;
  State.FCW = MContext.Fcw;
  State.AbridgedFTW = MContext.Ftw;
  State.flags[X87FLAG_IE_LOC] = MContext.Fsw & 1;
  State.flags[X87FLAG_C0_LOC] = (MContext.Fsw >> 8) & 1;
  State.flags[X87FLAG_C1_LOC] = (MContext.Fsw >> 9) & 1;
  State.flags[X87FLAG_C2_LOC] = (MContext.Fsw >> 10) & 1;
  State.flags[X87FLAG_C3_LOC] = (MContext.Fsw >> 14) & 1;
  State.flags[X87FLAG_TOP_LOC] = (MContext.Fsw >> 11) & 0b111;
  const uint32_t Top = State.flags[X87FLAG_TOP_LOC];
  for (uint32_t Index = 0; Index < 8; ++Index) {
    memcpy(&State.mm[(Index + Top) % 8], MContext.X87Registers[Index], sizeof(State.mm[0]));
  }
}

struct FaultDetails {
  uint16_t TrapNumber {};
  uint32_t ErrorCode {};
  uint64_t FaultAddress {};
};

uint64_t BuildGuestFrame(FEXCore::Core::InternalThreadState* Thread, SignalThreadState& Signals, int Signal, const siginfo_t& Info,
                         const GuestAction& Action, uint32_t MaskToRestore, uint32_t EFlags, const FaultDetails& Fault) {
  auto& State = Thread->CurrentFrame->State;
  const bool WasOnAltStack = Signals.OnAltStack;
  const bool SwitchToAltStack = (Action.Flags & SA_ONSTACK) && Signals.AltStackEnabled && !WasOnAltStack;

  uint64_t StackPointer = SwitchToAltStack ? Signals.AltStackBase + Signals.AltStackSize : State.gregs[REG_RSP];
  StackPointer -= X86_RED_ZONE;
  StackPointer -= sizeof(GuestXnuUContext);
  const uint64_t UContextAddress = StackPointer;
  StackPointer -= sizeof(siginfo_t);
  const uint64_t SigInfoAddress = StackPointer;
  StackPointer -= sizeof(GuestXnuMContext);
  const uint64_t MContextAddress = StackPointer;
  const uint64_t HandlerStack = FEXCore::AlignDown(StackPointer - X86_SIGTRAMP_LINKAGE, 16) - sizeof(uint64_t);

  GuestXnuMContext MContext {};
  MContext.TrapNumber = Fault.TrapNumber;
  MContext.ErrorCode = Fault.ErrorCode;
  MContext.FaultAddress = Fault.FaultAddress;
  for (int Index = 0; Index < 16; ++Index) {
    MContext.Registers[Index] = State.gregs[XnuToFexRegister[Index]];
  }
  MContext.Rip = State.rip;
  MContext.Rflags = EFlags;
  MContext.Cs = USER64_CS;
  MContext.Fcw = State.FCW;
  MContext.Fsw = ReconstructFsw(State);
  MContext.Ftw = State.AbridgedFTW;
  MContext.Mxcsr = State.mxcsr;
  MContext.MxcsrMask = 0xFFFF;
  const uint32_t Top = State.flags[X87FLAG_TOP_LOC];
  for (uint32_t Index = 0; Index < 8; ++Index) {
    memcpy(MContext.X87Registers[Index], &State.mm[(Index + Top) % 8], sizeof(MContext.X87Registers[0]));
  }
  __uint128_t XmmValues[16];
  SignalContext->ReconstructXMMRegisters(Thread, XmmValues, nullptr);
  memcpy(MContext.Xmm, XmmValues, sizeof(MContext.Xmm));

  GuestXnuUContext UContext {};
  UContext.OnStack = WasOnAltStack ? 1 : 0;
  UContext.SignalMask = MaskToRestore;
  UContext.StackPointer = HandlerStack;
  UContext.StackSize = SwitchToAltStack ? Signals.AltStackSize : 0;
  UContext.StackFlags = WasOnAltStack ? SS_ONSTACK : 0;
  UContext.MContextSize = sizeof(GuestXnuMContext);
  UContext.MContext = MContextAddress;

  memcpy(reinterpret_cast<void*>(MContextAddress), &MContext, sizeof(MContext));
  memcpy(reinterpret_cast<void*>(SigInfoAddress), &Info, sizeof(Info));
  memcpy(reinterpret_cast<void*>(UContextAddress), &UContext, sizeof(UContext));

  if (SwitchToAltStack) {
    Signals.OnAltStack = true;
  }

  State.rip = Action.Trampoline;
  State.gregs[REG_RSP] = HandlerStack;
  State.gregs[REG_RDI] = Action.Handler;
  State.gregs[REG_RSI] = (Action.Flags & SA_SIGINFO) ? UC_FLAVOR : UC_TRAD;
  State.gregs[REG_RDX] = static_cast<uint64_t>(Signal);
  State.gregs[REG_RCX] = SigInfoAddress;
  State.gregs[REG_R8] = UContextAddress;
  State.gregs[REG_R9] = UContextAddress;
  SignalContext->SetFlagsFromCompactedEFLAGS(Thread, EFlags & ~(X86_EFLAGS_TRAP | X86_EFLAGS_DIRECTION));
  return UContextAddress;
}

uint32_t MaskWhileHandling(int Signal, const GuestAction& Action, uint32_t CurrentMask) {
  uint32_t Mask = CurrentMask | Action.Mask;
  if (!(Action.Flags & SA_NODEFER)) {
    Mask |= SignalBit(Signal);
  }
  return Mask & ~NEVER_BLOCKED_SIGNALS;
}

void ResetHandlerIfAsked(int Signal) {
  if (!(Actions[Signal].Flags & SA_RESETHAND)) {
    return;
  }
  Actions[Signal].Handler = GUEST_SIG_DFL;
  if (!(FEXMACX_OWNED_SIGNALS & SignalBit(Signal))) {
    signal(Signal, SIG_DFL);
  }
}

bool DeliverFromHostContext(FEXCore::Core::InternalThreadState* Thread, SignalThreadState& Signals, int Signal, const siginfo_t& HostInfo,
                            ucontext_t* HostContext, bool GeneratedByGuest) {
  auto* Frame = Thread->CurrentFrame;
  int GuestSignal = GeneratedByGuest ? Frame->SynchronousFaultData.Signal : Signal;
  if (GuestSignal <= 0 || GuestSignal >= GUEST_SIGNAL_COUNT) {
    return false;
  }
  const GuestAction Action = Actions[GuestSignal];
  if (Action.Handler == GUEST_SIG_DFL || Action.Handler == GUEST_SIG_IGN || !Action.Trampoline || !Signals.HostAltStack) {
    return false;
  }

  auto& HostRegisters = HostContext->uc_mcontext->__ss;
  auto& HostVectors = HostContext->uc_mcontext->__ns;
  const bool InJIT = SignalContext->IsAddressInCodeBuffer(Thread, HostRegisters.__pc);
  if (!InJIT && !GeneratedByGuest) {
    return false;
  }

  const uint64_t BackupAddress = FEXCore::AlignDown(HostRegisters.__sp - X86_RED_ZONE - sizeof(HostContextBackup), alignof(HostContextBackup));
  auto* Backup = reinterpret_cast<HostContextBackup*>(BackupAddress);
  memcpy(Backup->Registers, HostRegisters.__x, sizeof(Backup->Registers));
  Backup->FramePointer = HostRegisters.__fp;
  Backup->LinkRegister = HostRegisters.__lr;
  Backup->StackPointer = HostRegisters.__sp;
  Backup->ProgramCounter = HostRegisters.__pc;
  Backup->Cpsr = HostRegisters.__cpsr;
  Backup->Fpsr = HostVectors.__fpsr;
  Backup->Fpcr = HostVectors.__fpcr;
  memcpy(Backup->VectorRegisters, HostVectors.__v, sizeof(Backup->VectorRegisters));
  memcpy(&Backup->GuestState, &Frame->State, sizeof(Frame->State));
  Backup->InSyscallInfo = Frame->InSyscallInfo;
  Backup->FaultToTop = Frame->SynchronousFaultData.FaultToTopAndGeneratedException;
  Frame->SynchronousFaultData.FaultToTopAndGeneratedException = false;
  HostRegisters.__sp = BackupAddress;

  if (InJIT) {
    SpillStaticRegisters(Thread, HostContext, Frame->InSyscallInfo & 0xFFFF);
    Frame->InSyscallInfo = 0;
  }
  Backup->OriginalRip = Frame->State.rip;

  siginfo_t GuestInfo = HostInfo;
  FaultDetails Fault {};
  if (GeneratedByGuest) {
    GuestInfo.si_signo = GuestSignal;
    GuestInfo.si_code = Frame->SynchronousFaultData.si_code;
    GuestInfo.si_addr = reinterpret_cast<void*>(Frame->State.rip);
    Fault.TrapNumber = Frame->SynchronousFaultData.TrapNo;
    Fault.ErrorCode = Frame->SynchronousFaultData.err_code;
  } else if (Signal == SIGSEGV || Signal == SIGBUS) {
    Fault.TrapNumber = X86_TRAP_PAGE_FAULT;
    Fault.FaultAddress = reinterpret_cast<uint64_t>(HostInfo.si_addr);
  }

  const uint32_t EFlags = SignalContext->ReconstructCompactedEFLAGS(Thread, false, nullptr, 0);
  const uint32_t MaskToRestore = Signals.GuestMask;
  Backup->GuestUContext = BuildGuestFrame(Thread, Signals, GuestSignal, GuestInfo, Action, MaskToRestore, EFlags, Fault);
  Backup->Older = Signals.NewestBackup;
  Signals.NewestBackup = Backup;
  ++Frame->SignalHandlerRefCounter;

  Signals.GuestMask = MaskWhileHandling(GuestSignal, Action, MaskToRestore);
  HostContext->uc_sigmask = HostMaskFor(Signals.GuestMask);
  ResetHandlerIfAsked(GuestSignal);

  HostRegisters.__pc = DispatcherConfig.AbsoluteLoopTopAddressFillSRA;
  HostRegisters.__x[STATE_REGISTER] = reinterpret_cast<uint64_t>(Frame);
  HostRegisters.__x[FILL_SRA_SINGLE_INST_REGISTER] = 0;
  return true;
}

bool FinishGuestSignalReturn(FEXCore::Core::InternalThreadState* Thread, SignalThreadState& Signals, ucontext_t* HostContext) {
  auto& HostRegisters = HostContext->uc_mcontext->__ss;
  auto& HostVectors = HostContext->uc_mcontext->__ns;
  auto* Backup = Signals.ReturningBackup;
  if (HostRegisters.__pc != DispatcherConfig.SignalHandlerReturnAddressRT || !Backup) {
    return false;
  }
  Signals.ReturningBackup = nullptr;

  memcpy(HostRegisters.__x, Backup->Registers, sizeof(Backup->Registers));
  HostRegisters.__fp = Backup->FramePointer;
  HostRegisters.__lr = Backup->LinkRegister;
  HostRegisters.__sp = Backup->StackPointer;
  HostRegisters.__pc = Backup->ProgramCounter;
  HostRegisters.__cpsr = Backup->Cpsr;
  HostVectors.__fpsr = Backup->Fpsr;
  HostVectors.__fpcr = Backup->Fpcr;
  memcpy(HostVectors.__v, Backup->VectorRegisters, sizeof(Backup->VectorRegisters));

  auto* Frame = Thread->CurrentFrame;
  memcpy(&Frame->State, &Backup->GuestState, sizeof(Frame->State));
  Frame->InSyscallInfo = Backup->InSyscallInfo;
  --Frame->SignalHandlerRefCounter;

  const auto* UContext = reinterpret_cast<const GuestXnuUContext*>(Signals.ReturningUContext);
  const auto* MContext = reinterpret_cast<const GuestXnuMContext*>(UContext->MContext);
  if (MContext->Rip != Backup->OriginalRip || Backup->FaultToTop) {
    LoadGuestMContext(Thread, *MContext);
    Frame->InSyscallInfo = 0;
    HostRegisters.__pc = DispatcherConfig.AbsoluteLoopTopAddressFillSRA;
    HostRegisters.__x[STATE_REGISTER] = reinterpret_cast<uint64_t>(Frame);
    HostRegisters.__x[FILL_SRA_SINGLE_INST_REGISTER] = 0;
  }
  HostContext->uc_sigmask = HostMaskFor(Signals.GuestMask);
  return true;
}

void MarkPending(SignalThreadState& Signals, int Signal, const siginfo_t* Info) {
  Signals.PendingInfo[Signal] = *Info;
  Signals.PendingMask.fetch_or(SignalBit(Signal));
}

void HostAsyncSignalHandler(int Signal, siginfo_t* Info, void* RawContext) {
  const int SavedErrno = errno;
  auto* Signals = CurrentSignalState();
  auto* Thread = Signals ? GetCurrentGuestThread() : nullptr;
  if (!Signals || !Thread) {
    if (Signals && pthread_equal(pthread_self(), MainGuestThread)) {
      MarkPending(*Signals, Signal, Info);
    } else {
      ForwardToMainGuestThread(Signal);
    }
    errno = SavedErrno;
    return;
  }

  auto* Context = static_cast<ucontext_t*>(RawContext);
  const bool Blocked = Signals->GuestMask & SignalBit(Signal);
  const bool SafeToEnter = Thread->CurrentFrame->State.DeferredSignalRefCount.Load() == 0;
  if (Blocked || !SafeToEnter || !DeliverFromHostContext(Thread, *Signals, Signal, *Info, Context, false)) {
    MarkPending(*Signals, Signal, Info);
  }
  errno = SavedErrno;
}

void UpdateHostDisposition(int Signal, const GuestAction& Action) {
  if (FEXMACX_OWNED_SIGNALS & SignalBit(Signal)) {
    return;
  }
  struct sigaction HostAction {};
  HostAction.sa_flags = Action.Flags & (SA_NOCLDSTOP | SA_NOCLDWAIT);
  if (Action.Handler == GUEST_SIG_DFL || Action.Handler == GUEST_SIG_IGN) {
    HostAction.sa_handler = Action.Handler == GUEST_SIG_DFL ? SIG_DFL : SIG_IGN;
    sigemptyset(&HostAction.sa_mask);
  } else {
    HostAction.sa_sigaction = HostAsyncSignalHandler;
    HostAction.sa_flags |= SA_SIGINFO | SA_ONSTACK;
    HostAction.sa_mask = static_cast<sigset_t>(~FEXMACX_OWNED_SIGNALS);
  }
  sigaction(Signal, &HostAction, nullptr);
}

bool SyscallNeverRestarts(uint32_t Number) {
  switch (Number) {
  case 93:
  case 111:
  case 184:
  case 230:
  case 330:
  case 334:
  case 363:
  case 369:
  case 374:
  case 375:
  case 394:
  case 395:
  case 407:
  case 410:
  case 417:
  case 423: return true;
  default: return false;
  }
}

int HandleSigAction(const uint64_t* Args) {
  const int Signal = static_cast<int>(Args[0]);
  const auto* NewAction = reinterpret_cast<const GuestXnuSigAction*>(Args[1]);
  auto* OldAction = reinterpret_cast<GuestXnuOldSigAction*>(Args[2]);
  if (Signal <= 0 || Signal >= GUEST_SIGNAL_COUNT || (NewAction && (Signal == SIGKILL || Signal == SIGSTOP))) {
    return EINVAL;
  }

  std::lock_guard Lock {ActionLock};
  const GuestAction Previous = Actions[Signal];
  GuestAction Incoming {};
  if (NewAction) {
    Incoming = {NewAction->Handler, NewAction->Trampoline, NewAction->Mask & ~NEVER_BLOCKED_SIGNALS, NewAction->Flags};
  }
  if (OldAction) {
    *OldAction = {Previous.Handler, Previous.Mask, Previous.Flags};
  }
  if (NewAction) {
    Actions[Signal] = Incoming;
    UpdateHostDisposition(Signal, Incoming);
  }
  return 0;
}

int HandleMaskChange(SignalThreadState& Signals, const uint64_t* Args) {
  const int How = static_cast<int>(Args[0]);
  const auto* NewSet = reinterpret_cast<const uint32_t*>(Args[1]);
  auto* OldSet = reinterpret_cast<uint32_t*>(Args[2]);
  const uint32_t Previous = Signals.GuestMask;
  if (NewSet) {
    uint32_t Mask = Previous;
    switch (How) {
    case SIG_BLOCK: Mask |= *NewSet; break;
    case SIG_UNBLOCK: Mask &= ~*NewSet; break;
    case SIG_SETMASK: Mask = *NewSet; break;
    default: return EINVAL;
    }
    Signals.GuestMask = Mask & ~NEVER_BLOCKED_SIGNALS;
  }
  if (OldSet) {
    *OldSet = Previous;
  }
  if (NewSet) {
    ApplyHostMask(Signals.GuestMask);
  }
  return 0;
}

int HandleAltStack(SignalThreadState& Signals, const uint64_t* Args) {
  const auto* NewStack = reinterpret_cast<const GuestXnuStack*>(Args[0]);
  auto* OldStack = reinterpret_cast<GuestXnuStack*>(Args[1]);
  GuestXnuStack Incoming {};
  if (NewStack) {
    Incoming = *NewStack;
  }
  if (OldStack) {
    OldStack->Base = Signals.AltStackBase;
    OldStack->Size = Signals.AltStackSize;
    OldStack->Flags = Signals.OnAltStack ? SS_ONSTACK : (Signals.AltStackEnabled ? 0 : SS_DISABLE);
  }
  if (!NewStack) {
    return 0;
  }
  if (Signals.OnAltStack) {
    return EPERM;
  }
  if (Incoming.Flags & ~SS_DISABLE) {
    return EINVAL;
  }
  if (Incoming.Flags & SS_DISABLE) {
    Signals.AltStackEnabled = false;
    return 0;
  }
  if (Incoming.Size < MINSIGSTKSZ) {
    return ENOMEM;
  }
  Signals.AltStackBase = Incoming.Base;
  Signals.AltStackSize = Incoming.Size;
  Signals.AltStackEnabled = true;
  return 0;
}

int HandleSuspend(SignalThreadState& Signals, uint32_t SuspendMask) {
  Signals.MaskBeforeSuspend = Signals.GuestMask;
  Signals.SuspendActive = true;
  Signals.GuestMask = SuspendMask & ~NEVER_BLOCKED_SIGNALS;
  const sigset_t HostMask = HostMaskFor(Signals.GuestMask);
  if ((Signals.PendingMask.load() & ~Signals.GuestMask) == 0) {
    sigsuspend(&HostMask);
  }
  if ((Signals.PendingMask.load() & ~Signals.GuestMask) == 0) {
    Signals.GuestMask = Signals.MaskBeforeSuspend;
    Signals.SuspendActive = false;
  }
  return EINTR;
}

bool HandleSigReturn(FEXCore::Core::CpuStateFrame* Frame, SignalThreadState& Signals, const uint64_t* Args, uint64_t& Result, bool& Failed) {
  const uint64_t UContextAddress = Args[0];
  const int32_t Style = static_cast<int32_t>(Args[1]);
  if (Style == UC_SET_ALT_STACK || Style == UC_RESET_ALT_STACK) {
    Signals.OnAltStack = Style == UC_SET_ALT_STACK;
    Result = 0;
    Failed = false;
    return false;
  }
  if (!UContextAddress) {
    Result = EINVAL;
    Failed = true;
    return false;
  }

  const auto* UContext = reinterpret_cast<const GuestXnuUContext*>(UContextAddress);
  Signals.OnAltStack = UContext->OnStack & 1;
  Signals.GuestMask = UContext->SignalMask & ~NEVER_BLOCKED_SIGNALS;

  HostContextBackup* Match = Signals.NewestBackup;
  while (Match && Match->GuestUContext != UContextAddress) {
    Match = Match->Older;
  }
  if (Match) {
    Signals.NewestBackup = Match->Older;
    Signals.ReturningBackup = Match;
    Signals.ReturningUContext = UContextAddress;
    const sigset_t Everything = static_cast<sigset_t>(~FEXMACX_OWNED_SIGNALS);
    pthread_sigmask(SIG_SETMASK, &Everything, nullptr);
    reinterpret_cast<void (*)()>(DispatcherConfig.SignalHandlerReturnAddressRT)();
    __builtin_unreachable();
  }

  LoadGuestMContext(Frame->Thread, *reinterpret_cast<const GuestXnuMContext*>(UContext->MContext));
  ApplyHostMask(Signals.GuestMask);
  return true;
}
} // namespace

bool GuestSignals::Install(const FEXCore::SignalDelegator* Delegator) {
  SignalContext = CTX;
  DispatcherConfig = Delegator->GetConfig();
  MainGuestThread = pthread_self();
  if (pthread_key_create(&ThreadStateKey, FreeSignalThreadState) != 0) {
    return false;
  }
  SignalsInstalled = true;
  return true;
}

void PrepareGuestSignalThread() {
  if (!SignalsInstalled) {
    return;
  }
  auto* Signals = CurrentSignalState();
  if (Signals) {
    Signals->NewestBackup = nullptr;
    Signals->ReturningBackup = nullptr;
    return;
  }

  Signals = new SignalThreadState {};
  sigset_t CurrentHostMask {};
  pthread_sigmask(SIG_BLOCK, nullptr, &CurrentHostMask);
  Signals->GuestMask = static_cast<uint32_t>(CurrentHostMask);

  void* AltStack = mmap(nullptr, HOST_ALT_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (AltStack != MAP_FAILED) {
    stack_t HostStack {};
    HostStack.ss_sp = AltStack;
    HostStack.ss_size = HOST_ALT_STACK_SIZE;
    if (sigaltstack(&HostStack, nullptr) == 0) {
      Signals->HostAltStack = AltStack;
    } else {
      munmap(AltStack, HOST_ALT_STACK_SIZE);
    }
  }
  pthread_setspecific(ThreadStateKey, Signals);
}

bool HandleGuestFaultSignal(int Signal, siginfo_t* Info, ucontext_t* Context) {
  auto* Signals = CurrentSignalState();
  auto* Thread = Signals ? GetCurrentGuestThread() : nullptr;
  if (!Thread) {
    return false;
  }
  const uint64_t PC = Context->uc_mcontext->__ss.__pc;
  if (Signal == SIGILL && FinishGuestSignalReturn(Thread, *Signals, Context)) {
    return true;
  }
  const bool InDispatcher = PC >= DispatcherConfig.DispatcherBegin && PC < DispatcherConfig.DispatcherEnd;
  const bool GeneratedByGuest =
    (Signal == SIGILL || Signal == SIGTRAP) && InDispatcher && Thread->CurrentFrame->SynchronousFaultData.FaultToTopAndGeneratedException;
  return DeliverFromHostContext(Thread, *Signals, Signal, *Info, Context, GeneratedByGuest);
}

bool GuestSignals::HandleSyscall(FEXCore::Core::CpuStateFrame* Frame, uint32_t Number, const uint64_t* Args, uint64_t& Result, bool& Failed,
                                 bool& StateAlreadySet) {
  auto* Signals = CurrentSignalState();
  if (!Signals) {
    return false;
  }
  int Error = 0;
  switch (Number) {
  case UNIX_SIGACTION: Error = HandleSigAction(Args); break;
  case UNIX_SIGPROCMASK:
  case UNIX_PTHREAD_SIGMASK: Error = HandleMaskChange(*Signals, Args); break;
  case UNIX_SIGPENDING: {
    sigset_t HostPending {};
    sigpending(&HostPending);
    *reinterpret_cast<uint32_t*>(Args[0]) = static_cast<uint32_t>(HostPending) | (Signals->PendingMask.load() & Signals->GuestMask);
    break;
  }
  case UNIX_SIGALTSTACK: Error = HandleAltStack(*Signals, Args); break;
  case UNIX_SIGSUSPEND:
  case UNIX_SIGSUSPEND_NOCANCEL: Error = HandleSuspend(*Signals, static_cast<uint32_t>(Args[0])); break;
  case UNIX_SIGRETURN: StateAlreadySet = HandleSigReturn(Frame, *Signals, Args, Result, Failed); return true;
  default: return false;
  }
  Result = static_cast<uint64_t>(Error);
  Failed = Error != 0;
  return true;
}

void GuestSignals::DeliverPendingSignals(FEXCore::Core::CpuStateFrame* Frame, uint32_t Number, uint64_t RawNumber, bool Interrupted) {
  auto* Signals = CurrentSignalState();
  if (!Signals) {
    return;
  }
  const uint32_t Deliverable = Signals->PendingMask.load() & ~Signals->GuestMask;
  if (!Deliverable) {
    return;
  }
  const int Signal = __builtin_ctz(Deliverable) + 1;
  Signals->PendingMask.fetch_and(~SignalBit(Signal));
  const siginfo_t Info = Signals->PendingInfo[Signal];

  const uint32_t MaskToRestore = Signals->SuspendActive ? Signals->MaskBeforeSuspend : Signals->GuestMask;
  Signals->SuspendActive = false;

  const GuestAction Action = Actions[Signal];
  if (Action.Handler == GUEST_SIG_IGN || !Action.Trampoline) {
    Signals->GuestMask = MaskToRestore;
    ApplyHostMask(MaskToRestore);
    return;
  }
  if (Action.Handler == GUEST_SIG_DFL) {
    Signals->GuestMask = MaskToRestore;
    ApplyHostMask(MaskToRestore);
    if (!(IGNORED_BY_DEFAULT & SignalBit(Signal))) {
      signal(Signal, SIG_DFL);
      raise(Signal);
    }
    return;
  }

  auto* Thread = Frame->Thread;
  auto& State = Frame->State;
  if (Interrupted && (Action.Flags & SA_RESTART) && !SyscallNeverRestarts(Number)) {
    State.rip -= 2;
    State.gregs[REG_RAX] = RawNumber;
  }
  const uint32_t EFlags = CTX->ReconstructCompactedEFLAGS(Thread, false, nullptr, 0);
  BuildGuestFrame(Thread, *Signals, Signal, Info, Action, MaskToRestore, EFlags, {});
  Signals->GuestMask = MaskWhileHandling(Signal, Action, Signals->GuestMask);
  ApplyHostMask(Signals->GuestMask);
  ResetHandlerIfAsked(Signal);
}
} // namespace FEXMacx
