/* kernel/syscall.h — System call interface (INT 0x80) */
#ifndef SYSCALL_H
#include "../../types.h"
#define SYSCALL_H

#include "irq.h"

/* Syscall numbers */
#define SYS_WRITE  1    /* write(fd, buf, len) → bytes written */
#define SYS_EXIT   2    /* exit(code)          → noreturn      */
#define SYS_READ   3    /* read(fd, buf, len)  → bytes read    */
#define SYS_OPEN   4    /* open(path, flags)   → fd or -1      */
#define SYS_CLOSE  5    /* close(fd)           → 0 or -1       */

/* Initialise syscall handler (registers INT 0x80) */
void syscall_init(void);

#endif /* SYSCALL_H */
