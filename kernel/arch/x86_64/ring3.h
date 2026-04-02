/* kernel/ring3.h — Ring-3 (user-mode) entry point
 *
 * ring3_enter() uses iretq to transition the current task from ring-0
 * kernel mode to ring-3 user mode at the given entry point and user stack.
 *
 * After ring3_enter() returns (via SYSCALL or interrupt back to kernel),
 * the kernel continues on the ring-0 stack.  The TSS RSP0 field must be
 * kept up to date by the scheduler (tss_set_rsp0) so the correct kernel
 * stack is used on the next privilege transition.
 *
 * syscall_fast_init() configures the SYSCALL MSR path (STAR/LSTAR/SFMASK)
 * as a faster alternative to INT 0x80 for syscall-heavy Linux ABI programs.
 */
#ifndef RING3_H
#define RING3_H
#include "../../types.h"

/* Enter ring-3 at user_rip with user_rsp as the stack.
 * Does NOT return until the task calls exit() or is killed.
 * rflags is the initial RFLAGS for user code (typically 0x202 = IF+reserved).
 */
void ring3_enter(uint64_t user_rip, uint64_t user_rsp, uint64_t rflags);

/* Configure SYSCALL/SYSRET MSRs.  Call once during kernel init after
 * tss_init().  After this, user code can use the SYSCALL instruction
 * as an alternative to INT 0x80.  The same syscall_handler() dispatcher
 * is invoked in both cases. */
void syscall_fast_init(void);

#endif /* RING3_H */
