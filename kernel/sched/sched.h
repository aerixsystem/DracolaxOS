/* kernel/sched.h — Round-robin preemptive scheduler */
#ifndef SCHED_H
#define SCHED_H

#include "task.h"

/* Initialise scheduler; registers itself on IRQ0 (timer) */
void sched_init(void);

/* Create a new kernel thread; returns task ID or -1 on failure */
int sched_spawn(void (*fn)(void), const char *name);

/* Voluntarily yield the CPU to the next ready task */
void sched_yield(void);

/* Called from timer ISR (IRQ0) — may trigger a context switch */
void sched_tick(void);

/* Sleep for ms milliseconds (approximate; depends on PIT frequency) */
void sched_sleep(uint64_t ms);

/* Mark the current task as dead and yield */
void sched_exit(void) __attribute__((noreturn));

/* Return a pointer to the current task */
task_t *sched_current(void);

/* Number of live tasks */
int sched_task_count(void);

/* Iterate over all tasks (for shell 'tasks' command) */
task_t *sched_task_at(int i);

#endif /* SCHED_H */
