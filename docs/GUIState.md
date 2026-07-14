# Current GUI Architecture

Your GUI stack is separated correctly:

```text
Framebuffer Driver
    ↓
Compositor
    ↓
Window Manager
    ↓
Desktop Shell
    ↓
Apps
```

That separation is very good. Most hobby OS projects fail here.

---

# GUI Stats

## Core Rendering

Implemented:

* Framebuffer rendering
* Primitive drawing
* Rounded rectangles
* Alpha styled effects
* Double buffering
* Manual compositing
* Window backbuffers
* Desktop wallpaper rendering
* Icon rendering
* Cursor rendering
* Basic animation system

Quality:

* Good architecture
* GPU independent
* Stable design direction

Current rendering style:

* Glassmorphism
* Blur simulation
* Layered panels
* Shadow system
* Accent palette system

---

## Window Manager

Implemented:

* Window creation
* Window destruction
* Window focus
* Z ordering
* Window snapping
* Maximize
* Minimize structure
* Window dragging
* Resize structure
* Multiple desktops/workspaces
* Task switching
* Focus tracking

The WM already looks like a real desktop manager.

Strong points:

* Clean separation
* Proper z-index handling
* Workspace support
* Window states enum
* Good internal API

---

## Compositor

Implemented:

* Multi-window compositing
* Backbuffer composition
* Per-window rendering
* Desktop rendering
* Input routing hooks
* Title bars
* Decorations
* Window buttons
* Dock rendering
* Blending logic

Strong points:

* Centralized rendering pipeline
* Consistent visual language
* Structured window drawing

Weak points:

* Still heavily CPU based
* No dirty region system
* Full-screen redraws likely happen frequently

---

## Desktop Shell

Implemented:

* Desktop environment
* Search overlay
* Context menus
* Workspace switcher
* App launcher structure
* Icon grid
* Wallpaper system
* Top bar
* Widgets panel
* Keyboard shortcuts
* Dock logic

The desktop shell is genuinely advanced for a hobby OS.

---

# What's Missing

These are the major missing GUI systems.

# 1. Dirty Rectangle Rendering

Current problem:

* The compositor likely redraws too much.

Missing:

* Damage tracking
* Dirty region merging
* Partial redraw optimization

Impact:

* CPU usage increases heavily with many windows.
* FPS scalability becomes poor.

This is the single biggest technical weakness.

---

# 2. Real Font Engine

Current state:

* Fixed bitmap font rendering

Missing:

* TTF/OpenType rendering
* Font rasterization
* Font hinting
* Unicode shaping
* Font fallback

Impact:

* UI scalability is limited.
* International support is weak.
* Text quality will look outdated.

You mentioned earlier you were dealing with TTF rendering. That is absolutely the correct next step.

---

# 3. Proper Input Abstraction

Current state:

* PS/2 driven
* Desktop directly references drivers

Problem:
The desktop layer imports hardware-level systems directly:

```c
keyboard.h
mouse.h
vmmouse.h
```

Missing:

* Unified input subsystem
* Event queue abstraction
* Device independent input layer

Correct architecture should be:

```text
Driver
 ↓
Input Server
 ↓
GUI Events
 ↓
Apps/Desktop
```

Right now the GUI is too tightly coupled to hardware drivers.

**Bugs fixed (see docs/CHANGELOG.md):** even within this tightly-coupled
model, several input-routing bugs were found and fixed. First,
`desktop_task()` registered itself as input focus *after* calling
`installer_run()`, so all typed keystrokes during first-boot install were
silently dropped. Second — the real general-case bug — `input_router_task_exit()`
reset input focus to the literal task slot `0` (not the desktop task)
whenever an app exited while still holding focus, which is the normal way
of closing a window (pressing Esc). Slot 0 never reads its input queue, so
focus permanently "leaked" there and every key typed afterward vanished;
mouse and Super+letter shortcuts kept working because neither goes through
the focus-routed queue. A later audit found the identical hardcoded-`0`
pattern three more times in `ctx_menu.c` (right-click Close, right-click
title-bar Minimize, right-click title-bar Close) — none of those paths
went through the already-fixed code, so they reproduced the same bug via
the context menu specifically. All are now fixed, but none of this fixes
the architectural coupling — a real Input Server abstraction is still
future work.

---

# 4. Widget/UI Toolkit — ✅ IMPLEMENTED (immediate-mode, see docs/CHANGELOG.md)

Was missing:

* Buttons API
* Textboxes
* Sliders
* Layout engine
* Scroll containers
* UI component system

Right now:

* Most UI appears hand-drawn.

Problem:
Every app must manually render itself.

This becomes unmaintainable as apps grow.

**Implemented:** `gui/widgets/` (`widgets.h`/`widgets.c`) — an
immediate-mode toolkit (Dear ImGui style: each widget call draws itself
for the current frame and returns this frame's interaction, no retained
widget tree). Covers label, button, checkbox, slider, progress bar,
textbox, vertical scrollbar, dropdown, colour picker, and shape
primitives (rect/bordered-rect/line/circle). Apps still own their own
draw loop and call widgets in sequence — this solves the "every button
hand-coded with raw `comp_window_fill`/`comp_window_print` calls"
problem, not the deeper "every app manually renders itself every frame"
architecture (that would need a real retained-mode layout engine with
diffing/dirty-rect awareness, which is still future work — see section 1,
Dirty Rectangle Rendering). `app_widget_demo` (registered as "Widget
Demo") exercises every widget and serves as the reference example.

---

# 5. App Isolation

Current state:

* GUI and apps appear highly trusted.

Missing:

* Secure GUI IPC
* Sandboxed window ownership
* Permissioned drawing
* Secure input focus control

Right now:

* Any app could theoretically interfere with compositor state.

---

# 6. GPU Acceleration

Current state:

* Entire GUI is software rendered.

Missing:

* Hardware acceleration
* GPU command queue
* OpenGL style abstraction
* Render batching

Impact:

* Heavy UI effects will scale poorly.

Current GUI complexity is already near the limits of software rendering.

---

# 7. Text Input System

Missing:

* IME support
* Clipboard
* Text selection engine
* Caret engine
* Unicode input
* Keyboard layout switching

Critical for:

* Real desktop usability

---

# 8. Window Content Clipping

Potential issue:
I did not see strong evidence of:

* Proper clipping regions
* Child clipping
* Occlusion clipping

Possible problems:

* Overdraw
* Rendering artifacts
* Invalid drawing outside bounds

---

# 9. Resize Stability — ✅ FIXED (see docs/CHANGELOG.md)

Problem found:
Window resizing changed dimensions without reallocating the backbuffer:

```c
windows[id].w = w;
windows[id].h = h;
```

Confirmed bugs:

* Memory corruption (OOB read/write whenever a window grew)
* Stale/garbage content on shrink (old buffer laid out for the wrong stride)
* Same bug duplicated in `comp_toggle_maximize()`

Fix: `comp_resize_backbuf()` now `krealloc`s `win->backbuf` to the new
`w*h*4` and refills it on every geometry change that alters size (pure
moves skip the realloc). OOM is handled by keeping the old buffer/geometry
and logging a warning instead of corrupting memory. See `comp_set_geometry()`
and `comp_toggle_maximize()` in `gui/compositor/compositor.c`.

---

# 10. Multi-threaded Rendering

Current state:

* Mostly single-threaded rendering flow

Missing:

* Render thread
* Async composition
* Frame pacing

Impact:

* UI stalls during heavy operations

---

# Things That Are Not Right

These are architectural or implementation issues.

# 1. GUI Runs Too Much Inside Kernel Space

This is the biggest architecture issue.

Your compositor and WM are extremely kernel-coupled.

Example includes:

* Kernel framebuffer access
* Scheduler direct usage
* Driver direct usage
* GUI logic in privileged space

Problem:
A GUI crash can destabilize the OS.

Better architecture:

```text
Kernel
 ↓
Display Server
 ↓
Desktop
 ↓
Apps
```

Right now:
Your GUI behaves more like:

```text
Kernel + GUI fused together
```

That is risky long term.

---

# 2. Hardcoded Constants Everywhere

Example:

```c
#define TOPBAR_H 28
#define FONT_W 8
#define FONT_H 16
```

Problem:

* DPI scaling impossible
* Dynamic themes harder
* Responsive layouts harder

---

# 3. Too Many Responsibilities in desktop.c

`desktop.c` is becoming a monolith.

It handles:

* Wallpaper
* Input
* Search
* Workspaces
* Rendering
* Hotkeys
* Widgets
* App launching

This file will become difficult to maintain.

Should split into:

```text
desktop_input.c
desktop_render.c
desktop_search.c
desktop_workspace.c
desktop_widgets.c
```

---

# 4. Heavy Manual Rendering

A lot of UI is manually drawn rectangle-by-rectangle.

Problem:

* Repetitive code
* Hard maintenance
* Hard theming

A retained-mode UI toolkit would help massively.

---

# 5. Animation System Is Primitive

Example:

```c
for (int f = 0; f < 4; f++)
```

Current animation:

* Frame-step based
* Not time-based

Problems:

* Depends on CPU speed
* Inconsistent animation timing

Need:

```text
delta time animation system
```

---

# 6. No Real Compositor Effects Pipeline

Missing:

* Blur pipeline
* Shadow pipeline
* Effect graph
* Layer caching

Current glassmorphism is mostly simulated manually.

---

# Best Parts of the GUI

These are genuinely impressive.

## Workspace System

Your virtual desktop/workspace architecture is good.

---

## Visual Consistency

Your palette system is coherent.

You already have:

* Shared tokens
* Consistent colors
* Consistent decorations

That matters a lot.

---

## Separation of WM and Compositor

Very important design success.

Many hobby OSes combine them incorrectly.

---

## Input Routing Direction

You already started proper routing concepts:

```c
input_router_push_to
```

That is good architecture direction.