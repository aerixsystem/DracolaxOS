/* kernel/limits.c — System resource limit enforcement
 *
 * FIX v1.1 — memory accounting:
 *   The old code compared pmm_free_pages()*4 against mem_total_kb directly,
 *   but vmm_init() pre-marks the entire 32 MB heap window as "used" in PMM
 *   even though the bump allocator has not handed any of it out yet.
 *   This caused mem_used_kb to appear at ~90% immediately after boot.
 *
 *   Fix: subtract vmm_heap_reserved() (unused portion of the heap window)
 *   from the raw PMM "used" value before computing percentage.
 *   Real pressure = PMM used – (heap window – actual heap allocations).
 */
#include "types.h"
#include "klibc.h"
#include "log.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "limits.h"
#include "drivers/vga/vga.h"

system_limits_t g_limits = {
    .volume_pct     = 100,
    .brightness_pct = 100,
};

void limits_init(void) {
    g_limits.mem_total_kb = (uint32_t)(pmm_total_bytes() / 1024);
    g_limits.mem_warn_threshold_kb =
        (g_limits.mem_total_kb * MEM_WARN_PCT) / 100;
    g_limits.mem_deny_threshold_kb =
        (g_limits.mem_total_kb * MEM_DENY_PCT) / 100;

    kinfo("LIMITS: mem total=%u KB  warn@%u KB  deny@%u KB\n",
          g_limits.mem_total_kb,
          g_limits.mem_warn_threshold_kb,
          g_limits.mem_deny_threshold_kb);
}

void limits_update(void) {
    /* Use VMM heap as the resource metric. The heap is the OS's allocation
     * pool. PMM raw pages include huge identity-mapped regions that skew
     * percentages. Heap-based tracking is accurate and actionable.
     *
     * heap_total = total heap window (fixed at boot)
     * heap_used  = bytes actually handed out by vmm_alloc
     * used_pct   = heap_used * 100 / heap_total
     */
    uint32_t heap_total_kb = 32768u;  /* matches VMM_HEAP_SIZE in vmm.c */
    uint32_t heap_used_kb  = (uint32_t)(vmm_heap_used() / 1024);
    if (heap_used_kb > heap_total_kb) heap_used_kb = heap_total_kb;

    /* Also track physical for informational display */
    uint32_t free_kb       = pmm_free_pages() * 4u;
    uint32_t raw_used_kb   = (g_limits.mem_total_kb > free_kb)
                             ? (g_limits.mem_total_kb - free_kb) : 0;
    uint32_t heap_reserved = (uint32_t)(vmm_heap_reserved() / 1024);
    g_limits.mem_used_kb   = (raw_used_kb > heap_reserved)
                             ? (raw_used_kb - heap_reserved) : heap_used_kb;

    /* Thresholds are now heap-relative */
    uint32_t warn_kb = (heap_total_kb * MEM_WARN_PCT) / 100;
    uint32_t deny_kb = (heap_total_kb * MEM_DENY_PCT) / 100;

    uint8_t was_warn = g_limits.mem_warn_active;
    uint8_t was_deny = g_limits.mem_deny_active;

    g_limits.mem_warn_active = (heap_used_kb >= warn_kb);
    g_limits.mem_deny_active = (heap_used_kb >= deny_kb);

    if (g_limits.mem_deny_active && !was_deny) {
        kerror("LIMITS: VMM heap CRITICAL — %u/%u KB used (%u%%), "
               "new large allocs BLOCKED\n",
               heap_used_kb, heap_total_kb,
               heap_used_kb * 100 / heap_total_kb);
    } else if (!g_limits.mem_deny_active && was_deny) {
        kinfo("LIMITS: VMM heap pressure cleared (%u KB used)\n", heap_used_kb);
        g_limits.mem_deny_active = 0;
    } else if (g_limits.mem_warn_active && !was_warn) {
        kwarn("LIMITS: VMM heap warning — %u/%u KB used (%u%%)\n",
              heap_used_kb, heap_total_kb,
              heap_used_kb * 100 / heap_total_kb);
    }
    (void)warn_kb; (void)deny_kb;
}

int limits_allow_alloc(size_t size) {
    if (!g_limits.mem_deny_active) return 1;
    if (size <= 64) return 1;  /* always allow tiny kernel allocations */
    return 0;
}

int limits_set_volume(uint32_t pct) {
    if (pct > 200) {
        vga_set_color(VGA_LIGHT_RED, VGA_BLACK);
        vga_print("Volume: hard limit is 200%. Denied.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        return -1;
    }
    if (pct > VOL_WARN_PCT) {
        vga_set_color(VGA_LIGHT_BROWN, VGA_BLACK);
        vga_print("Warning: volume above 100% may cause audio distortion.\n");
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    }
    g_limits.volume_pct = pct;
    return 0;
}

uint32_t limits_get_volume(void) { return g_limits.volume_pct; }

int limits_set_brightness(uint32_t pct) {
    if (pct > BRIGHTNESS_MAX) pct = BRIGHTNESS_MAX;
    g_limits.brightness_pct = pct;
    return 0;
}

void limits_print_status(void) {
    limits_update();
    char buf[256];
    uint32_t pct = g_limits.mem_total_kb
                 ? (g_limits.mem_used_kb * 100) / g_limits.mem_total_kb : 0;
    snprintf(buf, sizeof(buf),
        "  Memory  : %u/%u KB (%u%%)%s\n"
        "  Volume  : %u%%\n"
        "  Bright  : %u%%\n",
        g_limits.mem_used_kb, g_limits.mem_total_kb, pct,
        g_limits.mem_deny_active ? " [FULL]" :
        g_limits.mem_warn_active ? " [WARN]" : "",
        g_limits.volume_pct,
        g_limits.brightness_pct);
    vga_print(buf);
}
