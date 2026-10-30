// SPDX-License-Identifier: MIT
/*
 FEXMacx ./some_x86_64_program [args...]
 loads an x86_64 mach-o and runs it thru FEXCore on apple silicon
 the startup stack looks like wat the macOS kernel builds for a new process
 argc then argv then a null then envp then a null then the apple strings then a null
 dynamic programs (LC_LOAD_DYLINKER) need the x86_64 dyld which isnt done yet
*/

#include "MacSyscalls.h"
#include "MachOImage.h"

#include "Common/Config.h"
#include "Common/HostFeatures.h"
#include "DummyHandlers.h"
#include "GuestThread.h"

#include <FEXCore/Config/Config.h>
#include <FEXCore/Core/Context.h>
#include <FEXCore/Core/CoreState.h>
#include <FEXCore/Core/X86Enums.h>
#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/LogManager.h>
#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/fextl/fmt.h>
#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <cstdio>
#include <cstring>
#include <sys/mman.h>

namespace {
constexpr size_t GuestStackSize = 8 * 1024 * 1024;

void MsgHandler(LogMan::DebugLevels Level, const char* Message) {
  fprintf(stderr, "%s %s\n", LogMan::DebugLevelStr(Level), Message);
}

void AssertHandler(const char* Message) {
  fprintf(stderr, "A %s\n", Message);
  fflush(nullptr);
}

uint64_t BuildInitialStack(uint8_t* StackMemory, const fextl::vector<fextl::string>& Arguments, char** const Environment,
                           const fextl::string& ExecutablePath) {
  uint64_t Cursor = reinterpret_cast<uint64_t>(StackMemory) + GuestStackSize;
  auto CopyString = [&Cursor](std::string_view Text) {
    Cursor -= Text.size() + 1;
    memcpy(reinterpret_cast<void*>(Cursor), Text.data(), Text.size());
    reinterpret_cast<char*>(Cursor)[Text.size()] = '\0';
    return Cursor;
  };

  fextl::vector<uint64_t> ArgumentPointers;
  for (const auto& Argument : Arguments) {
    ArgumentPointers.push_back(CopyString(Argument));
  }
  fextl::vector<uint64_t> EnvironmentPointers;
  for (char* const* Variable = Environment; Variable && *Variable; ++Variable) {
    EnvironmentPointers.push_back(CopyString(*Variable));
  }
  fextl::vector<uint64_t> AppleStringPointers;
  AppleStringPointers.push_back(CopyString(fextl::fmt::format("executable_path={}", ExecutablePath)));

  const size_t SlotCount = 1 + ArgumentPointers.size() + 1 + EnvironmentPointers.size() + 1 + AppleStringPointers.size() + 1;
  Cursor = FEXCore::AlignDown(Cursor - SlotCount * sizeof(uint64_t), 16);

  auto* Slot = reinterpret_cast<uint64_t*>(Cursor);
  *Slot++ = ArgumentPointers.size();
  for (auto Pointer : ArgumentPointers) {
    *Slot++ = Pointer;
  }
  *Slot++ = 0;
  for (auto Pointer : EnvironmentPointers) {
    *Slot++ = Pointer;
  }
  *Slot++ = 0;
  for (auto Pointer : AppleStringPointers) {
    *Slot++ = Pointer;
  }
  *Slot++ = 0;

  return Cursor;
}
} // namespace

int main(int argc, char** argv, char** const envp) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <x86_64 mach-o> [args...]\n", argv[0]);
    return 1;
  }

  LogMan::Throw::InstallHandler(AssertHandler);
  LogMan::Msg::InstallHandler(MsgHandler);

  FEX::Config::InitializeConfigs(FEX::Config::PortableInformation {});
  FEXCore::Config::Initialize();
  FEXCore::Config::AddLayer(FEX::Config::CreateEnvironmentLayer(envp));
  FEXCore::Config::Load();
  FEXCore::Config::Set(FEXCore::Config::CONFIG_IS64BIT_MODE, "1");

  const fextl::string ProgramPath = argv[1];
  fextl::string Error;
  auto Image = FEXMacx::MachOImage::Open(ProgramPath, Error);
  if (!Image) {
    fprintf(stderr, "FEXMacx: %s: %s\n", ProgramPath.c_str(), Error.c_str());
    return 1;
  }
  if (Image->NeedsDynamicLinker()) {
    fprintf(stderr, "FEXMacx: %s needs %s and dyld support isnt done yet\n", ProgramPath.c_str(), Image->GetDynamicLinkerPath().c_str());
    return 1;
  }

  auto MappedImage = Image->Map(Error);
  if (!MappedImage) {
    fprintf(stderr, "FEXMacx: %s: %s\n", ProgramPath.c_str(), Error.c_str());
    return 1;
  }

  auto HostFeatures = FEX::FetchHostFeatures(true);
  auto CTX = FEXCore::Context::Context::CreateNewContext(HostFeatures);
  auto SignalDelegation = FEX::DummyHandlers::CreateSignalDelegator();
  auto SyscallHandler = fextl::make_unique<FEXMacx::MacSyscallHandler>(CTX.get());
  CTX->SetSignalDelegator(SignalDelegation.get());
  CTX->SetSyscallHandler(SyscallHandler.get());
  if (!CTX->InitCore()) {
    fprintf(stderr, "FEXMacx: FEXCore failed to start\n");
    return 1;
  }

  auto* StackMemory = static_cast<uint8_t*>(mmap(nullptr, GuestStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (StackMemory == MAP_FAILED) {
    fprintf(stderr, "FEXMacx: couldnt map the guest stack\n");
    return 1;
  }

  fextl::vector<fextl::string> GuestArguments;
  for (int ArgumentIndex = 1; ArgumentIndex < argc; ++ArgumentIndex) {
    GuestArguments.emplace_back(argv[ArgumentIndex]);
  }

  auto* Thread = CTX->CreateThread();
  FEXMacx::GuestThreadSetup ThreadSetup {Thread};
  if (!ThreadSetup.IsReady()) {
    fprintf(stderr, "FEXMacx: couldnt set up the guest thread\n");
    return 1;
  }

  auto& State = Thread->CurrentFrame->State;
  State.rip = MappedImage->EntryPoint;
  State.gregs[FEXCore::X86State::REG_RSP] = BuildInitialStack(StackMemory, GuestArguments, envp, ProgramPath);

  CTX->ExecuteThread(Thread);
  return 0;
}
