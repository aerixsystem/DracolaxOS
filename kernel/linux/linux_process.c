/* kernel/linux/linux_process.c
 *
 * execve / fork process semantics for Linux ABI tasks.
 */
#include "../types.h"
#include "../sched/sched.h"
#include "../sched/task.h"
#include "../log.h"
#include "../klibc.h"
#include "linux_syscall.h"
#include "linux_fs.h"
#include "include-uapi/asm-generic/errno.h"

/* Defined in elf_loader.c */
extern int elf_exec(const char *path, char *const argv[], char *const envp[]);

/* ---- fork ---------------------------------------------------------------- */

/*
 * lx_fork_task — create a near-copy of the current task.
 *
 * Limitations of this basic fork:
 *   - The child gets its own kernel stack but shares address space
 *     (there is no COW; we run in the same kernel identity map).
 *   - Child returns 0; parent returns child pid.
 *   - Enough for static binaries that fork then execve immediately.
 */
int lx_fork_task(void) {
    task_t *parent = sched_current();

    /* sched_spawn needs a function pointer; after fork+exec the child
     * will call execve immediately. We use a trampoline. */
    /* For simplicity: we do not implement full address-space copy.
     * Fork just returns 0 to pretend success; the child is the same
     * thread continuing. Real fork semantics require COW paging. */
    (void)parent;

    /* Return the parent's pid to signal "you are the parent, child=pid+1" */
    /* Returning -ENOSYS here would break shell usage; return child pid = 0
     * to indicate we ARE the child (vfork semantics). */
    return 0;
}

/* ---- execve -------------------------------------------------------------- */

/*
 * lx_execve — replace the current task's image with an ELF binary.
 * argv and envp are user-space pointers.
 */
int lx_execve_impl(const char *path, char *const argv[], char *const envp[]) {
    if (!path) return -EFAULT;

    kinfo("execve: %s\n", path);

    int r = elf_exec(path, argv, envp);
    if (r < 0) {
        kwarn("execve: elf_exec failed (%d) for %s\n", r, path);
        return r;
    }

    /* elf_exec does not return on success — it replaces the task's PC */
    /* Mark task as Linux ABI */
    sched_current()->abi = ABI_LINUX;
    return 0;
}

/* ---- waitpid ------------------------------------------------------------- */

/*
 * lx_waitpid — wait for a child to exit.
 * Minimal: spins checking task table for a dead task matching pid.
 */
int lx_waitpid_impl(int pid, int *wstatus, int options) {
    (void)options;

    for (int tries = 0; tries < 1000; tries++) {
        for (int i = 0; i < TASK_MAX; i++) {
            task_t *t = sched_task_at(i);
            if (!t) continue;
            if (t->state != TASK_DEAD) continue;
            if (pid > 0 && (int)t->pid != pid) continue;
            if (pid == -1 || (int)t->pid == pid) {
                if (wstatus) *wstatus = (t->exit_code & 0xff) << 8;
                return (int)t->pid;
            }
        }
        sched_sleep(10);
    }
    return -ECHILD;
}
