// SPDX-License-Identifier: MIT
#include "MachOImage.h"

#include <FEXCore/Utils/MathUtils.h>
#include <FEXCore/fextl/fmt.h>

#include <libkern/OSByteOrder.h>
#include <mach-o/fat.h>
#include <mach-o/loader.h>
#include <mach/machine.h>

#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace FEXMacx {
namespace {
constexpr uint32_t X86_THREAD_STATE64_FLAVOR = 4;
constexpr uint32_t X86_THREAD_STATE64_RIP_INDEX = 16;
constexpr uint64_t PREFERRED_GUEST_IMAGE_AREA = 0x4'0000'0000ULL;

template<typename T>
bool ReadAt(const fextl::vector<uint8_t>& Data, uint64_t Offset, T& Out) {
  if (Offset > Data.size() || Data.size() - Offset < sizeof(T)) {
    return false;
  }
  memcpy(&Out, Data.data() + Offset, sizeof(T));
  return true;
}

bool ReadWholeFile(const fextl::string& Path, fextl::vector<uint8_t>& Contents) {
  int FD = open(Path.c_str(), O_RDONLY | O_CLOEXEC);
  if (FD == -1) {
    return false;
  }
  struct stat FileInfo {};
  if (fstat(FD, &FileInfo) != 0) {
    close(FD);
    return false;
  }
  Contents.resize(FileInfo.st_size);
  size_t TotalRead = 0;
  while (TotalRead < Contents.size()) {
    const ssize_t ReadNow = read(FD, Contents.data() + TotalRead, Contents.size() - TotalRead);
    if (ReadNow <= 0) {
      close(FD);
      return false;
    }
    TotalRead += ReadNow;
  }
  close(FD);
  return true;
}

bool ExtractX86_64Slice(fextl::vector<uint8_t>& FileData, fextl::string& Error) {
  uint32_t Magic {};
  if (!ReadAt(FileData, 0, Magic)) {
    Error = "file is too small to be a mach-o";
    return false;
  }

  const uint32_t BigEndianMagic = OSSwapBigToHostInt32(Magic);
  if (BigEndianMagic != FAT_MAGIC && BigEndianMagic != FAT_MAGIC_64) {
    return true;
  }

  fat_header FatHeader {};
  ReadAt(FileData, 0, FatHeader);
  const uint32_t ArchCount = OSSwapBigToHostInt32(FatHeader.nfat_arch);
  for (uint32_t ArchIndex = 0; ArchIndex < ArchCount; ++ArchIndex) {
    uint64_t SliceOffset {};
    uint64_t SliceSize {};
    cpu_type_t CPUType {};
    if (BigEndianMagic == FAT_MAGIC) {
      fat_arch Arch {};
      if (!ReadAt(FileData, sizeof(fat_header) + ArchIndex * sizeof(fat_arch), Arch)) {
        break;
      }
      CPUType = OSSwapBigToHostInt32(Arch.cputype);
      SliceOffset = OSSwapBigToHostInt32(Arch.offset);
      SliceSize = OSSwapBigToHostInt32(Arch.size);
    } else {
      fat_arch_64 Arch {};
      if (!ReadAt(FileData, sizeof(fat_header) + ArchIndex * sizeof(fat_arch_64), Arch)) {
        break;
      }
      CPUType = OSSwapBigToHostInt32(Arch.cputype);
      SliceOffset = OSSwapBigToHostInt64(Arch.offset);
      SliceSize = OSSwapBigToHostInt64(Arch.size);
    }

    if (CPUType != CPU_TYPE_X86_64) {
      continue;
    }
    if (SliceOffset > FileData.size() || FileData.size() - SliceOffset < SliceSize) {
      Error = "x86_64 slice goes past the end of the file";
      return false;
    }
    FileData = fextl::vector<uint8_t>(FileData.begin() + SliceOffset, FileData.begin() + SliceOffset + SliceSize);
    return true;
  }

  Error = "universal binary has no x86_64 slice";
  return false;
}
} // namespace

std::optional<MachOImage> MachOImage::Open(const fextl::string& Path, fextl::string& Error) {
  MachOImage Image;
  if (!ReadWholeFile(Path, Image.SliceData)) {
    Error = fextl::fmt::format("couldnt read {}", Path);
    return std::nullopt;
  }
  if (!ExtractX86_64Slice(Image.SliceData, Error) || !Image.Parse(Error)) {
    return std::nullopt;
  }
  return Image;
}

bool MachOImage::Parse(fextl::string& Error) {
  mach_header_64 Header {};
  if (!ReadAt(SliceData, 0, Header) || Header.magic != MH_MAGIC_64) {
    Error = "not a 64-bit mach-o";
    return false;
  }
  if (Header.cputype != CPU_TYPE_X86_64) {
    Error = "mach-o is not x86_64";
    return false;
  }
  if (Header.filetype != MH_EXECUTE) {
    Error = "only executables can be launched";
    return false;
  }
  PositionIndependent = (Header.flags & MH_PIE) != 0;

  uint64_t CommandOffset = sizeof(mach_header_64);
  for (uint32_t CommandIndex = 0; CommandIndex < Header.ncmds; ++CommandIndex) {
    load_command Command {};
    if (!ReadAt(SliceData, CommandOffset, Command) || Command.cmdsize < sizeof(load_command)) {
      Error = "broken load command";
      return false;
    }

    switch (Command.cmd) {
    case LC_SEGMENT_64: {
      segment_command_64 Segment {};
      if (!ReadAt(SliceData, CommandOffset, Segment)) {
        Error = "broken segment command";
        return false;
      }
      Segments.push_back({
        .Name = fextl::string(Segment.segname, strnlen(Segment.segname, sizeof(Segment.segname))),
        .VMAddress = Segment.vmaddr,
        .VMSize = Segment.vmsize,
        .FileOffset = Segment.fileoff,
        .FileSize = Segment.filesize,
        .InitialProtection = static_cast<uint32_t>(Segment.initprot),
      });
      break;
    }
    case LC_UNIXTHREAD: {
      uint64_t StateOffset = CommandOffset + sizeof(load_command);
      const uint64_t CommandEnd = CommandOffset + Command.cmdsize;
      while (StateOffset + 8 <= CommandEnd) {
        uint32_t Flavor {};
        uint32_t Count {};
        ReadAt(SliceData, StateOffset, Flavor);
        ReadAt(SliceData, StateOffset + 4, Count);
        if (Flavor == X86_THREAD_STATE64_FLAVOR) {
          uint64_t RIP {};
          if (ReadAt(SliceData, StateOffset + 8 + X86_THREAD_STATE64_RIP_INDEX * sizeof(uint64_t), RIP)) {
            ThreadEntryPoint = RIP;
          }
        }
        StateOffset += 8 + static_cast<uint64_t>(Count) * sizeof(uint32_t);
      }
      break;
    }
    case LC_MAIN: {
      entry_point_command EntryCommand {};
      if (ReadAt(SliceData, CommandOffset, EntryCommand)) {
        MainEntryOffset = EntryCommand.entryoff;
      }
      break;
    }
    case LC_LOAD_DYLINKER: {
      dylinker_command LinkerCommand {};
      if (ReadAt(SliceData, CommandOffset, LinkerCommand) && LinkerCommand.name.offset < Command.cmdsize) {
        const char* LinkerName = reinterpret_cast<const char*>(SliceData.data() + CommandOffset + LinkerCommand.name.offset);
        DynamicLinkerPath = fextl::string(LinkerName, strnlen(LinkerName, Command.cmdsize - LinkerCommand.name.offset));
      }
      break;
    }
    default: break;
    }

    CommandOffset += Command.cmdsize;
  }

  if (!ThreadEntryPoint && !MainEntryOffset) {
    Error = "no entry point (no LC_UNIXTHREAD or LC_MAIN)";
    return false;
  }
  return true;
}

std::optional<MappedMachOImage> MachOImage::Map(fextl::string& Error) const {
  const size_t HostPageSize = getpagesize();
  uint64_t LowestAddress = UINT64_MAX;
  uint64_t HighestAddress = 0;
  for (const auto& Segment : Segments) {
    const bool IsPageZero = Segment.InitialProtection == 0 && Segment.FileSize == 0;
    if (IsPageZero || Segment.VMSize == 0) {
      continue;
    }
    LowestAddress = std::min(LowestAddress, Segment.VMAddress);
    HighestAddress = std::max(HighestAddress, Segment.VMAddress + Segment.VMSize);
  }
  if (LowestAddress >= HighestAddress) {
    Error = "image has no segments to map";
    return std::nullopt;
  }

  const uint64_t AlignedLowest = FEXCore::AlignDown(LowestAddress, HostPageSize);
  const uint64_t MappedSize = FEXCore::AlignUp(HighestAddress, HostPageSize) - AlignedLowest;
  void* Mapping = mmap(reinterpret_cast<void*>(PREFERRED_GUEST_IMAGE_AREA), MappedSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (Mapping == MAP_FAILED) {
    Error = fextl::fmt::format("couldnt reserve 0x{:x} bytes for the image", MappedSize);
    return std::nullopt;
  }

  const uint64_t MappedBase = reinterpret_cast<uint64_t>(Mapping);
  const uint64_t Slide = MappedBase - AlignedLowest;

  for (const auto& Segment : Segments) {
    if (Segment.FileSize == 0) {
      continue;
    }
    const uint64_t BytesToCopy = std::min(Segment.FileSize, Segment.VMSize);
    if (Segment.FileOffset > SliceData.size() || SliceData.size() - Segment.FileOffset < BytesToCopy) {
      Error = fextl::fmt::format("segment {} points past the end of the file", Segment.Name);
      munmap(Mapping, MappedSize);
      return std::nullopt;
    }
    memcpy(reinterpret_cast<void*>(Segment.VMAddress + Slide), SliceData.data() + Segment.FileOffset, BytesToCopy);
  }

  uint64_t EntryPoint {};
  if (ThreadEntryPoint) {
    EntryPoint = *ThreadEntryPoint + Slide;
  } else {
    auto TextSegment = std::find_if(Segments.begin(), Segments.end(), [](const MachOSegment& Segment) { return Segment.Name == "__TEXT"; });
    if (TextSegment == Segments.end()) {
      Error = "LC_MAIN binary without a __TEXT segment";
      munmap(Mapping, MappedSize);
      return std::nullopt;
    }
    EntryPoint = TextSegment->VMAddress + *MainEntryOffset + Slide;
  }

  return MappedMachOImage {
    .MappedBase = MappedBase,
    .MappedSize = MappedSize,
    .Slide = Slide,
    .EntryPoint = EntryPoint,
  };
}
} // namespace FEXMacx
