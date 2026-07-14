/* kernel/linux/linux_syscall.h
 *
 * Linux i386 (32-bit) syscall numbers for INT 0x80.
 * Numbers from arch/x86/entry/syscalls/syscall_32.tbl (Linux v6.x).
 */
#ifndef LINUX_SYSCALL_H
#define LINUX_SYSCALL_H

#include "../types.h"
#include "../arch/x86_64/irq.h"

/* ---- Linux i386 syscall numbers ----------------------------------------- */
#define LX_SYS_restart_syscall  0
#define LX_SYS_exit             1
#define LX_SYS_fork             2
#define LX_SYS_read             3
#define LX_SYS_write            4
#define LX_SYS_open             5
#define LX_SYS_close            6
#define LX_SYS_waitpid          7
#define LX_SYS_creat            8
#define LX_SYS_link             9
#define LX_SYS_unlink           10
#define LX_SYS_execve           11
#define LX_SYS_chdir            12
#define LX_SYS_time             13
#define LX_SYS_mknod            14
#define LX_SYS_chmod            15
#define LX_SYS_lseek            19
#define LX_SYS_getpid           20
#define LX_SYS_mount            21
#define LX_SYS_access           33
#define LX_SYS_dup              41
#define LX_SYS_pipe             42
#define LX_SYS_brk              45
#define LX_SYS_ioctl            54
#define LX_SYS_fcntl            55
#define LX_SYS_dup2             63
#define LX_SYS_getppid          64
#define LX_SYS_stat             106
#define LX_SYS_lstat            107
#define LX_SYS_fstat            108
#define LX_SYS_wait4            114
#define LX_SYS_uname            122
#define LX_SYS_mprotect         125
#define LX_SYS_readlink         85
#define LX_SYS_mmap             90
#define LX_SYS_munmap           91
#define LX_SYS_getcwd           183
#define LX_SYS_mmap2            192
#define LX_SYS_exit_group       252

#define LX_NR_SYSCALLS          256

/* ---- Syscall handler type ----------------------------------------------- */
typedef int32_t (*linux_syscall_t)(struct isr_frame *frame);

/* Public table built by linux_syscall_table_init() */
extern linux_syscall_t linux_syscall_table[LX_NR_SYSCALLS];

/* Init the table */
void linux_syscall_table_init(void);

/* Dispatch entry point — called from syscall.c */
void linux_syscall_dispatch(struct isr_frame *frame);

/* Boot-time init */
void linux_compat_init(void);

#endif /* LINUX_SYSCALL_H */
