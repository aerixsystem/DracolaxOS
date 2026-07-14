/* kernel/irq.c — IRQ handler registration and dispatch (x86_64) */
#include "../../types.h"
#include "irq.h"
#include "pic.h"
#include "../../log.h"
#include "../../klog.h"
#include "../../klibc.h"
#include "../../drivers/serial/serial.h"
#include "../../sched/sched.h"

static irq_handler_t handlers[256];

static const char *exception_name[] = {
    "Divide by Zero",        "Debug",
    "NMI",                   "Breakpoint",
    "Overflow",              "Bound Range Exceeded",
    "Invalid Opcode",        "Device Not Available",
    "Double Fault",          "Coprocessor Segment Overrun",
    "Invalid TSS",           "Segment Not Present",
    "Stack Fault",           "General Protection Fault",
    "Page Fault",            "Reserved",
    "x87 FPU Error",         "Alignment Check",
    "Machine Check",         "SIMD FPU Error",
    "Virtualization",        "Control Protection",
    "Reserved","Reserved","Reserved","Reserved","Reserved","Reserved",
    "Reserved","Reserved","Security Exception","Reserved"
};

/* Page fault error code bits */
#define PF_PRESENT  (1u<<0)
#define PF_WRITE    (1u<<1)
#define PF_USER     (1u<<2)
#define PF_RESERVED (1u<<3)
#define PF_FETCH    (1u<<4)

static void default_exception(struct isr_frame *f) {
    const char *name = (f->int_no < 32) ? exception_name[f->int_no] : "Unknown";
    char buf[512];

    /* ── Fault origin: Ring 3 (user task) or Ring 0 (kernel)? ──────────
     * CS bottom 2 bits = CPL. If CPL == 3 the fault came from user space.
     * For user faults: log the fault, kill the offending task, continue.
     * For kernel faults: full panic (no safe way to continue). */
    int from_user = (f->cs & 3) == 3;

    if (f->int_no == 14) {
        /* Page fault */
        uint64_t cr2 = 0;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));

        const char *reason = (f->err_code & PF_PRESENT) ? "protection" : "not-present";
        const char *access = (f->err_code & PF_WRITE)   ? "write"      : "read";
        const char *ring   = from_user                   ? "user"       : "kernel";

        serial_print("\n[FAULT] Page Fault at 0x");
        char hx[17]; static const char *hex = "0123456789ABCDEF";
        uint64_t v = cr2;
        for (int i = 15; i >= 0; i--) { hx[i] = hex[v & 0xF]; v >>= 4; }
        hx[16] = '\0'; serial_print(hx);
        serial_print(" rip=0x");
        v = f->rip;
        for (int i = 15; i >= 0; i--) { hx[i] = hex[v & 0xF]; v >>= 4; }
        hx[16] = '\0'; serial_print(hx);
        serial_print("\n");

        snprintf(buf, sizeof(buf),
            "Page Fault\n"
            "  Addr   : 0x%llx\n"
            "  Reason : %s page, %s access from %s mode\n"
            "  RIP    : 0x%llx  ErrCode: 0x%llx\n"
            "  CS=0x%llx  RFLAGS=0x%llx  RSP=0x%llx\n",
            (unsigned long long)cr2, reason, access, ring,
            (unsigned long long)f->rip, (unsigned long long)f->err_code,
            (unsigned long long)f->cs,
            (unsigned long long)f->rflags, (unsigned long long)f->rsp);
    } else {
        snprintf(buf, sizeof(buf),
            "%s (vector %llu)\n"
            "  RIP=0x%llx  CS=0x%llx\n"
            "  RFLAGS=0x%llx  err=0x%llx\n",
            name, (unsigned long long)f->int_no,
            (unsigned long long)f->rip, (unsigned long long)f->cs,
            (unsigned long long)f->rflags, (unsigned long long)f->err_code);
    }

    if (from_user) {
        /* ── User-space fault: kill the task, don't crash the kernel ── */
        task_t *t = sched_current();
        kerror("FAULT [user] in task '%s' (id=%d): %s",
               t ? t->name : "?", sched_current_id(), buf);
        klog_crash_dump(t ? t->name : "unknown", buf, "user-mode fault");
        /* Kill the faulting task and schedule the next one */
        sched_kill(sched_current_id());
        /* sched_kill calls sched_exit() for the current task — does not return */
        __builtin_unreachable();
    } else {
        /* ── Kernel-mode fault: full panic, no recovery possible ─────── */
        kpanic(buf);
    }
}

static void default_irq(struct isr_frame *f) { (void)f; }

void irq_init(void) {
    for (int i = 0; i < 32; i++) handlers[i] = default_exception;
    for (int i = 32; i < 48; i++) handlers[i] = default_irq;
}

void irq_register(uint8_t n, irq_handler_t fn)   { handlers[n] = fn; }
void irq_unregister(uint8_t n)                     { handlers[n] = NULL; }

void isr_dispatch(struct isr_frame *frame) {
    if (frame->int_no >= 32 && frame->int_no < 48)
        pic_eoi((uint8_t)(frame->int_no - 32));
    irq_handler_t fn = handlers[frame->int_no];
    if (fn) fn(frame);
}
