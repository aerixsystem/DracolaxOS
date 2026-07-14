/* kernel/ring3.c — Ring-3 entry via iretq + SYSCALL/SYSRET MSR setup
 *
 * ring3_enter():
 *   Constructs the stack frame that iretq expects to find:
 *     [RSP+0]  user RIP
 *     [RSP+8]  user CS  (GDT_USER_CODE = 0x23)
 *     [RSP+16] user RFLAGS (IF enabled)
 *     [RSP+24] user RSP
 *     [RSP+32] user SS  (GDT_USER_DATA = 0x1B)
 *   Then executes iretq which atomically:
 *     • loads RIP, CS, RFLAGS, RSP, SS from the frame
 *     • switches the CPL to 3
 *
 * syscall_fast_init():
 *   Configures three MSRs for SYSCALL/SYSRET support:
 *     STAR   (0xC0000081) — kernel/user CS selectors
 *     LSTAR  (0xC0000082) — kernel entry point for SYSCALL
 *     SFMASK (0xC0000084) — RFLAGS bits to clear on SYSCALL (clears IF)
 *
 * References:
 *   Intel SDM Vol.3 §6.8.3  — iretq privilege return
 *   AMD64 APM Vol.2 §2.4    — SYSCALL/SYSRET instruction
 *   OSDev wiki — "Getting to Ring 3"
 */
#include "ring3.h"
#include "tss.h"
#include "../../log.h"
#include "../../klibc.h"

/* stddef.h is not available (nostdinc), so define offsetof manually.
 * This is the canonical compiler-builtin form — safe on all GCC versions. */
#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

/* ---- MSR helpers --------------------------------------------------------- */

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ---- MSR addresses ------------------------------------------------------- */

#define MSR_EFER   0xC0000080   /* Extended Feature Enable Register */
#define MSR_STAR   0xC0000081   /* SYSCALL target selectors          */
#define MSR_LSTAR  0xC0000082   /* SYSCALL 64-bit entry RIP          */
#define MSR_SFMASK 0xC0000084   /* SYSCALL RFLAGS mask               */
#define EFER_SCE   (1ULL << 0)  /* System Call Extensions enable bit */

/* ---- SYSCALL entry stub (assembly) --------------------------------------- */
/* The CPU jumps here on SYSCALL; must save user state and call the C handler.
 * RCX = user RIP (saved by SYSCALL), R11 = user RFLAGS.
 * We piggyback the existing INT 0x80 dispatcher by constructing an isr_frame
 * on the kernel stack and calling syscall_c_entry(). */

/* Forward declaration — implemented in syscall.c */
void syscall_c_entry_from_syscall(void);

__attribute__((naked))
static void syscall_entry_asm(void) {
    __asm__ volatile (
        /* At entry: RSP is still user RSP (SYSCALL does NOT switch stacks).
         * We must switch to kernel stack (TSS RSP0) ourselves. */
        "swapgs\n"                  /* swap GS base so we can reach per-CPU data */
        /* For now: load kernel stack from TSS directly.
         * In a real SMP kernel this would use per-CPU data via GS. */
        "mov %%rsp, %%r10\n"        /* save user RSP in R10 (caller-saved) */
        /* Build a minimal kernel call frame on the current (user) RSP —
         * we'll switch to the real kernel stack after saving user RSP. */
        /* Load RSP0 from TSS: address of g_tss.rsp0 is in a C symbol */
        "lea  tss_rsp0_ptr(%%rip), %%rsp\n"
        "mov  (%%rsp), %%rsp\n"     /* RSP = *tss_rsp0_ptr (kernel stack top) */
        /* Push iretq-style frame for return to user */
        "push $0x1B\n"              /* user SS  (GDT_USER_DATA | 3) */
        "push %%r10\n"              /* user RSP */
        "push %%r11\n"              /* user RFLAGS (saved by SYSCALL) */
        "push $0x23\n"              /* user CS  (GDT_USER_CODE | 3)  */
        "push %%rcx\n"              /* user RIP (saved by SYSCALL)    */
        /* Push general-purpose registers to match isr_frame layout */
        "push %%rax\n"
        "push %%rbx\n"
        "push %%rcx\n"
        "push %%rdx\n"
        "push %%rsi\n"
        "push %%rdi\n"
        "push %%rbp\n"
        "push %%r8\n"
        "push %%r9\n"
        "push %%r10\n"
        "push %%r11\n"
        "push %%r12\n"
        "push %%r13\n"
        "push %%r14\n"
        "push %%r15\n"
        /* push fake int_no + err_code so frame matches isr_frame */
        "push $0\n"                 /* err_code */
        "push $0x80\n"              /* int_no = 128 = INT 0x80 */
        /* Ensure 16-byte stack alignment before C call.
         * We have pushed 22 qwords (176 bytes). CALL pushes 8 more = 184.
         * 184 % 16 = 8 → misaligned. Sub 8 to align, add back after. */
        "sub  $8, %%rsp\n"
        "mov  %%rsp, %%rdi\n"       /* arg: isr_frame* (pointer, not aligned value) */
        "add  $8, %%rdi\n"          /* point past the alignment pad to real frame */
        "call syscall_c_entry_from_syscall\n"
        "add  $8, %%rsp\n"          /* remove alignment pad */
        /* Restore registers */
        "add  $16, %%rsp\n"         /* discard int_no + err_code */
        "pop  %%r15\n"
        "pop  %%r14\n"
        "pop  %%r13\n"
        "pop  %%r12\n"
        "pop  %%r11\n"
        "pop  %%r10\n"
        "pop  %%r9\n"
        "pop  %%r8\n"
        "pop  %%rbp\n"
        "pop  %%rdi\n"
        "pop  %%rsi\n"
        "pop  %%rdx\n"
        "pop  %%rcx\n"
        "pop  %%rbx\n"
        "pop  %%rax\n"
        /* Restore user RSP/RIP/RFLAGS from iretq frame we built */
        "add  $24, %%rsp\n"         /* skip RIP/CS/RFLAGS already in RCX/R11 */
        "pop  %%rsp\n"              /* user RSP — wait, sysretq does this */
        /* Use sysretq to return: RCX→RIP, R11→RFLAGS, SS/CS from STAR */
        "swapgs\n"
        "sysretq\n"
        ::: "memory"
    );
}

/* Symbol for the TSS RSP0 field — so the naked asm above can find it.
 * We export a pointer-to-RSP0 so the asm can indirect through it. */
extern tss64_t *tss_get(void);
uint64_t *tss_rsp0_ptr;   /* points to g_tss.rsp0, set in syscall_fast_init */

/* ---- ring3_enter --------------------------------------------------------- */

void ring3_enter(uint64_t user_rip, uint64_t user_rsp, uint64_t rflags) {
    kinfo("RING3: entering user mode RIP=0x%llx RSP=0x%llx\n",
          (unsigned long long)user_rip,
          (unsigned long long)user_rsp);

    /* Update TSS RSP0 so the next ring-3→ring-0 transition uses the
     * current task's kernel stack (top of current RSP before we drop). */
    uint64_t cur_rsp;
    __asm__ volatile ("mov %%rsp, %0" : "=r"(cur_rsp));
    tss_set_rsp0(cur_rsp);

    /* Build iretq frame and jump to user mode.
     *
     * iretq pops (low → high address):
     *   RIP, CS, RFLAGS, RSP, SS
     *
     * We push them in reverse order (high to low) onto the current
     * kernel stack, then execute iretq.
     */
    __asm__ volatile (
        "push %[ss]\n"          /* SS  = user data */
        "push %[usp]\n"         /* RSP = user stack */
        "push %[fl]\n"          /* RFLAGS (IF=1, reserved bit=1) */
        "push %[cs]\n"          /* CS  = user code */
        "push %[rip]\n"         /* RIP = user entry */
        "iretq\n"
        ::
        [ss]  "r"((uint64_t)GDT_USER_DATA),   /* 0x1B */
        [usp] "r"(user_rsp),
        [fl]  "r"(rflags),
        [cs]  "r"((uint64_t)GDT_USER_CODE),   /* 0x23 */
        [rip] "r"(user_rip)
        : "memory"
    );
    __builtin_unreachable();
}

/* ---- SYSCALL MSR setup --------------------------------------------------- */

void syscall_fast_init(void) {
    /* Enable SCE (System Call Extensions) in EFER */
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    /* STAR: [47:32] = kernel CS (SYSCALL sets CS=0x08, SS=0x10)
     *        [63:48] = used by SYSRET: CS = val+16 = 0x20|3 = 0x23
     *                                  SS = val+8  = 0x18|3 = 0x1B
     * So STAR[63:48] = 0x0010 gives the right SYSRET selectors. */
    uint64_t star = ((uint64_t)0x0010 << 48) |  /* SYSRET base */
                    ((uint64_t)0x0008 << 32);    /* SYSCALL CS  */
    wrmsr(MSR_STAR, star);

    /* LSTAR: RIP of kernel SYSCALL entry point */
    wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry_asm);

    /* SFMASK: bits to clear in RFLAGS on SYSCALL.
     * Clear IF (bit 9) so interrupts are disabled in the syscall handler
     * until we explicitly re-enable them. Also clear TF (bit 8). */
    /* Clear IF (9), TF (8), and DF (10) on SYSCALL entry.
     * DF must be cleared so kernel memcpy/memset (rep movs/stos) runs
     * forward — a user task setting DF before syscall would corrupt memory. */
    wrmsr(MSR_SFMASK, (1ULL << 9) | (1ULL << 8) | (1ULL << 10));  /* IF | TF | DF */

    /* Point tss_rsp0_ptr at the RSP0 field of the global TSS.
     *
     * BUG FIX: tss64_t is __attribute__((packed)), so &_t->rsp0 is an
     * unaligned pointer (rsp0 sits at byte offset 4 after uint32_t reserved0).
     * GCC emits -Waddress-of-packed-member for that direct address-of.
     *
     * Fix: compute the address arithmetically via char* (which may alias any
     * object) + offsetof, then cast to uint64_t*.  The resulting pointer has
     * the same value as &_t->rsp0 but is derived without taking the address
     * of a packed member, so no warning is emitted and no UB is introduced
     * (x86 tolerates unaligned 64-bit stores from C; the TSS loader uses
     * the raw bytes, not the pointer type's alignment assumption). */
    {
        tss64_t *_t = tss_get();
        tss_rsp0_ptr = (uint64_t *)(void *)((char *)_t + offsetof(tss64_t, rsp0));
    }

    kinfo("RING3: SYSCALL/SYSRET MSRs configured (LSTAR=0x%llx)\n",
          (unsigned long long)(uintptr_t)syscall_entry_asm);
}
