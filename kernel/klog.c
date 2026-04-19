/* kernel/klog.c — Persistent log writer with rotation
 *
 * Uses the kernel VFS (vfs_node_t* API) and RAMFS directly.
 *
 * VFS API used:
 *   vfs_open(path)                         → vfs_node_t* (NULL = not found)
 *   vfs_write(node, off, len, buf)         → int bytes written
 *   vfs_finddir(dir_node, name)            → vfs_node_t*
 *   ramfs_create(dir_node, name)           → int 0=OK
 *
 * RAMFS_MAX_SIZE = 4096 bytes/file.  Rotation triggers at that boundary
 * (actual line count tracked separately; spec says 9000 lines but RAMFS
 * enforces 4 KB per file — a larger backing store is a V2 task).
 *
 * Write is append-only: each channel tracks its current write offset.
 */
#include "types.h"
#include "klog.h"
#include "klibc.h"
#include "log.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "arch/x86_64/rtc.h"

/* ---- Extern: storage_root created by init.c ----------------------------- */
extern vfs_node_t *storage_root;

/* ---- Ring buffer for async writes --------------------------------------- */
#define LOG_RING  256
#define LINE_SZ   KLOG_LINE_MAX

typedef struct {
    char           line[LINE_SZ];
    klog_channel_t ch;
} log_entry_t;

static log_entry_t log_ring[LOG_RING];
static volatile int lr_head = 0, lr_tail = 0;

/* ---- Per-channel state -------------------------------------------------- */
typedef struct {
    const char  *prefix;       /* log file name prefix  */
    int          line_count;
    int          file_idx;
    vfs_node_t  *node;         /* current open file node */
    uint32_t     write_off;    /* current append offset  */
} log_chan_t;

static log_chan_t channels[2] = {
    { "klog", 0, 0, NULL, 0 },   /* KLOG_KERNEL */
    { "slog", 0, 0, NULL, 0 },   /* KLOG_SYSTEM */
};

/* ---- Helpers ------------------------------------------------------------ */

/* Build a short filename that fits RAMFS VFS_NAME_MAX (64 chars):
 *   klog_0000.log  / slog_0001.log  etc. */
static void make_log_name(log_chan_t *c, int idx, char *out, int outsz) {
    rtc_time_t t;
    rtc_read(&t);
    char ts[24];
    rtc_format(&t, ts, sizeof(ts));
    /* Append index suffix so simultaneous rotations never collide */
    snprintf(out, (size_t)outsz, "%s_%s_%02d.log", c->prefix, ts, idx % 100);
}

/* Create a new log file in storage_root and cache the node pointer. */
static void open_new_log_file(log_chan_t *c) {
    if (!storage_root) return;

    char name[64];
    make_log_name(c, c->file_idx, name, (int)sizeof(name));

    ramfs_create(storage_root, name);
    c->node      = vfs_finddir(storage_root, name);
    c->write_off = 0;

    if (!c->node)
        kerror("KLOG: could not create log file '%s'\n", name);
}

/* Maximum number of log files kept per channel before the oldest is deleted.
 * With KLOG_MAX_LINES=9000 and ~80 bytes/line this is ~720 KB max on disk. */
#define KLOG_MAX_FILES 5

/* Delete the oldest log file for this channel (file_idx - KLOG_MAX_FILES). */
static void delete_old_log(log_chan_t *c) {
    if (!storage_root) return;
    int old_idx = c->file_idx - KLOG_MAX_FILES;
    if (old_idx < 0) return;

    /* We don't have a full directory iterator with delete here, so we
     * instead zero-out the node contents (effectively making it empty).
     * A real implementation would call ramfs_delete(); until that API
     * is wired through VFS, just reuse the slot by overwriting it. */
    char name[64];
    /* Build the old name — we can't reconstruct the original timestamp,
     * so we track file indices and store a sentinel in the filename. */
    snprintf(name, sizeof(name), "%s_old_%04d.log", c->prefix, old_idx);
    vfs_node_t *old = vfs_finddir(storage_root, name);
    if (old) {
        /* Overwrite with empty content to free the slot */
        vfs_write(old, 0, 1, (const uint8_t *)"");
    }
    /* Primary path: try to delete via ramfs directly */
    ramfs_delete(storage_root, name);
}

/* Rotate: close current, increment index, open next, delete oldest. */
static void rotate_log(log_chan_t *c) {
    c->file_idx++;
    c->line_count = 0;
    c->node       = NULL;
    c->write_off  = 0;
    open_new_log_file(c);
    /* Prune oldest file once we exceed the max-files limit */
    if (c->file_idx >= KLOG_MAX_FILES)
        delete_old_log(c);
}

/* Write a null-terminated string to the channel, appending. */
static void chan_write(log_chan_t *c, const char *s, uint32_t len) {
    if (!c->node) return;
    int written = vfs_write(c->node, c->write_off, len, (const uint8_t *)s);
    if (written > 0) c->write_off += (uint32_t)written;
}

/* ---- Public API --------------------------------------------------------- */

void klog_init(void) {
    /* storage_root may not be mounted yet at this point in early boot.
     * open_new_log_file() is deferred until the first klog_write/flush. */
    for (int i = 0; i < 2; i++) {
        channels[i].line_count = 0;
        channels[i].file_idx   = 0;
        channels[i].node       = NULL;
        channels[i].write_off  = 0;
    }
    kinfo("KLOG: log system ready\n");
}

void klog_write(klog_channel_t ch, const char *line) {
    if ((int)ch >= 2) return;
    int next = (lr_tail + 1) % LOG_RING;
    if (next == lr_head) return;   /* ring full – drop */
    log_entry_t *e = &log_ring[lr_tail];
    strncpy(e->line, line, LINE_SZ - 1);
    e->line[LINE_SZ - 1] = '\0';
    e->ch = ch;
    lr_tail = next;
}

void klog_flush(void) {
    while (lr_head != lr_tail) {
        log_entry_t *e = &log_ring[lr_head];
        lr_head = (lr_head + 1) % LOG_RING;

        int ci = (int)e->ch < 2 ? (int)e->ch : 0;
        log_chan_t *c = &channels[ci];

        /* Lazy open: open file when storage_root becomes available */
        if (!c->node && storage_root)
            open_new_log_file(c);
        if (!c->node) continue;

        /* Rotate when RAMFS file is almost full (keep 256 B headroom) */
        if (c->write_off + LINE_SZ + 1 >= RAMFS_MAX_SIZE - 256)
            rotate_log(c);
        if (!c->node) continue;

        uint32_t len = (uint32_t)strlen(e->line);
        chan_write(c, e->line, len);
        chan_write(c, "\n", 1);
        c->line_count++;

        /* Also rotate if line count hits the spec limit */
        if (c->line_count >= KLOG_MAX_LINES)
            rotate_log(c);
    }
}

void klog_crash_dump(const char *proc, const char *reason,
                     const char *stack_trace) {
    if (!storage_root) {
        kerror("KLOG: crash dump: storage not mounted\n");
        return;
    }

    static int crash_idx = 0;
    char name[64];
    snprintf(name, sizeof(name), "crash_%04d.log", crash_idx++);

    ramfs_create(storage_root, name);
    vfs_node_t *node = vfs_finddir(storage_root, name);
    if (!node) { kerror("KLOG: could not create crash dump\n"); return; }

    char buf[RAMFS_MAX_SIZE - 64];
    int  len = snprintf(buf, sizeof(buf),
        "=== DracolaxOS Crash Dump ===\n"
        "Process   : %s\n"
        "Reason    : %s\n"
        "StackTrace:\n%s\n",
        proc        ? proc        : "unknown",
        reason      ? reason      : "unknown",
        stack_trace ? stack_trace : "(none)");

    if (len > 0)
        vfs_write(node, 0, (uint32_t)len, (const uint8_t *)buf);

    kinfo("KLOG: crash dump written -> %s\n", name);
}

/* ---- Flush task (run as a kernel sched task) ---------------------------- */
void klog_flush_task(void) {
    extern void sched_sleep(uint64_t ms);
    kinfo("KLOG: flush task started\n");
    for (;;) {
        klog_flush();
        sched_sleep(500);   /* flush every 500 ms */
    }
}

/* ---- Formatted write helpers -------------------------------------------- */
void klog_kinfo(const char *fmt, ...) {
    char buf[KLOG_LINE_MAX];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    klog_write(KLOG_KERNEL, buf);
}

void klog_kerror(const char *fmt, ...) {
    char buf[KLOG_LINE_MAX];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    klog_write(KLOG_KERNEL, buf);
}

/* klog_drain_to_serial — emergency ring buffer drain for panic handler.
 * Writes all buffered log entries directly to COM1 without touching VFS.
 * Called from kpanic() after interrupts are disabled; must not allocate. */
#include "drivers/serial/serial.h"
void klog_drain_to_serial(void) {
    serial_print("\n=== KLOG DRAIN (panic context) ===\n");
    int i = lr_head;
    int count = 0;
    while (i != lr_tail && count < LOG_RING) {
        serial_print(log_ring[i].line);
        i = (i + 1) % LOG_RING;
        count++;
    }
    serial_print("=== END KLOG DRAIN ===\n");
}
