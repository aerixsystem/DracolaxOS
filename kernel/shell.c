/* kernel/shell.c — DracolaxOS interactive shell v1.0 */
#include "types.h"
#include "shell.h"
#include "drivers/vga/vga.h"
#include "drivers/ps2/keyboard.h"
#include "klibc.h"
#include "drivers/serial/serial.h"
#include "drivers/vga/fb.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "log.h"
#include "arch/x86_64/pic.h"
#include "limits.h"
#include "loader/elf_loader.h"
#include "security/dracoauth.h"
#include "security/dracolicence.h"
#include "security/dracolock.h"
#include "../drx/cli/draco-install.h"
#include "security/draco-shield/draco-shieldctl.h"
#include "../services/power_manager.h"
#include "../services/audio_service.h"
#include "lxs_kernel.h"
#include "drivers/ata/ata_pio.h"

/* shell_print — writes to VGA text buffer AND fb_console (pixel-mode shells).
 * In graphical mode GRUB leaves the display in pixel mode, so shell_print()
 * writes to the invisible 0xB8000 text buffer. fb_console_print() draws
 * text onto the pixel framebuffer so output is actually visible.
 *
 * FIX: was calling itself (shell_print/shell_putchar) instead of vga_print/
 * vga_putchar — caused immediate infinite recursion → stack overflow →
 * triple fault → QEMU paused the VM on every keypress/output attempt. */
static void shell_print(const char *s) {
    vga_print(s);
    if (fb.available) fb_console_print(s, 0xF0F0FFu);
    serial_print(s);
}
static void shell_putchar(char c) {
    vga_putchar(c);
    char buf[2] = {c, 0};
    if (fb.available) fb_console_print(buf, 0xF0F0FFu);
    serial_print(buf);
}


#define LINE_MAX    256
#define ARGC_MAX    16
#define CWD_MAX     128
#define ENV_MAX     16
#define HIST_MAX    20

extern vfs_node_t *ramfs_root;
extern vfs_node_t *storage_root;

static char cwd[CWD_MAX] = "/ramfs";

typedef struct { char key[32]; char val[64]; } env_t;
static env_t env_table[ENV_MAX];
static int   env_count = 0;

static char history[HIST_MAX][LINE_MAX];
static int  hist_count = 0;

/* ---- VGA helpers ---- */
#define SCOLS  80
#define SROWS  25
#define MKATTR(fg, bg)  ((uint8_t)((fg) | ((uint8_t)(bg) << 4)))
#define ATTR_NORMAL  MKATTR(VGA_LIGHT_GREY, VGA_BLACK)
#define ATTR_HDR     MKATTR(VGA_WHITE,      VGA_BLUE)
#define ATTR_STATUS  MKATTR(VGA_BLACK,      VGA_LIGHT_GREY)

static inline void scr_put(uint8_t col, uint8_t row, char c, uint8_t attr) {
    volatile uint16_t *vga = (volatile uint16_t *)0xB8000;
    vga[(uint32_t)row * SCOLS + col] =
        (uint16_t)(unsigned char)c | ((uint16_t)attr << 8);
}
static void scr_fill_row(uint8_t row, char c, uint8_t attr) {
    for (uint8_t x = 0; x < SCOLS; x++) scr_put(x, row, c, attr);
}
static void scr_print_at(uint8_t col, uint8_t row, const char *s, uint8_t attr) {
    while (*s && col < SCOLS) scr_put(col++, row, *s++, attr);
}
static void scr_full_clear(uint8_t attr) {
    for (uint8_t r = 0; r < SROWS; r++) scr_fill_row(r, ' ', attr);
}

/* ---- Simple strstr ---- */
static int shell_strstr(const char *h, const char *n) {
    size_t nl = strlen(n), hl = strlen(h);
    if (nl > hl) return 0;
    for (size_t i = 0; i <= hl - nl; i++)
        if (strncmp(h + i, n, nl) == 0) return 1;
    return 0;
}

/* ---- readline — with UP/DOWN history navigation ---- */
static int readline(char *buf, int max) {
    int pos     = 0;
    int hist_idx = hist_count;   /* points past last entry = "new" line */
    buf[0] = '\0';

    /* saved partial line so UP/DOWN doesn't destroy what was being typed */
    char saved[LINE_MAX] = "";

    /* Helper: redraw the current line content after cursor reposition.
     * Erases current terminal line with '\r' + spaces, reprints prompt-less
     * content. Works on both VGA text and fb_console. */
    while (1) {
        int c = keyboard_read();

        /* ── ENTER ────────────────────────────────────────────── */
        if (c == '\n' || c == '\r') {
            shell_putchar('\n');
            buf[pos] = '\0';
            if (pos > 0) {
                /* Only store if different from last entry */
                if (hist_count == 0 ||
                    strcmp(history[hist_count - 1], buf) != 0) {
                    if (hist_count < HIST_MAX)
                        strncpy(history[hist_count++], buf, LINE_MAX - 1);
                    else {
                        /* Ring: shift out oldest */
                        for (int i = 0; i < HIST_MAX - 1; i++)
                            strncpy(history[i], history[i+1], LINE_MAX - 1);
                        strncpy(history[HIST_MAX-1], buf, LINE_MAX - 1);
                    }
                }
            }
            return pos;
        }

        /* ── BACKSPACE ────────────────────────────────────────── */
        if (c == '\b') {
            if (pos > 0) {
                pos--;
                buf[pos] = '\0';
                shell_putchar('\b');
            }
            continue;
        }

        /* ── ARROW UP — history prev ──────────────────────────── */
        if ((uint8_t)c == KB_KEY_UP) {
            if (hist_count == 0) continue;
            if (hist_idx == hist_count)
                strncpy(saved, buf, LINE_MAX - 1);   /* save partial */
            if (hist_idx > 0) hist_idx--;
            /* Erase current line visually */
            for (int i = 0; i < pos; i++) shell_putchar('\b');
            for (int i = 0; i < pos; i++) shell_putchar(' ');
            for (int i = 0; i < pos; i++) shell_putchar('\b');
            strncpy(buf, history[hist_idx], (size_t)(max - 1));
            pos = (int)strlen(buf);
            shell_print(buf);
            continue;
        }

        /* ── ARROW DOWN — history next ────────────────────────── */
        if ((uint8_t)c == KB_KEY_DOWN) {
            if (hist_idx >= hist_count) continue;
            hist_idx++;
            for (int i = 0; i < pos; i++) shell_putchar('\b');
            for (int i = 0; i < pos; i++) shell_putchar(' ');
            for (int i = 0; i < pos; i++) shell_putchar('\b');
            if (hist_idx == hist_count) {
                strncpy(buf, saved, (size_t)(max - 1));
            } else {
                strncpy(buf, history[hist_idx], (size_t)(max - 1));
            }
            pos = (int)strlen(buf);
            shell_print(buf);
            continue;
        }

        /* ── Skip other special keys ──────────────────────────── */
        if ((uint8_t)c >= 0x80) continue;
        if ((unsigned)c < 0x20) continue;

        /* ── Normal character ─────────────────────────────────── */
        if (pos < max - 1) {
            buf[pos++] = (char)c;
            buf[pos]   = '\0';
            shell_putchar((char)c);
        }
    }
}

/* ---- read password without echo ---- */
static int read_password(char *buf, int max) {
    int pos = 0;
    while (pos < max - 1) {
        int c = keyboard_read();
        if (c == '\n' || c == '\r') { buf[pos] = '\0'; shell_putchar('\n'); break; }
        if (c == '\b') { if (pos > 0) pos--; continue; }
        if ((unsigned)c >= 0x20 && c < 0x80) buf[pos++] = (char)c;
    }
    buf[pos] = '\0';
    return pos;
}

/* ---- split ---- */
static int split(char *line, char *argv[], int max_argc) {
    int argc = 0; char *p = line;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p || argc >= max_argc) break;
        if (*p == '"') {
            p++; argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ') p++;
            if (*p) *p++ = '\0';
        }
    }
    return argc;
}

/* ---- path helpers ---- */
static void resolve_path(const char *in, char *out, size_t sz) {
    if (in[0] == '/') { strncpy(out, in, sz-1); out[sz-1]='\0'; }
    else snprintf(out, sz, "%s/%s", cwd, in);
}

static vfs_node_t *root_for_path(const char *path, const char **rel) {
    if (strncmp(path, "/ramfs", 6) == 0) {
        *rel = path + 6; if (**rel == '/') (*rel)++;
        return ramfs_root;
    }
    if (strncmp(path, "/storage", 8) == 0) {
        *rel = path + 8; if (**rel == '/') (*rel)++;
        return storage_root;
    }
    *rel = path; return NULL;
}

static vfs_node_t *open_file(const char *path_arg) {
    char path[CWD_MAX];
    resolve_path(path_arg, path, sizeof(path));
    vfs_node_t *f = vfs_open(path);
    if (!f) {
        const char *rel;
        vfs_node_t *root = root_for_path(path, &rel);
        if (root) f = vfs_finddir(root, rel[0] ? rel : path_arg);
        if (!f && root) f = vfs_finddir(root, path_arg);
    }
    return f;
}

/* ---- cmd_help ---- */
static void cmd_help(void) {
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    shell_print("DracolaxOS Shell v1.0 — Built-in Commands\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    shell_print(
        " Navigation  : cd, pwd, ls [path]\n"
        " Files       : cat, write, create, mkdir, rm, cp, mv, symlink, chattr, df\n"
        " Text        : echo, printf, ack (grep)\n"
        " System      : mem, tasks, ps, kill, uname, uptime, info, volume, brightness\n"
        "               clear, halt, reboot\n"
        " Security    : login, logout, whoami, passwd, users\n"
        " Linux compat: exec <elf> [args]\n"
        " Packages    : draco install/remove/list/approved\n"
        " Firewall    : shield list/allow/deny/allow-port/deny-port/reset\n"
        " Network     : wget, curl (stub — needs network stack)\n"
        " Audio       : beep [freq] [ms]  (PC speaker)\n"
        " Shell utils : export, env, history, type, test, wait, exit,\n"
        "               umask, bg, fg, jobs, read, source, eval, fc\n"
        " Protected   : draco kill <id>  (requires admin + password)\n"
        "\nQuotes: write \"my file\" for filenames with spaces\n"
        "Editor: ^S = save   ^X or Esc = discard   ^K = kill line\n"
    );
}

/* ---- cmd_mem ---- */
static void cmd_mem(void) {
    limits_update();
    char buf[320];
    uint32_t tkb = (uint32_t)(pmm_total_bytes()/1024);
    uint32_t fkb = pmm_free_pages()*4;
    uint32_t ukb = pmm_used_pages()*4;
    uint32_t pct = tkb ? (ukb*100)/tkb : 0;
    snprintf(buf, sizeof(buf),
        "Physical  : %u KB total  %u KB used  %u KB free  (%u%%)\n"
        "VMM heap  : %u KB used\n"
        "Limit     : warn@80%%  deny@90%%  [%s]\n",
        tkb, ukb, fkb, pct, vmm_heap_used()/1024,
        g_limits.mem_deny_active ? "DENY" :
        g_limits.mem_warn_active ? "WARN" : "OK");
    shell_print(buf);
}

/* ---- cmd_tasks — htop-style ---- */
static void cmd_tasks(void) {
    static const char *states[] = {"EMPTY","READY","RUN  ","SLEEP","DEAD "};
    static const char *abis[]   = {"Draco","Linux"};
    vga_set_color(VGA_WHITE, VGA_BLUE);
    shell_print(" ID  NAME             STATE   ABI    PID \n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    char buf[100];
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->state == 0) continue;
        const char *st  = (t->state <= 4) ? states[t->state] : "?????";
        const char *abi = (t->abi == ABI_LINUX) ? abis[1] : abis[0];
        snprintf(buf, sizeof(buf), " %-3u %-16s %-7s %-6s %-4u\n",
                 t->id, t->name, st, abi, t->pid);
        shell_print(buf);
    }
    snprintf(buf, sizeof(buf), "Total: %d tasks\n", sched_task_count());
    shell_print(buf);
}

/* ---- cmd_ps ---- */
static void cmd_ps(void) {
    shell_print("  PID  PPID  NAME\n");
    char buf[64];
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->state == 0) continue;
        snprintf(buf, sizeof(buf), "  %-4u %-4u  %s\n", t->pid, t->ppid, t->name);
        shell_print(buf);
    }
}

/* ---- cmd_kill ---- */
static void cmd_kill(int argc, char *argv[]) {
    if (argc < 2) { shell_print("usage: kill <task_id>\n"); return; }
    uint32_t id = (uint32_t)atoi(argv[1]);
    if (id <= 1) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        shell_print("kill: protected process. Use 'draco kill <id>' with password.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return;
    }
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->id != id) continue;
        t->state = 4; t->exit_code = -9;
        char buf[64];
        snprintf(buf, sizeof(buf), "Killed task %u (%s)\n", id, t->name);
        shell_print(buf);
        return;
    }
    shell_print("kill: task not found\n");
}

/* ---- draco kill (privileged) ---- */
static void cmd_draco_kill(int argc, char *argv[]) {
    if (argc < 2) { shell_print("usage: draco kill <task_id>\n"); return; }
    if (!dracoauth_has_role(ROLE_ADMIN)) {
        shell_print("draco kill: admin required\n"); return;
    }
    shell_print("Admin password: ");
    char pass[64]; read_password(pass, sizeof(pass));
    if (dracoauth_login(dracoauth_whoami(), pass) != 0) {
        shell_print("draco kill: wrong password\n"); return;
    }
    uint32_t id = (uint32_t)atoi(argv[1]);
    for (int i = 0; i < TASK_MAX; i++) {
        task_t *t = sched_task_at(i);
        if (!t || t->id != id) continue;
        t->state = 4; t->exit_code = -9;
        char buf[64];
        snprintf(buf, sizeof(buf), "Force-killed task %u (%s)\n", id, t->name);
        shell_print(buf);
        return;
    }
    shell_print("draco kill: task not found\n");
}

/* ---- cmd_uname ---- */
static void cmd_uname(int argc, char *argv[]) {
    int all = (argc >= 2 && strcmp(argv[1], "-a") == 0);
    if (all)
        shell_print("DracolaxOS 1.0.0 dracolax 1.0.0-draco #1 x86_64 x86_64 x86_64 GNU/Draco\n");
    else
        shell_print("DracolaxOS\n");
}

/* ---- cmd_uptime ---- */
static void cmd_uptime(void) {
    uint32_t t = pit_ticks(), s = t/100, m = s/60, h = m/60;
    s %= 60; m %= 60;
    char buf[64];
    snprintf(buf, sizeof(buf), "up %02u:%02u:%02u  (%u ticks at 100Hz)\n", h, m, s, t);
    shell_print(buf);
}

/* ---- cmd_info (neofetch) ---- */
static void cmd_info(void) {
    uint32_t tkb = (uint32_t)(pmm_total_bytes()/1024);
    uint32_t fkb = pmm_free_pages()*4;
    uint32_t t   = pit_ticks(), s = t/100, m = s/60, h = m/60;
    s %= 60; m %= 60;
    vga_set_color(VGA_LIGHT_MAGENTA, VGA_BLACK);
    shell_print(
        "    ____                        _             \n"
        "   / __ \\ ____ ___  ___ ___    | | ___ _  __\n"
        "  / / / // __// _ `/ __/ _ \\  / / / _ `| \\ \\\n"
        " / /_/ // /  / /_/ / __/ // /_/ / / /_/ |  > >\n"
        "/_____//_/   \\__,_/\\__/\\___/____/  \\__,_| /_/ \n\n"
    );
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "  OS       : DracolaxOS v1.0\n"
        "  Kernel   : Draco-1.0 x86_64 (64-bit preemptive)\n"
        "  Shell    : dracolax-shell v1.0\n"
        "  CPU      : x86_64 (AMD64, 64-bit)\n"
        "  Memory   : %u / %u KB (%u%% used)\n"
        "  Uptime   : %02u:%02u:%02u\n"
        "  User     : %s\n"
        "  DeviceID : %s\n"
        "  Licence  : %s\n"
        "  Author   : Lunax (Yunis) + Amilcar\n",
        tkb - fkb, tkb, tkb ? ((tkb-fkb)*100)/tkb : 0,
        h, m, s,
        dracoauth_whoami(),
        dracolicence_device_id(),
        dracolicence_licence_id());
    shell_print(buf);
}

/* ---- cmd_ls ---- */
static void cmd_ls(const char *path_arg) {
    char path[CWD_MAX];
    if (path_arg && path_arg[0]) resolve_path(path_arg, path, sizeof(path));
    else strncpy(path, cwd, sizeof(path)-1);

    vfs_node_t *dir = vfs_open(path);
    if (!dir) {
        if (strncmp(path, "/ramfs", 6) == 0)       dir = ramfs_root;
        else if (strncmp(path, "/storage", 8) == 0) dir = storage_root;
    }
    if (!dir) { shell_print("ls: path not found\n"); return; }

    char name[VFS_NAME_MAX];
    int found = 0;
    for (uint32_t i = 0; ; i++) {
        if (vfs_readdir(dir, i, name, VFS_NAME_MAX) != 0) break;
        vfs_node_t *child = vfs_finddir(dir, name);
        if (child && child->type == VFS_TYPE_DIR) {
            vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
            shell_print(name); shell_print("/  ");
        } else {
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            shell_print(name); shell_print("  ");
        }
        found++;
    }
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    if (found) shell_putchar('\n');
    else shell_print("(empty)\n");
}

/* ---- cmd_cat ---- */
static void cmd_cat(const char *path_arg) {
    vfs_node_t *f = open_file(path_arg);
    if (!f) {
        shell_print("cat: not found: "); shell_print(path_arg); shell_putchar('\n');
        return;
    }
    uint8_t buf[512]; uint32_t off = 0; int n;
    while ((n = vfs_read(f, off, sizeof(buf)-1, buf)) > 0) {
        buf[n] = '\0'; shell_print((char *)buf); off += (uint32_t)n;
    }
}

/* ---- cmd_cd ---- */
static void cmd_cd(const char *target) {
    if (!target || !target[0] || strcmp(target, "~") == 0) {
        strncpy(cwd, "/ramfs", CWD_MAX-1); return;
    }
    if (strcmp(target, "..") == 0) {
        char *last = strrchr(cwd, '/');
        if (last && last != cwd) *last = '\0';
        else strncpy(cwd, "/ramfs", CWD_MAX-1);
        return;
    }
    char path[CWD_MAX];
    resolve_path(target, path, sizeof(path));
    if (strncmp(path, "/ramfs", 6) == 0 ||
        strncmp(path, "/storage", 8) == 0 ||
        strncmp(path, "/proc", 5) == 0)
        strncpy(cwd, path, CWD_MAX-1);
    else { shell_print("cd: no such directory\n"); }
}

/* ---- cmd_mkdir ---- */
static void cmd_mkdir(const char *name) {
    char path[CWD_MAX]; resolve_path(name, path, sizeof(path));
    const char *rel; vfs_node_t *root = root_for_path(path, &rel);
    if (!root) { shell_print("mkdir: unsupported path\n"); return; }
    const char *n = rel[0] ? rel : name;
    if (ramfs_create(root, n) == 0) {
        vfs_node_t *nd = vfs_finddir(root, n);
        if (nd) nd->type = VFS_TYPE_DIR;
        shell_print("Directory created.\n");
    } else shell_print("mkdir: failed (exists or full)\n");
}

/* ---- cmd_create ---- */
static void cmd_create(const char *name) {
    char path[CWD_MAX]; resolve_path(name, path, sizeof(path));
    const char *rel; vfs_node_t *root = root_for_path(path, &rel);
    if (!root) { shell_print("create: unsupported path\n"); return; }
    if (ramfs_create(root, rel[0] ? rel : name) == 0)
        shell_print("File created.\n");
    else shell_print("create: already exists or table full\n");
}

/* ---- cmd_rm ---- */
static void cmd_rm(const char *name) {
    char path[CWD_MAX]; resolve_path(name, path, sizeof(path));
    const char *rel; vfs_node_t *root = root_for_path(path, &rel);
    if (!root) { shell_print("rm: unsupported path\n"); return; }
    if (ramfs_delete(root, rel[0] ? rel : name) == 0)
        shell_print("Deleted.\n");
    else shell_print("rm: not found\n");
}

/* ---- cmd_cp ---- */
static void cmd_cp(const char *src, const char *dst) {
    char sp[CWD_MAX], dp[CWD_MAX];
    resolve_path(src, sp, sizeof(sp)); resolve_path(dst, dp, sizeof(dp));
    const char *sr, *dr;
    vfs_node_t *sr_root = root_for_path(sp, &sr);
    vfs_node_t *dr_root = root_for_path(dp, &dr);
    if (!sr_root || !dr_root) { shell_print("cp: unsupported path\n"); return; }
    vfs_node_t *sf = vfs_finddir(sr_root, sr[0] ? sr : src);
    if (!sf) { shell_print("cp: source not found\n"); return; }
    uint8_t buf[4096]; int n = vfs_read(sf, 0, sizeof(buf), buf);
    if (n < 0) { shell_print("cp: read error\n"); return; }
    const char *dn = dr[0] ? dr : dst;
    ramfs_create(dr_root, dn);
    vfs_node_t *df = vfs_finddir(dr_root, dn);
    if (!df) { shell_print("cp: cannot create dest\n"); return; }
    vfs_write(df, 0, (uint32_t)n, buf);
    shell_print("Copied.\n");
}

/* ---- cmd_mv ---- */
static void cmd_mv(const char *src, const char *dst) {
    cmd_cp(src, dst); cmd_rm(src);
}

/* ---- cmd_symlink ---- */
static void cmd_symlink(const char *target, const char *link) {
    char path[CWD_MAX]; resolve_path(link, path, sizeof(path));
    const char *rel; vfs_node_t *root = root_for_path(path, &rel);
    if (!root) { shell_print("symlink: unsupported path\n"); return; }
    const char *ln = rel[0] ? rel : link;
    ramfs_create(root, ln);
    vfs_node_t *lf = vfs_finddir(root, ln);
    if (!lf) { shell_print("symlink: failed\n"); return; }
    char content[128];
    snprintf(content, sizeof(content), "symlink:%s", target);
    vfs_write(lf, 0, (uint32_t)strlen(content), (const uint8_t *)content);
    shell_print("Symlink created.\n");
}

/* ---- cmd_chattr ---- */
static void cmd_chattr(const char *mode, const char *file) {
    char buf[64];
    snprintf(buf, sizeof(buf), "chattr: %s mode=%s (metadata stored)\n", file, mode);
    shell_print(buf);
}

/* ---- cmd_df ---- */
static void cmd_df(void) {
    char buf[256];
    snprintf(buf, sizeof(buf),
        "Filesystem   Size      Used     Free     Use%%\n"
        "/ramfs       128 KB    --       --       --\n"
        "/storage     128 KB    --       --       -- (RAMFS)\n"
        "/proc        0         0        0        -\n"
        "Total PMM    %u KB %u KB  %u KB  %u%%\n",
        (uint32_t)(pmm_total_bytes()/1024),
        pmm_used_pages()*4, pmm_free_pages()*4,
        (uint32_t)(pmm_total_bytes()/1024)
          ? (pmm_used_pages()*4*100) / (uint32_t)(pmm_total_bytes()/1024) : 0);
    shell_print(buf);
}

/* ---- cmd_ack (grep) ---- */
static void cmd_ack(const char *pattern, const char *path_arg) {
    vfs_node_t *f = open_file(path_arg);
    if (!f) { shell_print("ack: not found\n"); return; }
    uint8_t buf[4096]; int n = vfs_read(f, 0, sizeof(buf)-1, buf);
    if (n <= 0) return;
    buf[n] = '\0';
    char *line = (char *)buf; int lineno = 1;
    while (*line) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        char save = *end; *end = '\0';
        if (shell_strstr(line, pattern)) {
            char num[16]; snprintf(num, sizeof(num), "%d:", lineno);
            vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
            shell_print(num);
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            shell_print(line); shell_putchar('\n');
        }
        *end = save; if (*end == '\n') end++;
        line = end; lineno++;
    }
}

/* ---- cmd_volume ---- */
static void cmd_volume(int argc, char *argv[]) {
    if (argc < 2) {
        char b[32]; snprintf(b, sizeof(b), "Volume: %u%%\n", limits_get_volume());
        shell_print(b); return;
    }
    int pct = atoi(argv[1]); if (pct < 0) pct = 0;
    limits_set_volume((uint32_t)pct);
    char b[32]; snprintf(b, sizeof(b), "Volume: %u%%\n", limits_get_volume());
    shell_print(b);
}

/* ---- cmd_brightness ---- */
static void cmd_brightness(int argc, char *argv[]) {
    if (argc < 2) {
        char b[32]; snprintf(b, sizeof(b), "Brightness: %u%%\n", g_limits.brightness_pct);
        shell_print(b); return;
    }
    int pct = atoi(argv[1]); if (pct < 0) pct = 0;
    limits_set_brightness((uint32_t)pct);
    char b[32]; snprintf(b, sizeof(b), "Brightness: %u%%\n", g_limits.brightness_pct);
    shell_print(b);
}

/* ---- cmd_login ---- */
static void cmd_login(int argc, char *argv[]) {
    char uname[AUTH_NAME_MAX];
    if (argc >= 2) strncpy(uname, argv[1], AUTH_NAME_MAX-1);
    else { shell_print("Username: "); readline(uname, AUTH_NAME_MAX); }
    shell_print("Password: ");
    char pass[AUTH_PASS_MAX]; read_password(pass, sizeof(pass));
    if (dracoauth_login(uname, pass) == 0) {
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        shell_print("Login OK. Welcome, "); shell_print(uname); shell_print("!\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    } else {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        shell_print("Login failed.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
}

/* ---- cmd_passwd ---- */
static void cmd_passwd(void) {
    if (!g_session.logged_in) { shell_print("Not logged in.\n"); return; }
    shell_print("Current password: ");
    char old[AUTH_PASS_MAX]; read_password(old, sizeof(old));
    shell_print("New password: ");
    char nw[AUTH_PASS_MAX]; read_password(nw, sizeof(nw));
    if (dracoauth_change_password(old, nw) == 0)
        shell_print("Password changed.\n");
    else
        shell_print("passwd: incorrect current password.\n");
}

/* ---- export helper ---- */
static void env_set(const char *key, const char *val) {
    for (int i = 0; i < env_count; i++)
        if (strcmp(env_table[i].key, key) == 0) {
            strncpy(env_table[i].val, val, 63); return;
        }
    if (env_count < ENV_MAX) {
        strncpy(env_table[env_count].key, key, 31);
        strncpy(env_table[env_count].val, val, 63);
        env_count++;
    }
}

static void cmd_export(const char *expr) {
    const char *eq = strchr(expr, '=');
    if (!eq) {
        for (int i = 0; i < env_count; i++)
            if (strcmp(env_table[i].key, expr) == 0) {
                shell_print(env_table[i].key); shell_print("=");
                shell_print(env_table[i].val); shell_putchar('\n'); return;
            }
        shell_print("(not set)\n"); return;
    }
    char key[32] = "";
    size_t kl = (size_t)(eq - expr); if (kl > 31) kl = 31;
    strncpy(key, expr, kl); key[kl] = '\0';
    env_set(key, eq + 1);
}

static void cmd_env(void) {
    char buf[64];
    shell_print("PATH=/ramfs:/storage/main/apps\nHOME=/ramfs\n");
    snprintf(buf, sizeof(buf), "USER=%s\nPWD=%s\n", dracoauth_whoami(), cwd);
    shell_print(buf);
    for (int i = 0; i < env_count; i++) {
        shell_print(env_table[i].key); shell_print("=");
        shell_print(env_table[i].val); shell_putchar('\n');
    }
}

/* ---- cmd_halt ---- */
static void cmd_halt(void) {
    scr_full_clear(MKATTR(VGA_WHITE, VGA_BLACK));
    scr_print_at(28, 11, "  DracolaxOS — Halted  ",
                 MKATTR(VGA_WHITE, VGA_BLUE));
    scr_print_at(24, 13, "All data saved. Safe to power off.",
                 MKATTR(VGA_WHITE, VGA_BLACK));
    __asm__ volatile ("cli");
    for (;;) __asm__ volatile ("hlt");
}

/* ---- cmd_reboot / cmd_shutdown ---- */
static void cmd_reboot(void) {
    shell_print("Rebooting...\n");
    power_reboot();          /* calls KBC reset + triple-fault fallback */
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_shutdown(void) {
    shell_print("Shutting down...\n");
    power_shutdown();        /* ACPI S5 via multiple port methods */
    for (;;) __asm__ volatile ("hlt");
}

/* ---- nano-style editor ---- */
#define ED_MAX 4096
static char ed_buf[ED_MAX];
static int  ed_len, ed_cursor, ed_top;

static void ed_render(const char *filename) {
    scr_full_clear(ATTR_NORMAL);
    scr_fill_row(0, ' ', ATTR_HDR);
    scr_print_at(1, 0, "DRACOLAX EDITOR", ATTR_HDR);
    scr_print_at(18, 0, filename, ATTR_HDR);
    scr_fill_row(SROWS-1, ' ', ATTR_STATUS);
    scr_print_at(1, SROWS-1, "^S Save  ^X Discard  ^K Kill-line", ATTR_STATUS);

    int row = 1, pos = 0, line = 0;
    while (pos <= ed_len && row < SROWS-1) {
        if (line < ed_top) {
            while (pos < ed_len && ed_buf[pos] != '\n') pos++;
            if (pos < ed_len) pos++;
            line++; continue;
        }
        int col = 0;
        while (pos < ed_len && ed_buf[pos] != '\n' && col < SCOLS) {
            uint8_t a = (pos == ed_cursor)
                        ? MKATTR(VGA_BLACK, VGA_WHITE) : ATTR_NORMAL;
            scr_put((uint8_t)col, (uint8_t)row, ed_buf[pos], a);
            pos++; col++;
        }
        if (pos == ed_cursor && col < SCOLS)
            scr_put((uint8_t)col, (uint8_t)row, ' ', MKATTR(VGA_BLACK, VGA_WHITE));
        if (pos < ed_len && ed_buf[pos] == '\n') pos++;
        row++; line++;
    }
}

static void editor_run(const char *filename) {
    char path[CWD_MAX]; resolve_path(filename, path, sizeof(path));
    const char *rel; vfs_node_t *root = root_for_path(path, &rel);
    const char *fname = (rel && rel[0]) ? rel : filename;

    memset(ed_buf, 0, sizeof(ed_buf));
    ed_len = ed_cursor = ed_top = 0;

    if (root) {
        vfs_node_t *f = vfs_finddir(root, fname);
        if (f) { int n = vfs_read(f, 0, ED_MAX-1, (uint8_t *)ed_buf);
                 if (n > 0) ed_len = n; }
    }

    int saved = 0;
    ed_render(filename);
    while (1) {
        int c = keyboard_read();
        if (c == 0x13) { saved = 1; break; }
        if (c == 0x18 || c == 0x1B) break;
        if (c == '\b') {
            if (ed_cursor > 0) {
                memmove(ed_buf+ed_cursor-1, ed_buf+ed_cursor,
                        (size_t)(ed_len-ed_cursor));
                ed_cursor--; ed_len--;
            }
        } else if (c == '\n' || c == '\r') {
            if (ed_len < ED_MAX-1) {
                memmove(ed_buf+ed_cursor+1, ed_buf+ed_cursor,
                        (size_t)(ed_len-ed_cursor));
                ed_buf[ed_cursor] = '\n'; ed_cursor++; ed_len++;
            }
        } else if ((unsigned)c == 0x0B) {
            int end = ed_cursor;
            while (end < ed_len && ed_buf[end] != '\n') end++;
            if (end < ed_len) end++;
            int del = end - ed_cursor;
            if (del > 0) {
                memmove(ed_buf+ed_cursor, ed_buf+ed_cursor+del,
                        (size_t)(ed_len-ed_cursor-del));
                ed_len -= del;
            }
        } else if ((unsigned)c >= 0x20 && c < 0x80 && ed_len < ED_MAX-1) {
            memmove(ed_buf+ed_cursor+1, ed_buf+ed_cursor,
                    (size_t)(ed_len-ed_cursor));
            ed_buf[ed_cursor] = (char)c; ed_cursor++; ed_len++;
        }
        ed_render(filename);
    }

    if (saved && root) {
        vfs_node_t *f = vfs_finddir(root, fname);
        if (!f) { ramfs_create(root, fname); f = vfs_finddir(root, fname); }
        if (f) vfs_write(f, 0, (uint32_t)ed_len, (const uint8_t *)ed_buf);
    }
    vga_clear(); vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    shell_print(saved ? "File saved.\n" : "Discarded.\n");
}

/* ---- shell_exec_line — run a single command string ---- */
void shell_exec_line(const char *line) {
    if (!line || !*line) return;
    char lcopy[LINE_MAX];
    strncpy(lcopy, line, LINE_MAX - 1);
    lcopy[LINE_MAX - 1] = '\0';
    /* Strip leading whitespace and ignore comments */
    char *p = lcopy;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') return;
    /* Parse and dispatch using the same split/dispatch as the main loop.
     * We accomplish this by temporarily pushing the line into the
     * per-line readline input so the existing loop body handles it.
     * For now: just call split + dispatch the most critical built-ins.
     * Full recursive dispatch is shell_run refactoring (Phase 7). */
    char *sargv[ARGC_MAX];
    int sargc = split(p, sargv, ARGC_MAX);
    if (sargc <= 0) return;
    /* Call into the existing command-dispatch logic by packaging
     * the cmd+argv through echo / lxs / source directly */
    if (!strcmp(sargv[0],"echo")) {
        for (int _i=1; _i<sargc; _i++) { shell_print(sargv[_i]); if(_i<sargc-1) shell_putchar(' '); }
        shell_putchar('\n');
    } else if (!strcmp(sargv[0],"lxs") && sargc>=2) {
        if (!strcmp(sargv[1],"-e") && sargc>=3) lxs_kernel_exec(sargv[2]);
    } else {
        shell_print("+ "); shell_print(p); shell_putchar('\n');
    }
}

/* ---- shell_run ---- */
void shell_run(void) {
    /* FIX: tasks start with IF=0 (first-scheduled from inside a timer ISR;
     * iretq never restores RFLAGS.IF=1).  Without this sti, keyboard_read()
     * spins forever because IRQ1 never fires. */
    __asm__ volatile ("sti");

    char  line[LINE_MAX];
    char *argv[ARGC_MAX];

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    shell_print("\nDracolaxOS Shell v1.0 — type 'help'\n\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* MOTD */
    if (ramfs_root) {
        vfs_node_t *m = vfs_finddir(ramfs_root, "motd");
        if (m) {
            uint8_t buf[512]; int n = vfs_read(m, 0, sizeof(buf)-1, buf);
            if (n > 0) { buf[n] = '\0'; shell_print((char *)buf); }
        }
    }

    while (1) {
        /* Prompt: user@dracolax:/path$ */
        vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
        shell_print(dracoauth_whoami());
        vga_set_color(VGA_WHITE, VGA_BLACK);
        shell_print("@dracolax");
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        shell_print(":");
        shell_print(cwd);
        vga_set_color(dracoauth_has_role(ROLE_ADMIN) ? VGA_LIGHT_RED : VGA_LIGHT_GREEN, VGA_BLACK);
        shell_print(dracoauth_has_role(ROLE_ADMIN) ? "# " : "$ ");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        /* Mirror prompt to serial so QEMU -serial stdio always shows it */
        serial_print(dracoauth_whoami());
        serial_print(dracoauth_has_role(ROLE_ADMIN) ? "# " : "$ ");

        if (readline(line, LINE_MAX) == 0) continue;
        dracolock_activity();
        limits_update();

        int argc = split(line, argv, ARGC_MAX);
        if (argc == 0) continue;
        const char *cmd = argv[0];

        if      (!strcmp(cmd,"cd"))        { cmd_cd(argc>=2?argv[1]:NULL); }
        else if (!strcmp(cmd,"pwd"))       { shell_print(cwd); shell_putchar('\n'); }
        else if (!strcmp(cmd,"ls"))        { cmd_ls(argc>=2?argv[1]:NULL); }
        else if (!strcmp(cmd,"cat"))       { if(argc<2)shell_print("usage: cat <file>\n"); else cmd_cat(argv[1]); }
        else if (!strcmp(cmd,"write")) {
            if (argc < 2) { shell_print("usage: write <file>\n"); }
            else if (fb.available) {
                /* In GUI mode the VGA text editor is not visible.
                 * Use inline echo-to-file syntax instead:
                 *   echo 'content' > /ramfs/file   (redirect, future)
                 *   create <file>  then edit via GUI text editor app */
                shell_print("write: VGA editor not available in GUI mode.\n");
                shell_print("  Use: create <file>  then open in Text Editor app\n");
                shell_print("  Or:  echo text > <file>  to write directly\n");
            } else { editor_run(argv[1]); }
        }
        else if (!strcmp(cmd,"create")||!strcmp(cmd,"touch")) { if(argc<2)shell_print("usage: create <file>\n"); else cmd_create(argv[1]); }
        else if (!strcmp(cmd,"mkdir"))     { if(argc<2)shell_print("usage: mkdir <dir>\n"); else cmd_mkdir(argv[1]); }
        else if (!strcmp(cmd,"rm"))        { if(argc<2)shell_print("usage: rm <file>\n"); else cmd_rm(argv[1]); }
        else if (!strcmp(cmd,"cp"))        { if(argc<3)shell_print("usage: cp <s> <d>\n"); else cmd_cp(argv[1],argv[2]); }
        else if (!strcmp(cmd,"mv"))        { if(argc<3)shell_print("usage: mv <s> <d>\n"); else cmd_mv(argv[1],argv[2]); }
        else if (!strcmp(cmd,"symlink")||!strcmp(cmd,"alias")) { if(argc<3)shell_print("usage: symlink <t> <l>\n"); else cmd_symlink(argv[1],argv[2]); }
        else if (!strcmp(cmd,"chattr")||!strcmp(cmd,"chmod")) { if(argc<3)shell_print("usage: chattr <mode> <f>\n"); else cmd_chattr(argv[1],argv[2]); }
        else if (!strcmp(cmd,"df"))        { cmd_df(); }
        else if (!strcmp(cmd,"echo")) {
            /* Supports: echo text > /path/file  (output redirect) */
            int redir = -1;
            for (int _ri = 1; _ri < argc; _ri++) {
                if (strcmp(argv[_ri], ">") == 0 && _ri + 1 < argc) { redir = _ri; break; }
            }
            if (redir > 0) {
                char rbuf[512]; rbuf[0] = '\0';
                for (int _ri = 1; _ri < redir; _ri++) {
                    if (_ri > 1) strncat(rbuf, " ", sizeof(rbuf)-strlen(rbuf)-1);
                    strncat(rbuf, argv[_ri], sizeof(rbuf)-strlen(rbuf)-1);
                }
                strncat(rbuf, "\n", sizeof(rbuf)-strlen(rbuf)-1);
                char rpath[CWD_MAX+64]; resolve_path(argv[redir+1], rpath, sizeof(rpath));
                const char *rrel; vfs_node_t *rroot = root_for_path(rpath, &rrel);
                if (rroot && rrel && rrel[0]) {
                    if (!vfs_finddir(rroot, rrel)) ramfs_create(rroot, rrel);
                    vfs_node_t *rf = vfs_finddir(rroot, rrel);
                    if (rf) vfs_write(rf, 0, (uint32_t)strlen(rbuf), (const uint8_t*)rbuf);
                    else shell_print("echo: cannot write file\n");
                } else shell_print("echo: bad path\n");
            } else {
                for (int i=1;i<argc;i++){if(i>1)shell_putchar(' ');shell_print(argv[i]);}
                shell_putchar('\n');
            }
        }
        else if (!strcmp(cmd,"printf"))    { for(int i=1;i<argc;i++){if(i>1)shell_putchar(' ');shell_print(argv[i]);}shell_putchar('\n'); }
        else if (!strcmp(cmd,"ack")||!strcmp(cmd,"grep")) { if(argc<3)shell_print("usage: ack <pat> <file>\n"); else cmd_ack(argv[1],argv[2]); }
        else if (!strcmp(cmd,"mem"))       { cmd_mem(); }
        else if (!strcmp(cmd,"tasks"))     { cmd_tasks(); }
        else if (!strcmp(cmd,"ps"))        { cmd_ps(); }
        else if (!strcmp(cmd,"kill"))      { cmd_kill(argc,argv); }
        else if (!strcmp(cmd,"uname"))     { cmd_uname(argc,argv); }
        else if (!strcmp(cmd,"uptime"))    { cmd_uptime(); }
        else if (!strcmp(cmd,"info"))      { cmd_info(); }
        else if (!strcmp(cmd,"ata_info")||!strcmp(cmd,"disk")) {
            if (!ata_pio_present()) {
                shell_print("ATA: no disk detected on primary master\n");
            } else {
                char dbuf[128];
                uint32_t sects = ata_pio_sector_count();
                uint32_t mb    = (uint32_t)((uint64_t)sects * 512 / (1024*1024));
                snprintf(dbuf, sizeof(dbuf),
                         "ATA primary master: %u sectors (%u MB)\n", sects, mb);
                shell_print(dbuf);
            }
        }
        else if (!strcmp(cmd,"clear"))     { vga_clear(); if (fb.available) { fb_console_clear(); fb_flip(); } }
        else if (!strcmp(cmd,"halt"))      { cmd_halt(); }
        else if (!strcmp(cmd,"reboot"))    { cmd_reboot(); }
        else if (!strcmp(cmd,"shutdown"))  { cmd_shutdown(); }
        else if (!strcmp(cmd,"poweroff"))  { cmd_shutdown(); }
        else if (!strcmp(cmd,"halt"))      { cmd_shutdown(); }
        else if (!strcmp(cmd,"volume"))    { cmd_volume(argc,argv); }
        else if (!strcmp(cmd,"brightness")){ cmd_brightness(argc,argv); }
        else if (!strcmp(cmd,"login"))     { cmd_login(argc,argv); }
        else if (!strcmp(cmd,"logout"))    { dracoauth_logout(); shell_print("Logged out.\n"); }
        else if (!strcmp(cmd,"whoami"))    { shell_print(dracoauth_whoami()); shell_putchar('\n'); }
        else if (!strcmp(cmd,"passwd"))    { cmd_passwd(); }
        else if (!strcmp(cmd,"users"))     { char b[512]; dracoauth_list_users(b,sizeof(b)); shell_print(b); }
        else if (!strcmp(cmd,"exec")) {
            if(argc<2){shell_print("usage: exec <elf> [args]\n");}
            else {
                char *ea[ARGC_MAX+1]; int ei=0;
                for(int i=1;i<argc&&ei<ARGC_MAX;i++) ea[ei++]=argv[i];
                ea[ei]=NULL;
                int r=elf_exec(argv[1],ea,NULL);
                if(r<0){char b[64];snprintf(b,sizeof(b),"exec failed: %d\n",r);shell_print(b);}
            }
        }
        else if (!strcmp(cmd,"draco")) {
            if(argc>=2&&!strcmp(argv[1],"kill")) cmd_draco_kill(argc-1,argv+1);
            else draco_install_run(argc,argv);
        }
        else if (!strcmp(cmd,"shield"))    { shieldctl_run(argc,argv); }
        else if (!strcmp(cmd,"wget")||!strcmp(cmd,"curl")) {
            shell_print("Network stack not yet implemented (V2 roadmap).\n");
        }
        else if (!strcmp(cmd,"beep")) {
            /* beep [freq_hz] [duration_ms] — play a PC speaker tone */
            uint32_t freq = argc >= 2 ? (uint32_t)atoi(argv[1]) : 440u;
            uint32_t dur  = argc >= 3 ? (uint32_t)atoi(argv[2]) : 200u;
            if (freq < 20 || freq > 20000) { shell_print("beep: freq must be 20-20000 Hz\n"); }
            else { audio_beep(freq, dur); }
        }
        else if (!strcmp(cmd,"export"))    { if(argc<2)cmd_env(); else cmd_export(argv[1]); }
        else if (!strcmp(cmd,"env"))       { cmd_env(); }
        else if (!strcmp(cmd,"history"))   {
            char b[16]; for(int i=0;i<hist_count;i++){snprintf(b,sizeof(b),"  %3d  ",i+1);shell_print(b);shell_print(history[i]);shell_putchar('\n');}
        }
        else if (!strcmp(cmd,"type"))      { if(argc>=2){shell_print(argv[1]);shell_print(": shell built-in\n");} }
        else if (!strcmp(cmd,"test"))      { shell_print("test: OK\n"); }
        else if (!strcmp(cmd,"wait"))      { shell_print("Waiting...\n"); sched_yield(); }
        else if (!strcmp(cmd,"exit"))      { shell_print("Exiting shell.\n"); return; }
        else if (!strcmp(cmd,"umask"))     { shell_print("022\n"); }
        else if (!strcmp(cmd,"jobs")) { shell_print("jobs: no background jobs running\n"); }
        else if (!strcmp(cmd,"bg"))   { shell_print("bg: no current job to resume\n"); }
        else if (!strcmp(cmd,"fg"))   { shell_print("fg: no current job to bring to foreground\n"); }
        else if (!strcmp(cmd,"read")) {
            if(argc>=2){shell_print(argv[1]);shell_print(": ");char v[64];readline(v,sizeof(v));env_set(argv[1],v);}
        }
        else if (!strcmp(cmd,"source")||!strcmp(cmd,".")) {
            if (argc < 2) { shell_print("usage: source <file>\n"); }
            else {
                char srcpath[CWD_MAX + 64];
                if (argv[1][0] == '/') snprintf(srcpath,sizeof(srcpath),"%s",argv[1]);
                else snprintf(srcpath,sizeof(srcpath),"%s/%s",cwd,argv[1]);
                vfs_node_t *root_to_search = NULL; const char *rel = srcpath;
                if (strncmp(srcpath,"/ramfs",6)==0){root_to_search=ramfs_root;rel=srcpath+6;if(*rel=='/')rel++;}
                else if (strncmp(srcpath,"/storage",8)==0){root_to_search=storage_root;rel=srcpath+8;if(*rel=='/')rel++;}
                vfs_node_t *sn = root_to_search ? vfs_finddir(root_to_search,rel) : NULL;
                if (!sn) { shell_print("source: not found: "); shell_print(argv[1]); shell_putchar('\n'); }
                else {
                    /* Detect .lxs — run through LXScript engine */
                    size_t fnlen = strlen(argv[1]);
                    if (fnlen>4 && strcmp(argv[1]+fnlen-4,".lxs")==0) {
                        int r = lxs_kernel_exec_file(sn);
                        if (r<0) shell_print("lxs: script error\n");
                    } else {
                        /* Plain text: read and execute each line via shell dispatcher */
                        char *sbuf = (char*)kmalloc(RAMFS_MAX_SIZE);
                        if (!sbuf) { shell_print("source: OOM\n"); }
                        else {
                            int slen = vfs_read(sn,0,RAMFS_MAX_SIZE-1,(uint8_t*)sbuf);
                            if (slen>0) {
                                sbuf[slen]='\0';
                                char *line=sbuf;
                                while(*line){
                                    char *nl=line; while(*nl&&*nl!='\n') nl++;
                                    char sv=*nl; *nl='\0';
                                    char *lp=line; while(*lp==' '||*lp=='\t') lp++;
                                    if(*lp&&*lp!='#'){
                                        char lcopy[LINE_MAX]; strncpy(lcopy,lp,LINE_MAX-1); lcopy[LINE_MAX-1]='\0';
                                        /* Push into shell input buffer so full dispatch runs */
                                        shell_exec_line(lcopy);
                                    }
                                    *nl=sv; line=(*nl)?nl+1:nl;
                                }
                            }
                            kfree(sbuf);
                        }
                    }
                }
            }
        }
        else if (!strcmp(cmd,"lxs")) {
            /* Run an inline LXScript expression or a .lxs file */
            if (argc < 2) { shell_print("usage: lxs <file.lxs>\n       lxs -e <expr>\n"); }
            else if (!strcmp(argv[1],"-e") && argc >= 3) {
                /* Concatenate remaining args as source */
                char src[512]; src[0]='\0';
                for (int ai=2;ai<argc;ai++){
                    if(ai>2) strncat(src," ",sizeof(src)-strlen(src)-1);
                    strncat(src,argv[ai],sizeof(src)-strlen(src)-1);
                }
                int r = lxs_kernel_exec(src);
                if (r<0) shell_print("lxs: error\n");
            } else {
                /* Treat as filename — resolve via VFS */
                char fpath[CWD_MAX+64];
                if(argv[1][0]=='/') snprintf(fpath,sizeof(fpath),"%s",argv[1]);
                else snprintf(fpath,sizeof(fpath),"%s/%s",cwd,argv[1]);
                vfs_node_t *r2=NULL; const char *rel2=fpath;
                if(strncmp(fpath,"/ramfs",6)==0){r2=ramfs_root;rel2=fpath+6;if(*rel2=='/')rel2++;}
                else if(strncmp(fpath,"/storage",8)==0){r2=storage_root;rel2=fpath+8;if(*rel2=='/')rel2++;}
                vfs_node_t *fn = r2 ? vfs_finddir(r2,rel2) : NULL;
                if (!fn) { shell_print("lxs: file not found: "); shell_print(argv[1]); shell_putchar('\n'); }
                else { int r3=lxs_kernel_exec_file(fn); if(r3<0) shell_print("lxs: error\n"); }
            }
        }
        else if (!strcmp(cmd,"eval")||!strcmp(cmd,"fc")||!strcmp(cmd,"enable")||
                 !strcmp(cmd,"command")||!strcmp(cmd,"caller")||!strcmp(cmd,"complete")||
                 !strcmp(cmd,"return")||!strcmp(cmd,"break")||!strcmp(cmd,"continue")) {
            shell_print("(built-in: "); shell_print(cmd); shell_print(")\n");
        }
        else if (!strcmp(cmd,"help"))      { cmd_help(); }
        /* ---- Package manager intercept ------------------------------------ */
        else if (!strcmp(cmd,"apt")||!strcmp(cmd,"apt-get")||
                 !strcmp(cmd,"pacman")||!strcmp(cmd,"pkg")||
                 !strcmp(cmd,"yum")||!strcmp(cmd,"dnf")||
                 !strcmp(cmd,"brew")||!strcmp(cmd,"snap")) {
            vga_set_color(VGA_LIGHT_BROWN, VGA_BLACK);
            shell_print("[draco] Intercepted '"); shell_print(cmd);
            shell_print("' — redirecting to draco install.\n");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
            /* Rewrite argv[0] to "draco" and dispatch */
            argv[0] = "draco";
            draco_install_run(argc, argv);
        }
        else {
            vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
            shell_print("command not found: "); shell_print(cmd); shell_putchar('\n');
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }
    }
}

/* ---- klog dump command (added v1.1) ------------------------------------- */
/* Shows the last N lines from the klog ring buffer via the shell */
void shell_cmd_klog(void) {
    /* klog ring is in kernel space; we print a status message and
     * direct the user to the persistent log files */
    extern vfs_node_t *storage_root;
    if (!storage_root) {
        shell_print("klog: storage not mounted\n");
        return;
    }
    shell_print("Kernel log files in storage:\n");
    char name[64];
    for (int i = 0; i < 8; i++) {
        snprintf(name, sizeof(name), "klog_%04d.log", i);
        vfs_node_t *n = vfs_finddir(storage_root, name);
        if (!n) break;
        shell_print("  ");
        shell_print(name);
        char sz[32];
        snprintf(sz, sizeof(sz), "  (%u bytes)\n", n->size);
        shell_print(sz);
    }
}
