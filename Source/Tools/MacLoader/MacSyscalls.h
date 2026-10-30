// SPDX-License-Identifier: MIT
/*
 macOS syscall bridge
 guest and host are both XNU so most syscalls can go straight to the real kernel
 x86_64 puts the class in the top byte of rax (1 = mach trap 2 = unix 3 = machdep)
 args come in rdi rsi rdx r10 r8 r9 and unix syscalls report errors with the carry flag
 on arm64 the same call is svc #0x80 with the number in x16 (mach traps are negative)
*/
#pragma once

#include <FEXCore/HLE/SyscallHandler.h>
#include <FEXCore/Utils/AllocatorHooks.h>

namespace FEXCore::Context {
class Context;
}

namespace FEXMacx {
class MacSyscallHandler final : public FEXCore::HLE::SyscallHandler, public FEXCore::Allocator::FEXAllocOperators {
public:
  explicit MacSyscallHandler(FEXCore::Context::Context* CTX)
    : CTX {CTX} {}

  void HandleSyscall(FEXCore::Core::CpuStateFrame* Frame) override;

  std::optional<FEXCore::ExecutableFileSectionInfo> LookupExecutableFileSection(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return std::nullopt;
  }

  FEXCore::HLE::ExecutableRangeInfo QueryGuestExecutableRange(FEXCore::Core::InternalThreadState*, uint64_t) override {
    return {0, UINT64_MAX, true};
  }

private:
  FEXCore::Context::Context* CTX;
};
} // namespace FEXMacx
