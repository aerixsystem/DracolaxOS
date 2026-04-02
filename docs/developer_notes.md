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
