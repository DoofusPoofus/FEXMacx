// SPDX-License-Identifier: MIT
/*
 host fault handling for guest code on macOS
 1 the fake x86_64 commpage
   x86_64 code expects a kernel page at 0x7fffffe00000 with cpu info timing data and flags
   an arm64 process can never map that high so jitted loads from it fault and get emulated
   from our own copy of the page
 2 unaligned atomics
   FEX uses arm64 acquire/release loads and stores for x86 memory ordering and those trap on
   unaligned addresses. FEXCore knows how to backpatch them so the SIGBUS just gets handed to it
 3 call/ret stack overflow
   FEX keeps a host side call/ret stack in x25. running into its guard pages just puts x25 back
   to the default spot like linux FEX does
 4 everything else goes to MacSignals first so guest handlers (and sigreturn) get a shot before we crash
*/
#pragma once

namespace FEXCore::Context {
class Context;
}
namespace FEXCore::Core {
struct InternalThreadState;
}

namespace FEXMacx {
bool InstallGuestFaultHandlers(FEXCore::Context::Context* CTX);
void SetCurrentGuestThread(FEXCore::Core::InternalThreadState* Thread);
FEXCore::Core::InternalThreadState* GetCurrentGuestThread();
} // namespace FEXMacx
