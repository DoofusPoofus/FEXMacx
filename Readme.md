# FEXMacx

FEX ported to macOS on apple silicon. the goal is running x86_64 mac apps without rosetta

**super early. nothing runs yet.** this is a seperate fork of [FEX](https://github.com/FEX-Emu/FEX) and has nothing to do with the FEX team so dont send FEXMacx bugs to them

### roadmap

- [x] FEXCore (the x86 → arm64 jit) builds on macOS
- [x] jit memory works with macOS W^X (`MAP_JIT` + `pthread_jit_write_protect_np`)
- [x] x86_64 smoke tests run on apple silicon (math loops stack call/ret sse x87 rep stos)
- [ ] 4K guest pages on top of 16K host pages
- [ ] FEX asm tests (they put code below 4GB which arm64 macOS never allows so they need moving)
- [x] Mach-O loader + XNU syscalls for tiny static x86_64 programs (`FEXMacx ./program`)
- [x] x86_64 dyld + the x86_64 shared cache (a normal `printf` hello world runs)
- [ ] threads signals and c++ exceptions
- [ ] x86_64 system frameworks beyond libSystem (Foundation AppKit)
- [ ] real apps

### try it

```
git clone --recursive https://github.com/DoofusPoofus/FEXMacx.git
cd FEXMacx
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DENABLE_LTO=FALSE -DTUNE_CPU=apple-m1
cmake --build build -j
./build/Bin/FEXMacxSmokeTest
unittests/MacOS/run_tests.sh build/Bin/FEXMacx
```

`FEXMacx ./some_x86_64_program args` runs x86_64 mac programs. dynamic ones go thru the real x86_64 dyld and the x86_64 shared cache

heads up: the x86_64 shared cache comes from rosettas cryptex (`/System/Volumes/Preboot/Cryptexes/Rosetta`) so rosetta has to be installed even tho it doesnt run anything

### stuff that will never work

arm64 macOS wont launch a process with less than 4GB of `__PAGEZERO`. so x86_64 apps that were linked to use memory below 4GB (some old games with LuaJIT do this) cant run here. rosetta can only do it cuz the kernel treats its processes as x86_64

below is the original FEX readme

---

[中文](https://github.com/FEX-Emu/FEX/blob/main/docs/Readme_CN.md)
# FEX: Emulate x86 Programs on ARM64
FEX allows you to run x86 applications on ARM64 Linux devices, similar to qemu-user and box64.
It offers broad compatibility with both 32-bit and 64-bit binaries, and it can be used alongside Wine/Proton to play Windows games.

It supports forwarding API calls to host system libraries like OpenGL or Vulkan to reduce emulation overhead.
An experimental code cache helps minimize in-game stuttering as much as possible.
Furthermore, a per-app configuration system allows tweaking performance per game, e.g. by skipping costly memory model emulation.
We also provide a user-friendly FEXConfig GUI to explore and change these settings.

## Prerequisites
FEX requires ARMv8.0-a or newer hardware, with minimum extensions FEAT_FP and FEAT_CRC32.

It has been tested with the following Linux distributions, though others are likely to work as well:

- Arch Linux
- Fedora Linux
- openSUSE
- Ubuntu 22.04/24.04/24.10/25.04

An x86-64 RootFS is required and can be downloaded using our `FEXRootFSFetcher` tool for many distributions.
For other distributions you will need to generate your own RootFS (our [wiki page](https://wiki.fex-emu.com/index.php/Development:Setting_up_RootFS) might help).

## Quick Start
### For Ubuntu 22.04, 24.04, 24.10 and 25.04
Execute the following command in the terminal to install FEX through a PPA.

```sh
curl --silent https://raw.githubusercontent.com/FEX-Emu/FEX/main/Scripts/InstallFEX.py | python3
```

This command will walk you through installing FEX through a PPA, and downloading a RootFS for use with FEX.

### For other Distributions
Follow the guide on the official FEX-Emu Wiki [here](https://wiki.fex-emu.com/index.php/Development:Setting_up_FEX).

### Navigating the Source
See the [Source Outline](docs/SourceOutline.md) for more information.
