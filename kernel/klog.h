/* kernel/klog.h — Persistent log writer with rotation
 *
 * Logs written to storage/main/logs/kernel/ and storage/main/logs/system/
 * New file created when line count reaches KLOG_MAX_LINES (9000).
 * File names: ISO datetime stamp YYYY-MM-DD_HH-MM-SS.log
 * Crash dumps written to storage/main/crash/ with metadata.
 *
 * Write path: ring buffer → flush task → VFS write (atomic via temp+rename).
 */
#ifndef KLOG_H
#define KLOG_H
#include "types.h"

#define KLOG_MAX_LINES  9000
#define KLOG_LINE_MAX   256

typedef enum { KLOG_KERNEL = 0, KLOG_SYSTEM = 1 } klog_channel_t;

void klog_init(void);
void klog_write(klog_channel_t ch, const char *line);
void klog_flush(void);   /* called by log flush task */

/* Crash dump: captures timestamp, process name, and stack message */
void klog_crash_dump(const char *proc, const char *reason,
                     const char *stack_trace);

/* Periodic flush task — register with sched_spawn(klog_flush_task, "klog") */
void klog_flush_task(void);

/* Emergency drain: write all buffered entries to serial without VFS.
 * Safe to call from kpanic() with interrupts disabled. */
void klog_drain_to_serial(void);

/* Convenience: write a formatted line to kernel channel */
void klog_kinfo (const char *fmt, ...);
void klog_kerror(const char *fmt, ...);

#endif /* KLOG_H */
