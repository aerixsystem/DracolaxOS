<picture>
  <source media="(prefers-color-scheme: dark)" srcset="./storage/main/system/images/icon-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="./storage/main/system/images/icon-black.svg">
  <img align="left" width="60" height="60" style="margin-right: 15px;" src="./storage/main/system/images/icon-black.svg">
</picture>

# DracolaxOS

A custom x86_64 operating system built from scratch in C and Assembly.

> **Current status:** Active development, AI-assisted with Claude Web.

## Why I started this

I started DracolaxOS because I wanted to make my own operating system instead of only using one. why? I don't know.

Basically... I wanted to see how far I could go, starting from the boot process and kernel and then adding memory management, GUI mode, services, my own language and formats.

A lot of this project came from experimenting. The programming language, formats, GUI, and a lot of other things were created because I wanted to see if I could actually make them work.
Some things looked simple from the outside, but they turned out to be much harder once I had to actually implement them and make them work together.

At one point, QEMU was also getting too slow to use normally, which made development even more annoying.

## What I currently have built

* GDT/IDT/TSS, physical page allocator, virtual memory management, and process scheduling (supporting Ring 0 and Ring 3)
* Basic ATA drive support, PS/2 keyboard/mouse input, and initial USB stack handling
* Custom VFS supporting RAMFS and procfs, plus a basic TCP/IP-adjacent stack (ARP, IPv4, ICMP, UDP)
* Custom framebuffer compositor for window rendering, LXScript, and initial Linux syscall stubs

> *Note: Many high-level services (like DracoShield and DRX) are still skeleton implementations because I am focusing on stabilizing the kernel memory first.*

## How I develop DracolaxOS

All the development happens on Linux, specifically Dedian.

I use QEMU a lot because constantly rebooting real hardware while working on a kernel gets painful very quickly. I use serial output and debugging tools to see what the kernel is doing when the graphical interface is not enough.

My usual workflow is roughly:

1. Build the kernel and image on my device
2. Boot it in QEMU
3. Check the serial output and GUI
4. Run tests for the affected subsystem
5. Fix whatever broken
6. Report to GPT, giving all logs
7. Get GPTs prompt
8. Give the prompt made by GPT to Claude
9. Wait hours for the usage limit to reset
10. Get the project zip
11. Extract
12. Repeat

I also test some parts on real hardware when possible. QEMU is extremely useful, but hardware can expose problems that an emulator does not.

> I didn't log all problems I had...

## AI-assisted development

I use AI tools during development, mainly GPT and Claude.
I use them when I get stuck, when I need another way to look at a problem, when I want code reviewed, or when I need help working through a subsystem.

The AI does not get to decide whether something belongs in DracolaxOS. 
I still have to understand what changed, test it, look at the results and decide what to keep, change or remove.

## Development environment

All the development happens on Linux, specifically Dedian.

The project uses an x86_64-elf-gcc cross-compiler toolchain, NASM, GRUB tools, Xorriso and QEMU.
I also use other tools (like python) when they are useful for debugging and testing on specific parts of the OS.

## Building

First, pull the build dependencies:
```bash
make install-deps
```

To compile, assemble code and output the bootable ISO image:
```bash
make
```

To test in QEMU (using serial output redirected to terminal):
```bash
make run-qemu
```

Run in Oracle VirtualBox:
```bash
make run-vbox
```

Some useful debug/testing flags:
```bash
make run-debug
make run-headless
make tests
```

Clean the build:
```bash
make clean
```

## Repository Structure

```text
apps/ -> OS built-in apps
build/ -> Contains Linker and Grub
docs/ -> Some Documents about the OS (AI Generated)
drx/ -> Future OS Updater, just a skeleton, it has nothing to do with the project at the moment
gui/ -> The GUI system of the OS
kernel/ -> The OS kernel
lxscript/ -> the OS programming language and also script
services/ -> OS services
storage/ -> This is nothing, just designing the OS FS as an actual storage on my linux (I will delete this in the future or the next update)
tests/ -> The OS tests
tools/ -> Tools I made with Gemini for the OS own image format
userland/ -> ...
```

The project is still changing, so the structure may change as I keep working on it.

## Roadmap

**Completed:**
* Phase 0: Project restructure
* Phase 1: GUI bug fixes
* Phase 2: `.dxi` icon system
* Phase 7: Rust memory manager refactor
* Phase X: Kernel hardening

**In Progress:**
* Phase 5: LXScript userland APIs
* Phase 8: Subsystem stubs (networking, audio, image decoders)

**Planned:**
* Phase 3: DRX package & update system
* Phase 4: Wine integration
* Phase 6: Real hardware testing & release

> *No promises that this roadmap is actually genuine...*

## Documentation

Some parts of the project have their own documentation:

* [`docs/STRUCTURE.md`](docs/STRUCTURE.md) -> Source tree layout
* [`docs/DRX_SPEC.md`](docs/DRX_SPEC.md) -> DRX design spec
* [`docs/DXI_FORMAT.md`](docs/DXI_FORMAT.md) -> DXI icon format
* [`docs/WINE_INTEGRATION.md`](docs/WINE_INTEGRATION.md) -> Wine integration design
* [`docs/architecture.md`](docs/architecture.md) -> System architecture
* [`docs/api_reference.md`](docs/api_reference.md) -> Kernel APIs

## Current state

DracolaxOS is not finished, not even close.

There are working components, experimental components and components that still need more work. 
I am currently focused on getting the existing system stable before continually adding new features (while also handling school and coming exams).

The goal is to have the things that are already implemented actually work together.

Right now I am implementing Network. After that, audio is next.

## License

DracolaxOS is proprietary.

All rights reserved.
