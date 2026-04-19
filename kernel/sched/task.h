/* kernel/sched/task.h — Task structure (x86_64) */
#ifndef TASK_H
#define TASK_H
#include "../types.h"
#include "../fs/vfs.h"
#include "../ipc/signal.h"

#define TASK_MAX        32          /* increased from 16 for stress testing  */
#define TASK_STACK_SZ   16384u      /* 16 KB per task                        */
#define TASK_NAME_LEN   32
#define TASK_FD_MAX     32

#define ABI_DRACO  0
#define ABI_LINUX  1

/* ── Task priorities ─────────────────────────────────────────────────────
 * Higher value = more CPU time per scheduling round.
 * REALTIME tasks always run before any NORMAL task.
 * Starvation prevention: tasks gain +1 priority every SCHED_AGING_TICKS
 * ticks without running, capped at PRIO_HIGH. */
#define PRIO_IDLE       0
#define PRIO_LOW        1
#define PRIO_NORMAL     2   /* default for all spawned tasks */
#define PRIO_HIGH       3   /* interactive / GUI tasks */
#define PRIO_REALTIME   4   /* never preempted by lower priority */

/* Base timeslices in scheduler ticks (100 Hz → 10 ms/tick) */
#define TIMESLICE_IDLE      1
#define TIMESLICE_LOW       2
#define TIMESLICE_NORMAL    4
#define TIMESLICE_HIGH      8
#define TIMESLICE_REALTIME  16

#define SCHED_AGING_TICKS   50   /* ticks before starving task gets boosted */
#define WATCHDOG_STRIKE_MAX  3   /* strikes before stuck task is killed */

typedef enum {
    TASK_EMPTY = 0,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_DEAD,
} task_state_t;

typedef struct task {
    /* ── Scheduler state ─────────────────────────────────────────────── */
    uint64_t     rsp;           /* saved stack pointer                      */
    uint32_t     id;
    task_state_t state;
    char         name[TASK_NAME_LEN];
    uint8_t     *stack;
    uint64_t     sleep_until;   /* tick to wake at                          */

    /* ── Priority + timeslice ────────────────────────────────────────── */
    uint8_t      priority;      /* current effective priority (PRIO_*)      */
    uint8_t      base_priority; /* original priority (before aging)         */
    uint16_t     timeslice;     /* ticks remaining in current quantum       */
    uint64_t     last_run_tick; /* global tick when task last ran           */
    uint64_t     total_ticks;   /* total CPU ticks consumed (profiling)     */

    /* ── Watchdog / heartbeat ─────────────────────────────────────────── */
    uint64_t     heartbeat;        /* bumped each sched_yield() / sched_sleep() */
    uint64_t     last_heartbeat;   /* value at last watchdog check              */
    int          watchdog_strikes; /* consecutive missed heartbeats             */

    /* ── Identity ───────────────────────────────────────────────────── */
    uint32_t     pid;
    uint32_t     ppid;
    uint32_t     abi;           /* ABI_DRACO or ABI_LINUX                   */

    /* ── Memory accounting ───────────────────────────────────────────── */
    uint32_t     alloc_count;   /* live kmalloc blocks attributed to task   */
    uint32_t     total_allocs;  /* cumulative allocations by this task      */

    /* ── User-space ──────────────────────────────────────────────────── */
    uint64_t     user_entry;
    uint64_t     user_stack;
    uint64_t     brk_start;
    uint64_t     brk_current;

    /* ── File descriptors ────────────────────────────────────────────── */
    vfs_node_t  *fd_table[TASK_FD_MAX];

    /* ── Exit status ─────────────────────────────────────────────────── */
    int32_t      exit_code;

    /* ── mmap region table ───────────────────────────────────────────── */
    void        *mmap_regions;

    /* ── Signal state ────────────────────────────────────────────────── */
    signal_state_t signals;
} task_t;

#endif /* TASK_H */
