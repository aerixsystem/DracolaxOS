/* gui/apps/apps.c — All 13 built-in DracolaxOS applications
 *
 * Each app runs as a sched task with its own compositor window.
 * Apps that require hardware not yet available (audio, network, PNG decode)
 * open in a window displaying a "coming in V2" notice.
 */
#include "../../kernel/types.h"
#include "../../kernel/klibc.h"
#include "../../kernel/log.h"
#include "../../kernel/drivers/vga/vga.h"
#include "../../kernel/sched/sched.h"
#include "../../kernel/fs/vfs.h"
#include "../../kernel/fs/ramfs.h"
#include "../../kernel/drivers/ps2/keyboard.h"
#include "../../kernel/drivers/ps2/mouse.h"
#include "../../kernel/mm/pmm.h"
#include "../../kernel/mm/vmm.h"
#include "../../kernel/arch/x86_64/pic.h"
#include "../../kernel/drivers/vga/fb.h"
#include "../../kernel/security/dracoauth.h"
#include "../../kernel/limits.h"
#include "../../kernel/security/dracolock.h"
#include "../../kernel/security/dracolicence.h"
#include "../../kernel/security/draco-shield/firewall.h"
#include "../../drx/cli/draco-install.h"
#include "../../gui/compositor/compositor.h"
#include "appman.h"

extern vfs_node_t *ramfs_root;
extern vfs_node_t *storage_root;

/* ---- Helper: open a compositor window or fallback to VGA text ---------- */

#define APP_W  600
#define APP_H  400

static int open_app_window(const char *title, int *win_out) {
    if (fb.available) {
        int w = comp_create_window(title,
            (fb.width  - APP_W) / 2,
            (fb.height - APP_H) / 2,
            APP_W, APP_H);
        *win_out = w;
        return (w >= 0) ? 1 : 0;
    }
    /* VGA fallback: clear screen and print a header */
    vga_clear();
    vga_set_color(VGA_WHITE, VGA_BLUE);
    vga_print("  "); vga_print(title); vga_print("  \n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    *win_out = -1;
    return 0; /* 0 = VGA mode, not FB */
}

/* ---- Shared VGA read line (used by text-mode apps) --------------------- */
static int app_readline(char *buf, int max) {
    int pos = 0;
    while (1) {
        int c = keyboard_read();   /* FIX A5a: int not char — KB_KEY_* >= 0x80 must not sign-extend */
        if (c == '\n' || c == '\r') { buf[pos] = '\0'; vga_putchar('\n'); return pos; }
        if (c == '\b') { if (pos>0){pos--;vga_putchar('\b');} continue; }
        if (c == 0x1B) { buf[0] = '\0'; return -1; }
        if ((unsigned char)c >= 0x20 && pos < max-1) { buf[pos++]=c; vga_putchar(c); }
    }
}

/* ========================================================================
 * 1. Text Editor
 * ======================================================================*/
void app_text_editor(void) {
    int win;
    int fb_mode = open_app_window("Text Editor", &win);

    if (fb_mode && win >= 0) {
        comp_window_fill(win, 0, 0, APP_W, APP_H, fb_color(20, 20, 30));
        comp_window_print(win, 10, 30, "Text Editor", fb_color(200, 200, 255));
        comp_window_print(win, 10, 50, "Filename: ", fb_color(180,180,180));
        /* comp_render() removed: desktop loop composites all windows */
    }

    /* Always use VGA text input for now */
    vga_print("Text Editor\nFilename: ");
    char fname[64];
    app_readline(fname, sizeof(fname));
    if (fname[0] == '\0') goto done;

    /* Load file */
    char ed_buf[4096]; int ed_len = 0;
    vfs_node_t *f = vfs_finddir(ramfs_root, fname);
    if (f) { int n = vfs_read(f, 0, sizeof(ed_buf)-1, (uint8_t*)ed_buf);
             if (n>0) ed_len = n; }
    ed_buf[ed_len] = '\0';

    vga_print("--- Content (^S=save, ^X=quit) ---\n");
    vga_print(ed_buf);
    vga_print("\n--- Append text, then ^S to save ---\n");

    while (1) {
        char line[256];
        int r = app_readline(line, sizeof(line));
        if (r < 0) break; /* ESC */
        if (line[0] == 0x13 - 0x40) break; /* ^S */
        /* Append line to buffer */
        if (ed_len + r + 1 < 4095) {
            strncat(ed_buf, line, sizeof(ed_buf)-ed_len-2);
            ed_buf[ed_len + r] = '\n';
            ed_len += r + 1;
            ed_buf[ed_len] = '\0';
        }
    }

    /* Save */
    if (!f) { ramfs_create(ramfs_root, fname); f = vfs_finddir(ramfs_root, fname); }
    if (f) { vfs_write(f, 0, (uint32_t)ed_len, (const uint8_t*)ed_buf);
             vga_print("Saved.\n"); }

done:
    if (fb_mode && win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 2. File Manager
 * ======================================================================*/
void app_file_manager(void) {
    int win;
    int fb_mode = open_app_window("File Manager", &win);

    if (fb_mode && win >= 0) {
        comp_window_fill(win, 0, 0, APP_W, APP_H, fb_color(15, 15, 25));
        comp_window_print(win, 10, 30, "File Manager", fb_color(200,200,255));
        /* comp_render() removed: desktop loop composites all windows */
    }

    vga_print("File Manager\n");
    vga_print("Locations: /ramfs  /storage  /proc\n\n");

    while (1) {
        vga_print("fm> ");
        char cmd[128];
        int r = app_readline(cmd, sizeof(cmd));
        if (r < 0 || !strcmp(cmd, "exit") || !strcmp(cmd, "quit")) break;

        if (!strcmp(cmd, "ls") || !strcmp(cmd, "")) {
            /* List /ramfs */
            vga_print("/ramfs:\n");
            char name[64];
            for (uint32_t i = 0; ; i++) {
                if (vfs_readdir(ramfs_root, i, name, sizeof(name)) != 0) break;
                vga_print("  "); vga_print(name); vga_putchar('\n');
            }
        } else if (!strncmp(cmd, "open ", 5)) {
            vfs_node_t *f = vfs_finddir(ramfs_root, cmd+5);
            if (!f) { vga_print("Not found.\n"); continue; }
            uint8_t buf[512]; int n = vfs_read(f, 0, sizeof(buf)-1, buf);
            if (n>0){buf[n]='\0';vga_print((char*)buf);}
        } else if (!strncmp(cmd, "del ", 4)) {
            ramfs_delete(ramfs_root, cmd+4);
            vga_print("Deleted.\n");
        } else {
            vga_print("Commands: ls, open <f>, del <f>, exit\n");
        }
    }

    if (fb_mode && win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 3. System Monitor (htop-like)
 * ======================================================================*/
void app_system_monitor(void) {
    int win;
    int fb_mode = open_app_window("System Monitor", &win);

    vga_print("System Monitor (press Q to quit, R to refresh)\n");

    while (1) {
        limits_update();
        uint32_t tkb = (uint32_t)(pmm_total_bytes()/1024);
        uint32_t ukb = pmm_used_pages()*4;
        uint32_t fkb = pmm_free_pages()*4; (void)fkb; (void)fkb;
        uint32_t pct = tkb ? (ukb*100)/tkb : 0;
        uint32_t t   = pit_ticks(), s = t/100, m = s/60, h = m/60;
        s %= 60; m %= 60;

        char buf[512];
        snprintf(buf, sizeof(buf),
            "\n[System Monitor]\n"
            "  Uptime : %02u:%02u:%02u\n"
            "  Memory : %u/%u KB (%u%%) [%s]\n"
            "  Tasks  : %d\n\n",
            h,m,s, ukb,tkb,pct,
            g_limits.mem_deny_active?"CRITICAL":
            g_limits.mem_warn_active?"WARNING":"OK",
            sched_task_count());
        vga_print(buf);

        /* Task table */
        vga_print("  PID  NAME             STATE\n");
        for (int i = 0; i < TASK_MAX; i++) {
            task_t *t2 = sched_task_at(i);
            if (!t2 || t2->state == 0) continue;
            static const char *st[]={"EMPTY","READY","RUN  ","SLEEP","DEAD "};
            snprintf(buf, sizeof(buf), "  %-4u %-16s %s\n",
                     t2->id, t2->name,
                     t2->state<=4?st[t2->state]:"?");
            vga_print(buf);
        }

        /* comp_render() removed: desktop loop composites all windows */

        /* Wait ~1s then refresh, or Q to quit */
        sched_sleep(1000);
        /* Non-blocking key check via keyboard_try — would need keyboard_try()
         * For now we just loop once and exit on escape */
        break;
    }

    vga_print("\nPress any key...\n");
    keyboard_read();

    if (fb_mode && win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 4. Terminal Emulator
 *
 * FIX: The old implementation called shell_run() directly inside app_terminal
 * which runs as a sched task.  shell_run() calls keyboard_read() in a tight
 * loop — while that loop owns the keyboard the desktop task's input path is
 * starved and the screen appears frozen ("shell takes over the whole input
 * flow" — ChatGPT audit, verified).
 *
 * Fix: spawn shell_run() as a NEW independent task so the desktop task keeps
 * running its own event loop.  The terminal task owns the VGA/fb_console
 * output region; the desktop task continues to handle mouse + rendering.
 * app_terminal() itself immediately exits after spawning so the app-manager
 * entry point returns without blocking.
 * ======================================================================*/
static void terminal_shell_task(void) {
    __asm__ volatile ("sti");   /* spawned tasks start with IF=0 */
    extern void shell_run(void);
    vga_print("[Terminal] shell started - type 'exit' to close\n");
    shell_run();
    /* shell_run returns on 'exit' command */
    vga_print("[Terminal] shell exited\n");
    sched_exit();
}

void app_terminal(void) {
    int win;
    open_app_window("Terminal", &win);
    (void)win;   /* window handle tracked by compositor; terminal uses VGA */

    /* Spawn shell as a separate task — does NOT block app_terminal */
    sched_spawn(terminal_shell_task, "terminal-shell");

    /* app_terminal returns immediately; the spawned task runs independently */
    sched_exit();
}

/* ========================================================================
 * 5. Calculator
 * ======================================================================*/
static int calc_eval(const char *expr) {
    /* Simple: parse "A op B" */
    int a = 0, b = 0; char op = 0;
    const char *p = expr;
    while (*p == ' ') p++;
    a = atoi(p);
    while (*p && *p != '+' && *p != '-' && *p != '*' && *p != '/') p++;
    if (*p) { op = *p; p++; b = atoi(p); }
    switch (op) {
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b ? a / b : 0;
    default:  return a;
    }
}

void app_calculator(void) {
    int win;
    open_app_window("Calculator", &win);

    vga_print("Calculator (type expression like '12 + 34', or 'exit')\n");
    while (1) {
        vga_print("calc> ");
        char expr[64];
        int r = app_readline(expr, sizeof(expr));
        if (r < 0 || !strcmp(expr,"exit")) break;
        if (!expr[0]) continue;
        int result = calc_eval(expr);
        char buf[64];
        snprintf(buf, sizeof(buf), "= %d\n", result);
        vga_print(buf);
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 6. Settings
 * ======================================================================*/
void app_settings(void) {
    int win;
    open_app_window("Settings", &win);

    vga_print("Settings\n");
    vga_print("  1. Volume\n  2. Brightness\n  3. User info\n  4. Device ID\n  5. Exit\n\n");

    while (1) {
        vga_print("settings> ");
        char line[32]; app_readline(line, sizeof(line));
        if (!strcmp(line,"1")||!strncmp(line,"vol",3)) {
            char b[32]; snprintf(b,sizeof(b),"Volume: %u%% (use 'volume <n>' in shell)\n",
                         limits_get_volume()); vga_print(b);
        } else if (!strcmp(line,"2")) {
            char b[32]; snprintf(b,sizeof(b),"Brightness: %u%% (use 'brightness <n>')\n",
                         g_limits.brightness_pct); vga_print(b);
        } else if (!strcmp(line,"3")) {
            vga_print("User: "); vga_print(dracoauth_whoami()); vga_putchar('\n');
        } else if (!strcmp(line,"4")) {
            vga_print("Device: "); vga_print(dracolicence_device_id()); vga_putchar('\n');
            vga_print("Licence: "); vga_print(dracolicence_licence_id()); vga_putchar('\n');
        } else if (!strcmp(line,"5")||!strcmp(line,"exit")||line[0]==0x1B) {
            break;
        } else {
            vga_print("Options: 1-5\n");
        }
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 7. Package Manager
 * ======================================================================*/
void app_pkg_manager(void) {
    int win;
    open_app_window("Package Manager", &win);

    vga_print("Package Manager (Draco Install)\n");
    vga_print("Commands: list, install <f>, remove <n>, approved, exit\n\n");

    while (1) {
        vga_print("pkg> ");
        char line[128]; int r = app_readline(line, sizeof(line));
        if (r < 0 || !strcmp(line,"exit")) break;
        if (!line[0]) continue;

        /* Build argv and call draco_install_run */
        char *argv[8]; int argc = 0;
        argv[argc++] = (char*)"draco";
        char *p = line;
        while (*p && argc < 8) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        draco_install_run(argc, argv);
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 8. Draco Shield UI
 * ======================================================================*/
void app_shield_ui(void) {
    int win;
    open_app_window("Draco Shield", &win);

    vga_print("Draco Shield — Firewall\n");
    vga_print("Commands: list, reset, allow <ip>, deny <ip>, allow-port <n>, deny-port <n>, exit\n\n");

    while (1) {
        vga_print("shield> ");
        char line[128]; int r = app_readline(line, sizeof(line));
        if (r < 0 || !strcmp(line,"exit")) break;
        if (!line[0]) continue;
        char *argv[8]; int argc = 0;
        argv[argc++] = (char*)"shield";
        char *p = line;
        while (*p && argc < 8) {
            while (*p == ' ') p++;
            if (!*p) break;
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
        extern void shieldctl_run(int, char**);
        shieldctl_run(argc, argv);
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 9. Draco Manager
 * ======================================================================*/
void app_draco_manager(void) {
    int win;
    open_app_window("Draco Manager", &win);

    vga_print("Draco Manager\n");
    vga_print("  1. System Info\n"
              "  2. Security Status\n"
              "  3. User Management\n"
              "  4. Device Licence\n"
              "  5. Memory Limits\n"
              "  6. Shutdown\n"
              "  0. Exit\n\n");

    while (1) {
        vga_print("draco-mgr> ");
        char line[32]; app_readline(line, sizeof(line));
        char buf[512];
        if (!strcmp(line,"1")) {
            uint32_t tkb = (uint32_t)(pmm_total_bytes()/1024);
            uint32_t t   = pit_ticks(), s=t/100, m=s/60, h=m/60; s%=60; m%=60;
            snprintf(buf, sizeof(buf),
                "OS: DracolaxOS v1.0  Kernel: Draco-1.0 x86_64\n"
                "Uptime: %02u:%02u:%02u  RAM: %u KB\n",
                h,m,s,tkb);
            vga_print(buf);
        } else if (!strcmp(line,"2")) {
            snprintf(buf, sizeof(buf),
                "Auth:     %s\nHW verify: %s\nContext:  %u\n",
                g_session.logged_in ? "LOGGED IN" : "NOT LOGGED IN",
                g_lock.hw_verified  ? "OK" : "FAILED",
                g_lock.context);
            vga_print(buf);
        } else if (!strcmp(line,"3")) {
            dracoauth_list_users(buf, sizeof(buf)); vga_print(buf);
        } else if (!strcmp(line,"4")) {
            snprintf(buf, sizeof(buf), "Device:  %s\nLicence: %s\nValid:   %s\n",
                dracolicence_device_id(), dracolicence_licence_id(),
                g_licence.valid ? "yes" : "no");
            vga_print(buf);
        } else if (!strcmp(line,"5")) {
            limits_print_status();
        } else if (!strcmp(line,"6")) {
            vga_print("Shutdown: use 'halt' in shell.\n");
        } else if (!strcmp(line,"0")||!strcmp(line,"exit")) {
            break;
        }
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 10. Login Manager
 * ======================================================================*/
void app_login_manager(void) {
    int win;
    int fb_mode = open_app_window("DracolaxOS Login", &win);

    if (fb_mode && win >= 0) {
        comp_window_fill(win, 0, 0, APP_W, APP_H, fb_color(10, 10, 20));
        comp_window_print(win, APP_W/2 - 60, 60, "DracolaxOS v1.0", fb_color(180, 120, 255));
        comp_window_print(win, APP_W/2 - 40, 100, "Login", fb_color(200, 200, 255));
        /* comp_render() removed: desktop loop composites all windows */
    }

    /* VGA login */
    vga_set_color(VGA_LIGHT_MAGENTA, VGA_BLACK);
    vga_print("\n  ===  DracolaxOS v1.0  ===\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    int attempts = 0;
    while (attempts < 3) {
        char uname[32], pass[64];
        vga_print("  Username: ");
        app_readline(uname, sizeof(uname));
        vga_print("  Password: ");
        /* read without echo */
        int pl = 0;
        while (pl < 63) {
            int c = keyboard_read();   /* FIX A5b: int not char — special keys must not sign-extend */
            if (c == '\n' || c == '\r') { pass[pl] = '\0'; vga_putchar('\n'); break; }
            if (c == '\b') { if(pl>0)pl--; continue; }
            if ((unsigned char)c >= 0x20) pass[pl++] = c;
        }
        if (dracoauth_login(uname, pass) == 0) {
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            vga_print("\n  Login successful. Welcome, ");
            vga_print(uname); vga_print("!\n\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            if (win >= 0) comp_destroy_window(win);
            sched_exit();
            return;
        }
        attempts++;
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_print("  Incorrect credentials. ");
        char b[16]; snprintf(b, sizeof(b), "(%d/3)\n", attempts);
        vga_print(b);
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
    vga_print("  Too many attempts. System locked.\n");
    dracolock_lock_screen();

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 11. Paint
 *
 * FIX: Mouse drawing is now wired.  The app uses mouse_get_x/Y() and
 * mouse_btn_pressed(MOUSE_BTN_LEFT) to paint pixels on the canvas.
 * Colour is selected by clicking the palette at the bottom.
 * Q / Esc exits.
 * ======================================================================*/
void app_paint(void) {
    int win;
    open_app_window("Paint", &win);

    if (!fb.available) {
        vga_print("Paint: Requires VESA framebuffer.\n");
        keyboard_read();
        if (win >= 0) comp_destroy_window(win);
        sched_exit();
        return;
    }

    /* Canvas region on framebuffer */
    uint32_t ox = (fb.width  - APP_W) / 2;
    uint32_t oy = (fb.height - APP_H) / 2;

    static const uint32_t palette[] = {
        0xFF0000u, 0x00CC00u, 0x0055FFu,
        0xFFEE00u, 0xFF00FFu, 0x00FFFFu,
        0xFF8800u, 0x883300u,
        0x000000u, 0xFFFFFFu
    };
    static const int PAL_COUNT = 10;
    uint32_t cur_col = 0x000000u;
    int      brush   = 3;   /* radius in pixels */

    /* Clear canvas */
    fb_fill_rect(ox, oy, APP_W, APP_H, 0xFFFFFFu);

    /* Title bar */
    fb_fill_rect(ox, oy, APP_W, 24, 0x7828C8u);
    fb_print(ox+8, oy+4, "Paint  [Q] quit  [+/-] brush size", 0xF0F0FFu, 0x7828C8u);

    /* Palette row */
    uint32_t pal_y = oy + APP_H - 44;
    fb_fill_rect(ox, pal_y - 4, APP_W, 48, 0x1A1D3Au);
    for (int i = 0; i < PAL_COUNT; i++) {
        uint32_t px2 = ox + 8 + (uint32_t)i * 44;
        fb_fill_rect(px2, pal_y, 38, 36, palette[i]);
        fb_rounded_rect(px2, pal_y, 38, 36, 4, 0x3A3F7Au);
    }
    fb_flip();

    int running = 1;
    while (running) {
        sched_sleep(20);

        /* Mouse input */
        mouse_update_edges();
        int mx = mouse_get_x(), my = mouse_get_y();

        int c = keyboard_getchar();
        if (c == 'q' || c == 'Q' || c == 0x1B) { running = 0; break; }
        if ((c == '+' || c == '=') && brush < 20) brush++;
        if ((c == '-' || c == '_') && brush > 1)  brush--;

        if (mouse_btn_pressed(MOUSE_BTN_LEFT)) {
            int lx = mx - (int)ox, ly = my - (int)oy;

            /* Palette click */
            if (ly >= (int)(pal_y - oy) - 4 && ly < (int)APP_H) {
                for (int i = 0; i < PAL_COUNT; i++) {
                    int px2 = 8 + i * 44;
                    if (lx >= px2 && lx < px2 + 38)
                        cur_col = palette[i];
                }
            } else if (lx >= 0 && ly >= 24 && lx < (int)APP_W &&
                       ly < (int)(pal_y - oy) - 4) {
                /* Draw on canvas */
                for (int dy = -brush; dy <= brush; dy++)
                    for (int dx = -brush; dx <= brush; dx++)
                        if (dx*dx + dy*dy <= brush*brush)
                            fb_put_pixel((uint32_t)(mx+dx), (uint32_t)(my+dy), cur_col);
                /* Redraw palette + title so they stay on top */
                fb_fill_rect(ox, oy, APP_W, 24, 0x7828C8u);
                fb_print(ox+8, oy+4, "Paint  [Q] quit  [+/-] brush size",
                         0xF0F0FFu, 0x7828C8u);
                for (int i = 0; i < PAL_COUNT; i++) {
                    uint32_t px2 = ox + 8 + (uint32_t)i * 44;
                    fb_fill_rect(px2, pal_y, 38, 36, palette[i]);
                    if (palette[i] == cur_col)
                        fb_rounded_rect(px2, pal_y, 38, 36, 4, 0xFFFFFFu);
                    else
                        fb_rounded_rect(px2, pal_y, 38, 36, 4, 0x3A3F7Au);
                }
                fb_flip();
            }
        }
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 12. Image Viewer
 *
 * Full PNG/JPEG decoding requires a V2 decoder library.  For V1 we show
 * a procedural colour test chart (gradients + colour bars) so the app is
 * actually useful for display-calibration and proves the compositor works.
 * ======================================================================*/
void app_image_viewer(void) {
    int win;
    int fb_mode = open_app_window("Image Viewer", &win);

    if (fb_mode && fb.available) {
        /* Centre of framebuffer for the test chart */
        uint32_t ox = (fb.width  - APP_W) / 2;
        uint32_t oy = (fb.height - APP_H) / 2;

        /* Background */
        fb_fill_rect(ox, oy, APP_W, APP_H, 0x0A0A14u);

        /* Title bar */
        fb_fill_rect(ox, oy, APP_W, 24, 0x2878C8u);
        fb_print(ox + 8, oy + 4, "Image Viewer — Colour Test Chart", 0xF0F0FFu, 0x2878C8u);
        fb_print(ox + 8, oy + 28, "(PNG/JPEG decoder pending — showing test pattern)", 0x6060A0u, 0x0A0A14u);

        /* Horizontal gradient bar */
        for (uint32_t dx = 0; dx < (uint32_t)APP_W - 16; dx++) {
            uint8_t t = (uint8_t)(dx * 255 / (APP_W - 16));
            fb_fill_rect(ox + 8 + dx, oy + 52, 1, 40, fb_color(t, 0, 255 - t));
        }
        fb_print(ox + 8, oy + 96, "Hue gradient", 0x8080C0u, 0x0A0A14u);

        /* Luminance ramp */
        for (uint32_t dx = 0; dx < (uint32_t)APP_W - 16; dx++) {
            uint8_t t = (uint8_t)(dx * 255 / (APP_W - 16));
            fb_fill_rect(ox + 8 + dx, oy + 116, 1, 30, fb_color(t, t, t));
        }
        fb_print(ox + 8, oy + 150, "Luminance ramp", 0x8080C0u, 0x0A0A14u);

        /* RGBCMYW colour bars */
        uint32_t bar_cols[] = {
            0xFF0000u, 0x00FF00u, 0x0000FFu,
            0x00FFFFu, 0xFF00FFu, 0xFFFF00u, 0xFFFFFFu
        };
        uint32_t bar_w = ((uint32_t)APP_W - 16) / 7;
        for (int i = 0; i < 7; i++) {
            fb_fill_rect(ox + 8 + (uint32_t)i * bar_w, oy + 170,
                         bar_w - 2, 50, bar_cols[i]);
        }
        fb_print(ox + 8, oy + 224, "Colour bars: R G B C M Y W", 0x8080C0u, 0x0A0A14u);

        /* Checkerboard patch */
        for (uint32_t cy2 = 0; cy2 < 40; cy2++)
            for (uint32_t cx2 = 0; cx2 < 120; cx2++) {
                uint32_t c2 = ((cx2 / 10) + (cy2 / 10)) & 1 ? 0xFFFFFFu : 0x000000u;
                fb_put_pixel(ox + 8 + cx2, oy + 244 + cy2, c2);
            }
        fb_print(ox + 8, oy + 288, "Press any key to close", 0x404060u, 0x0A0A14u);

        fb_flip();
    } else {
        vga_print("Image Viewer: Requires VESA framebuffer.\n");
        vga_print("Boot with 'DracolaxOS V1 (Graphical)' to use Image Viewer.\n");
    }

    keyboard_read();
    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}

/* ========================================================================
 * 13. Media Player
 *
 * AC97/HDA audio driver is a V2 item.  For V1 we show an animated
 * spectrum-analyser visualisation driven by a pseudo-random waveform so
 * the window looks alive and proves the compositor composites correctly.
 * Volume +/- keys update the audio service volume state.
 * ======================================================================*/
void app_media_player(void) {
    int win;
    open_app_window("Media Player", &win);

    if (!fb.available) {
        vga_print("Media Player: Requires VESA framebuffer.\n");
        keyboard_read();
        if (win >= 0) comp_destroy_window(win);
        sched_exit();
        return;
    }

    extern uint32_t audio_get_volume(void);
    extern void     audio_set_volume(uint32_t);
    extern void     audio_mute(int);
    extern int      audio_is_muted(void);

    uint32_t ox = (fb.width  - APP_W) / 2;
    uint32_t oy = (fb.height - APP_H) / 2;
    uint32_t vol = audio_get_volume();
    int      running = 1;
    uint32_t frame   = 0;

    while (running) {
        /* Clear */
        fb_fill_rect(ox, oy, APP_W, APP_H, 0x07070Fu);
        fb_fill_rect(ox, oy, APP_W, 24, 0x7828C8u);
        fb_print(ox + 8, oy + 4, "Media Player", 0xF0F0FFu, 0x7828C8u);

        /* Status line */
        char status[80];
        snprintf(status, sizeof(status),
                 "Vol: %u%%  %s  [+/-] vol  [M] mute  [Q] quit",
                 vol, audio_is_muted() ? "[MUTED]" : "       ");
        fb_print(ox + 8, oy + 28, status, 0xA0A0C8u, 0x07070Fu);

        /* Pseudo-spectrum bars — 32 bands, amplitude from sin-ish prng */
        fb_print(ox + 8, oy + 52, "Spectrum (no audio driver — simulated)", 0x404068u, 0x07070Fu);
        uint32_t bar_w = ((uint32_t)APP_W - 16) / 32;
        for (int b = 0; b < 32; b++) {
            /* Simple deterministic "noise" that changes each frame */
            uint32_t seed = (uint32_t)(b * 137 + frame * 31);
            seed ^= seed >> 13; seed *= 0x45d9f3b; seed ^= seed >> 15;
            uint32_t amp = 20 + (seed % 160);
            uint32_t bx  = ox + 8 + (uint32_t)b * bar_w;
            uint32_t by  = oy + 220 - amp;
            /* Colour: cool blue → warm purple based on amplitude */
            uint8_t  r2   = (uint8_t)(amp * 180 / 180);
            uint8_t  g2   = 20;
            uint8_t  b2   = (uint8_t)(255 - amp);
            fb_fill_rect(bx, by, bar_w - 2, amp, fb_color(r2, g2, b2));
        }

        /* Volume bar */
        fb_print(ox + 8, oy + 240, "Volume:", 0x8080C0u, 0x07070Fu);
        uint32_t vol_w = (uint32_t)((APP_W - 90) * vol / 100);
        fb_fill_rect(ox + 70, oy + 240, APP_W - 90, 12, 0x1A1A2Au);
        fb_fill_rect(ox + 70, oy + 240, vol_w,      12,
                     audio_is_muted() ? 0x444444u : 0x7828C8u);

        fb_print(ox + 8, oy + APP_H - 20,
                 "[+] Vol up   [-] Vol dn   [M] Mute   [Q] Quit",
                 0x404060u, 0x07070Fu);

        fb_flip();
        frame++;
        sched_sleep(80);   /* ~12 fps animation */

        /* Input */
        int c = keyboard_getchar();
        if (c == 'q' || c == 'Q' || c == 0x1B) running = 0;
        if (c == '+' || c == '=') { if (vol < 100) { vol += 5; audio_set_volume(vol); } }
        if (c == '-' || c == '_') { if (vol > 0)   { vol -= 5; audio_set_volume(vol); } }
        if (c == 'm' || c == 'M') audio_mute(!audio_is_muted());
    }

    if (win >= 0) comp_destroy_window(win);
    sched_exit();
}
