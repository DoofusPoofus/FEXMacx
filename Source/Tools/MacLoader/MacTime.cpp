// SPDX-License-Identifier: MIT
#include "MacTime.h"

#include <cstring>
#include <mach/mach_time.h>

namespace FEXMacx {
namespace {
constexpr int16_t EVFILT_TIMER_FILTER = -7;
constexpr uint32_t NOTE_LEEWAY_FLAG = 0x10;
constexpr uint32_t NOTE_MACH_CONTINUOUS_TIME_FLAG = 0x80;
constexpr uint32_t NOTE_MACHTIME_FLAG = 0x100;
constexpr uint64_t KEVENT_QOS_SIZE = 72;
constexpr uint64_t KEVENT_FILTER_OFFSET = 0x08;
constexpr uint64_t KEVENT_FFLAGS_OFFSET = 0x18;
constexpr uint64_t KEVENT_DATA_OFFSET = 0x20;
constexpr uint64_t KEVENT_LEEWAY_OFFSET = 0x28 + 8;

mach_timebase_info_data_t HostTimebase() {
  static const mach_timebase_info_data_t Timebase = [] {
    mach_timebase_info_data_t Info {};
    mach_timebase_info(&Info);
    return Info;
  }();
  return Timebase;
}
} // namespace

uint64_t GuestNanosecondsToHostTicks(uint64_t Nanoseconds) {
  const auto Timebase = HostTimebase();
  if (Timebase.numer == Timebase.denom || Nanoseconds == 0 || Nanoseconds == UINT64_MAX) {
    return Nanoseconds;
  }
  return static_cast<uint64_t>(static_cast<__uint128_t>(Nanoseconds) * Timebase.denom / Timebase.numer);
}

uint64_t HostTicksToGuestNanoseconds(uint64_t Ticks) {
  const auto Timebase = HostTimebase();
  if (Timebase.numer == Timebase.denom || Ticks == 0 || Ticks == UINT64_MAX) {
    return Ticks;
  }
  return static_cast<uint64_t>(static_cast<__uint128_t>(Ticks) * Timebase.numer / Timebase.denom);
}

void ConvertGuestTimerKevents(uint64_t EventList, uint64_t EventCount) {
  if (!EventList) {
    return;
  }
  for (uint64_t Index = 0; Index < EventCount; ++Index) {
    auto* Event = reinterpret_cast<uint8_t*>(EventList + Index * KEVENT_QOS_SIZE);
    int16_t Filter {};
    uint32_t FilterFlags {};
    memcpy(&Filter, Event + KEVENT_FILTER_OFFSET, sizeof(Filter));
    memcpy(&FilterFlags, Event + KEVENT_FFLAGS_OFFSET, sizeof(FilterFlags));
    if (Filter != EVFILT_TIMER_FILTER || !(FilterFlags & (NOTE_MACHTIME_FLAG | NOTE_MACH_CONTINUOUS_TIME_FLAG))) {
      continue;
    }

    uint64_t Deadline {};
    memcpy(&Deadline, Event + KEVENT_DATA_OFFSET, sizeof(Deadline));
    Deadline = GuestNanosecondsToHostTicks(Deadline);
    memcpy(Event + KEVENT_DATA_OFFSET, &Deadline, sizeof(Deadline));

    if (FilterFlags & NOTE_LEEWAY_FLAG) {
      uint64_t Leeway {};
      memcpy(&Leeway, Event + KEVENT_LEEWAY_OFFSET, sizeof(Leeway));
      Leeway = GuestNanosecondsToHostTicks(Leeway);
      memcpy(Event + KEVENT_LEEWAY_OFFSET, &Leeway, sizeof(Leeway));
    }
  }
}
} // namespace FEXMacx
