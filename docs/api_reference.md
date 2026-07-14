# Kernel API Reference

Internal API used by kernel subsystems and Ring-3 apps (via syscall). All functions listed here are Ring-0 unless noted.

---

## Framebuffer (kernel/drivers/vga/fb.h)

```c
/* Initialise framebuffer from Multiboot2 info */
void     fb_init(struct multiboot_tag_framebuffer *tag);

/* Shadow buffer (double-buffering) */
void     fb_enable_shadow(void);
void     fb_flip(void);                         /* shadow → VRAM */
uint32_t *fb_shadow_ptr(void);                  /* direct pointer */

/* Lock/unlock kernel console output into framebuffer */
void     fb_console_lock(int lock);

/* Colour helpers */
uint32_t fb_color(uint8_t r, uint8_t g, uint8_t b);
uint32_t fb_blend(uint32_t a, uint32_t b, uint8_t alpha); /* alpha: 0=a, 255=b */

/* Drawing primitives */
void     fb_clear(uint32_t color);
void     fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     fb_rounded_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                         uint32_t r, uint32_t color);
void     fb_blit(uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
                 const uint32_t *src);

/* Text rendering (8x16 VGA font, scale=1; fb_print_s for any scale) */
void     fb_print(uint32_t x, uint32_t y, const char *s,
                  uint32_t fg, uint32_t bg);
void     fb_print_s(uint32_t x, uint32_t y, const char *s,
                    uint32_t fg, uint32_t bg, int scale);

/* Framebuffer info struct (read-only after init) */
extern fb_info_t fb;   /* .width, .height, .pitch, .bpp, .available */
```

---

## Window Manager (gui/wm/wm.h)

```c
void wm_init(void);
int  wm_create_window(const char *title, int x, int y, int w, int h);
void wm_destroy_window(int id);
void wm_focus_window(int id);       /* raises z-order, marks as focused */
void wm_snap_window(int id, wm_win_state_t snap);
void wm_resize_window(int id, int w, int h);
void wm_move_window(int id, int x, int y);
void wm_switch_desktop(int idx);    /* 0-3 */
int  wm_current_desktop(void);
void wm_task_switcher_open(void);
void wm_task_switcher_next(void);
void wm_task_switcher_commit(void);
void wm_render_frame(void);         /* z-sorted blit to shadow buffer */
void wm_draw_shadow(int x, int y, int w, int h);
```

`wm_render_frame()` must be called from the desktop task render loop — it composites all WM-managed windows for the current desktop onto the shadow buffer in z-order.

---

## Compositor (gui/compositor/compositor.h)

```c
int  comp_create_window(const char *title, uint32_t x, uint32_t y,
                        uint32_t w, uint32_t h);
void comp_destroy_window(int handle);
void comp_move_window(int handle, uint32_t x, uint32_t y);
void comp_focus_window(int handle);    /* raises z, clears focus on others */
void comp_window_print(int handle, uint32_t x, uint32_t y,
                       const char *s, uint32_t fg);
void comp_window_fill(int handle, uint32_t x, uint32_t y,
                      uint32_t w, uint32_t h, uint32_t color);
void comp_render(void);               /* z+desktop-sorted blit, desktop-filtered */
void comp_switch_desktop(int idx);    /* 0-3 */
int  comp_current_desktop(void);
void comp_init(void);
void comp_task(void);                 /* standalone compositor task */
```

---

## Scheduler (kernel/sched/sched.h)

```c
void     sched_init(void);
int      sched_spawn(void (*entry)(void), const char *name);
void     sched_sleep(uint32_t ms);
void     sched_yield(void);
void     sched_exit(void);
task_t  *sched_current(void);
int      sched_get_pid(void);
```

---

## Memory (kernel/mm/)

```c
/* Physical */
void     pmm_init(uint64_t mem_size_bytes);
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t phys_addr);

/* Virtual */
void     vmm_init(void);
void     vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void     vmm_unmap(uint64_t virt);

/* Heap (Ring-0) */
void    *kmalloc(size_t size);
void    *kzalloc(size_t size);   /* zero-initialised */
void     kfree(void *ptr);       /* currently a no-op */
```

---

## Filesystem (kernel/fs/vfs.h)

```c
int   vfs_mount(const char *path, vfs_driver_t *driver);
int   vfs_open(const char *path, int flags);
int   vfs_read(int fd, void *buf, size_t len);
int   vfs_write(int fd, const void *buf, size_t len);
int   vfs_close(int fd);
int   vfs_stat(const char *path, vfs_stat_t *st);
int   vfs_mkdir(const char *path);
int   vfs_unlink(const char *path);
```

---

## Input (kernel/drivers/ps2/)

```c
/* Keyboard */
int      keyboard_getchar(void);      /* 0 = no key ready */

/* Mouse — call mouse_update_edges() ONCE per frame before pressed/released */
void     mouse_update_edges(void);
int      mouse_btn_pressed(uint8_t mask);
int      mouse_btn_released(uint8_t mask);
int      mouse_get_x(void);           /* clamped to [0, fb.width-1] */
int      mouse_get_y(void);           /* clamped to [0, fb.height-1] */
uint8_t  mouse_get_buttons(void);

/* VMware absolute mouse */
void     vmmouse_poll(void);          /* call once per frame */
```

---

## App Manager (apps/appman/appman.h)

```c
void              appman_init(void);
int               appman_register(const char *name, const char *category,
                                  void (*entry)(void));
int               appman_launch(const char *name);   /* spawns task, returns task id */
int               appman_count(void);
const app_entry_t *appman_get(int idx);              /* NULL if out of range */
void              appman_list(char *buf, size_t sz); /* newline-separated names */
```

---

## Logging (kernel/log.h)

```c
void kinfo (const char *fmt, ...);
void kwarn (const char *fmt, ...);
void kerror(const char *fmt, ...);
void kdebug(const char *fmt, ...);
```

Output goes to serial (`serial.c`) and, if framebuffer console is unlocked, to the VGA text overlay. `fb_console_lock(1)` silences FB output (call before desktop starts).

---

## Auth (kernel/security/dracoauth.h)

```c
int         dracoauth_login(const char *user, const char *pass); /* 0 = success */
void        dracoauth_logout(void);
const char *dracoauth_whoami(void);  /* current username, or NULL if not logged in */
```

---

## LXScript Kernel API (kernel/lxs_kernel.h)

```c
void lxs_kernel_init(void);
int  lxs_exec_file(const char *path);   /* run a .lxs file from VFS */
int  lxs_exec_string(const char *src);  /* run source string directly */
```

Kernel bindings exposed to `.lxs` scripts: `print`, `sleep`, `fb_pixel`, `fb_clear`, `vfs_read`, `vfs_write`, `sched_sleep`, `task_spawn`.
