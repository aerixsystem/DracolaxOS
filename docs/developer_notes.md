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
