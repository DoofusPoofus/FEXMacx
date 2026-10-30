// SPDX-License-Identifier: MIT
#include "GuestThread.h"

#include <FEXCore/Debug/InternalThreadState.h>
#include <FEXCore/Utils/MathUtils.h>

#include <sys/mman.h>
#include <unistd.h>

namespace FEXMacx {
void ReleaseGuestRange(uint64_t Address, uint64_t Size) {
  const uint64_t HostPageSize = getpagesize();
  const uint64_t FullPagesStart = FEXCore::AlignUp(Address, HostPageSize);
  const uint64_t FullPagesEnd = FEXCore::AlignDown(Address + Size, HostPageSize);
  if (FullPagesStart < FullPagesEnd) {
    munmap(reinterpret_cast<void*>(FullPagesStart), FullPagesEnd - FullPagesStart);
  }
}

GuestThreadSetup::GuestThreadSetup(FEXCore::Core::InternalThreadState* Thread) {
  auto& State = Thread->CurrentFrame->State;

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
  const size_t AllocationSize = CallRetStackSize + 2 * HostPageSize;
  auto* Allocation = static_cast<uint8_t*>(mmap(nullptr, AllocationSize, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  if (Allocation == MAP_FAILED) {
    return;
  }
  if (mprotect(Allocation + HostPageSize, CallRetStackSize, PROT_READ | PROT_WRITE) != 0) {
    munmap(Allocation, AllocationSize);
    return;
  }

  CallRetAllocation = Allocation;
  CallRetAllocationSize = AllocationSize;
  Thread->CallRetStackBase = Allocation + HostPageSize;
  State.callret_sp = reinterpret_cast<uint64_t>(Thread->CallRetStackBase) + CallRetStackSize / 4;
}

GuestThreadSetup::~GuestThreadSetup() {
  if (CallRetAllocation) {
    munmap(CallRetAllocation, CallRetAllocationSize);
  }
}
} // namespace FEXMacx
