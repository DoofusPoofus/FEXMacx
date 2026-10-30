# FEXMacx

FEX ported to macOS on apple silicon. the goal is running x86_64 mac apps without rosetta

**super early. nothing runs yet.** this is a seperate fork of [FEX](https://github.com/FEX-Emu/FEX) and has nothing to do with the FEX team so dont send FEXMacx bugs to them

### roadmap

- [x] FEXCore (the x86 → arm64 jit) builds on macOS
- [ ] jit memory works with macOS W^X (`MAP_JIT` + `pthread_jit_write_protect_np`)
- [ ] FEX asm tests pass on apple silicon
- [ ] Mach-O loader + XNU syscalls for a tiny static x86_64 program
- [ ] x86_64 dyld and system frameworks
- [ ] real apps

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
