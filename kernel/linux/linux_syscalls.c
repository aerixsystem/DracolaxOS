/* kernel/linux/linux_syscalls.c
 *
 * Linux i386 syscall implementations.
 * Each function receives the ISR frame; args are in EBX, ECX, EDX, ESI, EDI.
 * Linux i386 calling convention:  eax=nr  ebx=a1  ecx=a2  edx=a3
 *                                  esi=a4  edi=a5  ebp=a6
 * Return value goes into frame->rax.
 */
#include "../types.h"
#include "../arch/x86_64/irq.h"
#include "../sched/sched.h"
#include "../sched/task.h"
#include "../fs/vfs.h"
#include "../drivers/ps2/keyboard.h"
#include "../drivers/vga/vga.h"
#include "../log.h"
#include "../klibc.h"
#include "../arch/x86_64/pic.h"
#include "../fs/ramfs.h"
#include "../ipc/signal.h"
#include "linux_process.h"
#include "linux_syscall.h"
#include "linux_fs.h"
#include "linux_types.h"
#include "linux_memory.h"
#include "include-uapi/asm-generic/fcntl.h"
#include "include-uapi/asm-generic/errno.h"
#include "../uaccess.h"
#include "../mm/vmm.h"

/* ---- helpers ------------------------------------------------------------ */

static inline task_t *curtask(void) { return sched_current(); }

#define A1 ((uint32_t)(frame->rbx))
#define A2 ((uint32_t)(frame->rcx))
#define A3 ((uint32_t)(frame->rdx))
#define A4 ((uint32_t)(frame->rsi))
#define A5 ((uint32_t)(frame->rdi))

/* Utsname structure used by uname(2) */
typedef struct { char sysname[65]; char nodename[65]; char release[65];
                 char version[65]; char machine[65]; char domainname[65]; } lx_utsname;

/* ---- read ---------------------------------------------------------------- */
int32_t lx_sys_read(struct isr_frame *frame) {
    int         fd  = (int)A1;
    char       *buf = (char *)(uintptr_t)A2;
    uint32_t    len = A3;

    /* Validate user pointer before touching user memory */
    if (!buf || !access_ok(buf, len)) return -EFAULT;

    if (fd == 0) {
        /* stdin — read from keyboard into kernel buffer, then copy to user */
        char kbuf[256];
        uint32_t i;
        uint32_t chunk = len < sizeof(kbuf) ? len : (uint32_t)sizeof(kbuf);
        for (i = 0; i < chunk; i++) {
            int c = keyboard_read();
            kbuf[i] = (char)c;
            vga_putchar(c);
            if (c == '\n') { i++; break; }
        }
        if (copy_to_user(buf, kbuf, i) != 0) return -EFAULT;
        return (int32_t)i;
    }
    vfs_node_t *node = lx_fd_get(curtask(), fd);
    if (!node) return -EBADF;

    /* Read into kernel buffer, then copy to user */
    uint8_t *kbuf = (uint8_t *)kmalloc(len);
    if (!kbuf) return -ENOMEM;
    int n = vfs_read(node, 0, len, kbuf);
    if (n > 0 && copy_to_user(buf, kbuf, (size_t)n) != 0) {
        kfree(kbuf); return -EFAULT;
    }
    kfree(kbuf);
    return (n < 0) ? -EIO : n;
}

/* ---- write --------------------------------------------------------------- */
int32_t lx_sys_write(struct isr_frame *frame) {
    int          fd  = (int)A1;
    const char  *buf = (const char *)(uintptr_t)A2;
    uint32_t     len = A3;

    /* Validate user pointer */
    if (!buf || !access_ok(buf, len)) return -EFAULT;

    if (fd == 1 || fd == 2) {
        /* Copy from user into kernel buffer before touching bytes */
        char kbuf[256];
        uint32_t off = 0;
        while (off < len) {
            uint32_t chunk = (len - off) < sizeof(kbuf)
                             ? (len - off) : (uint32_t)sizeof(kbuf);
            if (copy_from_user(kbuf, buf + off, chunk) != 0) return -EFAULT;
            for (uint32_t i = 0; i < chunk; i++) vga_putchar(kbuf[i]);
            off += chunk;
        }
        return (int32_t)len;
    }
    vfs_node_t *node = lx_fd_get(curtask(), fd);
    if (!node) return -EBADF;

    /* Copy from user then write to VFS */
    uint8_t *kbuf = (uint8_t *)kmalloc(len);
    if (!kbuf) return -ENOMEM;
    if (copy_from_user(kbuf, buf, len) != 0) { kfree(kbuf); return -EFAULT; }
    int n = vfs_write(node, 0, len, kbuf);
    kfree(kbuf);
    return (n < 0) ? -EIO : (int32_t)len;
}

/* ---- open ---------------------------------------------------------------- */
int32_t lx_sys_open(struct isr_frame *frame) {
    const char *path  = (const char *)(uintptr_t)A1;
    int         flags = (int)A2;
    /* mode = A3 (unused yet) */

    if (!path) return -EFAULT;

    vfs_node_t *node = lx_path_resolve(path);

    if (!node && (flags & O_CREAT)) {
        /* Try to create in /ramfs */
        extern vfs_node_t *ramfs_root;
        if (ramfs_root) {
            const char *name = path;
            /* strip /ramfs/ prefix */
            if (strncmp(path, "/ramfs/", 7) == 0) name = path + 7;
            if (ramfs_create(ramfs_root, name) == 0)
                node = vfs_finddir(ramfs_root, name);
        }
    }
    if (!node) return -ENOENT;

    int fd = lx_fd_alloc(curtask(), node);
    return fd;
}

/* ---- close --------------------------------------------------------------- */
int32_t lx_sys_close(struct isr_frame *frame) {
    int fd = (int)A1;
    if (fd < 3) return 0; /* can't close stdin/stdout/stderr */
    if (!lx_fd_get(curtask(), fd)) return -EBADF;
    lx_fd_free(curtask(), fd);
    return 0;
}

/* ---- lseek --------------------------------------------------------------- */
int32_t lx_sys_lseek(struct isr_frame *frame) {
    /* We don't track per-fd offsets yet; return 0 to not crash callers */
    (void)frame;
    return 0;
}

/* ---- exit / exit_group --------------------------------------------------- */
int32_t lx_sys_exit(struct isr_frame *frame) {
    task_t *t = curtask();
    t->exit_code = (int32_t)A1;
    kinfo("linux task %u exited with code %d\n", t->pid, (int)A1);
    /* FIX 3.8: notify parent via SIGCHLD before exiting */
    signal_notify_parent((int)t->ppid, (int)t->id, (int)A1);
    sched_exit();
}

int32_t lx_sys_exit_group(struct isr_frame *frame) {
    return lx_sys_exit(frame);
}

/* ---- getpid / getppid ---------------------------------------------------- */
int32_t lx_sys_getpid (struct isr_frame *f) { (void)f; return (int32_t)curtask()->pid;  }
int32_t lx_sys_getppid(struct isr_frame *f) { (void)f; return (int32_t)curtask()->ppid; }

/* ---- getuid / getgid ----------------------------------------------------- */
int32_t lx_sys_getuid(struct isr_frame *f) { (void)f; return 0; }
int32_t lx_sys_getgid(struct isr_frame *f) { (void)f; return 0; }
int32_t lx_sys_setuid(struct isr_frame *f) { (void)f; return 0; }
int32_t lx_sys_setgid(struct isr_frame *f) { (void)f; return 0; }

/* ---- fork ---------------------------------------------------------------- */
int32_t lx_sys_fork(struct isr_frame *frame) {
    (void)frame;
    return lx_fork_task();
}

/* ---- execve -------------------------------------------------------------- */
int32_t lx_sys_execve(struct isr_frame *frame) {
    const char   *path = (const char *)(uintptr_t)A1;
    char *const  *argv = (char *const *)(uintptr_t)A2;
    char *const  *envp = (char *const *)(uintptr_t)A3;
    return (int32_t)lx_execve_impl(path, argv, envp);
}

/* ---- waitpid / wait4 ----------------------------------------------------- */
int32_t lx_sys_waitpid(struct isr_frame *frame) {
    int  pid     = (int)A1;
    int *wstatus = (int *)(uintptr_t)A2;
    int  options = (int)A3;
    return (int32_t)lx_waitpid_impl(pid, wstatus, options);
}

int32_t lx_sys_wait4(struct isr_frame *frame) {
    return lx_sys_waitpid(frame);
}

/* ---- mmap / mmap2 / munmap / brk / mprotect ------------------------------ */
int32_t lx_sys_mmap(struct isr_frame *frame) {
    return (int32_t)lx_mmap(A1, A2, (int)A3, (int)A4, (int)A5,
                              (uint32_t)frame->rdi);
}
int32_t lx_sys_mmap2(struct isr_frame *frame) {
    /* mmap2: offset in 4096-byte units */
    return (int32_t)lx_mmap(A1, A2, (int)A3, (int)A4, (int)A5,
                              A5 * 4096);
}
int32_t lx_sys_munmap(struct isr_frame *frame) {
    return lx_munmap(A1, A2);
}
int32_t lx_sys_brk(struct isr_frame *frame) {
    return (int32_t)lx_brk(A1);
}
int32_t lx_sys_mprotect(struct isr_frame *frame) {
    (void)frame; return 0; /* stub: accept all mprotect calls */
}

/* ---- dup / dup2 / pipe --------------------------------------------------- */
int32_t lx_sys_dup(struct isr_frame *frame) {
    int fd = (int)A1;
    vfs_node_t *node = lx_fd_get(curtask(), fd);
    if (!node && fd > 2) return -EBADF;
    return lx_fd_alloc(curtask(), node);
}
int32_t lx_sys_dup2(struct isr_frame *frame) {
    int oldfd = (int)A1;
    int newfd = (int)A2;
    if (newfd < 0 || newfd >= TASK_FD_MAX) return -EBADF;
    vfs_node_t *node = lx_fd_get(curtask(), oldfd);
    if (!node && oldfd > 2) return -EBADF;
    curtask()->fd_table[newfd] = node;
    return newfd;
}
int32_t lx_sys_pipe(struct isr_frame *frame) {
    /* Pipe not fully implemented; return -ENOSYS */
    (void)frame; return -ENOSYS;
}

/* ---- stat / fstat / lstat ------------------------------------------------ */
static int do_stat(vfs_node_t *node, lx_stat_t *st) {
    if (!node || !st) return -EFAULT;
    lx_fill_stat(st, node);
    return 0;
}
int32_t lx_sys_stat(struct isr_frame *frame) {
    vfs_node_t *n = lx_path_resolve((const char *)(uintptr_t)A1);
    return do_stat(n, (lx_stat_t *)(uintptr_t)A2);
}
int32_t lx_sys_lstat(struct isr_frame *frame) {
    return lx_sys_stat(frame);
}
int32_t lx_sys_fstat(struct isr_frame *frame) {
    vfs_node_t *n = lx_fd_get(curtask(), (int)A1);
    return do_stat(n, (lx_stat_t *)(uintptr_t)A2);
}

/* ---- access -------------------------------------------------------------- */
int32_t lx_sys_access(struct isr_frame *frame) {
    vfs_node_t *n = lx_path_resolve((const char *)(uintptr_t)A1);
    return n ? 0 : -ENOENT;
}

/* ---- readlink ------------------------------------------------------------ */
int32_t lx_sys_readlink(struct isr_frame *frame) {
    const char *path = (const char *)(uintptr_t)A1;
    char       *buf  = (char *)(uintptr_t)A2;
    uint32_t    sz   = A3;
    /* /proc/self → pid string */
    if (strcmp(path, "/proc/self/exe") == 0) {
        strncpy(buf, curtask()->name, sz);
        return (int32_t)strlen(buf);
    }
    return -ENOENT;
}

/* ---- getcwd -------------------------------------------------------------- */
int32_t lx_sys_getcwd(struct isr_frame *frame) {
    char    *buf = (char *)(uintptr_t)A1;
    uint32_t sz  = A2;
    if (!buf) return -EFAULT;
    strncpy(buf, "/", sz);
    return 1;
}

/* ---- chdir --------------------------------------------------------------- */
int32_t lx_sys_chdir(struct isr_frame *frame) {
    (void)frame;
    return 0; /* no cwd tracking yet */
}

/* ---- ioctl --------------------------------------------------------------- */
int32_t lx_sys_ioctl(struct isr_frame *frame) {
    /* Return 0 for TIOCGWINSZ (0x5413) so shells don't crash */
    uint32_t req = A2;
    if (req == 0x5413) { /* TIOCGWINSZ */
        struct { uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel; }
            *ws = (void *)(uintptr_t)A3;
        if (ws) { ws->ws_row=25; ws->ws_col=80; ws->ws_xpixel=0; ws->ws_ypixel=0; }
        return 0;
    }
    return -ENOTTY;
}

/* ---- fcntl --------------------------------------------------------------- */
/* FIX: implement the two most-used fcntl commands so ELF binaries that
 * call fcntl(fd, F_GETFL) / fcntl(fd, F_SETFL, flags) don't silently
 * fail.  Full descriptor-flag storage is a V2 item; for now we return
 * O_RDWR for F_GETFL and accept (ignore) F_SETFL flags. */
#ifndef F_GETFL
#define F_GETFL  3
#define F_SETFL  4
#define O_RDWR   2
#endif
int32_t lx_sys_fcntl(struct isr_frame *frame) {
    int cmd = (int)A2;
    switch (cmd) {
    case F_GETFL: return O_RDWR;   /* report all fds as read-write */
    case F_SETFL: return 0;        /* accept any flag change silently */
    default:      return -EINVAL;
    }
}

/* ---- uname --------------------------------------------------------------- */
int32_t lx_sys_uname(struct isr_frame *frame) {
    lx_utsname *u = (lx_utsname *)(uintptr_t)A1;
    if (!u) return -EFAULT;
    strncpy(u->sysname,    "DracolaxOS",   65);
    strncpy(u->nodename,   "dracolax",     65);
    strncpy(u->release,    "1.0.0",        65);
    strncpy(u->version,    "#1 SMP",       65);
    strncpy(u->machine,    "i686",         65);
    strncpy(u->domainname, "(none)",       65);
    return 0;
}

/* ---- writev -------------------------------------------------------------- */
int32_t lx_sys_writev(struct isr_frame *frame) {
    int fd = (int)A1;
    lx_iovec_t *iov = (lx_iovec_t *)(uintptr_t)A2;
    int iovcnt = (int)A3;
    int total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!iov[i].iov_base) continue;
        /* Reuse write logic */
        struct isr_frame tmp = *frame;
        tmp.rbx = (uint64_t)fd;
        tmp.rcx = (uint64_t)iov[i].iov_base;
        tmp.rdx = (uint64_t)iov[i].iov_len;
        int r = (int)lx_sys_write(&tmp);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

/* ---- unlink -------------------------------------------------------------- */
int32_t lx_sys_unlink(struct isr_frame *frame) {
    extern vfs_node_t *ramfs_root;
    const char *path = (const char *)(uintptr_t)A1;
    if (!path) return -EFAULT;
    const char *name = path;
    if (strncmp(path, "/ramfs/", 7) == 0) name = path + 7;
    if (ramfs_root && ramfs_delete(ramfs_root, name) == 0) return 0;
    return -ENOENT;
}

/* ---- kill (FIX 3.8) ------------------------------------------------------ */
int32_t lx_sys_kill(struct isr_frame *frame) {
    int pid = (int)(int32_t)A1;
    int sig = (int)(int32_t)A2;
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    /* pid > 0: send to that task id.  pid == 0: send to current task group
     * (simplified: send to current task). */
    int target = (pid > 0) ? pid : (int)sched_current()->id;
    int r = signal_send(target, sig);
    return r == 0 ? 0 : -ESRCH;
}
