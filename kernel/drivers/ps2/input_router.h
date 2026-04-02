/* kernel/input_router.h — Per-task keyboard input queues
 *
 * FIX 1.11 / 2.1 — replaces the single global keyboard ring buffer that
 * all tasks competed for.  Now each task has its own 64-character circular
 * queue; the IRQ handler delivers characters only to the focused task.
 *
 * API:
 *   input_router_init()          — called once during kernel init
 *   input_router_set_focus(id)   — set the focused task (desktop calls this)
 *   input_router_get_focus()     — query current focus
 *   input_router_push(c)         — called from keyboard IRQ; routes to focused task
 *   input_router_getchar(id)     — non-blocking; returns -1 if queue empty
 *   input_router_getchar_wait(id)— blocks (sched_yield loop) until a char arrives
 *   input_router_flush(id)       — discard all queued chars for a task
 *   input_router_task_exit(id)   — called from sched_exit to clean up queue
 */
#ifndef INPUT_ROUTER_H
#define INPUT_ROUTER_H

#include "../../types.h"
#include "../../sched/sched.h"

#define IR_QUEUE_SIZE  64    /* chars per task queue (power of 2) */
#define IR_QUEUE_MASK  (IR_QUEUE_SIZE - 1)

typedef struct {
    volatile char buf[IR_QUEUE_SIZE];
    volatile int  head;
    volatile int  tail;
} ir_queue_t;

/* Initialise the router (zero all queues, set focus to task 0) */
void input_router_init(void);

/* Focus management — the focused task receives all keyboard input */
void input_router_set_focus(int task_id);
int  input_router_get_focus(void);

/* Called from keyboard IRQ handler — routes char to focused task's queue */
void input_router_push(char c);

/* Non-blocking read for task_id's queue. Returns char (0-255) or -1 if empty */
int  input_router_getchar(int task_id);

/* Blocking read — yields until a char is available for task_id */
char input_router_getchar_wait(int task_id);

/* Discard all chars queued for task_id (e.g. on app switch) */
void input_router_flush(int task_id);

/* Notify router that a task is exiting — flushes its queue */
void input_router_task_exit(int task_id);

#endif /* INPUT_ROUTER_H */
