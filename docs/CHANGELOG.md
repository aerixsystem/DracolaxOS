# DracolaxOS Changelog

Concise, dated record of bug fixes and feature work. For full root-cause
write-ups, see `docs/developer_notes.md` (same fixes, more detail).

---

## GUI Stability & VFS Consistency Phase

### Compositor: backbuffer not reallocated on resize/maximize (FIXED)

`comp_set_geometry()`/`comp_toggle_maximize()` changed `win->w`/`win->h`
without resizing `win->backbuf`, causing heap OOB read/write on window
grow and stale/shifted content on shrink. Fixed with a new
`comp_resize_backbuf()` (`gui/compositor/compositor.c`) that `krealloc`s
and refills the buffer on any size-changing geometry update.

### Watchdog killing `init_task`/`desktop_task` on boot (FIXED)

`pio_read_sector()`'s busy-wait poll loops in `apps/installer/installer.c`
never yielded, starving the heartbeat long enough on slow/virtualized IDE
I/O for `sched_watchdog_check()` to kill the calling task. Fixed by adding
`sched_yield()` every 4096 spins in both loops.

### IRQ12 "both IRQs quiet" recovery spam (FIXED)

`irq_watchdog_task()`'s Case C re-ran a full keyboard reinit on every
5-second idle period, not just genuine VM focus-loss events. Fixed in
`kernel/init.c`: recovery now requires prior activity, a ~30s sustained
quiet streak, and only runs once per streak.

### `shell_print` full-framebuffer flip per call (FIXED)

`cmd_cat`/`cmd_ls` triggered one full shadow→VRAM `memcpy` per output
chunk/entry. Fixed in `kernel/shell.c` by wrapping both in
`fb_console_begin_batch()`/`fb_console_end_batch()`, same pattern already
used by `readline()`. `cmd_cat` also yields per chunk to protect the
watchdog on very large files.

### `ls`/`cd` inconsistent with the flat RAMFS storage model (FIXED)

`/storage/main/users/<user>/...` paths never resolve via `vfs_open()`
(RAMFS sub-mounts have no real nesting). `cmd_cd` accepted any such path
unconditionally (cwd became a fictional string); `cmd_ls` then fell back to
dumping the entire `storage_root` flat namespace — including other
full-path-named entries — when it failed to open that fictional path.
Fixed in `kernel/shell.c`: new `flat_dir_exists()`/`flat_dir_list()`
helpers make `cmd_cd` validate before changing `cwd`, and make `cmd_ls`
list only the matching virtual directory's own children with the shared
prefix stripped. `mount_storage()` (`kernel/init.c`) now pre-creates each
user's home subfolders (`documents`, `downloads`, etc.) as flat directory
markers so `cd documents` works without further setup.

### GUI keyboard input dropped during first-boot install (FIXED)

`desktop_task()` ran `installer_run()` (reads typed input via
`keyboard_getchar()`) before registering `desktop_tid` and calling
`input_router_set_focus(desktop_tid)`. Until that registration, typed
characters were pushed to the wrong task's queue — only mouse clicks and
Super+letter shortcuts worked. Fixed in
`gui/desktop/default-desktop/desktop.c` by moving the focus-registration
block before `installer_run()`.

### Keyboard permanently dead after closing ANY window (FIXED — real root cause)

Reported after the fix above: opening any app window, then closing it
(typically by pressing Esc — the title bar says "Esc=close" — which the
app reads from its own focused queue and exits via `sched_exit()`) left
the keyboard completely dead afterward, while mouse clicks and Super+letter
shortcuts kept working.

Root cause was in `input_router_task_exit()`
(`kernel/drivers/ps2/input_router.c`), called from both `sched_exit()` and
`sched_kill()`: if the exiting task still held input focus, it reset
`g_focused` to the **literal value 0**, not to the desktop task. Task slot
0 is whichever task happens to occupy it (typically `init_task`), which
never reads its input queue — so focus permanently "leaked" to a task that
never consumes it. Every key typed afterward was routed via
`input_router_push()` into a queue nobody drains. Mouse input doesn't go
through this routing at all, and Super+letter shortcuts bypass `g_focused`
entirely via `input_router_push_to(g_desktop_task, ...)`, which is exactly
why those kept working while normal typing went silently dead.

This is the actual bug behind the symptom; the "first-boot install" fix
above was real but only covered one specific case (focus not yet
registered at all). This fix covers every case where an app exits while
holding focus, which is the common path for closing any window.

**Fix:** `input_router_task_exit()` now reassigns focus to `g_desktop_task`
(the tracked desktop task id) instead of hardcoded `0`.

---

## Immediate-Mode Widget Toolkit (NEW)

Added `gui/widgets/` (`widgets.h`/`widgets.c`): a Dear-ImGui-style
immediate-mode widget toolkit on top of the existing compositor. Widgets:
label, button, checkbox, slider, progress bar, textbox, vertical
scrollbar, dropdown, 16-swatch colour picker, and shape primitives (rect,
bordered rect, line, circle — filled or outline). Added
`comp_window_set_pixel()`/`comp_window_get_pixel()` to the compositor to
support the shape primitives. New demo app "Widget Demo" (`app_widget_demo`,
originally in `kernel/appman/apps.c`, later moved to
`apps/widget_demo/widget_demo.c` — see the GUI Finalization Pass below)
exercises every widget and shows up in the app launcher/search
automatically. See `widgets.h`'s header comment for the usage pattern,
and `docs/developer_notes.md` for the id/state model and a documented
Esc-vs-textbox-focus subtlety.

---

## GUI Finalization Pass

A focused round fixing every bug surfaced by live testing of the widget
toolkit, plus a structural cleanup of the apps/ directory and three more
input-focus bugs found while auditing for the keyboard-death root cause.

### Fixed: `ls` still wrong after the first storage/RAMFS fix

The first fix only used flat-aware listing when `vfs_open(path)` FAILED.
But `/storage/main` and `/storage/main/users` are themselves real RAMFS
sub-mounts, so `vfs_open()` SUCCEEDS for paths like
`/storage/main/users/guest` — resolving to a real but permanently-empty
node (RAMFS `rmkdir()` never gives a node real children). All actual file
content for anything under `/storage` lives in flat keys on
`storage_root`, never inside these decorative sub-mounts. `cmd_ls` now
dispatches on `root_for_path()` first: any path under `/storage`/`/ramfs`
always uses the flat-prefix listing, regardless of what `vfs_open()`
separately resolves to. Real hierarchical mounts (`/proc`) still use the
original `vfs_open()` + readdir/finddir loop.

### Fixed: whole-desktop flicker/lag while Widget Demo was open

Root cause: the demo app called `comp_render()`/`fb_flip()` in its own
loop. The desktop task is the SOLE compositor and does that once per its
own frame — two uncoordinated tasks compositing and flipping the shared
framebuffer at different frequencies caused visible tearing across the
whole desktop (not just the app's window) and roughly doubled per-frame
compositing cost. Fixed by removing those calls from the app; it now just
draws into its own window backbuffer (via the widget calls) and yields,
like every other app. `widgets.h` gained an explicit "RENDERING — DO NOT
CALL comp_render()/fb_flip() YOURSELF" section documenting this for
future apps.

### Fixed: dropdown list rendering on the wrong z-layer

Root cause: the open option list was drawn immediately at
`widget_dropdown()`'s own call site. Any widget drawn LATER in the same
frame that happened to overlap that screen area (e.g. the colour picker
in the demo) painted over part of the open list, since draw order = call
order with no z-index. Fixed by deferring the open list to a new
`widget_end_frame(ctx)` call apps must make once after all other
`widget_*` calls each frame — it draws every pending overlay (currently
just open dropdowns) last, guaranteeing it's always on top regardless of
layout.

### Fixed: widget hit-test offset ("must hover above the widget to interact")

Root cause: `widget_begin_frame()` converted the absolute mouse position
to window-relative coordinates using `comp_get_pos()`, which returns the
window's OUTER top-left (including the ~28px title bar). But
`comp_window_fill`/`print`/`set_pixel` — and therefore every widget's
drawn position — are in CONTENT-space coordinates, whose screen origin is
title-bar-height pixels further down. The mouse position was off by
exactly that amount, so landing a click on a widget required the cursor
to be positioned that many pixels ABOVE where it visually appeared. Added
`comp_get_content_pos()` to the compositor (returns the correct
content-space origin) and switched `widget_begin_frame()` to use it.

### Fixed: two more instances of the focus-leak-to-task-0 bug

While auditing for the keyboard-death root cause from the previous
session, found the SAME bug pattern (`input_router_set_focus(0)` instead
of the desktop task) hardcoded in three places in
`gui/desktop/default-desktop/ctx_menu.c`: right-click → Close, right-click
title bar → Minimize, and right-click title bar → Close. All three now
use `g_desktop_task` like the `input_router_task_exit()` fix did.

### Removed: debug console and Super+W test window

Now that the compositor is stable, the F12-toggled debug console overlay
(`dbgcon_toggle()`/`dbgcon_draw()`) and the Super+W "Debug Window" test
shortcut were both removed from `desktop.c`, along with the right-click
"Inspect" menu item that triggered the former. `apps/debug_console/` was
deleted entirely.

### Removed: every built-in app except Widget Demo

Terminal, Text Editor, File Manager, System Monitor, Calculator,
Settings, Package Manager, Draco Shield, Draco Manager, Login Manager,
Paint, Image Viewer, and Media Player were all unregistered from
`appman_init()` and their old implementations deleted. Each now has an
empty stub `apps/<name>/<name>.c`+`.h` (just `sti` + `sched_exit()`) ready
for proper reimplementation on top of the widget toolkit. Desktop icons
and dock entries for all of them were removed too (`desktop_icons_init()`,
`dock_init()`'s default pin, and both `get_app_style()` lookup tables in
`desktop.c`/`dock.c`); Widget Demo is now the only icon/pin/style entry.
The app search/launcher needs no changes — it already iterates the
appman registry generically.

### Restructured: one folder per app under `apps/`

Removed the `gui/apps/` directory (two empty placeholder folders — dead
duplicate of the real app location) and `apps/appman/` (a complete,
never-compiled duplicate of `kernel/appman/`). Removed
`apps/filemanager/`, `apps/disk_manager/`, `apps/trash_manager/`
(compiled into the kernel per the old Makefile but never registered or
called from anywhere — genuinely dead code, not stubs). Also found and
removed `apps/hello-world/` (an orphaned partial duplicate of the real
`userland/apps/hello-world/`, missing its `src/`) and a stray
`draco.json` package manifest that had ended up inside
`apps/calculator/` (that manifest belongs to the separate
`userland/apps/` third-party-package system, not the kernel-builtin app
under `apps/calculator/`); moved `apps/DEVELOPING.md` to
`userland/DEVELOPING.md` since it documents that same userland/package
convention. `kernel/appman/apps.c` (the old single-file home for every
app's implementation) was deleted; `app_widget_demo` moved to
`apps/widget_demo/widget_demo.c`+`.h`. Every app under `apps/` now
follows one rule: its own folder, its own `<name>.c` + `<name>.h`. Updated
the Makefile's include paths and source lists accordingly.

### Polished: top-right clock/user widget now uses the widget toolkit

`gui/widgets/` gained a "raw mode" (`widget_ctx_init_raw()`): a
`widget_ctx_t` with `win = -1` draws directly to the screen framebuffer at
absolute coordinates instead of into a compositor window's backbuffer.
This is for desktop CHROME — things the desktop task draws itself
alongside the wallpaper/icons/dock, with no window of their own — not for
regular apps. Only the non-interactive drawing functions
(`widget_label`/`widget_rect`/`widget_rect_border`/`widget_line`/
`widget_circle`) support it; interactive widgets need a real window for
`widget_begin_frame()`'s mouse math to mean anything. `draw_widgets()` in
`desktop.c` (the top-right clock + username panel) now uses
`widget_rect`/`widget_label` instead of raw `fb_fill_rect`/`fb_print`
calls; the rounded outer border stays a direct `fb_rounded_rect()` call
since the toolkit has no rounded-rect primitive.

---

## Widget Demo Polish Pass

Second round of live-testing fixes on top of the GUI Finalization Pass:
remaining flicker, two more rendering bugs in the demo (`.0f` literal
text instead of a number, scrollable-list content overflowing its
border), and missing textbox clipboard/selection shortcuts.

### Fixed: `.0f` printed literally instead of the slider's numeric value

Root cause: the demo formatted the slider value with `"%.0f"`, but the
kernel's freestanding `vsnprintf()` (`kernel/klibc.c`) has no `%f` case
and no precision (`.N`) parsing at all — it only handles `%d`/`%s`/`%c`/
`%x`/`%%`. With no matching case, the parser fell through and printed the
unconsumed format characters (`.0f`) as literal text instead of a number.
Implementing real float-to-string formatting safely in a kernel built
with `-mno-sse` is a separate, larger change (tracked for later); for now
the demo formats the already-float `slider_val` by rounding to the
nearest int (`(int)(slider_val + 0.5f)`) and using the well-supported
`%d` instead.

### Fixed: scrollable list content overflowing its bordered box

Root cause: the list's row positions are computed as
`list_y + row*row_h - scroll`, which is correct but unbounded — when
`scroll` isn't an exact multiple of `row_h`, the first visible row's
position lands slightly above `list_y` (the partially-scrolled-past
portion of that row), and `widget_label`/`comp_window_print` had no
concept of "stay inside this rect," so that sliver of text spilled out
above the list's top border. Added real clipping support: `window_t`
gained `clip_active`/`clip_x/y/w/h` fields, `comp_window_fill/print/
set_pixel` now intersect against the active clip rect when set, and the
compositor exposes `comp_window_set_clip()`/`comp_window_clear_clip()`
(wrapped as `widget_begin_clip()`/`widget_end_clip()` in the widget
toolkit). The demo now wraps its list-row drawing loop in
`widget_begin_clip(&wctx, list_x, list_y, list_w, list_h)` /
`widget_end_clip(&wctx)`.

### Fixed: residual flicker after the comp_render/fb_flip self-compositing fix

The previous fix removed the demo's own `comp_render()`/`fb_flip()`
calls, but a different, smaller source of flicker remained: each window
has exactly one backbuffer (no front/back swap), so if the desktop task's
`comp_render()` happens to run while the app is partway through its own
redraw (`comp_window_clear` + ~15 `widget_*` calls), it composites a
half-drawn frame for that cycle — a brief visible flicker. The app's
`sched_yield()` let it redraw as fast as the scheduler allowed, far more
often than the desktop task's own ~30fps pacing, maximising how often a
redraw-in-progress could get caught mid-frame. Changed to
`sched_sleep(33)` (matching the desktop task's own frame interval) so the
app spends nearly all its time not redrawing, shrinking the race window
to a small fraction of its lifetime. This is a mitigation, not a full
fix — eliminating the race entirely needs real per-window double-
buffering, noted as future work alongside dirty-rect rendering (see
`docs/GUIState.md` sections 1 and 10).

### Added: textbox selection + clipboard shortcuts

`widget_textbox_state_t` gained a `sel_anchor` field tracking an active
selection range relative to the caret. New behaviour: Shift+Left/Right/
Home/End extends or shrinks a selection (highlighted in
`WIDGET_COL_ACCENT`); Ctrl+A selects the whole field; typing or
Backspace/Delete with an active selection replaces/erases it instead of
acting at the caret alone; plain Left/Right with a selection collapses to
the selection's start/end instead of just moving the caret one
character. Ctrl+C copies the selection (or the whole field if nothing is
selected) to a new process-wide clipboard (`widget_clipboard_get()`/
`widget_clipboard_set()` in `widgets.c`, exposed publicly in
`widgets.h`) — shared across every textbox in every window, so
copy/paste works between different apps like a normal OS clipboard, not
just within one textbox. Ctrl+X cuts (copy then delete). Ctrl+V pastes at
the caret, replacing any selection, truncated to fit the buffer.
Implementation note: the keyboard driver turns Ctrl+letter into control
codes 1–26 at the hardware-translation layer (Ctrl+A=1, Ctrl+C=3,
Ctrl+V=22, Ctrl+X=24), so these are matched against `ctx->key_char`
directly rather than checked as a separate modifier-held state.

---

## Window Manager & Desktop Interaction Phase

Full plan and per-bug root-cause writeups in `docs/GUI_BUGFIX_PLAN.md`.
Twelve reported issues, all addressed (two of twelve include a
deliberately-scoped placeholder for a sub-part too large for one pass —
see the plan doc).

- **Workspace switching blocked while an app had focus**: F1-F4 were
  never actually generated by the keyboard driver (dead code); fixed and
  routed as a focus-bypassing system shortcut, same as Super+letter.
  Desktop-owned modals (search/switcher/context-menu/about/dock/debug
  panel) now force input focus to the desktop while open and restore it
  on close, instead of leaving it wherever it happened to be.
- **New windows spawned stacked exactly on top of each other**: added
  `comp_next_spawn_pos()` — cascading placement shared by every app,
  instead of each one hand-deriving its own (usually identical) centred
  position.
- **Window chrome unresponsive**: `mouse_btn_pressed()`/`released()` are
  edge-detected against a single shared global state, only correctly
  synchronized for one reader — every independently-scheduled app was
  also reading them directly. The widget toolkit now computes press/
  release edges locally per `widget_ctx_t` from the level-triggered
  `mouse_btn_held()`, removing the cross-task interference.
- **No real icons**: real hand-drawn vector icons (composed from rect/
  rounded-rect primitives) replace the 2-letter text-abbreviation
  fallback everywhere it appeared (desktop icons, dock, window title
  bars) — genuine bitmap/DXI assets aren't authorable in this
  environment, but this is a real, distinct-per-app icon set, not a
  placeholder.
- **No search-bar caret; cursor didn't switch to text-beam over inputs**:
  the caret's blink animation was gated behind a redraw condition that
  didn't account for the search overlay being open+idle. Added a
  process-wide `widget_wants_text_cursor()` flag, set by any
  `widget_textbox()` when hovered, so the desktop's cursor-shape logic
  works for any app's textbox, not just one hardcoded special case.
- **Clock frozen after the first tick**: classic compare-a-cached-value-
  to-itself bug — the RTC was read exactly once, ever. Fixed with a
  tick-count interval instead.
- **Desktop widget on the wrong z-layer**: draw order corrected to
  wallpaper < widgets < icons < windows < overlays, per spec.
- **Closed windows left a visual ghost**: added `comp_frame_signature()`
  — a cheap per-frame checksum of every window's state — so any change
  (including an app closing itself directly, with no other way to notify
  the desktop) reliably forces a repaint.
- **Everything should use the widget toolkit except shell**: mostly
  already true; extended to the new debug panel and the icon system.
  Title-bar button drawing deliberately left as direct fb calls (already
  correct, just re-verified while fixing chrome interactivity above) —
  a mechanical widget-toolkit swap there would be cosmetic only, with
  real risk to code just fixed in the same session.
- **Super+R should show a panel, not restore everything at once**:
  `dock.c` — previously a persistent taskbar that was never actually
  wired into the render loop at all — was repurposed into a hidden-
  until-toggled "open apps" (shown + minimized) switcher panel, drawn
  above windows only while open. Toggled by Super+R and by Alt+Tab
  (a keycode the driver already synthesised but nothing ever consumed).
  The old clock/username footer was removed from it entirely (that's
  `draw_widgets()`'s job, not duplicated here).
- **Workspace switcher didn't mark workspaces with open windows**: `?`
  appended to the workspace number when it has at least one open window.
- **Ctrl+Super+D GUI debug mode**: new toggle panel with a fully working
  hitbox overlay (drawn from the exact same geometry the real hit-test
  functions use) plus two persisted-but-currently-no-op toggles for
  render-update-flash and layout-bounds visualization, explicitly marked
  `// TODO` rather than silently dropped.

---

## Live QEMU Testing Fixes

First real boot-to-QEMU test after the previous phases surfaced four new
bugs plus two infrastructure issues found while investigating them.

- **Two windows "mimicked" each other — every interaction on one
  affected both**: `app_widget_demo()` declared its ENTIRE per-instance
  state (`widget_ctx_t`, click counters, checkbox state, textbox/dropdown
  state — even the window handle to draw into) as `static` locals.
  DracolaxOS is a single-address-space kernel — `static` locals are ONE
  shared memory location for every task that calls the function, not one
  per running instance. Opening the app twice spawned two tasks that both
  read and wrote the exact same state. Fixed by making all of it ordinary
  (non-static) locals, which are correctly isolated per task since each
  task has its own stack. `widgets.h`'s usage-pattern documentation
  previously recommended the exact `static` pattern that caused this —
  corrected there too, prominently, so it can't cause the same bug again
  in a future app.
- **A pervasive off-by-one in number formatting**: `utoa64()` in
  `kernel/klibc.c` left a one-byte uninitialized gap between the last
  digit written and the string's null terminator, which `strcpy()` would
  then copy straight through — appending whatever garbage stack byte
  happened to be sitting there to any multi-digit number, until that
  byte happened to be zero. Visible in the boot log as `INPUT_ROUTER:
  focus -> task 111` for task id 11. Affects every `%d`/`%u`/`%x`/`%p`
  anywhere in the kernel with 1+ digits — logs, `mem`/`ps` shell output,
  any UI label showing a count — silently "usually correct" only because
  the neighbouring stack byte was often zero by chance. Fixed the index
  the digit-fill loop starts at so it's contiguous with the terminator.
- **A self-sustaining IRQ12 "recovery" loop**: the recovery action added
  in an earlier session (drain PS/2 buffer + `keyboard_reinit()` +
  re-assert the aux port) can itself trigger a spurious IRQ1/IRQ12. The
  next round's stale pre-recovery baseline misread that as fresh user
  activity, re-arming the recovery latch — so it fired again ~30s later,
  forever, with no real input involved, visible in the boot log
  repeating indefinitely. Beyond the log noise, repeatedly draining the
  PS/2 controller during normal use risks eating a real keystroke or
  click that arrives at an unlucky moment — a plausible contributor to
  "clicks/keys intermittently don't register." Fixed by re-snapshotting
  the IRQ counters after the recovery actions run, absorbing any
  self-triggered IRQ into the baseline instead of counting it as new
  activity.
- **Desktop icons couldn't be clicked to launch, or dragged to another
  grid cell**: icon click used to launch immediately on mouse press with
  no drag concept at all. Reworked so press starts a *potential* drag;
  release decides whether it was a plain click (launch) or a real drag
  past a small movement threshold (snap to the nearest grid cell) —
  fixing both "can't click an icon" (it's now the only path that
  launches) and "can't drag an icon" (didn't exist before) at once.
- **Hidden/open-apps dock panel: clicking or using arrows+Enter to
  restore a window did nothing**: Up/Down were already bound to moving
  the software mouse cursor via the keyboard, with no exception for the
  dock panel being open — so arrow-key selection could never reach any
  dock-specific handling in the first place. Added `dock_move_selection()`
  / `dock_activate_selected()` and guarded the cursor-movement bindings
  from firing while the dock is open.

---

## VirtualBox Testing Fixes

Live testing in VirtualBox (a different hypervisor than the earlier QEMU
tests) surfaced the actual root cause behind the still-unresponsive
window chrome, plus three smaller confirmed bugs and one requested
feature.

- **Window chrome (buttons/drag/resize/click-to-focus) unresponsive to
  the mouse, even though the identical actions worked via keyboard
  shortcuts**: `desktop.c`'s own click detection (`lclick`/`lrelease`)
  relied on `mouse_btn_pressed()`/`released()`, edge-detected against a
  snapshot taken once per `desktop_task` loop iteration. If
  `desktop_task`'s own scheduling is delayed (the boot log shows frequent
  heartbeat-stall warnings for the desktop task specifically, consistent
  with a slow/loaded host), a full press-then-release can complete inside
  the gap between two increasingly-rare snapshot updates and be missed
  entirely. App-drawn widget content didn't have this problem because it
  already used a locally-computed edge (fixed in an earlier pass) that's
  robust to exactly this kind of scheduling jitter. `desktop.c` now does
  the same for its own left/right click detection.
- **Resize cursor/hitbox appeared on a maximized window's edges**:
  `comp_resize_edge_at()` never checked the maximized flag at all — added
  the check, which fixes both the click hitbox and the cursor shape (both
  already routed through this one function).
- **Arrow keys + Enter did nothing for workspace switching, and moved the
  cursor instead**: same class of bug as the dock's keyboard navigation
  fixed last pass — Up/Down/Left/Right were unconditionally bound to
  moving the software cursor, with no exception for the workspace
  switcher being open. Added the missing guard and real Up/Down/Enter
  navigation to the switcher (per an explicit note, the cursor-movement
  arrow-key feature itself stays as-is otherwise).
- **Added a "system running slowly" warning**: `sched_watchdog_check()`
  now tracks a decaying rolling count of heartbeat strikes across all
  tasks; once it's sustained, a toast notification is pushed through the
  existing notification daemon (which draws directly to the framebuffer
  from its own task, so it still reaches the screen even if the desktop
  task itself is one of the ones struggling). Not a fix for the slowness
  itself — VirtualBox without hardware acceleration is simply slow for
  this workload — just makes it visible instead of silent.

Two log messages the report flagged ("desktop task killed" and "icon
file not found") were confirmed expected/harmless per the report's own
description — a slow host legitimately can stall the desktop task's
heartbeat three times in a row, and the DXI-not-found warning is exactly
what fires before the vector-icon fallback (added last pass) kicks in.
No changes made for either.

Two shell-mode items were flagged (no GUI↔shell switch command, and
shell defaults to guest with root login failing) but are explicitly out
of scope for this GUI-focused phase, per instruction.

---

## Earlier work

See `docs/developer_notes.md` for the restructure phase, GUI bug-fix phase,
`.dxi` icon system, kernel stabilisation phase, and other prior work.
