/* include/linux/syscalls.h — minimal stub
 * Full Linux header includes kernel-internal types. This stub only
 * declares what DracolaxOS linux-compat needs. */
#ifndef _LINUX_SYSCALLS_H
#define _LINUX_SYSCALLS_H

/* Syscall entry point attribute */
#define SYSCALL_DEFINE0(name)           long sys_##name(void)
#define SYSCALL_DEFINE1(name,t1,a1)     long sys_##name(t1 a1)
#define SYSCALL_DEFINE2(name,t1,a1,t2,a2) \
        long sys_##name(t1 a1, t2 a2)
#define SYSCALL_DEFINE3(name,t1,a1,t2,a2,t3,a3) \
        long sys_##name(t1 a1, t2 a2, t3 a3)

#endif /* _LINUX_SYSCALLS_H */
