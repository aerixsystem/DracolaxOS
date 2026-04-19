/* kernel/main.c — Kernel entry point v1.0 */
#include "types.h"
#include "drivers/vga/vga.h"
#include "drivers/serial/serial.h"
#include "log.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/irq.h"
#include "arch/x86_64/pic.h"
#include "mm/paging.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include "drivers/ps2/keyboard.h"
#include "drivers/ps2/input_router.h"
#include "drivers/ps2/mouse.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/ring3.h"
#include "drivers/ata/ata_pio.h"
#include "drivers/ps2/vmmouse.h"
#include "arch/x86_64/syscall.h"
#include "drivers/vga/fb.h"
#include "bootmode.h"
#include "linux/linux_syscall.h"

extern void init_task(void);

void kmain(uint32_t magic, uint32_t mbi_addr) {
    vga_init();
    serial_init();

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    vga_print("DracolaxOS v1.0\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    serial_print("DracolaxOS v1.0 booting\n");

    kdebug("kmain: magic=0x%x mbi=0x%x\n", magic, mbi_addr);
    if (magic != 0x36d76289)
        kpanic("Bad Multiboot2 magic — check GRUB is passing multiboot2 protocol");

    /* Parse boot mode FIRST so everything downstream can branch on it */
    bootmode_init(mbi_addr);

    gdt_init();
    tss_init();            /* FIX 1.5/3.1: ring-3 GDT + TSS */
    kinfo("GDT loaded\n");

    irq_init();
    idt_init();
    kinfo("IDT loaded\n");

    pic_remap();
    pit_init(100);
    kinfo("PIC remapped, PIT 100Hz\n");

    paging_init();
    kinfo("Paging enabled\n");

    pmm_init(mbi_addr);
    vmm_init();
    sched_init();
    keyboard_init();
    input_router_init();   /* FIX 1.11: per-task queues */
    mouse_init();
    vmmouse_init();   /* enables absolute coords under QEMU/VMware — no capture needed */
    syscall_init();
    syscall_fast_init();   /* FIX 1.5/3.1: SYSCALL MSR path */
    ata_pio_init();        /* FIX 3.5: ATA PIO block driver */
    linux_compat_init();

    /* Framebuffer — always init to provide visual output in all modes.
     * Atlas/desktop are still gated by bootmode_wants_desktop().
     * In text/shell modes the FB console is used for log output. */
    kdebug("kmain: init framebuffer (all modes)...\n");
    fb_init((uint64_t)mbi_addr);
    /* Reset mouse to screen center now that fb dimensions are known.
     * mouse_init() ran before fb_init so it set mx=my=0. */
    if (fb.available)
        mouse_set_pos((int)fb.width / 2, (int)fb.height / 2);
    if (!bootmode_wants_desktop() && fb.available) {
        /* Text/shell mode: activate FB console for visual log output */
        fb_console_init();
    }

    /* Final keyboard guarantee: called after mouse_init() which may have
     * modified the PS/2 config byte, potentially disabling kbd IRQ. */
    keyboard_reinit();

    __asm__ volatile ("sti");
    kinfo("Interrupts enabled\n");

    if (sched_spawn(init_task, "init") < 0)
        kpanic("Failed to spawn init_task");
    /* init runs at HIGH priority and is exempt from watchdog killing
     * (it deliberately holds the CPU during hardware enumeration phases) */
    sched_set_priority(1, PRIO_HIGH);

    kinfo("kmain idle\n");
    for (;;) { __asm__ volatile ("hlt"); sched_yield(); }
}
