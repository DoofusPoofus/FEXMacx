// SPDX-License-Identifier: MIT
/*
 FEXMacx smoke test
 runs tiny x86_64 snippets thru FEXCore on macOS and checks the registers after
 the snippets live in normal mmap memory above 4GB cuz arm64 macOS wont map anything lower
 evry snippet ends in hlt and FEX is told to exit on hlt so ExecuteThread comes back to us
 each test gets its own code page so FEX never reuses a block it compiled for an older test
 the bytes come from clang -target x86_64-apple-macos12 and the asm is kept next to them
*/

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include "DummyHandlers.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

namespace {
using namespace FEXCore::X86State;

struct ExpectedRegister {
  uint32_t Register;
  uint64_t Value;
};

struct SmokeTest {
  const char* Name;
  const char* Assembly;
  fextl::vector<uint8_t> Code;
  fextl::vector<ExpectedRegister> Expected;
};

constexpr const char* RegisterNames[] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                         "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

constexpr size_t CodePageSize = 0x4000;
constexpr size_t StackSize = 0x10'0000;

fextl::vector<SmokeTest> MakeSmokeTests() {
  return {
    {"add", "mov eax, 5; add eax, 37", {0xb8, 0x05, 0x00, 0x00, 0x00, 0x83, 0xc0, 0x25, 0xf4}, {{REG_RAX, 42}}},
    {"loop_sum",
     "xor eax, eax; mov ecx, 10; 1: add eax, ecx; dec ecx; jnz 1b",
     {0x31, 0xc0, 0xb9, 0x0a, 0x00, 0x00, 0x00, 0x01, 0xc8, 0xff, 0xc9, 0x75, 0xfa, 0xf4},
     {{REG_RAX, 55}, {REG_RCX, 0}}},
    {"mul_shift",
     "mov rax, 0x123456789; shl rax, 4; mov rbx, 3; imul rax, rbx",
     {0x48, 0xb8, 0x89, 0x67, 0x45, 0x23, 0x01, 0x00, 0x00, 0x00, 0x48, 0xc1, 0xe0, 0x04,
      0x48, 0xc7, 0xc3, 0x03, 0x00, 0x00, 0x00, 0x48, 0x0f, 0xaf, 0xc3, 0xf4},
     {{REG_RAX, 0x369d0369b0}}},
    {"push_pop",
     "mov rax, 0x1122334455667788; push rax; pop rbx",
     {0x48, 0xb8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x50, 0x5b, 0xf4},
     {{REG_RBX, 0x1122334455667788}}},
    {"call_ret", "call 2f; hlt; 2: mov eax, 7; ret", {0xe8, 0x01, 0x00, 0x00, 0x00, 0xf4, 0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3}, {{REG_RAX, 7}}},
    {"cmov",
     "mov eax, 3; mov ebx, 9; mov ecx, 100; mov edx, 200; cmp eax, ebx; cmovl ecx, edx",
     {0xb8, 0x03, 0x00, 0x00, 0x00, 0xbb, 0x09, 0x00, 0x00, 0x00, 0xb9, 0x64, 0x00, 0x00, 0x00,
      0xba, 0xc8, 0x00, 0x00, 0x00, 0x39, 0xd8, 0x0f, 0x4c, 0xca, 0xf4},
     {{REG_RCX, 200}}},
    {"sse_paddq",
     "mov rax, 40; movq xmm0, rax; mov rbx, 2; movq xmm1, rbx; paddq xmm0, xmm1; movq rax, xmm0",
     {0x48, 0xc7, 0xc0, 0x28, 0x00, 0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xc0, 0x48, 0xc7, 0xc3, 0x02, 0x00,
      0x00, 0x00, 0x66, 0x48, 0x0f, 0x6e, 0xcb, 0x66, 0x0f, 0xd4, 0xc1, 0x66, 0x48, 0x0f, 0x7e, 0xc0, 0xf4},
     {{REG_RAX, 42}}},
    {"x87",
     "fld1; fadd st(0), st(0); fadd st(0), st(0); fistp qword ptr [rsp - 16]; mov rax, qword ptr [rsp - 16]",
     {0xd9, 0xe8, 0xd8, 0xc0, 0xd8, 0xc0, 0xdf, 0x7c, 0x24, 0xf0, 0x48, 0x8b, 0x44, 0x24, 0xf0, 0xf4},
     {{REG_RAX, 4}}},
    {"div",
     "mov rax, 100; xor edx, edx; mov rcx, 7; div rcx",
     {0x48, 0xc7, 0xc0, 0x64, 0x00, 0x00, 0x00, 0x31, 0xd2, 0x48, 0xc7, 0xc1, 0x07, 0x00, 0x00, 0x00, 0x48, 0xf7, 0xf1, 0xf4},
     {{REG_RAX, 14}, {REG_RDX, 2}}},
    {"rep_stosb",
     "lea rdi, [rsp - 64]; mov rcx, 8; mov al, 0x5a; rep stosb; mov rax, qword ptr [rsp - 64]",
     {0x48, 0x8d, 0x7c, 0x24, 0xc0, 0x48, 0xc7, 0xc1, 0x08, 0x00, 0x00, 0x00, 0xb0, 0x5a, 0xf3, 0xaa, 0x48, 0x8b, 0x44, 0x24, 0xc0, 0xf4},
     {{REG_RAX, 0x5a5a5a5a5a5a5a5a}, {REG_RCX, 0}}},
  };
}

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fextl::fmt::print("{} {}\n", LogMan::DebugLevelStr(Level), Message);
}

void AssertHandler(const char* Message) {
  fextl::fmt::print("A {}\n", Message);
  fflush(nullptr);
}

bool RunSmokeTest(FEXCore::Context::Context* CTX, const SmokeTest& Test, uint8_t* CodePage, uint64_t StackTop) {
  memcpy(CodePage, Test.Code.data(), Test.Code.size());

  auto* Thread = CTX->CreateThread();
  auto& State = Thread->CurrentFrame->State;
  State.rip = reinterpret_cast<uint64_t>(CodePage);
  State.gregs[REG_RSP] = StackTop;

  FEXCore::Core::CPUState::gdt_segment GuestGDT[32] {};
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_GDT] = &GuestGDT[0];
  State.segment_arrays[FEXCore::Core::CPUState::SEGMENT_ARRAY_INDEX_LDT] = &GuestGDT[0];
  State.cs_idx = FEXCore::Core::CPUState::DEFAULT_USER_CS << 3;
  auto* CodeSegment = FEXCore::Core::CPUState::GetSegmentFromIndex(State, State.cs_idx);
  FEXCore::Core::CPUState::SetGDTBase(CodeSegment, 0);
  FEXCore::Core::CPUState::SetGDTLimit(CodeSegment, 0xF'FFFFU);
  CodeSegment->L = 1;
  CodeSegment->D = 0;
  State.cs_cached = FEXCore::Core::CPUState::CalculateGDTBase(*CodeSegment);

  const size_t HostPageSize = getpagesize();
  const size_t CallRetStackSize = FEXCore::AlignUp(FEXCore::Core::InternalThreadState::CALLRET_STACK_SIZE, HostPageSize);
  const size_t CallRetAllocationSize = CallRetStackSize + 2 * HostPageSize;
  auto* CallRetAllocation = static_cast<uint8_t*>(mmap(nullptr, CallRetAllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (CallRetAllocation == MAP_FAILED) {
    fextl::fmt::print("FAIL  {:10} couldnt map the call/ret stack\n", Test.Name);
    CTX->DestroyThread(Thread);
    return false;
  }
  mprotect(CallRetAllocation + HostPageSize, CallRetStackSize, PROT_READ | PROT_WRITE);
  Thread->CallRetStackBase = CallRetAllocation + HostPageSize;
  State.callret_sp = reinterpret_cast<uint64_t>(Thread->CallRetStackBase) + CallRetStackSize / 4;

  CTX->ExecuteThread(Thread);

  bool Passed = true;
  for (const auto& Expected : Test.Expected) {
    const auto Actual = State.gregs[Expected.Register];
    if (Actual != Expected.Value) {
      fextl::fmt::print("FAIL  {:10} {} = 0x{:x} (want 0x{:x})   [{}]\n", Test.Name, RegisterNames[Expected.Register], Actual,
                        Expected.Value, Test.Assembly);
      Passed = false;
    }
  }
  if (Passed) {
    fextl::fmt::print("PASS  {}\n", Test.Name);
  }

  CTX->DestroyThread(Thread);
  munmap(CallRetAllocation, CallRetAllocationSize);
  return Passed;
}
} // namespace

int main(int argc, char** argv, char** const envp) {
  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(envp));
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

  auto HostFeatures = FEX::FetchHostFeatures(true);
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  CTX->EnableExitOnHLT();

  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = FEX::DummyHandlers::CreateSyscallHandler();
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());

  if (!CTX->InitCore()) {
    fextl::fmt::print("FEXCore failed to start\n");
    return 1;
  }

  const auto Tests = MakeSmokeTests();
  const size_t CodeSize = CodePageSize * Tests.size();
  auto* CodeMemory = static_cast<uint8_t*>(mmap(nullptr, CodeSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  auto* StackMemory = static_cast<uint8_t*>(mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (CodeMemory == MAP_FAILED || StackMemory == MAP_FAILED) {
    fextl::fmt::print("couldnt map guest memory\n");
    return 1;
  }
  const uint64_t StackTop = reinterpret_cast<uint64_t>(StackMemory) + StackSize - 0x100;

  size_t FailedTests = 0;
  for (size_t TestIndex = 0; TestIndex < Tests.size(); ++TestIndex) {
    if (!RunSmokeTest(CTX.get(), Tests[TestIndex], CodeMemory + TestIndex * CodePageSize, StackTop)) {
      ++FailedTests;
    }
  }

  fextl::fmt::print("\n{} / {} passed\n", Tests.size() - FailedTests, Tests.size());
  return FailedTests == 0 ? 0 : 1;
}
