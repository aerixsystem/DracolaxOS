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

The project currently contains several parts of a complete operating system:

- x86_64 freestanding kernel
- GDT, IDT and TSS
- Ring 0 and Ring 3 execution
- System calls
- Physical and virtual memory management
- Paging
- Process scheduling
- Userland applications
- ATA storage support
- PS/2 keyboard and mouse support
- USB support
- Ethernet networking
- ARP, IPv4, ICMP and UDP
- Virtual filesystem
- RAMFS and procfs
- Framebuffer graphics
- GUI compositor
- Window management
- LXScript
- Linux syscall compatibility work
- DracoShield security components
- DRX package and update system
- System services

Some of these components are still incomplete because I am on the foundation first.

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
13. Repeat

I also test some parts on real hardware when possible. QEMU is extremely useful, but hardware can expose problems that an emulator does not.

> I didn't log all problems I had...

## AI-assisted development

I use AI tools during development.

I mainly use them for things such as:

- Discussing possible designs
- Reviewing code
- Finding bugs
- Explaining unfamiliar concepts
- Generating ideas
- Helping investigate difficult problems

I do not treat generated code as automatically correct. Changes still have to make sense in the project and I test them myself.

A kernel is a particularly bad place to blindly trust generated code. One small mistake can turn into a completely broken system.

## Development environment

The project is primarily developed and tested on Linux.

The main tools used by the project include:

- GCC or an x86_64 cross compiler
- NASM
- GRUB tools
- Xorriso
- QEMU
- GDB
- Make

## Building

Install the required dependencies, if needed:

```bash
make install-deps
````

Build the kernel and ISO:

```bash
make
```

Run DracolaxOS in QEMU:

```bash
make run-qemu
```

Run with debugging enabled:

```bash
make run-debug
```

Run without the graphical interface:

```bash
make run-headless
```

Run in VirtualBox:

```bash
make run-vbox
```

Run the test suite:

```bash
make tests
```

Clean the build:

```bash
make clean
```

## Project structure

The repository is organized into several major parts:

```text
kernel/      Kernel code
gui/         GUI, compositor and window management
apps/        Userland applications
services/    System services
drx/         Package and update system
lxscript/    LXScript
libc/        Minimal freestanding libc
runtimes/    Runtime integrations
tools/       Development tools
build/       Build and boot configuration
docs/        Project documentation
tests/       Tests
storage/     Runtime storage tree
```

The project is still changing, so the structure may change as I keep working on it.

## Roadmap

The roadmap changes as the project develops.

| Phase | Focus                         | Status      |
| ----- | ----------------------------- | ----------- |
| 0     | Project restructure           | Complete    |
| 1     | Kernel stability and userland | Paused      |
| 2     | GUI and desktop               | Paused      |
| 3     | DRX package and update system | Planned     |
| 4     | Wine integration              | Planned     |
| 5     | LXScript userland APIs        | In progress |
| 6     | Hardware testing and release  | Planned     |
| 7     | Rust for Memory Manager       | Complete    |
| 7     | Stubs (audio, images, network)| In progress |

## Documentation

Some parts of the project have their own documentation:

* [`docs/STRUCTURE.md`](docs/STRUCTURE.md) - Source tree layout
* [`docs/DRX_SPEC.md`](docs/DRX_SPEC.md) - DRX design
* [`docs/DXI_FORMAT.md`](docs/DXI_FORMAT.md) - DXI icon format
* [`docs/WINE_INTEGRATION.md`](docs/WINE_INTEGRATION.md) - Wine integration design
* [`docs/architecture.md`](docs/architecture.md) - System architecture
* [`docs/api_reference.md`](docs/api_reference.md) - Kernel API

## Current state

DracolaxOS is not finished, not even close.

There are working components, experimental components and components that still need more work. 
I am currently focused on getting the existing system stable before continually adding new features (while also handling school and coming exams).

The goal is to have the things that are already implemented actually work together reliably.

Right now I am implementing Rust for the OS memory and also full networking. After that, audio is next.

## License

DracolaxOS is proprietary.

All rights reserved.
