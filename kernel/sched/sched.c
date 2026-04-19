/* kernel/sched.c — Priority preemptive scheduler v2.0
 *
 * Algorithm: strict priority with round-robin within the same priority level.
 *   REALTIME (4) > HIGH (3) > NORMAL (2) > LOW (1) > IDLE (0)
 *
 * Starvation prevention: any READY task that has not run for >= SCHED_AGING_TICKS
 * gets its effective priority boosted by +1 (capped at PRIO_HIGH, never REALTIME).
 * Priority resets to base_priority once the task runs.
 *
 * Timeslice: each priority level has a base quantum in ticks (10 ms/tick at 100 Hz).
 * task.timeslice counts down; when it hits 0 the task is preempted.
 *
 * Heartbeat watchdog: sched_yield() and sched_sleep() bump task.heartbeat.
 * The IRQ watchdog task checks heartbeats every N seconds; tasks that have
 * not bumped in WATCHDOG_STRIKE_MAX consecutive checks are killed.
 */
#include "../types.h"
#include "sched.h"
#include "task.h"
#include "../mm/vmm.h"
#include "../arch/x86_64/irq.h"
#include "../arch/x86_64/pic.h"
#include "../log.h"
#include "../klibc.h"
#include "../drivers/ps2/input_router.h"
#include "../ipc/signal.h"

static task_t tasks[TASK_MAX];
static int    cur;
static int    ntasks;
static int    sched_ready;

/* ── Context switch ────────────────────────────────────────────────────── */

static uint16_t priority_timeslice(uint8_t prio) {
    switch (prio) {
    case PRIO_REALTIME: return TIMESLICE_REALTIME;
    case PRIO_HIGH:     return TIMESLICE_HIGH;
    case PRIO_NORMAL:   return TIMESLICE_NORMAL;
    case PRIO_LOW:      return TIMESLICE_LOW;
    default:            return TIMESLICE_IDLE;
    }
}

/* 64-bit context switch — saves/restores callee-saved registers.
 * In the System V AMD64 ABI the callee-saved registers are:
 *   rbp, rbx, r12, r13, r14, r15
 * The return address (for a fresh task: the task entry fn) sits just
 * above these 6 pushes on the initial stack frame built in sched_spawn. */
__attribute__((noinline))
static void ctx_switch(uint64_t *old_sp, uint64_t new_sp) {
    __asm__ volatile (
        "push %%rbp\n"
        "push %%rbx\n"
        "push %%r12\n"
        "push %%r13\n"
        "push %%r14\n"
        "push %%r15\n"
        "mov  %%rsp, (%0)\n"   /* save current RSP   */
        "mov  %1,    %%rsp\n"  /* restore new RSP    */
        "pop  %%r15\n"
        "pop  %%r14\n"
        "pop  %%r13\n"
        "pop  %%r12\n"
        "pop  %%rbx\n"
        "pop  %%rbp\n"
        : : "r"(old_sp), "r"(new_sp) : "memory"
    );
}

/* ---- task creation -------------------------------------------------------- */

int sched_spawn(void (*fn)(void), const char *name) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_EMPTY && tasks[i].state != TASK_DEAD) continue;

        uint8_t *stack = (uint8_t *)kmalloc(TASK_STACK_SZ);
        if (!stack) return -1;

        /* Build 64-bit initial stack frame consumed by ctx_switch.
         * Layout (high addr → low, callee-saved then return addr):
         *   fn   ← ctx_switch "ret" lands here on first switch
         *   0    ← rbp
         *   0    ← rbx
         *   0    ← r12
         *   0    ← r13
         *   0    ← r14
         *   0    ← r15  ← tasks[i].rsp points here
         */
        uint64_t *sp = (uint64_t *)(stack + TASK_STACK_SZ);
        *--sp = (uint64_t)(uintptr_t)fn; /* return address */
        *--sp = 0;   /* rbp */
        *--sp = 0;   /* rbx */
        *--sp = 0;   /* r12 */
        *--sp = 0;   /* r13 */
        *--sp = 0;   /* r14 */
        *--sp = 0;   /* r15 */

        tasks[i].rsp         = (uint64_t)(uintptr_t)sp;
        tasks[i].id          = (uint32_t)i;
        tasks[i].state       = TASK_READY;
        tasks[i].stack       = stack;
        tasks[i].sleep_until = 0;
        /* Priority: default NORMAL; callers can set higher via sched_set_priority() */
        tasks[i].priority       = PRIO_NORMAL;
        tasks[i].base_priority  = PRIO_NORMAL;
        tasks[i].timeslice      = TIMESLICE_NORMAL;
        tasks[i].last_run_tick  = pit_ticks();
        tasks[i].total_ticks    = 0;
        tasks[i].heartbeat      = 0;
        tasks[i].last_heartbeat = 0;
        tasks[i].watchdog_strikes = 0;
        tasks[i].alloc_count    = 0;
        tasks[i].total_allocs   = 0;
        strncpy(tasks[i].name, name ? name : "task", TASK_NAME_LEN - 1);
        signal_init(&tasks[i].signals);

        ntasks++;
        kinfo("SCHED: spawned '%s' (id=%d)\n", tasks[i].name, i);
        return i;
    }
    kerror("SCHED: task table full\n");
    return -1;
}

/* ---- scheduling logic ----------------------------------------------------- */

/* ── Priority scheduler ───────────────────────────────────────────────── *
 * Select the highest-priority READY task.
 * Starvation prevention: tasks waiting > SCHED_AGING_TICKS get +1 priority.
 * Within the same priority, round-robin starting from the slot after `cur`.
 * ────────────────────────────────────────────────────────────────────── */
static int next_ready(void) {
    uint64_t now = pit_ticks();

    /* Aging pass: boost starving tasks before selection */
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state != TASK_READY) continue;
        if (tasks[i].priority >= PRIO_REALTIME) continue;  /* don't boost RT */
        if ((now - tasks[i].last_run_tick) >= SCHED_AGING_TICKS) {
            if (tasks[i].priority < PRIO_HIGH)
                tasks[i].priority++;  /* boost once per aging window */
        }
    }

    /* Wake sleeping tasks */
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_SLEEPING && now >= tasks[i].sleep_until)
            tasks[i].state = TASK_READY;
    }

    /* Find highest-priority READY task, round-robin among equals */
    int    best_prio = -1;
    int    best      = cur;  /* fallback: keep running */

    for (int pass = 0; pass < TASK_MAX; pass++) {
        int i = (cur + 1 + pass) % TASK_MAX;
        if (tasks[i].state != TASK_READY) continue;
        if ((int)tasks[i].priority > best_prio) {
            best_prio = (int)tasks[i].priority;
            best      = i;
        }
    }

    /* Also consider currently-running task if no higher priority found */
    if (tasks[cur].state == TASK_RUNNING &&
        (best_prio < 0 || (int)tasks[cur].priority >= best_prio)) {
        /* timeslice check — if still has ticks, keep running */
        if (tasks[cur].timeslice > 0)
            return cur;
    }

    return (best_prio >= 0) ? best : cur;
}

static void switch_to(int next) {
    if (next == cur) {
        /* Timeslice decrement even when not switching */
        if (tasks[cur].timeslice > 0) tasks[cur].timeslice--;
        return;
    }
    int prev = cur;
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;

    /* Reset priority to base after it ran (aging boost consumed) */
    tasks[prev].priority = tasks[prev].base_priority;

    tasks[next].state         = TASK_RUNNING;
    tasks[next].last_run_tick = pit_ticks();
    tasks[next].total_ticks++;
    /* Reload timeslice for the incoming task */
    tasks[next].timeslice     = priority_timeslice(tasks[next].priority);

    cur = next;
    ctx_switch(&tasks[prev].rsp, tasks[next].rsp);
}

void sched_yield(void) {
    tasks[cur].heartbeat++;      /* bump heartbeat so watchdog knows we're alive */
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));
    switch_to(next_ready());
    __asm__ volatile ("push %0; popfq" :: "r"(rflags));
}

void sched_tick(void) {
    pit_tick();
    if (!sched_ready) return;
    signal_dispatch(cur);
    /* Decrement timeslice; preempt when exhausted */
    if (tasks[cur].timeslice > 0) tasks[cur].timeslice--;
    if (tasks[cur].timeslice == 0)
        switch_to(next_ready());
}

void sched_sleep(uint64_t ms) {
    tasks[cur].heartbeat++;      /* alive while sleeping */
    uint64_t wake = pit_ticks() + (ms / 10);
    tasks[cur].sleep_until = wake;
    tasks[cur].state       = TASK_SLEEPING;
    sched_yield();
}

void sched_exit(void) {
    __asm__ volatile ("cli");
    /* Free the heap-allocated stack before relinquishing the slot */
    if (tasks[cur].stack) {
        kfree(tasks[cur].stack);
        tasks[cur].stack = NULL;
    }
    /* FIX 1.11: flush this task's input queue and reassign focus */
    input_router_task_exit(cur);
    tasks[cur].state = TASK_EMPTY;
    ntasks--;
    int next = next_ready();
    if (next == cur) { /* no tasks left */
        kinfo("SCHED: all tasks done\n");
        for (;;) __asm__ volatile ("hlt");
    }
    tasks[next].state = TASK_RUNNING;
    /* No need to save old ESP — task is dead */
    cur = next;
    /* Manually load new stack and "return into" the next task */
    __asm__ volatile (
        "mov %0, %%rsp\n"
        "pop %%r15\n"
        "pop %%r14\n"
        "pop %%r13\n"
        "pop %%r12\n"
        "pop %%rbx\n"
        "pop %%rbp\n"
        "sti\n"
        "ret\n"
        :: "r"(tasks[next].rsp) : "memory"
    );
    __builtin_unreachable();
}

/* ---- timer ISR wrapper ---------------------------------------------------- */

static void timer_handler(struct isr_frame *f) {
    (void)f;
    sched_tick();
}

/* ---- init ----------------------------------------------------------------- */

void sched_init(void) {
    /* Task 0 = current (kmain) context; stack already set up by boot.s */
    tasks[0].id    = 0;
    tasks[0].state = TASK_RUNNING;
    tasks[0].stack = NULL; /* boot stack — not heap-allocated */
    strncpy(tasks[0].name, "kmain", TASK_NAME_LEN - 1);
    cur    = 0;
    ntasks = 1;

    irq_register(32, timer_handler); /* IRQ0 = vector 32 */
    sched_ready = 1;
    kinfo("SCHED: initialised (100Hz, %d task slots)\n", TASK_MAX);
}

task_t *sched_current(void)       { return &tasks[cur]; }
int     sched_task_count(void)    { return ntasks; }
task_t *sched_task_at(int i)      { return (i >= 0 && i < TASK_MAX) ? &tasks[i] : NULL; }

/* ── New public API ───────────────────────────────────────────────────── */

int sched_current_id(void) { return cur; }
int sched_get_pid(void)    { return (int)tasks[cur].pid; }

void sched_set_priority(int id, uint8_t prio) {
    if (id < 0 || id >= TASK_MAX) return;
    if (prio > PRIO_REALTIME) prio = PRIO_REALTIME;
    tasks[id].priority      = prio;
    tasks[id].base_priority = prio;
    tasks[id].timeslice     = priority_timeslice(prio);
}

uint8_t sched_get_priority(int id) {
    if (id < 0 || id >= TASK_MAX) return PRIO_NORMAL;
    return tasks[id].priority;
}

int sched_kill(int id) {
    if (id < 0 || id >= TASK_MAX) return -1;
    if (tasks[id].state == TASK_EMPTY || tasks[id].state == TASK_DEAD) return -1;
    if (id == 0) { kerror("SCHED: cannot kill idle/kernel task\n"); return -1; }

    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));

    kwarn("SCHED: killing task '%s' (id=%d) — watchdog/forced\n",
          tasks[id].name, id);

    if (tasks[id].stack) { kfree(tasks[id].stack); tasks[id].stack = NULL; }
    input_router_task_exit(id);

    tasks[id].state = TASK_DEAD;
    ntasks--;

    /* Log if this task had live allocations at kill time */
    if (tasks[id].alloc_count > 0)
        kwarn("SCHED: task '%s' killed with %u live allocs — possible leak\n",
              tasks[id].name, tasks[id].alloc_count);

    __asm__ volatile ("push %0; popfq" :: "r"(rflags));

    /* If we just killed the running task, yield immediately */
    if (id == cur) sched_exit();
    return 0;
}

/* sched_watchdog_check — called periodically by the watchdog task.
 * Any task that has not bumped its heartbeat since the last check
 * gets a strike. After WATCHDOG_STRIKE_MAX strikes it is killed.
 * REALTIME and IDLE tasks are exempt (they may legitimately not yield). */
void sched_watchdog_check(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        if (tasks[i].state == TASK_EMPTY || tasks[i].state == TASK_DEAD) continue;
        if (tasks[i].state == TASK_SLEEPING) {
            /* Sleeping tasks reset their strike counter — they're not stuck.
             * BUG FIX 3: do NOT advance last_heartbeat here. The heartbeat
             * bump from sched_sleep() must remain visible on the next
             * TASK_RUNNING check; consuming it here caused a perpetual
             * strike-1 loop for any well-behaved sleep-based task. */
            tasks[i].watchdog_strikes = 0;
            continue;
        }
        /* Exempt realtime and idle tasks from heartbeat checks */
        if (tasks[i].priority == PRIO_REALTIME ||
            tasks[i].priority == PRIO_IDLE) continue;

        if (tasks[i].heartbeat == tasks[i].last_heartbeat) {
            tasks[i].watchdog_strikes++;
            kwarn("SCHED: task '%s' (id=%d) heartbeat stalled — strike %d/%d\n",
                  tasks[i].name, i,
                  tasks[i].watchdog_strikes, WATCHDOG_STRIKE_MAX);
            if (tasks[i].watchdog_strikes >= WATCHDOG_STRIKE_MAX) {
                kerror("SCHED: task '%s' (id=%d) stuck — killing\n",
                       tasks[i].name, i);
                sched_kill(i);
            }
        } else {
            /* Heartbeat advanced — healthy */
            tasks[i].watchdog_strikes = 0;
        }
        tasks[i].last_heartbeat = tasks[i].heartbeat;
    }
}
