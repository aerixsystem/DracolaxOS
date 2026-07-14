/* kernel/uaccess.h — Safe user ↔ kernel memory transfers
 *
 * Every syscall that touches a user-supplied pointer MUST go through
 * copy_from_user() or copy_to_user().  Direct dereference of a user
 * pointer from Ring 0 is a security vulnerability: a malicious process
 * can pass a kernel address and corrupt kernel state.
 *
 * Validation model:
 *   User addresses must lie in the range [USER_MEM_START, USER_MEM_END).
 *   Any pointer outside this range is rejected with -EFAULT.
 *   The buffer [ptr, ptr+n) must fit entirely within the user range.
 *
 * No page-fault-based recovery (#PF fixup table) is implemented yet —
 * instead we validate before touching, then use memcpy.  When proper
 * per-process page tables exist this can be upgraded.
 *
 * KASSERT — compile-time and runtime assertion macro.
 *   In DEBUG builds:   triggers kpanic() with file/line info on failure.
 *   In RELEASE builds: becomes a no-op (zero overhead in hot paths).
 */
#ifndef UACCESS_H
#define UACCESS_H

#include "types.h"
#include "klibc.h"
#include "log.h"

/* ── User address space bounds ────────────────────────────────────────── */
/* Ring-3 user tasks live at 0x400000–0x7FFF_FFFF_FFFF (Linux-style).
 * Kernel is mapped above 0xFFFF_8000_0000_0000 (higher-half, future).
 * For now: anything below 0x1000 (null page) or above USER_MEM_END is bad. */
#define USER_MEM_START  ((uintptr_t)0x0000000000001000ULL)   /* 4 KiB — skip null page */
#define USER_MEM_END    ((uintptr_t)0x00007FFFFFFFFFFFULL)   /* canonical user range    */

/* ── Pointer range validation ─────────────────────────────────────────── */

/* Returns 1 if the range [ptr, ptr+n) lies entirely within user space.
 * Rejects NULL, kernel addresses, and ranges that wrap around. */
static inline int access_ok(const void *ptr, size_t n) {
    uintptr_t base = (uintptr_t)ptr;
    if (!ptr)               return 0;   /* NULL pointer */
    if (base < USER_MEM_START) return 0; /* null-page or low kernel address */
    if (base > USER_MEM_END)   return 0; /* kernel address */
    if (n == 0)             return 1;   /* zero-length is always valid */
    /* Check that the entire range fits (guards against wrap-around) */
    if (base + n < base)    return 0;   /* overflow */
    if (base + n > USER_MEM_END + 1) return 0; /* extends into kernel */
    return 1;
}

/* ── copy_from_user ───────────────────────────────────────────────────── */
/* Copy n bytes from user-space src into kernel dst.
 * Returns 0 on success, -1 if src is not a valid user pointer. */
static inline int copy_from_user(void *kdst, const void *usrc, size_t n) {
    if (!access_ok(usrc, n)) {
        kerror("UACCESS: copy_from_user: invalid user ptr 0x%llx len=%u\n",
               (unsigned long long)(uintptr_t)usrc, (uint32_t)n);
        return -1;
    }
    memcpy(kdst, usrc, n);
    return 0;
}

/* ── copy_to_user ────────────────────────────────────────────────────── */
/* Copy n bytes from kernel src into user-space dst.
 * Returns 0 on success, -1 if dst is not a valid user pointer. */
static inline int copy_to_user(void *udst, const void *ksrc, size_t n) {
    if (!access_ok(udst, n)) {
        kerror("UACCESS: copy_to_user: invalid user ptr 0x%llx len=%u\n",
               (unsigned long long)(uintptr_t)udst, (uint32_t)n);
        return -1;
    }
    memcpy(udst, ksrc, n);
    return 0;
}

/* ── strncpy_from_user ────────────────────────────────────────────────── */
/* Copy a NUL-terminated string from user space into a kernel buffer.
 * At most (maxlen-1) chars are copied; dst is always NUL-terminated.
 * Returns number of chars copied (excluding NUL) or -1 on invalid ptr. */
static inline int strncpy_from_user(char *kdst, const char *usrc, size_t maxlen) {
    if (!maxlen) return 0;
    /* We can only validate that usrc starts in user space; the string
     * length is unknown, so validate at least one byte. */
    if (!access_ok(usrc, 1)) {
        kerror("UACCESS: strncpy_from_user: invalid user ptr 0x%llx\n",
               (unsigned long long)(uintptr_t)usrc);
        return -1;
    }
    size_t i;
    for (i = 0; i < maxlen - 1; i++) {
        /* Byte-at-a-time with bounds check (we know usrc is in user range) */
        uintptr_t addr = (uintptr_t)usrc + i;
        if (addr > USER_MEM_END) break;  /* hit the top of user space */
        char c = ((const char *)usrc)[i];
        kdst[i] = c;
        if (c == '\0') return (int)i;
    }
    kdst[i] = '\0';
    return (int)i;
}

/* ── KASSERT ──────────────────────────────────────────────────────────── */
/* Kernel assertion.  In DEBUG builds triggers a kpanic with file:line info.
 * In RELEASE builds compiles away completely.
 *
 * Usage:
 *   KASSERT(ptr != NULL, "sched_spawn: null entry point");
 *   KASSERT(size <= HEAP_SIZE, "kmalloc: size exceeds heap");
 */
#ifdef DRACO_DEBUG
#  define KASSERT(expr, msg) \
    do { \
        if (!(expr)) { \
            /* Build a compact panic string: "KASSERT: msg [file:line]" */ \
            static const char _kf[] = __FILE__; \
            char _kb[128]; \
            snprintf(_kb, sizeof(_kb), "KASSERT: %s [%s:%d]", \
                     (msg), _kf, __LINE__); \
            kpanic(_kb); \
        } \
    } while (0)
#else
#  define KASSERT(expr, msg)  ((void)0)
#endif /* DRACO_DEBUG */

/* ── Convenience: validate a single syscall pointer argument ─────────── */
/* Returns -1 (EFAULT) via frame->rax and logs a warning if invalid.
 * Usage inside syscall handlers:
 *   SYSCALL_VALIDATE_PTR(buf, len, frame);
 */
#define SYSCALL_VALIDATE_PTR(ptr, len, frame) \
    do { \
        if (!access_ok((ptr), (len))) { \
            kwarn("SYSCALL: bad user ptr 0x%llx len=%u — EFAULT\n", \
                  (unsigned long long)(uintptr_t)(ptr), (uint32_t)(len)); \
            (frame)->rax = (uint64_t)-14LL; /* -EFAULT */ \
            return; \
        } \
    } while (0)

#endif /* UACCESS_H */
