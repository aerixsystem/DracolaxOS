/* kernel/sched/sched.h — Priority preemptive scheduler v2.0 */
#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/* ── Lifecycle ─────────────────────────────────────────────────────────── */
void    sched_init   (void);
int     sched_spawn  (void (*fn)(void), const char *name);
void    sched_yield  (void);
void    sched_tick   (void);   /* called from IRQ0 timer ISR */
void    sched_sleep  (uint64_t ms);
void    sched_exit   (void) __attribute__((noreturn));

/* ── Priority control ──────────────────────────────────────────────────── *
 * sched_spawn creates tasks at PRIO_NORMAL.
 * Call sched_set_priority() immediately after spawn to adjust.
 * id = task ID returned by sched_spawn().                                 */
void    sched_set_priority(int id, uint8_t prio);
uint8_t sched_get_priority(int id);

/* ── Task management ───────────────────────────────────────────────────── */
/* Forcibly kill a task by id (marks TASK_DEAD, frees stack, yields).
 * Safe to call from any context; the target is killed at next tick.
 * Returns 0 on success, -1 if id is invalid or already dead. */
int     sched_kill(int id);

/* ── Introspection ─────────────────────────────────────────────────────── */
task_t *sched_current    (void);
int     sched_current_id (void);
int     sched_task_count (void);
task_t *sched_task_at    (int i);
int     sched_get_pid    (void);

/* ── Watchdog integration ──────────────────────────────────────────────── *
 * Called periodically by the watchdog task.
 * Checks all running/ready tasks for stuck heartbeats.
 * After WATCHDOG_STRIKE_MAX consecutive misses the task is killed.        */
void    sched_watchdog_check(void);

#endif /* SCHED_H */
