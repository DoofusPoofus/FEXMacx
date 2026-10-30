// SPDX-License-Identifier: MIT
/*
 mach time for x86 guests
 on Intel macs mach_absolute_time counts nanoseconds (timebase 1/1) and x86 libdispatch counts on that
 apple silicon counts 24 MHz ticks (timebase 125/3) so evrything the guest hands the kernel in mach time
 gets turned into host ticks here: timer kevents with NOTE_MACHTIME or NOTE_MACH_CONTINUOUS_TIME
 (deadline and leeway) and mach_wait_until. the other way around gettimeofday hands back a mach time
 (libdispatch reads its timer clock from there) so that one gets turned back into ns
 mach_timebase_info answers 1/1 like intel does
*/
#pragma once

#include <cstdint>

namespace FEXMacx {
uint64_t GuestNanosecondsToHostTicks(uint64_t Nanoseconds);
uint64_t HostTicksToGuestNanoseconds(uint64_t Ticks);
void ConvertGuestTimerKevents(uint64_t EventList, uint64_t EventCount);
}
