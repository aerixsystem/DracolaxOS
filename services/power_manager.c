/* services/power_manager.c — DracolaxOS power manager
 *
 * FIX v1.1:
 *   Added power_shutdown() and power_reboot() so that:
 *     - QEMU / VirtualBox Shutdown signal (ACPI S5) is handled via the
 *       QEMU isa-debug-exit device (port 0x604) and the ACPI PM1a control
 *       register (port 0xB004 for QEMU, 0x4004 for VirtualBox).
 *     - Restart signal is handled via keyboard-controller CPU reset (port
 *       0x64, command 0xFE) with a triple-fault fallback.
 *
 * Without these, VMs show "Shutdown/Restart sent but VM keeps running"
 * because the guest never acknowledges or acts on the ACPI event.
 */
#include "../kernel/types.h"
#include "../kernel/log.h"
#include "../kernel/sched/sched.h"
#include "power_manager.h"

static power_state_t g_power = {
    .brightness  = 100,
    .on_battery  = 0,
    .battery_pct = 100
};

/* ---- port I/O helpers (duplicated here to avoid driver dependency) ---- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/* ---- shutdown --------------------------------------------------------- */

/* power_shutdown:
 *   Attempts ACPI S5 soft-off via multiple methods in order:
 *   1. QEMU isa-debug-exit device   (port 0x604, value 0x2000 — triggers exit)
 *   2. QEMU ACPI PM1a control reg   (port 0xB004, SLP_TYP S5 | SLP_EN)
 *   3. VirtualBox ACPI              (port 0x4004, same encoding)
 *   4. Bochs/older QEMU             (port 0x8900, write "Shutdown\0")
 *   5. Triple fault (load zero-length IDT + INT)
 *
 * Any one of these will succeed on its respective hypervisor. */
void power_shutdown(void) {
    kinfo("POWER: shutdown requested\n");

    /* 1. QEMU isa-debug-exit (added via -device isa-debug-exit,iobase=0x604)
     *    Writing value V causes QEMU to exit with code (V*2+1).
     *    FIX C: isa-debug-exit default iosize=2 → use outw(), not outb().
     *    outb() to a 2-byte-wide device is silently ignored by QEMU's I/O
     *    dispatch.  outw(0x604, 0x0031) → exit code (0x31<<1)|1 = 99. */
    outw(0x604, 0x0031);   /* FIX C: was outb(0x604, 0x31) — wrong width for iosize=2 */

    /* 2. QEMU ACPI PM1a: SLP_EN(bit13) | SLP_TYP=5 S5(bits[12:10]=0b101=0x1400)
     *    FIX C: was 0x2000 which is SLP_TYP=0 (S0/wake) — wrong sleep state. */
    outw(0xB004, 0x3400);  /* FIX C: 0x3400 = SLP_EN(0x2000) | SLP_TYP_S5(0x1400) */

    /* 3. VirtualBox ACPI shutdown port */
    outw(0x4004, 0x3400);

    /* 4. Bochs/QEMU legacy shutdown string port */
    const char *s = "Shutdown";
    while (*s) { outb(0x8900, (uint8_t)*s); s++; }
    outb(0x8900, 0);

    /* 5. Last resort: triple fault via zero-length IDTR.
     * FIX: struct must be __packed__ — without it, the compiler inserts 6
     * bytes of padding between lim(2B) and base(8B), making sizeof=16 instead
     * of the required 10 bytes.  LIDT with wrong size loads garbage → #GP. */
    kinfo("POWER: ACPI ports did not respond — forcing triple fault\n");
    static volatile struct __attribute__((packed)) {
        uint16_t lim;
        uint64_t base;
    } zero_idt = { 0, 0 };
    __asm__ volatile (
        "lidt %0\n"
        "int $3\n"
        :: "m"(zero_idt)
    );

    for (;;) __asm__ volatile ("hlt");
}

/* ---- reboot ----------------------------------------------------------- */

/* power_reboot:
 *   1. Keyboard controller CPU reset pulse (0x64 cmd 0xFE) — works on
 *      real hardware, QEMU, and VirtualBox.
 *   2. Triple-fault fallback. */
void power_reboot(void) {
    kinfo("POWER: reboot requested\n");

    /* Flush keyboard controller input buffer before issuing reset command */
    uint32_t timeout = 0x10000;
    while ((inb(0x64) & 0x02) && timeout--) { /* wait for IBF clear */ }

    /* Pulse the CPU reset line via keyboard controller */
    outb(0x64, 0xFE);

    /* If KBC reset doesn't work within ~100ms, fall back to triple fault */
    /* spin a while to let the reset propagate */
    for (volatile uint32_t i = 0; i < 0x400000; i++) {}

    kinfo("POWER: KBC reset did not respond — forcing triple fault\n");
    static volatile struct __attribute__((packed)) {
        uint16_t lim;
        uint64_t base;
    } zero_idt2 = { 0, 0 };
    __asm__ volatile (
        "lidt %0\n"
        "int $3\n"
        :: "m"(zero_idt2)
    );

    for (;;) __asm__ volatile ("hlt");
}

/* ---- standard interface ----------------------------------------------- */

void power_manager_init(void) { kinfo("POWER: manager init\n"); }
power_state_t *power_get_state(void) { return &g_power; }

void power_set_brightness(uint32_t pct) {
    if (pct > 100) pct = 100;
    g_power.brightness = pct;
}

/* ---- ACPI battery detection via port I/O ---------------------------
 * Real ACPI battery info requires AML interpretation (V2).  For now we
 * probe the APM BIOS legacy interface (int 15h, ax=0x530A) via a
 * port-based poll on the APM data register.  On QEMU/VirtualBox this
 * always returns "AC only / no battery", which is correct.  On real
 * hardware with APM BIOS it may return a valid charge level.
 *
 * Port 0x92 bit 0 = A20 gate; not battery.  We check the CMOS RTC
 * power-status byte at address 0x35 (defined by some BIOS).  This is
 * a best-effort heuristic — on systems without that CMOS byte it will
 * read 0xFF (no battery), which is the safe default.
 * ----------------------------------------------------------------- */
static void poll_battery(void) {
    /* CMOS address 0x35 used by some BIOS to store battery flags */
    outb(0x70, 0x35);
    uint8_t cmos_bat = inb(0x71);

    if (cmos_bat == 0xFF || cmos_bat == 0x00) {
        /* No CMOS battery byte — assume AC power (desktop / VM) */
        g_power.on_battery  = 0;
        g_power.battery_pct = 100;
        return;
    }

    /* Bit 7 of CMOS 0x35 = battery low (heuristic) */
    g_power.on_battery  = (cmos_bat & 0x80) ? 1 : 0;
    /* Bits 6:0 encode rough percentage on some BIOSes (0-100) */
    uint8_t raw_pct = cmos_bat & 0x7F;
    g_power.battery_pct = (raw_pct <= 100) ? raw_pct : 100;
}

void power_manager_task(void) {
    power_manager_init();
    for (;;) {
        poll_battery();
        sched_sleep(10000);   /* re-check every 10 s */
    }
}
