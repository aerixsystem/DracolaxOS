/* kernel/syscall.c — INT 0x80 syscall dispatcher
 *
 * Routes by task ABI:
 *   ABI_DRACO  → Draco native syscall numbers
 *   ABI_LINUX  → Linux x86_64 syscall numbers (linux_compat layer)
 */
#include "../../types.h"
#include "syscall.h"
#include "irq.h"
#include "../../fs/vfs.h"
#include "../../drivers/ps2/keyboard.h"
#include "../../drivers/vga/vga.h"
#include "../../sched/sched.h"
#include "../../log.h"
#include "../../klibc.h"
#include "../../linux/linux_syscall.h"
#include "../../uaccess.h"

/* ---- Draco native syscall numbers -------------------------------------- */
/* Kept separate from Linux numbers to avoid collisions. */
#define SYS_WRITE    1   /* write(fd, buf, len)   */
#define SYS_READ     3   /* read(fd, buf, len)    */
#define SYS_EXIT     2   /* exit(code)            */
#define SYS_EXEC     4   /* exec(path)  — Draco   */

/* ---- Draco native dispatch --------------------------------------------- */

static int draco_sys_write(uint32_t fd, const char *buf, uint32_t len) {
    /* Validate user buffer before touching it */
    if (!access_ok(buf, len)) {
        kwarn("SYS_WRITE: bad user buf 0x%llx len=%u\n",
              (unsigned long long)(uintptr_t)buf, len);
        return -14; /* -EFAULT */
    }
    if (fd == 1 || fd == 2) {
        /* Copy to kernel stack to avoid TOCTOU on user buffer */
        char kbuf[256];
        uint32_t remaining = len;
        uint32_t off = 0;
        while (remaining > 0) {
            uint32_t chunk = remaining < sizeof(kbuf) ? remaining : (uint32_t)sizeof(kbuf);
            if (copy_from_user(kbuf, buf + off, chunk) != 0) return -14;
            for (uint32_t i = 0; i < chunk; i++) vga_putchar(kbuf[i]);
            off += chunk;
            remaining -= chunk;
        }
        return (int)len;
    }
    return -1;
}

static int draco_sys_read(uint32_t fd, char *buf, uint32_t len) {
    if (!access_ok(buf, len)) {
        kwarn("SYS_READ: bad user buf 0x%llx len=%u\n",
              (unsigned long long)(uintptr_t)buf, len);
        return -14; /* -EFAULT */
    }
    if (fd == 0) {
        char kbuf[256];
        uint32_t collected = 0;
        while (collected < len) {
            int c = keyboard_read();
            kbuf[collected] = (char)c;
            collected++;
            if (c == '\n' || collected >= sizeof(kbuf)) break;
        }
        if (copy_to_user(buf, kbuf, collected) != 0) return -14;
        return (int)collected;
    }
    return -1;
}

__attribute__((noreturn))
static void draco_sys_exit(uint32_t code) {
    kinfo("SYSCALL: exit(%u)\n", code);
    sched_exit();
}

static void draco_syscall_dispatch(struct isr_frame *frame) {
    uint64_t nr  = frame->rax;
    uint64_t a1  = frame->rbx;
    uint64_t a2  = frame->rcx;
    uint64_t a3  = frame->rdx;
    int64_t  ret = -1;

    switch (nr) {
    case SYS_WRITE: ret = draco_sys_write((uint32_t)a1, (const char *)(uintptr_t)a2, (uint32_t)a3); break;
    case SYS_READ:  ret = draco_sys_read ((uint32_t)a1, (char *)(uintptr_t)a2, (uint32_t)a3);       break;
    case SYS_EXIT:  draco_sys_exit((uint32_t)a1); /* noreturn */
    default:
        kwarn("SYSCALL (draco): unknown nr=%u\n", (unsigned)nr);
        ret = -1;
        break;
    }
    frame->rax = (uint64_t)ret;
}

/* ---- Main handler -------------------------------------------------------- */

void syscall_handler(struct isr_frame *frame) {
    task_t *t = sched_current();

    if (t && t->abi == ABI_LINUX) {
        linux_syscall_dispatch(frame);
    } else {
        draco_syscall_dispatch(frame);
    }
}

/* Called from the SYSCALL asm stub in ring3.c — same dispatch as INT 0x80 */
void syscall_c_entry_from_syscall(struct isr_frame *frame) {
    syscall_handler(frame);
}

void syscall_init(void) {
    irq_register(128, syscall_handler);
    kinfo("SYSCALL: INT 0x80 registered (Draco + Linux ABI)\n");
}
