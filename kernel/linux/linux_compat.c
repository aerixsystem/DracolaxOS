/* kernel/linux/linux_compat.c — Boot-time init and dispatch */
#include "../types.h"
#include "../arch/x86_64/irq.h"
#include "../log.h"
#include "linux_syscall.h"

/* Dispatch a Linux syscall — called from syscall.c */
void linux_syscall_dispatch(struct isr_frame *frame) {
    uint32_t nr = frame->rax;
    if (nr >= LX_NR_SYSCALLS) {
        frame->rax = (uint32_t)-38; /* ENOSYS */
        return;
    }
    linux_syscall_t fn = linux_syscall_table[nr];
    frame->rax = (uint32_t)fn(frame);
}

void linux_compat_init(void) {
    linux_syscall_table_init();
    kinfo("LINUX: compatibility layer ready (%d syscalls)\n", LX_NR_SYSCALLS);
}
