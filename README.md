<picture>
  <source media="(prefers-color-scheme: dark)" srcset="./storage/main/system/images/icon-white.svg">
  <source media="(prefers-color-scheme: light)" srcset="./storage/main/system/images/icon-black.svg">
  <img align="left" width="60" height="60" style="margin-right: 15px;" src="./storage/main/system/images/icon-black.svg">
</picture>

# DracolaxOS

# DracolaxOS

A custom x86\_64 operating system built from scratch in C and Assembly. 💻🐉

## Overview 🛠️

**DracolaxOS** is a freestanding kernel targeting x86\_64 hardware, with a GUI compositor, window manager, desktop shell, Ring-3 userland, and a package/update system called **DRX**. It boots via GRUB and runs in QEMU or VirtualBox. 🚀

**Status:** Active development — Phase 1 (kernel stability + userland bootstrap)

## Features ✨

  * **x86\_64 freestanding kernel** (Ring 0–3, GDT/IDT/TSS, syscall/sysret)
  * **Physical and virtual memory manager** (PMM/VMM, paging)
  * **ATA PIO block driver**
  * **PS/2 keyboard and mouse** (including VMware mouse protocol)
  * **Framebuffer compositor** with alpha blending and double-buffering
  * **Window manager** with z-order, focus, and hit-testing
  * **VFS** with RAMFS and procfs
  * **Preemptive scheduler**
  * **LXScript** — embedded scripting language with kernel bindings
  * **Linux syscall compatibility shim** (ELF loader, partial POSIX)
  * **DracoShield firewall** 🛡️
  * **DRX update engine** (atomic swap, rollback, stable/beta channels)
  * **Wine integration** (planned as a first-class DRX component) 🍷

## Build 🏗️

```bash
# Install dependencies (Debian/Ubuntu)
make install-deps

# Build kernel ELF + ISO
make

# Run in QEMU
make run-qemu

# Attach GDB
make run-debug
# then: gdb kernel.elf -ex "target remote :1234"

# Run headless (serial only)
make run-headless

# Run in VirtualBox
make run-vbox

# Run tests
make tests

# Clean
make clean
```

**Requires:** `gcc` (or `x86_64-elf-gcc` for cross-compile), `nasm`, `grub-mkrescue`, `xorriso`, `qemu-system-x86_64`.

## Project Structure 📂

See [`docs/STRUCTURE.md`](https://www.google.com/search?q=docs/STRUCTURE.md) for the full canonical layout.

```text
kernel/      Ring-0 kernel (arch, mm, fs, sched, ipc, drivers, security)
gui/         Compositor, window manager, desktop shell
apps/        Ring-3 userland applications
drx/         Update and package engine
lxscript/    Embedded scripting language
services/    Background system services
libc/        Minimal freestanding libc
runtimes/    Wine integration layer
tools/       Host-side dev utilities (dxi-convert, lxs runner)
build/       Linker script, GRUB ISO config, CI
docs/        Internal documentation and specs
tests/       Host-side test suite
storage/     Runtime OS storage tree (not compiled)
```

## Roadmap 🗺️

| Phase | Focus |
| :--- | :--- |
| **0** | Project restructure (canonical layout) ✓ |
| **1** | Kernel stability, Ring-3 userland bootstrap |
| **2** | GUI polish, desktop, icon system (.dxi) |
| **3** | DRX update engine, package manager |
| **4** | Wine integration via DRX |
| **5** | LXScript userland APIs |
| **6** | Hardware testing and release |

## Documentation 📖

  * [`docs/STRUCTURE.md`](https://www.google.com/search?q=docs/STRUCTURE.md) — Source tree layout
  * [`docs/DRX_SPEC.md`](https://www.google.com/search?q=docs/DRX_SPEC.md) — Update engine protocol
  * [`docs/DXI_FORMAT.md`](https://www.google.com/search?q=docs/DXI_FORMAT.md) — Icon format spec
  * [`docs/WINE_INTEGRATION.md`](https://www.google.com/search?q=docs/WINE_INTEGRATION.md) — Wine DRX design
  * [`docs/architecture.md`](https://www.google.com/search?q=docs/architecture.md) — High-level design
  * [`docs/api_reference.md`](https://www.google.com/search?q=docs/api_reference.md) — Kernel API

## License 📜

**DracolaxOS** — Proprietary. All rights reserved. 🔒
