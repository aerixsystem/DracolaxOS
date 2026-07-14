# Developer Notes & Gotchas

Running notes on non-obvious behaviour, past bugs, and things to keep in mind.

---

## Phase 0 — Restructure (completed)

- All source files moved to canonical layout. See `STRUCTURE.md`.
- Include paths were broken after the move. Fixed by running `restructure.sh` and then the include-fixer script (`fix_includes.py`). **112 files updated.**
- `build/linker.ld` is the linker script — was at `kernel/linker.ld` before Phase 0.
- GRUB config is now at `build/iso/boot/grub/grub.cfg`.
- The Makefile `-T` flag and all `-I` flags were updated for new paths (Makefile v3.0).

---

## Phase 1 — GUI Bug Fixes (completed)

### Bugs confirmed and fixed

**BUG: wm_render_frame() never called from desktop loop**
- `wm_render_frame()` was defined in `wm.c` but never invoked anywhere. WM-managed windows were never visible.
- Fix: added `wm_render_frame()` call in `desktop.c` render section, after `draw_search_overlay()`.

**BUG: wm_window_t had no z field — wm_render_frame rendered in creation order**
- All windows rendered in creation order regardless of focus. Focused window could be drawn behind others.
- Fix: added `z` field to `wm_window_t`. `wm_create_window()` initialises z to creation index. `wm_focus_window()` raises z to max+1. `wm_render_frame()` now bubble-sorts by z before drawing.

**BUG: wm_switch_desktop() never called on workspace change**
- `g_ws` in `desktop.c` changed correctly but the WM (`wm.c`) was never notified. All WM windows stayed assigned to desktop 0. Windows created on desktops 1–3 would not appear on those desktops.
- Fix: `wm_switch_desktop(g_ws)` + `comp_switch_desktop(g_ws)` now called at all three `g_ws` mutation sites (workspace click, Alt+Tab, Ctrl+1-4).

**BUG: compositor rendered all windows regardless of desktop**
- `comp_render()` iterated all `visible` windows with no desktop filter. Windows from desktop 0 appeared on all desktops.
- Fix: `window_t` gained a `desktop` field. `comp_create_window()` sets it to `current_desktop`. `comp_render()` now filters by `current_desktop`. Added `comp_switch_desktop()` / `comp_current_desktop()` API.

**BUG: g_cx / g_cy not explicitly clamped before click dispatch**
- Arrow key handling clamped correctly, but the main click path (`handle_desktop_click`) had no guard. Defensive clamp added immediately before both `mouse_btn_pressed()` checks.

### Confirmed NOT bugs (already correct)

- **Edge detection (1.1):** `mouse_update_edges()` is called once per frame before any `mouse_btn_pressed()` check — already edge-triggered. No fix needed.
- **Dock hitbox (1.4):** `draw_dock()` and `handle_desktop_click()` both compute `btn_x = (DOCK_W-BTN_SZ)/2` and `btn_y = TOPBAR_H+DOCK_PAD+i*(BTN_SZ+BTN_GAP)` — identical formulas.
- **Icon hitbox (1.5):** Start menu label rendered at `cy + bh - FONT_H - 3`, inside `bh`. Hit area matches render area.
- **NULL checks in appman (1.6):** `appman_launch()` does not dereference pointers that could be NULL. `appman_get()` returns NULL checked by all callers.
- **Linked list iteration (1.7):** Both WM and compositor use fixed-size arrays. No unsafe iteration.
- **Double buffering (1.9):** `fb_enable_shadow()` called at desktop init; `fb_flip()` called at end of every frame.

---

## Framebuffer / Shadow Buffer

- **Shadow buffer** (`fb_shadow_ptr()`) is the only render target during a frame. Direct `fb_*` calls write there.
- `fb_flip()` copies shadow → VRAM. Call it once per frame as the last step before `cursor_move()`.
- `cursor_move()` stamps the cursor pixel directly onto VRAM (after flip) so it's not blended into the shadow. This means the cursor is not in screenshots of the shadow buffer.
- `fb_console_lock(1)` must be called before the desktop task starts drawing, to prevent klog output from bleeding into the framebuffer.

---

## Mouse Input

- `mouse_update_edges()` must be called once per frame **before** `mouse_btn_pressed()` / `mouse_btn_released()`.
- `mouse_get_x()` / `mouse_get_y()` return positions clamped to `[0, fb.width-1]` / `[0, fb.height-1]`.
- `vmmouse_poll()` must also be called once per frame to service the VMware absolute mouse protocol. If running in QEMU with `-device usb-tablet`, vmmouse gives absolute coordinates (no drift).
- The raw mouse tracking variables `g_mouse_x_raw / g_mouse_y_raw` in `desktop.c` prevent the cursor position from being overwritten by mouse when arrow keys are being used.

---

## LXScript

- Kernel bindings in `kernel/lxs_kernel.c` — exposes fs, fb, sched, and log APIs to `.lxs` scripts.
- The VM (`lxscript/vm/vm.c`) is stack-based, single-threaded, and runs inside a kernel task.
- `.lxs` source files for examples live in `tools/lxs/`. Compile with `lxscript/tools/lxs_cli`.

---

## Workspace / Virtual Desktop

- `desktop.c` owns `g_ws` (current workspace index, 0-based).
- `wm.c` owns `current_desktop` — must be kept in sync via `wm_switch_desktop(g_ws)`.
- `compositor.c` owns its own `current_desktop` — kept in sync via `comp_switch_desktop(g_ws)`.
- All three must be updated together whenever workspace changes. The three mutation sites in `desktop.c` now call both after every `g_ws =` assignment.

---

## DRX / Package System

- DRX CLI is in `drx/cli/`. Not yet wired to a network stack (Phase 3).
- The `drx/draco-updates/` directory is the GitHub-hosted update index, committed as a git submodule.
- `drx/cli/draco-install.c` handles local `.dracopkg` install. HTTP download is Phase 3 (`drx/net/`).

---

## Known TODOs (not bugs)

- `kernel/drivers/vga/opengl.c` is a stub — Mesa / software GL not yet wired up.
- `libc/` is empty — Ring-3 libc not yet written. Apps currently use `kernel/klibc.c` directly (Ring-0 only; will break when apps fully migrate to Ring 3).
- `runtimes/wine/` is empty — Wine is a Phase 4 DRX package.
- `kernel/drivers/usb/usb_stub.c` is a stub — USB enumeration not implemented.
- `kernel/drivers/audio/audio_driver.c` is a stub — no PCM output yet.


---

## Post-Phase-1 Boot Fixes

### BUG: Heap exhausted on boot — RAMFS instances consume 8 MB each

**Symptom:** Serial log shows repeated `[ERROR] VMM: heap exhausted (need 8401024 bytes)` followed by `[ERROR] RAMFS: ramfs_new alloc failed` during `/storage/main/system` creation. Several storage sub-mounts silently fail.

**Root cause:** `rfile_t` embedded `uint8_t data[65536]` statically. Each RAMFS instance allocates `sizeof(rfs_t) = 128 files × (65536 + ~64) ≈ 8.4 MB` in one shot via `kzalloc`. Four instances (root ramfs, storage, storage-main, and further sub-mounts) consumed > 33 MB before the 3 MB shadow framebuffer could be allocated from the 32 MB heap.

**Fix (`kernel/fs/ramfs.c`):**
- `rfile_t.data` changed from `uint8_t data[RAMFS_MAX_SIZE]` → `uint8_t *data` (pointer, initially NULL).
- `rfs_write()` calls `kzalloc(RAMFS_MAX_SIZE)` on first write, returns -1 if allocation fails.
- `rfs_read()` guards `if (!f->data)` and returns 0 bytes (empty file).
- `ramfs_delete()` calls `kfree(f->data)` before `memset`.
- Empty RAMFS instance now costs `128 × sizeof(vfs_node_t + pointer + size + used) ≈ 4 KB` instead of 8.4 MB.

### BUG: Dock/app buttons did nothing when clicked

**Symptom:** Clicking Start, Search, or any dock button produced no visible response. `appman_launch()` succeeded (task spawned), but the app window never appeared.

**Root cause (two parts):**

1. `comp_render()` in `compositor.c` was drawing a full background (`fb_fill_rect` covering all of `TOPBAR_H` to bottom) plus `render_dock()` and `render_topbar()` on every call. When apps called `comp_render()` from their task, it wiped the desktop's shadow buffer, replacing the desktop rendering with a plain dark rect + a duplicate compositor dock. The desktop's own `draw_dock()` was then overwritten on the next desktop frame, and the two docks flickered destructively.

2. The desktop loop had no `comp_render()` call at all — app windows drawn into compositor backbufs were never composited into the shadow buffer.

**Fix:**
- `comp_render()` (`compositor.c`): removed `fb_fill_rect` background wipe, `render_dock()`, and `render_topbar()`. Now renders **only** compositor-managed window backbufs, z-sorted and desktop-filtered.
- `desktop.c` render loop: added `comp_render()` call after `wm_render_frame()`, before `fb_flip()`. The desktop owns the background, dock, and topbar; comp_render pastes app windows on top.
- `apps/appman/apps.c`: removed all 4 `comp_render()` calls from app functions. Apps only update their backbufs via `comp_window_fill()` / `comp_window_print()`; the desktop loop does the actual compositing.

**Correct render pipeline (desktop loop, once per frame):**
```
draw_wallpaper()        → shadow buf: wallpaper
draw_dock()             → shadow buf: left dock
draw_topbar()           → shadow buf: top bar
draw_ctx_menu()         → shadow buf: right-click menu (if open)
draw_about()            → shadow buf: about overlay (if open)
draw_start_menu()       → shadow buf: start menu (if open)
draw_search_overlay()   → shadow buf: search (if open)
wm_render_frame()       → shadow buf: WM-managed windows (z-sorted)
comp_render()           → shadow buf: compositor app windows (z-sorted)
dbgcon_draw()           → shadow buf: debug console (if open)
fb_flip()               → VRAM ← shadow buf
cursor_move()           → VRAM: cursor stamped after flip
```

### WARNING: -Waddress-of-packed-member in ring3.c

**Symptom:** Build warning: `taking address of packed member of 'struct <anonymous>' may result in an unaligned pointer value` at `ring3.c:212`.

**Root cause:** `tss64_t` is declared `__attribute__((packed))`. `rsp0` sits at byte offset 4 (after `uint32_t reserved0`), so it is not 8-byte aligned. Taking `&_t->rsp0` directly gives a `uint64_t *` that GCC cannot guarantee is aligned — GCC warns even though x86 handles unaligned accesses in hardware.

**Fix (`kernel/arch/x86_64/ring3.c`):**
- Added `#define offsetof(type, member) __builtin_offsetof(type, member)` (no `<stddef.h>` available under `-nostdinc`).
- Replaced `(uint64_t *)(void *)&_t->rsp0` with `(uint64_t *)(void *)((char *)_t + offsetof(tss64_t, rsp0))`.
- `char *` arithmetic is allowed to alias any object; the pointer value is identical but derived without taking the address of a packed member. Warning is gone, no behaviour change on x86.


---

## Phase 2 — .dxi Icon System (completed)

### New files

| File | Purpose |
|------|---------|
| `kernel/dxi/dxi.h` | Format spec struct, `dxi_icon_t`, `dxi_load()` prototype, constants |
| `kernel/dxi/dxi.c` | VFS-backed loader, header validation, lazy pixel allocation |
| `tools/dxi-convert/dxi_convert.c` | Host-side C converter: PNG/JPG → .dxi via stb_image |
| `tools/dxi-convert/Makefile` | Builds `dxi_convert`, auto-downloads stb_image.h |

### Changed files

| File | Change |
|------|--------|
| `kernel/init.c` | Added `/storage/main/system/shared/images` RAMFS mount (icon store) |
| `gui/compositor/compositor.h` | Added `blit_icon_bgra()` prototype |
| `gui/compositor/compositor.c` | Implemented `blit_icon_bgra()` — per-pixel BGRA alpha blend, integer math, A=255/0 fast paths |
| `gui/desktop/default-desktop/desktop.c` | Added `dxi.h` include, icon cache state (`g_icons[]`, `g_icon_pixels[]`), `icons_load_all()`, `app_name_to_dxi()`, `blit_icon_bgra()` call in start menu renderer |
| `Makefile` | Added `-Ikernel/dxi` include path, `kernel/dxi/dxi.c` to `KERNEL_CORE` sources |
| `docs/DXI_FORMAT.md` | Fully rewritten to match implemented spec |

### Design decisions

**Fixed pixel storage, not per-icon kmalloc:**  
`g_icon_pixels[APP_MAX][48×48]` is a static array in `.bss`. Total: 16 × 9216 = 144 KB. This avoids fragmentation from 16 small separate `kmalloc` calls, and the desktop loop never touches the allocator per frame.

**Caller-supplied buffer:**  
`dxi_load()` checks `icon->pixels` before allocating. The desktop pre-fills `g_icons[i].pixels = g_icon_pixels[i]` before calling `dxi_load()`, so the loader writes directly into the static array. `dxi_free()` will not free a caller-supplied buffer.

**Graceful fallback:**  
If a `.dxi` file is missing (`dxi_load()` returns -1), `g_icons[i].loaded` stays 0. `draw_start_menu()` falls back to two-character text initials — the start menu remains fully functional without any icons installed.

**Icon naming:**  
App name → filename via `app_name_to_dxi()`: lowercase, spaces become hyphens, `.dxi` appended. Deterministic and requires no manifest/registry.

**`blit_icon_bgra()` does not call `fb_put_pixel()`:**  
It writes directly into `fb_shadow_ptr()` via pointer arithmetic — one array write per opaque pixel instead of a function call with bounds checks per pixel. Clipping is done once per row at the top of the function.

**`comp_render()` no longer draws background/dock/topbar:**  
This was the root cause of the dock-button bug fixed in the previous session. `comp_render()` now renders only compositor window backbufs; `blit_icon_bgra()` is called from the desktop loop (inside `draw_start_menu()`) which already owns the shadow buffer.

### How to add icons

```bash
cd tools/dxi-convert
make                                          # build converter + download stb
./dxi_convert my-icon.png terminal.dxi 48 48  # convert at dock size
cp terminal.dxi ../../storage/main/system/images/icons/
```

Rebuild and boot — `icons_load_all()` picks them up automatically.


---

## Phase X — Kernel Hardening (completed)

### Build warnings fixed

**WARNING: `render_dock` and `render_topbar` defined but not used (`compositor.c`)**
Both functions were dead code left over from the Phase 1 comp_render cleanup. Removed entirely. The desktop task owns dock and topbar rendering; the compositor only touches window backbufs.

### Hardening audit — what already existed

The kernel had more hardening than the checklist assumed:
- `kpanic()` fully implemented with VGA red screen, RIP/RSP dump, RAM stats, serial output, and framebuffer fallback — nothing to add.
- Ring-buffer `klog` with two channels (kernel + system), async flush task, file rotation — nothing to add.
- `kmalloc`/`kfree` already interrupt-safe via `pushfq/cli/popfq` around free-list operations.
- ATA PIO driver already has bounded timeout loops (`for i < 1000000`) returning error codes — not infinite waits.
- IRQ handlers already isolated from GUI code — no GUI calls inside interrupt context anywhere in the codebase.

### New: `kernel/uaccess.h` — user/kernel boundary

Every syscall that accepts a user pointer must validate it before touching. Previously `SYS_WRITE` and `SYS_READ` dereferenced `buf` directly — a malicious process could pass a kernel address and read/corrupt kernel memory.

**`access_ok(ptr, n)`** — validates a pointer range against `[USER_MEM_START=0x1000, USER_MEM_END=0x7FFFFFFFFFFF]`. Rejects NULL, kernel addresses, and ranges that wrap around.

**`copy_from_user(kdst, usrc, n)`** / **`copy_to_user(udst, ksrc, n)`** — safe copies with `access_ok` check before touching user memory.

**`strncpy_from_user(kdst, usrc, maxlen)`** — NUL-safe string copy from user space, byte-at-a-time with upper-bound check.

**`KASSERT(expr, msg)`** — triggers `kpanic()` with file:line in `DRACO_DEBUG` builds; compiles to nothing in release. Usage: `KASSERT(ptr != NULL, "sched_spawn: null entry");`

**`SYSCALL_VALIDATE_PTR(ptr, len, frame)`** — one-liner macro for syscall handlers: calls `access_ok`, logs a warning, sets `frame->rax = -EFAULT`, and returns if invalid.

`syscall.c` updated: `SYS_WRITE` and `SYS_READ` now call `access_ok` before touching user buffers, and copy through a kernel stack buffer to prevent TOCTOU between validation and use.

### New: heap poisoning + double-free detection (`vmm.c`)

**`free` flag replaced with `magic` field:**
- `ALLOC_MAGIC = 0xDEADC0DE` — set when block is allocated
- `FREE_MAGIC  = 0xFEEEFEEE` — set when block is freed

**Double-free detection in `kfree()`:** if `h->magic == FREE_MAGIC`, the block was already freed. Logs `DOUBLE FREE detected` and returns without touching the free list (prevents list corruption).

**Bad pointer detection:** if `h->magic` is neither `ALLOC_MAGIC` nor `FREE_MAGIC`, logs `bad magic — heap corrupt?` and returns.

**Payload poisoning:** on free, `memset(ptr, 0xCC, payload_sz)` fills the block payload before returning it to the free list. Any code that reads from a freed pointer will get `0xCC` bytes, making use-after-free bugs immediately visible in the debugger.

### New: `mem_check()` heap walker

Walks every block in the heap from `heap_start` to `heap_end`, verifying `magic` is either `ALLOC_MAGIC` or `FREE_MAGIC`. Reports corrupt blocks via `kerror`. Returns count of corrupt blocks (0 = healthy).

Available in the kernel shell: `memcheck`
Available in code: `#include "mm/vmm.h"` then call `mem_check()`.

### New: VFS path sanitisation (`vfs.c`, `vfs.h`)

`vfs_path_sanitize(src, dst, dstsz)` normalises a path before it enters the mount resolver:
1. Must start with `/`
2. Collapses consecutive slashes
3. Strips `.` components silently
4. **REJECTS** `..` components — returns -1 and logs a warning

`vfs_open()` now calls `vfs_path_sanitize()` first. Any `..` in a path from user space causes `vfs_open()` to return NULL rather than walking above the mount root. The old behaviour (`..` silently reset to the start node) was not a full fix — it allowed `/../../../etc/` to partially resolve depending on VFS structure.

### New: `DRACO_DEBUG` build mode (`Makefile` v3.1)

```bash
make           # RELEASE: -O2, KASSERT = no-op
make DEBUG=1   # DEBUG:   -O0 -g -DDRACO_DEBUG, KASSERT triggers kpanic
```

`DRACO_DEBUG` enables `KASSERT` expansion. The release build has zero overhead from assertions — they compile away completely via the preprocessor.


---

## Stability Phase — Kernel Stabilisation (completed)

### Audit: what was already fully implemented

The following checklist items required **no new code** — they were already correct:

| Item | Where |
|------|-------|
| `kpanic()` — RIP/RSP/RAM dump, VGA red screen, serial output, halt | `kernel/log.c` |
| Ring-buffer logger — 256-entry async, two channels (kernel/system), file rotation | `kernel/klog.c` |
| No GUI code in interrupt context — IRQ handlers never call fb/wm/desktop | `kernel/arch/x86_64/irq.c` |
| Watchdog task monitoring IRQ1/IRQ12, re-asserting stalled ports | `kernel/init.c::irq_watchdog_task` |
| VFS `..` traversal blocking — `vfs_path_sanitize()` rejects any `..` component | `kernel/fs/vfs.c` |
| ATA timeout loops — bounded `for i < 1000000` returning error codes, not infinite waits | `kernel/drivers/ata/ata_pio.c` |
| `copy_from_user` / `copy_to_user` / `access_ok` / `KASSERT` | `kernel/uaccess.h` |
| Header magic (`ALLOC_MAGIC`/`FREE_MAGIC`), double-free detection, payload poison (`0xCC`) | `kernel/mm/vmm.c` |
| Tail canary (`TAIL_MAGIC = 0xCAFEBABE`) written by `kmalloc` | `kernel/mm/vmm.c` |

### Fixed: tail canary was overflowing its block

**Root cause:** `kmalloc` computed `need = ALIGN16(size) + HDR_SIZE`. The tail canary was written at `h + h->size - 4`, which is the last 4 bytes of the block — but with the old formula those 4 bytes overlapped the *start* of the next block's header, silently overwriting it.

**Fix:** `need = ALIGN16(size + CANARY_SZ) + HDR_SIZE`. The canary now lives entirely within the block's own allocation, and `ALIGN16` ensures the next block header remains 16-byte aligned.

### Fixed: `kfree` did not verify the tail canary

**Fix:** `kfree` now calls `tail_ok(h)` before poisoning the payload. A corrupt canary at free time means a buffer overflow has already occurred — logged as `VMM: kfree: OVERFLOW detected`. The block is still freed (better a corrupt free than a permanent leak).

### Fixed: `mem_check()` / `heap_check_all()` did not check tail canaries

`mem_check()` previously only validated header magic. Now it also calls `tail_ok()` on every allocated block, catching overflows that happened between `kmalloc` and `mem_check`. Reports the address and block size of each corrupt block.

`heap_check_all()` added as the canonical name matching the stability spec — it is a direct call to `mem_check()`.

### Fixed: allocation tracking counters incomplete

`g_total_frees` was never incremented. `g_current_used_bytes` was never decremented on free. Both are now updated in `kfree`. Four new accessor functions added to `vmm.h`: `vmm_alloc_count()`, `vmm_total_allocs()`, `vmm_total_frees()`, `vmm_peak_bytes()`. `mem_check()` now prints all four in its OK summary line.

### Fixed: `panic()` alias missing

`log.h` now defines `#define panic(msg) kpanic(msg)`. Both names invoke the same implementation. Code written to either style compiles without changes.

### Fixed: Linux syscall layer dereferenced user pointers directly

`linux_syscalls.c` was included in the uaccess-hardening of the Draco ABI layer but the Linux compat `lx_sys_read` and `lx_sys_write` still dereferenced `buf` directly:

```c
buf[i] = c;   // buf is a user pointer — never do this in Ring 0
```

**Fix:** `uaccess.h` included in `linux_syscalls.c`. Both `lx_sys_read` and `lx_sys_write` now:
1. Call `access_ok(buf, len)` before touching user memory — reject with `-EFAULT` if invalid
2. Copy through kernel stack or `kmalloc` buffers using `copy_from_user`/`copy_to_user`
3. Never dereference user pointers from Ring 0

### `vmm.h` rewritten

The header had a corrupted include guard from a previous session (two `#endif` markers). Fully rewritten with a clean guard, complete API documentation, and all new symbols properly declared.

---

## GUI Stability & VFS Consistency Phase (completed)

Two work sessions. First pass fixed compositor memory safety, boot-time watchdog
starvation, IRQ12 log spam, and shell console performance. Second pass (after
live VirtualBox testing surfaced real symptoms) fixed a `ls`/`cd` path
inconsistency in the flat RAMFS-backed `/storage` mounts, and a GUI keyboard
routing bug where typed input was silently dropped during first-boot install.

### Fixed: compositor backbuffer not reallocated on resize/maximize

**Root cause:** `comp_set_geometry()` and `comp_toggle_maximize()` updated
`win->w`/`win->h` directly without resizing `win->backbuf`, which stays
allocated at its original `w*h*4` size. Any subsequent
`comp_window_print`/`comp_window_fill`/`shadow_blit` then indexes the buffer
using the *new* dimensions — heap buffer overflow on grow, stale/shifted
content on shrink.

**Fix:** new `comp_resize_backbuf()` in `gui/compositor/compositor.c`
`krealloc`s to the new size and refills with the focus-appropriate body
colour. Called from both `comp_set_geometry()` (skipped for pure moves where
w/h are unchanged) and `comp_toggle_maximize()` (both the maximize and
restore paths). On `krealloc` failure the old buffer/dimensions are kept
(logged via `kwarn`) rather than risking OOB access.

### Fixed: watchdog killing `init_task`/`desktop_task` on boot

**Root cause:** `pio_read_sector()` in `apps/installer/installer.c` (used by
`detect_other_os()` during first-boot detection) has two busy-wait polling
loops (100k / 300k iterations) with interrupts enabled but no
`sched_yield()`. On slow/virtualized IDE I/O these loops can run long enough
that the calling task's heartbeat goes stale for multiple
`sched_watchdog_check()` cycles (~5s each), accumulating
`WATCHDOG_STRIKE_MAX` strikes and getting killed mid-boot.

**Fix:** both loops now call `sched_yield()` every 4096 spins, keeping the
heartbeat fresh without materially slowing the (already best-effort,
fail-safe) OS detection.

### Fixed: IRQ12 "both IRQs quiet" recovery spam

**Root cause:** `irq_watchdog_task()`'s Case C (both keyboard and mouse IRQs
quiet — meant to recover from a VM losing/regaining focus) fired
`keyboard_reinit()` + a full PS/2 drain/reassert on *every* transition into a
5-second idle period, i.e. every time the user simply stopped typing/moving
the mouse. This spammed the log and briefly disturbed input state during
completely normal use.

**Fix:** recovery now requires (1) the machine has seen real PS/2 activity
before (`had_recent_activity`), (2) a sustained ~30s quiet streak
(`CASE_C_QUIET_THRESHOLD = 6` rounds), and (3) hasn't already recovered
during this particular quiet streak (`case_c_done_this_quiet` latch, reset
only by the next burst of activity).

### Fixed: `shell_print`/`shell_putchar` full-framebuffer flip per call

**Root cause:** `fb_console_print()` does a full shadow→VRAM `memcpy`
(`fb_flip()`) after every call unless batch mode is active. `cmd_cat`
called `shell_print()` once per 512-byte file chunk, and `cmd_ls` once per
directory entry — so `cat`-ing a large file or `ls`-ing a large directory
triggered hundreds of full-screen memcpys for what should be one screen
update.

**Fix:** both commands now wrap their output loop in
`fb_console_begin_batch()` / `fb_console_end_batch()` (the same pattern
already used by `readline()`'s `REDRAW()` macro), doing exactly one flip
regardless of output size. `cmd_cat` also added a `sched_yield()` per chunk
so very large files can't starve the watchdog (same class of issue as the
installer fix above).

### Fixed: `ls` and `cd` inconsistent with the flat RAMFS storage model

**Root cause:** `/storage/main`, `/storage/main/users`, etc. are separate
RAMFS volumes mounted as sub-paths, but RAMFS itself has no real nesting —
`rmkdir()` only sets `type = VFS_TYPE_DIR` on a flat leaf node whose `ops`
still has `finddir = NULL`/`readdir = NULL`. So a path like
`/storage/main/users/guest/documents` never resolves via `vfs_open()`;
`cmd_create`/`cmd_cat`/`cmd_rm`/`cmd_cp`/`cmd_mv`/`echo >` already handled
this by falling back to a **flat key** (e.g.
`"main/users/guest/documents/test.txt"`) stored directly on `storage_root`.

`cmd_cd`, however, only checked the path *prefix* (`/storage`, `/ramfs`,
`/proc`) and accepted **any** subpath unconditionally — `cwd` became a
fictional string even when nothing existed there. `cmd_ls` then failed to
`vfs_open()` that fictional path and fell back to dumping **all** of
`storage_root`'s flat entries — including other full-path-named files
created elsewhere — which is exactly the "ls shows the wrong/whole listing"
symptom reported from VirtualBox testing.

**Fix** (`kernel/shell.c`):
- Added `flat_dir_exists(root, rel)` — true if `rel` is the mount root
  itself, an exact flat entry of type `VFS_TYPE_DIR`, or any flat entry
  whose name starts with `"rel/"` (i.e. a non-empty virtual directory).
- Added `flat_dir_list(root, rel)` — lists immediate children of the
  virtual directory `rel` by scanning flat entries for the `"rel/"` prefix,
  stripping it, and collapsing deeper entries to just their first path
  component (deduplicated) so subdirectories show once with a trailing `/`.
- `cmd_cd` now calls `vfs_open()` first; if that fails, it requires
  `flat_dir_exists()` on the resolved path before changing `cwd`. A path
  that doesn't actually exist (in either the real VFS or the flat
  namespace) now correctly prints `cd: no such directory` instead of
  silently "succeeding".
- `cmd_ls` now calls `flat_dir_list()` as its fallback instead of dumping
  `storage_root`/`ramfs_root` wholesale, so listings inside a virtual
  directory show only that directory's own (relative-named) children.
- `mount_storage()` (`kernel/init.c`) now pre-creates each user's home
  subdirectories (`documents`, `downloads`, `desktop`, `configs`,
  `pictures`, `music`, `videos`, `trash`, `temp`) as flat `VFS_TYPE_DIR`
  marker entries (e.g. `"main/users/guest/documents"`), so `cd documents`
  works out of the box for both `root` and `guest` without needing a real
  hierarchical filesystem.

Note this is a flat-namespace workaround consistent with the existing
`cmd_create`/`cmd_cat` convention, not a real hierarchical filesystem.
`docs/STORAGE-SYSTEM.md` describes the long-term ID-based design; a real
ext2/fat32-style driver (tracked separately) is what will eventually replace
this.

### Fixed: GUI keyboard input dropped during first-boot install ("only shortcuts worked")

**Root cause:** `desktop_task()` called `installer_run()` (which reads
typed username/password input via `keyboard_getchar()` →
`input_router_getchar(sched_current_id())`) **before** registering
`desktop_tid` and calling `input_router_set_focus(desktop_tid)`. Until that
registration happened, the input router's `g_focused` still pointed at
whatever task owned focus by default — not `desktop_task` — so the keyboard
IRQ pushed every typed character into the wrong task's queue. Mouse clicks
and Super+letter shortcuts (`input_router_push_to(desktop_tid, ...)`, which
bypasses `g_focused` entirely) kept working, producing the exact reported
symptom: "only shortcuts work, typing doesn't."

**Fix:** moved the `desktop_tid = sched_current_id(); input_router_set_desktop_task(...);
input_router_set_focus(desktop_tid);` block to run immediately after
`desktop_icons_init()`, before the `installer_run()` call. `g_focused` is
now correctly set before any keyboard-driven UI (installer, login screen)
can run.

### Fixed: keyboard permanently dead after closing ANY window (real root cause)

Reported via live testing after the fix above: the previous fix solved
keyboard input during first-boot install, but the user found a more
general version of the same class of bug — open any app window, close it
(typically by pressing Esc, since the title bar literally says
"Esc=close"), and the keyboard goes completely dead afterward. Mouse
clicks and Super+letter shortcuts kept working — only normal typed input
stopped.

**Root cause:** `input_router_task_exit(task_id)`
(`kernel/drivers/ps2/input_router.c`) is called from both `sched_exit()`
and `sched_kill()` whenever a task terminates. It contained:

```c
if (g_focused == task_id)
    g_focused = 0;
```

This assumed task slot 0 was "the desktop/shell" (see the old comment),
but slot 0 is whatever task happens to occupy it — in practice
`init_task`, which never reads from its input queue. The common way to
close an app is pressing Esc while the window has input focus: the app's
own loop reads that Esc from *its own* queue (since `g_focused` already
equals the app's task id), calls `comp_destroy_window()` +
`sched_exit()`, and `input_router_task_exit()` then resets `g_focused` to
`0` — because the condition `g_focused == task_id` is true (nobody else
had reassigned focus first; `close_window()` in `desktop.c`, which *does*
correctly reset focus to `desktop_tid`, is only invoked for the
explicit-click-on-[X] path, not this self-initiated-exit path).

Once `g_focused == 0`, every subsequent keystroke is routed by
`input_router_push()` (called from the keyboard IRQ) into
`g_queues[0]` — a queue nobody ever drains. Mouse input doesn't go through
`g_focused` at all, and Super+letter shortcuts are pushed directly to
`g_desktop_task` via `input_router_push_to()`, bypassing `g_focused`
entirely — which is exactly why those two kept working while ordinary
typing went silently dead.

**Fix:** `input_router_task_exit()` now reassigns focus to
`g_desktop_task` (the tracked desktop task id, set via
`input_router_set_desktop_task()`) instead of the literal `0`:

```c
void input_router_task_exit(int task_id) {
    input_router_flush(task_id);
    if (g_focused == task_id)
        g_focused = g_desktop_task;
}
```

This also makes the explicit `close_window()` path's existing
`input_router_set_focus(desktop_tid)` call fully redundant-but-harmless
(the `g_focused == task_id` check is simply false by the time the app's
`sched_exit()` runs, since focus was already moved away) — both the
click-to-close and Esc-to-close paths now converge on the same correct
behavior. Also fixes the equivalent case for `sched_kill()` (e.g. the
watchdog force-killing a frozen, currently-focused app).

---

## Immediate-Mode Widget Toolkit Phase (completed)

New module: `gui/widgets/` (`widgets.h` + `widgets.c`), built on top of the
existing per-window compositor primitives. Dear-ImGui-style API — no
widget tree, no retained layout state. Each widget function both draws
itself for the current frame and returns whatever interaction happened
this frame; persistent hover/press/focus tracking lives in a
`widget_ctx_t` the calling app declares once (`static`) and refreshes
every frame via `widget_begin_frame(&ctx, win)`.

Widgets implemented: `widget_label`, `widget_button`, `widget_checkbox`,
`widget_slider` (horizontal, click-to-jump + drag), `widget_progress_bar`,
`widget_textbox` (single-line, caret, horizontal scroll, Left/Right/Home/
End/Backspace/Delete, Enter = submit, Esc = defocus), `widget_scrollbar_v`,
`widget_dropdown` (click to open/close, click a row to select, click
outside to close), `widget_color_picker` (16-swatch palette grid), and
shape primitives `widget_rect`/`widget_rect_border`/`widget_line`
(Bresenham)/`widget_circle` (midpoint algorithm, filled or outline).

Two new compositor primitives were needed to support shapes:
`comp_window_set_pixel()`/`comp_window_get_pixel()` in
`gui/compositor/compositor.c` — bounds-checked single-pixel read/write
into a window's backbuffer, since the existing `comp_window_fill()` only
does solid rects and `comp_window_print()` only draws font glyphs.

### Id and state model

Every interactive widget takes a caller-supplied `uint32_t id` (see
`WID(__LINE__)` convenience macro in `widgets.h`), used only to track
which widget is hovered/pressed/focused — ids need only be unique within
one window's `widget_ctx_t`. Three persistent ids live in the context:
`hot_id` (mouse-over, cosmetic), `active_id` (mouse button currently held
on this widget — used for the click-on-release rule and for
drag-continues-outside-track behaviour on sliders/scrollbars), and
`focused_id` (which textbox owns keyboard input). Textboxes and dropdowns
additionally need a little state of their own beyond what fits in the
shared context (caret/scroll position, open/closed) — that lives in small
per-instance structs (`widget_textbox_state_t`, `widget_dropdown_state_t`)
the caller declares `static` and passes by pointer, mirroring how the
data being edited (the buffer, the `int*`, the `float*`) is also always
caller-owned.

One documented subtlety: `widget_textbox()` defocuses itself on Esc
internally, so an app's own "Esc = close window" handling must snapshot
`ctx.focused_id != 0` *before* calling any `widget_*` functions that
frame — otherwise a single Esc both defocuses the textbox and closes the
window, since by the time the app checks `focused_id` after the widget
calls ran, it's already back to 0.

### Demo app

`app_widget_demo()` was added to `kernel/appman/apps.c` (registered as
"Widget Demo" in `appman_init()`, category "Accessories" — it shows up in
the app search/launcher automatically since those iterate the app
registry generically) and exercises every widget: a click counter
button, two checkboxes, a slider driving a live progress bar, a textbox,
a 5-item dropdown, a scrollable fake list with a working vertical
scrollbar, the colour picker, and all four shape primitives tinted with
the picked colour. It also doubles as the worked example for the
Esc-vs-textbox-focus interaction noted above. Doubles as living
documentation — read it alongside `widgets.h`'s header comment for the
full usage pattern.

---

## GUI Finalization Pass (completed)

Live testing of the widget toolkit in VirtualBox surfaced four real bugs
in one session, plus a structural cleanup. Concise summaries in
`docs/CHANGELOG.md`; a few worth the extra technical detail here.

**`ls` regression after the first storage fix.** The previous session's
`flat_dir_exists()`/`flat_dir_list()` fix only activated as a fallback
when `vfs_open(path)` failed. It turns out `vfs_open()` SUCCEEDS for
paths like `/storage/main/users/guest`, because `/storage/main` and
`/storage/main/users` are real, separate RAMFS sub-mounts — `vfs_open()`
correctly walks into them and returns a real node. The catch: that node
is permanently empty, because `rmkdir()` (in `kernel/init.c`) only sets
`type = VFS_TYPE_DIR` on a flat leaf — it never gives the node working
`readdir`/`finddir` ops or real children. Every actual file/directory
under `/storage` lives as a flat key on `storage_root` instead (the same
convention `cmd_create`/`cmd_cat` already used). So trusting a successful
`vfs_open()` for *listing* purposes was listing the wrong, always-empty
node. `cmd_ls` now checks `root_for_path()` first and, for anything under
`/storage`/`/ramfs`, always uses the flat-prefix listing regardless of
whether `vfs_open()` also separately succeeds. Real hierarchical mounts
(`/proc`) are unaffected — they still go through the original
`vfs_open()` + readdir/finddir path.

**Whole-desktop flicker/lag with Widget Demo open, but not other apps.**
Grepping for `comp_render()`/`fb_flip()` call sites outside `desktop.c`
turned up exactly one: the demo app itself, which copied a
"comp_render(); fb_flip(); sched_yield();" pattern that doesn't actually
belong in any app — the desktop task is the sole compositor, calling
those once per ITS OWN frame after compositing every window's
backbuffer, the wallpaper, icons, dock, and cursor. With the app also
calling them, two uncoordinated tasks raced to composite and flip the
shared framebuffer at different frequencies: visible tearing across the
WHOLE desktop (not just the app's window, since `comp_render()`
recomposites everything every time it's called from either task), and
roughly double the per-frame compositing cost. Background wallpaper
looked unaffected because it's static — even torn between two competing
composite passes, a static region looks the same in both. Fixed by
deleting those two calls from the app; `widgets.h` gained an explicit
section warning against this for future apps.

**Dropdown list on the wrong z-layer.** `widget_dropdown()` drew its open
option list immediately, synchronously, at its own call site. Any widget
drawn LATER in the same frame whose screen rect happened to overlap (in
the demo: the colour picker, positioned to partially overlap the open
fruit dropdown) painted over part of the list, since there's no z-index —
draw order is purely call order. Fixed with a small deferred-overlay
queue in `widget_ctx_t` (`pending[]`/`pending_count`,
`WIDGET_MAX_PENDING_OVERLAYS = 4`): `widget_dropdown()` now only queues
the open list's data (position, options, selection) instead of drawing
it, and a new `widget_end_frame(ctx)` — which every app must call once,
after all other `widget_*` calls — flushes the queue, drawing every
pending overlay last so it's always on top regardless of layout.
Click-to-select logic is unaffected since it uses the frame's already-
snapshotted mouse position, not drawing order.

**Widget hit-test offset ("must hover above the widget to interact").**
`widget_begin_frame()` converted the absolute mouse position to window-
relative coordinates via `comp_get_pos()`. That function returns the
window's OUTER top-left — i.e. the top of the title bar — while
`comp_window_fill`/`print`/`set_pixel` (and therefore every widget's
drawn position) operate in CONTENT-space, whose screen origin is exactly
`WIN_TITLE_H` (28px) further down (confirmed by reading
`render_window()`: it blits `win->backbuf` to screen position
`(win->x, win->y + WIN_TITLE_H)`). The mouse y came out too large by
that title-bar height, so landing a click on a widget drawn at
content-y `Y` required the cursor to actually be at screen-y `win.y + Y`
— title-bar-height pixels ABOVE where the widget visually appeared.
Added `comp_get_content_pos()` to `gui/compositor/compositor.c`/`.h`
(returns `win->x, win->y + WIN_TITLE_H`) and switched
`widget_begin_frame()` to use it instead of `comp_get_pos()` (which is
kept, unchanged, for callers that genuinely want the outer/title-bar-
included position, like window-dragging code).

**Two more instances of the focus-leak-to-task-0 bug.** While re-reading
the input-routing code to make sure the keyboard-death root cause from
the previous session was fully fixed, the same hardcoded
`input_router_set_focus(0)` pattern turned up three more times in
`gui/desktop/default-desktop/ctx_menu.c` — right-click → Close,
right-click title bar → Minimize, right-click title bar → Close — none
of which went through `desktop.c`'s `close_window()` (which already used
`desktop_tid` correctly) or `input_router_task_exit()` (already fixed
last session). All three now use `g_desktop_task` (already `extern`-
declared in `input_router.h`, no new plumbing needed).

**App removal and apps/ restructuring.** Per explicit request: every
built-in app except Widget Demo (Terminal, Text Editor, File Manager,
System Monitor, Calculator, Settings, Package Manager, Draco Shield,
Draco Manager, Login Manager, Paint, Image Viewer, Media Player) was
unregistered from `appman_init()`. Their implementations — previously
crammed together in one large `kernel/appman/apps.c` — were deleted; each
now gets an empty stub at `apps/<name>/<name>.c`+`.h` (just `sti` +
`sched_exit()`), ready to be properly rebuilt on the widget toolkit later.
Desktop icons (`desktop_icons_init()`), the dock's default pin
(`dock_init()`), and both `get_app_style()` lookup tables
(`desktop.c`/`dock.c`) were trimmed to just Widget Demo. The debug console
(F12 overlay, Super+W test window, right-click "Inspect" menu item) was
removed outright (not stubbed) along with `apps/debug_console/`, since it
predates the compositor being reliable and is no longer needed.

Auditing the `apps/` directory for the "one folder per app" cleanup
turned up more dead/duplicate code than expected: `gui/apps/` (two
already-empty placeholder folders, confirmed in the previous session —
deleted), `apps/appman/` (a complete second copy of `kernel/appman/`,
never referenced by the Makefile — deleted), `apps/filemanager/` +
`apps/disk_manager/` + `apps/trash_manager/` (compiled into the kernel by
the old Makefile but never registered with appman or called from
anywhere — genuinely dead, not stubs, so deleted rather than kept),
`apps/hello-world/` (an orphaned partial duplicate of the real
`userland/apps/hello-world/`, missing its `src/` — deleted), and a stray
`draco.json` package manifest sitting inside `apps/calculator/` (that
manifest format belongs to the unrelated `userland/apps/` third-party-
package system — removed from the kernel-builtin stub folder).
`apps/DEVELOPING.md`, which documents that `userland/apps/` convention,
was moved to `userland/DEVELOPING.md` to match. `apps/` now contains
exactly one folder per kernel-builtin app (installer + the 13 stubs +
widget_demo), each with just its own `<name>.c`/`<name>.h`.

**Clock/user widget polish.** `gui/widgets/` gained a "raw mode" —
`widget_ctx_init_raw()` sets `ctx->win = -1`, a sentinel three internal
helpers (`w_fill`/`w_pixel`/`w_text`) check before routing to either
`comp_window_*` (normal, window-backed mode) or the raw `fb_fill_rect`/
`fb_put_pixel`/`fb_print` (with transparent background) calls. This lets
desktop CHROME — things the desktop task draws directly alongside the
wallpaper/icons/dock, with no `comp_create_window()` of its own — use the
same `widget_label`/`widget_rect`/`widget_rect_border`/`widget_line`/
`widget_circle` calls as window-backed apps do. Only those non-
interactive functions support raw mode; the interactive ones all need
`widget_begin_frame()`'s window-relative mouse math, which has no
window to be relative to here. `draw_widgets()` in `desktop.c` (the
top-right clock + username panel) was converted to use
`widget_rect`/`widget_label` for its panel fill, shine highlight, and
text; the rounded outer border stays a direct `fb_rounded_rect()` call,
since the toolkit has no rounded-rect primitive.

---

## Widget Demo Polish Pass (completed)

A second round of live testing (after the GUI Finalization Pass) found
four more real issues: the residual flicker hadn't fully gone away, two
small rendering bugs (`.0f` instead of a number, list content
overflowing its border), and the textbox was missing the clipboard
shortcuts a user reasonably expects once free caret movement works.
Concise version in `docs/CHANGELOG.md`; worth a bit more detail here on
two of them.

**Why `.0f` printed literally.** `kernel/klibc.c`'s `vsnprintf()` is a
small, purpose-built formatter for this freestanding kernel — it
implements `%d` (signed decimal, with the existing `fmt_uint` helper and
manual sign handling), `%s`, `%c`, `%x`, and `%%`, full stop. There's no
`%f` case in the switch, and critically no parsing of a `.N` precision
specifier between `%` and the conversion character either. So
`"%.0f"` walked the format string as: `%` (start a conversion) → `.`
(not a recognised flag/conversion char, falls through to being emitted
literally since the switch's default case just copies the character) →
`0` (digit, also not consumed as anything since there's no width/
precision parsing — also emitted literally) → `f` (not a recognised
conversion char, emitted literally). Net effect: `.0f` appears verbatim
in the output, and the `%` that started it all is silently consumed with
nothing to show for it. Properly adding `%f` support means floating-
point-to-decimal-string conversion, which needs either software float
formatting (extra code, more surface area for bugs in a kernel that
hasn't needed this before) or relaxing `-mno-sse`/`-mno-mmx` to allow the
compiler's own float codegen (a build-flag change with its own
implications for a freestanding x86_64 target — worth doing deliberately
in its own pass, not as a one-line fix buried in a widget demo). For now,
every call site needing to display a float-derived value should do what
this fix does: round to int in plain arithmetic (already happening
throughout `widgets.c` without any float-formatting issues, since the
arithmetic itself works fine — only *printing* a float was broken) and
format with `%d`.

**Why the scrollable list overflowed its border, and how clipping was
added.** The list's per-row y-position, `list_y + row*row_h - scroll`,
is correct in the sense that it's exactly where that row belongs given
the current scroll offset — but it's unbounded. When `scroll` isn't an
exact multiple of `row_h` (the normal case while dragging the
scrollbar), the row currently scrolled halfway past the top of the list
has a position slightly less than `list_y` — by design, since that's
the row that should appear "cut off" at the top edge. The bug was that
nothing actually cut it off: `widget_label()` → `comp_window_print()`
just draws wherever it's told, with no concept of a bounding region, so
that sliver of text rendered above the list's border into whatever
space was above it in the window.

Rather than patch this one call site with manual bounds math (which
would have to be redone for every future scrollable-anything), real clip
rect support was added to the compositor itself, since that's the
correct general primitive and the toolkit will need it again. `window_t`
(`gui/compositor/compositor.h`) gained four new fields: `clip_active`
(0 by default — every existing `comp_create_window()` already zeroes the
whole struct, so old code is completely unaffected) and `clip_x/y/w/h`
in content/backbuf-relative coordinates, same space as every other
`comp_window_*` call. A small `clip_allows(win, x, y)` helper (defined
early in `compositor.c`, before anything that needs it) answers "is this
content-space pixel inside the active clip rect, or is there no active
clip rect" — used by `comp_window_print()`'s and
`comp_window_set_pixel()`'s existing per-pixel loops. `comp_window_fill()`
instead intersects its target rectangle against the clip rect once up
front (cheaper than a per-pixel check for what's normally a solid fill).
Two new public functions, `comp_window_set_clip(handle, x, y, w, h)` and
`comp_window_clear_clip(handle)`, round out the compositor-level API.

The widget toolkit wraps these as `widget_begin_clip(ctx, x, y, w, h)` /
`widget_end_clip(ctx)` (no-ops in raw mode, since there's no window to
clip against there — raw-mode chrome is expected to only draw inside its
own intended bounds in the first place). Clipping deliberately does not
nest or stack — a second `widget_begin_clip()` before `widget_end_clip()`
just replaces the active clip rect rather than intersecting with it,
documented as such in `widgets.h` so callers don't assume save/restore
semantics that aren't there. The demo now wraps its list-row loop in
`widget_begin_clip(&wctx, list_x, list_y, list_w, list_h)` /
`widget_end_clip(&wctx)`, so a partially-scrolled row is correctly
clipped at the list's border instead of spilling out above it.

**Textbox clipboard/selection.** `widget_textbox_state_t` gained a
`sel_anchor` field (`-1` = no selection; otherwise the selection spans
`[min(sel_anchor,cursor), max(sel_anchor,cursor))`, with `cursor` always
the "live" end the user is actively moving). Shift+Left/Right/Home/End
extend or shrink the selection from the caret; a plain (non-Shift)
Left/Right while a selection exists collapses the caret to the
selection's near edge instead of just moving one character (matching
standard text-editing convention — pressing an arrow key should always
visually move you in that direction, not silently jump past the whole
selection); a plain click anywhere always clears any selection (matches
the existing "click anywhere → caret to end" behaviour, just also
resetting `sel_anchor`).

Typing a character, or pressing Backspace/Delete, while a selection is
active now replaces/erases the whole selection instead of acting only at
the caret — a small shared helper, `textbox_delete_range(buf, len, a, b)`,
does the `memmove`-based deletion (including the trailing NUL) and is
reused by every "remove this range" call site (Backspace, Delete, typing
over a selection, Ctrl+X).

Ctrl+A/C/X/V are checked first, ahead of the existing branches, against
`ctx->key_char` directly: the keyboard driver
(`kernel/drivers/ps2/keyboard.c`) already turns Ctrl+letter into a
control-code byte 1–26 at the hardware-translation layer (Ctrl+A → 1,
Ctrl+C → 3, Ctrl+V → 22, Ctrl+X → 24 — note these overlap functionally,
not numerically, with the pre-existing special cases like `'\b'`=8 for
Backspace and `'\n'`=10 for Enter, which are themselves Ctrl+H/Ctrl+J
under this same scheme, an existing driver design predating this
session). `keyboard_shift()`/`keyboard_ctrl()` (already declared in
`keyboard.h`, just not previously used inside `widgets.c`) are queried
directly inside `widget_textbox()` — no new field was needed on
`widget_ctx_t` for this, since these are simple real-time state getters
rather than one-shot events that need per-frame draining like
`keyboard_getchar()`.

Ctrl+C copies the current selection, or — if nothing is selected — the
entire field's contents, to a new clipboard: `widget_clipboard_get()`/
`widget_clipboard_set()` in `widgets.c`, backed by one static
`WIDGET_CLIPBOARD_MAX`-byte (256) buffer. This is deliberately
process-wide (one clipboard shared by every `widget_textbox()` call
across every window), not per-textbox or per-window state, so that
copying in one app's textbox and pasting in a different app's textbox
works — matching how every real OS clipboard behaves. Both functions are
exposed in `widgets.h` in case an app wants to read or write the
clipboard from outside a textbox entirely (e.g. a dedicated "Copy"
button for non-text content). Ctrl+X performs the same copy, then
deletes the selection (or clears the whole field if nothing was
selected). Ctrl+V pastes the clipboard's contents at the caret — first
deleting any active selection — truncated as needed to fit the
remaining room in the caller's buffer (`buf_sz - 1 - len`, since one byte
must always be reserved for the terminating NUL).

The selection highlight itself draws as a filled `WIDGET_COL_ACCENT`
rectangle BEFORE the text is drawn, relying on `comp_window_print()`
already painting an opaque background behind every glyph it draws — so
selected characters end up correctly coloured without the toolkit needing
a separate "draw text in a different colour over a highlight" code path.
The blinking caret is suppressed entirely while a selection is active
(showing both a highlight and a blinking caret at the same time is just
visual noise — the highlight alone already communicates "this is where
things stand," same convention most text editors follow).

---

## Window Manager & Desktop Interaction Phase (completed)

A 12-item live-testing report covering workspace switching, window
chrome interactivity, z-order, icons, cursor shape, the clock, window
close cleanup, and a new GUI debug mode. Planned up front in
`docs/GUI_BUGFIX_PLAN.md` before any code changed, per an explicit
request to plan first — that doc has the full root-cause writeup and
scope decisions for all 12 items and is the primary reference for this
phase; `docs/CHANGELOG.md` has the concise per-item summary. A few
worth calling out here specifically:

**The keyboard driver gained two previously-dead mechanisms.** F1-F4
(scancodes 0x3B-0x3E) were mapped to 0 in `sc_normal`/`sc_shifted` (no
ASCII value), so the F1-F4 workspace-switch handling already written in
`desktop.c` had literally never fired from real hardware input — F1-F12
scancode handling was added, mirroring how the extended-key table already
handles arrows/Home/End. Separately, `KB_KEY_ALTTAB` was defined with the
comment "synthesised: Alt+Tab for task switcher" but nothing ever
consumed it; it's now the toggle for the repurposed dock panel (see
below). Both needed the same always-reaches-the-desktop bypass
Super+letter already used, since they're system-level shortcuts, not
app-directed input — F1-F4 got it directly; Alt+Tab and a new
Ctrl+Super+D (for the debug panel) needed it worked out fresh, since
Ctrl+letter is already claimed for control-code generation elsewhere in
the same function (had to reverse that transform to recover the original
letter for the Ctrl+Super+letter case — see the plan doc for the exact
mechanics).

**`dock.c` was more thoroughly repurposed than the original bug list
called for.** It was fully implemented but never wired into the render
loop at all (confirmed empty impact from removing its old always-drawn
call site). Rather than just wiring in the old pinned-apps taskbar
as-is, a follow-up instruction asked for it to become a hidden-until-
toggled open-apps switcher instead, sourced live from the compositor
(new `comp_window_count()`/`comp_window_title()`/`comp_window_exists()`/
`comp_window_is_minimized()` getters) rather than the app registry. This
also absorbed bug #10 (Super+R showing a restore-everything action
instead of a picker) for free, since "open apps" naturally includes
minimized ones once the getters distinguish minimized-but-alive from
actually-destroyed (both read `visible==0`; only the separate
`minimized` flag tells them apart).

**The mouse edge-detection fix (#3) is a plausible-but-unverified
diagnosis**, flagged as such in the plan doc: `mouse_btn_pressed()`/
`released()` share one global "previous button state" updated only by
`desktop_task`'s own `mouse_update_edges()` call, while every
independently-scheduled app was also reading those same functions
directly. This is a real, confirmed architectural sharing issue — fixed
by having the widget toolkit compute press/release locally per
`widget_ctx_t` from `mouse_btn_held()` instead — but without a live VM
to reproduce the original symptom against, it's presented as the most
credible mechanism found after everything else in the click-dispatch
path checked out clean, not a certainty.

---

## Live QEMU Testing Fixes (completed)

The first real boot log from an actual QEMU run, rather than static
reading, surfaced four reported bugs plus two things caught while
investigating them. Concise summaries in `docs/CHANGELOG.md`; the
reasoning behind the two non-obvious finds is worth keeping here.

**How "task 111" led to the utoa64 bug.** The boot log showed
`APPMAN: launched 'Widget Demo' (task 11)` immediately followed by
`INPUT_ROUTER: focus -> task 111` — the exact same integer, logged one
call later, gained a digit. Both call sites are simple
`kinfo("...%d\n", id)` with `id` confirmed to be 11 in both places (the
second is `appman_launch()` calling `input_router_set_focus(id)` with
the very same `id` it just logged), which ruled out a value bug and
pointed straight at the formatter. `vsnprintf()`'s `%d` case correctly
computes the digits via `utoa64()`, which fills a local `tmp[68]` buffer
from the end backwards and null-terminates at `tmp[67]`. The loop's
starting index was `i = 66`, so its first write landed at
`tmp[--i] == tmp[65]` — skipping `tmp[66]` entirely. That gap byte was
never initialized, sitting between the last real digit and the
terminator. `strcpy(buf, &tmp[i])` copies everything from the first
digit up to the first zero byte it finds — for any value needing two or
more digits, the gap byte was included in that range, and being
uninitialized stack memory, it usually happened to already be zero
(hence numbers "usually" printing correctly) but wasn't guaranteed to
be — in this case it happened to hold a leftover `'1'` character from a
previous call's stack usage, turning "11" into "111". Fixed by starting
`i` at 67 instead, so the first digit lands at `tmp[66]`, directly
adjacent to the terminator with no gap. Verified against a standalone
test harness (0, 1, 5, 11, 99, 100, a 9-digit value, and `UINT64_MAX`)
before applying — all produced exact matches. This bug is not new to
this session; it's been present since `utoa64()` was written and affects
every `%d`/`%u`/`%x`/`%p` call anywhere in the kernel with a multi-digit
(or, in principle, any) result — this is the first time a symptom of it
surfaced clearly enough to trace back to the formatter itself rather
than being invisible.

**The self-sustaining IRQ12 loop.** The boot log showed
`IRQ-WDG: both IRQs quiet for 30s — draining PS/2 buffer and running
keyboard_reinit()` repeating many times over the course of the log,
which shouldn't happen under the "fires once per genuine quiet streak,
latched until real activity resumes" design from an earlier session's
fix. Re-reading `irq_watchdog_task()`: `prev1`/`prev12` are updated to
that round's `cur1`/`cur12` snapshot (taken at the TOP of the loop
iteration, before the recovery actions run) at the very end of every
iteration, unconditionally. If `keyboard_reinit()` or the aux-port
`outb 0xA8` reassertion inside the Case C recovery block causes the PS/2
controller to raise a spurious IRQ1 or IRQ12 — a plausible side effect
of re-enabling scanning/aux reporting — that spurious tick wouldn't be
reflected in this round's `cur1`/`cur12` (already read before recovery
ran) but WOULD show up in the NEXT round's fresh read, diffed against
the stale pre-recovery `prev` values. That reads as "d1||d12 != 0" —
fresh activity — which resets `had_recent_activity`, `quiet_rounds`, and
`case_c_done_this_quiet`, re-arming Case C to fire again after another
30s of (genuinely quiet, this time) waiting. Forever, self-sustained by
its own recovery action, with zero real user input involved. Fixed by
sleeping briefly after the recovery actions and re-reading
`cur1`/`cur12` right before the unconditional `prev1 = cur1;
prev12 = cur12;` at the loop's end, so any self-triggered IRQ is folded
into this round's baseline instead of leaking into the next round's
diff. Flagged as a plausible contributor (not a proven one) to
"clicks/keys intermittently don't register" from the same report,
since repeatedly draining the PS/2 output buffer during normal use is
exactly the kind of action that could eat a real packet arriving at an
unlucky moment — but this is inference, not something verified against
a live repro.

**Desktop icon click/drag.** Never had a drag concept at all — the
existing code launched the app immediately on mouse press. Reworked as
press-starts-a-potential-drag, release-decides: no meaningful movement
between press and release means it was a click (launch, now the only
path that does so — this is also incidentally what fixes "can't click an
icon," since there was no other bug found in the click-dispatch path
itself for icons specifically); movement past a small threshold means a
real drag, and release snaps the icon to whichever grid cell
(`(x - ICON_GRID_X) / ICON_CELL_W`, same idea for row) the cursor ended
up over. No collision handling with an already-occupied cell — out of
scope for this pass.

**Dock keyboard navigation.** Up/Down were unconditionally bound to
moving the software mouse cursor via the keyboard (an existing
accessibility feature), with no exception for the dock panel being
open — so even after implementing `dock_move_selection()`, arrow key
presses while the dock was open would never reach it; the cursor-move
binding claimed them first every time. Added a `!dock_is_open()` guard
to that binding and separate Up/Down/Enter handling gated on
`dock_is_open()`, mirroring how `!g_search_open` already gates the same
binding for the search overlay.

---

## VirtualBox Testing Fixes (completed)

The QEMU-based tests in the previous two passes hadn't reproduced the
core window-chrome-unresponsive bug at all — testing in VirtualBox
instead (a genuinely different hypervisor, with its own PS/2 timing
characteristics and, per the user's own observation, noticeably slower
overall) finally surfaced enough of a pattern to find the real cause.

**Why chrome clicks failed but shortcuts and widget clicks didn't.** The
report's exact phrasing — "shortcuts work, but buttons/drag/resize/
click-to-focus via the mouse don't; I could still interact with a
background window's own content, just not bring it to front" — ruled out
several earlier hypotheses immediately. Shortcuts (Super+Q, Super+M,
Super+H) call the exact same underlying functions
(`close_window()`, `comp_toggle_maximize()`, `comp_set_visible()`) the
mouse path is supposed to call, so those functions are fine. Widget
content responding correctly rules out a *general* mouse-tracking
failure. That leaves exactly one thing different between "clicking a
widget inside a window" and "clicking a window's title bar/body from
desktop.c": which code reads the mouse button state, and how.

Every widget click goes through `widget_begin_frame()`, which — since an
earlier pass in this same overall effort — computes its own press/
release edge locally, by diffing the level-triggered `mouse_btn_held()`
against what that specific `widget_ctx_t` saw last time IT checked.
`desktop.c`'s own chrome click detection (`lclick`/`lrelease`) had never
received that same fix — it still used `mouse_btn_pressed()`/
`mouse_btn_released()`, which are edge-detected against a *shared*
snapshot that only `mouse_update_edges()` updates, called once per
`desktop_task` loop iteration. The boot log showed frequent
`SCHED: task 'desktop' (id=10) heartbeat stalled` warnings — direct
evidence that `desktop_task`'s own loop wasn't running on a tight,
predictable cadence. If the gap between two consecutive
`mouse_update_edges()` calls grows large enough (because the task
running them got delayed), an entire click-and-release can complete
inside that gap. The next call's diff, comparing current state against
an increasingly stale snapshot, shows no transition at all — nothing
looks like a "press" ever happened. Widget code sidesteps this because
however late a check runs, it's still comparing against *its own* last
observation, not a separately-timed external snapshot — so it degrades
to "clicks feel less instant" under load rather than "clicks are
silently dropped." Fixed by giving `desktop.c` the same local-edge
treatment: `static int prev_lheld`/`prev_rheld`, diffed against a fresh
`mouse_btn_held()` read each iteration, replacing the PS/2 fallback path
of `lclick`/`rclick`/`lrelease` (the vmmouse-latch fallback branch is
untouched, since vmmouse reports "backdoor not responding" in every
tested environment so far and was never the active path). This doesn't
make a full click-and-release between two *even local* checks
detectable — no polling scheme can do that — but it removes the added
fragility of also depending on a separately-timed snapshot function.

This diagnosis directly explains bug report #5's specific narrative too:
clicking a background window never called `comp_focus_window()`
(because the click was being missed at the `lclick` level before ever
reaching that code), so `comp_focused_window()` kept reporting whatever
was focused before — meaning Super+Q's "close the focused window" acted
on a stale target, matching "closing closed the wrong window" exactly.
Using the dock/Alt+Tab reveal path worked because that calls
`comp_focus_window()` directly from a different, click-independent code
path (`dock_activate_selected()`), which was never affected.

**Resize cursor appearing on a maximized window.** `comp_resize_edge_at()`
computes hit zones purely from `w->x/y/w/h`, with no check for
`w->maximized` at all — since a maximized window's geometry happens to
coincide with the screen edges, the edge-zone math "worked" in the sense
that it correctly matched real screen-edge coordinates, just for a
window that should never expose a resize affordance in the first place.
Added the missing check; since both the resize-cursor shape logic and
the resize-click hitbox already route through this one function, one
fix covers both symptoms.

**Workspace-switcher arrow keys.** Exactly the same root cause as the
dock's keyboard navigation fix from the previous pass, just not yet
applied to the switcher: Up/Down/Left/Right were unconditionally bound
to moving the software cursor, with no `!ws_switcher_is_open()`
exception, so the switcher never saw those keys even after
`ws_switcher_key()` gained Up/Down/Enter handling in this same pass
(mirroring the dock's `g_sel_idx`/`dock_move_selection()`/
`dock_activate_selected()` pattern as `g_kbd_sel` in `ws_switcher.c`).
Per an explicit instruction, the general cursor-movement-via-arrow-keys
feature itself is being kept as-is outside of switcher/dock/search being
open — this fix only adds the same class of exception those two already
had.

**Slow-system warning.** `sched_watchdog_check()` already had everything
needed to detect this — it just never surfaced past a log line.
`g_recent_strikes` is a simple decaying counter (incremented by however
many strikes fire in one watchdog pass, halved on any pass with zero
fresh strikes) rather than a precise measurement — the goal is "notice a
sustained pattern of many tasks stalling," not diagnose a specific cause.
`sched_is_system_slow()` exposes a threshold check on it.
`irq_watchdog_task()` (which already calls `sched_watchdog_check()`
every cycle) fires a `notif_push()` once on the transition into the slow
state. Routing through the existing notification daemon was deliberate:
it draws its toast directly to the framebuffer from its own
independently-scheduled task, so the warning can still reach the screen
even when `desktop_task` — plausibly one of the stalling tasks — can't
keep up with its own compositing.

