// SPDX-License-Identifier: MIT
/*
 guest thread setup for macOS
 on linux FEXs thread manager gives evry guest thread a GDT with a 64 bit code segment
 and a call/ret stack with guard pages. nothing else does that on mac so this does
 keep the object alive as long as the thread runs cuz the thread state points into it
 ReleaseGuestRange only frees whole 16K host pages cuz x86 code frees in 4K steps and a 16K page
 can still hold other stuff the guest uses
*/
#pragma once

#include <FEXCore/Core/CoreState.h>

#include <cstddef>
#include <cstdint>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXMacx {
void ReleaseGuestRange(uint64_t Address, uint64_t Size);

class GuestThreadSetup final {
public:
  explicit GuestThreadSetup(FEXCore::Core::InternalThreadState* Thread);
  ~GuestThreadSetup();

  GuestThreadSetup(const GuestThreadSetup&) = delete;
  GuestThreadSetup& operator=(const GuestThreadSetup&) = delete;

  bool IsReady() const {
    return CallRetAllocation != nullptr;
  }

private:
  FEXCore::Core::CPUState::gdt_segment GuestGDT[32] {};
  uint8_t* CallRetAllocation {};
  size_t CallRetAllocationSize {};
};
} // namespace FEXMacx
