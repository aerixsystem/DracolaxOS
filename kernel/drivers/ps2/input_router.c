/* kernel/input_router.c — Per-task keyboard input queues
 *
 * Replaces the single global keyboard ring that all tasks competed for
 * (audit bugs 1.11 / 2.1).  Each task slot has its own 64-char circular
 * queue.  The keyboard IRQ handler calls input_router_push(c) which writes
 * the character only into the currently focused task's queue.
 *
 * Interrupt safety:
 *   input_router_push() is called from IRQ context.  It only writes to
 *   one queue's tail pointer (an atomic 32-bit write on x86).  Readers
 *   poll the head pointer.  No lock is needed because:
 *     - Only the IRQ handler writes tail.
 *     - Only the owner task reads head.
 *   The focused-task id is read by the IRQ handler and written by the
 *   desktop task; on x86 int-aligned reads/writes are atomic.
 */
#include "input_router.h"
#include "../../klibc.h"
#include "../../log.h"

/* One queue per task slot */
static ir_queue_t g_queues[TASK_MAX];

/* Global focused task id — IRQ handler routes input here */
static volatile int g_focused = 0;

/* Desktop task id — Super+letter shortcuts are always routed here regardless
 * of which app has keyboard focus.  Set by the desktop at startup. */
volatile int g_desktop_task = 0;

void input_router_set_desktop_task(int task_id) {
    if (task_id >= 0 && task_id < TASK_MAX)
        g_desktop_task = task_id;
}

/* ---- init ---------------------------------------------------------------- */

void input_router_init(void) {
    for (int i = 0; i < TASK_MAX; i++) {
        g_queues[i].head = 0;
        g_queues[i].tail = 0;
    }
    g_focused = 0;
    kinfo("INPUT_ROUTER: per-task queues initialised (%d slots)\n", TASK_MAX);
}

/* ---- focus --------------------------------------------------------------- */

void input_router_set_focus(int task_id) {
    if (task_id < 0 || task_id >= TASK_MAX) return;
    if (g_focused != task_id) {
        kinfo("INPUT_ROUTER: focus -> task %d\n", task_id);
        g_focused = task_id;
    }
}

int input_router_get_focus(void) {
    return g_focused;
}

/* ---- push (called from IRQ) ---------------------------------------------- */

void input_router_push(char c) {
    int id = g_focused;
    if (id < 0 || id >= TASK_MAX) return;
    ir_queue_t *q = &g_queues[id];
    int next = (q->tail + 1) & IR_QUEUE_MASK;
    if (next == q->head) return;  /* queue full — drop (prevents stall) */
    q->buf[q->tail] = c;
    q->tail = next;
}

/* ---- push_to (task-context safe — push to any task's queue) -------------- */

void input_router_push_to(int task_id, char c) {
    if (task_id < 0 || task_id >= TASK_MAX) return;
    ir_queue_t *q = &g_queues[task_id];
    int next = (q->tail + 1) & IR_QUEUE_MASK;
    if (next == q->head) return;  /* full — drop */
    q->buf[q->tail] = c;
    q->tail = next;
}

/* ---- getchar (non-blocking) ---------------------------------------------- */

int input_router_getchar(int task_id) {
    if (task_id < 0 || task_id >= TASK_MAX) return -1;
    ir_queue_t *q = &g_queues[task_id];
    if (q->head == q->tail) return -1;   /* empty */
    char c = q->buf[q->head];
    q->head = (q->head + 1) & IR_QUEUE_MASK;
    return (unsigned char)c;
}

/* ---- getchar_wait (blocking) --------------------------------------------- */

char input_router_getchar_wait(int task_id) {
    int c;
    while ((c = input_router_getchar(task_id)) < 0)
        sched_yield();
    return (char)c;
}

/* ---- flush --------------------------------------------------------------- */

void input_router_flush(int task_id) {
    if (task_id < 0 || task_id >= TASK_MAX) return;
    g_queues[task_id].head = g_queues[task_id].tail;
}

/* ---- task exit ----------------------------------------------------------- */

/* BUG FIX (keyboard dead after closing any window, mouse/shortcuts still work):
 * Root cause — when an app closes itself by handling Esc internally (the
 * normal path: the title bar says "Esc=close", the user presses Esc while
 * the window has focus, the app's own loop reads it from its OWN queue
 * since g_focused == that app's task id, then calls comp_destroy_window()
 * + sched_exit()) — desktop.c's close_window() is NEVER involved, so its
 * input_router_set_focus(desktop_tid) never runs. sched_exit() calls this
 * function, which used to hardcode g_focused = 0 whenever the exiting task
 * still held focus. Task 0 is NOT the desktop task — it's whichever task
 * happens to occupy slot 0 (typically init_task), which never reads from
 * its input queue. So global focus permanently "leaked" to a task that
 * never consumes it: every subsequently typed character (routed via
 * input_router_push() -> g_queues[g_focused]) vanished into a queue
 * nobody drains. Mouse input doesn't go through this path at all, and
 * Super+letter shortcuts bypass g_focused entirely via
 * input_router_push_to(g_desktop_task, ...) — which is exactly why those
 * kept working while normal typing went silently dead.
 *
 * Fix: reassign focus to g_desktop_task (the real, tracked desktop task
 * id — see input_router_set_desktop_task()), not the literal slot 0. This
 * also self-heals the explicit close_window()-driven paths: if focus was
 * already moved to desktop_tid before the app's sched_exit() runs, this
 * check (g_focused == task_id) is simply false and nothing changes. */
void input_router_task_exit(int task_id) {
    input_router_flush(task_id);
    if (g_focused == task_id)
        g_focused = g_desktop_task;
}
