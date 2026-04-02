/* kernel/sched.c — Preemptive round-robin scheduler
 *
 * Context switch design (pure C + inline asm, no .s file):
 *   ctx_switch saves callee-saved registers (rbp/rbx/r12-r15) and the
 *   stack pointer to *old_rsp, then loads new_rsp and pops them back.
 *   Because ctx_switch is a regular C call, the compiler already
 *   saved/restored caller-saved registers (rax/rcx/rdx/rsi/rdi/r8-r11).
 *
 * Timer: sched_tick() is registered as the IRQ0 handler and fires at
 *        the PIT frequency (100 Hz by default → 10ms time slice).
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
static int    cur;          /* index of running task  */
static int    ntasks;       /* total non-empty slots  */
static int    sched_ready;  /* guard against early ticks */

/* ---- context switch ------------------------------------------------------- */

/* Save old ESP to *old_sp, restore new ESP from new_sp.
 * Callee-saved: ebp, ebx, esi, edi.  The function's return address
 * acts as the "next instruction" after the switch on the old side,
 * or as the entry point for a freshly created task (see sched_spawn). */
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
        strncpy(tasks[i].name, name ? name : "task", TASK_NAME_LEN - 1);
        signal_init(&tasks[i].signals);  /* FIX 3.8 */

        ntasks++;
        kinfo("SCHED: spawned '%s' (id=%d)\n", tasks[i].name, i);
        return i;
    }
    kerror("SCHED: task table full\n");
    return -1;
}

/* ---- scheduling logic ----------------------------------------------------- */

static int next_ready(void) {
    for (int pass = 0; pass < TASK_MAX; pass++) {
        int i = (cur + 1 + pass) % TASK_MAX;
        if (tasks[i].state == TASK_DEAD || tasks[i].state == TASK_EMPTY) continue;
        if (tasks[i].state == TASK_READY) return i;
        if (tasks[i].state == TASK_SLEEPING &&
            pit_ticks() >= tasks[i].sleep_until) {
            tasks[i].state = TASK_READY;
            return i;
        }
    }
    return cur; /* no other task; continue running */
}

static void switch_to(int next) {
    if (next == cur) return;
    int prev = cur;
    if (tasks[prev].state == TASK_RUNNING)
        tasks[prev].state = TASK_READY;
    tasks[next].state = TASK_RUNNING;
    cur = next;
    ctx_switch(&tasks[prev].rsp, tasks[next].rsp);
}

void sched_yield(void) {
    /* Save interrupt flag and disable; guaranteed restore on all exit paths
     * including a fault inside switch_to (iretq restores RFLAGS from stack). */
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(rflags));
    switch_to(next_ready());
    __asm__ volatile ("push %0; popfq" :: "r"(rflags));
}

void sched_tick(void) {
    pit_tick();   /* increment counter — required for sched_sleep() */
    if (!sched_ready) return;
    /* FIX 3.8: dispatch any pending signals before switching away */
    signal_dispatch(cur);
    switch_to(next_ready());
}

void sched_sleep(uint64_t ms) {
    uint64_t wake = pit_ticks() + (ms / 10); /* 100Hz → 10ms per tick */
    tasks[cur].sleep_until = wake;
    tasks[cur].state = TASK_SLEEPING;
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
