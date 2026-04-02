/* kernel/signal.h — Minimal signal delivery (FIX 3.8)
 *
 * Implements the minimum signal subset needed for:
 *   - SIGTERM  (15) — polite termination request
 *   - SIGKILL  (9)  — unconditional kill (cannot be caught)
 *   - SIGCHLD  (17) — child exited notification to parent
 *   - SIGALRM  (14) — timer expiry
 *
 * Design (kernel-space tasks, pre-ring3):
 *   Each task_t has a pending_signals bitmask and a handler table.
 *   signal_send(task_id, sig) sets the bit.
 *   signal_dispatch(task_id) is called by the scheduler on each tick for
 *   the running task; if a pending signal has a user handler it calls it,
 *   otherwise it executes the default action (usually sched_exit).
 *
 *   For ring-0 kernel tasks (current state): handlers run directly in
 *   kernel context. When ring-3 exists, signal_deliver_to_user() will
 *   redirect via the saved user-mode RSP/RIP.
 *
 * Signal numbers match POSIX / Linux (for Linux ABI compatibility).
 */
#ifndef SIGNAL_H
#define SIGNAL_H
#include "../types.h"

#define NSIG        32          /* number of signals supported          */

/* Standard signal numbers */
#define SIGHUP      1
#define SIGINT      2
#define SIGQUIT     3
#define SIGILL      4
#define SIGTRAP     5
#define SIGABRT     6
#define SIGBUS      7
#define SIGFPE      8
#define SIGKILL     9           /* cannot be caught or ignored          */
#define SIGUSR1     10
#define SIGSEGV     11
#define SIGUSR2     12
#define SIGPIPE     13
#define SIGALRM     14
#define SIGTERM     15
#define SIGSTKFLT   16
#define SIGCHLD     17
#define SIGCONT     18
#define SIGSTOP     19          /* cannot be caught or ignored          */
#define SIGTSTP     20

/* Signal handler type — matches POSIX sighandler_t */
typedef void (*sig_handler_t)(int signum);

/* Special handler values (POSIX) */
#define SIG_DFL  ((sig_handler_t)0)   /* default action  */
#define SIG_IGN  ((sig_handler_t)1)   /* ignore signal   */

/* Per-task signal state — embedded in task_t via signal_state_t */
typedef struct {
    uint32_t     pending;                  /* bitmask of pending signals      */
    uint32_t     blocked;                  /* bitmask of blocked signals       */
    sig_handler_t handlers[NSIG];          /* per-signal handler (SIG_DFL/IGN/fn) */
    int32_t      exit_signal;             /* signal that caused task to die   */
} signal_state_t;

/* ---- API ----------------------------------------------------------------- */

/* Initialise signal state for a new task */
void signal_init(signal_state_t *ss);

/* Send signal sig to task task_id.
 * SIGKILL/SIGSTOP cannot be blocked.
 * Returns 0 on success, -1 if task_id is invalid. */
int signal_send(int task_id, int sig);

/* Send SIGCHLD to a task's parent when it exits.
 * ppid is the parent task id (task->ppid). */
void signal_notify_parent(int ppid, int child_id, int exit_code);

/* Check and dispatch any pending non-blocked signals for task task_id.
 * Called by the scheduler once per task switch.
 * Executes SIG_DFL actions (kill/ignore) or calls user handlers.
 * Returns 1 if the task was terminated by a signal, 0 otherwise. */
int signal_dispatch(int task_id);

/* Register a handler for sig in the given signal state.
 * Returns the previous handler, or SIG_DFL on error. */
sig_handler_t signal_set_handler(signal_state_t *ss, int sig, sig_handler_t h);

/* Block/unblock a set of signals (bitmask) */
void signal_block(signal_state_t *ss, uint32_t mask);
void signal_unblock(signal_state_t *ss, uint32_t mask);

#endif /* SIGNAL_H */
