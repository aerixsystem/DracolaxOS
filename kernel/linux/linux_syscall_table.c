/* kernel/linux/linux_syscall_table.c
 *
 * Maps Linux i386 syscall numbers to handler function pointers.
 * Numbers sourced from arch/x86/entry/syscalls/syscall_32.tbl (Linux v6.x).
 *
 * Only the ~40 syscalls listed in the task spec are wired; the rest
 * return -ENOSYS so binaries that probe for unsupported calls still run.
 */
#include "../types.h"
#include "../arch/x86_64/irq.h"
#include "linux_syscall.h"

/* Forward declarations (implemented in linux_syscalls.c) */
extern int32_t lx_sys_exit       (struct isr_frame *f);
extern int32_t lx_sys_read       (struct isr_frame *f);
extern int32_t lx_sys_write      (struct isr_frame *f);
extern int32_t lx_sys_open       (struct isr_frame *f);
extern int32_t lx_sys_close      (struct isr_frame *f);
extern int32_t lx_sys_waitpid    (struct isr_frame *f);
extern int32_t lx_sys_execve     (struct isr_frame *f);
extern int32_t lx_sys_lseek      (struct isr_frame *f);
extern int32_t lx_sys_getpid     (struct isr_frame *f);
extern int32_t lx_sys_access     (struct isr_frame *f);
extern int32_t lx_sys_dup        (struct isr_frame *f);
extern int32_t lx_sys_pipe       (struct isr_frame *f);
extern int32_t lx_sys_brk        (struct isr_frame *f);
extern int32_t lx_sys_dup2       (struct isr_frame *f);
extern int32_t lx_sys_getppid    (struct isr_frame *f);
extern int32_t lx_sys_ioctl      (struct isr_frame *f);
extern int32_t lx_sys_fcntl      (struct isr_frame *f);
extern int32_t lx_sys_readlink   (struct isr_frame *f);
extern int32_t lx_sys_mmap       (struct isr_frame *f);
extern int32_t lx_sys_munmap     (struct isr_frame *f);
extern int32_t lx_sys_stat       (struct isr_frame *f);
extern int32_t lx_sys_lstat      (struct isr_frame *f);
extern int32_t lx_sys_fstat      (struct isr_frame *f);
extern int32_t lx_sys_wait4      (struct isr_frame *f);
extern int32_t lx_sys_uname      (struct isr_frame *f);
extern int32_t lx_sys_mprotect   (struct isr_frame *f);
extern int32_t lx_sys_exit_group (struct isr_frame *f);
extern int32_t lx_sys_getcwd     (struct isr_frame *f);
extern int32_t lx_sys_mmap2      (struct isr_frame *f);
extern int32_t lx_sys_writev     (struct isr_frame *f);
extern int32_t lx_sys_unlink     (struct isr_frame *f);
extern int32_t lx_sys_kill       (struct isr_frame *f);
extern int32_t lx_sys_chdir      (struct isr_frame *f);
extern int32_t lx_sys_fork       (struct isr_frame *f);
extern int32_t lx_sys_getgid     (struct isr_frame *f);
extern int32_t lx_sys_getuid     (struct isr_frame *f);
extern int32_t lx_sys_setuid     (struct isr_frame *f);
extern int32_t lx_sys_setgid     (struct isr_frame *f);

/* Stub: returns -ENOSYS */
static int32_t lx_sys_enosys(struct isr_frame *f) {
    (void)f;
    return -38; /* ENOSYS */
}

/* Table indexed by Linux i386 syscall number.
 * Entries not listed are initialised to lx_sys_enosys at runtime. */
static linux_syscall_t _table[LX_NR_SYSCALLS];

linux_syscall_t linux_syscall_table[LX_NR_SYSCALLS]; /* public copy */

void linux_syscall_table_init(void) {
    /* Fill all entries with enosys stub first */
    for (int i = 0; i < LX_NR_SYSCALLS; i++)
        _table[i] = lx_sys_enosys;

    /* Wire supported syscalls */
    _table[LX_SYS_exit]        = lx_sys_exit;
    _table[LX_SYS_read]        = lx_sys_read;
    _table[LX_SYS_write]       = lx_sys_write;
    _table[LX_SYS_open]        = lx_sys_open;
    _table[LX_SYS_close]       = lx_sys_close;
    _table[LX_SYS_waitpid]     = lx_sys_waitpid;
    _table[LX_SYS_execve]      = lx_sys_execve;
    _table[LX_SYS_chdir]       = lx_sys_chdir;
    _table[LX_SYS_lseek]       = lx_sys_lseek;
    _table[LX_SYS_getpid]      = lx_sys_getpid;
    _table[LX_SYS_access]      = lx_sys_access;
    _table[LX_SYS_dup]         = lx_sys_dup;
    _table[LX_SYS_pipe]        = lx_sys_pipe;
    _table[LX_SYS_brk]         = lx_sys_brk;
    _table[LX_SYS_ioctl]       = lx_sys_ioctl;
    _table[LX_SYS_fcntl]       = lx_sys_fcntl;
    _table[LX_SYS_dup2]        = lx_sys_dup2;
    _table[LX_SYS_getppid]     = lx_sys_getppid;
    _table[LX_SYS_readlink]    = lx_sys_readlink;
    _table[LX_SYS_mmap]        = lx_sys_mmap;
    _table[LX_SYS_munmap]      = lx_sys_munmap;
    _table[LX_SYS_stat]        = lx_sys_stat;
    _table[LX_SYS_lstat]       = lx_sys_lstat;
    _table[LX_SYS_fstat]       = lx_sys_fstat;
    _table[LX_SYS_wait4]       = lx_sys_wait4;
    _table[LX_SYS_uname]       = lx_sys_uname;
    _table[LX_SYS_mprotect]    = lx_sys_mprotect;
    _table[LX_SYS_exit_group]  = lx_sys_exit_group;
    _table[LX_SYS_getcwd]      = lx_sys_getcwd;
    _table[LX_SYS_mmap2]       = lx_sys_mmap2;
    _table[LX_SYS_execve]      = lx_sys_execve;
    _table[LX_SYS_fork]        = lx_sys_fork;
    _table[20]                  = lx_sys_getpid; /* getuid */
    _table[24]                  = lx_sys_getgid;
    _table[47]                  = lx_sys_getgid;
    _table[199]                 = lx_sys_getuid;
    _table[200]                 = lx_sys_getgid;
    _table[201]                 = lx_sys_getuid;
    _table[202]                 = lx_sys_getgid;
    _table[146]                 = lx_sys_writev;
    _table[10]                  = lx_sys_unlink;
    _table[62]                  = lx_sys_kill;    /* kill(pid, sig) */

    for (int i = 0; i < LX_NR_SYSCALLS; i++)
        linux_syscall_table[i] = _table[i];
}
