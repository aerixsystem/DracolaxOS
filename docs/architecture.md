# DracolaxOS — System Architecture

**Kernel:** Draco-1.0, x86_64, freestanding C + NASM assembly  
**ABI:** Native Draco + Linux x86_64 (SYSCALL/SYSRET)  
**Boot:** GRUB2 → Multiboot2 → kernel ELF

---

## Layer Overview

```
┌─────────────────────────────────────────────────────────┐
│  User Applications (Ring 3)                             │
│  apps/: appman, filemanager, terminal, installer, …     │
├─────────────────────────────────────────────────────────┤
│  GUI Layer                                              │
│  gui/desktop/   — wallpaper, dock, workspaces, overlays │
│  gui/wm/        — window manager (z-order, focus, snap) │
│  gui/compositor/— alpha blend, z-sort, backbuf blit     │
├─────────────────────────────────────────────────────────┤
│  System Services (Ring 3 tasks)                         │
│  services/: service_manager, network, audio, power, … │
├─────────────────────────────────────────────────────────┤
│  LXScript Runtime                                       │
│  lxscript/: lexer → parser → codegen → VM              │
│  kernel/lxs_kernel.c — kernel binding layer             │
├─────────────────────────────────────────────────────────┤
│  Kernel Core (Ring 0)                                   │
│  arch/x86_64/   GDT, IDT, TSS, SYSCALL, Ring3, RTC     │
│  mm/            PMM, VMM, paging                        │
│  fs/            VFS, RAMFS, procfs                      │
│  sched/         Preemptive task scheduler               │
│  ipc/           Signals                                 │
│  drivers/       ata, ps2, serial, vga, audio, net, usb  │
│  security/      dracoauth, dracolock, draco-shield       │
│  linux/         Linux syscall compatibility shim         │
│  loader/        ELF binary loader                       │
├─────────────────────────────────────────────────────────┤
│  Hardware: x86_64 CPU, VESA framebuffer, PS/2, ATA PIO  │
└─────────────────────────────────────────────────────────┘
```

---

## Boot Sequence

```
BIOS/UEFI
  └─ GRUB (build/iso/boot/grub/grub.cfg)
       └─ loads kernel.elf (Multiboot2)
            └─ kernel/arch/x86_64/boot.s   — sets up stack, calls kernel_main()
                 └─ kernel/main.c           — hardware init sequence:
                      1. VGA text mode init (serial debug output)
                      2. GDT + TSS load
                      3. IDT + ISR stubs
                      4. PIC remap (IRQ 0-15 → INT 0x20-0x2F)
                      5. PMM + VMM + paging
                      6. RAMFS mount
                      7. Scheduler init (creates kernel idle task)
                      8. PS/2 keyboard + mouse init
                      9. ATA PIO driver init
                      10. LXScript kernel init
                      11. Security subsystem (dracoauth)
                      12. VESA framebuffer init
                      13. Compositor task spawn
                      14. Services spawn (service_manager)
                      15. Desktop task spawn
                      16. STI + scheduler start (first task switch)
```

---

## Memory Map (Runtime)

```
0x0000_0000 — 0x0000_FFFF   Real-mode IVT + BDA (reserved)
0x0001_0000 — 0x0009_FFFF   Bootloader scratch (reclaimed after boot)
0x0010_0000 — kernel end    Kernel ELF (code + rodata + data + bss)
kernel end  — PMM bitmap    Physical memory manager bitmap
PMM end     — heap start    Kernel heap (bump allocator)
heap start  — 0x3FFF_FFFF   Kernel heap (grows up)
0x4000_0000 — …             VESA framebuffer (mapped by GRUB)
0xFFFF_8000_0000_0000 — …   Higher-half kernel mapping (future)
```

---

## Scheduler

- **Algorithm:** Cooperative + timer-preemptive (PIT IRQ0 at ~100 Hz)
- **Task states:** Running, Ready, Sleeping, Blocked
- **Task struct:** `kernel/sched/task.h` — stores register context, stack pointer, sleep timer
- **Context switch:** `kernel/sched/sched.c` — saves/restores GP registers via inline asm
- **sleep:** `sched_sleep(ms)` — puts task in sleep queue, wakes on tick

---

## Memory Management

### Physical (PMM)
Bitmap allocator. One bit per 4 KiB page frame. `pmm_alloc_page()` returns a 4 KiB-aligned physical address.

### Virtual (VMM)
4-level page table (PML4 → PDPT → PD → PT). `vmm_map()` maps virtual → physical with flags. Used to map VESA framebuffer and per-task stacks.

### Heap
Simple bump allocator (`kzalloc` / `kmalloc`). No free list yet — deallocation is a no-op. Suitable for kernel init-time allocations. Per-task heaps are planned for Phase 2.

---

## Filesystem

```
VFS (vfs.c)
├── mount("/", ramfs)        — root = in-memory RAM filesystem
└── mount("/proc", procfs)   — process info (read-only)
```

RAMFS stores files as flat byte arrays in kernel heap. No persistence across reboots (DRX updates are applied to the storage layer, not RAMFS directly).

---

## GUI Architecture

The GUI has three distinct layers that cooperate:

### 1. Desktop task (gui/desktop/default-desktop/desktop.c)
Owns the main render loop (33 ms / ~30 fps). Draws wallpaper, dock, topbar, overlays (start menu, search, context menu, about) directly via `fb_*` calls to the shadow buffer. Calls `wm_render_frame()` after drawing to composite WM-managed windows on top.

### 2. Window Manager (gui/wm/wm.c)
Manages `wm_window_t` structs: position, size, z-order, desktop assignment, focus. `wm_render_frame()` sorts by z and blits each window's backbuf onto the shadow buffer. Supports window snap, task switcher, virtual desktops.

### 3. Compositor (gui/compositor/compositor.c)
Used by individual app tasks (via `comp_*` API). Provides per-window backbufs, z-sorting, desktop filtering. Apps call `comp_render()` from their own task loop.

**Render pipeline:**
```
Desktop draws wallpaper + dock + topbar + overlays (fb_* direct)
    → wm_render_frame()  (WM windows, z-sorted, desktop-filtered)
    → dbgcon_draw()      (debug console overlay if open)
    → fb_flip()          (shadow buffer → VRAM)
    → cursor_move()      (cursor stamped onto VRAM after flip)
```

---

## Input Routing

```
PS/2 IRQ1 (keyboard) / IRQ12 (mouse)
    → kernel/drivers/ps2/keyboard.c / mouse.c
    → input_router (kernel/drivers/ps2/input_router.c)
         ├── if desktop task is active → desktop.c reads via keyboard_getchar() / mouse_get_*()
         └── if app is focused        → input_router_set_focus(task_id) routes to that task
```

Mouse position is edge-detected: `mouse_update_edges()` is called once per frame before any `mouse_btn_pressed()` check, ensuring click handlers fire exactly once per physical press.

---

## Linux Compatibility

`kernel/linux/` provides a partial Linux x86_64 ABI:

- `linux_syscall_table.c` — maps Linux syscall numbers to Draco kernel functions
- `linux_syscalls.c` — implements read, write, open, close, mmap, exit, clone, futex, …
- `linux_fs.c / linux_process.c / linux_memory.c` — subsystem implementations
- `kernel/loader/elf_loader.c` — loads ELF64 binaries, sets up Ring-3 stack and jumps via SYSRET

This layer is the foundation for Wine (Phase 4). Native Draco apps use the Draco ABI directly.

---

## Security

| Component | Location | Function |
|-----------|----------|----------|
| dracoauth | kernel/security/dracoauth.c | User authentication, session management |
| dracolock | kernel/security/dracolock.c | Screen lock, idle timeout |
| dracolicence | kernel/security/dracolicence.c | Licence validation |
| DracoShield | kernel/security/draco-shield/ | Network firewall (packet filter) |

Ring-0 / Ring-3 separation enforced by GDT privilege levels and SYSCALL/SYSRET. User tasks cannot access kernel memory.
