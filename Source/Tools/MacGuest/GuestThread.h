// SPDX-License-Identifier: MIT
/*
 guest thread setup for macOS
 on linux FEXs thread manager gives evry guest thread a GDT with a 64 bit code segment
 and a call/ret stack with guard pages. nothing else does that on mac so this does
 keep the object alive as long as the thread runs cuz the thread state points into it
*/
#pragma once

#include <FEXCore/Core/CoreState.h>

#include <cstddef>
#include <cstdint>

namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXMacx {
// x86 apps think pages are 4K but apple silicon pages are 16K. freeing a 4K aligned range would take
// the whole 16K page with it so only pages that are fully inside the range get released
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
