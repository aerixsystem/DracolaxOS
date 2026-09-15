<picture>
  <source media="(prefers-color-scheme: dark)" srcset="./storage/main/system/images/icon-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="./storage/main/system/images/icon-black.svg">
  <img align="left" width="60" height="60" style="margin-right: 15px;" src="./storage/main/system/images/icon-black.svg">
</picture>

# DracolaxOS

A custom x86_64 operating system built from scratch in C and Assembly.

> **Current status:** Active development, AI-assisted with Claude Web.

## Why I started this

I wanted to learn how computers work below the level that I normally interact with as a programmer.
I started with the basic boot and kernel code and gradually added more parts of the system.

Instead of only reading about kernels and operating systems, I wanted to actually build one and deal with the problems myself.

That means DracolaxOS has gone through a lot of broken builds, crashes, incomplete drivers and things that worked in QEMU but did not behave the way I expected. 
At one point, QEMU was also getting too slow to use normally, which made development even more annoying.

## What I have built

* **Core Kernel & Hardware:** GDT/IDT/TSS, physical page allocator, virtual memory management, and process scheduling (supporting Ring 0 and Ring 3).
* **Storage & Drivers:** Basic ATA drive support, PS/2 keyboard/mouse input, and initial USB stack handling.
* **Networking & Filesystem:** Custom VFS supporting RAMFS and procfs, with a basic TCP/IP-adjacent stack (ARP, IPv4, ICMP, UDP).
* **Graphics & Shell:** Custom framebuffer compositor for basic window rendering, coupled with LXScript and initial Linux syscall stubs.

> *Note: Many high-level services (like DracoShield and DRX) are still skeleton implementations because I am focusing on stabilizing the kernel memory first.*

## How I develop DracolaxOS

Most of my development happens on Linux.

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

I use AI tools during development.

I mainly use them for things such as: Discussing possible designs, Reviewing code, Finding bugs, Explaining unfamiliar concepts, Generating ideas, Helping investigate difficult problems

*I do not treat generated code as automatically correct. Changes still have to make sense in the project and I test them myself.*

A kernel is a particularly bad place to blindly trust generated code. One small mistake can turn into a completely broken system.

## Development environment

The project is primarily developed and tested on Linux, specifically Dedian.

You will need a working x86_64 ELF cross-compiler toolchain (`x86_64-elf-gcc`, `nasm`), `grub-mkrescue`, `xorriso`, and `qemu-system-x86_64`.

## Building

First, pull the build dependencies:
```bash
make install-deps
````

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

The roadmap changes as the project develops.

| Phase | Focus                         | Status      |
| ----- | ----------------------------- | ----------- |
| 0     | Project restructure           | Complete    |
| 1     | GUI Bugs Fixes                | Complete?   |
| 2     | .dxi icon System              | Complete    |
| 3     | DRX package and update system | Planned     |
| 4     | Wine integration              | Planned     |
| 5     | LXScript userland APIs        | In progress |
| 6     | Hardware testing and release  | Planned     |
| 7     | Rust for Memory Manager       | Complete    |
| 8     | Stubs (audio, images, network)| In progress |
| X     | Kernel Hardening              | Complete    |

## Documentation

Some parts of the project have their own documentation:

* [`docs/STRUCTURE.md`](docs/STRUCTURE.md) -> Contains Source tree layout
* [`docs/DRX_SPEC.md`](docs/DRX_SPEC.md) -> Contains DRX design
* [`docs/DXI_FORMAT.md`](docs/DXI_FORMAT.md) -> Contains DXI icon format
* [`docs/WINE_INTEGRATION.md`](docs/WINE_INTEGRATION.md) -> Contains Wine integration design
* [`docs/architecture.md`](docs/architecture.md) -> Contains The System architecture
* [`docs/api_reference.md`](docs/api_reference.md) -> Contains Kernel APIs

## Current state

DracolaxOS is not finished, not even close.

There are working components, experimental components and components that still need more work. 
I am currently focused on getting the existing system stable before continually adding new features (while also handling school and coming exams).

The goal is to have the things that are already implemented actually work together reliably.

Right now I am implementing Rust for the OS memory and also full networking. After that, audio is next.

## License

DracolaxOS is proprietary.

All rights reserved.
