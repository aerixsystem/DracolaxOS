# Build & Run Guide

## Prerequisites

```bash
make install-deps
```

Installs: `gcc`, `nasm`, `grub-mkrescue`, `xorriso`, `mtools`, `qemu-system-x86_64`.

For cross-compilation (recommended — avoids host ABI conflicts):
```bash
sudo apt-get install gcc-x86-64-linux-gnu binutils-x86-64-linux-gnu
# or build x86_64-elf-gcc from source / use a prebuilt toolchain
```

If `x86_64-elf-gcc` is on `$PATH`, the Makefile selects it automatically.

---

## Build

```bash
# Full build: kernel ELF + GRUB ISO
make

# Kernel ELF only
make build

# ISO only (requires prior make build)
make iso
```

Output: `DracolaxOS_v1_x64.iso`

---

## Run in QEMU

```bash
make run-qemu
```

QEMU flags used:
- `-m 512M` — 512 MiB RAM
- `-vga std` — standard VGA (VESA framebuffer via GRUB)
- `-serial stdio` — kernel serial output to your terminal
- `-device usb-tablet` — absolute mouse input (no cursor drift)
- `-cpu qemu64,+lahf_lm` — x86_64 with LAHF/SAHF support

---

## Debug with GDB

```bash
make run-debug
```

QEMU starts paused and listens on port 1234. In another terminal:

```bash
gdb kernel.elf
(gdb) target remote :1234
(gdb) c
```

Useful GDB commands:
```
info registers          — dump all GP registers
x/10i $rip              — disassemble from RIP
break kernel_main       — breakpoint at kernel entry
watch g_ws              — watchpoint on workspace variable
```

---

## Run Headless (serial only, no display)

```bash
make run-headless
```

Uses `-nographic` — all output goes to the terminal via serial. Useful for CI or remote debugging.

---

## Run in VirtualBox

```bash
make run-vbox
```

Creates a VM named `DracolaxOS_v1` if it doesn't exist. Subsequent runs reuse it.

Manual setup if needed:
- Type: Other (64-bit)
- RAM: 512 MB
- VRAM: 16 MB
- Boot: DVD from `DracolaxOS_v1_x64.iso`
- Enable ACPI and I/O APIC

---

## Tests

```bash
make tests
```

Runs `tests/run_all_tests.sh` — host-side unit tests for PMM, VFS, IRQ, ELF loader, WM, etc.

---

## Clean

```bash
make clean
```

Removes all `.o` files, `kernel.elf`, `DracolaxOS_v1_x64.iso`, and `iso_build/`.

---

## Project Layout Quick Reference

```
Makefile                 — root build entry (v3.0, Phase 0 paths)
build/linker.ld          — kernel linker script
build/iso/               — GRUB config + splash themes
kernel/                  — Ring-0 kernel source
gui/                     — compositor, WM, desktop
apps/                    — Ring-3 application source
drx/                     — update engine
lxscript/                — scripting language runtime
services/                — background system services
tools/                   — host-side dev tools
docs/                    — this documentation
```

Full layout: see `docs/STRUCTURE.md`.
