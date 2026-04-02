/* kernel/signal.c — Minimal signal delivery (FIX 3.8)
 *
 * Default actions for each signal group (POSIX):
 *   Term  — terminate the process   (SIGTERM, SIGINT, SIGPIPE, SIGALRM, ...)
 *   Ign   — ignore                  (SIGCHLD default is Ign in many configs,
 *                                    but we deliver it to allow wait() later)
 *   Core  — terminate + core dump   (treated as Term here, no dump yet)
 *   Stop  — stop the process        (SIGSTOP — treated as Term for now)
 *   Cont  — continue                (SIGCONT — ignored for now)
 */
#include "signal.h"
#include "../sched/sched.h"
#include "../sched/task.h"
#include "../log.h"
#include "../klibc.h"

/* Default action codes */
#define DA_TERM  0   /* terminate */
#define DA_IGN   1   /* ignore    */
#define DA_STOP  2   /* stop (treated as terminate) */
#define DA_CONT  3   /* continue  (ignored)         */

static const int default_action[NSIG] = {
/*  0 */ DA_TERM,  /* unused      */
/*  1 */ DA_TERM,  /* SIGHUP      */
/*  2 */ DA_TERM,  /* SIGINT      */
/*  3 */ DA_TERM,  /* SIGQUIT     */
/*  4 */ DA_TERM,  /* SIGILL      */
/*  5 */ DA_TERM,  /* SIGTRAP     */
/*  6 */ DA_TERM,  /* SIGABRT     */
/*  7 */ DA_TERM,  /* SIGBUS      */
/*  8 */ DA_TERM,  /* SIGFPE      */
/*  9 */ DA_TERM,  /* SIGKILL     */
/* 10 */ DA_TERM,  /* SIGUSR1     */
/* 11 */ DA_TERM,  /* SIGSEGV     */
/* 12 */ DA_TERM,  /* SIGUSR2     */
/* 13 */ DA_TERM,  /* SIGPIPE     */
/* 14 */ DA_TERM,  /* SIGALRM     */
/* 15 */ DA_TERM,  /* SIGTERM     */
/* 16 */ DA_TERM,  /* SIGSTKFLT   */
/* 17 */ DA_IGN,   /* SIGCHLD     */
/* 18 */ DA_CONT,  /* SIGCONT     */
/* 19 */ DA_STOP,  /* SIGSTOP     */
/* 20 */ DA_STOP,  /* SIGTSTP     */
/* 21 */ DA_STOP,  /* SIGTTIN     */
/* 22 */ DA_STOP,  /* SIGTTOU     */
/* 23 */ DA_IGN,   /* SIGURG      */
/* 24 */ DA_TERM,  /* SIGXCPU     */
/* 25 */ DA_TERM,  /* SIGXFSZ     */
/* 26 */ DA_TERM,  /* SIGVTALRM   */
/* 27 */ DA_TERM,  /* SIGPROF     */
/* 28 */ DA_IGN,   /* SIGWINCH    */
/* 29 */ DA_TERM,  /* SIGIO       */
/* 30 */ DA_TERM,  /* SIGPWR      */
/* 31 */ DA_TERM,  /* SIGSYS      */
};

/* Signals that cannot be caught, blocked, or ignored */
#define UNCATCHABLE_MASK  ((1u << SIGKILL) | (1u << SIGSTOP))

/* ---- init ---------------------------------------------------------------- */

void signal_init(signal_state_t *ss) {
    ss->pending      = 0;
    ss->blocked      = 0;
    ss->exit_signal  = 0;
    for (int i = 0; i < NSIG; i++)
        ss->handlers[i] = SIG_DFL;
}

/* ---- send ---------------------------------------------------------------- */

int signal_send(int task_id, int sig) {
    if (sig <= 0 || sig >= NSIG) return -1;
    task_t *t = sched_task_at(task_id);
    if (!t || t->state == TASK_EMPTY || t->state == TASK_DEAD) return -1;

    uint32_t bit = 1u << (uint32_t)sig;

    /* SIGKILL/SIGSTOP bypass blocking */
    if (bit & UNCATCHABLE_MASK) {
        t->signals.pending |= bit;
        kinfo("SIGNAL: SIGKILL/SIGSTOP → task %d\n", task_id);
        return 0;
    }

    if (!(t->signals.blocked & bit))
        t->signals.pending |= bit;

    return 0;
}

/* ---- notify parent ------------------------------------------------------- */

void signal_notify_parent(int ppid, int child_id, int exit_code) {
    if (ppid <= 0) return;
    task_t *parent = sched_task_at(ppid);
    if (!parent) return;
    (void)child_id; (void)exit_code;
    signal_send(ppid, SIGCHLD);
}

/* ---- handler registration ------------------------------------------------ */

sig_handler_t signal_set_handler(signal_state_t *ss, int sig, sig_handler_t h) {
    if (sig <= 0 || sig >= NSIG) return SIG_DFL;
    uint32_t bit = 1u << (uint32_t)sig;
    if (bit & UNCATCHABLE_MASK) return SIG_DFL;  /* cannot override */
    sig_handler_t old = ss->handlers[sig];
    ss->handlers[sig] = h;
    return old;
}

void signal_block(signal_state_t *ss, uint32_t mask) {
    /* Cannot block SIGKILL or SIGSTOP */
    ss->blocked |= (mask & ~UNCATCHABLE_MASK);
}

void signal_unblock(signal_state_t *ss, uint32_t mask) {
    ss->blocked &= ~mask;
}

/* ---- dispatch ------------------------------------------------------------ */

int signal_dispatch(int task_id) {
    task_t *t = sched_task_at(task_id);
    if (!t) return 0;

    uint32_t deliverable = t->signals.pending & ~t->signals.blocked;
    if (!deliverable) return 0;

    /* Find lowest pending signal */
    int sig = 0;
    for (int i = 1; i < NSIG; i++) {
        if (deliverable & (1u << (uint32_t)i)) { sig = i; break; }
    }
    if (!sig) return 0;

    /* Clear pending bit */
    t->signals.pending &= ~(1u << (uint32_t)sig);

    sig_handler_t h = t->signals.handlers[sig];

    /* SIG_IGN — done */
    if (h == SIG_IGN) return 0;

    /* User handler — call it directly (ring-0 tasks; ring-3 path is future) */
    if (h != SIG_DFL) {
        kinfo("SIGNAL: task %d calling handler for sig %d\n", task_id, sig);
        h(sig);
        return 0;
    }

    /* SIG_DFL — apply default action */
    int action = (sig < NSIG) ? default_action[sig] : DA_TERM;
    switch (action) {
    case DA_IGN:
        return 0;
    case DA_CONT:
        if (t->state == TASK_SLEEPING) t->state = TASK_READY;
        return 0;
    case DA_STOP:
    case DA_TERM:
    default:
        kinfo("SIGNAL: task %d terminated by signal %d\n", task_id, sig);
        t->signals.exit_signal = sig;
        /* Notify parent if it has a SIGCHLD handler */
        signal_notify_parent((int)t->ppid, task_id, -sig);
        /* Force the task to exit on its next slice */
        if (task_id == (int)sched_current()->id) {
            sched_exit();   /* noreturn */
        } else {
            /* Mark dead so scheduler skips it next round */
            t->state = TASK_DEAD;
        }
        return 1;
    }
}
