/* kernel/init.c — Init task v1.0
 *
 * Boot modes:
 *   SHELL      → minimal: ramfs + shell only
 *   TEXT       → full init, no desktop
 *   GRAPHICAL  → full init + progressive atlas + desktop
 *   AUTO       → GRAPHICAL if fb available, else TEXT
 *
 * Progressive boot: atlas_boot_start() is called before init steps;
 * atlas_boot_set_progress(N) is called between steps so the bar tracks
 * actual init time rather than a fixed animation.
 */
#include "types.h"
#include "fs/vfs.h"
#include "fs/ramfs.h"
#include "drivers/ps2/input_router.h"
#include "shell.h"
#include "log.h"
#include "klibc.h"
#include "sched/sched.h"
#include "mm/vmm.h"
#include "fs/procfs.h"
#include "drivers/vga/fb.h"
#include "drivers/vga/vga.h"
#include "limits.h"
#include "atlas.h"
#include "bootmode.h"
#include "arch/x86_64/pic.h"
#include "linux/linux_syscall.h"
#include "security/draco-shield/firewall.h"
#include "../gui/desktop/default-desktop/desktop.h"
#include "appman.h"
#include "../kernel/klog.h"
#include "arch/x86_64/rtc.h"
#include "../services/service_manager.h"
#include "../apps/installer/installer.h"
#include "security/dracoauth.h"
#include "security/dracolicence.h"
#include "security/dracolock.h"
#include "drivers/ps2/keyboard.h"
#include "drivers/ps2/mouse.h"

vfs_node_t *ramfs_root   = NULL;
vfs_node_t *storage_root = NULL;

/* ── helpers ──────────────────────────────────────────────────── */
static void rwrite(vfs_node_t *root, const char *name, const char *text) {
    ramfs_create(root, name);
    vfs_node_t *n = vfs_finddir(root, name);
    if (n) vfs_write(n, 0, (uint32_t)strlen(text), (const uint8_t *)text);
    else kerror("INIT: rwrite '%s' failed — ramfs_create returned no node "
                "(possible heap exhaustion)\n", name);
}

static void rmkdir(vfs_node_t *root, const char *name) {
    ramfs_create(root, name);
    vfs_node_t *nd = vfs_finddir(root, name);
    if (nd) nd->type = VFS_TYPE_DIR;
    else kerror("INIT: mkdir '%s' failed — vfs_finddir returned NULL "
                "(node may not have been created)\n", name);
}

/* ── PS/2 port helpers (for hardware detection) ──────────────── */
static inline uint8_t _inb(uint16_t p) {
    uint8_t v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v;
}

/* ── Hardware detection ───────────────────────────────────────── */
/* Probes PS/2 controller, PCI class codes, CPUID to log what hardware
 * is present. Results go to kinfo() so they appear in kernel log. */
static void detect_hardware(void) {
    kinfo("HW-DETECT: starting hardware enumeration\n");

    /* ── Keyboard ──────────────────────────────────────────────── */
    /* PS/2 controller status bit 0 = output buffer full after probe */
    uint8_t kbst = _inb(0x64);
    if (kbst != 0xFF) {
        kinfo("HW-DETECT: keyboard — PS/2 controller present (status=0x%02x)\n",
              (unsigned)kbst);
        /* Self-test: send 0xAA, expect 0x55 */
        for (int t = 10000; t-- && (_inb(0x64) & 0x02););
        __asm__ volatile("outb %0,%1"::"a"((uint8_t)0xAA),"Nd"((uint16_t)0x64));
        for (int t = 10000; t-- && !(_inb(0x64) & 0x01););
        uint8_t resp = _inb(0x60);
        if (resp == 0x55)
            kinfo("HW-DETECT: keyboard — PS/2 self-test PASS (type: PS/2 AT)\n");
        else
            kwarn("HW-DETECT: keyboard — PS/2 self-test unexpected response 0x%02x "
                  "(controller may be in non-standard state)\n", (unsigned)resp);
    } else {
        kwarn("HW-DETECT: keyboard — PS/2 controller not detected "
              "(status port 0x64 returned 0xFF; USB-only system?)\n");
    }

    /* ── Mouse ─────────────────────────────────────────────────── */
    /* Bit 5 of PS/2 config byte = aux device (mouse) clock disabled.
     * If clear, aux port is active and a mouse is likely connected. */
    for (int t = 10000; t-- && (_inb(0x64) & 0x02););
    __asm__ volatile("outb %0,%1"::"a"((uint8_t)0x20),"Nd"((uint16_t)0x64));
    for (int t = 10000; t-- && !(_inb(0x64) & 0x01););
    uint8_t cfg = _inb(0x60);
    if (cfg != 0xFF) {
        int has_mouse = !(cfg & 0x20);   /* bit5=0 → aux clock enabled */
        kinfo("HW-DETECT: mouse — %s (PS/2 aux port %s, cfg=0x%02x)\n",
              has_mouse ? "PS/2 mouse detected" : "no PS/2 mouse",
              has_mouse ? "active" : "disabled",
              (unsigned)cfg);
    }

    /* ── CPUID: feature flags ────────────────────────────────────── */
    uint32_t eax_id, ebx_id, ecx_id, edx_id;
    __asm__ volatile("cpuid":"=a"(eax_id),"=b"(ebx_id),"=c"(ecx_id),"=d"(edx_id):"a"(1u));
    kinfo("HW-DETECT: CPU features — SSE:%d SSE2:%d HTT:%d APIC:%d\n",
          (edx_id>>25)&1, (edx_id>>26)&1, (edx_id>>28)&1, (edx_id>>9)&1);

    /* ── Hypervisor ──────────────────────────────────────────────── */
    uint32_t hv_ebx, hv_ecx, hv_edx;
    __asm__ volatile("cpuid":"=a"(eax_id),"=b"(hv_ebx),"=c"(hv_ecx),"=d"(hv_edx)
                     :"a"(0x40000000u));
    /* VMware: EBX=0x61774D56 'VMwa' */
    if (hv_ebx == 0x61774D56u)
        kinfo("HW-DETECT: hypervisor — VMware detected (vmmouse backdoor active)\n");
    else if (hv_ebx == 0x786F4256u)
        kinfo("HW-DETECT: hypervisor — VirtualBox detected (PS/2 mode; no VMware backdoor)\n");
    else if (hv_ebx == 0x4D564B4Bu)
        kinfo("HW-DETECT: hypervisor — QEMU/KVM detected\n");
    else if (hv_ebx == 0x54474354u)
        kinfo("HW-DETECT: hypervisor — QEMU TCG (no KVM)\n");
    else if ((edx_id >> 31) & 1)
        kinfo("HW-DETECT: hypervisor — unknown VM (HV bit set in CPUID)\n");
    else
        kinfo("HW-DETECT: hypervisor — none (bare metal)\n");

    /* ── Audio (PCI class 0x04 = multimedia) ─────────────────────
     * Without a PCI bus driver we probe the legacy ISA AC97 base port.
     * Port 0x0100 = AC97 native audio mixer. If not all-FF, card present. */
    /* PCI config not available without driver; log as stub */
    kinfo("HW-DETECT: audio — PCI enumeration stub (AC97/HDA driver pending)\n");

    /* ── Camera ──────────────────────────────────────────────────
     * USB UVC cameras require a full USB host controller driver.
     * Log as not detected until USB stack is implemented. */
    kinfo("HW-DETECT: camera — USB UVC driver pending (USB stack not yet loaded)\n");

    /* ── Display / Framebuffer ──────────────────────────────────── */
    if (fb.available)
        kinfo("HW-DETECT: display — VESA framebuffer %ux%u bpp=%u pitch=%u\n",
              fb.width, fb.height, fb.bpp, fb.pitch);
    else
        kwarn("HW-DETECT: display — no VESA framebuffer "
              "(GRUB gfxpayload not set or unsupported mode)\n");

    kinfo("HW-DETECT: enumeration complete\n");
}

/* ── /ramfs mount ──────────────────────────────────────────────── */
static void mount_ramfs(void) {
    kdebug("INIT: mounting /ramfs\n");
    ramfs_root = ramfs_init();
    if (!ramfs_root)
        kpanic("INIT: ramfs_init() returned NULL — kernel heap exhausted "
               "during early boot (VMM heap may be too small)");
    vfs_mount("/ramfs", ramfs_root);
    kinfo("INIT: /ramfs mounted\n");

    /* /ramfs is intentionally minimal — volatile scratch only.
     * All persistent data lives under /storage. */
    rwrite(ramfs_root, "README.txt",
        "DracolaxOS /ramfs\n"
        "-------------------------------\n"
        "This is a volatile scratch filesystem.\n"
        "Files here are lost on reboot.\n"
        "\n"
        "For persistent storage, use:\n"
        "  cd /storage/main/system   -- OS config and logs\n"
        "  cd /storage/main/users    -- user home dirs\n"
        "  cd /storage/main/apps     -- installed apps\n"
        "\n"
        "Shell commands: help, ls, cd, cat, create, write, lxs\n"
        "ATA disk: ata_info  (if disk attached)\n"
        "Packages: draco install / remove / list\n"
        "Author: Lunax (Yunis)\n");
}

/* ── /proc mount ───────────────────────────────────────────────── */
static void mount_proc(void) {
    kdebug("INIT: mounting /proc\n");
    procfs_init();
    kinfo("INIT: /proc mounted\n");
}

/* ── /storage mount (full DracolaxOS storage tree) ────────────── */
/* The ramfs implementation is flat — all files/dirs belong to one
 * volume root. Nested directories require separate sub-volumes.
 * We create one ramfs volume per major storage node and mount them
 * at their respective paths. */
static void mount_storage(void) {
    kdebug("INIT: mounting /storage\n");
    storage_root = ramfs_new("storage");
    if (!storage_root) {
        kerror("INIT: ramfs_new(storage) failed — heap may be exhausted "
               "(check VMM heap size)\n");
        return;
    }
    vfs_mount("/storage", storage_root);

    /* ── Node 01: /storage/main — top-level directory index ── */
    vfs_node_t *main_vol = ramfs_new("storage-main");
    if (main_vol) {
        vfs_mount("/storage/main", main_vol);
        /* Directory entries (flat pointers to sub-mounts for ls) */
        rmkdir(main_vol, "system");
        rmkdir(main_vol, "users");
        rmkdir(main_vol, "apps");
        rmkdir(main_vol, "temp");
        rmkdir(main_vol, "cache");
        rmkdir(main_vol, "logs");
        kinfo("INIT: /storage/main mounted\n");
    } else {
        kerror("INIT: /storage/main volume creation failed\n");
    }

    /* ── Node 01a: /storage/main/system — OS config and metadata ── */
    vfs_node_t *sys_vol = ramfs_new("storage-system");
    if (sys_vol) {
        vfs_mount("/storage/main/system", sys_vol);
        /* Core OS metadata files — previously scattered on ramfs */
        rwrite(sys_vol, "version.txt",
               "DracolaxOS v1.0\n"
               "Kernel  : Draco-1.0 x86_64 (64-bit preemptive)\n"
               "ABI     : Draco native + Linux x86_64 (SYSCALL/SYSRET)\n"
               "Security: DracoAuth + DracoLock + DracoLicence + Draco-Shield\n"
               "Ring    : 3 (user mode active)\n"
               "Storage : ATA PIO + RAMFS (volatile) + VFS layer\n"
               "Author  : Lunax (Yunis)\n"
               "Build   : 2026-03-31\n");
        rwrite(sys_vol, "pkgdb.json",
               "{\"version\":2,\"packages\":[]}\n");
        rwrite(sys_vol, "ui.json",
               "{\"theme\":\"dracolax-dark\",\"accent\":\"#7828c8\","
               "\"wallpaper_overlay\":70,"
               "\"dock\":{\"position\":\"left\"},"
               "\"workspaces\":4,\"blur_radius\":8,\"rounded_corners\":12}\n");
        rwrite(sys_vol, "security.json",
               "{\"licence\":\"\",\"device_id\":\"\","
               "\"auth_users\":[],\"lock_timeout\":0}\n");
        rwrite(sys_vol, "motd.txt",
               "Welcome to DracolaxOS v1.0\n"
               "  help               list all commands\n"
               "  ls /ramfs          browse volatile filesystem\n"
               "  ls /storage/main   browse persistent storage\n"
               "  login root         admin login (password: dracolax)\n"
               "  lxs                run LXScript files\n"
               "  exec <elf>         run ELF64 binary\n"
               "  draco              package manager\n"
               "  Keyboard: Left/Right move cursor, Ctrl+A/E jump, Ctrl+K kill-to-end\n");
        rwrite(sys_vol, "fstab.txt",
               "/ramfs           ramfs  volatile     0\n"
               "/proc            procfs kernel       0\n"
               "/storage         ramfs  persistent   0\n"
               "/storage/main    ramfs  persistent   0\n"
               "/storage/main/system ramfs config   0\n"
               "/storage/main/users  ramfs users    0\n"
               "/storage/main/apps   ramfs apps     0\n"
               "/storage/usb     ramfs  removable    0\n"
               "/storage/network ramfs  network      0\n");
        kinfo("INIT: /storage/main/system mounted (version pkgdb ui security motd)\n");
    } else {
        kerror("INIT: /storage/main/system creation failed\n");
    }

    /* ── Node 01a-ii: /storage/main/system/shared — shared assets ── */
    vfs_node_t *shared_vol = ramfs_new("storage-shared");
    if (shared_vol) {
        vfs_mount("/storage/main/system/shared", shared_vol);
        rmkdir(shared_vol, "images");
        rmkdir(shared_vol, "fonts");
        rmkdir(shared_vol, "sounds");
        kinfo("INIT: /storage/main/system/shared mounted (images fonts sounds)\n");
    }

    /* Icon store inside shared */
    vfs_node_t *icons_vol = ramfs_new("storage-icons");
    if (icons_vol) {
        vfs_mount("/storage/main/system/shared/images", icons_vol);
        kinfo("INIT: /storage/main/system/shared/images mounted (icon store)\n");
    } else {
        kwarn("INIT: icon store mount failed — .dxi icons will not load\n");
    }

    /* Fonts store inside shared */
    vfs_node_t *fonts_vol = ramfs_new("storage-fonts");
    if (fonts_vol) {
        vfs_mount("/storage/main/system/shared/fonts", fonts_vol);
        kinfo("INIT: /storage/main/system/shared/fonts mounted\n");
    }

    /* Sounds store inside shared */
    vfs_node_t *sounds_vol = ramfs_new("storage-sounds");
    if (sounds_vol) {
        vfs_mount("/storage/main/system/shared/sounds", sounds_vol);
        kinfo("INIT: /storage/main/system/shared/sounds mounted\n");
    }

    /* ── Node 01b: /storage/main/users ── */
    vfs_node_t *users_vol = ramfs_new("storage-users");
    if (users_vol) {
        vfs_mount("/storage/main/users", users_vol);
        /* Create default user home dirs */
        rmkdir(users_vol, "root");
        rmkdir(users_vol, "guest");
        kinfo("INIT: /storage/main/users mounted\n");
    }

    /* ── Node 01c: /storage/main/apps ── */
    vfs_node_t *apps2_vol = ramfs_new("storage-apps-main");
    if (apps2_vol) {
        vfs_mount("/storage/main/apps", apps2_vol);
        kinfo("INIT: /storage/main/apps mounted\n");
    }

    /* ── Node 01d: /storage/main/logs ── */
    vfs_node_t *logs_vol = ramfs_new("storage-logs");
    if (logs_vol) {
        vfs_mount("/storage/main/logs", logs_vol);
        kinfo("INIT: /storage/main/logs mounted\n");
    }

    /* ── Node 02: /storage/usb ── */
    vfs_node_t *usb_vol = ramfs_new("storage-usb");
    if (usb_vol) {
        vfs_mount("/storage/usb", usb_vol);
        kinfo("INIT: /storage/usb mounted\n");
    }

    /* ── Node 03: /storage/network ── */
    vfs_node_t *net_vol = ramfs_new("storage-network");
    if (net_vol) {
        vfs_mount("/storage/network", net_vol);
        kinfo("INIT: /storage/network mounted\n");
    }

    /* ── Node 04: /storage/ramdisk ── (volatile) */
    vfs_node_t *rd_vol = ramfs_new("storage-ramdisk");
    if (rd_vol) {
        vfs_mount("/storage/ramdisk", rd_vol);
        rmkdir(rd_vol, "sessions");
        rmkdir(rd_vol, "temp");
        kinfo("INIT: /storage/ramdisk mounted (volatile)\n");
    }

    /* ── Node 05: /storage/apps ── (global apps) */
    vfs_node_t *apps_vol = ramfs_new("storage-apps");
    if (apps_vol) {
        vfs_mount("/storage/apps", apps_vol);
        kinfo("INIT: /storage/apps mounted\n");
    }

    kinfo("INIT: /storage mounted — nodes: main usb network ramdisk apps\n");
}

/* ── security ──────────────────────────────────────────────────── */
static void init_security(void) {
    dracolicence_init();
    dracoauth_init();
    dracolock_init();
    kinfo("INIT: security stack ready (DracoAuth + DracoLock + DracoLicence)\n");
}

/* ═══════════════════════════════════════════════════════════════
 * irq_watchdog_task — monitors IRQ1 (keyboard) and IRQ12 (mouse)
 *
 * Runs every 5 seconds.  Handles three failure cases:
 *   A) Only IRQ1 stalled  → kbd frozen, mouse alive
 *   B) Only IRQ12 stalled → mouse frozen, kbd alive
 *   C) Both stalled       → VM lost focus and regained it; PS/2
 *      controller may have accumulated stale bytes in its FIFO.
 *      Drain the buffer then do a full keyboard_reinit() to restore
 *      IRQ1, re-assert the config byte, and clear the ext_seq state.
 * ═══════════════════════════════════════════════════════════════ */
static void irq_watchdog_task(void) {
    uint32_t prev1 = 0, prev12 = 0;
    /* Track how many consecutive quiet periods we've seen so we
     * don't spam the log when the machine is simply idle. */
    uint32_t quiet_rounds = 0;
    /* BUG FIX 4: rate-limit IRQ12 re-assertion so a permanently absent
     * PS/2 mouse does not spam the log every 500 ms forever. */
    int irq12_fail_streak = 0;
#define IRQ12_MAX_RETRIES 5

    for (;;) {
        sched_sleep(500);   /* 500 ticks ≈ 5 s at 100 Hz */

        uint32_t cur1  = g_irq1_count;
        uint32_t cur12 = g_irq12_count;
        uint32_t d1    = cur1  - prev1;
        uint32_t d12   = cur12 - prev12;

        /* FIX [5]: In graphical (desktop) mode the fb_console is locked and
         * kinfo() output only goes to serial/VGA text.  Suppress the routine
         * heartbeat line entirely in graphical mode to avoid log noise; only
         * keep the recovery warnings below (they use kwarn, which callers need
         * to see even when the screen is owned by the compositor).
         * In text/shell modes the full periodic log is still useful. */
        if (!bootmode_wants_desktop()) {
            if (d1 || d12 || quiet_rounds < 3)
                kinfo("IRQ-WDG: IRQ1(kbd)=%u IRQ12(mouse)=%u "
                      "(delta1=%u delta12=%u)\n",
                      (unsigned)cur1, (unsigned)cur12,
                      (unsigned)d1, (unsigned)d12);
        }

        quiet_rounds = (d1 || d12) ? 0 : quiet_rounds + 1;

        /* ── Case A: keyboard frozen, mouse still moving ── */
        if (d1 == 0 && d12 != 0) {
            kwarn("IRQ-WDG: IRQ1 stalled — re-asserting kbd port\n");
            for (int t = 10000; t-- && (_inb(0x64) & 0x02););
            __asm__ volatile("outb %0,%1"
                             :: "a"((uint8_t)0xAE), "Nd"((uint16_t)0x64));
            pic_unmask(1);
        }

        /* ── Case B: mouse frozen, keyboard still firing ── */
        if (d12 == 0 && d1 != 0) {
            if (irq12_fail_streak < IRQ12_MAX_RETRIES) {
                kwarn("IRQ-WDG: IRQ12 stalled — re-asserting aux port\n");
                for (int t = 10000; t-- && (_inb(0x64) & 0x02););
                __asm__ volatile("outb %0,%1"
                                 :: "a"((uint8_t)0xA8), "Nd"((uint16_t)0x64));
                pic_unmask(12);
                irq12_fail_streak++;
                if (irq12_fail_streak == IRQ12_MAX_RETRIES)
                    kwarn("IRQ-WDG: IRQ12 unresponsive after %d retries — "
                          "PS/2 mouse may be absent\n", IRQ12_MAX_RETRIES);
            }
        } else if (d12 != 0) {
            irq12_fail_streak = 0;   /* mouse recovered, reset counter */
        }

        /* ── Case C: both stalled — VM likely lost + regained focus ──
         * The PS/2 controller may have buffered stale bytes during the
         * focus gap.  When focus returns the controller delivers them all
         * at once on IRQ1, which can desync both the scancode and mouse
         * state machines.  Drain the FIFO then do a full reinit. */
        if (d1 == 0 && d12 == 0 && quiet_rounds == 1) {
            kwarn("IRQ-WDG: both IRQs quiet — draining PS/2 buffer "
                  "and running keyboard_reinit()\n");
            /* Drain: read up to 32 stale bytes from the data port */
            for (int i = 0; i < 32; i++) {
                if (!(_inb(0x64) & 0x01)) break;
                _inb(0x60);
            }
            keyboard_reinit();
            /* Re-assert aux port enable and IRQ12 unmask */
            for (int t = 10000; t-- && (_inb(0x64) & 0x02););
            __asm__ volatile("outb %0,%1"
                             :: "a"((uint8_t)0xA8), "Nd"((uint16_t)0x64));
            pic_unmask(12);
            pic_unmask(1);
        }

        prev1  = cur1;
        prev12 = cur12;

        /* ── Task heartbeat watchdog — every 5s check ── */
        sched_watchdog_check();

        /* ── Periodic heap integrity check in DEBUG builds ── */
#ifdef DRACO_DEBUG
        if (mem_check() > 0)
            kwarn("IRQ-WDG: heap corruption detected — check serial log\n");
#endif
    }
}

/* ═══════════════════════════════════════════════════════════════
 * init_task — kernel init entry point
 * ═══════════════════════════════════════════════════════════════ */
void init_task(void) {
    /* FIX: spawned tasks start with IF=0.  Enable interrupts immediately
     * so PIT/keyboard/mouse IRQs fire and sched_sleep() works correctly. */
    __asm__ volatile ("sti");

    kinfo("INIT: starting (mode=%u)\n", g_boot_mode);

    /* ── SHELL mode: bare minimum ──────────────────────────────── */
    if (g_boot_mode == BOOT_MODE_SHELL) {
        /* Show a clean single-line boot message, then wipe the VGA
         * buffer so no kernel log lines appear behind the shell prompt. */
        vga_clear();
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_print("DracolaxOS — Loading Shell mode...\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

        mount_ramfs();
        mount_proc();
        mount_storage();   /* storage_root needed for cd /storage */
        klog_init();
        sched_spawn(klog_flush_task, "klog-flush");
        sched_spawn(irq_watchdog_task, "irq-watchdog");

        /* Clear VGA once more after all init kinfo() calls (which now
         * go only to serial/klog, not VGA) so the shell starts clean. */
        vga_clear();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        shell_run();
        for (;;) sched_sleep(500);
    }

    /* ── GRAPHICAL: start progressive boot screen ─────────────── */
    if (bootmode_wants_atlas()) {
        atlas_init();
        /* Lock the fb_console BEFORE starting the boot animation.
         * Without this, every kinfo/klog call triggers fb_console_print()
         * which calls fb_flip(), overwriting the animation shadow with
         * console text. Log output still goes to serial and VGA text. */
        fb_console_lock(1);
        atlas_boot_start();          /* 0% — bg + logo + bar drawn */
    } else {
        kinfo("INIT: [1] atlas skipped (text mode)\n");
    }

    /* Step 2 — /ramfs  → 10% */
    kdebug("INIT: [2] ramfs\n");
    mount_ramfs();
    sched_yield();   /* heartbeat: let watchdog see we're alive */
    if (bootmode_wants_atlas()) atlas_boot_set_progress(10);

    /* Step 3 — /proc   → 18% */
    kdebug("INIT: [3] proc\n");
    mount_proc();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(18);

    /* Step 4 — /storage → 28% */
    kdebug("INIT: [4] storage\n");
    mount_storage();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(28);

    /* Step 4b — klog + RTC → 35% */
    kdebug("INIT: [4b] klog + rtc\n");
    rtc_init();
    klog_init();
    sched_spawn(klog_flush_task, "klog-flush");
    sched_spawn(irq_watchdog_task, "irq-watchdog");
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(35);

    /* Step 5 — security → 45% */
    kdebug("INIT: [5] security\n");
    init_security();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(45);

    /* Step 5b — hardware detection → 52% */
    kdebug("INIT: [5b] hardware detect\n");
    detect_hardware();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(52);

    /* Step 6 — shield → 60% */
    kdebug("INIT: [6] draco-shield\n");
    shield_init();
    kinfo("INIT: Draco Shield armed\n");
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(60);

    /* Step 7 — limits → 65% */
    kdebug("INIT: [7] limits\n");
    limits_init();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(65);

    /* Step 8 — appman → 75% */
    kdebug("INIT: [8] appman\n");
    appman_init();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(75);

    /* Step 8b — services → 88% */
    kdebug("INIT: [8b] service manager\n");
    svc_manager_init();
    svc_manager_start_all();
    sched_yield();
    if (bootmode_wants_atlas()) atlas_boot_set_progress(88);

    /* Step 9 — finish boot animation, then launch desktop/shell */
    kdebug("INIT: [9] desktop/shell decision (fb=%d mode=%u)\n",
           fb.available, g_boot_mode);

    if (bootmode_wants_atlas()) {
        atlas_boot_set_progress(96);
        sched_sleep(80);
        atlas_boot_finish();   /* 100%, fade to black */
        /* Unlock console after animation completes */
        fb_console_lock(0);
    }

    if (bootmode_wants_desktop() && fb.available) {
        kinfo("INIT: VESA %ux%u bpp=%u — spawning desktop\n",
              fb.width, fb.height, fb.bpp);
        int did = sched_spawn(desktop_task, "desktop");
        if (did < 0) {
            kerror("INIT: desktop spawn failed — scheduler task table may be full "
                   "(MAX_TASKS exceeded); falling back to shell\n");
            sched_sleep(200);
            shell_run();
        } else {
            kinfo("INIT: desktop task %d spawned\n", did);
            /* Route keyboard input to the desktop task immediately */
            input_router_set_focus(did);
        }
    } else if (bootmode_wants_desktop() && !fb.available) {
        kwarn("INIT: graphical mode requested but VESA framebuffer unavailable — "
              "check GRUB gfxpayload=1024x768x32 in boot entry\n");
        vga_clear();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        shell_run();
    } else {
        /* TEXT mode — full init completed, shell owns VGA from here. */
        vga_clear();
        vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
        vga_print("DracolaxOS — Loading Text mode...\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        /* Brief pause so the user sees the mode label, then clear to shell */
        sched_sleep(200);
        vga_clear();
        shell_run();
    }

    kinfo("INIT: idle\n");
    /* BUG FIX 1: replaced bare hlt loop with sched_sleep(500).
     * hlt never calls sched_yield/sched_sleep so heartbeat was never
     * bumped, causing the watchdog to kill init in ~1.5 s. */
    for (;;) sched_sleep(500);
}
