// SPDX-License-Identifier: MIT
/*
 mach-o reader + mapper for x86_64 guest binaries
 takes thin x86_64 files or universal (fat) files and pulls out the x86_64 slice
 arm64 macOS never lets us map below 4GB and not even rite after it, so the image always gets slid
 to wherever the kernel gives us room. thats fine for PIE binaries (evry normal app since 10.7)
 the whole image gets mapped read+write in one go. real per segment protection needs the 4K page work first
*/
#pragma once

#include <FEXCore/fextl/string.h>
#include <FEXCore/fextl/vector.h>

#include <cstdint>
#include <optional>

namespace FEXMacx {
struct MachOSegment {
  fextl::string Name;
  uint64_t VMAddress;
  uint64_t VMSize;
  uint64_t FileOffset;
  uint64_t FileSize;
  uint32_t InitialProtection;
};

struct MappedMachOImage {
  uint64_t MappedBase;
  uint64_t MappedSize;
  uint64_t Slide;
  uint64_t EntryPoint;
  uint64_t HeaderAddress;
};

class MachOImage final {
public:
  enum class Kind {
    Executable,
    DynamicLinker,
  };

  static std::optional<MachOImage> Open(const fextl::string& Path, Kind ExpectedKind, fextl::string& Error);

  std::optional<MappedMachOImage> Map(fextl::string& Error) const;

  bool NeedsDynamicLinker() const {
    return !DynamicLinkerPath.empty();
  }
  const fextl::string& GetDynamicLinkerPath() const {
    return DynamicLinkerPath;
  }
  bool IsPositionIndependent() const {
    return PositionIndependent;
  }
  const fextl::vector<MachOSegment>& GetSegments() const {
    return Segments;
  }

private:
  bool Parse(Kind ExpectedKind, fextl::string& Error);

  fextl::vector<uint8_t> SliceData;
  fextl::vector<MachOSegment> Segments;
  fextl::string DynamicLinkerPath;
  std::optional<uint64_t> ThreadEntryPoint;
  std::optional<uint64_t> MainEntryOffset;
  bool PositionIndependent {};
};
} // namespace FEXMacx
